#pragma once

#include <windows.h>

#include <algorithm>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace vanacore::registry::policy {

// The keys a PlayOnline install has under its root (PlayOnline, PlayOnlineUS or PlayOnlineEU), the
// vendor spelled SquareEnix (JP's is SQUARE). Only these are stood in for: any other key is the
// registry's own, so a look for one not there still finds nothing, as it would with PlayOnline installed.
inline constexpr std::wstring_view kStandInKeys[] = {
    L"",
    L"InstallFolder",
    L"Interface",
    L"Product",
    L"SquareEnix",
    L"SquareEnix\\FinalFantasyXI",
    L"SquareEnix\\PlayOnlineViewer",
    L"SquareEnix\\PlayOnlineViewer\\Settings",
    L"SquareEnix\\PlayOnlineViewer\\Settings\\Controler",
    L"SquareEnix\\PlayOnlineViewer\\Settings\\Controller",
    L"SquareEnix\\PlayOnlineViewer\\SystemInfo",
    L"SquareEnix\\PlayOnlineViewer\\SystemInfo\\QCheck",
};

inline wchar_t ascii_lower(wchar_t c)
{
    return (c >= L'A' && c <= L'Z') ? static_cast<wchar_t>(c - L'A' + L'a') : c;
}

inline char ascii_lower(char c)
{
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

inline bool iequals(std::wstring_view a, std::wstring_view b)
{
    if (a.size() != b.size())
        return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (ascii_lower(a[i]) != ascii_lower(b[i]))
            return false;
    return true;
}

inline bool iequals_ascii(std::string_view a, std::string_view b)
{
    if (a.size() != b.size())
        return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (ascii_lower(a[i]) != ascii_lower(b[i]))
            return false;
    return true;
}

// PlayOnline, PlayOnlineUS or PlayOnlineEU (not PlayOnlineViewer).
inline bool is_region_root(std::wstring_view name)
{
    return name.size() <= 12 && iequals(name.substr(0, 10), L"PlayOnline");
}

inline bool is_vendor(std::wstring_view name)
{
    return iequals(name, L"SquareEnix") || iequals(name, L"SQUARE");
}

inline std::vector<std::wstring_view> split_path(std::wstring_view path)
{
    std::vector<std::wstring_view> parts;
    while (!path.empty()) {
        const size_t slash = path.find(L'\\');
        if (slash != 0)
            parts.push_back(path.substr(0, slash));
        if (slash == std::wstring_view::npos)
            break;
        path.remove_prefix(slash + 1);
    }
    return parts;
}

// Where `path` is among the PlayOnline keys: its root (PlayOnlineUS), and what's under it with the
// vendor spelled SquareEnix, if it's a key of kStandInKeys under HKLM\SOFTWARE (as the game or the
// kernel names it, WOW6432Node or not) or already a stand-in under HKCU\Software\VanaCore\Registry.
struct StandInTarget {
    std::wstring root;
    std::wstring subkey;   // as given: "SQUARE\FinalFantasyXI"
    std::wstring key;      // as kStandInKeys: "SquareEnix\FinalFantasyXI"
    bool stand_in = false; // under HKCU\Software\VanaCore\Registry already
};

inline std::optional<StandInTarget> parse_stand_in_place(std::wstring_view path)
{
    const std::vector<std::wstring_view> parts = split_path(path);
    size_t root = 0;
    while (root < parts.size() && !is_region_root(parts[root]))
        ++root;
    if (root == parts.size())
        return std::nullopt;

    const auto at = [&](size_t index, std::wstring_view name) { return index < root && iequals(parts[index], name); };
    size_t next = at(0, L"HKLM") ? 1 : at(0, L"REGISTRY") && at(1, L"MACHINE") ? 2 : 0;
    bool machine = false;
    if (next != 0 && at(next, L"SOFTWARE")) {
        ++next;
        if (at(next, L"WOW6432Node"))
            ++next;
        machine = next == root;
    }
    const bool stand_in = root >= 2 && iequals(parts[root - 2], L"VanaCore") && iequals(parts[root - 1], L"Registry");
    if (!machine && !stand_in)
        return std::nullopt;

    StandInTarget place{std::wstring(parts[root]), {}, {}, stand_in};
    for (size_t i = root + 1; i < parts.size(); ++i) {
        if (i > root + 1) {
            place.subkey += L'\\';
            place.key += L'\\';
        }
        place.subkey += parts[i];
        place.key += i == root + 1 && is_vendor(parts[i]) ? std::wstring(L"SquareEnix") : std::wstring(parts[i]);
    }
    if (std::none_of(std::begin(kStandInKeys), std::end(kStandInKeys),
                     [&](std::wstring_view known) { return iequals(place.key, known); }))
        return std::nullopt;
    return place;
}

// Returns the real HKLM subkey ("SOFTWARE\\PlayOnline[US|EU]\\Interface") whenever `path` targets a
// PlayOnline Interface key — whether opened directly under HKLM (with or without WOW6432Node) or
// opened relatively beneath a VanaCore HKCU stand-in PlayOnline* parent handle.
// Retail PlayOnline encrypts FINAL FANTASY XI\patch.ver with the install-specific key stored in
// HKLM\SOFTWARE\WOW6432Node\PlayOnline*\Interface\0001; shadowing Interface with an empty stand-in
// causes polcore's patch.ver decryption to fail silently before Direct3DCreate8.
inline std::optional<std::wstring> real_interface_subkey(std::wstring_view path)
{
    const std::optional<StandInTarget> place = parse_stand_in_place(path);
    if (!place || !iequals(place->key, L"Interface"))
        return std::nullopt;
    return L"SOFTWARE\\" + place->root + L"\\Interface";
}

// True when a relative subkey name refers directly to the "Interface" child key (ignoring leading/trailing slashes).
inline bool is_interface_subkey(const char* subkey)
{
    if (subkey == nullptr)
        return false;
    std::string_view sv(subkey);
    while (!sv.empty() && sv.front() == '\\')
        sv.remove_prefix(1);
    while (!sv.empty() && sv.back() == '\\')
        sv.remove_suffix(1);
    return iequals_ascii(sv, "Interface");
}

// Read-only access mask for opening real HKLM\SOFTWARE\PlayOnline*\Interface without ever requesting
// write rights. Defaults to the 32-bit registry view (WOW6432Node on 64-bit Windows) unless the caller
// explicitly requested KEY_WOW64_64KEY.
inline constexpr REGSAM interface_read_only_access(REGSAM access)
{
    return (access & KEY_WOW64_64KEY) != 0 ? (KEY_READ | KEY_WOW64_64KEY) : (KEY_READ | KEY_WOW64_32KEY);
}

} // namespace vanacore::registry::policy
