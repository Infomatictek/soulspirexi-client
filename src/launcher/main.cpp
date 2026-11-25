#include <windows.h>

#include <conio.h>
#include <shellapi.h>

#include <vanacore/boot_info.hpp>
#include <vanacore/console.hpp>
#include <vanacore/log_file.hpp>
#include <vanacore/version.hpp>

#include <climits>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <initializer_list>
#include <iterator>
#include <string>

#include "inject.hpp"
#include "login.hpp"
#include "playonline.hpp"
#include "settings.hpp"

namespace {

// The login server's ports: TLS login, then the lobby's data and view (the game's own).
constexpr std::uint16_t kAuthPort = 54231;
constexpr std::uint16_t kDataPort = 54230;
constexpr std::uint16_t kViewPort = 54001;

std::wstring exe_path()
{
    wchar_t path[MAX_PATH];
    const DWORD length = GetModuleFileNameW(nullptr, path, MAX_PATH);
    return std::wstring(path, length);
}

std::wstring exe_dir()
{
    const std::wstring full = exe_path();
    return full.substr(0, full.find_last_of(L"\\/"));
}

// This process's command line without its first word (VanaCore.exe's own name), as it was typed.
std::wstring arguments_text()
{
    const wchar_t* at = GetCommandLineW();
    if (*at == L'"') {
        for (++at; *at != L'\0' && *at != L'"'; ++at) {
        }
        if (*at == L'"')
            ++at;
    } else {
        while (*at != L'\0' && *at != L' ' && *at != L'\t')
            ++at;
    }
    while (*at == L' ' || *at == L'\t')
        ++at;
    return at;
}

bool is_elevated()
{
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
        return false;
    TOKEN_ELEVATION elevation{};
    DWORD size = 0;
    const bool known = GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &size) != FALSE;
    CloseHandle(token);
    return known && elevation.TokenIsElevated != 0;
}

// Quotes one argument so CommandLineToArgvW (and the CRT) parse it back unchanged.
std::wstring quote(const std::wstring& arg)
{
    if (!arg.empty() && arg.find_first_of(L" \t\"") == std::wstring::npos)
        return arg;
    std::wstring out = L"\"";
    for (auto it = arg.begin();; ++it) {
        size_t backslashes = 0;
        while (it != arg.end() && *it == L'\\') {
            ++it;
            ++backslashes;
        }
        if (it == arg.end()) {
            out.append(backslashes * 2, L'\\');
            break;
        }
        if (*it == L'"') {
            out.append(backslashes * 2 + 1, L'\\');
            out += L'"';
        } else {
            out.append(backslashes, L'\\');
            out += *it;
        }
    }
    out += L'"';
    return out;
}

constexpr WORD kMagenta = vanacore::kConsoleMagenta;
constexpr WORD kRed = vanacore::kConsoleRed;
constexpr WORD kGrey = vanacore::kConsoleGrey;

vanacore::LogFile g_log; // logs\vanacore.log, shared with VanaCore.Core in the game
bool g_detached = false; // attached=0 and the game is up: VanaCore left it running
bool g_refused = false;  // the login failed and a message box said so: nothing to wait for
bool g_non_interactive = false; // --non-interactive or [client] non_interactive=1: no MessageBox/prompts, structured status
std::wstring g_status_file;     // --status-file <path>: atomic JSON status/readiness output (zero secrets)
std::wstring g_logs_dir;        // <exe_dir>\logs: reported for crash evidence collection
bool g_mcp_enabled = false;
int g_mcp_port = 0;

std::string utf8_from_wide(const std::wstring& text)
{
    if (text.empty())
        return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    if (n <= 0)
        return {};
    std::string out(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), out.data(), n, nullptr, nullptr);
    return out;
}

std::string json_escape(const std::string& input)
{
    std::string out;
    out.reserve(input.size() + 8);
    out.push_back('"');
    for (unsigned char c : input) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\b': out += "\\b"; break;
        case '\f': out += "\\f"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (c < 0x20) {
                char buf[8];
                std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned>(c));
                out += buf;
            } else {
                out.push_back(static_cast<char>(c));
            }
            break;
        }
    }
    out.push_back('"');
    return out;
}

void emit_status(const char* stage, bool ok, int exit_code, const char* error_category,
                 const std::wstring& message, DWORD game_pid = 0)
{
    if (!g_non_interactive && g_status_file.empty())
        return;
    const std::string log_path = g_logs_dir.empty() ? std::string() : utf8_from_wide(g_logs_dir + L"\\vanacore.log");
    const std::string crash_dir = utf8_from_wide(g_logs_dir);
    std::string payload = "{";
    payload += "\"schema_version\":\"vanacore-launcher-status-1.0.0\",";
    payload += "\"stage\":" + json_escape(stage != nullptr ? stage : "unknown") + ",";
    payload += "\"ok\":" + std::string(ok ? "true" : "false") + ",";
    payload += "\"exit_code\":" + std::to_string(exit_code) + ",";
    payload += "\"error_category\":" + json_escape(error_category != nullptr ? error_category : "") + ",";
    payload += "\"message\":" + json_escape(utf8_from_wide(message)) + ",";
    payload += "\"game_pid\":" + std::to_string(game_pid) + ",";
    payload += "\"log_path\":" + json_escape(log_path) + ",";
    payload += "\"crash_dumps_dir\":" + json_escape(crash_dir) + ",";
    payload += "\"mcp_enabled\":" + std::string(g_mcp_enabled ? "true" : "false") + ",";
    payload += "\"mcp_port\":" + std::to_string(g_mcp_port);
    payload += "}";

    if (g_non_interactive) {
        std::fprintf(stderr, "VANACORE_STATUS: %s\n", payload.c_str());
        std::fflush(stderr);
    }
    if (!g_status_file.empty()) {
        const std::wstring tmp_path = g_status_file + L".tmp";
        HANDLE fh = CreateFileW(tmp_path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (fh != INVALID_HANDLE_VALUE) {
            DWORD written = 0;
            WriteFile(fh, payload.data(), static_cast<DWORD>(payload.size()), &written, nullptr);
            CloseHandle(fh);
            MoveFileExW(tmp_path.c_str(), g_status_file.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
        }
    }
}

// A line in vanacore.log only, marked as VanaCore.exe's.
void note(const std::wstring& message)
{
    g_log.write(L"[VanaCore.exe] " + message);
}

// One line in xiloader's style on the console only: the bootloader shares this console, so VanaCore's lines sit in
// with its output.
void show(FILE* stream, const std::wstring& message, WORD color)
{
    vanacore::write_console_line(GetStdHandle(stream == stderr ? STD_ERROR_HANDLE : STD_OUTPUT_HANDLE), message,
                                 color);
}

// The same, and logged too.
void say(FILE* stream, const std::wstring& message, WORD color)
{
    show(stream, message, color);
    note(message);
}

LONG WINAPI on_crash(EXCEPTION_POINTERS* info)
{
    const EXCEPTION_RECORD& record = *info->ExceptionRecord;
    const wchar_t* name = vanacore::describe_status(record.ExceptionCode);
    note(L"CRASH: VanaCore.exe crashed: " + std::wstring(name != nullptr ? name : L"unhandled exception") + L" (0x" +
         [&] { wchar_t hex[16]; swprintf_s(hex, L"%08lX", record.ExceptionCode); return std::wstring(hex); }() +
         L") at " + vanacore::describe_address(record.ExceptionAddress));
    return EXCEPTION_CONTINUE_SEARCH;
}

// "0 (normal)", "0xC0000005 (access violation)".
std::wstring describe_exit(DWORD code)
{
    wchar_t text[64];
    if (code < 0x10000)
        swprintf_s(text, L"%lu", code);
    else
        swprintf_s(text, L"0x%08lX", code);
    const wchar_t* name = vanacore::describe_status(code);
    return name != nullptr ? std::wstring(text) + L" (" + name + L")" : text;
}

int fail(const std::wstring& message, int code = 1, const char* category = "generic_error")
{
    say(stderr, L"VanaCore: " + message, kRed);
    emit_status("failed", false, code, category, message);
    return code;
}

// The login failed: said on the console and (in interactive player mode) in a message box.
int refuse(const std::wstring& message, int code, const char* category = "login_failed")
{
    say(stderr, L"SoulspireXI Client: couldn't log in: " + message, kRed);
    g_refused = true;
    emit_status("failed", false, code, category, message);
    if (!g_non_interactive) {
        const std::wstring text = message + L"\n\nPlease verify your credentials and try again.";
        MessageBoxW(nullptr, text.c_str(), L"SoulspireXI Client", MB_OK | MB_ICONWARNING | MB_SETFOREGROUND);
    }
    return code;
}

// VanaCore.exe again with administrator rights, which Windows asks the user for (a window of its own: an elevated process
// can't share this console), and its exit code. PlayOnline's pol.exe asks for them in its manifest, as the retail game has
// always run; Windower's launcher runs it as the invoking user instead and stands in for what it then can't write.
int run_elevated()
{
    say(stdout, L"PlayOnline asks for administrator rights: Windows will ask for them for VanaCore.", kGrey);
    const std::wstring path = exe_path();
    const std::wstring directory = exe_dir();
    const std::wstring arguments = arguments_text();
    SHELLEXECUTEINFOW info{};
    info.cbSize = sizeof(info);
    info.fMask = SEE_MASK_NOCLOSEPROCESS;
    info.lpVerb = L"runas";
    info.lpFile = path.c_str();
    info.lpParameters = arguments.c_str();
    info.lpDirectory = directory.c_str();
    info.nShow = SW_SHOWNORMAL;
    if (!ShellExecuteExW(&info) || info.hProcess == nullptr) {
        const DWORD error = GetLastError();
        return fail(error == ERROR_CANCELLED ? L"administrator rights were refused, so PlayOnline can't start."
                                             : L"couldn't ask Windows for administrator rights (error " +
                                                   std::to_wstring(error) + L").");
    }
    WaitForSingleObject(info.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(info.hProcess, &code);
    CloseHandle(info.hProcess);
    g_detached = true; // the elevated copy's window is the one that waits for a key
    note(L"the elevated VanaCore.exe exited with code " + std::to_wstring(code));
    return static_cast<int>(code);
}

// PlayOnline's region from --lang: JP, US or EU, or 0, 1 or 2. US otherwise.
std::uint32_t region(const std::wstring& text)
{
    if (_wcsicmp(text.c_str(), L"JP") == 0 || text == L"0")
        return 0;
    if (_wcsicmp(text.c_str(), L"EU") == 0 || text == L"2")
        return 2;
    if (_wcsicmp(text.c_str(), L"US") != 0 && text != L"1")
        note(L"--lang " + text + L" isn't JP, US or EU (0, 1 or 2): US");
    return 1;
}

// --ini <file>: the settings file to use in place of vanacore.ini, from VanaCore's folder unless it's a full path (the
// launcher passes the account's: vanacore.<hash of the username>.ini). VanaCore.exe and the game read it alone, in whichever
// way the game is started, and it never goes on to a bootloader or a program. "" when it isn't given.
std::wstring ini_argument(int argc, wchar_t** argv, const std::wstring& dir)
{
    std::wstring name;
    for (int i = 1; i + 1 < argc; ++i)
        if (_wcsicmp(argv[i], L"--ini") == 0)
            name = argv[++i];
    if (name.empty())
        return name;
    const bool full = (name.size() > 1 && name[1] == L':') || name.starts_with(L"\\") || name.starts_with(L"//");
    return full ? name : dir + L"\\" + name;
}

bool is_ini_argument(const wchar_t* argument)
{
    return _wcsicmp(argument, L"--ini") == 0;
}

// --retail: [client] retail=1 for this start
bool has_retail_argument(int argc, wchar_t** argv)
{
    for (int i = 1; i < argc; ++i)
        if (_wcsicmp(argv[i], L"--retail") == 0)
            return true;
    return false;
}

bool has_non_interactive_argument(int argc, wchar_t** argv)
{
    for (int i = 1; i < argc; ++i)
        if (_wcsicmp(argv[i], L"--non-interactive") == 0)
            return true;
    return false;
}

std::wstring status_file_argument(int argc, wchar_t** argv, const std::wstring& dir)
{
    std::wstring name;
    for (int i = 1; i + 1 < argc; ++i)
        if (_wcsicmp(argv[i], L"--status-file") == 0)
            name = argv[++i];
    if (name.empty())
        return name;
    const bool full = (name.size() > 1 && name[1] == L':') || name.starts_with(L"\\") || name.starts_with(L"//");
    return full ? name : dir + L"\\" + name;
}

// The server's host, for the core (BootInfo::server): --server's, else [server] server=; none for PlayOnline as installed
std::wstring server_host(int argc, wchar_t** argv, const vanacore::Settings& settings)
{
    if (settings.retail)
        return {};
    std::wstring server = settings.server;
    for (int i = 1; i + 1 < argc; ++i)
        if (_wcsicmp(argv[i], L"--server") == 0)
            server = argv[++i];
    return server;
}

// Reads a single UTF-8 password line from stdin pipe (--password-stdin), trimming trailing CR/LF and scrubbing raw bytes.
bool read_password_stdin(std::wstring& out)
{
    const HANDLE input = GetStdHandle(STD_INPUT_HANDLE);
    if (input == nullptr || input == INVALID_HANDLE_VALUE)
        return false;
    std::string raw;
    char ch = '\0';
    DWORD got = 0;
    constexpr size_t kMaxBytes = 512;
    while (raw.size() < kMaxBytes && ReadFile(input, &ch, 1, &got, nullptr) != FALSE && got == 1) {
        if (ch == '\n')
            break;
        raw.push_back(ch);
    }
    while (!raw.empty() && (raw.back() == '\r' || raw.back() == '\n'))
        raw.pop_back();
    bool ok = false;
    if (!raw.empty()) {
        const int needed = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, raw.data(), static_cast<int>(raw.size()),
                                               nullptr, 0);
        if (needed > 0) {
            out.assign(static_cast<size_t>(needed), L'\0');
            MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, raw.data(), static_cast<int>(raw.size()), out.data(),
                                needed);
            ok = true;
        }
    }
    if (!raw.empty())
        SecureZeroMemory(raw.data(), raw.size());
    return ok;
}

// Prompts interactively on an attached Windows console; when `hidden` is true, disables console echo so the password is
// never printed or logged.
bool prompt_console_line(const wchar_t* prompt, bool hidden, std::wstring& out)
{
    const HANDLE input = GetStdHandle(STD_INPUT_HANDLE);
    const HANDLE output = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD mode = 0;
    if (input == nullptr || input == INVALID_HANDLE_VALUE || !GetConsoleMode(input, &mode))
        return false;
    if (output != nullptr && output != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        WriteConsoleW(output, prompt, static_cast<DWORD>(std::wcslen(prompt)), &written, nullptr);
    }
    if (hidden)
        SetConsoleMode(input, (mode | ENABLE_LINE_INPUT) & ~static_cast<DWORD>(ENABLE_ECHO_INPUT));
    wchar_t buffer[256] = {};
    DWORD read = 0;
    const BOOL ok = ReadConsoleW(input, buffer, static_cast<DWORD>(std::size(buffer) - 1), &read, nullptr);
    if (hidden) {
        SetConsoleMode(input, mode);
        if (output != nullptr && output != INVALID_HANDLE_VALUE) {
            DWORD written = 0;
            WriteConsoleW(output, L"\r\n", 2, &written, nullptr);
        }
    }
    if (ok == FALSE) {
        SecureZeroMemory(buffer, sizeof(buffer));
        return false;
    }
    while (read > 0 && (buffer[read - 1] == L'\r' || buffer[read - 1] == L'\n'))
        --read;
    out.assign(buffer, read);
    SecureZeroMemory(buffer, sizeof(buffer));
    return !out.empty();
}

// SoulspireXI secure login ([client] bootloader unset): --server, --user (--username), --password-stdin and --lang from
// the command line, else vanacore.ini's [server] (server/user only; plaintext passwords in INI or CLI are refused).
// 0 once logged in, with what the game process needs in `login`; otherwise the exit code, having said why.
int log_in(int argc, wchar_t** argv, const vanacore::Settings& settings, vanacore::GameLogin& login)
{
    if (settings.plaintext_password_in_ini)
        return fail(L"plaintext [server] password= in vanacore.ini is disabled for credential safety. Remove password= "
                    L"from vanacore.ini and use --password-stdin or the hidden interactive prompt.");

    std::wstring server = settings.server, user = settings.user, password;
    std::uint32_t language = 1;
    bool password_stdin = false;
    for (int i = 1; i < argc; ++i) {
        const auto is = [&](std::initializer_list<const wchar_t*> names) {
            for (const wchar_t* name : names)
                if (_wcsicmp(argv[i], name) == 0)
                    return i + 1 < argc;
            return false;
        };
        if (_wcsicmp(argv[i], L"--password") == 0 || _wcsicmp(argv[i], L"--pass") == 0)
            return fail(L"plaintext --password / --pass command-line arguments are disabled for credential safety. "
                        L"Use --password-stdin (piped stdin) or omit the password for a hidden interactive prompt.");
        if (_wcsicmp(argv[i], L"--password-stdin") == 0)
            password_stdin = true;
        else if (is({L"--server"}))
            server = argv[++i];
        else if (is({L"--user", L"--username"}))
            user = argv[++i];
        else if (is({L"--lang"}))
            language = region(argv[++i]);
        else if (is({L"--ini", L"--status-file"}))
            ++i; // launcher-only arguments
        else if (_wcsicmp(argv[i], L"--non-interactive") == 0)
            continue;
        else if (std::wstring(argv[i]).starts_with(L"--"))
            note(L"ignoring " + std::wstring(argv[i]) + L": SoulspireXI login doesn't use it");
    }
    if (user.empty() && !g_non_interactive)
        prompt_console_line(L"SoulspireXI Username: ", false, user);
    if (password_stdin) {
        if (!read_password_stdin(password))
            return fail(L"--password-stdin was specified, but no password could be read from stdin.",
                        g_non_interactive ? vanacore::kExitMissingSettings : 1, "missing_credentials");
    } else if (password.empty() && !g_non_interactive) {
        prompt_console_line(L"SoulspireXI Password: ", true, password);
    }
    if (user.empty() || password.empty()) {
        if (!password.empty())
            SecureZeroMemory(password.data(), password.size() * sizeof(wchar_t));
        return fail(L"no username or password to log in with. Pass --user <name> with --password-stdin, or run "
                    L"interactively in a console window to enter credentials safely.",
                    g_non_interactive ? vanacore::kExitMissingSettings : 1, "missing_credentials");
    }

    emit_status("authenticating", true, 0, "", L"Logging in to " + server);
    std::wstring error;
    std::uint32_t address = 0;
    if (!vanacore::login::resolve(server, address, error))
        return refuse(error, vanacore::kExitServerProblem, "dns_failed");
    // [server] loginversion: the version the login reports, for a server that wants another than VanaCore's
    vanacore::login::Version version = vanacore::login::kVersion;
    if (!settings.login_version.empty()) {
        if (vanacore::login::parse_version(settings.login_version, version))
            say(stdout, L"Reporting login version " + settings.login_version + L" ([server] loginversion).", kGrey);
        else
            say(stderr, L"VanaCore: [server] loginversion=" + settings.login_version +
                            L" isn't a version (2.0.1); reporting VanaCore's own.", kRed);
    }
    // The account isn't named: players send screenshots of this window, and logs, for support
    show(stdout, L"Logging in to " + server + L"...", kGrey);
    const vanacore::login::Result result =
        vanacore::login::log_in(server, address, kAuthPort, user, password, version);
    SecureZeroMemory(password.data(), password.size() * sizeof(wchar_t));
    if (result.outcome != vanacore::login::Outcome::LoggedIn) {
        const bool is_refused = result.outcome == vanacore::login::Outcome::Refused;
        return refuse(result.message,
                      is_refused ? vanacore::kExitLoginRefused : vanacore::kExitServerProblem,
                      is_refused ? "authentication_refused" : "network_or_tls_error");
    }
    say(stdout,
        result.tls_protocol.empty() ? L"Logged in." : (L"Logged in (" + result.tls_protocol + L")."),
        kGrey);

    login.account_id = result.account_id;
    std::memcpy(login.session_hash, result.session_hash, sizeof(login.session_hash));
    login.server_address = address;
    login.language = language;
    login.data_port = kDataPort;
    login.view_port = kViewPort;
    login.lobby_tls = version[0] >= 3 ? 1u : 0u;
    login.has_server_key = result.has_server_key ? 1u : 0u;
    if (result.has_server_key)
        std::memcpy(login.server_key_sha256, result.server_key_sha256, sizeof(login.server_key_sha256));
    return 0;
}

// VanaCore.exe as the game process of its own login: VanaCore.Core, loaded before this ran, starts
// the game here and returns when it closes.
int run_game()
{
    using RunGame = int(WINAPI*)();
    const HMODULE core = GetModuleHandleW(L"VanaCore.Core.dll");
    const auto run = core != nullptr ? reinterpret_cast<RunGame>(GetProcAddress(core, vanacore::kRunGameExport)) : nullptr;
    if (run == nullptr) {
        vanacore::write_console_line(GetStdHandle(STD_ERROR_HANDLE),
                                     L"VanaCore: this is VanaCore's game process. Start VanaCore.exe on its own.", kRed);
        return vanacore::kExitGameStartFailed;
    }
    return run();
}

// True when the console window closes with VanaCore: it's visible and nothing else (such as a
// command prompt) is attached, i.e. VanaCore was double-clicked or started from a shortcut.
bool owns_console_window()
{
    DWORD processes[2];
    DWORD mode = 0;
    return GetConsoleWindow() != nullptr && GetConsoleProcessList(processes, 2) == 1 &&
           GetConsoleMode(GetStdHandle(STD_INPUT_HANDLE), &mode) != FALSE;
}

// What run() starts: the program, its command line, and where in VanaCore's own arguments the target's begin.
struct Target {
    std::wstring executable;
    std::wstring command_line; // may hold the bootloader's credentials: never log or print it
    int passthrough = 2;       // first argv index forwarded to the target
    bool retail = false;       // [client] retail=1: pol.exe as installed, and nothing here to wait for
};

// [client] retail=1: PlayOnline as installed, the way Windower starts it: pol.exe from the install the registry records (or
// [client] retail.pol=), with VanaCore.Core loaded before its first instruction. PlayOnline does the login, so there is no server,
// account or bootloader here, and the game finds its own folders and settings in the real registry. 0, or the exit code.
int retail_target(const vanacore::Settings& settings, const std::wstring& settings_ini, Target& target)
{
    vanacore::playonline::Install install;
    std::wstring error;
    if (!settings.retail_pol.empty()) {
        install.pol = settings.retail_pol;
        if (GetFileAttributesW((install.pol + L"\\pol.exe").c_str()) == INVALID_FILE_ATTRIBUTES)
            return fail(L"pol.exe isn't in " + install.pol + L"\nSet [client] retail.pol= in " + settings_ini +
                        L" to PlayOnline's folder, or take it out to use the registry's.");
    } else if (!vanacore::playonline::find(settings.retail_region, install, error)) {
        return fail(error);
    }
    target.executable = install.pol + L"\\pol.exe";
    target.command_line = quote(target.executable);
    if (!settings.retail_args.empty())
        target.command_line += L' ' + settings.retail_args;
    target.passthrough = INT_MAX; // VanaCore's own arguments aren't PlayOnline's
    target.retail = true;
    // [client] retail.pol= names a folder only: its region isn't known
    const std::wstring region = install.ffxi.empty() ? L"" : L" (" + std::wstring(vanacore::playonline::name(install.region)) + L")";
    const wchar_t* given = settings.retail_pol.empty() ? L"" : L" ([client] retail.pol=)";
    say(stdout, L"Starting PlayOnline from " + install.pol + region + given + L"...", kGrey);
    return 0;
}

// SoulspireXI: vanacore.ini's bootloader, pointed at our server unless the caller picks one. 0, or the exit code.
int bootloader_target(const vanacore::Settings& settings, const std::wstring& settings_ini, int argc, wchar_t** argv,
                      Target& target)
{
    if (GetFileAttributesW(settings.bootloader.c_str()) == INVALID_FILE_ATTRIBUTES)
        return fail(L"bootloader not found: " + settings.bootloader + L"\nSet [client] bootloader= in " + settings_ini +
                    L" (relative paths are from VanaCore.exe's folder).");
    target.executable = settings.bootloader;
    target.command_line = quote(settings.bootloader);
    // vanacore.ini's [server] fills in whatever the command line leaves out.
    const auto passed = [&](std::initializer_list<const wchar_t*> names) {
        for (int i = 1; i < argc; ++i)
            for (const wchar_t* name : names)
                if (_wcsicmp(argv[i], name) == 0)
                    return true;
        return false;
    };
    if (!passed({L"--server"}))
        target.command_line += L" --server " + quote(settings.server);
    if (!settings.user.empty() && !passed({L"--user", L"--username"}))
        target.command_line += L" --user " + quote(settings.user);
    target.passthrough = 1;
    return 0;
}

// Everything else goes to the target, e.g. xiloader's "--lang N".
void add_passthrough(int argc, wchar_t** argv, Target& target)
{
    for (int i = target.passthrough; i < argc; ++i) {
        if (is_ini_argument(argv[i]) || _wcsicmp(argv[i], L"--status-file") == 0) {
            ++i; // VanaCore's own, not the target's
            continue;
        }
        if (_wcsicmp(argv[i], L"--retail") != 0 && _wcsicmp(argv[i], L"--non-interactive") != 0)
            target.command_line += L' ' + quote(argv[i]);
    }
}

// --ini names a file the launcher made: one that isn't there is an error, not vanacore.ini or VanaCore's defaults. 0, or the
// exit code.
int missing_settings(int argc, wchar_t** argv, const std::wstring& dir, const std::wstring& settings_ini)
{
    if (GetFileAttributesW(settings_ini.c_str()) != INVALID_FILE_ATTRIBUTES || ini_argument(argc, argv, dir).empty())
        return 0;
    const std::wstring msg = L"the settings file " + settings_ini + L" (--ini) isn't there.";
    say(stderr, L"VanaCore: " + msg, kRed);
    g_refused = true;
    const int code = g_non_interactive ? vanacore::kExitMissingSettings : vanacore::kExitGameStartFailed;
    emit_status("failed", false, code, "missing_settings", msg);
    if (!g_non_interactive) {
        const std::wstring text = L"The settings file for this launch wasn't found:\n" + settings_ini +
                                  L"\n\nPlease check --ini or create vanacore.ini from vanacore.soulspire-minimal.ini.";
        MessageBoxW(nullptr, text.c_str(), L"SoulspireXI Client", MB_OK | MB_ICONWARNING | MB_SETFOREGROUND);
    }
    return code;
}

// The program wouldn't start: said, or for one that asks for administrator rights (retail's pol.exe) asked of Windows.
int launch_failed(std::wstring error, const std::wstring& executable)
{
    if (error == vanacore::kElevationRequired) {
        if (g_non_interactive)
            return fail(executable + L" requires administrator elevation, which is blocked in --non-interactive mode.",
                        vanacore::kExitElevationBlocked, "elevation_blocked");
        if (!is_elevated())
            return run_elevated();
        error = executable + L" asks for administrator rights, and Windows wouldn't give them to VanaCore.exe.";
    }
    return fail(error, g_non_interactive ? vanacore::kExitInjectionFailed : 1, "injection_failed");
}

// PlayOnline has its own windows from here, and the game is up much later (its login, then Play): nothing to wait for.
int leave_running(void* process)
{
    say(stdout, L"PlayOnline is starting with VanaCore. This window is done (attached=0).", kGrey);
    CloseHandle(process);
    g_detached = true;
    return 0;
}

// Everything but the help text: returns the exit code, which is the game's once it has run. `settings_ini` is the settings
// file (--ini's, else vanacore.ini).
int run(int argc, wchar_t** argv, const std::wstring& settings_ini)
{
    const std::wstring first = argc > 1 ? argv[1] : L"";
    const std::wstring dir = exe_dir();
    if (const int missing = missing_settings(argc, argv, dir, settings_ini); missing != 0)
        return missing;
    if (settings_ini != dir + L"\\vanacore.ini")
        note(L"settings: " + settings_ini + L" (--ini)");
    vanacore::Settings settings = vanacore::load_settings(dir, settings_ini);
    if (settings.non_interactive)
        g_non_interactive = true;
    g_mcp_enabled = GetPrivateProfileIntW(L"mcp", L"enabled", 0, settings_ini.c_str()) != 0;
    g_mcp_port = g_mcp_enabled ? static_cast<int>(GetPrivateProfileIntW(L"mcp", L"port", 47800, settings_ini.c_str())) : 0;
    if (has_retail_argument(argc, argv))
        settings.retail = true;
    emit_status("launching", true, 0, "", L"Settings loaded");
    Target target;
    vanacore::GameLogin login{};
    bool logged_in = false; // VanaCore's own login: the game process is VanaCore.exe again

    int refused = 0;
    if (!first.empty() && !first.starts_with(L"--")) {
        target.executable = first;
        target.command_line = quote(first);
    } else if (settings.retail) {
        refused = retail_target(settings, settings_ini, target);
    } else if (settings.bootloader.empty()) {
        refused = log_in(argc, argv, settings, login);
        logged_in = refused == 0;
        target.executable = exe_path();
        target.command_line = quote(target.executable) + L" " + vanacore::kGameHostArgument;
        target.passthrough = argc;
    } else {
        refused = bootloader_target(settings, settings_ini, argc, argv, target);
    }
    if (refused != 0)
        return refused;
    add_passthrough(argc, argv, target);

    // The command line itself holds the password: only the program goes in the log.
    note(L"starting " + target.executable + (settings.attached ? L" (attached=1)" : L" (attached=0)"));
    const vanacore::LaunchOptions options{target.executable, target.command_line, dir + L"\\VanaCore.Core.dll", dir,
                                          settings_ini, settings.attached, logged_in ? &login : nullptr, settings.retail,
                                          server_host(argc, argv, settings)};
    void* process = nullptr;
    std::wstring error;
    if (!vanacore::launch_and_inject(options, process, error))
        return launch_failed(error, target.executable);
    const DWORD game_pid = GetProcessId(process);
    note(L"game process " + std::to_wstring(game_pid) + L" started with VanaCore.Core");
    emit_status("injected", true, 0, "", L"VanaCore.Core injected into game process", game_pid);

    if (target.retail && !settings.attached)
        return leave_running(process);

    HANDLE started = nullptr;
    {
        wchar_t name[64];
        swprintf_s(name, vanacore::kGameStartedEvent, game_pid);
        if (!settings.attached || g_non_interactive || !g_status_file.empty())
            started = CreateEventW(nullptr, TRUE, FALSE, name);
    }
    if (settings.attached) {
        // The game joins a job that Windows ends when VanaCore does, however VanaCore closes
        // (its window, Ctrl+C, Task Manager, a crash). The job handle is never closed by hand.
        HANDLE job = CreateJobObjectW(nullptr, nullptr);
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (job != nullptr &&
            SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits)) &&
            AssignProcessToJobObject(job, process))
            say(stdout, L"Attached: this window shows VanaCore's log, and closing it closes the game.", kGrey);
        else
            say(stderr, L"VanaCore: couldn't attach to the game (error " + std::to_wstring(GetLastError()) +
                            L"); closing VanaCore won't close it.", kRed);
    }

    // The bootloader shares this console, and the window has to outlive its last lines
    // (xiloader prints its errors, then "Closing...").
    bool ready_reported = false;
    for (;;) {
        const HANDLE waits[2] = {process, started};
        const DWORD count = (started != nullptr && !ready_reported) ? 2 : 1;
        const DWORD woke = WaitForMultipleObjects(count, waits, FALSE, INFINITE);
        if (woke == WAIT_OBJECT_0 + 1) {
            ready_reported = true;
            emit_status("ready", true, 0, "", L"Game Direct3D device initialized", game_pid);
            if (!settings.attached) {
                // Logged only: a console write blocks while the window is in selection mode (a click on it), and VanaCore.exe
                // must exit now whatever the window is doing.
                note(L"The game is running. VanaCore is detaching (attached=0).");
                CloseHandle(started);
                CloseHandle(process);
                g_detached = true;
                return 0;
            }
            continue; // stay attached so Job Object lifecycle remains enforced until game closes
        }
        break;
    }
    if (started != nullptr)
        CloseHandle(started);
    DWORD code = 0;
    GetExitCodeProcess(process, &code);
    CloseHandle(process);
    // Codes from 0xC0000000 up are Windows' crash codes (access violation, ...). VanaCore then
    // exits with 1 (or kExitGameCrashed in --non-interactive mode), not the raw NTSTATUS code.
    if (code >= 0xC0000000) {
        const std::wstring msg = L"the game crashed, exit code " + describe_exit(code) + L". See logs\\vanacore.log.";
        say(stderr, L"VanaCore: " + msg, kRed);
        const int exit_code = g_non_interactive ? vanacore::kExitGameCrashed : 1;
        emit_status("failed", false, exit_code, "client_crash", msg, game_pid);
        return exit_code;
    }
    if (logged_in && code == static_cast<DWORD>(vanacore::kExitGameStartFailed)) {
        const std::wstring msg = L"the game couldn't start. See logs\\vanacore.log.";
        say(stderr, L"VanaCore: " + msg, kRed);
        emit_status("failed", false, static_cast<int>(code), "game_start_failed", msg, game_pid);
    } else if (code == static_cast<DWORD>(vanacore::kExitGraphicsCardLost)) {
        const std::wstring msg = L"the graphics card stopped (a driver timeout), so the game closed. See logs\\vanacore.log.";
        say(stderr, L"VanaCore: " + msg, kRed);
        emit_status("failed", false, static_cast<int>(code), "graphics_card_lost", msg, game_pid);
    } else if (settings.attached) {
        say(stdout, L"The game has closed (exit code " + describe_exit(code) + L").", kGrey);
        emit_status("exited", code == 0, static_cast<int>(code), code == 0 ? "" : "nonzero_exit",
                    L"Game closed with exit code " + describe_exit(code), game_pid);
    } else {
        note(L"the game exited before it was up, exit code " + describe_exit(code));
        emit_status("exited", code == 0, static_cast<int>(code), code == 0 ? "" : "early_exit",
                    L"Game exited before ready with code " + describe_exit(code), game_pid);
    }
    return static_cast<int>(code);
}

} // namespace

int wmain(int argc, wchar_t** argv)
{
    const std::wstring first = argc > 1 ? argv[1] : L"";
    // The game process of VanaCore's own login: VanaCore.Core has set up already (its crash
    // handler and log too).
    if (first == vanacore::kGameHostArgument)
        return run_game();
    if (first == L"/?" || first == L"-h") {
        std::fwprintf(stderr,
                      L"usage: VanaCore.exe [--server S] [--user U] [--password-stdin] [--lang JP|US|EU]\n"
                      L"                                                 SoulspireXI (based on VanaCore): logs in securely\n"
                      L"                                                 using --password-stdin or a hidden prompt (default\n"
                      L"                                                 server %ls; plaintext --password and INI password=\n"
                      L"                                                 are disabled)\n"
                      L"       VanaCore.exe --retail                     (or [client] retail=1) PlayOnline as installed: the\n"
                      L"                                                 registry's pol.exe, no login of VanaCore's own\n"
                      L"       VanaCore.exe [bootloader arguments...]    the same through [client] bootloader=\n"
                      L"       VanaCore.exe <executable> [arguments...]  any other program, with VanaCore.Core loaded\n"
                      L"       any of them with --ini <file>             that settings file in place of vanacore.ini (from\n"
                      L"                                                 VanaCore's folder unless a full path; it must be there)\n"
                      L"       --non-interactive                         suppress MessageBox/console prompts and emit JSON status\n"
                      L"       --status-file <file>                      write atomic machine-readable JSON readiness/exit status\n",
                      vanacore::kDefaultServer);
        return 2;
    }
    const std::wstring dir = exe_dir();
    g_non_interactive = has_non_interactive_argument(argc, argv);
    g_status_file = status_file_argument(argc, argv, dir);
    g_logs_dir = dir + L"\\logs";
    // the settings file: --ini's, else vanacore.ini
    const std::wstring given = ini_argument(argc, argv, dir);
    const std::wstring settings_ini = given.empty() ? dir + L"\\vanacore.ini" : given;
    // [logging] enabled=0 turns logs\vanacore.log off, on by default (VanaCore.Core reads the same key).
    if (GetPrivateProfileIntW(L"logging", L"enabled", 1, settings_ini.c_str()) != 0)
        g_log.open(dir);
    SetUnhandledExceptionFilter(&on_crash);
    note(L"VanaCore v" + std::wstring(vanacore::kVersion) + L" started (process " +
         std::to_wstring(GetCurrentProcessId()) + (g_non_interactive ? L", non-interactive)" : L")"));
    emit_status("starting", true, 0, "", L"VanaCore launcher started");
    // A click in the console window starts a text selection ("Select" in its title), which freezes every write to the
    // console until a key is pressed: VanaCore.exe would then never print or exit. Selection by click is off.
    {
        const HANDLE input = GetStdHandle(STD_INPUT_HANDLE);
        DWORD mode = 0;
        if (GetConsoleMode(input, &mode))
            SetConsoleMode(input, (mode & ~static_cast<DWORD>(ENABLE_QUICK_EDIT_MODE)) | ENABLE_EXTENDED_FLAGS);
    }
    say(stdout, L"Welcome to SoulspireXI Experimental Client (based on VanaCore)!", kMagenta);
    const int result = run(argc, argv, settings_ini);
    note(L"exiting with code " + describe_exit(static_cast<DWORD>(result)));
    // A window VanaCore owns would close now and take any error with it: wait for a key first.
    // Not after detaching: the game is running, and the window is meant to go. Not with
    // [client] autoclose=1 after a normal exit either; errors and crashes still wait.
    // Never wait in --non-interactive mode.
    const bool autoclose = vanacore::load_settings(dir, settings_ini).autoclose && result == 0;
    if (!g_non_interactive && !g_detached && !g_refused && !autoclose && owns_console_window()) {
        say(stdout, L"Press any key to close this window.", kGrey);
        FlushConsoleInputBuffer(GetStdHandle(STD_INPUT_HANDLE)); // drop keys typed while playing
        _getwch();
    }
    return result;
}
