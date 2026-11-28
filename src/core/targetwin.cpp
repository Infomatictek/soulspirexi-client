#include "targetwin.hpp"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <cwctype>
#include <string>
#include <utility>
#include <vector>

#include "../gfx/vanacore_gfx.h"
#include "chatfont.hpp"
#include "gametext.hpp"
#include "gameui.hpp"
#include "gmicons.hpp"
#include "soulspire/echo_icons.hpp"
#include "image.hpp"
#include "log.hpp"
#include "outline.hpp"
#include "scan.hpp"
#include "text.hpp"
#include "uispace.hpp"

namespace vanacore::targetwin {
namespace {

// The Target window is a window of the menu manager (CTkMenuMng, a global in FFXiMain.dll) whose layout is named in the
// game's data: 16 characters at the layout's +0x46, a window's +4 being its layout.
constexpr char kLayout[16 + 1] = "menu    targetwi";
constexpr char kPartyLayout[16 + 1] = "menu    partywin";

// The manager's global, found as uiscale.cpp finds it, in the window that has the help line's text (FUN_101182a0):
// push ebx; push ebp; push esi; mov ebp,ecx; push edi; mov ecx,<the menu manager>; call; cmp eax,ebp; jne; ...
constexpr char kHelp[] = "53 55 56 8B E9 57 B9 ?? ?? ?? ?? E8 ?? ?? ?? ?? 3B C5 0F 85 ?? ?? ?? ?? 66 8B 45 4C 8B CD 50 E8";
constexpr std::ptrdiff_t kManagerAt = 7;
// A window put at (x, y) with its parts, its handler told (SetLocate, FUN_10118d10; thiscall window, x, y, ret 8), which
// calls the window's resize (FUN_1011a1a0: thiscall window, x, y, width, height, 1, 0, 0, ret 1Ch) at +0x6C. Its first
// 8 bytes (sub esp,0Ch; mov ax,[esp+14h]) are itemlist.cpp's detour prologue, a jmp into VanaCore by the time this
// looks: matched from the push ebx after them, the game's own code whatever VanaCore put before it. A resize alone
// leaves the window's handler with its old rect: SetLocate after it tells it.
constexpr char kSetLocate[] = "53 55 8B 6C 24 18 56 8B F1 57 33 FF 8D 5E 14 66 89 6E 52 8B CB 66 89 46 54";
constexpr std::ptrdiff_t kSetLocatePrologue = 8, kResizeAt = 0x6C;

// The Target window's owner (CTkTarget in the PS2 client's names) draws it: FUN_1014EEF0 on the PC, its vtable's slot 4.
//   sub esp,8; push esi; mov esi,ecx; push edi; mov byte [esp+0Ch],1; mov al,[esi+65h]; test al,al; je ...
// Its rect (left, top, right, bottom) is the shorts at +0x4C. In it, from its start:
//   +0x2D  call the HP bar (cdecl x, y, max, &hp) with the window's own x+1Ch, y+1Dh (no target of ours)
//   +0xE1  the same call for a target
//   +0x2BA mov edx,[the target struct's global]; test byte [edx+5Ch],3: the lock on
//   +0x2DC call the lock's picture (thiscall shape: x, y, colour, 0, 0, ret 14h) at the window's left, top
//   +0x37C the arrow over the target, drawn last (seen live 2026-10-25): while the owner's +0xBC (its x) is nonzero,
//          the shape at [owner + 0x78 + 4 * byte +0xBA] (six, all the one sprite "anc": 20 wide, 32 tall, its point at
//          the x, y given) is drawn by the shape draw with a scale (FUN_1011F880, thiscall shape: x, y, scale x 1.0,
//          scale y 1.0, colour 0xE0808080, 0, 0, 0, ret 20h) at +0xBC, +0xBE: the call at +0x3B5, after
//          push 1.0; push 1.0; push edx; push eax. Then the sub-target's cursor the same way at +0xC0, +0xC2 while
//          byte +0xB8 is set, in the colour FUN_1014F2F0 picks (the call at +0x3F1): left as the game's
constexpr char kDraw[] = "83 EC 08 56 8B F1 57 C6 44 24 0C 01 8A 46 65 84 C0 74 63 66 8B 56 4E 8A 4E 66";
constexpr std::ptrdiff_t kBarCallA = 0x2D, kBarCallB = 0xE1, kLockFlagAt = 0x2BA, kLockCall = 0x2DC;
constexpr std::ptrdiff_t kArrowCall = 0x3B5;
//   +0x2B2 the target's name: call FUN_101C8700 (cdecl width, x, y, text, colour, drop, wrap) with the window's right less
//          left less 13, its left + 5, its top + 6; +0x68 the same call for an event script's name (the hooked fish's). In
//          it, +0x34 calls the count of a text's letters that fit a width in the game's letters (FUN_101C7E30, cdecl width,
//          text); a name that doesn't fit is wrapped onto a second line 12 lower (seen live 2026-11-06)
constexpr std::ptrdiff_t kNameCall = 0x2B2, kScriptNameCall = 0x68, kNameFitCall = 0x34;
//   +0x146 the target's status mark at the window's right - 24, top - 8 (thiscall shape: x, y, 0x80808080, 0, 0, ret 14h):
//          the shape for the code FUN_10097310 picks (a GM's mark among them), from the table the menu resources' getter
//          (the call at +0x115, given 0) holds at +0, by code less 0x20; edi the target's entity (seen live 2026-11-07)
constexpr std::ptrdiff_t kMarkCall = 0x146, kMarkGetterCall = 0x115;
constexpr std::uint8_t kMarkBefore[] = {0x8B, 0x4C, 0x8A, 0x80}; // mov ecx,[edx+ecx*4-80h]
constexpr int kNameLeft = 5, kNameRoom = 13, kNameMost = 0x18; // (a name is drawn from a copy of 24 letters)
constexpr std::uint8_t kArrowBefore[] = {0x68, 0x00, 0x00, 0x80, 0x3F, 0x68, 0x00, 0x00, 0x80, 0x3F, 0x52, 0x50};
constexpr std::uint8_t kLockFlagBytes[] = {0x8B, 0x15}; // ... mov edx,[global]
constexpr std::uint8_t kLockFlagTest[] = {0xF6, 0x42, 0x5C, 0x03};
// The HP bar's function (FUN_101C97F0, YkDrawHPBar2): push 1; call <the menu resource getter>; mov eax,[eax]; ... its
// call at +0x2B goes to a wrapper (+0x32 in it is a call of the bar's drawing with four scale floats of 1.0 pushed):
// bar(x, y, the fill (+0x184 in the resource), the track (+0x98), hp%, &shown, f6, f7, f8, f9), cdecl, the caller
// cleans up. The first shape is the one cut to the HP (FUN_101C9460 draws it at hp% x f6 wide, f7 tall, 4 units in);
// the second is drawn whole, f8 wide, f9 tall. `shown` is the percent drawn last, stepped 15% of the way to hp% a call.
constexpr std::ptrdiff_t kBarGetter = 2, kBarWrapperCall = 0x2B, kWrapperBarCall = 0x32;
constexpr std::ptrdiff_t kResShapeA = 0x98, kResShapeB = 0x184;
constexpr int kBarLeft = 0x1C, kBarTop = 0x1D; // the bar's place in the window: the rect's left + 1Ch, top + 1Dh
constexpr float kFillHeight = 6.0f; // the fill's picture is 64 x 6, the track's 72 x 8

// The manager: its windows' list at +0 (a node: +0 the next, +0x10 its window, +0x14 set to skip) and the area its windows
// are laid out in (left, top, right, bottom, 16 bits each) at +0x7C
constexpr std::ptrdiff_t kAreaLeft = 0x7C;
// A window: +4 its layout, +0xC its owner, +0x14 its pictures' list (a node: +0 the next, +0x10 the picture, whose +0 is
// its type, 0 for a picture of quads, and +8 its shape), +0x3A..+0x40 its frame (left, top, right, bottom), +0x42..+0x48
// its place as opened, +0x70 to be deleted
constexpr std::ptrdiff_t kWinLayout = 4, kLayoutName = 0x46, kWinOwner = 0xC, kWinPictures = 0x14, kWinFrame = 0x3A,
                         kWinOpened = 0x42, kWinDeleting = 0x70;
// The window's owner (CTkTarget, the global at 0x105762F8): +0x65 is nonzero while an event script has given the window a
// name and an HP bar of its own in place of a target's: the fishing script's opcode 2 (FUN_100B0800) puts the hooked fish
// there, "???" and its HP at +0x66 (1; another script's display is 2), and the draw above takes that branch first and
// returns. It's cleared as the window is made (FUN_1014ED30). No addon can draw that HP (it's nowhere else), so the window
// is shown while it's set, whatever hides it: hidden, players lost the fish's HP (reported 2026-10-21).
constexpr std::ptrdiff_t kOwnerScripted = 0x65;
// A picture's shape: its quads' list at +4 (a node: +0 the next, +0x10 the quad), their count at +0x14. A quad: its
// texture's name at +0x32 (16 characters), its four corners from +0x50, 0x1C apart, each with its alpha at +0x13
constexpr std::ptrdiff_t kParts = 4, kPartCount = 0x14, kNodeItem = 0x10, kQuadName = 0x32, kCorners = 0x50, kCorner = 0x1C,
                         kAlpha = 0x13;
// The caption's pieces ("menu    hfr1" behind "menu    menu2fon" letters); a quad's places are the shorts at +0xC0 (x of
// its four corners) and +0xC8 (y)
constexpr std::ptrdiff_t kLocalX = 0xC0, kLocalY = 0xC8;
constexpr char kCaptionTexture[16 + 1] = "menu    hfr1    ";
constexpr char kLetterTexture[16 + 1] = "menu    menu2fon";
// The lock's picture (the owner's +0x5C, a shape of 14 quads, seen live 2026-11-06), laid out for the game's 112-wide window:
// a glow in each corner ("menu    gauge", 31 x 22, fading in from the window's side), a bar just outside its left and right
// sides ("menu    colorbal", 10 wide), and "Locked" (menu2fon letters between two "menu    keytop" brackets) in its bottom
// middle
constexpr char kLockGlowTexture[16 + 1] = "menu    gauge   ";
constexpr char kLockBarTexture[16 + 1] = "menu    colorbal";

using SetLocate = void(__thiscall*)(void* window, int x, int y);
using Resize = void(__thiscall*)(void* window, int x, int y, int width, int height, int a, int b, int c);
using BarGame = void(__cdecl*)(int x, int y, int max, void* hp);
using BarDraw = void(__cdecl*)(int x, int y, void* fill, void* track, int max, void* hp, float f6, float f7, float f8, float f9);
using ResGetter = void* const*(__cdecl*)(int which);
using LockDraw = void(__thiscall*)(void* shape, int x, int y, std::uint32_t colour, int a, int b);
using NameDraw = int(__cdecl*)(int width, int x, int y, const char* text, std::uint32_t colour, int drop, int wrap);
using TextFit = int(__cdecl*)(int width, const char* text);
// The name draw's own one-line draw (FUN_101C7EE0: x, y, text, colour), the call at +0x8A. The name draw puts a name with a
// ':' in it on two lines, split there, however wide the window ("Door:Chamber of Commerce": strstr of the ":" it pushes
// at +0x54, found live 2026-11-07), and a name that fits and hasn't one on one line, 6 lower with `drop`
using LineDraw = int(__cdecl*)(int x, int y, const char* text, std::uint32_t colour);
constexpr std::ptrdiff_t kLineDrawCall = 0x8A, kColonPush = 0x54;
constexpr int kLineDrop = 6;
using ShapeDraw = void(__thiscall*)(void* shape, int x, int y, float sx, float sy, std::uint32_t colour, int centred, int blend,
                                    int skip);

enum class Lock { Game, Glow, None };

// [client] target.*
bool g_hide = false;   // target.hide: the window left out of the window draw (skip)
bool g_hide_logged = false;
// target.hide, or an addon's vana.gameui.hide('target_window'): the window put at -4000, -4000 (off the screen, as the chat
// line is for input.hide), where the game draws it unseen and its owner still draws the arrow over the target
constexpr int kHiddenAt = -4000;
bool wants_hidden()
{
    return g_hide || gameui::hidden("target_window");
}
bool g_move = false;
double g_x = 50;       // target.x: the window's middle, percent of the interface's width
double g_y = 4;        // target.y: its top, percent of the interface's height
int g_width = 0;       // target.width: units, 0 the game's
bool g_background = true;
bool g_label = true;
int g_label_x = 0, g_label_y = 0; // target.label.x, .y: the caption moved, in units
int g_bar_x = 0, g_bar_y = 0;
int g_name_align = 0;           // target.name.align: 0 left (the game's), 1 centre, 2 right
bool g_one_line = false;        // target.name.oneline: a name too long for the window kept on one line (the game wraps it)
constexpr int kOneLineRoom = 1024; // ... the room it's given then, the game's units: past the window's right
int g_name_x = 0, g_name_y = 0; // target.name.x, .y: the target's name moved, in units
int g_lock_label_x = 0, g_lock_label_y = 0; // target.lock.label.x, .y: the lock picture's "Locked" moved
bool g_lock_text = true;  // target.lock.label: the lock picture's "Locked" (its letters and brackets) drawn
// target.padding.x, .y: the window made that much bigger on each side, everything in it (the title, the name, the bar, the
// lock's "Locked") moved in with it; target.spacing: that much more between the title and the name and between the name and
// the bar, the window taller by both. Units.
int g_pad_x = 0, g_pad_y = 0, g_spacing = 0;
bool g_title_own = false; // a target.title.font* key is set: the title's own True Font, "Locked" kept the titles' (lock_hook)
Lock g_lock = Lock::Game;
std::uint32_t g_lock_rgb = 0xFF2A2A;
bool g_pulse = true;
bool g_mask = true; // target.bar.mask: the HP bar's white outline
bool g_outline = true; // target.bar.outline: VanaCore's outline of the bar, in place of the game's when that is left out for the width
bool g_arrow = true;         // target.arrow: the arrow over the target drawn at all
std::wstring g_arrow_image;  // target.arrow.image: the player's own picture in the arrow's place (a full path; empty: the game's)

bool g_searched = false;
char* g_manager = nullptr;
SetLocate g_set_locate = nullptr;
Resize g_resize = nullptr;
std::uintptr_t g_draw = 0;
char* const* g_target = nullptr; // the global holding the target struct (its +0x5C & 3: locked on)
bool g_hooked = false;
BarGame g_bar_game = nullptr;
BarDraw g_bar_draw = nullptr;
ResGetter g_res = nullptr;
LockDraw g_lock_game = nullptr;
ShapeDraw g_arrow_game = nullptr;
NameDraw g_name_game = nullptr;
TextFit g_text_fit = nullptr;
LineDraw g_line_draw = nullptr; // (target.name.oneline)
bool g_mark_hooked = false;
LockDraw g_mark_game = nullptr;
ResGetter g_mark_res = nullptr;
const void* g_mark_entity = nullptr; // edi at the mark's call: the target's entity

bool g_placed = false;    // the window has been put somewhere or made another size, to go back when it's off
float g_bar_scale = 1.0f; // the bar's width against the game's, for the window's width now

// The quads hidden for a moment: the window they're in and their corners' alphas as they were
struct Hidden {
    char* quad;
    std::uint8_t alpha[4];
};
char* g_hidden_window = nullptr;
std::vector<Hidden> g_hidden;
// The caption's quads' own places (their local x and y shorts, four each)
struct Moved {
    char* quad;
    short x[4];
    short y[4];
};
std::vector<Moved> g_caption;
bool g_bare = false; // the window's background is being left out (vanacore_gfx_set_hidden_windows)
bool g_recentre = false; // the window was resized: the caption is put in the middle once more

// The border for VanaCore's interface, as the window's place left it this frame
struct Pending {
    bool set = false;
    float left = 0, top = 0, right = 0, bottom = 0; // the frame, in the interface's units
};
Pending g_pending;
Pending g_bar_pending; // the HP bar's outline
// The picture in the arrow's place, as the game's draw of the arrow left it this frame: the arrow's point (the
// owner's +0xBC, +0xBE, in the interface's units) and whose picture (an addon's, else target.arrow.image)
struct ArrowPending {
    bool set = false;
    int x = 0, y = 0;
    gameui::Picture picture;
};
ArrowPending g_arrow_pending;
image::Texture g_arrow_texture; // the picture last asked for, decoded
std::wstring g_arrow_loaded;    // its file
bool g_arrow_failed = false;    // it couldn't be read (said once)
text::Sprite g_bar_outline;
int g_bar_outline_w = 0, g_bar_outline_h = 0, g_bar_outline_margin = 0;
text::Sprite g_glow;
int g_glow_w = 0, g_glow_h = 0, g_glow_margin = 0;
int g_glow_shape = 0, g_glow_corner = 0; // its corners (vanacore_gfx_window_corners: the shape, the size in pixels)
std::uint32_t g_glow_rgb = 0;
void* g_device = nullptr;
bool g_space = false;

template <typename T> T& at(void* base, std::ptrdiff_t offset)
{
    return *reinterpret_cast<T*>(static_cast<char*>(base) + offset);
}

std::uintptr_t call_target(std::uintptr_t call)
{
    std::int32_t rel = 0;
    std::memcpy(&rel, reinterpret_cast<const void*>(call + 1), 4);
    return call + 5 + rel;
}

bool is_call(std::uintptr_t at) { return *reinterpret_cast<const std::uint8_t*>(at) == 0xE8; }

double number(const wchar_t* key, double fallback, double low, double high, const wchar_t* ini)
{
    wchar_t text[32] = {};
    GetPrivateProfileStringW(L"client", key, L"", text, 32, ini);
    if (text[0] == 0)
        return fallback;
    wchar_t* end = nullptr;
    const double value = std::wcstod(text, &end);
    return end != text ? std::clamp(value, low, high) : fallback;
}

bool toggle(const wchar_t* key, bool fallback, const wchar_t* ini)
{
    return GetPrivateProfileIntW(L"client", key, fallback ? 1 : 0, ini) != 0;
}

std::uint32_t colour(const wchar_t* key, std::uint32_t fallback, const wchar_t* ini)
{
    wchar_t text[32] = {};
    GetPrivateProfileStringW(L"client", key, L"", text, 32, ini);
    const wchar_t* digits = text[0] == L'#' ? text + 1 : text;
    if (std::wcslen(digits) != 6)
        return fallback;
    wchar_t* end = nullptr;
    const unsigned long value = std::wcstoul(digits, &end, 16);
    return end != nullptr && *end == 0 ? static_cast<std::uint32_t>(value) : fallback;
}

// [client] target.arrow.image: the player's picture for the arrow, as a full path (one given relative is under VanaCore's
// own folder, the Core DLL's); empty for none
std::wstring picture_path(const wchar_t* ini)
{
    wchar_t text[MAX_PATH] = {};
    GetPrivateProfileStringW(L"client", L"target.arrow.image", L"", text, MAX_PATH, ini);
    std::wstring file = text;
    if (file.empty())
        return file;
    const bool full = file.size() > 2 && (file[1] == L':' || (file[0] == L'\\' && file[1] == L'\\'));
    if (full)
        return file;
    wchar_t path[MAX_PATH] = {};
    HMODULE self = nullptr;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(&picture_path), &self) &&
        GetModuleFileNameW(self, path, MAX_PATH) != 0) {
        if (wchar_t* slash = std::wcsrchr(path, L'\\'))
            slash[1] = 0;
        return path + file;
    }
    return file;
}

bool found()
{
    if (g_searched)
        return g_set_locate != nullptr;
    const HMODULE game = GetModuleHandleW(L"FFXiMain.dll");
    if (game == nullptr)
        return false;
    g_searched = true;
    const std::uintptr_t help = scan::find(game, kHelp);
    const std::uintptr_t match = scan::find(game, kSetLocate);
    if (help != 0 && match != 0) {
        const std::uintptr_t locate = match - kSetLocatePrologue;
        const auto* start = reinterpret_cast<const std::uint8_t*>(locate);
        if ((start[0] == 0x83 || start[0] == 0xE9) && start[kResizeAt] == 0xE8) {
            // the mov ecx,imm32's operand is the manager itself (a global object)
            std::uintptr_t manager = 0;
            std::memcpy(&manager, reinterpret_cast<const void*>(help + kManagerAt), sizeof manager);
            g_manager = reinterpret_cast<char*>(manager);
            g_set_locate = reinterpret_cast<SetLocate>(locate);
            g_resize = reinterpret_cast<Resize>(call_target(locate + kResizeAt));
        }
    }
    if (g_set_locate == nullptr)
        log::write(L"Target window: the game's window placing wasn't found in FFXiMain.dll: [client] target.* does nothing");
    if (const std::uintptr_t draw = scan::find(game, kDraw)) {
        g_draw = draw;
        const auto* flag = reinterpret_cast<const std::uint8_t*>(draw + kLockFlagAt);
        if (std::memcmp(flag, kLockFlagBytes, sizeof kLockFlagBytes) == 0 &&
            std::memcmp(flag + 6, kLockFlagTest, sizeof kLockFlagTest) == 0)
            g_target = *reinterpret_cast<char* const* const*>(flag + 2);
    } else {
        log::write(L"Target window: its drawing wasn't found in FFXiMain.dll: target.bar.*, target.width's bar and "
                   L"target.lock do nothing");
    }
    g_space = uispace::find(game);
    return g_set_locate != nullptr;
}

// A call's offset, written in one store (the game's thread runs this code)
bool write_offset(std::uintptr_t call, std::uintptr_t target)
{
    const std::uintptr_t where = call + 1;
    DWORD protect = 0;
    if (!VirtualProtect(reinterpret_cast<void*>(where), 4, PAGE_EXECUTE_READWRITE, &protect)) {
        log::write(L"Target window: the game's code at %p couldn't be written (error %lu)", reinterpret_cast<void*>(where),
                   GetLastError());
        return false;
    }
    *reinterpret_cast<volatile std::int32_t*>(where) = static_cast<std::int32_t>(target - (call + 5));
    VirtualProtect(reinterpret_cast<void*>(where), 4, protect, &protect);
    FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<const void*>(where), 4);
    return true;
}

constexpr float kTrackWidth = 72.0f, kFillWidth = 64.0f; // the bar's track and fill pictures in the menu resource (8 tall)

// The game's outline of the bar (its track) is left out when asked and when the window is another width: stretched it looks
// wrong, and VanaCore draws one of its own
bool hide_track() { return !g_mask || g_bar_scale != 1.0f; }

volatile bool g_bar_seen = false, g_lock_seen = false; // the game drew the bar / the lock picture (so the window is shown)

// What's in the window moved down from where the game has it: the name by the padding and one spacing (below the title),
// the bar by the padding and two (below the name)
int name_down() { return g_pad_y + g_spacing; }
int bar_down() { return g_pad_y + 2 * g_spacing; }

// The HP bar, with the width and place wanted. Untouched (the game's own call) while neither is asked for.
void __cdecl bar_hook(int x, int y, int max, void* hp)
{
    g_bar_seen = true;
    x += g_pad_x, y += bar_down();
    if (g_bar_scale == 1.0f && g_bar_x == 0 && g_bar_y == 0 && g_mask) {
        g_bar_game(x, y, max, hp);
        return;
    }
    void* const* resource = g_res(1);
    char* object = resource != nullptr ? static_cast<char*>(*resource) : nullptr;
    if (object == nullptr) {
        g_bar_game(x, y, max, hp);
        return;
    }
    // f6 scales the fill (cut to the HP), f8 the white outline (the track picture), in width; f7 and f9 their heights.
    // The fill starts 4 units inside the track, so it is scaled to stop 4 short of its end. The track is left out (its
    // width 0) when target.bar.mask=0 and when the window is another width, whose own outline (draw below) goes in its place.
    const float track = hide_track() ? 0.0f : g_bar_scale;
    const float fill = (kTrackWidth * g_bar_scale - 8.0f) / kFillWidth;
    g_bar_draw(x + g_bar_x, y + g_bar_y, at<void*>(object, kResShapeB), at<void*>(object, kResShapeA), max, hp, fill, 1.0f,
               track, 1.0f);
}

char* find_window(const char (&name)[17] = kLayout);

bool is_named(const char* quad, const char (&name)[17])
{
    return std::memcmp(quad + kQuadName, name, 16) == 0;
}

// Each quad of a shape (a window picture's, the lock's)
template <typename Fn> void each_part(char* shape, Fn fn)
{
    const int count = at<int>(shape, kPartCount);
    int parts = 0;
    for (char* part = at<char*>(shape, kParts); part != nullptr && parts < count && parts < 128; part = at<char*>(part, 0), ++parts) {
        char* quad = at<char*>(part, kNodeItem);
        if (quad != nullptr)
            fn(quad);
    }
}

// The Target window's frame (left, top, right, bottom), false while it isn't there
bool window_frame(short (&frame)[4])
{
    __try {
        char* window = g_manager != nullptr ? find_window() : nullptr;
        if (window == nullptr)
            return false;
        for (int i = 0; i < 4; ++i)
            frame[i] = at<short>(window, kWinFrame + 2 * i);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// A lock picture's piece's places as they were (its four corners' x and y) and its corners' alphas
struct LockPiece {
    char* quad;
    short x[4];
    short y[4];
    std::uint8_t alpha[4];
};
constexpr int kLockPieces = 32;

// The lock's picture laid out for a window `wide` x `tall` units (its own size is its glows' right and bottom edges, the
// game's 112 x 44) and its "Locked" moved by target.lock.label.x / .y, or left out (target.lock.label=0): the corners' glows
// stretched with the window, the bars beside it kept at its sides and as tall as it, "Locked" and its brackets kept in its
// bottom middle. The pieces as they were into `saved`; how many there are
int stretch_lock(char* shape, int wide, int tall, LockPiece (&saved)[kLockPieces])
{
    int span = 0, span_h = 0;
    each_part(shape, [&](char* quad) {
        if (is_named(quad, kLockGlowTexture))
            for (int i = 0; i < 4; ++i) {
                span = std::max(span, static_cast<int>(at<short>(quad, kLocalX + 2 * i)));
                span_h = std::max(span_h, static_cast<int>(at<short>(quad, kLocalY + 2 * i)));
            }
    });
    const int grow = wide - span, grow_h = tall - span_h;
    if (span <= 0 || span_h <= 0 || (grow == 0 && grow_h == 0 && g_lock_label_x == 0 && g_lock_label_y == 0 && g_lock_text))
        return 0;
    int count = 0;
    each_part(shape, [&](char* quad) {
        if (count == kLockPieces)
            return;
        LockPiece& piece = saved[count++];
        piece.quad = quad;
        short* x = &at<short>(quad, kLocalX);
        short* y = &at<short>(quad, kLocalY);
        std::copy(x, x + 4, piece.x);
        std::copy(y, y + 4, piece.y);
        for (int i = 0; i < 4; ++i)
            piece.alpha[i] = at<std::uint8_t>(quad, kCorners + i * kCorner + kAlpha);
        const bool glow = is_named(quad, kLockGlowTexture), bar = is_named(quad, kLockBarTexture);
        const bool right = x[0] + x[1] + x[2] + x[3] > 2 * span; // (its middle right of the picture's)
        for (int i = 0; i < 4; ++i) {
            if (glow || bar) {
                x[i] = static_cast<short>(glow ? std::lround(x[i] * static_cast<double>(wide) / span) : x[i] + (right ? grow : 0));
                y[i] = static_cast<short>(std::lround(y[i] * static_cast<double>(tall) / span_h));
            } else {
                x[i] = static_cast<short>(x[i] + grow / 2 + g_lock_label_x);
                y[i] = static_cast<short>(y[i] + grow_h + g_lock_label_y);
                if (!g_lock_text)
                    at<std::uint8_t>(quad, kCorners + i * kCorner + kAlpha) = 0;
            }
        }
    });
    return count;
}

// The lock's picture: the game's, or left to the glow / nothing. The game's is laid out for the window's width (stretch_lock)
// and, with the windows' corners cut or round, kept inside the window's shape (its bars beside the window round it); put back
// once it's drawn, as it's the game's own picture
void __fastcall lock_hook(void* shape, void*, int x, int y, std::uint32_t tint, int a, int b)
{
    g_lock_seen = true;
    if (g_lock != Lock::Game)
        return;
    short frame[4] = {};
    const bool here = shape != nullptr && window_frame(frame) && frame[0] == x && frame[1] == y;
    LockPiece saved[kLockPieces];
    const int pieces = here ? stretch_lock(static_cast<char*>(shape), frame[2] - frame[0], frame[3] - frame[1], saved) : 0;
    const bool clip = here && vanacore_gfx_window_corners(nullptr) != 0;
    if (clip) {
        // (the game's vertices lie half a unit up and left of its units)
        const float rect[4] = {frame[0] - 0.5f, frame[1] - 0.5f, frame[2] - 0.5f, frame[3] - 0.5f};
        vanacore_gfx_clip_to_corners(rect);
    }
    // "Locked" is a title's letters, drawn in the window: the titles' True Font as it was, not the window's own title's
    // (target.title.font, VANA_FONT_TARGET_TITLE: d3d8_chatfont.h cf_what)
    const int area = vanacore_gfx_text_area(VANA_FONT_TITLES);
    if (area != VANA_FONT_TARGET)
        vanacore_gfx_text_area(area);
    g_lock_game(shape, x, y, tint, a, b);
    if (area == VANA_FONT_TARGET)
        vanacore_gfx_text_area(area);
    if (clip)
        vanacore_gfx_clip_to_corners(nullptr);
    for (int i = 0; i < pieces; ++i) {
        std::copy(saved[i].x, saved[i].x + 4, &at<short>(saved[i].quad, kLocalX));
        std::copy(saved[i].y, saved[i].y + 4, &at<short>(saved[i].quad, kLocalY));
        for (int c = 0; c < 4; ++c)
            at<std::uint8_t>(saved[i].quad, kCorners + c * kCorner + kAlpha) = saved[i].alpha[c];
    }
}

#include "targetwin_name.h" // target.font: the name drawn whole as VanaCore's text in its True Font

// A name's width in the game's letters, or -1 when it's wider than `room` (the game wraps it): the narrowest width the game's
// count of the letters that fit (FUN_101C7E30) takes the whole name in
int name_width(const char* text, int room)
{
    char line[kNameMost + 1] = {};
    const std::size_t length = strnlen(text, kNameMost);
    std::memcpy(line, text, length);
    if (length == 0 || room <= 0 || g_text_fit(room, line) < static_cast<int>(length))
        return -1;
    int low = 0, high = room; // (the whole name fits in `high`, not in `low`)
    while (high - low > 1) {
        const int mid = (low + high) / 2;
        (g_text_fit(mid, line) >= static_cast<int>(length) ? high : low) = mid;
    }
    return high;
}

// The target's name, where target.name.align, .x and .y put it. The game's is 5 in from the window's left (`x`), `width`
// the window's less 13; a name too long for that, which the game wraps onto a second line, stays at the left. The padding
// takes its room in from both sides. The game works out all three in 16-bit registers and pushes them whole: their high
// halves are whatever was left there (width 0x31B0011F for 287, seen live 2026-11-07), which its draw never reads, so
// they're taken as the shorts they are (centring on the whole value put the name off the screen)
int __cdecl name_hook(int width, int x, int y, const char* text, std::uint32_t colour, int drop, int wrap)
{
    width = static_cast<short>(width) - 2 * g_pad_x, x = static_cast<short>(x) + g_pad_x, y = static_cast<short>(y) + name_down();
    // its True Font: the whole name VanaCore's text, on one line (6 lower with `drop`, as the game's one-line draw puts one)
    if (name_whole(x, y + g_name_y + (drop != 0 ? kLineDrop : 0), width, text, colour))
        return x;
    int at_x = x;
    if (g_name_align != 0 && text != nullptr && g_text_fit != nullptr) {
        int wide = name_width(text, width);
        // (with its True Font, the name's own width: its letters are laid by the font's advances from where the game's start)
        const float own = wide >= 0 ? chatfont::text_width(VANA_FONT_TARGET, text) : -1.0f;
        if (own >= 0.0f)
            wide = static_cast<int>(std::lround(own));
        const int left = x - kNameLeft, window = width + kNameRoom;
        if (wide >= 0)
            at_x = g_name_align == 1 ? left + (window - wide) / 2 : left + window - kNameLeft - wide;
    }
    // target.name.oneline: a name with a ':' drawn by the game's one-line draw, as a name without one (24 letters, as the
    // name draw copies); a name the game would wrap given room for all of it, from the left (it runs past the window)
    if (g_one_line && text != nullptr && g_line_draw != nullptr && std::strchr(text, ':') != nullptr) {
        char line[kNameMost + 1] = {};
        std::memcpy(line, text, strnlen(text, kNameMost));
        return g_line_draw(at_x + g_name_x, y + g_name_y + (drop != 0 ? kLineDrop : 0), line, colour);
    }
    if (g_one_line && text != nullptr && g_text_fit != nullptr && name_width(text, width) < 0)
        width = kOneLineRoom;
    return g_name_game(width, at_x + g_name_x, y + g_name_y, text, colour, drop, wrap);
}

// The arrow over the target: left out ([client] target.arrow=0, an addon's vana.gameui.hide('target_arrow')), or kept
// for a picture in its place (an addon's vana.gameui.image('target_arrow', ...), else the player's target.arrow.image),
// which draw() puts at its point in VanaCore's interface pass. Otherwise the game's own.
void __fastcall arrow_hook(void* shape, void*, int x, int y, float sx, float sy, std::uint32_t colour, int centred, int blend,
                           int skip)
{
    if (!g_arrow || gameui::hidden("target_arrow"))
        return;
    gameui::Picture picture;
    if (!gameui::pictured("target_arrow", picture)) {
        if (g_arrow_image.empty()) {
            g_arrow_game(shape, x, y, sx, sy, colour, centred, blend, skip);
            return;
        }
        picture.file = g_arrow_image;
    }
    g_arrow_pending.set = true;
    g_arrow_pending.x = x;
    g_arrow_pending.y = y;
    g_arrow_pending.picture = picture;
}

// The target's status mark: a staff member's GM mark as their picture ([graphics] nameplates.gm, gmicons.hpp), drawn in
// VanaCore's interface pass; anything else the game's
void __fastcall mark_hook(void* shape, void*, int x, int y, std::uint32_t colour, int a, int b)
{
    void* const* const res = g_mark_res(0);
    const void* const* const shapes = res != nullptr ? static_cast<const void* const*>(res[0]) : nullptr;
    if (!gmicons::target_mark(g_mark_entity, shape, shapes, x, y))
        g_mark_game(shape, x, y, colour, a, b);
}

// In place of the mark's call: the entity (edi) kept, then the hook as the game called it (its ret 14h returns to the game)
__declspec(naked) void mark_stub()
{
    __asm {
        mov g_mark_entity, edi
        jmp mark_hook
    }
}

void install_mark()
{
    g_mark_hooked = true;
    const std::uintptr_t mark = g_draw + kMarkCall, getter = g_draw + kMarkGetterCall;
    if (g_draw == 0 || !is_call(mark) || !is_call(getter) ||
        std::memcmp(reinterpret_cast<const void*>(mark - sizeof kMarkBefore), kMarkBefore, sizeof kMarkBefore) != 0) {
        log::write(L"Target window: its target's mark isn't drawn as expected: a GM's stays the game's ([graphics] nameplates.gm)");
        return;
    }
    g_mark_res = reinterpret_cast<ResGetter>(call_target(getter));
    g_mark_game = reinterpret_cast<LockDraw>(call_target(mark));
    write_offset(mark, reinterpret_cast<std::uintptr_t>(&mark_stub));
    log::write(L"Target window: a GM's mark is their staff picture ([graphics] nameplates.gm)");
}

// Whether the arrow is to be hidden or drawn as a picture, by the player or an addon
bool arrow_wanted()
{
    gameui::Picture picture;
    return !g_arrow || !g_arrow_image.empty() || gameui::hidden("target_arrow") || gameui::pictured("target_arrow", picture);
}

void install_hooks()
{
    g_hooked = true;
    if (g_draw == 0)
        return;
    const std::uintptr_t arrow = g_draw + kArrowCall;
    if (is_call(arrow) && std::memcmp(reinterpret_cast<const void*>(arrow - sizeof kArrowBefore), kArrowBefore, sizeof kArrowBefore) == 0) {
        g_arrow_game = reinterpret_cast<ShapeDraw>(call_target(arrow));
        write_offset(arrow, reinterpret_cast<std::uintptr_t>(&arrow_hook));
    } else {
        log::write(L"Target window: the arrow's drawing isn't as expected: target.arrow and target.arrow.image do nothing");
    }
    const std::uintptr_t name = g_draw + kNameCall, script_name = g_draw + kScriptNameCall;
    if (is_call(name) && is_call(script_name) && call_target(name) == call_target(script_name) &&
        is_call(call_target(name) + kNameFitCall)) {
        g_name_game = reinterpret_cast<NameDraw>(call_target(name));
        g_text_fit = reinterpret_cast<TextFit>(call_target(call_target(name) + kNameFitCall));
        const std::uintptr_t draw = call_target(name), colon = draw + kColonPush; // (push offset ":")
        if (is_call(draw + kLineDrawCall) && *reinterpret_cast<const std::uint8_t*>(colon) == 0x68 &&
            std::strcmp(*reinterpret_cast<const char* const*>(colon + 1), ":") == 0)
            g_line_draw = reinterpret_cast<LineDraw>(call_target(draw + kLineDrawCall));
        write_offset(name, reinterpret_cast<std::uintptr_t>(&name_hook));
    } else {
        log::write(L"Target window: the name's drawing isn't as expected: target.name.* does nothing");
    }
    const std::uintptr_t bar_a = g_draw + kBarCallA, bar_b = g_draw + kBarCallB, lock = g_draw + kLockCall;
    if (!is_call(bar_a) || !is_call(bar_b) || !is_call(lock) || call_target(bar_a) != call_target(bar_b)) {
        log::write(L"Target window: its drawing isn't as expected: target.bar.*, target.width's bar and target.lock do nothing");
        return;
    }
    const std::uintptr_t bar = call_target(bar_a);
    if (!is_call(bar + kBarGetter) || !is_call(bar + kBarWrapperCall)) {
        log::write(L"Target window: the HP bar's drawing isn't as expected: target.bar.* does nothing");
    } else {
        const std::uintptr_t wrapper = call_target(bar + kBarWrapperCall);
        if (is_call(wrapper + kWrapperBarCall)) {
            g_res = reinterpret_cast<ResGetter>(call_target(bar + kBarGetter));
            g_bar_draw = reinterpret_cast<BarDraw>(call_target(wrapper + kWrapperBarCall));
            g_bar_game = reinterpret_cast<BarGame>(bar);
            write_offset(bar_a, reinterpret_cast<std::uintptr_t>(&bar_hook));
            write_offset(bar_b, reinterpret_cast<std::uintptr_t>(&bar_hook));
        }
    }
    g_lock_game = reinterpret_cast<LockDraw>(call_target(lock));
    write_offset(lock, reinterpret_cast<std::uintptr_t>(&lock_hook));
    log::write(L"Target window: its drawing hooked (bar %d, lock %d, arrow %d, name %d)", g_bar_game != nullptr ? 1 : 0,
               g_lock_game != nullptr ? 1 : 0, g_arrow_game != nullptr ? 1 : 0, g_name_game != nullptr ? 1 : 0);
}

// Whether the window's drawing is to be hooked: its bar, lock, arrow or name anything but the game's, or the windows'
// corners cut or round (the lock's picture kept to them)
bool hooks_wanted()
{
    return g_bar_x != 0 || g_bar_y != 0 || !g_mask || g_width > 0 || g_lock != Lock::Game || arrow_wanted() || g_name_align != 0 ||
           g_name_x != 0 || g_name_y != 0 || g_lock_label_x != 0 || g_lock_label_y != 0 || vanacore_gfx_window_corners(nullptr) != 0 ||
           !g_lock_text || g_pad_x != 0 || g_pad_y != 0 || g_spacing != 0 || g_title_own || g_one_line || name_font_on();
}

// The Target window in the manager's list, or null
char* find_window(const char (&name)[17])
{
    int count = 0;
    for (char* node = at<char*>(g_manager, 0); node != nullptr && count < 512; node = at<char*>(node, 0), ++count) {
        char* window = at<char*>(node, 0x10);
        if (window == nullptr || at<std::uint8_t>(node, 0x14) != 0 || at<std::uint8_t>(window, kWinDeleting) != 0)
            continue;
        const char* layout = at<char*>(window, kWinLayout);
        if (layout != nullptr && std::memcmp(layout + kLayoutName, name, 16) == 0)
            return window;
    }
    return nullptr;
}

// The window given a name and HP bar of its own by an event script (the hooked fish's): drawn whatever hides it
bool scripted(char* window)
{
    char* owner = at<char*>(window, kWinOwner);
    return owner != nullptr && at<std::uint8_t>(owner, kOwnerScripted) != 0;
}

// Where the game has the window, `wide` x `height`: its right edge on the party list's, its bottom 2 above the list's top
// (the game puts it there itself as the list changes; its place as opened is for a full list)
void game_place(char* window, int wide, int height, int& x, int& y)
{
    char* party = find_window(kPartyLayout);
    if (party != nullptr) {
        x = at<short>(party, kWinFrame + 4) - wide;
        y = at<short>(party, kWinFrame + 2) - height - 2;
    } else {
        const int open_l = at<short>(window, kWinOpened), open_w = at<short>(window, kWinOpened + 4) - open_l;
        x = open_l + open_w - wide;
        y = at<short>(window, kWinOpened + 2);
    }
}

// Where it's put and how wide: the game's place and width unless target.move / target.width say, or an addon does
// (vana.gameui.move('target_window', x, y): its x and y in place of target.x and .y while it holds it). Without either a wider window grows to the left (its right edge stays), as it's at the screen's edge.
void place(char* window)
{
    const int left = at<short>(window, kWinFrame), top = at<short>(window, kWinFrame + 2);
    const int width = at<short>(window, kWinFrame + 4) - left, height = at<short>(window, kWinFrame + 6) - top;
    const int open_w = at<short>(window, kWinOpened + 4) - at<short>(window, kWinOpened);
    const int open_h = at<short>(window, kWinOpened + 6) - at<short>(window, kWinOpened + 2);
    if (open_w <= 0 || width <= 0)
        return;
    double move_x = g_x, move_y = g_y;
    const bool moved = gameui::placed("target_window", move_x, move_y) || g_move;
    const bool hide = wants_hidden() && !scripted(window); // (the fish's HP: shown, and put back if it was hidden)
    const bool grown = g_pad_x != 0 || g_pad_y != 0 || g_spacing != 0;
    const bool wanted = moved || hide || g_width > 0 || grown;
    if (!wanted && !g_placed)
        return;
    // its width (the game's, or target.width) and height (the game's) with the padding round them and the spacing in them
    const int wide = (g_width > 0 ? g_width : open_w) + 2 * g_pad_x;
    const int tall = open_h > 0 ? open_h + 2 * g_pad_y + 2 * g_spacing : height;
    int want_x, want_y;
    if (hide) {
        want_x = want_y = kHiddenAt;
    } else if (moved) {
        const int area_left = at<short>(g_manager, kAreaLeft), area_top = at<short>(g_manager, kAreaLeft + 2);
        const int area_w = at<short>(g_manager, kAreaLeft + 4) - area_left, area_h = at<short>(g_manager, kAreaLeft + 6) - area_top;
        if (area_w <= 0 || area_h <= 0)
            return;
        want_x = area_left + static_cast<int>(std::lround(area_w * move_x / 100.0)) - wide / 2;
        want_y = area_top + static_cast<int>(std::lround(area_h * move_y / 100.0));
        want_x = std::clamp(want_x, area_left, std::max(area_left, area_left + area_w - wide));
        want_y = std::clamp(want_y, area_top, std::max(area_top, area_top + area_h - tall));
    } else {
        game_place(window, wide, tall, want_x, want_y);
    }
    g_placed = wanted;
    if (width != wide || height != tall || left != want_x || top != want_y) {
        if (width != wide || height != tall)
            g_resize(window, left, top, wide, tall, 1, 0, 0), g_recentre = true; // the game moves the caption by the growth
        g_set_locate(window, want_x, want_y); // also tells its handler the new rect (the name and bar are placed from it)
    }
}

// The bar's width against the game's: the window's width less what's around the bar's track
void bar_scale(char* window)
{
    constexpr float kTrack = 72.0f; // the bar's track picture's width in the menu resource
    const int open_w = at<short>(window, kWinOpened + 4) - at<short>(window, kWinOpened);
    const int wide = at<short>(window, kWinFrame + 4) - at<short>(window, kWinFrame) - 2 * g_pad_x; // (inside the padding)
    float scale = 1.0f;
    if (wide > 0 && wide != open_w) {
        const float margin = std::max(4.0f, open_w - kBarLeft - kTrack); // what's right of the track in the game's width
        scale = std::max(0.25f, (wide - kBarLeft - margin) / kTrack);
    }
    g_bar_scale = scale;
}

// target.background / target.label: the quads of the window's picture made see-through (their alphas kept, to put back)
template <typename Fn> void each_quad(char* window, Fn fn)
{
    int pictures = 0;
    for (char* node = at<char*>(window, kWinPictures); node != nullptr && pictures < 16; node = at<char*>(node, 0), ++pictures) {
        char* picture = at<char*>(node, kNodeItem);
        if (picture == nullptr || at<short>(picture, 0) != 0)
            continue;
        if (char* shape = at<char*>(picture, 8))
            each_part(shape, fn);
    }
}

bool caption_quad(const char* quad) { return is_named(quad, kCaptionTexture) || is_named(quad, kLetterTexture); }

// The caption's quads made see-through (target.label=0), their alphas kept to put back
void caption_alpha(char* quad, bool hide)
{
    std::uint8_t* alpha[4];
    for (int i = 0; i < 4; ++i)
        alpha[i] = reinterpret_cast<std::uint8_t*>(quad + kCorners + i * kCorner + kAlpha);
    auto saved = std::find_if(g_hidden.begin(), g_hidden.end(), [&](const Hidden& h) { return h.quad == quad; });
    if (hide) {
        if (saved == g_hidden.end()) {
            if (*alpha[0] == 0 && *alpha[3] == 0)
                return;
            Hidden entry{quad, {*alpha[0], *alpha[1], *alpha[2], *alpha[3]}};
            g_hidden.push_back(entry);
        }
        for (int i = 0; i < 4; ++i)
            *alpha[i] = 0;
    } else if (saved != g_hidden.end()) {
        for (int i = 0; i < 4; ++i)
            *alpha[i] = saved->alpha[i];
        g_hidden.erase(saved);
    }
}

// The caption put in the middle of the window plus target.label.x (the game's resize shifts it by the whole growth, as
// it's anchored to the right) and target.label.y. Its horizontal place is found from where its quads are now (no place
// of its own is kept: one taken after a resize would be the resize's); its vertical one from where they were first seen.
void quads(char* window)
{
    if (window != g_hidden_window) { // a window made again: its quads are new, with their own alphas and places
        g_hidden.clear();
        g_caption.clear();
        g_hidden_window = window;
    }
    const bool recentre = g_recentre;
    g_recentre = false;
    if (g_hidden.empty() && g_label && g_label_x == 0 && g_label_y == 0 && g_caption.empty() && g_width == 0 && !g_placed &&
        !recentre && g_pad_y == 0)
        return;
    const int wide = at<short>(window, kWinFrame + 4) - at<short>(window, kWinFrame);
    std::vector<char*> caption;
    int lowest = 32767, highest = -32768;
    each_quad(window, [&](char* quad) {
        if (!caption_quad(quad))
            return;
        caption.push_back(quad);
        for (int i = 0; i < 4; ++i) {
            const int x = at<short>(quad, kLocalX + 2 * i);
            lowest = std::min(lowest, x);
            highest = std::max(highest, x);
        }
    });
    if (caption.empty())
        return;
    // the game's caption is 2 units right of the middle of its 112 wide window
    const int shift = wide / 2 + 2 + g_label_x - (lowest + highest) / 2;
    for (char* quad : caption) {
        auto saved = std::find_if(g_caption.begin(), g_caption.end(), [&](const Moved& m) { return m.quad == quad; });
        if (saved == g_caption.end()) {
            Moved entry{quad, {}, {}};
            for (int i = 0; i < 4; ++i)
                entry.y[i] = at<short>(quad, kLocalY + 2 * i);
            g_caption.push_back(entry);
            saved = g_caption.end() - 1;
        }
        for (int i = 0; i < 4; ++i) {
            at<short>(quad, kLocalX + 2 * i) = static_cast<short>(at<short>(quad, kLocalX + 2 * i) + shift);
            at<short>(quad, kLocalY + 2 * i) = static_cast<short>(saved->y[i] + g_label_y + g_pad_y);
        }
        caption_alpha(quad, !g_label);
    }
}

bool locked()
{
    char* target = g_target != nullptr ? *g_target : nullptr;
    return target != nullptr && (at<std::uint8_t>(target, 0x5C) & 3) != 0;
}

bool needed()
{
    double x, y; // an addon's place (vana.gameui.move)
    return g_hide || g_move || g_width > 0 || g_placed || gameui::placed("target_window", x, y) || !g_background || g_bare || !g_label ||
           !g_hidden.empty() || !g_caption.empty() || g_label_x != 0 || g_label_y != 0 || g_hooked || hooks_wanted();
}

// target.background=0: the graphics layer draws the window's background and frame pieces under its rectangle with no alpha
void bare(char* window)
{
    if (g_background) {
        if (g_bare) {
            vanacore_gfx_set_hidden_windows(nullptr, 0);
            g_bare = false;
        }
        return;
    }
    float rect[4] = {};
    if (window != nullptr) {
        rect[0] = at<short>(window, kWinFrame), rect[1] = at<short>(window, kWinFrame + 2);
        rect[2] = at<short>(window, kWinFrame + 4), rect[3] = at<short>(window, kWinFrame + 6);
    }
    vanacore_gfx_set_hidden_windows(rect, window != nullptr ? 1 : 0);
    g_bare = window != nullptr;
}

void frame()
{
    // whether the game drew the window's bar and its lock picture since the last frame: with a menu up it draws neither, and
    // VanaCore's outline and glow go with it
    const bool bar_seen = g_bar_seen, lock_seen = g_lock_seen;
    g_bar_seen = g_lock_seen = false;
    char* window = find_window();
    bare(window);
    // (the arrow's hook before the window is there: the game makes the window as something is targeted and draws the
    // arrow in that frame)
    if (!g_hooked && hooks_wanted())
        install_hooks();
    if (window == nullptr)
        return;
    if (!g_space)
        g_space = uispace::find(GetModuleHandleW(L"FFXiMain.dll"));
    place(window);
    bar_scale(window);
    quads(window);
    // left out of the window draw by target.hide or by an addon's hide (vana.gameui.hide('target_window')): nothing of
    // VanaCore's drawn over it either
    const bool hidden = wants_hidden() && !scripted(window);
    if (g_lock == Lock::Glow && g_space && lock_seen && !hidden) {
        g_pending.set = true;
        g_pending.left = at<short>(window, kWinFrame);
        g_pending.top = at<short>(window, kWinFrame + 2);
        g_pending.right = at<short>(window, kWinFrame + 4);
        g_pending.bottom = at<short>(window, kWinFrame + 6);
    }
    // the bar's own outline where the game's is left out for the window's width
    if (g_outline && g_mask && g_space && g_bar_scale != 1.0f && bar_seen && !hidden) {
        g_bar_pending.set = true;
        // the track is drawn at the bar's x, y; the fill (6 tall, 8 shorter than the track) 4 units in and 1 down from
        // it: the outline 3 units round the fill's sides and 2 above and below it
        const float fill_left = static_cast<float>(at<short>(window, kWinFrame) + kBarLeft + g_pad_x + g_bar_x + 4);
        const float fill_top = static_cast<float>(at<short>(window, kWinFrame + 2) + kBarTop + bar_down() + g_bar_y + 1);
        g_bar_pending.left = fill_left - 3.0f;
        g_bar_pending.top = fill_top - 2.0f;
        g_bar_pending.right = fill_left + kTrackWidth * g_bar_scale - 8.0f + 3.0f;
        g_bar_pending.bottom = fill_top + kFillHeight + 2.0f;
    }
}

// The signed distance (negative inside) from a point `x`, `y` off a box's middle to the box's edge, the box `half_w` x
// `half_h` each way, its corners as the windows' are (window.corners, vanacore_gfx_window_corners): 1 cut across `corner`
// from each, 2 rounded that much, else rounded a little (4 at most); `corner` half the box's height or width at most
float box_distance(float x, float y, float half_w, float half_h, int shape, float corner)
{
    const float most = std::min(half_w, half_h);
    const float c = std::min(shape != 0 ? corner : std::min(4.0f, most * 0.4f), most);
    if (shape == 1) {
        const float qx = std::fabs(x) - half_w, qy = std::fabs(y) - half_h;
        const float ox = std::max(qx, 0.0f), oy = std::max(qy, 0.0f);
        const float box = std::sqrt(ox * ox + oy * oy) + std::min(std::max(qx, qy), 0.0f);
        return std::max(box, (qx + qy + c) * 0.70710678f); // (the line across the corner)
    }
    const float px = std::fabs(x) - (half_w - c), py = std::fabs(y) - (half_h - c);
    const float ox = std::max(px, 0.0f), oy = std::max(py, 0.0f);
    return std::sqrt(ox * ox + oy * oy) + std::min(std::max(px, py), 0.0f) - c;
}

// The glow's picture: a border `width` x `height` inside `margin` pixels of glow, `rgb` (0xRRGGBB) with a brighter core, its
// corners as the windows' (`shape`, `corner` pixels: box_distance). Straight alpha.
std::vector<std::uint32_t> glow_image(int width, int height, int margin, std::uint32_t rgb, int shape, float corner)
{
    const int w = width + margin * 2, h = height + margin * 2;
    std::vector<std::uint32_t> pixels(static_cast<std::size_t>(w) * h);
    const float half_w = width * 0.5f, half_h = height * 0.5f;
    const float sigma = std::max(2.0f, margin * 0.45f);
    const float base[3] = {static_cast<float>((rgb >> 16) & 0xFF), static_cast<float>((rgb >> 8) & 0xFF),
                           static_cast<float>(rgb & 0xFF)};
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const float d = box_distance(x + 0.5f - w * 0.5f, y + 0.5f - h * 0.5f, half_w, half_h, shape, corner);
            // the glow: strongest at the edge, gone `margin` out; a fainter one inside it
            const float spread = d > 0 ? sigma : sigma * 0.5f;
            const float halo = std::exp(-(d * d) / (2.0f * spread * spread)) * (d > 0 ? 0.85f : 0.45f);
            // the core: a line a little under 2 pixels wide, softly edged
            const float core = std::clamp(1.4f - std::fabs(d), 0.0f, 1.0f);
            const float alpha = std::clamp(halo + core * (1.0f - halo), 0.0f, 1.0f);
            if (alpha <= 0.003f)
                continue;
            const float whiten = core * 0.55f; // the core is the colour pushed toward white
            std::uint32_t pixel = static_cast<std::uint32_t>(std::lround(alpha * 255)) << 24;
            for (int c = 0; c < 3; ++c) {
                const float value = base[c] + (255.0f - base[c]) * whiten;
                pixel |= static_cast<std::uint32_t>(std::lround(std::clamp(value, 0.0f, 255.0f))) << (16 - c * 8);
            }
            pixels[static_cast<std::size_t>(y) * w + x] = pixel;
        }
    }
    return pixels;
}

// The bar's outline: a thin rounded white line with a faint glow outside it (less than the lock-on's), `width` x
// `height` inside `margin` pixels. Straight alpha.
std::vector<std::uint32_t> outline_image(int width, int height, int margin)
{
    const int w = width + margin * 2, h = height + margin * 2;
    std::vector<std::uint32_t> pixels(static_cast<std::size_t>(w) * h);
    const float radius = std::min(3.0f, std::min(width, height) * 0.4f);
    const float half_w = width * 0.5f, half_h = height * 0.5f;
    const float sigma = std::max(1.5f, margin * 0.5f);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const float px = std::fabs(x + 0.5f - w * 0.5f) - (half_w - radius);
            const float py = std::fabs(y + 0.5f - h * 0.5f) - (half_h - radius);
            const float ox = std::max(px, 0.0f), oy = std::max(py, 0.0f);
            const float d = std::sqrt(ox * ox + oy * oy) + std::min(std::max(px, py), 0.0f) - radius;
            const float halo = d > 0 ? std::exp(-(d * d) / (2.0f * sigma * sigma)) * 0.3f : 0.0f;
            const float core = std::clamp(1.1f - std::fabs(d), 0.0f, 0.9f);
            const float alpha = std::max(halo, core);
            if (alpha <= 0.003f)
                continue;
            pixels[static_cast<std::size_t>(y) * w + x] =
                (static_cast<std::uint32_t>(std::lround(alpha * 255)) << 24) | 0x00FFFFFFu;
        }
    }
    return pixels;
}

} // namespace

bool hide(const void* ctrl)
{
    if (g_set_locate == nullptr || ctrl == nullptr)
        return false;
    __try {
        const char* layout = *reinterpret_cast<const char* const*>(static_cast<const char*>(ctrl) + kWinLayout);
        if (layout == nullptr || std::memcmp(layout + kLayoutName, kLayout, 16) != 0 || !wants_hidden())
            return false;
        char* window = const_cast<char*>(static_cast<const char*>(ctrl));
        if (scripted(window)) {
            // an event script's own name and HP bar (the hooked fish's): drawn whatever hides the window, where the game has
            // it. A window hidden before the script took it (the fishing script opens it only if it isn't open) is put back
            // here: on_frame's place doesn't run for an addon's hide alone (needed() doesn't count it)
            const int left = at<short>(window, kWinFrame), top = at<short>(window, kWinFrame + 2);
            const int width = at<short>(window, kWinFrame + 4) - left, height = at<short>(window, kWinFrame + 6) - top;
            if (left <= kHiddenAt && width > 0 && height > 0) {
                int x = 0, y = 0;
                game_place(window, width, height, x, y);
                g_set_locate(window, x, y);
                log::write(L"Target window: shown for an event's own HP bar (a hooked fish's) while hidden, back at %d, %d", x, y);
            }
            return true;
        }
        if (at<short>(window, kWinFrame) > kHiddenAt) {
            if (!g_hide_logged) {
                g_hide_logged = true;
                log::write(L"Target window: put off the screen, the arrow over the target kept ([client] target.hide=1)");
            }
            g_set_locate(window, kHiddenAt, kHiddenAt); // also tells its owner the new rect (the name and bar are placed from it)
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return false;
}

void before_draw(const void* ctrl)
{
    if (ctrl == nullptr)
        return;
    __try {
        const char* layout = *reinterpret_cast<const char* const*>(static_cast<const char*>(ctrl) + kWinLayout);
        if (layout == nullptr || std::memcmp(layout + kLayoutName, kLayout, 16) != 0 || !needed() || !found() ||
            g_manager == nullptr)
            return;
        // the same as frame() does after Present, ahead of the game's draw: the game makes the window at its own place (the
        // bottom right) as something is targeted and draws it in that frame, and frame() only moved it for the next one
        char* window = const_cast<char*>(static_cast<const char*>(ctrl));
        place(window);
        bar_scale(window);
        quads(window);
        bare(window);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

void reload(const wchar_t* ini)
{
    g_hide = toggle(L"target.hide", false, ini);
    g_move = toggle(L"target.move", false, ini);
    g_x = number(L"target.x", 50, 0, 100, ini);
    g_y = number(L"target.y", 4, 0, 100, ini);
    g_width = static_cast<int>(number(L"target.width", 0, 0, 800, ini));
    if (g_width > 0 && g_width < 80) // narrower than the caption and a bar's room: the game's looks break
        g_width = 80;
    g_background = toggle(L"target.background", true, ini);
    g_label = toggle(L"target.label", true, ini);
    g_label_x = static_cast<int>(number(L"target.label.x", 0, -300, 300, ini));
    g_label_y = static_cast<int>(number(L"target.label.y", 0, -20, 20, ini));
    wchar_t align[16] = {};
    GetPrivateProfileStringW(L"client", L"target.name.align", L"left", align, 16, ini);
    g_name_align = _wcsicmp(align, L"centre") == 0 || _wcsicmp(align, L"center") == 0 ? 1 : _wcsicmp(align, L"right") == 0 ? 2 : 0;
    g_name_x = static_cast<int>(number(L"target.name.x", 0, -300, 300, ini));
    g_name_y = static_cast<int>(number(L"target.name.y", 0, -20, 20, ini));
    g_one_line = toggle(L"target.name.oneline", false, ini);
    g_lock_label_x = static_cast<int>(number(L"target.lock.label.x", 0, -300, 300, ini));
    g_lock_label_y = static_cast<int>(number(L"target.lock.label.y", 0, -20, 20, ini));
    g_lock_text = toggle(L"target.lock.label", true, ini);
    g_pad_x = static_cast<int>(number(L"target.padding.x", 0, 0, 32, ini));
    g_pad_y = static_cast<int>(number(L"target.padding.y", 0, 0, 32, ini));
    g_spacing = static_cast<int>(number(L"target.spacing", 0, 0, 32, ini));
    g_title_own = false;
    for (const wchar_t* key : {L"target.title.font", L"target.title.font.family", L"target.title.font.size", L"target.title.font.bold",
                               L"target.title.font.italic", L"target.title.font.uppercase", L"target.title.font.outline"}) {
        wchar_t text[8] = {};
        g_title_own = g_title_own || GetPrivateProfileStringW(L"client", key, L"", text, 8, ini) > 0;
    }
    g_mask = toggle(L"target.bar.mask", true, ini);
    g_outline = toggle(L"target.bar.outline", true, ini);
    g_arrow = toggle(L"target.arrow", true, ini);
    g_arrow_image = picture_path(ini);
    g_bar_x = static_cast<int>(number(L"target.bar.x", 0, -200, 200, ini));
    g_bar_y = static_cast<int>(number(L"target.bar.y", 0, -20, 20, ini));
    wchar_t style[16] = {};
    GetPrivateProfileStringW(L"client", L"target.lock", L"game", style, 16, ini);
    g_lock = _wcsicmp(style, L"glow") == 0 ? Lock::Glow : _wcsicmp(style, L"none") == 0 ? Lock::None : Lock::Game;
    g_lock_rgb = colour(L"target.lock.color", 0xFF2A2A, ini);
    g_pulse = toggle(L"target.lock.pulse", true, ini);
}

void install(const wchar_t* ini)
{
    reload(ini);
}

void on_frame()
{
    if (!g_mark_hooked && gmicons::enabled() && found())
        install_mark();
    if (!needed())
        return;
    if (!found() || g_manager == nullptr)
        return;
    __try {
        frame();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

namespace {

// The picture in the arrow's place: its bottom middle at the arrow's point, its width and height in the interface's
// units (its own pixels as units when none are given), moved by the picture's x and y, tinted. The arrow isn't squeezed
// for [ui] aspect (the renderer leaves it where the game put it, over the world), so nor is this.
void draw_arrow(void* device, unsigned width, unsigned height)
{
    const gameui::Picture& picture = g_arrow_pending.picture;
    if (picture.file != g_arrow_loaded) {
        g_arrow_loaded = picture.file;
        g_arrow_failed = false;
        image::Pixels pixels;
        if (image::decode_file(picture.file, pixels) && pixels.width > 0 && pixels.height > 0) {
            g_arrow_texture.set(std::move(pixels));
            log::write(L"Target window: the arrow's picture is %s (%d x %d)", picture.file.c_str(), g_arrow_texture.width(),
                       g_arrow_texture.height());
        } else {
            g_arrow_texture.set(image::Pixels{});
            g_arrow_failed = true;
            log::write(L"Target window: the arrow's picture %s couldn't be read: nothing is drawn in the arrow's place",
                       picture.file.c_str());
        }
    }
    if (g_arrow_failed || g_arrow_texture.empty())
        return;
    const float wide = picture.width > 0 ? picture.width : static_cast<float>(g_arrow_texture.width());
    const float tall = picture.height > 0 ? picture.height : static_cast<float>(g_arrow_texture.height());
    const float left_units = static_cast<float>(g_arrow_pending.x) - wide * 0.5f + picture.x;
    const float top_units = static_cast<float>(g_arrow_pending.y) - tall + picture.y;
    float left = 0, top = 0, right = 0, bottom = 0, scale = 0;
    if (!uispace::to_pixels_plain(left_units, top_units, width, height, left, top, scale) ||
        !uispace::to_pixels_plain(left_units + wide, top_units + tall, width, height, right, bottom, scale))
        return;
    g_arrow_texture.draw(device, left, top, right - left, bottom - top, picture.argb);
}

} // namespace

void draw(void* device, unsigned width, unsigned height)
{
    gmicons::draw_mark(device, width, height); // a GM's mark as their staff picture
    soulspire::echo_icons::draw_party_icons(device, width, height); // QA-SOUL-COMPANIONS-001: native party HUD Echo Soul icons
    draw_name(device, width, height);          // target.font: the name whole in its True Font
    const bool lock = g_pending.set, bar = g_bar_pending.set, arrow = g_arrow_pending.set;
    g_pending.set = g_bar_pending.set = g_arrow_pending.set = false;
    if (!lock && !bar && !arrow)
        return;
    if (device != g_device) {
        g_glow.release();
        g_bar_outline.release();
        g_arrow_texture.release();
        g_glow_w = g_glow_h = 0;
        g_bar_outline_w = g_bar_outline_h = 0;
        g_device = device;
    }
    if (arrow)
        draw_arrow(device, width, height);
    float left = 0, top = 0, right = 0, bottom = 0, scale = 0, unused = 0;
    if (lock && uispace::to_pixels(g_pending.left, g_pending.top, width, height, left, top, scale) &&
        uispace::to_pixels(g_pending.right, g_pending.bottom, width, height, right, bottom, unused)) {
        const int w = std::max(8, static_cast<int>(std::lround(right - left)));
        const int h = std::max(8, static_cast<int>(std::lround(bottom - top)));
        const int margin = std::max(4, static_cast<int>(std::lround(7.0f * scale)));
        // its corners as the windows' (cut or round) where they're on
        float size = 0;
        const int shape = vanacore_gfx_window_corners(&size);
        const int corner = shape != 0 ? static_cast<int>(std::lround(size * scale)) : 0;
        if (w != g_glow_w || h != g_glow_h || margin != g_glow_margin || g_lock_rgb != g_glow_rgb || shape != g_glow_shape ||
            corner != g_glow_corner) {
            g_glow.set_image(glow_image(w, h, margin, g_lock_rgb, shape, static_cast<float>(corner)), w + margin * 2, h + margin * 2);
            g_glow_w = w, g_glow_h = h, g_glow_margin = margin, g_glow_rgb = g_lock_rgb;
            g_glow_shape = shape, g_glow_corner = corner;
        }
        // a slow pulse between 70 and 100 percent
        const float opacity = g_pulse ? 0.85f + 0.15f * static_cast<float>(std::sin(GetTickCount64() * 0.0045)) : 1.0f;
        if (g_glow.upload(device))
            g_glow.draw(device, static_cast<int>(std::lround(left)) - margin, static_cast<int>(std::lround(top)) - margin, opacity);
    }
    if (bar && uispace::to_pixels(g_bar_pending.left, g_bar_pending.top, width, height, left, top, scale) &&
        uispace::to_pixels(g_bar_pending.right, g_bar_pending.bottom, width, height, right, bottom, unused)) {
        const int w = std::max(6, static_cast<int>(std::lround(right - left)));
        const int h = std::max(4, static_cast<int>(std::lround(bottom - top)));
        const int margin = std::max(2, static_cast<int>(std::lround(2.5f * scale)));
        if (w != g_bar_outline_w || h != g_bar_outline_h || margin != g_bar_outline_margin) {
            g_bar_outline.set_image(outline_image(w, h, margin), w + margin * 2, h + margin * 2);
            g_bar_outline_w = w, g_bar_outline_h = h, g_bar_outline_margin = margin;
        }
        if (g_bar_outline.upload(device))
            g_bar_outline.draw(device, static_cast<int>(std::lround(left)) - margin, static_cast<int>(std::lround(top)) - margin);
    }
}

namespace {

// outline: the window's size as last seen (it's open only with something targeted), for its box while it isn't
int g_seen_w = 112, g_seen_h = 36;

bool open_rect(outline::Box& box)
{
    __try {
        char* window = find_window();
        if (window == nullptr)
            return false;
        box = {at<short>(window, kWinFrame), at<short>(window, kWinFrame + 2), at<short>(window, kWinFrame + 4),
               at<short>(window, kWinFrame + 6)};
        return box.right > box.left && box.bottom > box.top;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

} // namespace

bool outline(double* moved_x, double* moved_y)
{
    if (!found() || g_manager == nullptr)
        return false;
    const int area_left = at<short>(g_manager, kAreaLeft), area_top = at<short>(g_manager, kAreaLeft + 2);
    const int area_w = at<short>(g_manager, kAreaLeft + 4) - area_left, area_h = at<short>(g_manager, kAreaLeft + 6) - area_top;
    if (area_w <= 0 || area_h <= 0)
        return false;
    outline::Box box;
    if (open_rect(box)) {
        g_seen_w = box.right - box.left;
        g_seen_h = box.bottom - box.top;
    } else {
        // where target.x (its middle) and target.y (its top) put it, at the size it last had
        const int wide = g_width > 0 ? g_width + 2 * g_pad_x : g_seen_w;
        box.left = std::clamp(area_left + static_cast<int>(std::lround(area_w * g_x / 100.0)) - wide / 2, area_left,
                              std::max(area_left, area_left + area_w - wide));
        box.top = std::clamp(area_top + static_cast<int>(std::lround(area_h * g_y / 100.0)), area_top,
                             std::max(area_top, area_top + area_h - g_seen_h));
        box.right = box.left + wide;
        box.bottom = box.top + g_seen_h;
    }
    outline::Drag drag;
    if (!outline::draw("target", box, 0, drag))
        return false;
    // dragged: the window goes where the box is taken to (target.move on) at once
    if (drag.held) {
        g_move = true;
        g_x = std::clamp(std::round((drag.left + (box.right - box.left) / 2.0 - area_left) * 10000.0 / area_w) / 100.0, 0.0, 100.0);
        g_y = std::clamp(std::round((drag.top - area_top) * 10000.0 / area_h) / 100.0, 0.0, 100.0);
    }
    if (drag.released && moved_x != nullptr && moved_y != nullptr) {
        *moved_x = g_x;
        *moved_y = g_y;
    }
    return true;
}

bool rect(unsigned width, unsigned height, float& left, float& top, float& right, float& bottom)
{
    if (!found() || g_manager == nullptr || width == 0 || height == 0 || wants_hidden())
        return false;
    outline::Box box;
    if (!open_rect(box))
        return false;
    if (!g_space)
        g_space = uispace::find(GetModuleHandleW(L"FFXiMain.dll"));
    float scale = 0;
    return uispace::to_pixels(static_cast<float>(box.left), static_cast<float>(box.top), width, height, left, top, scale) &&
           uispace::to_pixels(static_cast<float>(box.right), static_cast<float>(box.bottom), width, height, right, bottom, scale);
}

} // namespace vanacore::targetwin
