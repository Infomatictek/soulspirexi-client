#include "packets.hpp"

#include <windows.h>

#include <vanacore/log_file.hpp>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <deque>
#include <iterator>
#include <mutex>
#include <vector>

#include "addon_trace.hpp"
#include "addons.hpp"
#include "damagetext.hpp"
#include "equipstats.hpp"
#include "game.hpp"
#include "imports.hpp"
#include "log.hpp"
#include "maxhp_ask.hpp"
#include "mcp.hpp"
#include "others.hpp"
#include "craftlog.hpp"
#include "packetflow.hpp"
#include "packetwatch.hpp"
#include "plugins.hpp"
#include "scan.hpp"
#include "soulspire/soulspire.hpp"
#include "trusts.hpp"
#include "zoneload.hpp"

namespace vanacore::packets {
namespace {

// int __cdecl (uint8_t* out, uint32_t capacity, void* key, void* table, const uint8_t* in, uint32_t
// size): decrypts and decompresses a buffer from the server into `out` (the 28-byte header, then
// the packets); returns the bytes written, or 0 or less on failure. The game then walks the
// packets up to that size. Starts mov eax,[esp+18h] / sub esp,5B8h (10 bytes).
using Decrypt = int(__cdecl*)(std::uint8_t* out, std::uint32_t capacity, void* key, void* table, const std::uint8_t* in,
                              std::uint32_t size);
constexpr char kDecryptPattern[] = "8B 44 24 18 81 EC B8 05 00 00 53 55 8B AC 24 C8 05 00 00 56 3B E8 57";
constexpr size_t kDecryptPrologue = 10;

// int __cdecl (uint8_t* out, uint32_t capacity, void* key, const uint8_t* in, uint32_t size, void*
// table): compresses and encrypts a buffer for the server from `in` (the header, then the
// packets). Starts mov eax,[esp+10h] / sub esp,68h (7 bytes).
using Encrypt = int(__cdecl*)(std::uint8_t* out, std::uint32_t capacity, void* key, const std::uint8_t* in, std::uint32_t size,
                              void* table);
constexpr char kEncryptPattern[] = "8B 44 24 10 83 EC 68 B9 07 00 00 00 53 55 8B 6C 24 74 56 57 8B F0 8B FD 83 C5 1C F3 A5";
constexpr size_t kEncryptPrologue = 7;

constexpr size_t kHeader = 0x1C;       // the buffer's header
constexpr size_t kMaxPacket = 0x7F * 4; // the largest size a packet's header can say
constexpr size_t kMaxOutgoing = 0x2000; // room for a buffer the game sends, plus added packets
// Ashita copies each received packet into a buffer of its own, 0x200 bytes of zeros first, for a plugin's `data` and
// `modified`. Plugins read past a short packet's end and take the zeros as "not there": Stylist reads the look at
// 0x00D+0x48 when the word there isn't 0, and the server's 0x00D is often 0x48 bytes long.
constexpr size_t kPluginPacket = 0x200;
static_assert(kMaxPacket < kPluginPacket, "a packet and at least one zero after it");

Decrypt g_decrypt = nullptr;
Encrypt g_encrypt = nullptr;
bool g_installed = false;
bool g_logged_first[2] = {};

struct Waiting {
    std::uint16_t id;
    std::vector<std::uint8_t> data;
    std::vector<std::string> from; // the Ashita plugins and addons that added it, and the packets it came from (see inject())
};

// Packets waiting to go, each way, past which something is adding them without end (a plugin and its own packets
// feeding each other): the server drops a client that floods it, so the rest are thrown away.
constexpr size_t kMaxWaiting = 256;
// Waiting packets handled in one buffer, past which they're thrown away: a packet that is added again each time it
// is handled (the queue never grows, so kMaxWaiting never sees it) would otherwise hold the game thread for good.
constexpr size_t kMaxHandled = 256;
// `from` of the waiting packet being handled on this thread: what it adds in the meantime comes of it (see inject())
thread_local const std::vector<std::string>* t_handling = nullptr;
std::mutex g_waiting_lock;
std::deque<Waiting> g_waiting[2]; // [0] outgoing, [1] incoming

// Each buffer handled each way, numbered (buffer_number())
std::atomic<std::uint32_t> g_buffer_number[2] = {};

// [client] packets.keepactions (configure()): an action from the server an addon blocked goes to the game all the same
constexpr std::uint16_t kAction = 0x028, kZoneIn = 0x00A;
std::wstring g_ini;
FILETIME g_ini_time{};
ULONGLONG g_next_check = 0;
std::atomic<bool> g_keep_actions{true};
unsigned g_kept_logged = 0;
constexpr unsigned kKeptLogged = 5; // the first few logged
// The counters (each buffer's first word) of the last buffers from the server: one that comes again is the server sending
// that buffer again. A zone starts the counter over, so they're forgotten at a zone-in (0x00A).
std::uint16_t g_counters[8] = {};
size_t g_counters_kept = 0;
size_t g_counter_next = 0;

std::uint16_t word(const std::uint8_t* at)
{
    std::uint16_t value;
    std::memcpy(&value, at, sizeof(value));
    return value;
}

// Whether the buffer from the server with this counter is new, not one of the last few again; kept as seen
bool new_buffer(std::uint16_t counter)
{
    for (size_t i = 0; i < g_counters_kept; ++i)
        if (g_counters[i] == counter)
            return false;
    g_counters[g_counter_next] = counter;
    g_counter_next = (g_counter_next + 1) % std::size(g_counters);
    if (g_counters_kept < std::size(g_counters))
        ++g_counters_kept;
    return true;
}

// A zone-in: the counters before it are another connection's; only the buffer's own is kept
void forget_counters(std::uint16_t counter)
{
    g_counters[0] = counter;
    g_counters_kept = 1;
    g_counter_next = 1;
}

FILETIME ini_time()
{
    WIN32_FILE_ATTRIBUTE_DATA data{};
    GetFileAttributesExW(g_ini.c_str(), GetFileExInfoStandard, &data);
    return data.ftLastWriteTime;
}

void read_settings(bool first)
{
    const bool keep = GetPrivateProfileIntW(L"client", L"packets.keepactions", 1, g_ini.c_str()) != 0;
    if (!first && keep == g_keep_actions.load())
        return;
    g_keep_actions = keep;
    log::write(L"Packets: an action from the server an addon blocked %ls ([client] packets.keepactions)",
               keep ? L"goes to the game all the same, without its chat lines" : L"stays blocked");
}

// An action packet (0x028, `length` bytes) without its chat lines: each result's message, and its added and spike effects'
// (10 bits each, 0 for none), as SimpleLog clears the ones it writes itself; the animations stay. Bit-packed from its 5th
// byte, lowest bit first (damagetext.cpp's on_action has the layout).
void silence_action(std::uint8_t* packet, size_t length)
{
    const size_t end = length * 8;
    size_t at = 72;
    bool short_read = false;
    const auto take = [&](unsigned count) -> std::uint32_t {
        if (short_read || at + count > end) {
            short_read = true;
            return 0;
        }
        std::uint32_t value = 0;
        for (unsigned i = 0; i < count; ++i)
            value |= static_cast<std::uint32_t>((packet[(at + i) / 8] >> ((at + i) % 8)) & 1) << i;
        at += count;
        return value;
    };
    const auto clear = [&](unsigned count) {
        if (short_read || at + count > end) {
            short_read = true;
            return;
        }
        for (unsigned i = 0; i < count; ++i)
            packet[(at + i) / 8] &= static_cast<std::uint8_t>(~(1u << ((at + i) % 8)));
        at += count;
    };
    const std::uint32_t targets = take(6);
    take(4);
    take(4);  // the category
    take(32); // its argument
    take(32); // the recast
    for (std::uint32_t t = 0; t < targets && !short_read; ++t) {
        take(32);
        const std::uint32_t results = take(4);
        for (std::uint32_t r = 0; r < results && !short_read; ++r) {
            take(5 + 12 + 7 + 3 + 17); // reaction, animation, effect, knockback, param
            clear(10);
            take(31);
            if (take(1) != 0) { // an added effect
                take(6 + 4 + 17);
                clear(10);
            }
            if (take(1) != 0) { // a spike effect
                take(6 + 4 + 14);
                clear(10);
            }
        }
    }
}

// An action from the server that an addon blocked (`packet` as it came, `modified` as the plugins and addons left it), for
// the game all the same: as they left it (as it came, if they made it something else), its chat lines out
void keep_action(const std::uint8_t* packet, size_t length, std::uint8_t* modified)
{
    if (word(modified) != word(packet)) // its id or size changed
        std::memcpy(modified, packet, length);
    silence_action(modified, length);
    if (g_kept_logged < kKeptLogged) {
        ++g_kept_logged;
        log::write(L"Packets: an addon blocked an action from the server (0x028); the game was given it without its chat lines, "
                   L"so its animation plays ([client] packets.keepactions)%ls",
                   g_kept_logged == kKeptLogged ? L"; not logged again this session" : L"");
    }
}
// The last packet of each id, each way (Windower's windower.packets.last_incoming and last_outgoing)
struct Last {
    std::uint16_t length = 0;
    double at = 0; // when it came, in seconds: each later than the one before
    std::uint8_t data[kMaxPacket];
};
std::mutex g_last_lock;
Last g_last[2][0x200]; // [0] sent, [1] received
double g_last_at = 0;
// When the server's last zone-out packet (0x00B) came (GetTickCount64), 0 once the character is back (see zoning())
std::atomic<ULONGLONG> g_zone_out_at{0};
// When the last buffer from the server came (GetTickCount64), 0 before the first (see server_silence())
std::atomic<ULONGLONG> g_incoming_at{0};

// The server's last zone-in packet (0x00A): the zone it names, the player's zone in the game's party list as it came, and
// when (GetTickCount64), `at` 0 once that list has the new zone (see player_zone())
struct ZoneIn {
    std::uint16_t zone = 0;
    std::uint16_t listed = 0;
    ULONGLONG at = 0;
};
std::mutex g_zone_in_lock;
ZoneIn g_zone_in;
// How long the party list may take to catch up before its zone is believed again whatever it says (ms)
constexpr ULONGLONG kZoneInPatience = 60000;
// The zone-in packet's byte that says whether the player is in their Mog House: 1 there, 2 elsewhere (logging in and zoning
// alike; the zone id is the city's either way), as a LandSandBoat server sends it (from XI on Anything,
// rubymatrix/xi-on-anything, host64.c moghouse_tap). The next zone-in, leaving it, clears it.
constexpr size_t kMogHouse = 0x80;
std::atomic<bool> g_in_mog_house{false};
// A home point's teleport: the destination picked (the home point event's reply 0x05B with option 2), from then until the
// zone-out (0x00B), when its effect - the bright pillars round the character - plays; 0 none (warping())
std::atomic<ULONGLONG> g_warp_at{0};
constexpr ULONGLONG kWarpLongest = 15000; // ms: no zone-out by then (the server said no), it's over

void zone_in(const std::uint8_t* packet, size_t length)
{
    if (length > kMogHouse)
        g_in_mog_house = packet[kMogHouse] == 1; // 1 in it, 2 elsewhere: not merely nonzero
    if (length < 0x34)
        return;
    const std::uint16_t zone = word(packet + 0x30); // ZoneNo (32 bits; zone ids fit in 16)
    game::PartyMember members[18];
    game::Alliance alliance;
    const std::uint16_t listed = game::party(members, alliance) ? members[0].zone : zone;
    const std::lock_guard lock(g_zone_in_lock);
    g_zone_in = {zone, listed, listed != zone ? GetTickCount64() : 0};
}

void remember(bool incoming, const std::uint8_t* packet, size_t length)
{
    if (length < 4 || length > kMaxPacket)
        return;
    if (incoming && (word(packet) & 0x1FF) == 0x00B)
        g_zone_out_at = GetTickCount64(), g_warp_at = 0;
    // (the option's low 16 bits: its high ones are the destination; a home point's menu is event 8700 to 8703 - 8701 in
    // Lower Jeuno - so another NPC's option 2 isn't taken for one)
    if (!incoming && (word(packet) & 0x1FF) == 0x05B && length >= 0x14 && word(packet + 0x08) == 2 &&
        (word(packet + 0x12) & 0xFFFC) == 0x21FC)
        g_warp_at = GetTickCount64();
    if (incoming && (word(packet) & 0x1FF) == 0x00A)
        zone_in(packet, length);
    if (incoming && (word(packet) & 0x1FF) == 0x0C9)
        equipstats::inspected(packet, length); // (a /check's gear, for the stats under the Check window)
    mcp::on_packet(incoming, packet, length); // the MCP server's packets tool
    game::on_packet(incoming, packet, length); // linkshells and merits, which the game keeps no readable copy of
    maxhp::on_packet(incoming, packet, length); // the server's answers to an addon's or plugin's max HP request
    zoneload::on_packet(incoming, packet, length); // a zone change's timeline (logs\zoneload-*.log)
    const std::lock_guard lock(g_last_lock);
    Last& last = g_last[incoming][word(packet) & 0x1FF];
    std::memcpy(last.data, packet, length);
    last.length = static_cast<std::uint16_t>(length);
    g_last_at = std::max(static_cast<double>(GetTickCount64()) / 1000.0, g_last_at + 0.000001);
    last.at = g_last_at;
}

// Every packet in a buffer (its header first) as it goes by untouched
void remember_all(bool incoming, const std::uint8_t* buffer, size_t size)
{
    for (size_t at = kHeader; at + 4 <= size;) {
        const size_t length = static_cast<size_t>(word(buffer + at) >> 9) * 4;
        if (length < 4 || at + length > size)
            return;
        remember(incoming, buffer + at, length);
        at += length;
    }
}

void set_word(std::uint8_t* at, std::uint16_t value)
{
    std::memcpy(at, &value, sizeof(value));
}

// Passes each packet in `buffer` (`size` bytes, header first) through the plugins, then adds the
// packets waiting to go in, writing what's left into `out` (at most `capacity` bytes). Returns
// the new size.
size_t process(bool incoming, const std::uint8_t* buffer, size_t size, std::uint8_t* out, size_t capacity)
{
    ++g_buffer_number[incoming];
    if (incoming)
        g_incoming_at = GetTickCount64(); // before any plugin can block a packet: the server talked
    // A buffer the server sent again keeps whatever the addons block (packets.keepactions)
    const bool fresh = incoming && new_buffer(word(buffer));
    // Nothing to see or add: the buffer as it is.
    bool waiting = false;
    {
        const std::lock_guard lock(g_waiting_lock);
        waiting = !g_waiting[incoming].empty();
    }
    if (!waiting && !plugins::wants_packets() && !addons::wants_packets() && !(incoming && equipstats::wants_packets()) &&
        !(incoming && soulspire::wants_packets())) {
        remember_all(incoming, buffer, size < capacity ? size : capacity);
        if (out != buffer)
            std::memcpy(out, buffer, size < capacity ? size : capacity);
        return size < capacity ? size : capacity;
    }
    // The buffer as it came, for the plugins (and so `out` can be `buffer`). Zeros follow it, as they do the game's
    // own: LegacyAC walks on from a packet to the next until it meets a size of 0, with no check against the chunk's end.
    std::vector<std::uint8_t> original(size + kMaxPacket, 0);
    std::memcpy(original.data(), buffer, size);
    const std::uint8_t* chunk = original.data() + kHeader;
    const std::uint32_t chunk_size = static_cast<std::uint32_t>(size - kHeader);
    std::memcpy(out, original.data(), kHeader);
    size_t written = kHeader;
    std::uint16_t sequence = word(original.data()); // for added packets when the buffer has none
    std::uint8_t modified[kPluginPacket];
    std::uint8_t received[kPluginPacket]; // a received packet's own copy, the plugins' `data`

    const auto keep = [&](const std::uint8_t* packet, size_t length) {
        if (written + length > capacity)
            return false;
        std::memcpy(out + written, packet, length);
        written += length;
        return true;
    };

    for (size_t at = kHeader; at + 4 <= size;) {
        const std::uint8_t* packet = original.data() + at;
        const std::uint16_t head = word(packet);
        const size_t length = static_cast<size_t>(head >> 9) * 4;
        if (length < 4 || at + length > size) {
            // Not a packet: pass the rest on as it is, for the game to judge.
            keep(packet, size - at);
            break;
        }
        sequence = word(packet + 2);
        // the /checkparam lines the Equipment window's stats asked for (equipstats.cpp): kept from the game and the addons
        if (incoming && equipstats::swallow(packet, length)) {
            at += length;
            continue;
        }
        std::memcpy(modified, packet, length);
        std::memset(modified + length, 0, sizeof(modified) - length);
        // Received: a copy with zeros after it, as Ashita's (not the next packet). Sent: in the buffer, for LegacyAC.
        const std::uint8_t* data = packet;
        if (incoming) {
            std::memcpy(received, packet, length);
            std::memset(received + length, 0, sizeof(received) - length);
            data = received;
        }
        if (incoming)
            soulspire::on_packet_in(static_cast<std::uint16_t>(head & 0x1FF), data, length);
        const bool by_plugin = plugins::on_packet(incoming, static_cast<std::uint16_t>(head & 0x1FF),
                                                  static_cast<std::uint32_t>(length), data, modified, chunk_size, chunk, false, false);
        if (by_plugin && incoming)
            packetwatch::blocked("a plugin", head & 0x1FF);
        bool blocked = addons::on_packet(incoming, static_cast<std::uint16_t>(head & 0x1FF), static_cast<std::uint32_t>(length),
                                         data, modified, chunk_size, chunk, false, by_plugin);
        if (!blocked && incoming && soulspire::suppress_incoming_for_game(static_cast<std::uint16_t>(head & 0x1FF), data, length))
            blocked = true;
        // An action from the server an addon blocked: the game has it all the same, without its chat lines
        if (blocked && !by_plugin && fresh && (head & 0x1FF) == kAction && g_keep_actions.load()) {
            keep_action(packet, length, modified);
            blocked = false;
        }
        if (incoming && (head & 0x1FF) == kZoneIn)
            forget_counters(word(original.data()));
        if (!blocked) {
            // A plugin may have changed the size in its header; within reason, it's used.
            size_t new_length = static_cast<size_t>(word(modified) >> 9) * 4;
            if (new_length < 4 || new_length > kMaxPacket)
                new_length = length;
            if (keep(modified, new_length))
                remember(incoming, modified, new_length);
            else
                log::write(L"Packets: no room for a %ls packet 0x%03X; dropped", incoming ? L"received" : L"sent",
                           head & 0x1FF);
        }
        at += length;
    }

    // Packets added by plugins (including while the ones above were handled).
    for (size_t handled = 0;; ++handled) {
        Waiting next;
        {
            const std::lock_guard lock(g_waiting_lock);
            if (g_waiting[incoming].empty())
                break;
            if (handled >= kMaxHandled) {
                static bool logged[2] = {};
                if (!logged[incoming]) {
                    logged[incoming] = true;
                    log::write(L"Packets: %zu %ls packets added and handled again in one buffer (last 0x%03X, from %hs); the rest are dropped",
                               handled, incoming ? L"received" : L"sent", g_waiting[incoming].front().id,
                               g_waiting[incoming].front().from.empty() ? "something" : g_waiting[incoming].front().from.back().c_str());
                }
                g_waiting[incoming].clear();
                break;
            }
            next = std::move(g_waiting[incoming].front());
            g_waiting[incoming].pop_front();
        }
        const size_t length = (next.data.size() + 3) / 4 * 4;
        if (length < 4 || length > kMaxPacket)
            continue;
        std::vector<std::uint8_t> packet(length + kMaxPacket, 0); // zeros after it, as for `original`
        std::memcpy(packet.data(), next.data.data(), next.data.size());
        set_word(packet.data(), static_cast<std::uint16_t>((next.id & 0x1FF) | ((length / 4) << 9)));
        set_word(packet.data() + 2, sequence);
        std::memcpy(modified, packet.data(), length);
        std::memset(modified + length, 0, sizeof(modified) - length);
        t_handling = &next.from;
        bool blocked = plugins::on_packet(incoming, next.id, static_cast<std::uint32_t>(length), packet.data(), modified,
                                          chunk_size, chunk, true, false, next.from);
        blocked = addons::on_packet(incoming, next.id, static_cast<std::uint32_t>(length), packet.data(), modified, chunk_size,
                                    chunk, true, blocked, next.from);
        t_handling = nullptr;
        if (!next.from.empty() && addon_trace::any())
            addon_trace::packet_result(next.from.back(), incoming, next.id,
                                       blocked ? "blocked (by another addon or plugin) before it went"
                                       : written + length > capacity ? "no room in this buffer: held for the next"
                                                                     : "put in the buffer that goes out now");
        if (!blocked && !keep(modified, length)) {
            // No room in this buffer: it goes in the next. (Already seen by the plugins: they see it again then, but
            // not the one that added it.)
            const std::lock_guard lock(g_waiting_lock);
            g_waiting[incoming].push_front({next.id, std::vector<std::uint8_t>(modified, modified + length), next.from});
            break;
        }
        if (!blocked)
            remember(incoming, modified, length);
    }
    return written;
}

// A buffer from the server the game couldn't decrypt or unpack (the game's result 0 or less): with none decrypted yet, the
// game's zone key isn't the server's, and the login ends in FFXI-3001. The first few, and a count, to the log.
void note_undecrypted(std::uint32_t size, int result)
{
    static std::atomic<unsigned> count{0};
    const unsigned n = ++count;
    if (n <= 3 || n == 10 || n == 100 || n == 1000)
        log::write(L"Packets: a buffer from the server didn't decrypt (%u bytes, the game's result %d; %u so far)%ls", size,
                   result, n, g_logged_first[1] ? L"" : L": none has yet, so the game's zone key isn't the one the server uses");
}

int __cdecl decrypt(std::uint8_t* out, std::uint32_t capacity, void* key, void* table, const std::uint8_t* in, std::uint32_t size)
{
    packetflow::on_received(); // the server answered: the game may send again sooner
    const int result = g_decrypt(out, capacity, key, table, in, size);
    if (result <= 0 && in != nullptr)
        note_undecrypted(size, result);
    if (result <= static_cast<int>(kHeader) || out == nullptr || static_cast<std::uint32_t>(result) > capacity)
        return result;
    if (!g_logged_first[1]) {
        g_logged_first[1] = true;
        log::write(L"Packets: first buffer received (%d bytes)", result);
    }
    packetwatch::on_received(out, static_cast<size_t>(result)); // the buffer as the server sent it, before anything of ours
    craftlog::on_received(out, static_cast<size_t>(result));    // [Craft:] crystals, clusters and synthesis in vanacore.log
    trusts::on_received(out, static_cast<size_t>(result));
    // (other players left out before the plugins see them: they never hear of them, as the game doesn't)
    const size_t kept = others::on_received(out, static_cast<size_t>(result), capacity - static_cast<std::uint32_t>(result));
    const size_t length = process(true, out, kept, out, capacity);
    plugins::on_received(out, length);
    damagetext::on_received(out, length); // the buffer as the game gets it (packets added to it too)
    packetwatch::on_delivered(out, length);
    return static_cast<int>(length);
}

int __cdecl encrypt(std::uint8_t* out, std::uint32_t capacity, void* key, const std::uint8_t* in, std::uint32_t size, void* table)
{
    if (in == nullptr || size < kHeader || size > kMaxOutgoing)
        return g_encrypt(out, capacity, key, in, size, table);
    if (!g_logged_first[0]) {
        g_logged_first[0] = true;
        log::write(L"Packets: first buffer sent (%u bytes)", size);
    }
    static std::uint8_t buffer[kMaxOutgoing];
    const size_t length = process(false, in, size, buffer, sizeof(buffer));
    packetflow::on_sent(buffer, length);
    craftlog::on_sent(buffer, length);
    return g_encrypt(out, capacity, key, buffer, static_cast<std::uint32_t>(length), table);
}

void install()
{
    const HMODULE game = GetModuleHandleW(L"FFXiMain.dll");
    if (game == nullptr)
        return;
    const std::uintptr_t decrypt_at = scan::find(game, kDecryptPattern);
    const std::uintptr_t encrypt_at = scan::find(game, kEncryptPattern);
    if (decrypt_at == 0 || encrypt_at == 0) {
        log::write(L"Packets: the game's packet %ls wasn't found in FFXiMain.dll: plugins see no packets",
                   decrypt_at == 0 ? L"decryption" : L"encryption");
        return;
    }
    if (!imports::detour(reinterpret_cast<Decrypt>(decrypt_at), kDecryptPrologue, &decrypt, g_decrypt) ||
        !imports::detour(reinterpret_cast<Encrypt>(encrypt_at), kEncryptPrologue, &encrypt, g_encrypt)) {
        log::write(L"Packets: couldn't hook the game's packet handling: plugins see no packets");
        return;
    }
    log::write(L"Packets: hooked (received at %ls, sent at %ls)", describe_address(reinterpret_cast<void*>(decrypt_at)).c_str(),
               describe_address(reinterpret_cast<void*>(encrypt_at)).c_str());
}

} // namespace

void configure(const wchar_t* ini)
{
    g_ini = ini != nullptr ? ini : L"";
    if (g_ini.empty())
        return;
    g_ini_time = ini_time();
    read_settings(true);
}

void on_frame()
{
    if (!g_ini.empty()) {
        const ULONGLONG now = GetTickCount64();
        if (now >= g_next_check) {
            g_next_check = now + 500;
            const FILETIME time = ini_time();
            if (CompareFileTime(&time, &g_ini_time) != 0) {
                g_ini_time = time;
                read_settings(false);
            }
        }
    }
    if (g_installed)
        return;
    g_installed = true;
    install();
}

std::uint32_t buffer_number(bool incoming)
{
    return g_buffer_number[incoming].load(std::memory_order_relaxed);
}

void inject(bool incoming, std::uint16_t id, std::uint32_t size, const std::uint8_t* data, std::string_view from)
{
    if (data == nullptr || size < 4 || size > kMaxPacket)
        return;
    if (!from.empty() && addon_trace::any())
        addon_trace::packet_added(from, incoming, id, data, size);
    // Those who add a packet while another they added is being handled (LuAshitacast's, which LegacyAC changes the gear
    // for and adds again) are left out of what comes of it as well: Ashita's guards are all set by then, nested
    // inside one another, and a handler that was added to the chain at the end would add it again for ever.
    std::vector<std::string> chain;
    if (t_handling != nullptr)
        chain = *t_handling;
    if (!from.empty() && std::find(chain.begin(), chain.end(), from) == chain.end())
        chain.emplace_back(from);
    const std::lock_guard lock(g_waiting_lock);
    std::deque<Waiting>& waiting = g_waiting[incoming];
    if (waiting.size() >= kMaxWaiting) {
        static bool logged[2] = {};
        if (!logged[incoming]) {
            logged[incoming] = true;
            log::write(L"Packets: %hs keeps adding %ls packets (%zu waiting, last 0x%03X); the rest are dropped",
                       from.empty() ? "something" : std::string(from).c_str(), incoming ? L"received" : L"sent", waiting.size(), id);
        }
        return;
    }
    waiting.push_back({id, std::vector<std::uint8_t>(data, data + size), std::move(chain)});
}

bool ready()
{
    return g_decrypt != nullptr && g_encrypt != nullptr;
}

bool zoning(bool in_world)
{
    const ULONGLONG at = g_zone_out_at.load();
    if (at == 0)
        return false;
    const ULONGLONG age = GetTickCount64() - at;
    if (in_world) {
        // Still the old zone's player (Ashita's status stays 2 for about 0.4 s after 0x00B, until the entity goes), or
        // the new zone's: back, once enough time has passed that the entity must have gone in between.
        if (age > 3000)
            g_zone_out_at = 0;
        return false;
    }
    // Not back after two minutes: it's the title screen (the character logged out), not a zone change.
    if (age > 120000) {
        g_zone_out_at = 0;
        return false;
    }
    return true;
}

std::uint16_t player_zone(std::uint16_t listed)
{
    const std::lock_guard lock(g_zone_in_lock);
    if (g_zone_in.at == 0)
        return listed;
    // The list has moved on (to the new zone, normally), or never did: its zone from now on.
    if (listed != g_zone_in.listed || GetTickCount64() - g_zone_in.at > kZoneInPatience) {
        g_zone_in.at = 0;
        return listed;
    }
    return g_zone_in.zone;
}

bool in_mog_house()
{
    return g_in_mog_house;
}

ULONGLONG server_silence()
{
    const ULONGLONG at = g_incoming_at.load();
    return at == 0 ? 0 : GetTickCount64() - at;
}

bool warping()
{
    const ULONGLONG at = g_warp_at.load();
    return at != 0 && GetTickCount64() - at < kWarpLongest;
}

std::string last(bool incoming, std::uint16_t id, double* at)
{
    const std::lock_guard lock(g_last_lock);
    const Last& last = g_last[incoming][id & 0x1FF];
    if (at != nullptr)
        *at = last.at;
    return std::string(reinterpret_cast<const char*>(last.data), last.length);
}

} // namespace vanacore::packets
