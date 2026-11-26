#include "registry.hpp"

#include <windows.h>

#include <intrin.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "client.hpp"
#include "imports.hpp"
#include "log.hpp"
#include "registry_policy.hpp"
#include "window.hpp"

namespace vanacore::registry {
namespace {

// advapi32's Reg* exports are stubs that jump through advapi32's own imports from this API set,
// so hooking those imports covers every caller (FFXiMain.dll, polcore.dll, ...), however it
// found the function.
constexpr const char* kRegistryApiSets[] = {"api-ms-win-core-registry-l1-1-0.dll",
                                            "api-ms-win-core-registry-l1-1-1.dll"};

// A value served in place of the registry's, held as the bytes RegQueryValueExA hands back.
struct Value {
    Key key;
    std::string name; // value name, e.g. "0001"
    DWORD type;       // REG_DWORD or REG_SZ
    std::vector<BYTE> data;
    std::wstring text; // as written in the ini, for the log
    std::wstring from; // where it came from, for the log: "[ffxi] mipmap", "[ffxi] mipmap's default",
                       // "[ffxi.registry]"; empty for [language]'s and the install folders
};

// [ffxi]: the game's own settings by name, each handed to the game as the registry value it reads it
// by (0001), as [ffxi.registry]'s are. For a value both set, [ffxi]'s wins, or [ffxi.registry]'s with
// [ffxi] registry.override=1. One neither sets gets the default here, a new account's in the
// CatsEyeXI launcher, if it has one; else the game's own. resolution.game is also the window's size
// in the windowed modes (window.cpp), and mode is 0034 (query_value).
enum class Form {
    Number, // REG_DWORD, from `low` to `high`
    Size,   // <width>x<height>: REG_DWORDs `name` and `height`
    Float,  // REG_DWORD holding a float's bits
    Text,   // REG_SZ
};
struct Named {
    const wchar_t* key; // in [ffxi]
    const char* name;   // the game's value name
    Form form;
    const wchar_t* preset = nullptr; // the default, as [ffxi] would write it; "screen" is the main
                                     // display's resolution, "resolution.game" that setting's
    const char* height = nullptr;    // Size: the height's value name
    long low = 0, high = 0;          // Number: what it's known to take
};
constexpr Named kNamed[] = {
    {L"resolution.game", "0001", Form::Size, L"screen", "0002"},
    {L"resolution.ui", "0037", Form::Size, L"resolution.game", "0038"},
    {L"resolution.background", "0003", Form::Size, L"2048x2048", "0004"},
    {L"mipmap", "0000", Form::Number, L"6", nullptr, 0, 6},
    {L"env_anim_mode", "0011", Form::Number, L"2", nullptr, 0, 2},
    {L"bump_mapping", "0017", Form::Number, L"0", nullptr, 0, 1},
    {L"texture_compression", "0018", Form::Number, L"2", nullptr, 0, 2},
    {L"map_compression", "0019", Form::Number, L"2", nullptr, 0, 2},
    {L"font_compression", "0036", Form::Number, L"0", nullptr, 0, 2},
    {L"graphics_stabilization", "0040", Form::Number, L"1", nullptr, 0, 1},
    {L"gamma", "0028", Form::Float},
    {L"lcd_3d", "0030", Form::Number, nullptr, nullptr, 0, 1},
    {L"sound.enabled", "0007", Form::Number, nullptr, nullptr, 0, 1},
    {L"sound.max_sounds", "0029", Form::Number, L"20", nullptr, 12, 100}, // soundslots.cpp: 100 slots, this the cap, live
    {L"sound.background", "0035", Form::Number, L"0", nullptr, 0, 1},
    {L"hardware_mouse", "0021", Form::Number, nullptr, nullptr, 0, 1},
    {L"opening_movie", "0022", Form::Number, nullptr, nullptr, 0, 1},
    {L"simple_char_creation", "0023", Form::Number, nullptr, nullptr, 0, 1},
    {L"ime_mode", "0039", Form::Number, nullptr, nullptr, 0, 1},
    {L"beta_ui", "0041", Form::Number, nullptr, nullptr, 0, 1},
    {L"screenshot.path", "0042", Form::Text},
    {L"screenshot.screen_resolution", "0043", Form::Number, nullptr, nullptr, 0, 1},
    {L"gamepad.mode", "padmode000", Form::Text},
    {L"gamepad.buttons", "padsin000", Form::Text},
    {L"gamepad.buttons_ex", "padexsin000", Form::Text},
    {L"gamepad.guid", "padguid000", Form::Text},
};
constexpr int kDefaultMode = 3; // [ffxi] mode's default: borderless full screen, the launcher's

std::vector<Value> g_values;                   // fixed before the game runs
std::unique_ptr<std::atomic<bool>[]> g_logged; // per value: first read logged

// [graphics] background (renderer=d3d12 or d3d11): the game's background resolution (0003, 0004), the
// size it draws the 3D scene at before making it the window's, served in place of [ffxi]
// resolution.background; 0 x 0 leaves that (the launcher's) to the game.
constexpr DWORD kMaxBackground = 4096; // a side FFXI's background is known to work at
DWORD g_background[2] = {};
std::atomic<bool> g_background_logged[2];

// FFXI's window mode (0034): [ffxi] mode's when set, else the setting's; as the game read it.
int g_forced_mode = -1;
bool g_registry_first = false; // registry.override=1: [ffxi.registry]'s values win over [ffxi]'s
bool g_retail = false;         // [client] retail=1: the machine's own PlayOnline keys, only what [ffxi] names served
bool g_window_always = false; // renderer=d3d12 or d3d11: full screen (0) is served as a window (1) too
std::atomic<int> g_window_mode{-1};
std::atomic<bool> g_mode_logged{false};

decltype(&RegOpenKeyExA) g_open_key = nullptr;
decltype(&RegCreateKeyExA) g_create_key = nullptr;
decltype(&RegQueryValueExA) g_query_value = nullptr;
decltype(&RegEnumValueA) g_enum_value = nullptr;
decltype(&RegQueryInfoKeyA) g_query_info = nullptr;

using NtQueryKey = LONG(NTAPI*)(HANDLE key, int info_class, void* info, ULONG length, ULONG* result_length);
constexpr int kKeyNameInformation = 3;
NtQueryKey g_query_key = nullptr;

// A PlayOnline install's keys are always stood in for by keys of the same name under
// HKEY_CURRENT_USER\<kStandInRoot>, which needs no admin rights, whether or not the registry has them.
constexpr wchar_t kStandInRoot[] = L"Software\\VanaCore\\Registry";
std::mutex g_stand_in_mutex;
std::set<std::wstring> g_stand_in_logged; // stand-in keys, and their default values, logged once

// Diagnostics: each distinct registry call on a PlayOnline key is logged with the DLL that made
// it, up to this many lines per run.
std::atomic<int> g_trace_lines{1000};
std::mutex g_trace_mutex;
std::set<std::wstring> g_traced;

// Calls that reached open_key, to check at install that the hooks are on the way
std::atomic<unsigned> g_open_calls{0};

std::wstring_view trim(std::wstring_view text)
{
    const size_t first = text.find_first_not_of(L" \t");
    if (first == std::wstring_view::npos)
        return {};
    return text.substr(first, text.find_last_not_of(L" \t") - first + 1);
}

std::string narrow(std::wstring_view text)
{
    const int length = WideCharToMultiByte(CP_ACP, 0, text.data(), static_cast<int>(text.size()), nullptr, 0,
                                           nullptr, nullptr);
    std::string out(length, '\0');
    WideCharToMultiByte(CP_ACP, 0, text.data(), static_cast<int>(text.size()), out.data(), length, nullptr, nullptr);
    return out;
}

std::wstring widen(const char* text)
{
    if (text == nullptr)
        return {};
    const int length = MultiByteToWideChar(CP_ACP, 0, text, -1, nullptr, 0);
    std::wstring out(length > 0 ? length - 1 : 0, L'\0');
    MultiByteToWideChar(CP_ACP, 0, text, -1, out.data(), length);
    return out;
}

// "0001=1920" -> REG_DWORD 1920; "0042=C:\..." -> REG_SZ. False for comments and kept values.
bool parse(std::wstring_view line, Value& value)
{
    const size_t equals = line.find(L'=');
    if (equals == std::wstring_view::npos)
        return false;
    const std::wstring_view key = trim(line.substr(0, equals));
    const std::wstring text(trim(line.substr(equals + 1)));
    if (key.empty() || key[0] == L';' || key[0] == L'#' || text.empty() || text == L"-1")
        return false;

    value.key = Key::Settings;
    value.name = narrow(key);
    value.text = text;
    wchar_t* end = nullptr;
    errno = 0;
    const long long number = std::wcstoll(text.c_str(), &end, 10);
    if (*end == L'\0' && errno == 0 && number >= INT32_MIN && number <= UINT32_MAX) {
        const auto dword = static_cast<DWORD>(number);
        value.type = REG_DWORD;
        value.data.assign(reinterpret_cast<const BYTE*>(&dword), reinterpret_cast<const BYTE*>(&dword + 1));
    } else {
        const std::string ansi = narrow(text);
        value.type = REG_SZ;
        value.data.assign(ansi.begin(), ansi.end());
        value.data.push_back(0);
    }
    return true;
}

const Value* find(Key key, const char* name)
{
    if (name == nullptr)
        return nullptr;
    for (const Value& value : g_values)
        if (value.key == key && _stricmp(value.name.c_str(), name) == 0) // value names are case-insensitive
            return &value;
    return nullptr;
}

bool any_named(const char* name)
{
    return find(Key::Settings, name) != nullptr || find(Key::InstallFolder, name) != nullptr ||
           find(Key::PolSettings, name) != nullptr;
}

Value dword_value(const char* name, DWORD number, std::wstring text, std::wstring from)
{
    return Value{Key::Settings, name, REG_DWORD,
                 std::vector<BYTE>(reinterpret_cast<const BYTE*>(&number), reinterpret_cast<const BYTE*>(&number + 1)),
                 std::move(text), std::move(from)};
}

// "2560x1600" (spaces, and a capital X, too). False for anything else.
bool parse_size(const std::wstring& text, long& width, long& height)
{
    wchar_t* end = nullptr;
    width = std::wcstol(text.c_str(), &end, 10);
    if (end == text.c_str())
        return false;
    while (*end == L' ')
        ++end;
    if (*end != L'x' && *end != L'X')
        return false;
    const wchar_t* at = end + 1;
    height = std::wcstol(at, &end, 10);
    return end != at && *end == L'\0' && width >= 1 && height >= 1 && width <= 16384 && height <= 16384;
}

// The resolution Windows has the monitor at that `window` is on (the main one for nullptr), in real
// pixels whatever the display's scaling.
bool monitor_resolution(HWND window, long& width, long& height)
{
    const HMONITOR monitor = window != nullptr ? MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST)
                                               : MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFOEXW info{};
    info.cbSize = sizeof info;
    DEVMODEW mode{};
    mode.dmSize = sizeof mode;
    if (GetMonitorInfoW(monitor, &info) && EnumDisplaySettingsW(info.szDevice, ENUM_CURRENT_SETTINGS, &mode) && mode.dmPelsWidth > 0 &&
        mode.dmPelsHeight > 0) {
        width = static_cast<long>(mode.dmPelsWidth), height = static_cast<long>(mode.dmPelsHeight);
        return true;
    }
    const HDC screen = GetDC(nullptr);
    width = GetDeviceCaps(screen, DESKTOPHORZRES), height = GetDeviceCaps(screen, DESKTOPVERTRES);
    ReleaseDC(nullptr, screen);
    return width > 0 && height > 0;
}

// The monitor's work area (the screen less the taskbar and anything docked like it), the same monitor as
// monitor_resolution's, in its real pixels whatever this thread's DPI awareness
bool work_area(HWND window, long& width, long& height)
{
    const HMONITOR monitor = window != nullptr ? MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST)
                                               : MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO info{sizeof info};
    long screen_width = 0, screen_height = 0;
    if (!GetMonitorInfoW(monitor, &info) || !monitor_resolution(window, screen_width, screen_height))
        return false;
    const long across = info.rcMonitor.right - info.rcMonitor.left, down = info.rcMonitor.bottom - info.rcMonitor.top;
    if (across <= 0 || down <= 0)
        return false;
    width = std::max(1L, static_cast<long>(MulDiv(info.rcWork.right - info.rcWork.left, screen_width, across)));
    height = std::max(1L, static_cast<long>(MulDiv(info.rcWork.bottom - info.rcWork.top, screen_height, down)));
    return true;
}

// `text` as [ffxi] `named` takes it: its values (a Size's two) into `out`, each from `from`. False,
// logged, if it isn't what the setting takes.
bool named_values(const Named& named, const std::wstring& text, const std::wstring& from, std::vector<Value>& out)
{
    const auto invalid = [&](const wchar_t* expected) {
        log::write(L"Game settings: %ls=%ls isn't %ls; ignored", from.c_str(), text.c_str(), expected);
        return false;
    };
    wchar_t* end = nullptr;
    switch (named.form) {
    case Form::Size: {
        long width = 0, height = 0;
        // resolution.ui=window[-share]: its size once the game's resolution is known (fit_window), whatever sets that
        if (_stricmp(named.name, "0037") == 0 && window_share(text) > 0) {
            out.push_back(dword_value(named.name, 0, text, from));
            out.push_back(dword_value(named.height, 0, text, from));
            return true;
        }
        if (!parse_resolution(text, nullptr, width, height))
            return invalid(L"<width>x<height> (1920x1080), auto, auto-<percent> (auto-75) or auto-taskbar");
        if (const std::wstring trimmed(trim(text)); _wcsnicmp(trimmed.c_str(), L"auto", 4) == 0)
            log::write(L"Game settings: %ls=%ls is %ldx%ld (the main display's resolution%ls)", from.c_str(), text.c_str(), width, height,
                       _wcsicmp(trimmed.c_str(), L"auto") == 0            ? L""
                       : _wcsicmp(trimmed.c_str(), L"auto-taskbar") == 0 ? L" less the taskbar"
                                                                          : L", scaled");
        out.push_back(dword_value(named.name, static_cast<DWORD>(width), std::to_wstring(width), from));
        out.push_back(dword_value(named.height, static_cast<DWORD>(height), std::to_wstring(height), from));
        return true;
    }
    case Form::Number: {
        const long number = std::wcstol(text.c_str(), &end, 10);
        if (*end != L'\0')
            return invalid(L"a whole number");
        // what the setting is known to take; the game gets any other number all the same
        if (number < named.low || number > named.high)
            log::write(L"Game settings: %ls=%ls is outside the %ld to %ld it's known to take; handed to the game "
                       L"as it is",
                       from.c_str(), text.c_str(), named.low, named.high);
        out.push_back(dword_value(named.name, static_cast<DWORD>(number), text, from));
        return true;
    }
    case Form::Float: {
        const float number = std::wcstof(text.c_str(), &end);
        if (end == text.c_str() || *end != L'\0')
            return invalid(L"a number");
        DWORD bits = 0;
        std::memcpy(&bits, &number, sizeof bits);
        out.push_back(dword_value(named.name, bits, text, from));
        return true;
    }
    default: {
        const std::string ansi = narrow(text);
        Value value{Key::Settings, named.name, REG_SZ, std::vector<BYTE>(ansi.begin(), ansi.end()), text, from};
        value.data.push_back(0);
        out.push_back(std::move(value));
        return true;
    }
    }
}

} // namespace

bool parse_background(const std::wstring& text, long& width, long& height)
{
    std::wstring compact;
    for (const wchar_t c : text)
        if (c != L' ' && c != L'\t')
            compact += c;
    width = height = 0;
    if (compact.empty() || _wcsicmp(compact.c_str(), L"launcher") == 0)
        return true;
    if (compact.find_first_not_of(L"0123456789") == std::wstring::npos)
        width = height = std::wcstol(compact.c_str(), nullptr, 10);
    else if (!parse_size(compact, width, height))
        width = height = 0;
    if (width >= 512 && height >= 512 && width <= 8192 && height <= 8192)
        return true;
    width = height = 0;
    return false;
}

bool parse_resolution(const std::wstring& text, void* window, long& width, long& height)
{
    std::wstring compact;
    for (const wchar_t c : text)
        if (c != L' ' && c != L'\t')
            compact += c;
    if (_wcsnicmp(compact.c_str(), L"auto", 4) != 0)
        return parse_size(compact, width, height);
    if (_wcsicmp(compact.c_str(), L"auto-taskbar") == 0)
        return work_area(static_cast<HWND>(window), width, height);
    long percent = 100;
    if (compact.size() > 4) {
        if (compact[4] != L'-')
            return false;
        const wchar_t* const digits = compact.c_str() + 5;
        wchar_t* end = nullptr;
        percent = std::wcstol(digits, &end, 10);
        if (end == digits || (*end != L'\0' && !(*end == L'%' && end[1] == L'\0')) || percent < 1 || percent > 100)
            return false;
    }
    long screen_width = 0, screen_height = 0;
    if (!monitor_resolution(static_cast<HWND>(window), screen_width, screen_height))
        return false;
    width = std::max(1L, std::lround(static_cast<double>(screen_width) * percent / 100.0));
    height = std::max(1L, std::lround(static_cast<double>(screen_height) * percent / 100.0));
    return true;
}

int window_share(const std::wstring& text)
{
    std::wstring compact;
    for (const wchar_t c : text)
        if (c != L' ' && c != L'\t')
            compact += c;
    if (_wcsnicmp(compact.c_str(), L"window", 6) != 0)
        return 0;
    if (compact.size() == 6)
        return 100;
    if (compact[6] != L'-')
        return 0;
    const wchar_t* const digits = compact.c_str() + 7;
    wchar_t* end = nullptr;
    const long percent = std::wcstol(digits, &end, 10);
    if (end == digits || (*end != L'\0' && !(*end == L'%' && end[1] == L'\0')) || percent < 1 || percent > 100)
        return 0;
    return static_cast<int>(percent);
}

namespace {

// A REG_DWORD value's number as text ("2560"); empty for none.
std::wstring number_text(const Value* value)
{
    if (value == nullptr || value->type != REG_DWORD)
        return {};
    std::int32_t number = 0;
    std::memcpy(&number, value->data.data(), sizeof number);
    return std::to_wstring(number);
}

// [ffxi] resolution.ui=window[-share], once the rest is read: the menus that share of the game's resolution (0001 x 0002 as
// the game gets them, else the main display's), each side, the width worked out from the height so the shape is the
// game's. It's the window's client area in the windowed modes, its title bar and borders left out. uiscale.cpp fits it
// to [ui] aspect's shape once the game runs, when that squeezes the interface.
void fit_window()
{
    Value* width = nullptr;
    Value* height = nullptr;
    for (Value& value : g_values) {
        if (value.key == Key::Settings && _stricmp(value.name.c_str(), "0037") == 0)
            width = &value;
        else if (value.key == Key::Settings && _stricmp(value.name.c_str(), "0038") == 0)
            height = &value;
    }
    const int share = width != nullptr && height != nullptr ? window_share(width->text) : 0;
    if (share == 0)
        return;
    long game_w = std::wcstol(number_text(find(Key::Settings, "0001")).c_str(), nullptr, 10);
    long game_h = std::wcstol(number_text(find(Key::Settings, "0002")).c_str(), nullptr, 10);
    if ((game_w < 64 || game_h < 64) && !monitor_resolution(nullptr, game_w, game_h))
        return;
    const long h = std::max(64L, std::lround(static_cast<double>(game_h) * share / 100.0));
    const long w = std::max(64L, std::lround(static_cast<double>(h) * game_w / game_h));
    const std::wstring text = width->text, from = width->from;
    *width = dword_value("0037", static_cast<DWORD>(w), std::to_wstring(w), from);
    *height = dword_value("0038", static_cast<DWORD>(h), std::to_wstring(h), from);
    log::write(L"Game settings: %ls=%ls is %ldx%ld (%d%% of the game's resolution, %ldx%ld)", from.c_str(), text.c_str(), w, h,
               share, game_w, game_h);
}

// [ffxi]'s defaults into g_values for what neither [ffxi] nor [ffxi.registry] sets and [ffxi] -1
// doesn't leave to the game, [ffxi] mode's (0034) among them. How many went in.
size_t add_defaults(const std::set<std::string>& own)
{
    size_t defaults = 0;
    for (const Named& setting : kNamed) {
        if (setting.preset == nullptr)
            continue;
        std::wstring text = setting.preset;
        if (text == L"screen") {
            const HDC screen = GetDC(nullptr);
            text = std::to_wstring(GetDeviceCaps(screen, DESKTOPHORZRES)) + L"x" +
                   std::to_wstring(GetDeviceCaps(screen, DESKTOPVERTRES));
            ReleaseDC(nullptr, screen);
        } else if (text == L"resolution.game") {
            const std::wstring width = number_text(find(Key::Settings, "0001"));
            const std::wstring height = number_text(find(Key::Settings, "0002"));
            if (width.empty() || height.empty())
                continue;
            text = width + L"x" + height;
        }
        std::vector<Value> values;
        named_values(setting, text, std::wstring(L"[ffxi] ") + setting.key + L"'s default", values);
        for (Value& value : values)
            if (find(Key::Settings, value.name.c_str()) == nullptr && own.count(value.name) == 0) {
                g_values.push_back(std::move(value));
                ++defaults;
            }
    }
    if (g_forced_mode < 0 && find(Key::Settings, "0034") == nullptr && own.count("0034") == 0) {
        g_values.push_back(dword_value("0034", kDefaultMode, std::to_wstring(kDefaultMode), L"[ffxi] mode's default"));
        ++defaults;
    }
    return defaults;
}

// The game's settings from vanacore.ini into g_values, the one that wins first: [ffxi]'s, then
// [ffxi.registry]'s for what it doesn't set (the other way round with registry.override=1), then
// [ffxi]'s defaults for what neither sets. [ffxi] -1 has no default: the game's registry decides it.
// Also [ffxi] mode (g_forced_mode).
void read_settings(const wchar_t* ini)
{
    const bool registry_first = GetPrivateProfileIntW(L"ffxi", L"registry.override", 0, ini) != 0;
    g_registry_first = registry_first;

    std::vector<Value> named, numbered;
    std::set<std::string> own; // [ffxi] -1: the game's own, no default
    for (const Named& setting : kNamed) {
        wchar_t buffer[1024] = {};
        GetPrivateProfileStringW(L"ffxi", setting.key, L"", buffer, 1024, ini);
        const std::wstring text(trim(buffer));
        if (text == L"-1") {
            own.insert(setting.name);
            if (setting.height != nullptr)
                own.insert(setting.height);
        } else if (!text.empty()) {
            named_values(setting, text, std::wstring(L"[ffxi] ") + setting.key, named);
        }
    }
    wchar_t mode[64] = {};
    GetPrivateProfileStringW(L"ffxi", L"mode", L"", mode, 64, ini);
    if (trim(mode) == L"-1")
        own.insert("0034");

    std::wstring section(32767, L'\0'); // GetPrivateProfileSection's maximum
    const DWORD length =
        GetPrivateProfileSectionW(L"ffxi.registry", section.data(), static_cast<DWORD>(section.size()), ini);
    // The section comes back as "key=value\0key=value\0\0".
    for (size_t at = 0; at < length;) {
        const std::wstring_view line(section.c_str() + at);
        at += line.size() + 1;
        Value value;
        if (parse(line, value)) {
            value.from = L"[ffxi.registry]";
            numbered.push_back(std::move(value));
        }
    }

    // First one wins, as in GetPrivateProfileString.
    size_t from_named = 0, from_numbered = 0;
    const auto add = [&](std::vector<Value>& values, size_t& count) {
        for (Value& value : values) {
            if (const Value* known = find(Key::Settings, value.name.c_str())) {
                if (known->from != value.from)
                    log::write(L"Game settings: %hs=%ls from %ls is ignored: %ls sets it (registry.override=%d)",
                               value.name.c_str(), value.text.c_str(), value.from.c_str(), known->from.c_str(),
                               registry_first ? 1 : 0);
                continue;
            }
            g_values.push_back(std::move(value));
            ++count;
        }
    };
    add(registry_first ? numbered : named, registry_first ? from_numbered : from_named);
    add(registry_first ? named : numbered, registry_first ? from_named : from_numbered);

    // [ffxi] mode: FFXI's window mode, in place of 0034's
    g_forced_mode = window::parse_mode(mode);
    if (const Value* numbered_mode = find(Key::Settings, "0034"); g_forced_mode >= 0 && numbered_mode != nullptr) {
        if (registry_first)
            g_forced_mode = -1;
        log::write(L"Game settings: %ls is ignored: %ls sets it (registry.override=%d)",
                   registry_first ? (L"[ffxi] mode=" + std::wstring(mode)).c_str()
                                  : (L"0034=" + numbered_mode->text + L" from [ffxi.registry]").c_str(),
                   registry_first ? L"[ffxi.registry] 0034" : L"[ffxi] mode", registry_first ? 1 : 0);
    }

    // retail: a setting [ffxi] doesn't name is the game's own, not the launcher's default
    const size_t defaults = g_retail ? 0 : add_defaults(own);
    fit_window();

    log::write(L"Game settings from %ls: %zu values from [ffxi], %zu from [ffxi.registry]%ls, %zu defaults", ini,
               from_named, from_numbered, registry_first ? L" (first: registry.override=1)" : L"", defaults);
}

// The keys a vanacore.ini from before has where they no longer are: moved to where they are now,
// where it doesn't have them already, or taken out. The window's size in the windowed modes is
// resolution.game's now, so size is gone.
void move_old_keys(const wchar_t* ini)
{
    struct Move {
        const wchar_t* section;
        const wchar_t* key;
        const wchar_t* to_section; // nullptr: taken out
        const wchar_t* to_key;
    };
    constexpr Move kMoves[] = {
        {L"window", L"mode", L"ffxi", L"mode"},
        {L"window", L"resolution", L"graphics", L"resolution"},
        {L"ffxi", L"resolution.window", L"graphics", L"resolution"},
        {L"window", L"size", nullptr, nullptr},
        {L"ffxi", L"size", nullptr, nullptr},
    };
    constexpr wchar_t kMissing[] = L"\x1F<none>\x1F";
    for (const Move& move : kMoves) {
        wchar_t value[256] = {}, now[256] = {};
        GetPrivateProfileStringW(move.section, move.key, kMissing, value, 256, ini);
        if (std::wcscmp(value, kMissing) == 0)
            continue;
        WritePrivateProfileStringW(move.section, move.key, nullptr, ini);
        if (move.to_section == nullptr) {
            log::write(L"Settings: [%ls] %ls=%ls taken out: the window is [ffxi] resolution.game's size", move.section,
                       move.key, value);
            continue;
        }
        GetPrivateProfileStringW(move.to_section, move.to_key, kMissing, now, 256, ini);
        const bool moved = std::wcscmp(now, kMissing) == 0 && value[0] != L'\0';
        if (moved)
            WritePrivateProfileStringW(move.to_section, move.to_key, value, ini);
        log::write(L"Settings: [%ls] %ls=%ls %ls [%ls] %ls", move.section, move.key, value,
                   moved ? L"moved to" : L"taken out: its place is", move.to_section, move.to_key);
    }
}

// The key's full kernel name (\REGISTRY\MACHINE\SOFTWARE\WOW6432Node\...), whichever way it was
// opened. Empty for predefined keys (HKEY_LOCAL_MACHINE, ...: pseudo-handles with the top bit set).
std::wstring key_path(HKEY key)
{
    if (g_query_key == nullptr || key == nullptr || (reinterpret_cast<ULONG_PTR>(key) & 0x80000000) != 0)
        return {};
    struct {
        ULONG length; // bytes
        wchar_t name[512];
    } info; // KEY_NAME_INFORMATION
    ULONG length = 0;
    if (g_query_key(key, kKeyNameInformation, &info, sizeof(info), &length) < 0)
        return {};
    return std::wstring(info.name, info.length / sizeof(wchar_t));
}

// Takes the last component off `path` and returns it.
std::wstring_view pop(std::wstring_view& path)
{
    const size_t slash = path.rfind(L'\\');
    const std::wstring_view last = slash == std::wstring_view::npos ? path : path.substr(slash + 1);
    path = slash == std::wstring_view::npos ? std::wstring_view{} : path.substr(0, slash);
    return last;
}

bool is(std::wstring_view text, std::wstring_view expected)
{
    return text.size() == expected.size() && _wcsnicmp(text.data(), expected.data(), text.size()) == 0;
}

// PlayOnline, PlayOnlineUS or PlayOnlineEU (not PlayOnlineViewer).
bool is_region_root(std::wstring_view name)
{
    return name.size() <= 12 && is(name.substr(0, 10), L"PlayOnline");
}

bool is_vendor(std::wstring_view name)
{
    return is(name, L"SquareEnix") || is(name, L"SQUARE");
}

// Which served key `path` is, under WOW6432Node or the UAC VirtualStore alike (JP uses SQUARE
// where the others use SquareEnix):
//   Settings       ...\PlayOnline[US|EU]\SquareEnix\FinalFantasyXI
//   InstallFolder  ...\PlayOnline[US|EU]\InstallFolder
//   PolSettings    ...\PlayOnline[US|EU]\SquareEnix\PlayOnlineViewer\Settings
std::optional<Key> classify(std::wstring_view path)
{
    const std::wstring_view last = pop(path);
    if (is(last, L"FinalFantasyXI")) {
        const std::wstring_view vendor = pop(path);
        if (is_vendor(vendor) && is_region_root(pop(path)))
            return Key::Settings;
        return std::nullopt;
    }
    if (is(last, L"InstallFolder") && is_region_root(pop(path)))
        return Key::InstallFolder;
    if (is(last, L"Settings") && is(pop(path), L"PlayOnlineViewer") && is_vendor(pop(path)) &&
        is_region_root(pop(path)))
        return Key::PolSettings;
    return std::nullopt;
}

const wchar_t* key_label(Key key)
{
    switch (key) {
    case Key::Settings: return L"Game setting";
    case Key::InstallFolder: return L"Install folder";
    default: return L"PlayOnline setting";
    }
}

// A normal install's values (the CatsEyeXI launcher's reference ffxi.reg, with its resolutions and
// window mode), which a stand-in key serves for a value it doesn't hold: [ffxi.registry], [language]
// and the install folders come first, and a value the game writes to the stand-in is kept.
struct StandInValue {
    std::wstring_view key; // as policy::kStandInKeys
    const char* name;      // "" is the key's default value
    DWORD type;
    DWORD number;          // REG_DWORD
    const wchar_t* text;   // REG_SZ (environment variables expanded); REG_BINARY as hex
};
constexpr StandInValue kStandInValues[] = {
    {L"", "CommonFilesFolder", REG_SZ, 0, L"%CommonProgramFiles%\\"},
    {L"Interface", "", REG_SZ, 0, L""},
    {L"Interface", "0001", REG_SZ, 0, L"0"},
    {L"Interface", "1000", REG_SZ, 0, L"001b1394"},
    {L"Product", "0001", REG_BINARY, 0, L""},
    {L"SquareEnix\\FinalFantasyXI", "0000", REG_DWORD, 6, nullptr},
    {L"SquareEnix\\FinalFantasyXI", "0001", REG_DWORD, 1920, nullptr},
    {L"SquareEnix\\FinalFantasyXI", "0002", REG_DWORD, 1080, nullptr},
    {L"SquareEnix\\FinalFantasyXI", "0003", REG_DWORD, 2048, nullptr},
    {L"SquareEnix\\FinalFantasyXI", "0004", REG_DWORD, 2048, nullptr},
    {L"SquareEnix\\FinalFantasyXI", "0007", REG_DWORD, 1, nullptr},
    {L"SquareEnix\\FinalFantasyXI", "0011", REG_DWORD, 2, nullptr},
    {L"SquareEnix\\FinalFantasyXI", "0017", REG_DWORD, 0, nullptr},
    {L"SquareEnix\\FinalFantasyXI", "0018", REG_DWORD, 2, nullptr},
    {L"SquareEnix\\FinalFantasyXI", "0019", REG_DWORD, 1, nullptr},
    {L"SquareEnix\\FinalFantasyXI", "0020", REG_DWORD, 1, nullptr},
    {L"SquareEnix\\FinalFantasyXI", "0021", REG_DWORD, 1, nullptr},
    {L"SquareEnix\\FinalFantasyXI", "0022", REG_DWORD, 0, nullptr},
    {L"SquareEnix\\FinalFantasyXI", "0023", REG_DWORD, 0, nullptr},
    {L"SquareEnix\\FinalFantasyXI", "0024", REG_DWORD, 0, nullptr},
    {L"SquareEnix\\FinalFantasyXI", "0028", REG_DWORD, 0, nullptr},
    {L"SquareEnix\\FinalFantasyXI", "0029", REG_DWORD, 20, nullptr},
    {L"SquareEnix\\FinalFantasyXI", "0030", REG_DWORD, 0, nullptr},
    {L"SquareEnix\\FinalFantasyXI", "0031", REG_DWORD, 0x3BC49BA6, nullptr}, // the float 0.006
    {L"SquareEnix\\FinalFantasyXI", "0032", REG_DWORD, 0, nullptr},
    {L"SquareEnix\\FinalFantasyXI", "0033", REG_DWORD, 0, nullptr},
    {L"SquareEnix\\FinalFantasyXI", "0034", REG_DWORD, 3, nullptr},
    {L"SquareEnix\\FinalFantasyXI", "0035", REG_DWORD, 1, nullptr},
    {L"SquareEnix\\FinalFantasyXI", "0036", REG_DWORD, 2, nullptr},
    {L"SquareEnix\\FinalFantasyXI", "0037", REG_DWORD, 1920, nullptr},
    {L"SquareEnix\\FinalFantasyXI", "0038", REG_DWORD, 1080, nullptr},
    {L"SquareEnix\\FinalFantasyXI", "0039", REG_DWORD, 1, nullptr},
    {L"SquareEnix\\FinalFantasyXI", "0040", REG_DWORD, 0, nullptr},
    {L"SquareEnix\\FinalFantasyXI", "0041", REG_DWORD, 0, nullptr},
    {L"SquareEnix\\FinalFantasyXI", "0042", REG_SZ, 0, L"%USERPROFILE%\\Documents\\My Games\\FINAL FANTASY XI"},
    {L"SquareEnix\\FinalFantasyXI", "0043", REG_DWORD, 0, nullptr},
    {L"SquareEnix\\FinalFantasyXI", "0044", REG_DWORD, 1, nullptr},
    {L"SquareEnix\\FinalFantasyXI", "0045", REG_DWORD, 0, nullptr},
    {L"SquareEnix\\FinalFantasyXI", "bFirst", REG_BINARY, 0, L"00"},
    {L"SquareEnix\\FinalFantasyXI", "padguid000", REG_SZ, 0, L"{00000000-0000-0000-0000-000000000000}"},
    {L"SquareEnix\\FinalFantasyXI", "padmode000", REG_SZ, 0, L"1,0,0,0,1,1"},
    {L"SquareEnix\\FinalFantasyXI", "padsin000", REG_SZ, 0,
     L"8,9,13,12,10,0,1,3,2,15,-1,-1,14,-33,-33,32,32,-36,-36,35,35,6,7,5,4,11,-1,-1,-1,-1,-1,-1"},
    {L"SquareEnix\\PlayOnlineViewer", "FirstBootPlayMovie", REG_DWORD, 0, nullptr},
    {L"SquareEnix\\PlayOnlineViewer\\Settings", "FullScreen", REG_DWORD, 0, nullptr},
    {L"SquareEnix\\PlayOnlineViewer\\Settings", "Language", REG_DWORD, 1, nullptr},
    {L"SquareEnix\\PlayOnlineViewer\\Settings", "PlayAudio", REG_DWORD, 1, nullptr},
    {L"SquareEnix\\PlayOnlineViewer\\Settings", "PlayOpeningMovie", REG_DWORD, 1, nullptr},
    {L"SquareEnix\\PlayOnlineViewer\\Settings", "ResetSettings", REG_DWORD, 0, nullptr},
    {L"SquareEnix\\PlayOnlineViewer\\Settings", "SupportLanguage", REG_DWORD, 1, nullptr},
    {L"SquareEnix\\PlayOnlineViewer\\Settings", "UseGameController", REG_DWORD, 0, nullptr},
    {L"SquareEnix\\PlayOnlineViewer\\Settings", "WindowH", REG_DWORD, 480, nullptr},
    {L"SquareEnix\\PlayOnlineViewer\\Settings", "WindowW", REG_DWORD, 640, nullptr},
    {L"SquareEnix\\PlayOnlineViewer\\Settings", "WindowX", REG_DWORD, 0, nullptr},
    {L"SquareEnix\\PlayOnlineViewer\\Settings", "WindowY", REG_DWORD, 0, nullptr},
    {L"SquareEnix\\PlayOnlineViewer\\Settings\\Controler", "AnchorDown", REG_DWORD, 0x34, nullptr},
    {L"SquareEnix\\PlayOnlineViewer\\Settings\\Controler", "AnchorLeft", REG_DWORD, 0x36, nullptr},
    {L"SquareEnix\\PlayOnlineViewer\\Settings\\Controler", "AnchorRight", REG_DWORD, 0x32, nullptr},
    {L"SquareEnix\\PlayOnlineViewer\\Settings\\Controler", "AnchorUp", REG_DWORD, 0x30, nullptr},
    {L"SquareEnix\\PlayOnlineViewer\\Settings\\Controler", "Cancel", REG_DWORD, 2, nullptr},
    {L"SquareEnix\\PlayOnlineViewer\\Settings\\Controler", "ChrCsrNext", REG_DWORD, 7, nullptr},
    {L"SquareEnix\\PlayOnlineViewer\\Settings\\Controler", "ChrCsrPrev", REG_DWORD, 6, nullptr},
    {L"SquareEnix\\PlayOnlineViewer\\Settings\\Controler", "ID", REG_DWORD, 0, nullptr},
    {L"SquareEnix\\PlayOnlineViewer\\Settings\\Controler", "Menu", REG_DWORD, 0, nullptr},
    {L"SquareEnix\\PlayOnlineViewer\\Settings\\Controler", "Navi", REG_DWORD, 3, nullptr},
    {L"SquareEnix\\PlayOnlineViewer\\Settings\\Controler", "Ok", REG_DWORD, 1, nullptr},
    {L"SquareEnix\\PlayOnlineViewer\\Settings\\Controler", "PageNext", REG_DWORD, 5, nullptr},
    {L"SquareEnix\\PlayOnlineViewer\\Settings\\Controler", "PagePrev", REG_DWORD, 4, nullptr},
    {L"SquareEnix\\PlayOnlineViewer\\SystemInfo\\QCheck", "LastMeasurementTime", REG_BINARY, 0, L""},
};

using StandInPlace = policy::StandInTarget;

std::optional<StandInPlace> stand_in_place(std::wstring_view path)
{
    return policy::parse_stand_in_place(path);
}

bool has_stand_in_value(const char* name)
{
    if (g_retail)
        return false;
    const char* wanted = name != nullptr ? name : "";
    return std::any_of(std::begin(kStandInValues), std::end(kStandInValues),
                       [&](const StandInValue& value) { return _stricmp(value.name, wanted) == 0; });
}

// A normal install's value `name` for stand-in key `path` (its kernel path), when the stand-in
// doesn't hold one; nullopt for any other key.
std::optional<Value> stand_in_value(std::wstring_view path, const char* name)
{
    if (!has_stand_in_value(name))
        return std::nullopt;
    const std::optional<StandInPlace> place = stand_in_place(path);
    if (!place || (!place->stand_in && !is(place->key, L"Interface")))
        return std::nullopt;
    const char* wanted = name != nullptr ? name : "";
    for (const StandInValue& entry : kStandInValues) {
        if (!is(entry.key, place->key) || _stricmp(entry.name, wanted) != 0)
            continue;
        Value value{Key::Settings, entry.name, entry.type, {}, {}};
        if (entry.type == REG_DWORD) {
            value.data.assign(reinterpret_cast<const BYTE*>(&entry.number),
                              reinterpret_cast<const BYTE*>(&entry.number + 1));
            value.text = std::to_wstring(entry.number);
        } else if (entry.type == REG_SZ) {
            std::wstring expanded(32767, L'\0'); // ExpandEnvironmentStrings' maximum
            const DWORD length = ExpandEnvironmentStringsW(entry.text, expanded.data(), static_cast<DWORD>(expanded.size()));
            expanded.resize(length > 0 ? length - 1 : 0); // the length counts the terminating null
            value.text = length > 0 ? expanded : std::wstring(entry.text);
            const std::string ansi = narrow(value.text);
            value.data.assign(ansi.begin(), ansi.end());
            value.data.push_back(0);
        } else {
            value.text = std::wstring(L"hex:") + entry.text;
            for (const wchar_t* hex = entry.text; hex[0] != L'\0' && hex[1] != L'\0'; hex += 2)
                value.data.push_back(static_cast<BYTE>(std::wcstoul(std::wstring(hex, 2).c_str(), nullptr, 16)));
        }
        bool first = false;
        {
            std::lock_guard lock(g_stand_in_mutex);
            first = g_stand_in_logged.insert(place->key + L"\\" + widen(entry.name)).second;
        }
        if (first)
            log::write(L"Registry: %ls\\%hs = %ls (a normal install's value, as the stand-in key has none)",
                       place->key.empty() ? place->root.c_str() : place->key.c_str(), entry.name, value.text.c_str());
        return value;
    }
    return std::nullopt;
}

bool mentions_playonline(std::wstring_view text)
{
    constexpr std::wstring_view needle = L"PlayOnline";
    for (size_t i = 0; i + needle.size() <= text.size(); ++i)
        if (_wcsnicmp(text.data() + i, needle.data(), needle.size()) == 0)
            return true;
    return false;
}

std::wstring describe(HKEY key)
{
    if (key == HKEY_CLASSES_ROOT)
        return L"HKCR";
    if (key == HKEY_CURRENT_USER)
        return L"HKCU";
    if (key == HKEY_LOCAL_MACHINE)
        return L"HKLM";
    if (key == HKEY_USERS)
        return L"HKU";
    return key_path(key);
}

std::wstring module_name(void* address)
{
    HMODULE module = nullptr;
    wchar_t path[MAX_PATH] = L"?";
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           static_cast<LPCWSTR>(address), &module))
        GetModuleFileNameW(module, path, MAX_PATH);
    const wchar_t* name = wcsrchr(path, L'\\');
    return name != nullptr ? name + 1 : path;
}

bool tracing()
{
    return g_trace_lines.load(std::memory_order_relaxed) > 0;
}

void trace(void* caller, const wchar_t* format, ...)
{
    wchar_t message[900];
    va_list args;
    va_start(args, format);
    _vsnwprintf_s(message, _TRUNCATE, format, args);
    va_end(args);
    const std::wstring line = std::wstring(message) + L"  [" + module_name(caller) + L"]";
    {
        // Only the set is locked: module_name can take loader locks, and hooks run under them.
        std::lock_guard lock(g_trace_mutex);
        if (!g_traced.insert(line).second)
            return;
    }
    const int left = g_trace_lines.fetch_sub(1);
    if (left > 0)
        log::write(L"registry: %ls%ls", line.c_str(), left == 1 ? L" (trace limit reached)" : L"");
}

void trace_open(void* caller, const wchar_t* function, HKEY parent, LPCSTR subkey, LSTATUS status, const HKEY* result,
                bool read_only)
{
    std::wstring path = status == ERROR_SUCCESS && result != nullptr ? key_path(*result) : std::wstring{};
    if (path.empty())
        path = describe(parent) + L"\\" + widen(subkey);
    if (mentions_playonline(path))
        trace(caller, L"%ls %ls = %ld%ls", function, path.c_str(), status,
              read_only ? L" (write access denied: opened read-only)" : L"");
}

// Retail PlayOnline ran elevated. Under xiloader the game still opens its HKLM\SOFTWARE\PlayOnline*
// keys for writing (FFXiMain.dll creates its settings key with full access), gets
// ERROR_ACCESS_DENIED and never reads its settings. Those opens are retried read-only.
bool retry_read_only(LSTATUS status, REGSAM access, HKEY parent, LPCSTR subkey)
{
    constexpr REGSAM kReadOnly = KEY_READ | KEY_WOW64_32KEY | KEY_WOW64_64KEY;
    return status == ERROR_ACCESS_DENIED && (access & ~kReadOnly) != 0 &&
           mentions_playonline(describe(parent) + L"\\" + widen(subkey));
}

REGSAM read_only_access(REGSAM access)
{
    return KEY_READ | (access & (KEY_WOW64_32KEY | KEY_WOW64_64KEY));
}

// The game's PlayOnline keys are VanaCore's own, whether or not the registry has them (a PlayOnline
// install, retail or another server's): the game gets a stand-in, HKCU\<kStandInRoot>\PlayOnlineUS\...,
// made the first time, so no install is needed and one on the machine neither affects the game nor
// is changed by it. It's a real key, so every registry call works on it as usual: the values above
// are served through it (classify() knows it by the same trailing path), and whatever the game
// writes stays there. ERROR_FILE_NOT_FOUND for anything that isn't one of a PlayOnline install's keys.
LSTATUS open_stand_in(HKEY parent, LPCSTR subkey, REGSAM access, PHKEY result, LPDWORD disposition)
{
    if (g_retail)
        return ERROR_FILE_NOT_FOUND; // retail: the game's keys are the registry's own
    std::wstring path = describe(parent);
    if (subkey != nullptr && subkey[0] != '\0')
        path += L"\\" + widen(subkey);
    const std::optional<StandInPlace> place = stand_in_place(path);
    if (!place || result == nullptr)
        return ERROR_FILE_NOT_FOUND;

    // PlayOnline encrypts FINAL FANTASY XI\patch.ver with the install-specific hex key stored in the
    // real Windows registry under HKLM\SOFTWARE\WOW6432Node\PlayOnline*\Interface\0001. Shadowing
    // Interface with an empty stand-in (defaulting 0001 to "0") causes polcore's patch.ver decryption
    // to fail and FFXiMain.dll to exit silently before creating its window or calling Direct3DCreate8.
    // Open the real HKLM Interface key read-only first (both for direct HKLM opens/creates and for
    // relative "Interface" opens beneath a stand-in PlayOnline* parent handle, never modifying the
    // user's registry); fall back to the stand-in key only when no real PlayOnline Interface key exists.
    if (const std::optional<std::wstring> real_subkey = policy::real_interface_subkey(path)) {
        const auto open = g_open_key != nullptr ? g_open_key : &RegOpenKeyExA;
        HKEY real_key = nullptr;
        if (open(HKEY_LOCAL_MACHINE, narrow(*real_subkey).c_str(), 0,
                 policy::interface_read_only_access(access), &real_key) == ERROR_SUCCESS) {
            *result = real_key;
            if (disposition != nullptr)
                *disposition = REG_OPENED_EXISTING_KEY;
            bool first = false;
            {
                std::lock_guard lock(g_stand_in_mutex);
                first = g_stand_in_logged.insert(L"real:" + place->root + L"\\" + place->key).second;
            }
            if (first)
                log::write(L"Registry: HKLM\\%ls opened from the real Windows registry (read-only) for PlayOnline patch.ver verification",
                           real_subkey->c_str());
            return ERROR_SUCCESS;
        }
    }

    std::wstring stand_in = std::wstring(kStandInRoot) + L"\\" + place->root;
    if (!place->subkey.empty())
        stand_in += L"\\" + place->subkey;
    const auto create = g_create_key != nullptr ? g_create_key : &RegCreateKeyExA;
    DWORD created = 0;
    const LSTATUS status =
        create(HKEY_CURRENT_USER, narrow(stand_in).c_str(), 0, nullptr, 0, access, nullptr, result, &created);
    if (status != ERROR_SUCCESS) {
        log::write(L"Registry: the stand-in for %ls, HKCU\\%ls, couldn't be opened (error %ld); the registry's is used",
                   path.c_str(), stand_in.c_str(), status);
        return ERROR_FILE_NOT_FOUND;
    }
    if (disposition != nullptr)
        *disposition = created;

    bool first = false;
    {
        std::lock_guard lock(g_stand_in_mutex);
        first = g_stand_in_logged.insert(stand_in).second;
    }
    if (first && !place->stand_in)
        log::write(L"Registry: %ls is VanaCore's stand-in, HKCU\\%ls%ls", path.c_str(), stand_in.c_str(),
                   created == REG_CREATED_NEW_KEY ? L" (made now)" : L"");
    return ERROR_SUCCESS;
}

// Whether a key named `subkey` could be a PlayOnline install's without asking the parent's name: every
// path to one names its root (PlayOnlineUS) except a path relative to a stand-in, which the registry
// then finds under HKCU itself.
bool names_playonline(LPCSTR subkey)
{
    return subkey != nullptr && mentions_playonline(widen(subkey));
}

// Same contract as the real RegQueryValueExA: sizes in bytes; a short buffer gets
// ERROR_MORE_DATA and the size it needs; a null buffer just asks for the size.
LSTATUS serve(const Value& value, LPDWORD reserved, LPDWORD type, LPBYTE data, LPDWORD size)
{
    if (reserved != nullptr || (data != nullptr && size == nullptr))
        return ERROR_INVALID_PARAMETER;
    const auto needed = static_cast<DWORD>(value.data.size());
    LSTATUS status = ERROR_SUCCESS;
    if (type != nullptr)
        *type = value.type;
    if (data != nullptr) {
        if (*size < needed)
            status = ERROR_MORE_DATA;
        else
            std::memcpy(data, value.data.data(), needed);
    }
    if (size != nullptr)
        *size = needed;
    return status;
}

LSTATUS APIENTRY query_value(HKEY key, LPCSTR name, LPDWORD reserved, LPDWORD type, LPBYTE data, LPDWORD size)
{
    const int background = g_background[0] == 0 || name == nullptr ? -1
                           : _stricmp(name, "0003") == 0           ? 0
                           : _stricmp(name, "0004") == 0           ? 1
                                                                   : -1;
    const bool mode_query = name != nullptr && _stricmp(name, "0034") == 0;
    if (background < 0 && !mode_query && !any_named(name) && !tracing()) {
        const LSTATUS status = g_query_value(key, name, reserved, type, data, size);
        if (status != ERROR_FILE_NOT_FOUND || !has_stand_in_value(name))
            return status;
        const std::optional<Value> fallback = stand_in_value(key_path(key), name);
        return fallback ? serve(*fallback, reserved, type, data, size) : status;
    }

    const std::wstring path = key_path(key);
    const std::optional<Key> kind = classify(path);
    if (mode_query && kind == Key::Settings) {
        // the window mode: 2 and 3 (borderless) are VanaCore's to make; the game gets a window
        int mode = g_forced_mode;
        if (mode < 0) {
            if (const Value* value = find(Key::Settings, "0034"); value != nullptr && value->type == REG_DWORD) {
                DWORD stored = 0;
                std::memcpy(&stored, value->data.data(), sizeof stored);
                mode = static_cast<int>(stored);
            } else {
                DWORD stored_type = 0, stored = 0, stored_size = sizeof stored;
                const LSTATUS status =
                    g_query_value(key, name, nullptr, &stored_type, reinterpret_cast<BYTE*>(&stored), &stored_size);
                if (status == ERROR_SUCCESS && stored_type == REG_DWORD)
                    mode = static_cast<int>(stored);
                else if (status == ERROR_FILE_NOT_FOUND)
                    if (const std::optional<Value> fallback = stand_in_value(path, name);
                        fallback && fallback->type == REG_DWORD) {
                        std::memcpy(&stored, fallback->data.data(), sizeof stored);
                        mode = static_cast<int>(stored);
                    }
            }
        }
        if (mode >= 0 && mode <= 3) {
            g_window_mode = mode;
            const DWORD served = mode == 0 && !g_window_always ? 0 : 1;
            const Value value{Key::Settings, name, REG_DWORD,
                              std::vector<BYTE>(reinterpret_cast<const BYTE*>(&served),
                                                reinterpret_cast<const BYTE*>(&served + 1)),
                              std::to_wstring(served)};
            static const wchar_t* const kNames[] = {L"full screen", L"windowed", L"borderless windowed",
                                                    L"borderless full screen"};
            if (!g_mode_logged.exchange(true))
                log::write(L"Game setting 0034 = %lu (window mode %d, %ls%ls)", served, mode, kNames[mode],
                           mode >= 2 || served != static_cast<DWORD>(mode)
                               ? L": the game makes a window, VanaCore makes it borderless" : L"");
            return serve(value, reserved, type, data, size);
        }
    }
    if (background >= 0 && kind == Key::Settings) {
        // the game's own target no side past 4096; the renderer draws it at [graphics] background's size
        const DWORD side = std::min(g_background[background], kMaxBackground);
        const Value value{Key::Settings, name, REG_DWORD,
                          std::vector<BYTE>(reinterpret_cast<const BYTE*>(&side),
                                            reinterpret_cast<const BYTE*>(&side + 1)),
                          std::to_wstring(side)};
        if (!g_background_logged[background].exchange(true))
            log::write(L"Game setting %hs = %lu ([graphics] background%ls)", name, side,
                       side < g_background[background] ? L"; the renderer draws it larger" : L"");
        return serve(value, reserved, type, data, size);
    }
    if (const Value* value = kind ? find(*kind, name) : nullptr) {
        if (!g_logged[value - g_values.data()].exchange(true))
            log::write(L"%ls %hs = %ls%ls", key_label(value->key), value->name.c_str(), value->text.c_str(),
                       value->from.empty() ? L"" : (L" (" + value->from + L")").c_str());
        if (tracing())
            trace(_ReturnAddress(), L"RegQueryValueExA %ls\\%hs -> served %ls", path.c_str(), value->name.c_str(),
                  value->text.c_str());
        return serve(*value, reserved, type, data, size);
    }
    const LSTATUS status = g_query_value(key, name, reserved, type, data, size);
    if (status == ERROR_FILE_NOT_FOUND)
        if (const std::optional<Value> fallback = stand_in_value(path, name)) {
            if (tracing())
                trace(_ReturnAddress(), L"RegQueryValueExA %ls\\%hs -> stand-in %ls", path.c_str(),
                      name != nullptr ? name : "", fallback->text.c_str());
            return serve(*fallback, reserved, type, data, size);
        }
    if (tracing() && mentions_playonline(path))
        trace(_ReturnAddress(), L"RegQueryValueExA %ls\\%hs = %ld", path.c_str(), name != nullptr ? name : "",
              status);
    return status;
}

LSTATUS APIENTRY open_key(HKEY parent, LPCSTR subkey, DWORD options, REGSAM access, PHKEY result)
{
    g_open_calls.fetch_add(1, std::memory_order_relaxed);
    // A PlayOnline install's keys are always the stand-ins (except Interface, which resolves to real
    // HKLM read-only even when opened relative to a stand-in PlayOnline* parent handle).
    LSTATUS status = (names_playonline(subkey) || policy::is_interface_subkey(subkey))
                         ? open_stand_in(parent, subkey, access, result, nullptr)
                         : ERROR_FILE_NOT_FOUND;
    bool read_only = false;
    if (status != ERROR_SUCCESS) {
        status = g_open_key(parent, subkey, options, access, result);
        read_only = retry_read_only(status, access, parent, subkey);
        if (read_only)
            status = g_open_key(parent, subkey, options, read_only_access(access), result);
        // one of a PlayOnline install's keys, named relative to a stand-in, that it doesn't have yet
        if (status == ERROR_FILE_NOT_FOUND)
            status = open_stand_in(parent, subkey, access, result, nullptr);
    }
    if (tracing())
        trace_open(_ReturnAddress(), L"RegOpenKeyExA", parent, subkey, status, result, read_only);
    return status;
}

LSTATUS APIENTRY create_key(HKEY parent, LPCSTR subkey, DWORD reserved, LPSTR key_class, DWORD options,
                            REGSAM access, const LPSECURITY_ATTRIBUTES security, PHKEY result, LPDWORD disposition)
{
    // A PlayOnline install's keys are always the stand-ins (except Interface, which resolves to real
    // HKLM read-only even when created/opened relative to a stand-in PlayOnline* parent handle).
    LSTATUS status = (names_playonline(subkey) || policy::is_interface_subkey(subkey))
                         ? open_stand_in(parent, subkey, access, result, disposition)
                         : ERROR_FILE_NOT_FOUND;
    bool read_only = false;
    if (status != ERROR_SUCCESS) {
        status = g_create_key(parent, subkey, reserved, key_class, options, access, security, result, disposition);
        // Without admin rights HKLM refuses to make a key whatever access is asked for, so a read-only
        // create of a missing key is denied too: opening it is all that's left.
        read_only = retry_read_only(status, access, parent, subkey) ||
                    (status == ERROR_ACCESS_DENIED && mentions_playonline(describe(parent) + L"\\" + widen(subkey)));
        if (read_only) {
            // Opening only: creating a key under HKLM needs the rights the game doesn't have.
            const auto open = g_open_key != nullptr ? g_open_key : &RegOpenKeyExA;
            status = open(parent, subkey, 0, read_only_access(access), result);
            if (status == ERROR_SUCCESS && disposition != nullptr)
                *disposition = REG_OPENED_EXISTING_KEY;
        }
    }
    if (tracing())
        trace_open(_ReturnAddress(), L"RegCreateKeyExA", parent, subkey, status, result, read_only);
    return status;
}

// These two only trace, to show how the game reads its settings.

LSTATUS APIENTRY enum_value(HKEY key, DWORD index, LPSTR name, LPDWORD name_size, LPDWORD reserved, LPDWORD type,
                            LPBYTE data, LPDWORD data_size)
{
    const LSTATUS status = g_enum_value(key, index, name, name_size, reserved, type, data, data_size);
    if (tracing()) {
        const std::wstring path = key_path(key);
        if (mentions_playonline(path))
            trace(_ReturnAddress(), L"RegEnumValueA %ls #%lu = %ld %hs", path.c_str(), index, status,
                  status == ERROR_SUCCESS && name != nullptr ? name : "");
    }
    return status;
}

LSTATUS APIENTRY query_info(HKEY key, LPSTR key_class, LPDWORD class_size, LPDWORD reserved, LPDWORD subkeys,
                            LPDWORD max_subkey, LPDWORD max_class, LPDWORD values, LPDWORD max_value_name,
                            LPDWORD max_value, LPDWORD security, PFILETIME last_write)
{
    const LSTATUS status = g_query_info(key, key_class, class_size, reserved, subkeys, max_subkey, max_class, values,
                                        max_value_name, max_value, security, last_write);
    if (tracing()) {
        const std::wstring path = key_path(key);
        if (mentions_playonline(path))
            trace(_ReturnAddress(), L"RegQueryInfoKeyA %ls = %ld", path.c_str(), status);
    }
    return status;
}

// The five registry calls VanaCore stands between the game and the registry on: the hook each
// goes to, and where it goes on from there.
struct Hook {
    const char* name;
    void* replacement;
    void** original;
};
const Hook kHooks[] = {
    {"RegQueryValueExA", reinterpret_cast<void*>(&query_value), reinterpret_cast<void**>(&g_query_value)},
    {"RegOpenKeyExA", reinterpret_cast<void*>(&open_key), reinterpret_cast<void**>(&g_open_key)},
    {"RegCreateKeyExA", reinterpret_cast<void*>(&create_key), reinterpret_cast<void**>(&g_create_key)},
    {"RegEnumValueA", reinterpret_cast<void*>(&enum_value), reinterpret_cast<void**>(&g_enum_value)},
    {"RegQueryInfoKeyA", reinterpret_cast<void*>(&query_info), reinterpret_cast<void**>(&g_query_info)},
};
int g_advapi32_hooked = 0; // of the five, in advapi32's own import table

// Where a hook goes on to: the registry's own code, in kernelbase (Windows 8 and later; kernel32
// on 7), found by name so that no import table VanaCore has hooked is on the way, whichever one
// the call came in through.
void resolve_originals()
{
    for (const Hook& hook : kHooks) {
        if (*hook.original != nullptr)
            continue;
        for (const wchar_t* dll : {L"kernelbase.dll", L"kernel32.dll"}) {
            const HMODULE module = GetModuleHandleW(dll);
            void* function = module != nullptr ? reinterpret_cast<void*>(GetProcAddress(module, hook.name)) : nullptr;
            if (function != nullptr) {
                *hook.original = function;
                break;
            }
        }
    }
}

// advapi32's own imports from the registry API sets: its exported Reg* functions are stubs that
// jump through them, so this catches a call made through advapi32's export however the caller
// found it (GetProcAddress, LuaJIT's ffi, a DLL of Windows'). Each function is looked for in
// every API set: a build may import them from more than one.
void on_advapi32_loaded(HMODULE advapi32)
{
    for (const Hook& hook : kHooks) {
        void* previous = nullptr;
        // An original still unknown is taken from the slot, before the slot is changed
        void** original = *hook.original == nullptr ? hook.original : &previous;
        bool hooked = false;
        for (const char* api_set : kRegistryApiSets)
            hooked = imports::hook(advapi32, api_set, hook.name, hook.replacement, original) || hooked;
        if (hooked)
            ++g_advapi32_hooked;
    }
}

// A module's own imports of the five, from advapi32 or an API set: the game's DLLs (FFXi.dll,
// FFXiMain.dll, polcore.dll), Ashita plugins and VanaCore itself, not Windows' own. A call then
// reaches VanaCore from the caller's import table, whatever advapi32's stubs do on this Windows
// (a build that imports them from another API set, security software's patch on the stubs):
// one player's game found no install folder with advapi32's imports alone hooked (2026-10-10).
void on_module(HMODULE module)
{
    if (imports::is_windows_module(module))
        return;
    for (const Hook& hook : kHooks) {
        void* previous = nullptr;
        // An original still unknown is advapi32's export itself: its import table isn't hooked
        // for this function then (resolve_originals and on_advapi32_loaded both found nothing)
        void** original = *hook.original == nullptr ? hook.original : &previous;
        imports::hook(module, "advapi32.dll", hook.name, hook.replacement, original);
        for (const char* api_set : kRegistryApiSets)
            imports::hook(module, api_set, hook.name, hook.replacement, original);
    }
}

// Whether a call reaches VanaCore: one through VanaCore's own import table, as the game's DLLs
// make theirs, and one through advapi32's export, as a caller that looked the function up makes
// it. open_key counts its calls; HKCU\Software is opened, which no stand-in or trace is for.
void check_hooks(bool& via_import, bool& via_export)
{
    const auto probe = [](decltype(&RegOpenKeyExA) open) {
        const unsigned before = g_open_calls.load();
        HKEY key = nullptr;
        if (open(HKEY_CURRENT_USER, "Software", 0, KEY_READ, &key) == ERROR_SUCCESS)
            RegCloseKey(key);
        return g_open_calls.load() > before;
    };
    via_import = probe(&RegOpenKeyExA);
    via_export = false;
    if (const HMODULE advapi32 = GetModuleHandleW(L"advapi32.dll")) {
        if (const auto open = reinterpret_cast<decltype(&RegOpenKeyExA)>(GetProcAddress(advapi32, "RegOpenKeyExA")))
            via_export = probe(open);
    }
}

} // namespace

void serve(Key key, const char* name, const std::wstring& text)
{
    if (find(key, name) != nullptr)
        return;
    const std::string ansi = narrow(text);
    Value value{key, name, REG_SZ, std::vector<BYTE>(ansi.begin(), ansi.end()), text};
    value.data.push_back(0);
    g_values.push_back(std::move(value));
}

// The settings whose default changed, taken out once of a file that has them at the old default, so the new one applies.
// [client] defaults is how many of these steps the file has had: a value set again after that is the player's, and stays.
// 1: [graphics.effects] sun_near 25 to 50 (2026-11-21: past 25 thin shadows were the far map's coarse ones and popped in
// and out as the sharp map's edge went by with the player walking; the middle map is off by default again)
// 2: [graphics] grass.pathpad 0.25 to unset (2026-11-22: each zone's own now, data\grass_dirt.json, and off the /vana panel)
// 3: [graphics.effects] sun_near 50 to 10 (2026-11-23: sharp close up again; the popping past it to be met another way)
static void reset_old_defaults(const wchar_t* ini)
{
    struct Reset {
        int step;
        const wchar_t* section;
        const wchar_t* key;
        double old;
    };
    constexpr Reset kResets[] = {
        {1, L"graphics.effects", L"sun_near", 25.0},
        {2, L"graphics", L"grass.pathpad", 0.25},
        {3, L"graphics.effects", L"sun_near", 50.0},
    };
    constexpr int kSteps = 3;
    const int had = static_cast<int>(GetPrivateProfileIntW(L"client", L"defaults", 0, ini));
    if (had >= kSteps)
        return;
    for (const Reset& reset : kResets) {
        wchar_t value[64] = {};
        GetPrivateProfileStringW(reset.section, reset.key, L"", value, 64, ini);
        wchar_t* end = nullptr;
        const double number = std::wcstod(value, &end);
        if (reset.step <= had || value[0] == L'\0' || end == value || number != reset.old)
            continue;
        WritePrivateProfileStringW(reset.section, reset.key, nullptr, ini);
        log::write(L"Settings: [%ls] %ls=%ls taken out: its default is new, and it was the old one", reset.section, reset.key, value);
    }
    WritePrivateProfileStringW(L"client", L"defaults", std::to_wstring(kSteps).c_str(), ini);
}

void migrate(const wchar_t* ini)
{
    move_old_keys(ini);
    reset_old_defaults(ini);
}

void install(const wchar_t* ini)
{
    g_retail = client::retail();
    read_settings(ini);

    // renderer=d3d12 has no full screen of its own (VanaCore covers the monitor with the window): the
    // game is given a window even then, as its own full-screen code (staying on top, taking the focus
    // back, owning the keyboard) is for Windows' exclusive full screen
    wchar_t renderer[16] = {};
    GetPrivateProfileStringW(L"graphics", L"renderer", L"d3d8", renderer, 16, ini);
    g_window_always = _wcsicmp(renderer, L"d3d12") == 0 || _wcsicmp(renderer, L"d3d11") == 0;

    // [graphics] background=<width>x<height>: the 3D scene's size, in place of [ffxi] resolution.background (the
    // launcher's); VanaCore's renderers alone, which draw it at that size as the game runs (graphics.cpp)
    wchar_t background[32] = {};
    GetPrivateProfileStringW(L"graphics", L"background", L"", background, 32, ini);
    if (g_window_always) {
        // DirectX 11 and 12: VanaCore sizes the 3D scene, apart from the launcher. A size is used as set; anything
        // else (empty, the old "launcher", or junk) defaults to 3072x3072, so the scene never follows [ffxi]
        // resolution.background (which the launcher sets).
        long width = 0, height = 0;
        if (!parse_background(background, width, height)) {
            if (background[0] != L'\0')
                log::write(L"Graphics: background=%ls isn't a size from 512x512 to 8192x8192; 3072x3072", background);
            width = height = 3072;
        } else if (width == 0) {
            width = height = 3072;
        }
        g_background[0] = static_cast<DWORD>(width), g_background[1] = static_cast<DWORD>(height);
        log::write(L"Graphics: the 3D scene drawn at %ldx%ld ([graphics] background), apart from [ffxi] "
                   L"resolution.background",
                   width, height);
    }

    // [language] playonline: the game's text language. PlayOnline keeps it in its own settings key
    // (Language: 0 Japanese, 1 English), which the bootloader reads and passes on to the game.
    const UINT language = GetPrivateProfileIntW(L"language", L"playonline", 0, ini);
    if (language == 1 || language == 2) {
        const DWORD registry_value = language == 1 ? 0 : 1;
        const wchar_t* name = language == 1 ? L"Japanese" : L"English";
        Value value{Key::PolSettings, "Language", REG_DWORD,
                    std::vector<BYTE>(reinterpret_cast<const BYTE*>(&registry_value),
                                      reinterpret_cast<const BYTE*>(&registry_value + 1)),
                    std::to_wstring(registry_value) + L" (" + name + L")"};
        g_values.push_back(std::move(value));
        log::write(L"Language: the game's text in %ls ([language] playonline=%u)", name, language);
    } else if (language != 0) {
        log::write(L"Language: [language] playonline=%u isn't 1 (Japanese) or 2 (English); the game's own setting "
                   L"stands",
                   language);
    }

    g_logged = std::make_unique<std::atomic<bool>[]>(g_values.size());
    g_query_key = reinterpret_cast<NtQueryKey>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryKey"));

    // The registry calls, two ways: each DLL's own import table, and advapi32's. The originals
    // first, so a hooked slot never leads to a hook that has nowhere to go on to.
    resolve_originals();
    const bool advapi32 = imports::when_loaded(L"advapi32.dll", &on_advapi32_loaded);
    const bool modules = imports::for_each_module(&on_module);
    bool via_import = false, via_export = false;
    check_hooks(via_import, via_export);
    log::write(L"Hooked the registry calls: [ffxi] and [ffxi.registry] apply to every DLL in the game process (each "
               L"DLL's import table%ls, and %d of 5 in advapi32's); a call through a DLL's import table %ls VanaCore, "
               L"one through advapi32's export %ls",
               modules ? L"" : L": could not watch DLLs load, so those loaded already", advapi32 ? g_advapi32_hooked : 0,
               via_import ? L"reaches" : L"MISSES", via_export ? L"reaches" : L"misses");
    if (!via_import)
        log::write(L"Registry: the game's own registry calls may not reach VanaCore: [ffxi] and [ffxi.registry] may be "
                   L"ignored, and FFXI may not find its install folder");
}

int window_mode()
{
    return g_window_mode;
}

std::wstring setting(const char* name)
{
    if (name != nullptr && _stricmp(name, "0034") == 0 && g_forced_mode >= 0)
        return std::to_wstring(g_forced_mode);
    const Value* value = find(Key::Settings, name);
    if (value == nullptr)
        return {};
    return value->type == REG_DWORD ? number_text(value) : value->text;
}

bool registry_overrides(const char* name)
{
    const Value* value = find(Key::Settings, name);
    return g_registry_first && value != nullptr && value->from == L"[ffxi.registry]";
}

} // namespace vanacore::registry
