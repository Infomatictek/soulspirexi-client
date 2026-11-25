#pragma once

#include <cstdint>

static_assert(sizeof(void*) == 4, "VanaCore must be built for Win32: FFXI is a 32-bit process");

namespace vanacore {

// What VanaCore.exe's own login (login.cpp) hands the game process, which starts the game with it
// (the core's loader.cpp): VanaCore's stand-in for a bootloader such as xiloader.
struct GameLogin {
    std::uint32_t account_id;
    std::uint8_t session_hash[16];     // from the login server; the lobby's packets carry it
    std::uint32_t server_address;      // the server's IPv4 address, in network byte order
    std::uint32_t language;            // PlayOnline's region: 0 Japanese (JP), 1 English (US), 2 European (EU)
    std::uint16_t data_port;           // the lobby's data port (54230)
    std::uint16_t view_port;           // the lobby's view port (54001)
    std::uint32_t lobby_tls;           // 1: LandSandBoat xiloader v3.0+ TLS 1.3 on lobby data (54230) and view (54001)
    std::uint32_t has_server_key;      // 1: server_key_sha256 holds the authenticated login server's public key SHA-256
    std::uint8_t server_key_sha256[32]; // fail-closed key pin shared between auth (54231) and lobby (54230/54001)
};

// Passed from VanaCore.exe to VanaCore.Core.dll's VanaCoreInstall export. The launcher writes it
// into the game process; the core copies it before returning.
struct BootInfo {
    std::uint32_t size;       // sizeof(BootInfo), so either side can detect a mismatched build
    std::uint32_t version;    // kBootInfoVersion
    wchar_t install_dir[260]; // folder holding VanaCore.exe, VanaCore.Core.dll, config and logs
    wchar_t settings_ini[260]; // absolute path of vanacore.ini ([client] pol=/ffxi=, [ffxi], [ffxi.registry])
    std::uint32_t attached;   // [client] attached=1: VanaCore.exe stays with the game; mirror the log to the console
    std::uint32_t logged_in;  // 1: VanaCore logged in itself; the game process calls VanaCoreRunGame with `login`
    std::uint32_t retail;     // 1: [client] retail=1 or --retail (client.hpp): PlayOnline as installed
    GameLogin login;
    wchar_t server[256];      // the server's host (--server, else [server] server=); empty for retail
};

inline constexpr std::uint32_t kBootInfoVersion = 7;
inline constexpr char kInstallExport[] = "VanaCoreInstall";

// VanaCore.Core.dll's export that starts the game after VanaCore's own login (BootInfo::logged_in):
// int __stdcall VanaCoreRunGame(). It returns once the game has closed, with the process's exit code.
inline constexpr char kRunGameExport[] = "VanaCoreRunGame";

// VanaCore.exe's first argument when it's the game process of its own login (it runs the game
// through VanaCoreRunGame instead of launching anything).
inline constexpr wchar_t kGameHostArgument[] = L"--vanacore-game";

// Named event the core signals once the game is up (its Direct3D device exists), so VanaCore.exe
// can detach (attached=0). Formatted with the game's process ID.
inline constexpr wchar_t kGameStartedEvent[] = L"Local\\VanaCore.GameStarted.%lu";

// VanaCore.exe's exit codes for a game that never started, for the CatsEyeXI launcher.
inline constexpr int kExitLoginRefused = 3;     // wrong username or password, banned, already logged in
inline constexpr int kExitServerProblem = 4;    // the login server couldn't be reached, or its reply made no sense
inline constexpr int kExitGameStartFailed = 5;  // logged in, but PlayOnline or FFXI couldn't be started

// The game's exit code when VanaCore closed it because the graphics card was lost (a driver timeout): nothing it drew
// could show again (graphics::close_if_lost).
inline constexpr int kExitGraphicsCardLost = 6;

} // namespace vanacore
