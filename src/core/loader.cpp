#include "loader.hpp"

#include <vanacore/tls_client.hpp>

#include <bcrypt.h>
#include <objbase.h>

#include <vanacore/console.hpp>

#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <initializer_list>
#include <mutex>
#include <string>

#include "client.hpp"
#include "imports.hpp"
#include "log.hpp"
#include "scan.hpp"
#include "shutdown.hpp"

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "bcrypt.lib") // MD5 of the session hash (the zone's key)

namespace vanacore::loader {
namespace {

// --- PlayOnline's and FFXI's COM classes (the loader creates them itself) ------------------------

// polcore's class and interface per region: JP, US, EU (GameLogin::language).
constexpr CLSID kPolCoreClass[3] = {
    {0x07974581, 0x0DF6, 0x4EF0, {0xBD, 0x05, 0x60, 0x4B, 0x3A, 0xDA, 0x9B, 0xE9}},
    {0x3501F5DD, 0x7894, 0x42DF, {0x86, 0x6A, 0xA2, 0xB6, 0x52, 0x7D, 0x80, 0x49}},
    {0xE5966FB3, 0xC97B, 0x42EB, {0x84, 0xBF, 0x37, 0xF9, 0x5E, 0xE5, 0x4A, 0x9F}},
};
constexpr IID kPolCoreInterface[3] = {
    {0x9A30D565, 0xA74C, 0x4B56, {0xB9, 0x71, 0xDC, 0xF0, 0x21, 0x85, 0xB1, 0x0D}},
    {0xE0516654, 0xEF77, 0x435D, {0xAA, 0x7D, 0x50, 0xD2, 0xC0, 0x69, 0xCE, 0x34}},
    {0xDFEC2E93, 0x4971, 0x4A54, {0xB8, 0xED, 0x63, 0x81, 0x5C, 0x20, 0x8C, 0x5A}},
};
// FFXi.dll's FFXiEntry
constexpr CLSID kGameClass = {0x989D790D, 0x6236, 0x11D4, {0x80, 0xE9, 0x00, 0x10, 0x5A, 0x81, 0xE8, 0x90}};
constexpr IID kGameInterface = {0x989D790C, 0x6236, 0x11D4, {0x80, 0xE9, 0x00, 0x10, 0x5A, 0x81, 0xE8, 0x90}};

// The vtable slots called, IUnknown's three first: IPOLCoreCom's and IFFXiEntry's.
constexpr int kSetParamInit = 5;           // (HINSTANCE, char* command line)
constexpr int kGetCommonFunctionTable = 7; // (void*** table)
constexpr int kSetAreaCode = 27;           // (long region)
constexpr int kGameStart = 3;              // (IUnknown* polcore, IUnknown** message)

// polcore's common function table: the entries that set it up without the PlayOnline viewer.
constexpr int kSetInstallFolder = 0x007D; // (const char* PlayOnline's folder)
constexpr int kSetRegistryKey = 0x016F;   // (const char* "SOFTWARE\\PlayOnlineUS")
constexpr int kSetGameLanguage = 0x01A4;  // (int PlayOnline's language setting)
constexpr int kInetMutex = 0x032F;        // ()
constexpr int kSetRegion = 0x03C5;        // (int region)
constexpr int kSetMessageText = 0x0516;   // (int message, const char* text)
constexpr int kShowError = 0x0402;        // (int code, const char* text): how FFXi.dll reports why it can't start
constexpr int kSessionValue = 0x03EB;     // (uint8_t out[16]): the session value FFXiMain keys the zone with
constexpr int kNameUnavailable = 0x04;    // the message for a character name that's taken
constexpr int kCharacterSlots = 0x00D3;   // its code holds the address of the lobby's character slots at +31
constexpr size_t kCharacterSlotSize = 0x68;

using Command = int(__cdecl*)(...);

template <typename... Args>
HRESULT call(void* object, int slot, Args... args)
{
    using Method = HRESULT(__stdcall*)(void*, Args...);
    return reinterpret_cast<Method>((*static_cast<void***>(object))[slot])(object, args...);
}

// polcore's error report, which FFXi.dll calls when it gives up starting ("<what>(<code>)"): it shows nothing
// without the PlayOnline viewer, so the text goes to the log first.
Command g_show_error = nullptr;

int __cdecl show_error(int code, const char* text)
{
    log::write(L"Game start: FFXI reported an error: %hs (code %d)", text != nullptr ? text : "(no text)", code);
    return g_show_error(code, text);
}

// MD5 of `size` bytes at `data` into `out`, by Windows (BCrypt, with its own hash object: Windows 7 and later); false if it
// can't
bool md5(const void* data, ULONG size, std::uint8_t (&out)[16])
{
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    if (!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_MD5_ALGORITHM, nullptr, 0)))
        return false;
    BCRYPT_HASH_HANDLE hash = nullptr;
    bool done = false;
    if (BCRYPT_SUCCESS(BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0))) {
        done = BCRYPT_SUCCESS(BCryptHashData(hash, static_cast<PUCHAR>(const_cast<void*>(data)), size, 0)) &&
               BCRYPT_SUCCESS(BCryptFinishHash(hash, out, sizeof(out), 0));
        BCryptDestroyHash(hash);
    }
    BCryptCloseAlgorithmProvider(algorithm, 0);
    return done;
}

// --- State --------------------------------------------------------------------------------------

GameLogin g_login{};
// The session value: md5(the session hash). FFXiMain starts its zone key with these 16 bytes, and so does the map
// server since LandSandBoat #11736 (2026-10-10), which gives each login its own key; servers before it key the zone
// with the data link's 0xA2 key, which carries the same bytes. PlayOnline's profile server would hand the value to
// polcore; the stand-in here answers zeros, so polcore's slot for it gives this instead (session_value).
std::uint8_t g_session_value[16] = {};
std::atomic<char*> g_character_slots{nullptr}; // polcore's, once found
SOCKET g_data = INVALID_SOCKET;                 // the lobby's data link to the server (legacy plaintext mode)
vanacore::tls::Client g_data_tls;               // the lobby's TLS 1.3 data link (xiloader v3.0+ mode)
SOCKET g_view_relay = INVALID_SOCKET;           // local loopback TLS 1.3 relay listener for lobby view (54001)
std::uint16_t g_view_relay_port = 0;            // ephemeral loopback port for lobby view TLS 1.3 relay
SOCKET g_profile = INVALID_SOCKET;              // the stand-in profile server's listener
char g_server[INET_ADDRSTRLEN] = {};            // the server's address, dotted
std::wstring g_server_host;                     // the server's hostname for TLS SNI

// polcore keeps pointers to these for the whole session
char g_command_line[64] = {};
char g_install_folder[MAX_PATH] = {};
char g_connection_buffer[0x1000] = {};

// The session value's FNV-1a, as LandSandBoat's xi_connect logs the zone key's ("key value %08X", since #11736): the two
// match when the server keys the zone as the game does. Safe in a log a player sends: it gives nothing to log in with.
std::uint32_t key_fingerprint()
{
    std::uint32_t hash = 2166136261u;
    for (const std::uint8_t byte : g_session_value)
        hash = (hash ^ byte) * 16777619u;
    return hash;
}

// polcore's copy of the session value, which FFXiMain asks for once, in the lobby (through the table, at the call)
int __cdecl session_value(std::uint8_t* out)
{
    if (out != nullptr)
        std::memcpy(out, g_session_value, sizeof(g_session_value));
    static std::atomic<int> said{3};
    if (said.fetch_sub(1) > 0)
        log::write(L"Lobby: the game took the session value for its zone key (value %08X)", key_fingerprint());
    return static_cast<int>(sizeof(g_session_value));
}

// A line in the log and, in red, on the console VanaCore.exe shares with the game.
void report(const wchar_t* format, ...)
{
    wchar_t message[512];
    va_list args;
    va_start(args, format);
    _vsnwprintf_s(message, _TRUNCATE, format, args);
    va_end(args);
    log::write(L"Game start: %ls", message);
    write_console_line(GetStdHandle(STD_ERROR_HANDLE), std::wstring(L"VanaCore: ") + message, kConsoleRed);
}

// --- winsock: the server's name and the lobby's packets -----------------------------------------

using GetHostByName = hostent*(WINAPI*)(const char* name);
using Send = int(WINAPI*)(SOCKET socket, const char* data, int size, int flags);
using Recv = int(WINAPI*)(SOCKET socket, char* data, int size, int flags);
using Connect = int(WINAPI*)(SOCKET socket, const sockaddr* to, int length);
GetHostByName g_get_host_by_name = nullptr;
Send g_send = nullptr;
Recv g_recv = nullptr;
Connect g_connect = nullptr;

// The game's first connections, lookups and lobby traffic go to the log, this many lines at most.
std::atomic<int> g_trace_lines{60};

bool tracing()
{
    return g_trace_lines.fetch_sub(1) > 0;
}

// "100.4.171.10:54001"
std::string address_text(const sockaddr_in& at)
{
    char text[INET_ADDRSTRLEN] = "?";
    InetNtopA(AF_INET, &at.sin_addr, text, sizeof(text));
    return std::string(text) + ":" + std::to_string(ntohs(at.sin_port));
}

bool peer_of(SOCKET socket, sockaddr_in& peer)
{
    int length = sizeof(peer);
    return getpeername(socket, reinterpret_cast<sockaddr*>(&peer), &length) == 0 && peer.sin_family == AF_INET;
}

// The lobby's ports and this machine: the traffic worth logging
bool traced_peer(const sockaddr_in& peer)
{
    const std::uint16_t port = ntohs(peer.sin_port);
    return port == g_login.view_port || port == g_login.data_port || peer.sin_addr.s_addr == htonl(INADDR_LOOPBACK);
}

// The first bytes of a packet, in hex.
std::wstring head(const char* data, int size)
{
    std::wstring text;
    for (int i = 0; i < size && i < 12; ++i) {
        wchar_t byte[4];
        swprintf_s(byte, L"%02X ", static_cast<unsigned char>(data[i]));
        text += byte;
    }
    return text;
}

#include "loader_zonelink.h" // zonelink: the game's UDP with the map server, for the log

// PlayOnline looks the game's lobby up as ffxi00.pol.com: that's the server. Its profile server,
// pp000.pol.com, is the stand-in on this machine (otherwise Square Enix's own, which never answers).
hostent* WINAPI get_host_by_name(const char* name)
{
    const DWORD error = GetLastError();
    if (name != nullptr && _stricmp(name, "ffxi00.pol.com") == 0) {
        static std::atomic<bool> logged{false};
        if (!logged.exchange(true))
            log::write(L"Lobby: ffxi00.pol.com is the server, %hs", g_server);
        SetLastError(error);
        return g_get_host_by_name(g_server);
    }
    if (name != nullptr && _stricmp(name, "pp000.pol.com") == 0) {
        static std::atomic<bool> logged{false};
        if (!logged.exchange(true))
            log::write(L"Lobby: pp000.pol.com is the stand-in profile server on this machine");
        SetLastError(error);
        return g_get_host_by_name("127.0.0.1");
    }
    if (name != nullptr && tracing())
        log::write(L"Lobby: looking up %hs", name);
    SetLastError(error);
    return g_get_host_by_name(name);
}

int WINAPI connect_traced(SOCKET socket, const sockaddr* to, int length)
{
    sockaddr_in redirected{};
    const sockaddr* actual_to = to;
    int actual_length = length;
    if (g_login.lobby_tls != 0 && g_view_relay_port != 0 && to != nullptr &&
        length >= static_cast<int>(sizeof(sockaddr_in)) && to->sa_family == AF_INET) {
        const auto* in = reinterpret_cast<const sockaddr_in*>(to);
        if (in->sin_addr.s_addr == g_login.server_address && ntohs(in->sin_port) == g_login.view_port) {
            redirected = *in;
            redirected.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            redirected.sin_port = htons(g_view_relay_port);
            actual_to = reinterpret_cast<const sockaddr*>(&redirected);
            actual_length = static_cast<int>(sizeof(redirected));
            if (tracing())
                log::write(L"Lobby: redirecting view port %u through local TLS 1.3 relay (127.0.0.1:%u)",
                           g_login.view_port, g_view_relay_port);
        }
    }
    const int result = g_connect(socket, actual_to, actual_length);
    const DWORD error = GetLastError();
    if (to != nullptr && to->sa_family == AF_INET && tracing())
        log::write(L"Lobby: connecting to %hs: %ls", address_text(*reinterpret_cast<const sockaddr_in*>(to)).c_str(),
                   result == 0 ? L"connected"
                   : error == WSAEWOULDBLOCK ? L"under way (non-blocking)"
                                             : (L"winsock error " + std::to_wstring(error)).c_str());
    SetLastError(error);
    return result;
}

// The lobby's world list (0x23, lpkt_world_list): how many worlds at 28, then each as its number (4 bytes) and its name (16).
// A private server sends one, its own name, the one the character list shows: kept for addons (vana.game.server).
void on_world_list(const char* data, int size)
{
    if (size < 52 || std::memcmp(data + 4, "IXFF", 4) != 0 || static_cast<unsigned char>(data[8]) != 0x23)
        return;
    std::uint32_t worlds = 0;
    std::memcpy(&worlds, data + 28, sizeof(worlds));
    if (worlds == 0)
        return;
    std::string name;
    for (int i = 0; i < 16 && data[36 + i] != '\0'; ++i) {
        const unsigned char c = static_cast<unsigned char>(data[36 + i]);
        name += c >= 0x20 && c < 0x7F ? static_cast<char>(c) : '?';
    }
    while (!name.empty() && name.back() == ' ')
        name.pop_back();
    if (!name.empty())
        client::set_server_name(name);
}

int WINAPI recv_traced(SOCKET socket, char* data, int size, int flags)
{
    const int result = g_recv(socket, data, size, flags);
    const DWORD error = GetLastError();
    sockaddr_in peer{};
    if (result != SOCKET_ERROR || error != WSAEWOULDBLOCK)
        if (peer_of(socket, peer) && traced_peer(peer) && tracing())
            log::write(L"Lobby: from %hs, %d bytes: %ls", address_text(peer).c_str(), result,
                       result > 0 ? head(data, result).c_str() : L"(closed or failed)");
    if (result > 0 && (flags & MSG_PEEK) == 0) {
        zonelink::on_lobby_reply(data, result);
        on_world_list(data, result);
    }
    SetLastError(error);
    return result;
}

// A lobby request's command: "IXFF" at 4, a lobby command at 8. 0 for anything else.
unsigned lobby_command(const char* data, int size)
{
    if (data == nullptr || size < 28 || std::memcmp(data + 4, "IXFF", 4) != 0)
        return 0;
    const unsigned command = static_cast<unsigned char>(data[8]);
    switch (command) {
    case 0x07: // log in with this character
    case 0x14: // delete a character
    case 0x1F: // the character list
    case 0x21: // a character was made
    case 0x22: // make a character (name check)
    case 0x24: // the server's name
    case 0x26: // the client's version
    case 0x28: // a renamed character
    case 0x2B: // move a character (GM)
        return command;
    default:
        return 0;
    }
}

// The lobby's requests to its view or data port carry the session: the server knows it by the hash at 12.
int WINAPI send_lobby(SOCKET socket, const char* data, int size, int flags)
{
    const DWORD error = GetLastError();
    const unsigned command = lobby_command(data, size);
    sockaddr_in peer{};
    const bool known_peer = peer_of(socket, peer);
    if (command == 0) {
        if (known_peer && traced_peer(peer) && tracing())
            log::write(L"Lobby: to %hs, %d bytes: %ls", address_text(peer).c_str(), size, head(data, size).c_str());
        SetLastError(error);
        return g_send(socket, data, size, flags);
    }
    const std::uint16_t port = known_peer ? ntohs(peer.sin_port) : 0;
    const bool is_view_relay =
        known_peer && g_view_relay_port != 0 && port == g_view_relay_port && peer.sin_addr.s_addr == htonl(INADDR_LOOPBACK);
    const bool lobby = port == g_login.view_port || port == g_login.data_port || is_view_relay;
    // each command's first, in the log
    static std::atomic<std::uint64_t> logged{0};
    const std::uint64_t bit = std::uint64_t{1} << (command & 63);
    if ((logged.fetch_or(bit) & bit) == 0)
        log::write(L"Lobby: request 0x%02X to port %u%ls", command, port,
                   lobby ? L", with the session" : L": not the lobby's port, sent as it is");
    SetLastError(error);
    if (!lobby)
        return g_send(socket, data, size, flags);
    std::string stamped(data, static_cast<size_t>(size));
    std::memcpy(stamped.data() + 12, g_login.session_hash, sizeof(g_login.session_hash));
    return g_send(socket, stamped.data(), size, flags);
}

// The first bytes of code, in hex, for the log: what a system function starts with.
std::wstring code_head(const void* code)
{
    if (code == nullptr)
        return L"(not found)";
    std::wstring text;
    for (int i = 0; i < 12; ++i) {
        wchar_t byte[4];
        swprintf_s(byte, L"%02X ", static_cast<const unsigned char*>(code)[i]);
        text += byte;
    }
    return text;
}

// winsock's own send and gethostbyname, patched for every caller as xiloader does (with Detours):
// PlayOnline and FFXI reach winsock from several modules, by ordinal. The functions' starts are
// decoded rather than matched (a newer Windows, or a VPN or game booster that already hooks winsock,
// starts them differently). False (logged, with the bytes) if they can't be patched.
bool hook_winsock()
{
    const HMODULE ws2 = GetModuleHandleW(L"ws2_32.dll");
    const auto send_function = ws2 != nullptr ? reinterpret_cast<Send>(GetProcAddress(ws2, "send")) : nullptr;
    const auto host_function =
        ws2 != nullptr ? reinterpret_cast<GetHostByName>(GetProcAddress(ws2, "gethostbyname")) : nullptr;
    if (send_function == nullptr || host_function == nullptr) {
        report(L"this Windows' winsock (ws2_32.dll) has no send or gethostbyname to patch");
        return false;
    }
    if (!imports::detour_auto(host_function, &get_host_by_name, g_get_host_by_name) ||
        !imports::detour_auto(send_function, &send_lobby, g_send)) {
        report(L"couldn't patch winsock's send and gethostbyname (they start with %ls and %ls)",
               code_head(reinterpret_cast<const void*>(send_function)).c_str(),
               code_head(reinterpret_cast<const void*>(host_function)).c_str());
        return false;
    }
    // connect and recv only for the log: what the game connects to, and what the lobby answers
    const auto connect_function = reinterpret_cast<Connect>(GetProcAddress(ws2, "connect"));
    const auto recv_function = reinterpret_cast<Recv>(GetProcAddress(ws2, "recv"));
    const bool traced = connect_function != nullptr && recv_function != nullptr &&
                        imports::detour_auto(connect_function, &connect_traced, g_connect) &&
                        imports::detour_auto(recv_function, &recv_traced, g_recv);
    log::write(L"Game start: winsock's send and gethostbyname patched (the lobby's server and session)%ls",
               traced ? L"; its connections are logged" : L"");
    if (!zonelink::install(ws2))
        log::write(L"Game start: winsock's sendto, recvfrom and bind weren't patched: the map server's traffic isn't logged");
    return true;
}

// --- The lobby's data link: the server asks, the loader answers ---------------------------------

bool send_all(SOCKET socket, const char* data, int size)
{
    while (size > 0) {
        const int sent = ::send(socket, data, size, 0);
        if (sent <= 0)
            return false;
        data += sent;
        size -= sent;
    }
    return true;
}

bool send_all_raw(SOCKET socket, const char* data, int size)
{
    while (size > 0) {
        const int sent = g_send != nullptr ? g_send(socket, data, size, 0) : ::send(socket, data, size, 0);
        if (sent <= 0)
            return false;
        data += sent;
        size -= sent;
    }
    return true;
}

bool send_data_packet(const char* data, int size)
{
    if (g_login.lobby_tls != 0)
        return g_data_tls.write(data, static_cast<std::size_t>(size));
    return send_all(g_data, data, size);
}

int recv_data_packet(char* data, int size, std::wstring& error)
{
    if (g_login.lobby_tls != 0)
        return g_data_tls.read_some(data, size, error);
    return ::recv(g_data, data, size, 0);
}

DWORD WINAPI data_link(void*)
{
    char out[28] = {};
    out[0] = static_cast<char>(0xFE); // the session, so the server can pair this link with the login
    std::memcpy(out + 12, g_login.session_hash, sizeof(g_login.session_hash));
    if (!send_data_packet(out, static_cast<int>(sizeof(out)))) {
        log::write(L"Lobby: couldn't open the data link (winsock error %d)", WSAGetLastError());
        return 0;
    }
    char in[4096];
    std::wstring tls_error;
    for (;;) {
        const int got = recv_data_packet(in, sizeof(in), tls_error);
        if (got < 2) {
            if (got < 0 && !tls_error.empty())
                log::write(L"Lobby: data link TLS read error: %ls", tls_error.c_str());
            break; // the server closed it (the lobby is done), or the game is closing
        }
        std::memset(out, 0, sizeof(out));
        switch (static_cast<unsigned char>(in[0])) {
        case 0x01: // who's this: the account, and the server as the game knows it
            out[0] = static_cast<char>(0xA1);
            std::memcpy(out + 1, &g_login.account_id, 4);
            std::memcpy(out + 5, &g_login.server_address, 4);
            std::memcpy(out + 12, g_login.session_hash, sizeof(g_login.session_hash));
            log::write(L"Lobby: sending the account");
            send_data_packet(out, static_cast<int>(sizeof(out)));
            break;
        case 0x02:
        case 0x15: // the key: the session value (what servers before LandSandBoat #11736 key the zone with), and
                   // the counter's start, fixed
            out[0] = static_cast<char>(0xA2);
            std::memcpy(out + 1, g_session_value, sizeof(g_session_value));
            out[17] = static_cast<char>(0x58);
            out[18] = static_cast<char>(0xE0);
            out[19] = static_cast<char>(0x5D);
            out[20] = static_cast<char>(0xAD);
            log::write(L"Lobby: sending the key (value %08X, LandSandBoat xi_connect's \"key value\" fingerprint)",
                       key_fingerprint());
            send_data_packet(out, static_cast<int>(sizeof(out)));
            break;
        case 0x03: { // the characters: into polcore's slots, where the lobby lists them
            char* slots = g_character_slots.load();
            const int count = static_cast<unsigned char>(in[1]);
            if (slots == nullptr) {
                log::write(L"Lobby: %d characters arrived before PlayOnline was ready; not shown", count);
                break;
            }
            int shown = 0;
            for (int i = 0; i < count && 0x10 * (i + 1) + 8 <= got; ++i, ++shown) {
                char* slot = slots + static_cast<size_t>(i) * kCharacterSlotSize;
                const char* character = in + 0x10 * (i + 1);
                slot[0x00] = 1;
                slot[0x02] = 1;
                slot[0x10] = static_cast<char>(i);
                slot[0x11] = static_cast<char>(0x80);
                slot[0x18] = 0x20;
                slot[0x28] = 0x20;
                std::memcpy(slot + 0x04, character + 4, 4); // character id
                std::memcpy(slot + 0x08, character, 4);     // content id
            }
            log::write(L"Lobby: %d character%ls", shown, shown == 1 ? L"" : L"s");
            break;
        }
        default:
            break;
        }
    }
    log::write(L"Lobby: data link closed");
    return 0;
}

// --- Local TLS 1.3 relay for the lobby view port (54001) on LandSandBoat xiloader v3.0+ ----------

struct ViewRelayPump {
    SOCKET local;
    vanacore::tls::Client* remote;
};

DWORD WINAPI view_relay_local_to_tls(void* parameter)
{
    auto* pump = static_cast<ViewRelayPump*>(parameter);
    char buffer[16384];
    for (;;) {
        const int got = g_recv != nullptr ? g_recv(pump->local, buffer, sizeof(buffer), 0)
                                          : ::recv(pump->local, buffer, sizeof(buffer), 0);
        if (got <= 0)
            break;
        if (!pump->remote->write(buffer, static_cast<std::size_t>(got)))
            break;
    }
    pump->remote->shutdown_socket();
    return 0;
}

DWORD WINAPI view_relay_client(void* parameter)
{
    const SOCKET local = reinterpret_cast<SOCKET>(parameter);
    vanacore::tls::Client remote;
    std::wstring tls_error;
    const std::uint8_t* expected_key = g_login.has_server_key != 0 ? g_login.server_key_sha256 : nullptr;
    if (!remote.connect(g_login.server_address, g_login.view_port, g_server_host, tls_error, expected_key, 30000,
                        g_connect)) {
        log::write(L"Lobby: TLS 1.3 view relay connection to %hs:%u failed: %ls", g_server, g_login.view_port,
                   tls_error.c_str());
        closesocket(local);
        return 0;
    }
    log::write(L"Lobby: TLS view relay connected to %hs:%u (%ls)", g_server, g_login.view_port,
               vanacore::tls::protocol_name(remote.negotiated_protocol()).c_str());

    ViewRelayPump pump{local, &remote};
    HANDLE writer = CreateThread(nullptr, 0, &view_relay_local_to_tls, &pump, 0, nullptr);
    char buffer[16384];
    for (;;) {
        const int got = remote.read_some(buffer, static_cast<int>(sizeof(buffer)), tls_error);
        if (got <= 0)
            break;
        if (!send_all_raw(local, buffer, got))
            break;
    }
    ::shutdown(local, SD_BOTH);
    remote.shutdown_socket();
    if (writer != nullptr) {
        WaitForSingleObject(writer, 2000);
        CloseHandle(writer);
    }
    closesocket(local);
    return 0;
}

DWORD WINAPI view_relay_server(void*)
{
    for (;;) {
        const SOCKET client = accept(g_view_relay, nullptr, nullptr);
        if (client == INVALID_SOCKET)
            break;
        const HANDLE thread = CreateThread(nullptr, 0, &view_relay_client, reinterpret_cast<void*>(client), 0, nullptr);
        if (thread != nullptr)
            CloseHandle(thread);
        else
            closesocket(client);
    }
    return 0;
}

// --- PlayOnline's profile server, stood in for on this machine ----------------------------------

DWORD WINAPI profile_client(void* parameter)
{
    const SOCKET client = reinterpret_cast<SOCKET>(parameter);
    unsigned char buffer[1024] = {};
    bool new_character = false;
    // polcore's three requests, each answered as PlayOnline's server would for a known account
    for (int step = 0; step < 3; ++step) {
        const int got = recv(client, reinterpret_cast<char*>(buffer), sizeof(buffer), 0);
        if (got <= 0)
            break;
        const unsigned char kind = buffer[4];
        std::memset(buffer, 0, 32);
        int reply = got;
        if (step == 0) {
            buffer[0] = 0x81;
            const auto now = static_cast<std::uint32_t>(std::time(nullptr));
            std::memcpy(buffer + 0x14, &now, sizeof(now));
            reply = 24;
        } else if (step == 1) {
            new_character = kind != 0x28;
            buffer[0x00] = 0x28;
            buffer[0x04] = 0x20;
            buffer[0x08] = 0x01;
            buffer[0x0B] = 0x7F;
            reply = new_character ? 144 : 24;
        }
        log::write(L"Profile: PlayOnline's request %d (%d bytes, kind 0x%02X), answered with %d bytes", step + 1, got,
                   kind, reply);
        if (!send_all(client, reinterpret_cast<const char*>(buffer), reply))
            break;
    }
    ::shutdown(client, SD_SEND);
    closesocket(client);
    return 0;
}

DWORD WINAPI profile_server(void*)
{
    for (;;) {
        const SOCKET client = accept(g_profile, nullptr, nullptr);
        if (client == INVALID_SOCKET)
            break; // closed: the game is closing
        log::write(L"Profile: PlayOnline connected to the stand-in profile server");
        const HANDLE thread = CreateThread(nullptr, 0, &profile_client, reinterpret_cast<void*>(client), 0, nullptr);
        if (thread != nullptr)
            CloseHandle(thread);
        else
            closesocket(client);
    }
    return 0;
}

// A listener on this machine at a port Windows picks; 0 if there's none.
std::uint16_t listen_locally(SOCKET& listener)
{
    listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listener == INVALID_SOCKET)
        return 0;
    sockaddr_in at{};
    at.sin_family = AF_INET;
    at.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    int length = sizeof(at);
    if (bind(listener, reinterpret_cast<const sockaddr*>(&at), sizeof(at)) == SOCKET_ERROR ||
        listen(listener, SOMAXCONN) == SOCKET_ERROR ||
        getsockname(listener, reinterpret_cast<sockaddr*>(&at), &length) == SOCKET_ERROR) {
        closesocket(listener);
        listener = INVALID_SOCKET;
        return 0;
    }
    return ntohs(at.sin_port);
}

// --- polcore: set up as the PlayOnline viewer would, without it ---------------------------------

const char* registry_key(std::uint32_t region)
{
    static const char* const kKeys[3] = {"SOFTWARE\\PlayOnline", "SOFTWARE\\PlayOnlineUS", "SOFTWARE\\PlayOnlineEU"};
    return kKeys[region < 3 ? region : 1];
}

// PlayOnline's language setting (its Settings key's Language), else the region's.
int game_language(std::uint32_t region)
{
    char path[MAX_PATH];
    sprintf_s(path, "%s\\%s\\PlayOnlineViewer\\Settings", registry_key(region), region == 0 ? "Square" : "SquareEnix");
    int language = static_cast<int>(region);
    HKEY key = nullptr;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, path, 0, KEY_QUERY_VALUE | KEY_WOW64_32KEY, &key) == ERROR_SUCCESS) {
        DWORD value = 0, type = 0, size = sizeof(value);
        if (RegQueryValueExA(key, "Language", nullptr, &type, reinterpret_cast<BYTE*>(&value), &size) == ERROR_SUCCESS &&
            type == REG_DWORD)
            language = static_cast<int>(value);
        RegCloseKey(key);
    }
    return language;
}

// PlayOnline's folder, as its InstallFolder key holds it ([client] pol=, through VanaCore's registry)
bool install_folder(std::uint32_t region)
{
    char path[MAX_PATH];
    sprintf_s(path, "%s\\InstallFolder", registry_key(region));
    HKEY key = nullptr;
    bool found = false;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, path, 0, KEY_QUERY_VALUE | KEY_WOW64_32KEY, &key) == ERROR_SUCCESS) {
        DWORD type = 0, size = sizeof(g_install_folder) - 1;
        found = RegQueryValueExA(key, "1000", nullptr, &type, reinterpret_cast<BYTE*>(g_install_folder), &size) ==
                    ERROR_SUCCESS &&
                type == REG_SZ && size > 1;
        RegCloseKey(key);
    }
    return found;
}

// Writes `value` over the 16 bits at `at`, in code.
bool patch16(std::uintptr_t at, std::uint16_t value)
{
    DWORD protect = 0;
    if (!VirtualProtect(reinterpret_cast<void*>(at), sizeof(value), PAGE_EXECUTE_READWRITE, &protect))
        return false;
    std::memcpy(reinterpret_cast<void*>(at), &value, sizeof(value));
    VirtualProtect(reinterpret_cast<void*>(at), sizeof(value), protect, &protect);
    FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(at), sizeof(value));
    return true;
}

// polcore's own setup, by its code: false (reported) if this polcore isn't one VanaCore knows.
bool prepare_polcore(HMODULE polcore, Command* table, std::uint16_t profile_port)
{
    // Its connection lock, made once: the function called just before this code
    const std::uintptr_t mutex_site = scan::find(polcore, "8B 56 2C 8B 46 28 8B 4E 24 52 50 51");
    // Its connection object: cleared, with a buffer of its own for what it encrypts
    const std::uintptr_t connection_site = scan::find(polcore, "81 C6 38 03 00 00 83 C4 04 81 FE");
    // The profile server's port (51220) in its two places: this machine's stand-in instead
    const std::uintptr_t port_site = scan::find(polcore, "66 C7 46 26 14 C8 88 46 09 8D 46 24");
    const std::uintptr_t port_site2 = scan::find(polcore, "66 C7 05 BA 4A ?? ?? 14 C8");
    if (mutex_site == 0 || connection_site == 0 || port_site == 0 || port_site2 == 0) {
        report(L"this PlayOnline (polcore) isn't one VanaCore knows (not found:%ls%ls%ls%ls)",
               mutex_site == 0 ? L" lock" : L"", connection_site == 0 ? L" connection" : L"",
               port_site == 0 ? L" profile port" : L"", port_site2 == 0 ? L" profile port 2" : L"");
        return false;
    }

    std::int32_t offset = 0;
    std::memcpy(&offset, reinterpret_cast<const void*>(mutex_site - 4), sizeof(offset));
    reinterpret_cast<Command>(mutex_site + offset)();

    char* connection = nullptr;
    std::memcpy(&connection, reinterpret_cast<const void*>(connection_site - 10), sizeof(connection));
    std::memset(connection, 0, 0x68);
    char* const buffer = g_connection_buffer;
    std::memcpy(connection + 0x48, &buffer, sizeof(buffer));

    char* slots = nullptr;
    std::memcpy(&slots, reinterpret_cast<const char*>(table[kCharacterSlots]) + 31, sizeof(slots));
    g_character_slots = slots;

    const std::uint32_t region = g_login.language;
    table[kSetRegion](static_cast<int>(region));
    table[kSetGameLanguage](game_language(region));
    table[kSetRegistryKey](registry_key(region));
    if (!install_folder(region))
        log::write(L"Game start: no PlayOnline folder in the registry (%hs\\InstallFolder)", registry_key(region));
    table[kSetInstallFolder](g_install_folder);
    // polcore's own text for a taken character name is Japanese (the viewer would load the English
    // from sqpolcts.bin): the English one, as xiloader 2.1.2 sets it
    if (region != 0)
        table[kSetMessageText](kNameUnavailable, "The name you entered is unavailable.\nPlease choose another name.");
    table[kInetMutex]();

    if (!patch16(port_site + 4, profile_port) || !patch16(port_site2 + 7, profile_port)) {
        report(L"couldn't point PlayOnline at the profile server (error %lu)", GetLastError());
        return false;
    }
    return true;
}

} // namespace

int run_game(const BootInfo& boot)
{
    g_login = boot.login;
    g_server_host = boot.server;
    g_view_relay_port = 0;
    if (!md5(g_login.session_hash, sizeof(g_login.session_hash), g_session_value)) {
        std::memset(g_session_value, 0, sizeof(g_session_value));
        log::write(L"Game start: Windows couldn't work out the session value (MD5): zeros, which servers since "
                   L"LandSandBoat #11736 don't take (zoning stops at \"Downloading data\")");
    }
    in_addr server{};
    server.s_addr = g_login.server_address;
    InetNtopA(AF_INET, &server, g_server, sizeof(g_server));
    // The account's id isn't logged: a log is what a player sends for support
    log::write(L"Game start: on %hs (lobby %u and %u), PlayOnline region %u", g_server, g_login.view_port,
               g_login.data_port, g_login.language);

    WSADATA wsa{};
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        report(L"Windows' networking (winsock) wouldn't start");
        return kExitGameStartFailed;
    }
    const HRESULT com = CoInitialize(nullptr);
    if (FAILED(com)) {
        report(L"COM wouldn't start (0x%08lX)", static_cast<unsigned long>(com));
        return kExitGameStartFailed;
    }
    if (!hook_winsock()) {
        CoUninitialize();
        WSACleanup();
        return kExitGameStartFailed;
    }

    int result = kExitGameStartFailed;
    HANDLE data_thread = nullptr, profile_thread = nullptr, view_relay_thread = nullptr;
    void* polcore = nullptr;
    void* game = nullptr;
    do {
        // The lobby's data link, optional TLS 1.3 view relay (xiloader v3.0+), and the profile server's stand-in
        if (g_login.lobby_tls != 0) {
            std::wstring tls_error;
            const std::uint8_t* expected_key = g_login.has_server_key != 0 ? g_login.server_key_sha256 : nullptr;
            if (!g_data_tls.connect(g_login.server_address, g_login.data_port, g_server_host, tls_error, expected_key,
                                    15000, g_connect)) {
                report(L"couldn't connect to the lobby at %hs:%u over TLS: %ls", g_server, g_login.data_port,
                       tls_error.c_str());
                break;
            }
            g_view_relay_port = listen_locally(g_view_relay);
            if (g_view_relay_port == 0) {
                report(L"couldn't open the local TLS 1.3 lobby view relay (winsock error %d)", WSAGetLastError());
                break;
            }
            view_relay_thread = CreateThread(nullptr, 0, &view_relay_server, nullptr, 0, nullptr);
            log::write(L"Game start: lobby data link open (%ls); view TLS relay on port %u",
                       vanacore::tls::protocol_name(g_data_tls.negotiated_protocol()).c_str(), g_view_relay_port);
        } else {
            g_data = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
            sockaddr_in to{};
            to.sin_family = AF_INET;
            to.sin_port = htons(g_login.data_port);
            to.sin_addr = server;
            if (g_data == INVALID_SOCKET ||
                g_connect(g_data, reinterpret_cast<const sockaddr*>(&to), sizeof(to)) == SOCKET_ERROR) {
                report(L"couldn't connect to the lobby at %hs:%u (winsock error %d)", g_server, g_login.data_port,
                       WSAGetLastError());
                break;
            }
        }
        const std::uint16_t profile_port = listen_locally(g_profile);
        if (profile_port == 0) {
            report(L"couldn't open PlayOnline's profile server on this machine (winsock error %d)", WSAGetLastError());
            break;
        }
        data_thread = CreateThread(nullptr, 0, &data_link, nullptr, 0, nullptr);
        profile_thread = CreateThread(nullptr, 0, &profile_server, nullptr, 0, nullptr);
        log::write(L"Game start: lobby data link open; profile server on port %u", profile_port);

        // PlayOnline
        const std::uint32_t region = g_login.language < 3 ? g_login.language : 1;
        HRESULT created = CoCreateInstance(kPolCoreClass[region], nullptr, CLSCTX_ALL, kPolCoreInterface[region], &polcore);
        if (FAILED(created) || polcore == nullptr) {
            report(L"couldn't start PlayOnline (polcore, 0x%08lX). Check [client] pol= in vanacore.ini",
                   static_cast<unsigned long>(created));
            break;
        }
        call(polcore, kSetAreaCode, static_cast<long>(region));
        // No arguments, as the xiloader 2.0.1 CatsEyeXI runs gives it in effect: it means
        // " /game eAZcFcB -net 3 -port 54001", but a bug hands polcore stray text instead (2.1 fixes that)
        call(polcore, kSetParamInit, GetModuleHandleW(nullptr), static_cast<char*>(g_command_line));
        Command* table = nullptr;
        if (FAILED(call(polcore, kGetCommonFunctionTable, reinterpret_cast<void***>(&table))) || table == nullptr) {
            report(L"PlayOnline (polcore) gave no function table");
            break;
        }
        const HMODULE polcore_module = GetModuleHandleW(region == 2 ? L"polcoreeu.dll" : L"polcore.dll");
        if (polcore_module == nullptr) {
            report(L"PlayOnline's polcore isn't loaded");
            break;
        }
        if (!prepare_polcore(polcore_module, table, profile_port))
            break;
        log::write(L"Game start: PlayOnline ready");

        // FFXI, in this process: GameStart returns when the game closes
        created = CoCreateInstance(kGameClass, nullptr, CLSCTX_ALL, kGameInterface, &game);
        if (FAILED(created) || game == nullptr) {
            report(L"couldn't start FFXI (FFXiEntry, 0x%08lX). Check [client] ffxi= in vanacore.ini",
                   static_cast<unsigned long>(created));
            break;
        }
        log::write(L"Game start: starting FFXI");
        DWORD protect = 0;
        if (VirtualProtect(&table[kShowError], sizeof(Command), PAGE_EXECUTE_READWRITE, &protect)) {
            g_show_error = table[kShowError];
            table[kShowError] = reinterpret_cast<Command>(&show_error);
            VirtualProtect(&table[kShowError], sizeof(Command), protect, &protect);
        }
        if (VirtualProtect(&table[kSessionValue], sizeof(Command), PAGE_EXECUTE_READWRITE, &protect)) {
            table[kSessionValue] = reinterpret_cast<Command>(&session_value);
            VirtualProtect(&table[kSessionValue], sizeof(Command), protect, &protect);
        } else {
            // polcore's zeros then, and the same in the 0xA2 key (asked for once the lobby starts): servers before
            // LandSandBoat #11736 still take them
            std::memset(g_session_value, 0, sizeof(g_session_value));
            log::write(L"Game start: couldn't give FFXI the session value (error %lu): zeros, which servers since "
                       L"LandSandBoat #11736 don't take (zoning stops at \"Downloading data\")",
                       GetLastError());
        }
        const ULONGLONG started = GetTickCount64();
        IUnknown* message = nullptr;
        const HRESULT ended = call(game, kGameStart, static_cast<IUnknown*>(polcore), &message);
        if (message != nullptr)
            message->Release();
        log::write(L"Game start: FFXI has closed (0x%08lX, after %llu ms)", static_cast<unsigned long>(ended),
                   GetTickCount64() - started);
        shutdown::run(L"FFXI has closed");
        result = 0;
    } while (false);

    if (game != nullptr)
        static_cast<IUnknown*>(game)->Release();
    if (polcore != nullptr)
        static_cast<IUnknown*>(polcore)->Release();
    // Closing the sockets ends all lobby/profile threads
    g_data_tls.shutdown_socket();
    if (g_data != INVALID_SOCKET) {
        closesocket(g_data);
        g_data = INVALID_SOCKET;
    }
    if (g_view_relay != INVALID_SOCKET) {
        closesocket(g_view_relay);
        g_view_relay = INVALID_SOCKET;
    }
    g_view_relay_port = 0;
    if (g_profile != INVALID_SOCKET) {
        closesocket(g_profile);
        g_profile = INVALID_SOCKET;
    }
    for (HANDLE thread : {data_thread, profile_thread, view_relay_thread})
        if (thread != nullptr) {
            WaitForSingleObject(thread, 1000);
            CloseHandle(thread);
        }
    g_data_tls.close();
    CoUninitialize();
    WSACleanup();
    return result;
}

} // namespace vanacore::loader
