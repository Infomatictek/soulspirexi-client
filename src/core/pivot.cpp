#include "pivot.hpp"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <cwctype>
#include <filesystem>
#include <iterator>
#include <mutex>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "client.hpp"
#include "imports.hpp"
#include "log.hpp"

namespace vanacore::pivot {
namespace {

constexpr int kDepth = 16;     // folder levels listed under an overlay
constexpr size_t kLogged = 20; // files logged as they're served, unless [pivot] log=1 (all of them)
constexpr const wchar_t* kServerOverlays[] = {L"soulspirexi", L"flintlock-akimbo-compact"}; // mandatory server overlays (server_first())
constexpr DWORD kPoll = 500;       // ms between watch()'s looks at the ini
constexpr ULONGLONG kQuiet = 1500; // ms the root folder is left alone before its overlays are listed again
constexpr ULONGLONG kSettle = 300; // ms after the ini changes: the /vana Pivot tab writes [pivot.order] a key at a time

struct Overlay {
    std::wstring name;   // as [pivot.order] has it
    std::wstring folder; // absolute, no trailing backslash
};
struct Entry {
    std::uint16_t overlay; // index into Table::overlays
    bool folder;
};
// The overlays and their files, as the hooks look in them: listed at start, and again into a new table that replaces
// the old one whole whenever they change (watch()).
struct Table {
    std::vector<Overlay> overlays;                 // [pivot.order], lowest number first; catseyexi first
    std::unordered_map<std::wstring, Entry> files; // path in an overlay, lower case -> the first overlay with it
};
using Order = std::vector<std::pair<long, std::wstring>>; // [pivot.order]: number, folder

// What the hooks read is never freed: they run on the game's own threads (its music streaming opens files all along),
// which can outlive VanaCore.Core.dll's static destructors as the process ends. A destroyed file list crashed in
// create_file_a (its bucket array null) on FFXI's sound thread.
template <typename T>
T& forever()
{
    return *new T();
}

// The table in use is never freed; one replaced is, once no hook is in it (they hold the lock shared while they look).
SRWLOCK g_table_lock = SRWLOCK_INIT;
const Table* g_table = nullptr;                   // under g_table_lock
std::atomic<const std::wstring*> g_ffxi{nullptr}; // FFXiMain.dll's folder, lower case, trailing backslash
std::wstring& g_root = forever<std::wstring>();   // [pivot] rootfolder, absolute (overlay_root()); set at start
std::wstring& g_ini = forever<std::wstring>();    // the ini install() read, read again when it changes
Order& g_listed = forever<Order>();               // the order g_table was listed in (watch()'s)
std::atomic<bool> g_log_all{false};

std::mutex& g_log_lock = forever<std::mutex>();
std::set<std::wstring>& g_logged = forever<std::set<std::wstring>>();

// The DATs opened to read lately (opened_dats()): path under the FFXI folder, lower case -> when (a count).
constexpr size_t kOpened = 1024;
std::mutex& g_opened_lock = forever<std::mutex>();
std::unordered_map<std::wstring, std::uint64_t>& g_opened = forever<std::unordered_map<std::wstring, std::uint64_t>>();
std::uint64_t g_opens = 0;

void opened(std::wstring path)
{
    const std::lock_guard guard(g_opened_lock);
    g_opened[std::move(path)] = ++g_opens;
    if (g_opened.size() <= 2 * kOpened)
        return;
    // the oldest half goes
    std::vector<std::uint64_t> when;
    when.reserve(g_opened.size());
    for (const auto& [name, at] : g_opened)
        when.push_back(at);
    std::nth_element(when.begin(), when.end() - kOpened, when.end());
    const std::uint64_t keep = *(when.end() - kOpened);
    for (auto it = g_opened.begin(); it != g_opened.end();)
        it = it->second < keep ? g_opened.erase(it) : std::next(it);
}

std::wstring lower(std::wstring_view text)
{
    std::wstring out(text);
    for (wchar_t& c : out)
        c = static_cast<wchar_t>(std::towlower(c));
    return out;
}

std::wstring wide(const char* text)
{
    const int length = MultiByteToWideChar(CP_ACP, 0, text, -1, nullptr, 0);
    if (length <= 1)
        return {};
    std::wstring out(static_cast<size_t>(length), L'\0');
    MultiByteToWideChar(CP_ACP, 0, text, -1, out.data(), length);
    out.pop_back();
    return out;
}

// `text` in the ANSI code page; false if it has characters that can't be.
bool narrow(const std::wstring& text, std::string& out)
{
    BOOL lossy = FALSE;
    BOOL* check = GetACP() == CP_UTF8 ? nullptr : &lossy; // UTF-8 can say anything (and refuses the check)
    const int length = WideCharToMultiByte(CP_ACP, 0, text.c_str(), -1, nullptr, 0, nullptr, check);
    if (length <= 1 || lossy)
        return false;
    out.assign(static_cast<size_t>(length), '\0');
    WideCharToMultiByte(CP_ACP, 0, text.c_str(), -1, out.data(), length, nullptr, nullptr);
    out.pop_back();
    return true;
}

bool is_folder(const std::wstring& path)
{
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

// `value` as an absolute folder, from `base` when it's relative; no trailing backslash.
std::wstring folder_path(const std::wstring& base, std::wstring_view value)
{
    // An absolute path replaces base; lexically_normal() also collapses doubled backslashes.
    std::wstring path = (std::filesystem::path(base) / value).lexically_normal().wstring();
    while (path.size() > 3 && path.back() == L'\\')
        path.pop_back();
    return path;
}

// `text` without spaces around it, a comment after it (" ; ..." : Windows keeps those in the
// value) or quotes.
std::wstring_view trim(std::wstring_view text)
{
    for (size_t at = 1; at < text.size(); ++at)
        if (text[at] == L';' && std::iswspace(text[at - 1])) {
            text = text.substr(0, at);
            break;
        }
    while (!text.empty() && std::iswspace(text.front()))
        text.remove_prefix(1);
    while (!text.empty() && std::iswspace(text.back()))
        text.remove_suffix(1);
    if (text.size() >= 2 && text.front() == L'"' && text.back() == L'"')
        text = text.substr(1, text.size() - 2);
    return text;
}

// Adds what's under `folder` to `files` as overlay `overlay`'s, where no overlay before it has the
// same path. `under`: the path in the overlay so far, lower case. `added` and `hidden` count files.
void list(std::unordered_map<std::wstring, Entry>& files, const std::wstring& folder, const std::wstring& under,
          std::uint16_t overlay, int depth, size_t& added, size_t& hidden)
{
    WIN32_FIND_DATAW found{};
    const HANDLE handle = FindFirstFileExW((folder + L"\\*").c_str(), FindExInfoBasic, &found, FindExSearchNameMatch,
                                           nullptr, FIND_FIRST_EX_LARGE_FETCH);
    if (handle == INVALID_HANDLE_VALUE)
        return;
    do {
        const std::wstring_view name = found.cFileName;
        if (name == L"." || name == L"..")
            continue;
        std::wstring path = under + lower(name);
        if ((found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
            files.emplace(path, Entry{overlay, true});
            if (depth < kDepth)
                list(files, folder + L'\\' + found.cFileName, path + L'\\', overlay, depth + 1, added, hidden);
        } else if (files.emplace(std::move(path), Entry{overlay, false}).second) {
            ++added;
        } else {
            ++hidden;
        }
    } while (FindNextFileW(handle, &found));
    FindClose(handle);
}

// How the caller uses the path.
enum class Use {
    Read,   // opens a file to read it
    Look,   // its attributes (a folder too)
    Search, // FindFirstFile: a pattern, left alone if it has wildcards (a folder too)
    Write,  // opens it to write: never an overlay's, only logged
};

// The first files served from an overlay, in the log (every one once with [pivot] log=1). `key`:
// the file's path in the overlay, `asked`: as the game spelled it, `overlay`: the overlay's name.
void note(const std::wstring& key, std::wstring_view asked, const std::wstring& overlay, bool writing)
{
    const bool all = g_log_all.load(std::memory_order_relaxed);
    bool last = false;
    {
        const std::lock_guard guard(g_log_lock);
        if ((!all && g_logged.size() >= kLogged) || !g_logged.insert(writing ? L">" + key : key).second)
            return;
        last = !all && g_logged.size() == kLogged;
    }
    const std::wstring file(asked);
    const wchar_t* more = last ? L" (the rest aren't logged: [pivot] log=1 logs them all)" : L"";
    if (writing)
        log::write(L"Pivot: %ls is opened for writing, so it's the FFXI folder's own, not %ls's%ls", file.c_str(),
                   overlay.c_str(), more);
    else
        log::write(L"Pivot: %ls from %ls%ls", file.c_str(), overlay.c_str(), more);
}

// The overlay's path that stands in for `path`, into `out`; false if none does. `hooked`: the path came through the
// hooked file functions (a DAT opened to read is remembered for opened_dats()), not VanaCore's own resolve().
bool find(const wchar_t* path, std::wstring& out, Use use, bool hooked = false)
{
    const std::wstring* ffxi = g_ffxi.load(std::memory_order_acquire);
    if (ffxi == nullptr || path == nullptr || path[0] == L'\0')
        return false;
    if (use == Use::Search && std::wcspbrk(path, L"*?<>\"") != nullptr)
        return false;
    // Absolute, '\' only, runs of them as one, . and .. resolved: the game asks for FFXI//ROM/1/2.DAT.
    wchar_t full[MAX_PATH * 2];
    const DWORD length = GetFullPathNameW(path, static_cast<DWORD>(std::size(full)), full, nullptr);
    if (length <= ffxi->size() || length >= std::size(full))
        return false;
    const std::wstring key = lower(std::wstring_view(full, length));
    if (key.compare(0, ffxi->size(), *ffxi) != 0)
        return false;
    const std::wstring relative = key.substr(ffxi->size());
    if (hooked && use == Use::Read && relative.size() > 4 && relative.compare(relative.size() - 4, 4, L".dat") == 0)
        opened(relative);
    bool found = false;
    std::wstring overlay; // the overlay's name, for the log, when it's a file it has
    AcquireSRWLockShared(&g_table_lock);
    if (g_table != nullptr) {
        const auto at = g_table->files.find(relative);
        if (at != g_table->files.end() && !(at->second.folder && (use == Use::Read || use == Use::Write))) {
            found = true;
            const Overlay& from = g_table->overlays[at->second.overlay];
            if (!at->second.folder)
                overlay = from.name;
            if (use != Use::Write)
                out = from.folder + L'\\' + relative;
        }
    }
    ReleaseSRWLockShared(&g_table_lock);
    if (!found)
        return false;
    if (!overlay.empty())
        note(relative, std::wstring_view(full, length).substr(ffxi->size()), overlay, use == Use::Write);
    return use != Use::Write;
}

bool find(const char* path, std::wstring& out, Use use, bool hooked = false)
{
    return path != nullptr && g_ffxi.load(std::memory_order_acquire) != nullptr && find(wide(path).c_str(), out, use, hooked);
}

constexpr DWORD kWrites = GENERIC_WRITE | GENERIC_ALL | MAXIMUM_ALLOWED | FILE_WRITE_DATA | FILE_APPEND_DATA |
                          FILE_WRITE_EA | FILE_WRITE_ATTRIBUTES | DELETE | WRITE_DAC | WRITE_OWNER;

Use opening(DWORD access, DWORD disposition)
{
    return (access & kWrites) == 0 && (disposition == OPEN_EXISTING || disposition == OPEN_ALWAYS) ? Use::Read : Use::Write;
}

#define ORIGINAL(name) decltype(&::name) g_##name = &::name

ORIGINAL(CreateFileA);
ORIGINAL(CreateFileW);
ORIGINAL(GetFileAttributesA);
ORIGINAL(GetFileAttributesW);
ORIGINAL(GetFileAttributesExA);
ORIGINAL(GetFileAttributesExW);
ORIGINAL(FindFirstFileA);
ORIGINAL(FindFirstFileW);
ORIGINAL(FindFirstFileExA);
ORIGINAL(FindFirstFileExW);

// The ANSI functions open an overlay's file through the wide ones, so any folder name works; the
// FindFirstFile ones can't (the found file comes back in the caller's form).
HANDLE WINAPI create_file_a(LPCSTR path, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES security, DWORD disposition,
                            DWORD flags, HANDLE templ)
{
    std::wstring overlay;
    if (find(path, overlay, opening(access, disposition), true))
        return g_CreateFileW(overlay.c_str(), access, share, security, disposition, flags, templ);
    return g_CreateFileA(path, access, share, security, disposition, flags, templ);
}
HANDLE WINAPI create_file_w(LPCWSTR path, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES security, DWORD disposition,
                            DWORD flags, HANDLE templ)
{
    std::wstring overlay;
    return g_CreateFileW(find(path, overlay, opening(access, disposition), true) ? overlay.c_str() : path, access, share,
                         security, disposition, flags, templ);
}
DWORD WINAPI get_file_attributes_a(LPCSTR path)
{
    std::wstring overlay;
    return find(path, overlay, Use::Look) ? g_GetFileAttributesW(overlay.c_str()) : g_GetFileAttributesA(path);
}
DWORD WINAPI get_file_attributes_w(LPCWSTR path)
{
    std::wstring overlay;
    return g_GetFileAttributesW(find(path, overlay, Use::Look) ? overlay.c_str() : path);
}
BOOL WINAPI get_file_attributes_ex_a(LPCSTR path, GET_FILEEX_INFO_LEVELS level, LPVOID info)
{
    std::wstring overlay;
    return find(path, overlay, Use::Look) ? g_GetFileAttributesExW(overlay.c_str(), level, info)
                                          : g_GetFileAttributesExA(path, level, info);
}
BOOL WINAPI get_file_attributes_ex_w(LPCWSTR path, GET_FILEEX_INFO_LEVELS level, LPVOID info)
{
    std::wstring overlay;
    return g_GetFileAttributesExW(find(path, overlay, Use::Look) ? overlay.c_str() : path, level, info);
}
HANDLE WINAPI find_first_file_a(LPCSTR path, LPWIN32_FIND_DATAA data)
{
    std::wstring overlay;
    std::string ansi;
    return g_FindFirstFileA(find(path, overlay, Use::Search) && narrow(overlay, ansi) ? ansi.c_str() : path, data);
}
HANDLE WINAPI find_first_file_w(LPCWSTR path, LPWIN32_FIND_DATAW data)
{
    std::wstring overlay;
    return g_FindFirstFileW(find(path, overlay, Use::Search) ? overlay.c_str() : path, data);
}
HANDLE WINAPI find_first_file_ex_a(LPCSTR path, FINDEX_INFO_LEVELS level, LPVOID data, FINDEX_SEARCH_OPS search,
                                   LPVOID filter, DWORD flags)
{
    std::wstring overlay;
    std::string ansi;
    return g_FindFirstFileExA(find(path, overlay, Use::Search) && narrow(overlay, ansi) ? ansi.c_str() : path, level,
                              data, search, filter, flags);
}
HANDLE WINAPI find_first_file_ex_w(LPCWSTR path, FINDEX_INFO_LEVELS level, LPVOID data, FINDEX_SEARCH_OPS search,
                                   LPVOID filter, DWORD flags)
{
    std::wstring overlay;
    return g_FindFirstFileExW(find(path, overlay, Use::Search) ? overlay.c_str() : path, level, data, search, filter,
                              flags);
}

// The C runtimes: fopen, std::ifstream and _open open files through their own CreateFileW, so a DLL that reads the game's
// files with them (Minimap reads its map DATs with fopen_s) would get the FFXI folder's. XIPivot hooks the file functions
// process-wide, so under Ashita those reads see the overlays; these are hooked though they're Windows' own DLLs.
bool is_c_runtime(HMODULE module)
{
    wchar_t path[MAX_PATH] = {};
    const DWORD length = GetModuleFileNameW(module, path, MAX_PATH);
    if (length == 0 || length >= MAX_PATH)
        return false;
    const wchar_t* slash = std::wcsrchr(path, L'\\');
    const wchar_t* name = slash != nullptr ? slash + 1 : path;
    return _wcsicmp(name, L"ucrtbase.dll") == 0 || _wcsicmp(name, L"msvcrt.dll") == 0;
}

void on_module(HMODULE module)
{
    if (imports::is_windows_module(module) && !is_c_runtime(module))
        return;
    static constexpr const char* kKernel[] = {"KERNEL32.dll", "api-ms-win-core-file-l1-1-0.dll",
                                              "api-ms-win-core-file-l1-2-0.dll", "api-ms-win-core-file-l2-1-0.dll"};
    for (const char* dll : kKernel) {
#define HOOK(name, replacement) imports::hook(module, dll, #name, &replacement, g_##name)
        HOOK(CreateFileA, create_file_a);
        HOOK(CreateFileW, create_file_w);
        HOOK(GetFileAttributesA, get_file_attributes_a);
        HOOK(GetFileAttributesW, get_file_attributes_w);
        HOOK(GetFileAttributesExA, get_file_attributes_ex_a);
        HOOK(GetFileAttributesExW, get_file_attributes_ex_w);
        HOOK(FindFirstFileA, find_first_file_a);
        HOOK(FindFirstFileW, find_first_file_w);
        HOOK(FindFirstFileExA, find_first_file_ex_a);
        HOOK(FindFirstFileExW, find_first_file_ex_w);
#undef HOOK
    }
}

// FFXiMain.dll's folder is the FFXI folder the game reads from: pivot starts with it.
void on_game_loaded(HMODULE module)
{
    if (g_ffxi.load() != nullptr)
        return;
    wchar_t file[MAX_PATH * 2] = {};
    const DWORD length = GetModuleFileNameW(module, file, static_cast<DWORD>(std::size(file)));
    wchar_t full[MAX_PATH * 2] = {};
    const DWORD full_length =
        length > 0 && length < std::size(file) ? GetFullPathNameW(file, static_cast<DWORD>(std::size(full)), full, nullptr) : 0;
    std::wstring folder(full, full_length < std::size(full) ? full_length : 0);
    const size_t slash = folder.find_last_of(L'\\');
    folder.resize(slash != std::wstring::npos ? slash + 1 : 0);
    if (folder.empty()) {
        log::write(L"Pivot: can't tell FFXiMain.dll's folder: the game's files are its own");
        return;
    }
    g_ffxi.store(new std::wstring(lower(folder)), std::memory_order_release); // never freed: hooks may be reading it
    log::write(L"Pivot: the game's files under %ls come from the overlays first", folder.c_str());
}

// [pivot.order] from `ini`, in number order (the lowest is looked in first). Lines that aren't <number>=<folder> are
// logged and left out.
Order read_order(const wchar_t* ini)
{
    std::wstring order(32767, L'\0'); // GetPrivateProfileSection's maximum
    const DWORD length = GetPrivateProfileSectionW(L"pivot.order", order.data(), static_cast<DWORD>(order.size()), ini);
    // "0=ffxi-hd\01=catseyexi\0\0"
    Order listed;
    for (size_t at = 0; at < length;) {
        const std::wstring_view line(order.c_str() + at);
        at += line.size() + 1;
        const std::wstring_view trimmed = trim(line);
        if (trimmed.empty() || trimmed.front() == L';' || trimmed.front() == L'#')
            continue;
        const size_t equals = line.find(L'=');
        const std::wstring number(trim(line.substr(0, equals == std::wstring_view::npos ? 0 : equals)));
        const std::wstring_view name = equals == std::wstring_view::npos ? std::wstring_view() : trim(line.substr(equals + 1));
        wchar_t* end = nullptr;
        const long position = number.empty() ? -1 : std::wcstol(number.c_str(), &end, 10);
        if (position < 0 || *end != L'\0' || name.empty()) {
            log::write(L"Pivot: [pivot.order] %.*ls isn't <number>=<folder>; ignored", static_cast<int>(line.size()),
                       line.data());
            continue;
        }
        listed.emplace_back(position, std::wstring(name));
    }
    std::stable_sort(listed.begin(), listed.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    return listed;
}

// SoulspireXI's mandatory server overlays (base soulspirexi DATs + protected COR Flintlock dual-pistol DATs)
// always go first in deterministic order whatever [pivot.order] says, whenever present under rootfolder:
// no user overlay comes before them and they cannot be switched off. The /vana Pivot tab shows them locked.
void server_first(Order& listed, const std::wstring& root)
{
    if (client::retail()) {
        // the server's DATs are for its own client: not used, even where [pivot.order] lists them
        for (const wchar_t* ov : kServerOverlays) {
            const std::wstring server_key = lower(folder_path(root, ov));
            listed.erase(std::remove_if(listed.begin(), listed.end(),
                                        [&](const auto& entry) { return lower(folder_path(root, entry.second)) == server_key; }),
                         listed.end());
        }
        return;
    }
    Order pinned;
    for (const wchar_t* ov : kServerOverlays) {
        const std::wstring server = folder_path(root, ov);
        if (!is_folder(server))
            continue;
        const std::wstring key = lower(server);
        std::wstring name = ov;
        long own = static_cast<long>(pinned.size());
        for (auto it = listed.begin(); it != listed.end();) {
            if (lower(folder_path(root, it->second)) == key) {
                name = it->second;
                own = it->first;
                it = listed.erase(it);
            } else {
                ++it;
            }
        }
        pinned.emplace_back(own, std::move(name));
    }
    if (!pinned.empty())
        listed.insert(listed.begin(), pinned.begin(), pinned.end());
}

// The game's menu DATs (ROM\119\51 English, ROM\91\15 Japanese) and the pictures of theirs the game takes by number,
// which a copy made for an older client may not have: the game reads past its sheet's end and crashes. The item details'
// badges are windowps's sprites 294, 295, 296, 293 and 842 (their numbers at 0x1036DA24, taken in 0x1013BB00);
// ffxi-hd's Japanese menu DAT has 811 of its 845, and opening the Equipment window crashed (2026-11-08). Its English
// one, a client older too (843), has them all. A copy short of one isn't used: the next overlay's that isn't, else the
// game's own.
struct MenuDat {
    const wchar_t* path; // as Table::files has it
    const wchar_t* shown;
};
constexpr MenuDat kMenuDats[] = {{L"rom\\119\\51.dat", L"ROM\\119\\51.DAT"}, {L"rom\\91\\15.dat", L"ROM\\91\\15.DAT"}};
struct Needed {
    const char* sheet; // its name, 16 letters
    unsigned sprites;  // how many the game needs it to have
    const wchar_t* what;
};
constexpr Needed kNeeded[] = {{"menu    windowps", 843, L"the item details' badges"}};
constexpr LONGLONG kMenuDatMost = 64LL << 20;

// The file at `path`, read whole; empty if it can't be. (An overlay's: not under the FFXI folder, so the hooks leave it as
// it is.)
std::vector<std::uint8_t> read_whole(const std::wstring& path)
{
    std::vector<std::uint8_t> data;
    const HANDLE file =
        CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return data;
    LARGE_INTEGER size{};
    DWORD read = 0;
    if (GetFileSizeEx(file, &size) && size.QuadPart > 0 && size.QuadPart <= kMenuDatMost) {
        data.resize(static_cast<size_t>(size.QuadPart));
        if (!ReadFile(file, data.data(), static_cast<DWORD>(data.size()), &read, nullptr))
            read = 0;
    }
    CloseHandle(file);
    data.resize(read);
    return data;
}

// A DAT's sprite sheets to their sprites' count. Its chunks: a 16-byte header (the dword at +4: the type in its low 7
// bits, the size in 16-byte units over them), then the chunk's data; a sheet's (type 31h): its name (16 letters), its
// images' count (a byte) and names (16 letters each), its sprites' count (2 bytes). tools\jplabels\jplabels.py's.
std::unordered_map<std::string, unsigned> sheets_of(const std::vector<std::uint8_t>& data)
{
    constexpr std::uint32_t kSheet = 0x31;
    std::unordered_map<std::string, unsigned> sheets;
    for (size_t at = 0; at + 16 <= data.size();) {
        std::uint32_t packed = 0;
        std::memcpy(&packed, data.data() + at + 4, sizeof packed);
        const size_t size = static_cast<size_t>(packed >> 7) * 16;
        if (size < 16 || size > data.size() - at)
            break;
        const std::uint8_t* body = data.data() + at + 16;
        const size_t length = size - 16;
        const size_t count = length > 16 ? 17 + 16 * static_cast<size_t>(body[16]) : length;
        if ((packed & 0x7F) == kSheet && count + 2 <= length)
            sheets[std::string(reinterpret_cast<const char*>(body), 16)] = static_cast<unsigned>(body[count] | body[count + 1] << 8);
        at += size;
    }
    return sheets;
}

// The first picture the game takes by number that the menu DAT at `file` hasn't (its sheet's sprites into `has`); none if
// it has them all, or isn't a DAT of sprite sheets that can be read
const Needed* short_of(const std::wstring& file, unsigned& has)
{
    const auto sheets = sheets_of(read_whole(file));
    if (sheets.empty())
        return nullptr;
    for (const Needed& needed : kNeeded) {
        const auto at = sheets.find(needed.sheet);
        has = at != sheets.end() ? at->second : 0;
        if (has < needed.sprites)
            return &needed;
    }
    return nullptr;
}

// Each menu DAT `table` has from an overlay, from the first overlay whose copy has the pictures the game takes by
// number; none: the game's own
void vet_menu_dats(Table& table)
{
    for (const MenuDat& dat : kMenuDats) {
        const auto at = table.files.find(dat.path);
        if (at == table.files.end() || at->second.folder)
            continue;
        size_t use = table.overlays.size();
        for (size_t i = at->second.overlay; i < table.overlays.size() && use == table.overlays.size(); ++i) {
            const Overlay& overlay = table.overlays[i];
            const std::wstring file = overlay.folder + L'\\' + dat.path;
            if (GetFileAttributesW(file.c_str()) == INVALID_FILE_ATTRIBUTES)
                continue; // (a later overlay without it)
            unsigned has = 0;
            const Needed* missing = short_of(file, has);
            if (missing == nullptr) {
                use = i;
                continue;
            }
            log::write(L"Pivot: %ls's %ls is from an older game: its \"%hs\" sheet has %u pictures, fewer than the %u the game "
                       L"takes for %ls, so it isn't used",
                       overlay.name.c_str(), dat.shown, missing->sheet, has, missing->sprites, missing->what);
        }
        if (use == at->second.overlay)
            continue;
        if (use < table.overlays.size()) {
            at->second.overlay = static_cast<std::uint16_t>(use);
            log::write(L"Pivot: %ls comes from %ls instead", dat.shown, table.overlays[use].name.c_str());
        } else {
            table.files.erase(at);
            log::write(L"Pivot: %ls is the game's own instead", dat.shown);
        }
    }
}

// A new table of the overlays in `listed` (folders under `root`, or absolute), each logged with its files.
Table* build(const std::wstring& root, const Order& listed)
{
    auto* table = new Table();
    const ULONGLONG start = GetTickCount64();
    std::set<std::wstring> folders;
    for (const auto& [position, name] : listed) {
        const std::wstring folder = folder_path(root, name);
        if (!is_folder(folder)) {
            log::write(L"Pivot: [pivot.order] %ld=%ls is not a folder (%ls); skipped", position, name.c_str(), folder.c_str());
            continue;
        }
        if (!folders.insert(lower(folder)).second || table->overlays.size() >= 0xFFFF)
            continue;
        table->overlays.push_back({name, folder});
        size_t added = 0, hidden = 0;
        list(table->files, folder, L"", static_cast<std::uint16_t>(table->overlays.size() - 1), 0, added, hidden);
        if (hidden == 0)
            log::write(L"Pivot: %ld=%ls: %zu file%ls (%ls)", position, name.c_str(), added, added == 1 ? L"" : L"s",
                       folder.c_str());
        else
            log::write(L"Pivot: %ld=%ls: %zu file%ls, and %zu more that an overlay before it has (%ls)", position,
                       name.c_str(), added, added == 1 ? L"" : L"s", hidden, folder.c_str());
    }
    vet_menu_dats(*table);
    size_t files = 0;
    for (const auto& [path, entry] : table->files)
        files += entry.folder ? 0 : 1;
    if (files == 0)
        log::write(L"Pivot: no files in [pivot.order]'s overlays under %ls: the game's files are its own", root.c_str());
    else
        log::write(L"Pivot: %zu overlay%ls, %zu files, listed in %llu ms", table->overlays.size(),
                   table->overlays.size() == 1 ? L"" : L"s", files, GetTickCount64() - start);
    return table;
}

// `table` in use from now on: the file the game opens next comes from its overlays. The one it replaces is freed once no
// hook is looking in it.
void publish(const Table* table)
{
    AcquireSRWLockExclusive(&g_table_lock);
    const Table* old = std::exchange(g_table, table);
    ReleaseSRWLockExclusive(&g_table_lock);
    delete old;
    const std::lock_guard guard(g_log_lock);
    g_logged.clear(); // the first files served from the new table are logged again
}

// The overlays listed again, when files came or went under the root folder (`files`) or [pivot.order] changed.
void relist(bool files)
{
    Order listed = read_order(g_ini.c_str());
    server_first(listed, g_root);
    if (!files && listed == g_listed)
        return; // another setting changed
    if (files)
        log::write(L"Pivot: files changed under %ls: listing the overlays again", g_root.c_str());
    else
        log::write(L"Pivot: [pivot.order] changed: listing the overlays again");
    g_listed = listed;
    publish(build(g_root, listed));
}

// When the ini (`path`) was last written.
FILETIME written(const std::wstring& path)
{
    WIN32_FILE_ATTRIBUTE_DATA data{};
    return g_GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data) ? data.ftLastWriteTime : FILETIME{};
}

// Lists the overlays again when they change, so the game gets the new ones from its next file loads (a zone's DATs at the
// next zone; what it loads once, at start, stays as it was until a restart): [pivot.order] in the ini (the /vana Pivot
// tab, the file's settings taken up), or files and folders made, renamed or deleted under the root folder (an overlay
// added, a mod's files copied in). Each waits for things to settle: the Pivot tab writes [pivot.order] a key at a time,
// and a copy makes thousands of files. On a thread of its own: ffxi-hd's 34000 files list in 30 ms when Windows has them
// cached, seconds when it hasn't.
DWORD WINAPI watch(void*)
{
    HANDLE change = FindFirstChangeNotificationW(g_root.c_str(), TRUE, FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME);
    if (change == INVALID_HANDLE_VALUE)
        log::write(L"Pivot: can't watch %ls (error %lu): files added to the overlays are seen when [pivot.order] changes",
                   g_root.c_str(), GetLastError());
    FILETIME ini_time = written(g_ini);
    ULONGLONG due = 0;  // when to list again; 0: nothing has changed
    bool files = false; // what changed was under the root folder (else the ini)
    for (;;) {
        if (change == INVALID_HANDLE_VALUE) {
            Sleep(kPoll);
        } else if (WaitForSingleObject(change, kPoll) == WAIT_OBJECT_0) {
            files = true;
            due = GetTickCount64() + kQuiet; // each change puts it off: a copy is listed once, after its last file
            if (!FindNextChangeNotification(change)) {
                FindCloseChangeNotification(change);
                change = INVALID_HANDLE_VALUE;
            }
        }
        const FILETIME time = written(g_ini);
        if (CompareFileTime(&time, &ini_time) != 0) {
            ini_time = time;
            g_log_all.store(GetPrivateProfileIntW(L"pivot", L"log", 0, g_ini.c_str()) != 0, std::memory_order_relaxed);
            due = std::max(due, GetTickCount64() + kSettle);
        }
        if (due != 0 && GetTickCount64() >= due) {
            relist(files);
            due = 0;
            files = false;
        }
    }
}

} // namespace

void install(const wchar_t* ini, const wchar_t* base_dir)
{
    if (GetPrivateProfileIntW(L"pivot", L"enabled", 0, ini) == 0) {
        std::wstring order(64, L'\0'); // anything in it
        if (GetPrivateProfileSectionW(L"pivot.order", order.data(), static_cast<DWORD>(order.size()), ini) > 0)
            log::write(L"Pivot: off ([pivot] enabled=0): [pivot.order]'s overlays aren't used");
        return;
    }
    g_log_all.store(GetPrivateProfileIntW(L"pivot", L"log", 0, ini) != 0, std::memory_order_relaxed);

    wchar_t value[MAX_PATH] = {};
    GetPrivateProfileStringW(L"pivot", L"rootfolder", L"pivot", value, MAX_PATH, ini);
    const std::wstring root = folder_path(base_dir, trim(value).empty() ? L"pivot" : trim(value));
    if (!is_folder(root)) {
        log::write(L"Pivot: [pivot] rootfolder=%ls is not a folder (%ls): the game's files are its own", value, root.c_str());
        return;
    }
    g_root = root;
    g_ini = ini;

    // Listed here, before the game loads: its first files come from the overlays. Hooked even with none in use, so
    // that one switched on later is used.
    g_listed = read_order(ini);
    server_first(g_listed, root);
    publish(build(root, g_listed));

    if (!imports::when_loaded(L"FFXiMain.dll", &on_game_loaded) || !imports::for_each_module(&on_module)) {
        log::write(L"Pivot: can't watch DLLs load: the game's files are its own");
        return;
    }
    const HANDLE thread = CreateThread(nullptr, 0, &watch, nullptr, 0, nullptr);
    if (thread == nullptr)
        log::write(L"Pivot: can't start watching the overlays (error %lu): changes to them apply at the next start",
                   GetLastError());
    else
        CloseHandle(thread);
}

std::wstring resolve(const std::wstring& path)
{
    std::wstring overlay;
    return find(path.c_str(), overlay, Use::Read) ? overlay : path;
}

const std::wstring& overlay_root()
{
    return g_root;
}

std::vector<std::wstring> opened_dats()
{
    std::vector<std::pair<std::uint64_t, std::wstring>> sorted;
    {
        const std::lock_guard guard(g_opened_lock);
        sorted.reserve(g_opened.size());
        for (const auto& [name, at] : g_opened)
            sorted.emplace_back(at, name);
    }
    std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    std::vector<std::wstring> out;
    out.reserve(sorted.size());
    for (auto& [at, name] : sorted)
        out.push_back(std::move(name));
    return out;
}

} // namespace vanacore::pivot
