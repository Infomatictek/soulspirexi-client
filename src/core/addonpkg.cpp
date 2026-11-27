#include "addonpkg.hpp"

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <map>
#include <set>
#include <string_view>
#include <vector>

#include <vanacore/version.hpp>

#include "json.hpp"
#include "log.hpp"
#include "zipread.hpp"

namespace vanacore::addonpkg {

void register_association(const std::wstring& install_dir); // addonpkg_win.cpp

namespace {

// publish_r2.py's limits (CatsAndBoats/cexi-addons): what the launcher publishes, a package may hold
constexpr std::uint32_t kMaxFile = 32u * 1024 * 1024;
constexpr std::uint64_t kMaxUnpacked = 256ull * 1024 * 1024;
constexpr std::size_t kMaxPackage = 256u * 1024 * 1024;
constexpr std::size_t kMaxEntries = 5000;
constexpr std::uint32_t kMaxManifest = 64 * 1024;
constexpr int kBackupsKept = 3;
constexpr wchar_t kMarker[] = L".vcaddon.json";

// The file types addons ship (publish_r2.py's ALLOWED_EXTENSIONS): nothing a program could be
constexpr const char* kExtensions[] = {
    "lua", "json", "xml", "xaml", "txt", "md", "csv", "ini", "cfg", "yml", "yaml", "tbl", "addon", "png", "jpg", "jpeg",
    "gif", "bmp", "ico", "ttf", "otf", "wav", "ogg", "mp3", "html", "htm", "css",
};
constexpr const char* kBareNames[] = {"license", "licence", "copying", "notice", "authors", "contributors", "changelog",
                                      "readme", "version"};
// What a zip made by Finder or Explorer carries beside the files: left out, not refused
constexpr const char* kJunk[] = {"__macosx", ".ds_store", "thumbs.db", "desktop.ini"};
// VanaCore's own addons (addons\vanacore) and SoulspireXI's protected core addon: never overwritten by packages
constexpr const char* kReserved[] = {"vana", "keybinds", "freecam", "mcp", "hello", "libs", "apiexplorer", "plates",
                                     "sendkey", "minparty", "loadscreen", "vanastore", "soulspire_core"};
constexpr const char* kDevices[] = {"con", "prn", "aux", "nul", "com1", "com2", "com3", "com4", "com5", "com6", "com7",
                                    "com8", "com9", "lpt1", "lpt2", "lpt3", "lpt4", "lpt5", "lpt6", "lpt7", "lpt8", "lpt9"};

template <size_t N>
bool listed(const char* const (&list)[N], std::string_view word)
{
    return std::any_of(std::begin(list), std::end(list), [&](const char* each) { return word == each; });
}

std::string lower(std::string_view text)
{
    std::string out(text);
    for (char& c : out)
        if (c >= 'A' && c <= 'Z')
            c = static_cast<char>(c - 'A' + 'a');
    return out;
}

std::wstring folder(std::wstring dir)
{
    while (!dir.empty() && (dir.back() == L'\\' || dir.back() == L'/'))
        dir.pop_back();
    return dir;
}

bool is_dir(const std::wstring& path)
{
    const DWORD a = GetFileAttributesW(path.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

bool exists(const std::wstring& path)
{
    return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

bool read_small(const std::wstring& path, std::string& out, DWORD most)
{
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return false;
    out.assign(most, '\0');
    DWORD got = 0;
    const bool ok = ReadFile(file, out.data(), most, &got, nullptr) != 0;
    CloseHandle(file);
    out.resize(ok ? got : 0);
    return ok;
}

bool write_new(const std::wstring& path, const std::string& data, DWORD how = CREATE_NEW)
{
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, how, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return false;
    DWORD written = 0;
    const bool ok = data.empty() || (WriteFile(file, data.data(), static_cast<DWORD>(data.size()), &written, nullptr) != 0 &&
                                     written == data.size());
    CloseHandle(file);
    if (!ok)
        DeleteFileW(path.c_str());
    return ok;
}

// Every folder of `path` made, its parents first
bool make_dirs(const std::wstring& path)
{
    if (path.empty() || is_dir(path))
        return true;
    const size_t cut = path.find_last_of(L'\\');
    if (cut != std::wstring::npos && cut > 2 && !make_dirs(path.substr(0, cut)))
        return false;
    return CreateDirectoryW(path.c_str(), nullptr) != 0 || GetLastError() == ERROR_ALREADY_EXISTS;
}

// A folder and what's in it gone. A link (a junction) inside is removed itself, never followed.
bool remove_tree(const std::wstring& path)
{
    WIN32_FIND_DATAW found{};
    HANDLE find = FindFirstFileW((path + L"\\*").c_str(), &found);
    if (find != INVALID_HANDLE_VALUE) {
        do {
            const std::wstring name = found.cFileName;
            if (name == L"." || name == L"..")
                continue;
            const std::wstring full = path + L"\\" + name;
            SetFileAttributesW(full.c_str(), FILE_ATTRIBUTE_NORMAL);
            const bool dir = (found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
            const bool link = (found.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
            if (dir && !link)
                remove_tree(full);
            else if (dir)
                RemoveDirectoryW(full.c_str());
            else
                DeleteFileW(full.c_str());
        } while (FindNextFileW(find, &found));
        FindClose(find);
    }
    return RemoveDirectoryW(path.c_str()) != 0 || !exists(path);
}

// Valid UTF-8, printable: what a manifest or a Lua header says, for a dialog. Bytes that aren't UTF-8 become '?', control
// characters go (a line break stays where `lines`), and it's cut at `most` bytes.
std::string clean(std::string_view text, size_t most, bool lines = false)
{
    std::string out;
    for (size_t i = 0; i < text.size() && out.size() < most;) {
        const auto c = static_cast<unsigned char>(text[i]);
        size_t n = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 0;
        for (size_t k = 1; n != 0 && k < n; ++k)
            if (i + k >= text.size() || (static_cast<unsigned char>(text[i + k]) & 0xC0) != 0x80)
                n = 0;
        if (n == 0) {
            out += '?';
            ++i;
        } else if (n == 1 && (c < 0x20 || c == 0x7F)) {
            if (lines && c == '\n')
                out += '\n';
            else if (c == '\t')
                out += ' ';
            ++i;
        } else {
            out.append(text, i, n);
            i += n;
        }
    }
    while (!out.empty() && (out.back() == ' ' || out.back() == '\n'))
        out.pop_back();
    const size_t first = out.find_first_not_of(" \n");
    return first == std::string::npos ? std::string() : out.substr(first);
}

bool valid_name(std::string_view name)
{
    if (name.empty() || name.size() > 64 || name.front() == '.')
        return false;
    for (const char c : name)
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.'))
            return false;
    return name.find("..") == std::string_view::npos;
}

// Version numbers compared a dotted part at a time ("0.8.254" < "0.10"); what isn't a number counts as 0
int compare_versions(std::string_view a, std::string_view b)
{
    size_t i = 0, j = 0;
    const auto part = [](std::string_view text, size_t& at) {
        long n = 0;
        for (; at < text.size() && text[at] != '.'; ++at)
            if (text[at] >= '0' && text[at] <= '9' && n < 100000000)
                n = n * 10 + (text[at] - '0');
        ++at; // past the dot
        return n;
    };
    while (i < a.size() || j < b.size()) {
        const long x = part(a, i);
        const long y = part(b, j);
        if (x != y)
            return x < y ? -1 : 1;
    }
    return 0;
}

std::vector<std::string_view> split_path(std::string_view name)
{
    std::vector<std::string_view> parts;
    size_t start = 0;
    while (start <= name.size()) {
        const size_t slash = name.find('/', start);
        const size_t end = slash == std::string_view::npos ? name.size() : slash;
        parts.push_back(name.substr(start, end - start));
        if (slash == std::string_view::npos)
            break;
        start = slash + 1;
    }
    if (parts.size() > 1 && parts.back().empty())
        parts.pop_back(); // a directory's trailing '/'
    return parts;
}

bool junk(const zipread::Entry& e)
{
    for (const std::string_view part : split_path(e.name))
        if (listed(kJunk, lower(part)))
            return true;
    return split_path(e.name).back().starts_with("._"); // a Mac's resource fork beside a file
}

// Why one part of an entry's path can't be a file or folder on Windows, or empty
std::string part_problem(std::string_view part)
{
    if (part.empty())
        return "has an empty folder name in its path";
    if (part == "." || part == "..")
        return "reaches outside the addon's folder";
    if (part.back() == '.' || part.back() == ' ' || part.front() == ' ')
        return "has a name Windows can't hold";
    const std::string stem = lower(part.substr(0, part.find('.')));
    if (listed(kDevices, stem))
        return "is named for a Windows device (" + stem + ")";
    return {};
}

// Why an entry can't be unpacked into addons\vanacore\<name>\, or empty
std::string entry_problem(const zipread::Entry& e, const std::string& name)
{
    const std::string& n = e.name;
    if (e.symlink())
        return "is a symbolic link";
    if ((e.flags & 1) != 0)
        return "is encrypted";
    if (n.empty() || n.size() > 240)
        return "has a name too long for Windows";
    if (n.front() == '/' || (n.size() > 1 && n[1] == ':'))
        return "is an absolute path";
    for (const char c : n) {
        const auto u = static_cast<unsigned char>(c);
        if (u < 0x20 || u == 0x7F || std::strchr("\\:*?\"<>|", c) != nullptr)
            return "has a character Windows can't hold in a name";
        if (u >= 0x80 && (e.flags & 0x800) == 0)
            return "has a name that isn't UTF-8";
    }
    const std::vector<std::string_view> parts = split_path(n);
    for (const std::string_view part : parts)
        if (std::string why = part_problem(part); !why.empty())
            return why;
    if (parts.front() != name || (parts.size() < 2 && !e.directory()))
        return "is outside the addon's folder (" + name + "\\)";
    if (!e.directory()) {
        const std::string file = lower(parts.back());
        const size_t dot = file.rfind('.');
        const bool ok = dot == std::string::npos ? listed(kBareNames, file) : listed(kExtensions, file.substr(dot + 1));
        if (!ok)
            return "isn't a kind of file addons ship (" + (dot == std::string::npos ? file : file.substr(dot)) + ")";
        if (e.size > kMaxFile)
            return "is " + std::to_string(e.size / (1024 * 1024)) + " MB, over the 32 MB a file may be";
    }
    return {};
}

// `key = 'value'` in a Lua file's first lines (addon.name, addon.version, ...): the value, or empty
std::string lua_field(const std::string& lua, const char* key)
{
    const std::string want = std::string("addon.") + key;
    for (size_t at = lua.find(want); at != std::string::npos; at = lua.find(want, at + 1)) {
        if (at > 0 && lua[at - 1] != '\n' && lua[at - 1] != ' ' && lua[at - 1] != '\t')
            continue;
        size_t i = at + want.size();
        while (i < lua.size() && (lua[i] == ' ' || lua[i] == '\t'))
            ++i;
        if (i >= lua.size() || lua[i] != '=')
            continue;
        ++i;
        while (i < lua.size() && (lua[i] == ' ' || lua[i] == '\t'))
            ++i;
        if (i >= lua.size() || (lua[i] != '\'' && lua[i] != '"'))
            continue;
        const size_t end = lua.find(lua[i], i + 1);
        if (end != std::string::npos && lua.find('\n', i) > end)
            return lua.substr(i + 1, end - i - 1);
    }
    return {};
}

const zipread::Entry* entry_named(const zipread::Archive& a, std::string_view name)
{
    for (const zipread::Entry& e : a.entries)
        if (e.name == name)
            return &e;
    return nullptr;
}

// The addon's folder when there's no addon.json: the first part of every entry's path, if they all share one
std::string only_folder(const zipread::Archive& a)
{
    std::string name;
    for (const zipread::Entry& e : a.entries) {
        if (junk(e))
            continue;
        const std::string first(split_path(e.name).front());
        if (name.empty())
            name = first;
        else if (first != name)
            return {};
    }
    return name;
}

// addon.json's fields into `out`. False with out.problem.
bool read_manifest(const zipread::Archive& a, const zipread::Entry& entry, Package& out)
{
    std::string text, error;
    json::Value m;
    if (entry.size > kMaxManifest || !zipread::extract(a, entry, text, error) || !json::parse(text, m, error) || !m.is_object()) {
        out.problem = "Its addon.json can't be read" + (error.empty() ? std::string() : ": " + error);
        return false;
    }
    if (m.get_number("format", 1) != 1) {
        out.problem = "It's a newer kind of package than this VanaCore reads: update VanaCore";
        return false;
    }
    if (const std::string kind = m.get_string("kind", "vanacore"); kind != "vanacore") {
        out.problem = "It holds a " + clean(kind, 20) + " addon: packages hold VanaCore addons only";
        return false;
    }
    out.name = m.get_string("name");
    out.version = clean(m.get_string("version"), 32);
    out.title = clean(m.get_string("title"), 64);
    out.author = clean(m.get_string("author"), 64);
    out.description = clean(m.get_string("description"), 1200, true);
    out.homepage = clean(m.get_string("homepage"), 200);
    out.min_vanacore = clean(m.get_string("min_vanacore"), 32);
    if (!out.homepage.starts_with("https://") && !out.homepage.starts_with("http://"))
        out.homepage.clear();
    return true;
}

// The details addon.json left out, from the addon's Lua (what /vana's Addons page shows of it)
void fill_from_lua(const zipread::Archive& a, Package& out)
{
    const zipread::Entry* main = entry_named(a, out.name + "/" + out.name + ".lua");
    std::string lua, error;
    if (main == nullptr || main->size > kMaxFile || !zipread::extract(a, *main, lua, error))
        return;
    lua.resize(std::min<size_t>(lua.size(), 64 * 1024));
    if (out.version.empty())
        out.version = clean(lua_field(lua, "version"), 32);
    if (out.author.empty())
        out.author = clean(lua_field(lua, "author"), 64);
    if (out.description.empty())
        out.description = clean(lua_field(lua, "desc"), 1200, true);
    if (out.homepage.empty()) {
        const std::string link = clean(lua_field(lua, "link"), 200);
        if (link.starts_with("https://") || link.starts_with("http://"))
            out.homepage = link;
    }
}

// Every entry checked, and counted. False with out.problem.
bool check_entries(const zipread::Archive& a, Package& out)
{
    std::set<std::string> seen;
    bool has_main = false;
    for (const zipread::Entry& e : a.entries) {
        if (e.name == "addon.json" || junk(e))
            continue;
        if (std::string why = entry_problem(e, out.name); !why.empty()) {
            out.problem = "It can't be installed safely: " + clean(e.name, 120) + " " + why;
            return false;
        }
        if (!seen.insert(lower(e.directory() ? e.name.substr(0, e.name.size() - 1) : e.name)).second) {
            out.problem = "It can't be installed safely: " + clean(e.name, 120) + " is in it twice (Windows' names ignore case)";
            return false;
        }
        if (e.directory())
            continue;
        has_main = has_main || e.name == out.name + "/" + out.name + ".lua";
        out.files += 1;
        out.bytes += e.size;
    }
    if (out.bytes > kMaxUnpacked) {
        out.problem = "It unpacks to " + std::to_string(out.bytes / (1024 * 1024)) + " MB, over the 256 MB an addon may be";
        return false;
    }
    if (!has_main) {
        out.problem = "It has no " + out.name + "\\" + out.name + ".lua, the file VanaCore loads";
        return false;
    }
    return true;
}

std::wstring addon_dir(const std::wstring& install_dir, const std::string& name)
{
    return folder(install_dir) + L"\\addons\\vanacore\\" + wide(name);
}

// What's there already: installed_version, and the problem when a package can't go there
void check_here(const std::wstring& install_dir, Package& out)
{
    const std::string shown = out.title.empty() ? out.name : out.title;
    if (listed(kReserved, lower(out.name))) {
        out.problem = out.name + " is one of VanaCore's own addons: a package can't replace it";
        return;
    }
    if (!out.min_vanacore.empty() && compare_versions(utf8(kVersion), out.min_vanacore) < 0) {
        out.problem = shown + " needs VanaCore " + out.min_vanacore + " or newer; this is " + utf8(kVersion);
        return;
    }
    const std::wstring dir = addon_dir(install_dir, out.name);
    if (!exists(dir))
        return;
    std::string text, error;
    json::Value marker;
    if (!read_small(dir + L"\\" + kMarker, text, kMaxManifest) || !json::parse(text, marker, error)) {
        out.installed_version = "?";
        out.problem = "An addon named " + out.name + " is already installed, and not from a package (the launcher keeps it "
                      "up to date, or it was copied in by hand), so a package can't replace it";
        return;
    }
    out.installed_version = clean(marker.get_string("version", "?"), 32);
    if (out.installed_version.empty())
        out.installed_version = "?";
}

// The URL the file was downloaded from (Windows' mark on it), else empty
std::string downloaded_from(const std::wstring& path)
{
    std::string zone;
    if (!read_small(path + L":Zone.Identifier", zone, 8192))
        return {};
    for (const char* key : {"HostUrl=", "ReferrerUrl="}) {
        const size_t at = zone.find(key);
        if (at == std::string::npos)
            continue;
        std::string url = zone.substr(at + std::strlen(key));
        url.resize(std::min(url.size(), url.find_first_of("\r\n")));
        if (url.starts_with("http"))
            return clean(url, 300);
    }
    return {};
}

std::string origin(const std::wstring& path)
{
    std::string from;
    if (read_small(path + L".from", from, 2048) && !clean(from, 600).empty())
        return clean(from, 600);
    from = downloaded_from(path);
    return from.empty() ? utf8(path) : from;
}

// The package read and checked (a.k.a. inspect, keeping the archive for install)
bool read_package(const std::wstring& install_dir, const std::wstring& path, Package& out, zipread::Archive& a)
{
    out = {};
    out.path = path;
    out.from = origin(path);
    std::string error;
    if (!zipread::open(path, kMaxPackage, kMaxEntries, a, error)) {
        out.problem = "It isn't an addon package: " + error;
        return false;
    }
    const zipread::Entry* manifest = entry_named(a, "addon.json");
    if (manifest != nullptr) {
        if (!read_manifest(a, *manifest, out))
            return false;
    } else {
        out.name = only_folder(a); // a zip of the addon's folder, renamed
    }
    if (!valid_name(out.name)) {
        out.problem = out.name.empty() ? "It isn't an addon package: it holds no addon.json and no single addon folder"
                                       : "Its addon name, " + clean(out.name, 64) + ", isn't one VanaCore can load";
        return false;
    }
    if (!check_entries(a, out))
        return true; // a package, which can't be installed: its name and details show with why
    fill_from_lua(a, out);
    check_here(install_dir, out);
    return true;
}

std::string timestamp(const char* format)
{
    SYSTEMTIME t{};
    GetLocalTime(&t);
    char text[40];
    std::snprintf(text, sizeof text, format, t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    return text;
}

// Every entry written under `staging` (which isn't there yet), then the marker. False with `error`.
bool unpack(const zipread::Archive& a, const Package& p, const std::wstring& staging, std::string& error)
{
    if (!make_dirs(staging)) {
        error = "its folder can't be made (error " + std::to_string(GetLastError()) + ")";
        return false;
    }
    for (const zipread::Entry& e : a.entries) {
        if (e.name == "addon.json" || junk(e))
            continue;
        std::wstring rest = wide(e.name.substr(p.name.size()));
        std::replace(rest.begin(), rest.end(), L'/', L'\\');
        while (!rest.empty() && rest.back() == L'\\')
            rest.pop_back();
        const std::wstring target = staging + rest;
        std::string data;
        if (e.directory()) {
            if (!make_dirs(target)) {
                error = "a folder can't be made: " + e.name;
                return false;
            }
            continue;
        }
        if (!make_dirs(target.substr(0, target.find_last_of(L'\\'))) || !zipread::extract(a, e, data, error) ||
            !write_new(target, data)) {
            error = error.empty() ? "a file can't be written: " + e.name : error;
            return false;
        }
    }
    json::Value marker = json::Value::make_object();
    marker["name"] = p.name;
    marker["version"] = p.version;
    marker["title"] = p.title;
    marker["author"] = p.author;
    marker["from"] = p.from;
    marker["installed"] = timestamp("%04u-%02u-%02u %02u:%02u:%02u");
    if (!write_new(staging + L"\\" + kMarker, json::dump(marker))) {
        error = "its .vcaddon.json can't be written";
        return false;
    }
    return true;
}

// The newest kBackupsKept of an addon's backups kept, the older ones removed
void prune_backups(const std::wstring& backups, const std::string& name)
{
    std::vector<std::pair<ULONGLONG, std::wstring>> found;
    WIN32_FIND_DATAW f{};
    HANDLE find = FindFirstFileW((backups + L"\\" + wide(name) + L" *").c_str(), &f);
    if (find == INVALID_HANDLE_VALUE)
        return;
    do {
        if ((f.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
            found.emplace_back((static_cast<ULONGLONG>(f.ftCreationTime.dwHighDateTime) << 32) | f.ftCreationTime.dwLowDateTime,
                               f.cFileName);
    } while (FindNextFileW(find, &f));
    FindClose(find);
    std::sort(found.begin(), found.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    for (size_t i = kBackupsKept; i < found.size(); ++i)
        remove_tree(backups + L"\\" + found[i].second);
}

std::string safe_version(const std::string& version)
{
    std::string out;
    for (const char c : version)
        if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '.' || c == '-' || c == '_')
            out += c;
    return out.empty() ? "unknown" : out;
}

// The unpacked folder put in the addon's place, the old one (if any) moved to backups first. False with `error`.
bool swap_in(const std::wstring& install_dir, const Package& p, const std::wstring& staging, std::string& error)
{
    const std::wstring target = addon_dir(install_dir, p.name);
    const std::wstring backups = folder(install_dir) + L"\\backups\\addons";
    std::wstring backup;
    if (exists(target)) {
        const std::wstring stem = backups + L"\\" + wide(p.name + " " + safe_version(p.installed_version) + " (" +
                                                       timestamp("%04u-%02u-%02u %02u.%02u.%02u") + ")");
        backup = stem;
        for (int n = 2; exists(backup) && n < 100; ++n)
            backup = stem + L" " + std::to_wstring(n);
        if (!make_dirs(backups) || !MoveFileExW(target.c_str(), backup.c_str(), 0)) {
            error = "the installed " + p.name + " can't be moved aside (a file in it is open?)";
            return false;
        }
    }
    if (!MoveFileExW(staging.c_str(), target.c_str(), 0)) {
        error = "it can't be put in place (error " + std::to_string(GetLastError()) + ")";
        if (!backup.empty())
            MoveFileExW(backup.c_str(), target.c_str(), 0);
        return false;
    }
    if (!backup.empty())
        prune_backups(backups, p.name);
    return true;
}

// The claims on packages in the inbox this game holds: their lock files, open with no sharing
std::map<std::wstring, HANDLE> g_claims;
std::map<std::wstring, std::pair<ULONGLONG, ULONGLONG>> g_sizes; // a file seen: its size, and since when
std::wstring g_install;
HANDLE g_running = nullptr;
ULONGLONG g_next_look = 0;

std::wstring running_name(const std::wstring& install_dir)
{
    std::wstring dir = folder(install_dir);
    CharLowerBuffW(dir.data(), static_cast<DWORD>(dir.size()));
    unsigned long long h = 1469598103934665603ull;
    for (const wchar_t c : dir)
        h = (h ^ c) * 1099511628211ull;
    wchar_t name[64];
    std::swprintf(name, std::size(name), L"Local\\VanaCore.Game.%016llx", h);
    return name;
}

// A package in the inbox, and whether its size has held for a second (not still being copied in)
bool settled(const std::wstring& path, ULONGLONG size, ULONGLONG now)
{
    auto it = g_sizes.find(path);
    if (it == g_sizes.end() || it->second.first != size) {
        g_sizes[path] = {size, now};
        return false;
    }
    if (now - it->second.second < 1000)
        return false;
    // and nobody writing it: a copy still open for writing keeps a read without write sharing out
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return false;
    CloseHandle(file);
    return true;
}

bool claim(const std::wstring& path)
{
    HANDLE lock = CreateFileW((path + L".lock").c_str(), GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS,
                              FILE_ATTRIBUTE_HIDDEN | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
    if (lock == INVALID_HANDLE_VALUE)
        return false; // another game has it
    g_claims[path] = lock;
    return true;
}

const char kInboxReadme[] =
    "Drop an addon package (a .vcaddon file) in this folder while the game runs, or before you start it:\r\n"
    "VanaCore shows what's in it and asks whether to install it.\r\n"
    "\r\n"
    "Packages it has offered move to the done folder here.\r\n";

} // namespace

std::string utf8(const std::wstring& text)
{
    if (text.empty())
        return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(std::max(n, 0)), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), n, nullptr, nullptr);
    return out;
}

std::wstring wide(const std::string& text)
{
    if (text.empty())
        return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring out(static_cast<size_t>(std::max(n, 0)), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), n);
    return out;
}

bool inspect(const std::wstring& install_dir, const std::wstring& path, Package& out)
{
    zipread::Archive a;
    return read_package(install_dir, path, out, a);
}

bool install(const std::wstring& install_dir, const Package& package, std::string& error)
{
    Package p;
    zipread::Archive a;
    if (!read_package(install_dir, package.path, p, a) || !p.problem.empty() || p.name != package.name) {
        error = p.problem.empty() ? "the package changed since it was read" : p.problem;
        return false;
    }
    const std::wstring staging = folder(install_dir) + L"\\addons\\vanacore\\." + wide(p.name) + L".installing";
    if (exists(staging))
        remove_tree(staging);
    const bool ok = unpack(a, p, staging, error) && swap_in(install_dir, p, staging, error);
    if (!ok)
        remove_tree(staging);
    log::write(L"Addon packages: %ls %hs %hs from %hs%hs%hs", ok ? L"installed" : L"couldn't install", p.name.c_str(),
               p.version.c_str(), p.from.c_str(), ok ? "" : ": ", ok ? "" : error.c_str());
    return ok;
}

bool load_at_start(const std::wstring& install_dir, const std::string& name)
{
    const std::wstring scripts = folder(install_dir) + L"\\scripts";
    const std::wstring path = scripts + L"\\boot.txt";
    std::string text;
    read_small(path, text, 1024 * 1024);
    const std::string want = lower(name);
    size_t start = 0;
    while (start < text.size()) {
        size_t end = text.find('\n', start);
        end = end == std::string::npos ? text.size() : end;
        std::string line = lower(text.substr(start, end - start));
        start = end + 1;
        line.erase(0, line.find_first_not_of(" \t"));
        if (!line.starts_with("/addon load ") && !line.starts_with("//addon load "))
            continue;
        line += ' ';
        for (size_t at = line.find(" load ") + 6; at < line.size();) {
            const size_t next = line.find_first_of(" \t\r", at);
            if (line.substr(at, next - at) == want)
                return true; // loaded by a line already
            at = line.find_first_not_of(" \t\r", next);
        }
    }
    const std::string eol = text.find("\r\n") != std::string::npos || text.empty() ? "\r\n" : "\n";
    if (!text.empty() && text.back() != '\n')
        text += eol;
    text += "/addon load " + name + " vanacore" + eol;
    make_dirs(scripts);
    const std::wstring temp = path + L".tmp";
    return write_new(temp, text, CREATE_ALWAYS) && MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING);
}

std::wstring inbox(const std::wstring& install_dir)
{
    return folder(install_dir) + L"\\addons\\install";
}

bool queue(const std::wstring& install_dir, const std::wstring& path, std::string& error)
{
    const std::wstring dir = inbox(install_dir);
    make_dirs(dir);
    std::wstring file = path.substr(path.find_last_of(L"\\/") + 1);
    const size_t dot = file.rfind(L'.');
    const std::wstring stem = dot == std::wstring::npos ? file : file.substr(0, dot);
    std::wstring target = dir + L"\\" + stem + L".vcaddon";
    for (int n = 2; exists(target) && n < 100; ++n)
        target = dir + L"\\" + stem + L" (" + std::to_wstring(n) + L").vcaddon";
    write_new(target + L".from", origin(path), CREATE_ALWAYS);
    const std::wstring part = target + L".part";
    if (!CopyFileW(path.c_str(), part.c_str(), FALSE) || !MoveFileExW(part.c_str(), target.c_str(), 0)) {
        error = "it can't be copied into " + utf8(dir) + " (error " + std::to_string(GetLastError()) + ")";
        DeleteFileW(part.c_str());
        DeleteFileW((target + L".from").c_str());
        return false;
    }
    return true;
}

void start(const wchar_t* install_dir)
{
    g_install = folder(install_dir != nullptr ? install_dir : L"");
    if (g_install.empty())
        return;
    g_running = CreateMutexW(nullptr, FALSE, running_name(g_install).c_str());
    const std::wstring dir = inbox(g_install);
    make_dirs(dir);
    if (!exists(dir + L"\\README.txt"))
        write_new(dir + L"\\README.txt", kInboxReadme);
    register_association(g_install);
}

const std::wstring& home()
{
    return g_install;
}

bool game_running(const std::wstring& install_dir)
{
    HANDLE m = OpenMutexW(SYNCHRONIZE, FALSE, running_name(install_dir).c_str());
    if (m == nullptr)
        return false;
    CloseHandle(m);
    return true;
}

std::wstring next_waiting()
{
    const ULONGLONG now = GetTickCount64();
    if (g_install.empty() || now < g_next_look)
        return {};
    g_next_look = now + 500;
    const std::wstring dir = inbox(g_install);
    WIN32_FIND_DATAW f{};
    HANDLE find = FindFirstFileW((dir + L"\\*.vcaddon").c_str(), &f);
    if (find == INVALID_HANDLE_VALUE)
        return {};
    std::wstring picked;
    do {
        const std::wstring path = dir + L"\\" + f.cFileName;
        if ((f.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0 || g_claims.count(path) != 0)
            continue;
        const ULONGLONG size = (static_cast<ULONGLONG>(f.nFileSizeHigh) << 32) | f.nFileSizeLow;
        if (settled(path, size, now) && claim(path)) {
            picked = path;
            break;
        }
    } while (FindNextFileW(find, &f));
    FindClose(find);
    return picked;
}

void release(const std::wstring& path)
{
    if (auto it = g_claims.find(path); it != g_claims.end()) {
        CloseHandle(it->second);
        g_claims.erase(it);
    }
    g_sizes.erase(path);
}

void finish(const std::wstring& path)
{
    release(path);
    const std::wstring dir = inbox(g_install);
    if (g_install.empty() || _wcsnicmp(path.c_str(), (dir + L"\\").c_str(), dir.size() + 1) != 0)
        return; // a file picked elsewhere stays where it is
    // kept, not deleted: a file dragged into the folder in Finder or Explorer may have been moved, not copied
    const std::wstring done = dir + L"\\done";
    make_dirs(done);
    MoveFileExW(path.c_str(), (done + path.substr(path.find_last_of(L'\\'))).c_str(), MOVEFILE_REPLACE_EXISTING);
    DeleteFileW((path + L".from").c_str());
}

} // namespace vanacore::addonpkg
