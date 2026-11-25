#include "sigs.hpp"

#include <cstdlib>
#include <cwchar>
#include <mutex>
#include <set>
#include <string>

#include "log.hpp"
#include "scan.hpp"

namespace vanacore::sigs {
namespace {

// The builds tools\sigaudit.py has checked every pattern against: the header's TimeDateStamp and SizeOfImage
struct Known {
    std::uint32_t timestamp;
    std::uint32_t image_size;
    const char* name;
};
constexpr Known kKnown[] = {
    {0x694E5965, 0x00BDF000, "CatsEyeXI reference build (2025-12-26)"},
    {0x6A995428, 0x00C15000, "retail reference build (2026-09-03)"},
    {0x6AE818B4, 0x00C17000, "SoulspireXI target retail build (0x6AE818B4 — runtime scan active)"},
};

constexpr std::size_t kMaxLogged = 200; // an addon scanning for patterns of its own can't fill the log

std::mutex g_lock;
std::set<std::string> g_missed;
bool g_build_logged = false;

// The item icon drawing, as itemrarity.cpp hooks it (FUN_1015a2d0, __thiscall on the item's data): sub esp,8; push ebx;
// push esi; push edi; mov esi,ecx; call is-loaded; test al,al; je; mov ecx,[esi+0D0Ch]. The field is the item's icon,
// 0xD0C in CatsEyeXI's build and 0x150C in retail's.
constexpr char kItemIcon[] = "83 EC 08 53 56 57 8B F1 E8 ?? ?? ?? ?? 84 C0 0F 84 ?? ?? ?? ?? 8B 8E ?? ?? ?? ??";
constexpr std::ptrdiff_t kItemIconAt = 23;
constexpr std::int32_t kItemIconField = 0xD0C;
constexpr std::int32_t kMostShift = 0x4000; // a shift past this isn't the same structure

std::wstring file_name(HMODULE module)
{
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(module, path, MAX_PATH);
    const wchar_t* name = wcsrchr(path, L'\\');
    return name != nullptr ? name + 1 : path;
}

} // namespace

Build build(HMODULE game)
{
    Build out;
    if (game == nullptr)
        return out;
    const auto* base = reinterpret_cast<const BYTE*>(game);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + reinterpret_cast<const IMAGE_DOS_HEADER*>(base)->e_lfanew);
    out.timestamp = nt->FileHeader.TimeDateStamp;
    out.image_size = nt->OptionalHeader.SizeOfImage;
    for (const Known& known : kKnown) {
        if (known.timestamp == out.timestamp && known.image_size == out.image_size)
            out.name = known.name;
    }
    return out;
}

void note_scan(HMODULE module, const char* pattern, std::uintptr_t found)
{
    std::lock_guard<std::mutex> lock(g_lock);
    if (!g_build_logged && module == GetModuleHandleW(L"FFXiMain.dll")) {
        g_build_logged = true;
        const Build now = build(module);
        if (now.name != nullptr)
            log::write(L"Signatures: FFXiMain.dll is %hs (header %08X, image %X)", now.name, now.timestamp, now.image_size);
        else
            log::write(L"Signatures: FFXiMain.dll is a build tools\\sigaudit.py hasn't checked (header %08X, image %X); the "
                       L"patterns that find nothing in it are logged here",
                       now.timestamp, now.image_size);
    }
    if (found != 0 || pattern == nullptr || g_missed.size() >= kMaxLogged || !g_missed.insert(pattern).second)
        return;
    log::write(L"Signatures: nothing in %ls matches \"%.64hs%ls\"", file_name(module).c_str(), pattern,
               std::char_traits<char>::length(pattern) > 64 ? L"..." : L"");
}

unsigned missed()
{
    std::lock_guard<std::mutex> lock(g_lock);
    return static_cast<unsigned>(g_missed.size());
}

std::ptrdiff_t item_data_shift()
{
    static bool done = false;
    static std::ptrdiff_t shift = 0;
    if (done)
        return shift;
    const HMODULE game = GetModuleHandleW(L"FFXiMain.dll");
    if (game == nullptr)
        return 0;
    done = true;
    const std::uintptr_t site = scan::find(game, kItemIcon);
    if (site == 0) {
        log::write(L"Signatures: the item data's layout wasn't read (the item icon drawing wasn't found); CatsEyeXI's is assumed");
        return 0;
    }
    const auto field = static_cast<std::int32_t>(scan::operand(site, kItemIconAt));
    if (std::abs(field - kItemIconField) > kMostShift) {
        log::write(L"Signatures: the item data's icon is at +0x%X, nothing like CatsEyeXI's +0x%X; its layout is assumed", field,
                   kItemIconField);
        return 0;
    }
    shift = field - kItemIconField;
    if (shift != 0)
        log::write(L"Signatures: the item data's fields are %+d bytes from CatsEyeXI's", static_cast<int>(shift));
    return shift;
}

} // namespace vanacore::sigs
