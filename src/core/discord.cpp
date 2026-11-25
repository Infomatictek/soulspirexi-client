#include "discord.hpp"

#include <windows.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "compat/ashita/host.hpp" // resource_string: the zones' and jobs' names, from the game's DATs
#include "game.hpp"
#include "json.hpp"
#include "log.hpp"
#include "packets.hpp"

// Ported from XI on Anything (rubymatrix/xi-on-anything, host/discord.c, MIT), with its author's permission: the pipe,
// its frames, the handshake and SET_ACTIVITY, the 2 s look at the game, the 5 s between activities and the 15 s between
// looks for Discord are his. The game's side is VanaCore's own readers (game.hpp, packets.hpp) on the game's thread; the
// pipe is a thread of its own here.
namespace vanacore::discord {
namespace {

// SoulspireXI: disabled by default until a SoulspireXI Discord application ID is explicitly configured.
constexpr char kAppId[] = "";
constexpr char kArt[] = "logo";
constexpr char kArtText[] = "SoulspireXI";
constexpr ULONGLONG kReadMs = 2000;      // how often the game is read while Discord is there
constexpr ULONGLONG kIdleReadMs = 15000; // and while it isn't: who is logged in, for the start time
constexpr ULONGLONG kMinSendMs = 5000;   // between activities: Discord takes about 5 in 20 s
constexpr ULONGLONG kRetryMs = 15000;    // between looks for Discord
constexpr DWORD kListenMs = 2000;        // between looks at what Discord sent, connected
constexpr std::uint32_t kMaxFrame = 64 * 1024; // a frame from Discord longer than this: the pipe is out of step
constexpr ULONGLONG kReadyMs = 15000;    // a pipe that hasn't said READY by then isn't Discord's (or Discord is stuck)
constexpr int kInvalidClient = 4000;     // the code of Discord's close for an application it doesn't know
constexpr size_t kMaxText = 128;         // Discord's longest details and state
// 0x00B's LogoutState for a zone change, and into a Mog House (1 is a logout: GP_GAME_LOGOUT_STATE)
constexpr std::uint8_t kZoneChange = 2, kMyRoom = 3;

// Discord's opcodes
constexpr std::uint32_t kHandshake = 0, kFrame = 1, kClose = 2, kPing = 3, kPong = 4;

// The game as the game's thread last read it, for the Discord thread
struct Game {
    bool known = false;    // read since the game started: an activity can be made
    bool in_world = false; // a character, in the world or between zones; false: the title screen, the character select
    std::string name;      // Shift-JIS, as the game has it
    std::uint8_t main_job = 0, main_level = 0, sub_job = 0, sub_level = 0;
    std::uint16_t zone = 0;
    std::int64_t start = 0; // Unix seconds: the character's login, else the game's start
    bool operator==(const Game&) const = default;
};

// [discord]
struct Settings {
    bool enabled = true;
    bool name = true; // name: the character's name shown
    bool operator==(const Settings&) const = default;
};

std::mutex g_lock; // g_game, g_settings
Game g_game;
Settings g_settings;
bool g_read = false;    // g_settings read from vanacore.ini
HANDLE g_wake = nullptr; // set when either changes: the Discord thread looks again at once
std::once_flag g_started;
std::atomic<bool> g_enabled{false};
std::atomic<bool> g_connected{false}; // Discord answered the handshake

// The game's thread's
ULONGLONG g_next_read = 0;
bool g_was_connected = false;
std::string g_who;        // the character logged in (between zones too), "" at the character select
std::int64_t g_login = 0; // when g_who logged in
std::int64_t g_boot = 0;  // when the game started
Game g_seen;              // what was handed over last

// The Discord thread's
HANDLE g_pipe = INVALID_HANDLE_VALUE;
int g_pipe_number = 0;
int g_first_pipe = 0;    // where the next look starts: past a pipe that never said READY
ULONGLONG g_opened = 0;  // when the pipe was opened
bool g_refused = false;  // Discord turned the application down: not tried again until [discord] changes
Settings g_refused_by;   // as [discord] was then
bool g_ready = false;    // Discord's READY came
bool g_error_logged = false;
std::string g_sent;      // the activity Discord has (its JSON), "" for none
ULONGLONG g_last_send = 0;
ULONGLONG g_next_try = 0;
unsigned g_nonce = 0;
std::vector<char> g_in;  // what Discord sent that isn't a whole frame yet

// ---- the game's side (its thread) ----------------------------------------------------------------------------------

// The server's last zone-out (0x00B) was a zone change, not a logout: the character comes back
bool zone_change()
{
    const std::string packet = packets::last(true, 0x00B);
    return packet.size() > 4 && (static_cast<std::uint8_t>(packet[4]) == kZoneChange || static_cast<std::uint8_t>(packet[4]) == kMyRoom);
}

// The character, its jobs and its zone now. Between zones the game has no player: the last ones read, in the zone the
// server named in its zone-in (0x00A) once it has
Game read_game()
{
    Game out;
    out.known = true;
    out.start = g_boot;
    if (g_who.empty())
        return out;
    out.in_world = true;
    out.name = g_who;
    out.start = g_login;
    game::Player player;
    if (game::player(player) && player.main_job != 0 && player.main_job_level != 0) {
        out.main_job = player.main_job;
        out.main_level = player.main_job_level;
        out.sub_job = player.sub_job;
        out.sub_level = player.sub_job_level;
    } else if (g_seen.in_world) {
        out.main_job = g_seen.main_job;
        out.main_level = g_seen.main_level;
        out.sub_job = g_seen.sub_job;
        out.sub_level = g_seen.sub_level;
    }
    game::PartyMember members[18];
    game::Alliance alliance;
    if (game::party(members, alliance))
        out.zone = packets::player_zone(members[0].zone);
    if (out.zone == 0 && g_seen.in_world)
        out.zone = g_seen.zone;
    return out;
}

// ---- Discord's side (its thread) -----------------------------------------------------------------------------------

// Game text (Shift-JIS) as UTF-8
std::string utf8(const char* text)
{
    if (text == nullptr || text[0] == '\0')
        return {};
    const int wide = MultiByteToWideChar(932, 0, text, -1, nullptr, 0);
    if (wide <= 1)
        return {};
    std::wstring w(static_cast<size_t>(wide), L'\0');
    MultiByteToWideChar(932, 0, text, -1, w.data(), wide);
    const int size = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (size <= 1)
        return {};
    std::string out(static_cast<size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, out.data(), size, nullptr, nullptr);
    out.resize(static_cast<size_t>(size - 1));
    return out;
}

// At most Discord's longest (kMaxText characters), cut where a character starts
std::string clamp(std::string text)
{
    size_t count = 0, at = 0;
    for (; at < text.size(); ++at)
        if ((static_cast<unsigned char>(text[at]) & 0xC0) != 0x80 && count++ == kMaxText)
            break;
    text.resize(at);
    return text;
}

// "WAR75", "" before the job is known
std::string job(std::uint8_t id, std::uint8_t level)
{
    const char* abbr = id != 0 && level != 0 ? ashita::resource_string("jobs.names_abbr", id, 0) : nullptr;
    const std::string text = utf8(abbr);
    return text.empty() ? text : text + std::to_string(level);
}

// The activity for `game`: details the character and its jobs ("Name (WAR75/NIN37)", "WAR75/NIN37" with name=0), state
// its zone; at the character select, "Character select". Timed from the login, else the game's start.
json::Value activity(const Game& game, bool show_name)
{
    json::Value out = json::Value::make_object();
    if (game.in_world) {
        std::string jobs = job(game.main_job, game.main_level);
        if (const std::string sub = job(game.sub_job, game.sub_level); !jobs.empty() && !sub.empty())
            jobs += "/" + sub;
        const std::string name = show_name ? utf8(game.name.c_str()) : std::string();
        if (!name.empty())
            out["details"] = clamp(jobs.empty() ? name : name + " (" + jobs + ")");
        else
            out["details"] = jobs.empty() ? std::string("In Vana'diel") : jobs;
        const char* zone = game.zone != 0 ? ashita::resource_string("zones.names", game.zone, 0) : nullptr;
        if (const std::string text = utf8(zone); text.size() >= 2) // Discord's shortest
            out["state"] = clamp(text);
    } else {
        out["state"] = "Character select";
    }
    if (game.start != 0)
        out["timestamps"]["start"] = game.start;
    json::Value& assets = out["assets"];
    assets["large_image"] = kArt;
    assets["large_text"] = kArtText;
    return out;
}

// SET_ACTIVITY with `activity` (null: none, which clears it)
std::string set_activity(const json::Value& activity)
{
    json::Value command = json::Value::make_object();
    command["cmd"] = "SET_ACTIVITY";
    json::Value& args = command["args"];
    args["pid"] = GetCurrentProcessId();
    args["activity"] = activity;
    command["nonce"] = std::to_string(++g_nonce);
    return json::dump(command);
}

bool pipe_open() { return g_pipe != INVALID_HANDLE_VALUE; }

bool send(std::uint32_t op, const std::string& json)
{
    const auto length = static_cast<std::uint32_t>(json.size());
    std::string frame(8, '\0');
    std::memcpy(frame.data(), &op, 4);
    std::memcpy(frame.data() + 4, &length, 4);
    frame += json;
    DWORD written = 0;
    return WriteFile(g_pipe, frame.data(), static_cast<DWORD>(frame.size()), &written, nullptr) != FALSE &&
           written == frame.size();
}

void disconnect()
{
    if (g_pipe != INVALID_HANDLE_VALUE)
        CloseHandle(g_pipe);
    g_pipe = INVALID_HANDLE_VALUE;
    g_ready = false;
    g_connected = false;
    g_sent.clear(); // a new connection starts with no activity
    g_in.clear();
}

// The first Discord pipe there is, and the handshake as kAppId. False if Discord isn't running.
bool connect()
{
    for (int k = 0; k < 10; ++k) {
        const int i = (g_first_pipe + k) % 10;
        wchar_t path[40] = {};
        swprintf_s(path, L"\\\\.\\pipe\\discord-ipc-%d", i);
        // identification only: whatever holds the pipe's name can't act as this process
        const HANDLE pipe = CreateFileW(path, GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                                        SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, nullptr);
        if (pipe == INVALID_HANDLE_VALUE)
            continue;
        g_pipe = pipe;
        g_pipe_number = i;
        g_opened = GetTickCount64();
        g_error_logged = false;
        json::Value hello = json::Value::make_object();
        hello["v"] = 1;
        hello["client_id"] = kAppId;
        if (send(kHandshake, json::dump(hello)))
            return true;
        disconnect();
    }
    return false;
}

enum class Heard { Open, Lost, Refused };

// One frame from Discord: its READY (connected), a ping (answered), an error, a close
Heard heard(std::uint32_t op, const std::string& body)
{
    json::Value value;
    std::string error;
    json::parse(body, value, error);
    if (op == kPing)
        return send(kPong, body) ? Heard::Open : Heard::Lost;
    if (op == kClose) {
        // turned down before READY for an application (kAppId) Discord doesn't know: not tried again. Any other close
        // (Discord starting, signing in, closing) is tried again in 15 s
        if (g_ready || static_cast<int>(value.get_number("code")) != kInvalidClient)
            return Heard::Lost;
        log::write(L"Discord: Discord doesn't know application %hs (\"%hs\"): not tried again until [discord] changes",
                   kAppId, value.get_string("message").substr(0, 120).c_str());
        return Heard::Refused;
    }
    if (op != kFrame)
        return Heard::Open;
    const std::string evt = value.get_string("evt");
    if (evt == "READY" && !g_ready) {
        g_ready = true;
        g_connected = true;
        log::write(L"Discord: connected (discord-ipc-%d, application %hs): rich presence on", g_pipe_number, kAppId);
    } else if (evt == "ERROR" && !g_error_logged) {
        const json::Value* data = value.find("data");
        g_error_logged = true; // once a connection
        log::write(L"Discord: Discord turned the activity down (%d, \"%hs\")", data != nullptr ? static_cast<int>(data->get_number("code")) : 0,
                   data != nullptr ? data->get_string("message").substr(0, 160).c_str() : "");
    }
    return Heard::Open;
}

// What Discord sent since the last look, without waiting for more, each whole frame heard
Heard listen()
{
    // the pipe gone (Discord closes its end after a close frame): what it sent before is heard first
    bool lost = false;
    for (;;) {
        DWORD available = 0;
        if (!PeekNamedPipe(g_pipe, nullptr, 0, nullptr, &available, nullptr)) {
            lost = true;
            break;
        }
        if (available == 0)
            break;
        const size_t had = g_in.size();
        g_in.resize(had + available);
        DWORD got = 0;
        if (!ReadFile(g_pipe, g_in.data() + had, available, &got, nullptr) || got == 0) {
            g_in.resize(had);
            lost = true;
            break;
        }
        g_in.resize(had + got);
    }
    while (g_in.size() >= 8) {
        std::uint32_t op = 0, length = 0;
        std::memcpy(&op, g_in.data(), 4);
        std::memcpy(&length, g_in.data() + 4, 4);
        if (length > kMaxFrame)
            return Heard::Lost;
        if (g_in.size() < 8 + static_cast<size_t>(length))
            break;
        const std::string body(g_in.data() + 8, length);
        g_in.erase(g_in.begin(), g_in.begin() + static_cast<std::ptrdiff_t>(8 + static_cast<size_t>(length)));
        if (const Heard result = heard(op, body); result != Heard::Open)
            return result;
    }
    return lost ? Heard::Lost : Heard::Open;
}

// One look: Discord let go of or found, what it sent heard, the activity sent if it changed. How long until the next (ms).
DWORD step()
{
    Settings settings;
    Game game;
    {
        const std::lock_guard lock(g_lock);
        settings = g_settings;
        game = g_game;
    }
    if (!settings.enabled)
        g_refused = false; // turned off and on again: tried again
    const bool wanted = settings.enabled && !(g_refused && settings == g_refused_by);
    if (pipe_open() && !wanted) {
        if (g_ready)
            send(kFrame, set_activity(json::Value())); // cleared, before letting go
        if (g_ready && !settings.enabled)
            log::write(L"Discord: off ([discord] enabled=0): the activity cleared");
        disconnect();
        g_next_try = 0;
    }
    if (!wanted)
        return INFINITE;
    const ULONGLONG now = GetTickCount64();
    if (!pipe_open()) {
        if (now < g_next_try)
            return static_cast<DWORD>(g_next_try - now);
        g_next_try = now + kRetryMs;
        if (!connect())
            return static_cast<DWORD>(kRetryMs); // not running: nothing else until the next look
    }
    const bool was_ready = g_ready;
    if (const Heard result = listen(); result != Heard::Open) {
        if (result == Heard::Refused)
            g_refused = true, g_refused_by = settings;
        else if (g_ready)
            log::write(L"Discord: the connection to Discord was lost: looking for it again every 15 s");
        disconnect();
        return result == Heard::Refused ? INFINITE : static_cast<DWORD>(kRetryMs);
    }
    if (!g_ready && now - g_opened > kReadyMs) {
        // a pipe that never answers: another program's, or Discord stuck. The next look starts past it
        g_first_pipe = (g_pipe_number + 1) % 10;
        disconnect();
        return static_cast<DWORD>(kRetryMs);
    }
    if (g_ready && !was_ready)
        g_first_pipe = g_pipe_number;
    // READY just now: the game's thread hands over the game as it is now (g_game may be from before a reconnect) and
    // wakes this one
    if (!g_ready || !was_ready || !game.known)
        return kListenMs;
    const json::Value now_activity = activity(game, settings.name);
    std::string text = json::dump(now_activity);
    if (text == g_sent)
        return kListenMs;
    if (g_last_send != 0 && now - g_last_send < kMinSendMs)
        return static_cast<DWORD>(kMinSendMs - (now - g_last_send));
    if (!send(kFrame, set_activity(now_activity))) {
        log::write(L"Discord: the connection to Discord was lost: looking for it again every 15 s");
        disconnect();
        return static_cast<DWORD>(kRetryMs);
    }
    g_sent = std::move(text);
    g_last_send = now;
    return kListenMs;
}

void run()
{
    for (;;)
        WaitForSingleObject(g_wake, step());
}

} // namespace

void reload(const wchar_t* ini)
{
    if (g_wake == nullptr) // not installed
        return;
    Settings settings;
    const bool file = ini != nullptr && ini[0] != L'\0';
    // Disabled by default in SoulspireXI minimal client and whenever no Discord application ID is configured.
    settings.enabled = kAppId[0] != '\0' && file && GetPrivateProfileIntW(L"discord", L"enabled", 0, ini) != 0;
    settings.name = !file || GetPrivateProfileIntW(L"discord", L"name", 1, ini) != 0;
    {
        const std::lock_guard lock(g_lock);
        if (g_read && settings == g_settings)
            return;
        g_read = true;
        g_settings = settings;
    }
    g_enabled = settings.enabled;
    if (settings.enabled)
        try {
            std::call_once(g_started, [] { std::thread(run).detach(); });
        } catch (const std::system_error& error) {
            log::write(L"Discord: its thread couldn't start (%hs): [discord] does nothing", error.what());
        }
    SetEvent(g_wake);
}

void install(const wchar_t* ini)
{
    g_boot = static_cast<std::int64_t>(std::time(nullptr));
    g_wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (g_wake == nullptr) {
        log::write(L"Discord: no event for its thread (error %lu): [discord] does nothing", GetLastError());
        return;
    }
    reload(ini);
}

void on_frame()
{
    if (!g_enabled.load(std::memory_order_relaxed))
        return;
    const bool connected = g_connected.load(std::memory_order_relaxed);
    const ULONGLONG now = GetTickCount64();
    if (now < g_next_read && connected == g_was_connected)
        return;
    g_next_read = now + (connected ? kReadMs : kIdleReadMs);
    g_was_connected = connected;

    // Who is logged in, kept between zones; the title screen or the character select (a logout) forgets them, so the
    // next login is timed afresh
    const std::string name = game::local_player_name();
    const bool zoning = packets::zoning(!name.empty());
    if (!name.empty()) {
        if (name != g_who) {
            g_who = name;
            g_login = static_cast<std::int64_t>(std::time(nullptr));
        }
    } else if (!(zoning && zone_change())) {
        g_who.clear();
    }
    if (!connected)
        return;
    const Game state = read_game();
    if (state == g_seen)
        return;
    g_seen = state;
    {
        const std::lock_guard lock(g_lock);
        g_game = state;
    }
    SetEvent(g_wake);
}

} // namespace vanacore::discord
