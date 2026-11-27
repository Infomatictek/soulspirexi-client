#include "buttons.hpp"

#include <windows.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cwchar>

#include "../gfx/vanacore_gfx.h"
#include "chatfont.hpp"
#include "d3d8.hpp"
#include "imports.hpp"
#include "itemslot.hpp"
#include "jplabels.hpp"
#include "labeltext.hpp"
#include "log.hpp"
#include "macrotext.hpp"
#include "scan.hpp"
#include "sliders.hpp"
#include "soulspire/echoes_rebrand.hpp"
#include "soulspire/state.hpp"
#include "text.hpp"

namespace vanacore::buttons {
namespace {

// A menu button is one shape of quads (the window draw's type-1 piece, +4): the bar ("menu    buttonto", two quads, 48 and
// 40 units of a 64 x 64 DXT1 picture side by side, 88 x 16: a rounded left end, a slanted right one, a gloss along the
// top), then its label's letters ("font    font    ", each drawn twice: a shadow taken away from what's under it, then
// the letter added 2 units up and left). Seen live 2026-11-05 on the main menu. The window draw gives the shape its
// state by its colour: 0x80808080 plain, 0x80805020 the cursor's (gold: it tints the bar and the label), 0x30808080
// greyed out, 0x40402810 the cursor's greyed out.
//
// YmMenuShape::Draw with a scale (FUN_1011F880, __thiscall shape, ret 20h: x, y, scale x, scale y, colour, centred,
// blending, quads to skip), which the window draw's button (buffwin.cpp redirects that call) and the flash of a picked
// one (FUN_10119BB0, three draws: the button, then two copies growing and fading) both call; with both scales 1 it
// calls the plain shape draw (FUN_1011F760), which draws each quad (FUN_1011E580, itemslot.cpp's hook) with the shape's
// x, y, scale, colour and blending. The first 5 bytes left out of the signature: statuswindow.cpp finds the same
// function, before or after the hook. sub esp,0Ch; push ebx; push ebp | push esi; mov esi,ecx; push edi; mov ebp,[esi+14h];
// lea ecx,[esi+4]; mov [esp+18h],ebp; mov [esp+14h],ecx; call
constexpr char kShapeDraw[] = "?? ?? ?? ?? ?? 56 8B F1 57 8B 6E 14 8D 4E 04 89 6C 24 18 89 4C 24 14 E8";
constexpr char kShapeStart[] = {'\x83', '\xEC', '\x0C', '\x53', '\x55'};
// YmMenuShape::Draw (FUN_1011F760, __thiscall shape, ret 14h: x, y, colour, blending, quads to skip): the scaled draw's at a
// scale of 1, and what a menu calls itself for a button it draws apart from the window's loop (the main menu's Map,
// 0x1013FB83; the Party menu's Find Member, 0x10203F6C: the cursor's through the scaled draw in gold, the rest through
// this; seen live 2026-11-05). The first 7 bytes left out of the signature: equipstats.cpp finds the same function, before
// or after the hook. push ecx; push ebx; push ebp; push esi; mov esi,[ecx+14h] | lea ebx,[ecx+4]; push edi; mov ecx,ebx; call
constexpr char kPlainDraw[] = "?? ?? ?? ?? ?? ?? ?? 8D 59 04 57 8B CB E8 ?? ?? ?? ?? 89 44 24 10 8B 44 24 28 33 FF 85 C0 7E 13";
constexpr char kPlainStart[] = {'\x51', '\x53', '\x55', '\x56', '\x8B', '\x71', '\x14'};
// The shape: its quads' list (+4: the first node, a node's next at +0, its quad at +10h) and their count (+14h)
constexpr std::ptrdiff_t kShapeList = 4, kShapeCount = 0x14, kNodeQuad = 0x10;
constexpr int kQuadsAtMost = 64;
// A quad (itemslot.cpp's): its picture's name (+32h), the game's texture (+4Ch; its Direct3D texture at +40h), its four
// corners (+50h, 1Ch apart: x, y, z, rhw, colour B G R A, u, v) and its own corners in the shape's units (x shorts at
// +C0h, y at +C8h: top left, top right, bottom left, bottom right), which each draw lays out from: x + its x times the
// scale. Its blending (+2Dh: on, then A, B, D as itemslot.cpp's): the label's shadow has A 2 (taken away).
constexpr std::ptrdiff_t kName = 0x32, kTexture = 0x4C, kCorners = 0x50, kCorner = 0x1C, kColour = 0x10, kU = 0x14, kV = 0x18;
constexpr std::ptrdiff_t kOwnX = 0xC0, kOwnY = 0xC8, kBlendA = 0x2E;
constexpr std::size_t kQuadSize = 0xD0;
constexpr std::ptrdiff_t kD3d = 0x40, kD3dElse = 0x44;
constexpr char kBar[] = "menu    button";
constexpr char kFont[] = "font    ";
constexpr int kTakeAway = 2;
constexpr int kBlendOver = 1 << 2 | 1 << 6; // itemslot.cpp's (A 0, B 1, D 1): blended over what's under it
constexpr std::uint32_t kGold = 0x805020, kGoldOff = 0x402810, kNeutral = 0x808080;
constexpr int kOpaque = 0x80; // a corner's alpha, doubled as it's drawn

// The window draw's buttons (FUN_101173D0's loop over its pieces), seen live 2026-11-05. The cursor's button is drawn a
// unit up and left of its place (dec eax; dec edi), and while it flashes (window +5Eh above 0: picked) by FUN_10119B80
// two units down and right of that (add edi,2; add ecx,2), so it's pressed in; jle (+46h) goes past the flash to the
// plain draw while there's none. mov di,[ebp+B8h]; mov edx,[esi+4]; mov ax,[esi+2Ah]; add di,[esi+2Ch]; mov [ebp+8],esi;
// mov ecx,[esi+1Ch]; mov [esp+1Ch],edx; mov [esp+14h],ecx; mov edx,[esi+20h]; dec eax; ...; dec edi; cmp eax,0Fh;
// mov ebx,80805020h; jne; mov byte ptr [esp+13h],1; mov ax,[ebp+5Eh]; test ax,ax; jle; mov ecx,[esp+20h]; add edi,2; push
// eax; mov eax,[esp+20h]; add ecx,2
constexpr char kSelected[] = "66 8B BD B8 00 00 00 8B 56 04 66 8B 46 2A 66 03 7E 2C 89 75 08 8B 4E 1C 89 54 24 1C 89 4C 24 14 "
                             "8B 56 20 48 89 54 24 18 89 44 24 20 8B 46 18 4F 83 F8 0F BB 20 50 80 80 75 05 C6 44 24 13 01 "
                             "66 8B 45 5E 66 85 C0 0F 8E ?? ?? ?? ?? 8B 4C 24 20 83 C7 02 50 8B 44 24 20 83 C1 02";
constexpr std::ptrdiff_t kLeft = 0x23, kUp = 0x2F, kFlash = 0x46, kPressDown = 0x52, kPressRight = 0x5A;
// The other two ways the cursor's button is drawn, each a unit down and right (inc eax; inc edi): a piece with +48h set,
// and the cursor's button greyed out (40402810h)
constexpr char kRaised[] = "66 8B BD B8 00 00 00 66 8B 46 2A 66 03 7E 2C 8B 56 04 40 BB 20 50 80 80 89 75 08 89 44 24 20 47";
constexpr std::ptrdiff_t kRaisedRight = 0x12, kRaisedDown = 0x1F;
constexpr char kGreyed[] = "66 8B BD B8 00 00 00 8B 4E 04 66 8B 46 2A 66 03 7E 2C 89 75 08 8B 56 1C 89 4C 24 1C 89 54 24 14 "
                           "8B 4E 20 40 BB 10 28 40 40 89 4C 24 18 89 44 24 20 47";
constexpr std::ptrdiff_t kGreyedRight = 0x23, kGreyedDown = 0x31;
// The game's button place (GetButtonLocate, FUN_10118500: __thiscall window, rect, id, as drawn, pressed; ret 10h) has the
// same nudge of its own: with `as drawn` the cursor's button a unit up and left (dec ecx; dec edx), greyed out (status 5) a
// unit down and right (inc ecx; inc edx), and with `pressed` while it flashes two down and right of that. A menu asks it
// where to draw a button it draws apart from the window's loop, inside the window's draw (its handler's draw, slot 4, from
// 0x10117904): the main menu's Map (0x1013FAD0: its Map, Mog House or Monstrosity label over item 12), the Party menu's
// Find Member, the selection menus' rows (chat.cpp). Left as the game's, the main menu's Map stood a unit up and left of its
// button with the nudge off, and True Font's column (cf_label_left) took every label with it, a pixel left while the cursor
// was on Map (2026-11-10). Matched past its first 11h bytes (chat.cpp's detour of its start): movsx ecx,[eax+2Ah];
// movsx edx,[eax+2Ch]; ...; dec ecx; dec edx; ...; add ecx,2; add edx,2; jmp; inc ecx; inc edx
constexpr char kPlace[] = "0F BF 48 2A 0F BF 50 2C 53 57 66 8B 78 3A 66 3B 7E 4C 75 30 8A 5E 63 84 DB 74 29 8A 5C 24 18 84 DB "
                          "74 21 83 78 10 05 74 19 8A 5C 24 1C 49 4A 84 DB 74 11 66 83 7E 5E 00 7E 0A 83 C1 02 83 C2 02 EB 02 41 42";
constexpr std::ptrdiff_t kPlaceLeft = 0x2D, kPlaceUp = 0x2E, kPlacePressRight = 0x3C, kPlacePressDown = 0x3F;
constexpr std::ptrdiff_t kPlaceGreyedRight = 0x42, kPlaceGreyedDown = 0x43;
constexpr std::uint8_t kNop = 0x90, kJmp = 0xE9;

struct ShapeArgs { // FUN_1011F880's, as pushed: x, y, scale x, scale y, colour, centred, blending, quads to skip
    std::uint32_t at[8];
};
constexpr int kArgColour = 4, kArgSkip = 7;
using ShapeDraw = void(__fastcall*)(void* shape, void* edx, ShapeArgs args);
struct PlainArgs { // FUN_1011F760's: x, y, colour, blending, quads to skip
    std::uint32_t at[5];
};
constexpr int kPlainColour = 2, kPlainSkip = 4;
using PlainDraw = void(__fastcall*)(void* shape, void* edx, PlainArgs args);

// The sets of [client] keys (/vana Interface > Buttons' tabs): Global's, buttons.*; Main Menu's, mainmenu.buttons.* (the main
// menu's windows and the menus it opens in its place: True Font's mainmenu.font windows); Action Menu's, action.buttons.*
// (the Action Menu, Monstrosity's, and the Magic and Abilities lists it opens beside it: True Font's action.font windows).
// The last two each have an Enable ([client] mainmenu.buttons, action.buttons: kEnable): 1, their own keys alone (unset, the
// default), Global's never reaching them; 0, Global's set alone; never set, each key the global one's until its own is set
// (as they were before the switch, 2026-11-07).
struct Style {
    bool flat = false;
    bool fill_set = false, selected_fill_set = false;            // .color, .selected.color set (the game style's bars too)
    std::uint32_t fill = 0x4B65A0, selected_fill = 0x4B4845;     // (0xRRGGBB)
    int alpha = kOpaque;                                         // .opacity, as a corner's alpha
    std::uint32_t border = 0x8A8FA6, selected_border = 0x8A8FA6;
    int border_size = 1;
    int pad_x = 0, pad_y = 0;
    int text_x = 0, text_y = 0; // .text.x, .text.y: a flat button's label moved this far (the interface's units)
    bool text_set = false, selected_text_set = false;
    std::uint32_t text = 0, selected_text = 0;
    int spacing = 0; // .spacing (buttonspace.cpp's, with the flat style)
    bool nudge = true, flash = true;
};
enum Set { kGlobal, kMainMenu, kActionMenu, kSets };
constexpr const wchar_t* kKeys[kSets] = {L"buttons.", L"mainmenu.buttons.", L"action.buttons."};
constexpr const wchar_t* kEnable[kSets] = {nullptr, L"mainmenu.buttons", L"action.buttons"};
Style g_styles[kSets];
bool g_own[kSets] = {true, false, false}; // a key of the set's own is set (the global one's always): its windows told apart
const Style* g_style = &g_styles[kGlobal]; // the window being drawn's
constexpr std::ptrdiff_t kWinLayout = 4; // a window's layout (its name at +46h: chatfont.cpp's)

// The shape drawings, hooked once they're wanted
bool g_shape_tried = false;
ShapeDraw g_shape_game = nullptr;
PlainDraw g_plain_game = nullptr;

// The bytes the nudge and flash change (the window draw's and the button place's): where, and the game's (found once). The
// nudge's steps of a unit (dec/inc: the window draw's six, the button place's four), its press's immediates (add ...,2: the
// window draw's two, the button place's two), then the flash's jle (0F 8E), jmp in its place: nop; jmp
struct Patch {
    std::uintptr_t at;
    std::uint8_t game;
};
constexpr int kSteps = 10, kPresses = 4, kFlashAt = kSteps + kPresses, kPatches = kFlashAt + 2;
Patch g_patches[kPatches] = {};
bool g_patch_tried = false, g_patch_dirty = true;
int g_written = 3; // what the bytes say now: nudge | flash << 1 (both on: the game's, as found)

// The button whose shape is being drawn: the shape, whether it's a button (it has bars), whether this restyles it (active),
// its bars' extent (the shape's units), its state and whether the box is drawn
struct Drawing {
    const void* shape;
    bool button, active, flat, drawn, selected;
    bool wells; // a window's frame: its glossy slots under the buttons (drawn with its background left out), hidden flat
    int left, top, right, bottom;
};
Drawing g_now = {};

// A flat button's label letter, kept to be drawn once its window's boxes are (leave_window): a box padded past its button
// (or a picked one's flash, growing) goes over the next button's place, and drawn after that one's letters it would hide
// them (True Font gives a letter that's drawn over back to the game's font). The game's quad and its draw's arguments; a
// mark of where each window being drawn (nested) started, and the style of the window it's drawn inside.
struct Label {
    const char* quad;
    int x, y;
    float sx, sy;
    std::uint32_t colour;
    int flags;
};
constexpr int kLabelsAtMost = 1024, kDepthAtMost = 8;
Label g_labels[kLabelsAtMost] = {};
int g_label_count = 0;
int g_marks[kDepthAtMost] = {};
const Style* g_outer[kDepthAtMost] = {};
int g_depth = 0;

// The white picture the boxes are drawn with, made on the game's device; a stand-in for the game's texture (its Direct3D
// texture at +40h and +44h the white one), and the quad copied to draw it with (the game's own left as it is)
void* g_white = nullptr;
void* g_white_device = nullptr;
alignas(8) std::uint8_t g_stand[kD3dElse + sizeof(void*)] = {};
alignas(8) std::uint8_t g_quad[kQuadSize] = {};

bool starts(const char* quad, const char* name, std::size_t size)
{
    return std::memcmp(quad + kName, name, size) == 0;
}

// The extent of the shape's bars in its own units; false if it has none (not a button)
bool bars_of(const char* shape, Drawing& now)
{
    __try {
        const int count = *reinterpret_cast<const int*>(shape + kShapeCount);
        const char* node = *reinterpret_cast<char* const*>(shape + kShapeList);
        bool found = false;
        for (int i = 0; node != nullptr && i < count && i < kQuadsAtMost; ++i, node = *reinterpret_cast<char* const*>(node)) {
            const char* quad = *reinterpret_cast<char* const*>(node + kNodeQuad);
            if (quad == nullptr || !starts(quad, kBar, sizeof kBar - 1))
                continue;
            const auto* xs = reinterpret_cast<const short*>(quad + kOwnX);
            const auto* ys = reinterpret_cast<const short*>(quad + kOwnY);
            for (int c = 0; c < 4; ++c) {
                now.left = found ? std::min<int>(now.left, xs[c]) : xs[c];
                now.right = found ? std::max<int>(now.right, xs[c]) : xs[c];
                now.top = found ? std::min<int>(now.top, ys[c]) : ys[c];
                now.bottom = found ? std::max<int>(now.bottom, ys[c]) : ys[c];
                found = true;
            }
        }
        return found;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool restyled(const Style& style)
{
    return style.flat || style.text_set || style.selected_text_set;
}

// Whether any set in use (the global one, and those with a key of their own) says `what`
template <typename What>
bool any_set(What what)
{
    for (int i = 0; i < kSets; ++i)
        if (g_own[i] && what(g_styles[i]))
            return true;
    return false;
}

bool restyled_any()
{
    return any_set([](const Style& s) { return restyled(s); });
}

// What a draw of `shape` in `colour` (skipping `skip` quads) draws: a button (bars in it: True Font's labels then the
// buttons', the rest the titles'), one this restyles (its bars' extent and state, active), or neither
Drawing drawing_of(const void* shape, std::uint32_t colour, std::uint32_t skip)
{
    Drawing now = {};
    now.shape = shape;
    if (!(restyled(*g_style) || chatfont::labels_on()) || !bars_of(static_cast<const char*>(shape), now))
        return now;
    now.button = true;
    if (restyled(*g_style) && skip == 0) {
        const std::uint32_t rgb = colour & 0xFFFFFF;
        now.active = true;
        now.flat = g_style->flat;
        now.selected = rgb == kGold || rgb == kGoldOff;
    } else if (g_style->flat && skip != 0) {
        // a window's frame (the window draw's frame part, its first quad the background drawn on its own: skip 1) has a
        // well for each button, two halves of the button's bar: left out with the bars (and stretched with buttons.spacing)
        now.active = now.flat = now.wells = true;
    }
    return now;
}

void __fastcall shape_hook(void* shape, void* edx, ShapeArgs args)
{
    shape = soulspire::rebrand_menu_shape(shape);
    const Drawing outer = g_now;
    g_now = drawing_of(shape, args.at[kArgColour], args.at[kArgSkip]);
    const macrotext::Key key(shape, static_cast<int>(args.at[0]), static_cast<int>(args.at[1]), std::bit_cast<float>(args.at[2]),
                             std::bit_cast<float>(args.at[3]), args.at[kArgColour]); // a macro bar key's label: VanaCore's text
    const labeltext::Shape whole(shape); // a Config window's labels: read, to be drawn whole (labeltext.cpp)
    const sliders::Shape gauges(shape);  // ... and its sliders' parts found, to be drawn flat (sliders.cpp)
    if (g_now.button)
        vanacore_gfx_button_drawing(1);
    jplabels::shape_begin(shape); // (a game in Japanese: its labels' parts put together)
    g_shape_game(shape, edx, args);
    jplabels::shape_end(shape);
    if (g_now.button)
        vanacore_gfx_button_drawing(0);
    g_now = outer;
}

void __fastcall plain_hook(void* shape, void* edx, PlainArgs args)
{
    shape = soulspire::rebrand_menu_shape(shape);
    if (g_now.shape == shape) { // the scaled draw's own at a scale of 1: the button as it found it
        g_plain_game(shape, edx, args);
        return;
    }
    const Drawing outer = g_now;
    g_now = drawing_of(shape, args.at[kPlainColour], args.at[kPlainSkip]);
    const macrotext::Key key(shape, static_cast<int>(args.at[0]), static_cast<int>(args.at[1]), 1.0f, 1.0f, args.at[kPlainColour]);
    const labeltext::Shape whole(shape);
    const sliders::Shape gauges(shape);
    if (g_now.button)
        vanacore_gfx_button_drawing(1);
    jplabels::shape_begin(shape);
    g_plain_game(shape, edx, args);
    jplabels::shape_end(shape);
    if (g_now.button)
        vanacore_gfx_button_drawing(0);
    g_now = outer;
}

// The white picture on the device of the game's texture of `quad`, made the first time (and again on another device)
void* white_for(const char* quad)
{
    void* device = sliders::device_of(quad);
    if (device == nullptr)
        return nullptr;
    if (device == g_white_device && g_white != nullptr)
        return g_white;
    g_white = nullptr; // made on another device: went with it
    g_white_device = device;
    void* white = nullptr;
    if (FAILED(d3d8::call(device, d3d8::slot::CreateTexture, 4u, 4u, 1u, DWORD{0}, d3d8::kFormatA8R8G8B8, d3d8::kPoolManaged,
                          &white)) ||
        white == nullptr)
        return nullptr;
    d3d8::LockedRect locked{};
    if (FAILED(d3d8::call(white, d3d8::slot::TextureLockRect, 0u, &locked, static_cast<RECT*>(nullptr), DWORD{0}))) {
        d3d8::call<ULONG>(white, d3d8::slot::Release);
        return nullptr;
    }
    for (int y = 0; y < 4; ++y)
        std::memset(static_cast<std::uint8_t*>(locked.bits) + y * locked.pitch, 0xFF, 16);
    d3d8::call(white, d3d8::slot::TextureUnlockRect, 0u);
    g_white = white;
    return white;
}

struct Pen { // where the box is drawn: the bar's own draw's
    int x, y;
    float sx, sy;
    std::uint32_t given;
};
struct Box {
    int left, top, right, bottom;
};

// One rectangle of the box (the shape's units) in `rgb` at `alpha`, drawn with the copied quad
void rect(const Pen& pen, const Box& box, std::uint32_t rgb, int alpha)
{
    if (box.right <= box.left || box.bottom <= box.top)
        return;
    auto* xs = reinterpret_cast<short*>(g_quad + kOwnX);
    auto* ys = reinterpret_cast<short*>(g_quad + kOwnY);
    xs[0] = xs[2] = static_cast<short>(box.left);
    xs[1] = xs[3] = static_cast<short>(box.right);
    ys[0] = ys[1] = static_cast<short>(box.top);
    ys[2] = ys[3] = static_cast<short>(box.bottom);
    // a corner's colour is doubled as it's drawn: half the colour wanted
    const auto half = [rgb](int shift) { return static_cast<std::uint8_t>(((rgb >> shift & 0xFF) + 1) / 2); };
    const std::uint8_t bgra[4] = {half(0), half(8), half(16), static_cast<std::uint8_t>(alpha)};
    for (int c = 0; c < 4; ++c)
        std::memcpy(g_quad + kCorners + c * kCorner + kColour, bgra, 4);
    itemslot::draw_game(g_quad, pen.x, pen.y, pen.sx, pen.sy, pen.given, kBlendOver);
}

// The button's box in the place of its bars, drawn from `bar` (its first) with its draw's place and scale: the fill,
// then the border round it. False if the white picture can't be had (the game's bars are drawn then).
bool draw_box(const char* bar, const Pen& pen)
{
    void* white = white_for(bar);
    if (white == nullptr)
        return false;
    std::memcpy(g_quad, bar, kQuadSize);
    std::memcpy(g_stand + kD3d, &white, sizeof white);
    std::memcpy(g_stand + kD3dElse, &white, sizeof white);
    const std::uint8_t* stand = g_stand;
    std::memcpy(g_quad + kTexture, &stand, sizeof stand);
    for (int c = 0; c < 4; ++c) {
        const float half = 0.5f;
        std::memcpy(g_quad + kCorners + c * kCorner + kU, &half, sizeof half);
        std::memcpy(g_quad + kCorners + c * kCorner + kV, &half, sizeof half);
    }
    const Style& s = *g_style;
    const Box box = {g_now.left - s.pad_x, g_now.top - s.pad_y, g_now.right + s.pad_x, g_now.bottom + s.pad_y};
    if (box.right - box.left < 1 || box.bottom - box.top < 1)
        return true; // padded away: nothing
    const int e = std::min({s.border_size, (box.right - box.left) / 2, (box.bottom - box.top) / 2});
    rect(pen, {box.left + e, box.top + e, box.right - e, box.bottom - e}, g_now.selected ? s.selected_fill : s.fill, s.alpha);
    if (e > 0) {
        const std::uint32_t edge = g_now.selected ? s.selected_border : s.border;
        rect(pen, {box.left, box.top, box.right, box.top + e}, edge, kOpaque);
        rect(pen, {box.left, box.bottom - e, box.right, box.bottom}, edge, kOpaque);
        rect(pen, {box.left, box.top + e, box.left + e, box.bottom - e}, edge, kOpaque);
        rect(pen, {box.right - e, box.top + e, box.right, box.bottom - e}, edge, kOpaque);
    }
    return true;
}

// A label's letter in the colour picked: the draw's colour, a letter's white times it doubled as it's drawn (the
// game's 0x805020 is its gold, #FFA040), its alpha kept. The shadow pass (taken away) keeps the game's.
void recolour_label(const char* quad, std::uint32_t& colour, int flags)
{
    const Style& s = *g_style;
    const bool picked = g_now.selected ? s.selected_text_set : s.text_set;
    const int a = flags != 0 ? flags & 3 : static_cast<std::uint8_t>(quad[kBlendA]);
    if (!picked || a == kTakeAway)
        return;
    const std::uint32_t rgb = g_now.selected ? s.selected_text : s.text;
    std::uint32_t given = colour & 0xFF000000;
    for (int shift = 0; shift < 24; shift += 8)
        given |= (((rgb >> shift & 0xFF) + 1) / 2) << shift;
    colour = given;
}

// The patches' bytes written as `values` say, with one change of the pages' protection round them all (the window draw and
// the button place, 0x1000 bytes apart in FFXiMain's code): the game's thread draws with them (on_frame and the window draws
// are on it), and with the main menu's own nudge or flash they're written at its draw and back after it, each frame
void write_all(const std::uint8_t (&values)[kPatches])
{
    std::uintptr_t low = ~std::uintptr_t{0}, high = 0;
    for (const Patch& patch : g_patches)
        if (patch.at != 0) {
            low = std::min(low, patch.at);
            high = std::max(high, patch.at + 1);
        }
    DWORD protect = 0;
    if (high == 0 || !VirtualProtect(reinterpret_cast<void*>(low), high - low, PAGE_EXECUTE_READWRITE, &protect))
        return;
    for (int i = 0; i < kPatches; ++i)
        if (g_patches[i].at != 0)
            *reinterpret_cast<std::uint8_t*>(g_patches[i].at) = values[i];
    VirtualProtect(reinterpret_cast<void*>(low), high - low, protect, &protect);
    FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(low), high - low);
}

// g_patches[first...] kept from `match` plus each of `offsets` (none if it wasn't found)
template <std::size_t N>
void keep_at(int first, std::uintptr_t match, const std::ptrdiff_t (&offsets)[N])
{
    if (match == 0)
        return;
    for (std::size_t i = 0; i < N; ++i) {
        const std::uintptr_t at = match + offsets[i];
        g_patches[first + static_cast<int>(i)] = {at, *reinterpret_cast<const std::uint8_t*>(at)};
    }
}

void find_patches(HMODULE game)
{
    g_patch_tried = true;
    const std::uintptr_t selected = scan::find(game, kSelected), place = scan::find(game, kPlace);
    keep_at(0, selected, {kLeft, kUp});
    keep_at(2, scan::find(game, kRaised), {kRaisedRight, kRaisedDown});
    keep_at(4, scan::find(game, kGreyed), {kGreyedRight, kGreyedDown});
    keep_at(6, place, {kPlaceLeft, kPlaceUp, kPlaceGreyedRight, kPlaceGreyedDown});
    keep_at(kSteps, selected, {kPressDown, kPressRight});
    keep_at(kSteps + 2, place, {kPlacePressRight, kPlacePressDown});
    keep_at(kFlashAt, selected, {kFlash, kFlash + 1});
    if (selected == 0)
        log::write(L"Buttons: the game's drawing of the selected button wasn't found in FFXiMain.dll: [client] "
                   L"buttons.selected.nudge and .flash do nothing");
    else
        log::write(L"Buttons: the selected button's drawing found at +0x%X",
                   static_cast<unsigned>(selected - reinterpret_cast<std::uintptr_t>(game)));
    if (place == 0)
        log::write(L"Buttons: the game's button place wasn't found in FFXiMain.dll: with buttons.selected.nudge=0 a button a "
                   L"menu draws itself (the main menu's Map) is still nudged");
}

// The nudge and the flash as `style` says: the game's bytes, or a step of nothing (nop, an add of 0) and the jump past the
// flash always taken; nothing written when they say that already
void apply_patches(const Style& style)
{
    const int want = (style.nudge ? 1 : 0) | (style.flash ? 2 : 0);
    if (!g_patch_tried || want == g_written)
        return;
    g_written = want;
    std::uint8_t values[kPatches] = {};
    for (int i = 0; i < kPatches; ++i)
        values[i] = style.nudge || i >= kFlashAt ? g_patches[i].game : i < kSteps ? kNop : std::uint8_t{0};
    if (!style.flash) {
        values[kFlashAt] = kNop;
        values[kFlashAt + 1] = kJmp;
    }
    write_all(values);
}

// The game's function at `pattern`, whose first bytes (the ones the detour moves) are `start`, hooked by `hook`: its
// trampoline, or nullptr (said in the log, `what` naming it)
void* detour_at(HMODULE game, const char* pattern, const char* start, std::size_t size, void* hook, const wchar_t* what)
{
    const std::uintptr_t at = scan::find(game, pattern);
    if (at == 0 || std::memcmp(reinterpret_cast<const void*>(at), start, size) != 0) {
        log::write(L"Buttons: the game's %ls wasn't found in FFXiMain.dll: [client] buttons.style and the labels' colours "
                   L"do nothing for the buttons drawn by it",
                   what);
        return nullptr;
    }
    void* trampoline = imports::detour(reinterpret_cast<void*>(at), size, hook);
    if (trampoline == nullptr)
        log::write(L"Buttons: the game's %ls couldn't be hooked", what);
    else
        log::write(L"Buttons: the %ls hooked at +0x%X", what, static_cast<unsigned>(at - reinterpret_cast<std::uintptr_t>(game)));
    return trampoline;
}

void install_shape_hook(HMODULE game)
{
    g_shape_tried = true;
    g_shape_game = reinterpret_cast<ShapeDraw>(
        detour_at(game, kShapeDraw, kShapeStart, sizeof kShapeStart, reinterpret_cast<void*>(&shape_hook), L"scaled shape drawing"));
    g_plain_game = reinterpret_cast<PlainDraw>(
        detour_at(game, kPlainDraw, kPlainStart, sizeof kPlainStart, reinterpret_cast<void*>(&plain_hook), L"shape drawing"));
}

// A flat button's label letter kept for leave_window, if a window is being drawn and there's room; false: draw it now
bool later(const char* quad, int x, int y, float sx, float sy, std::uint32_t colour, int flags)
{
    if (!g_now.flat || !g_now.drawn || g_depth == 0 || g_label_count == kLabelsAtMost)
        return false;
    g_labels[g_label_count++] = {quad, x, y, sx, sy, colour, flags};
    return true;
}

std::uint32_t rgb_of(const float (&c)[4])
{
    const auto byte = [](float v) { return static_cast<std::uint32_t>(std::lround(std::clamp(v, 0.0f, 255.0f))); };
    return byte(c[0]) << 16 | byte(c[1]) << 8 | byte(c[2]);
}

// A set's [client] keys: its own (`own` and the key: mainmenu.buttons.color), else (`fallback`) the global buttons'
// (buttons.color) while its own is unset. `own_set`: one of its own was.
struct Keys {
    const wchar_t* ini;
    const wchar_t* own;
    bool fallback;
    bool own_set;
};

// `key`'s text, its own or the global one's; false when both are empty
bool text_of(Keys& keys, const wchar_t* key, wchar_t* text, DWORD size)
{
    wchar_t name[64] = {};
    swprintf_s(name, L"%ls%ls", keys.own, key);
    GetPrivateProfileStringW(L"client", name, L"", text, size, keys.ini);
    if (text[0] != L'\0') {
        keys.own_set = true;
        return true;
    }
    if (!keys.fallback)
        return false;
    swprintf_s(name, L"%ls%ls", kKeys[kGlobal], key);
    GetPrivateProfileStringW(L"client", name, L"", text, size, keys.ini);
    return text[0] != L'\0';
}

// `key` as a colour (0xRRGGBB); false (and `rgb` left) when it's empty or not one
bool colour_of(Keys& keys, const wchar_t* key, std::uint32_t& rgb)
{
    wchar_t text[64] = {};
    float c[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    if (!text_of(keys, key, text, 64) || !text::parse_colour(text, c))
        return false;
    rgb = rgb_of(c);
    return true;
}

// `key` as a whole number (negative ones too), clamped; `fallback` when it's empty or not one
int number_of(Keys& keys, const wchar_t* key, int fallback, int low, int high)
{
    wchar_t text[32] = {};
    text_of(keys, key, text, 32);
    wchar_t* end = nullptr;
    const long value = std::wcstol(text, &end, 10);
    return std::clamp(end != text ? static_cast<int>(value) : fallback, low, high);
}

// `key` as a switch: `fallback` when it's empty, else on unless 0 (GetPrivateProfileInt's reading: not a number is 0)
bool flag_of(Keys& keys, const wchar_t* key, bool fallback)
{
    wchar_t text[32] = {};
    if (!text_of(keys, key, text, 32))
        return fallback;
    return std::wcstol(text, nullptr, 10) != 0;
}

Style read_style(Keys& keys)
{
    Style s;
    wchar_t text[32] = {};
    s.flat = text_of(keys, L"style", text, 32) && _wcsicmp(text, L"flat") == 0;
    s.fill_set = colour_of(keys, L"color", s.fill);
    s.selected_fill_set = colour_of(keys, L"selected.color", s.selected_fill);
    s.alpha = (number_of(keys, L"opacity", 100, 0, 100) * kOpaque + 50) / 100;
    colour_of(keys, L"border", s.border);
    s.selected_border = s.border;
    colour_of(keys, L"selected.border", s.selected_border);
    s.border_size = number_of(keys, L"border.size", 1, 0, 4);
    s.pad_x = number_of(keys, L"padding.x", 0, -16, 16);
    s.pad_y = number_of(keys, L"padding.y", 0, -8, 8);
    s.text_x = number_of(keys, L"text.x", 0, -16, 16);
    s.text_y = number_of(keys, L"text.y", 0, -8, 8);
    s.text_set = colour_of(keys, L"text", s.text);
    s.selected_text_set = colour_of(keys, L"selected.text", s.selected_text);
    s.spacing = number_of(keys, L"spacing", 0, 0, 16);
    s.nudge = flag_of(keys, L"selected.nudge", true);
    s.flash = flag_of(keys, L"selected.flash", true);
    return s;
}

// `window`'s layout, null for none (or one that can't be read)
const char* layout_of(const void* window)
{
    __try {
        return *reinterpret_cast<const char* const*>(static_cast<const char*>(window) + kWinLayout);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

// The style of `window`'s buttons: the main menu's for its windows, the Action Menu's for its (each when it has a key of its
// own), else the global one
const Style& style_for(const void* window)
{
    if ((!g_own[kMainMenu] && !g_own[kActionMenu]) || window == nullptr)
        return g_styles[kGlobal];
    const char* layout = layout_of(window);
    if (layout == nullptr)
        return g_styles[kGlobal];
    if (g_own[kMainMenu] && chatfont::main_menu_window(layout))
        return g_styles[kMainMenu];
    if (g_own[kActionMenu] && chatfont::action_menu_window(layout))
        return g_styles[kActionMenu];
    return g_styles[kGlobal];
}

} // namespace

void reload(const wchar_t* ini)
{
    for (int i = 0; i < kSets; ++i) {
        // its Enable: 1 its own keys alone, 0 Global's alone, -1 (never set) each the global one's until its own is set
        wchar_t text[16] = {};
        if (kEnable[i] != nullptr)
            GetPrivateProfileStringW(L"client", kEnable[i], L"", text, 16, ini);
        const int enable = text[0] == L'\0' ? -1 : std::wcstol(text, nullptr, 10) != 0 ? 1 : 0;
        if (i != kGlobal && enable == 0) {
            g_styles[i] = g_styles[kGlobal];
            g_own[i] = false;
            continue;
        }
        Keys keys = {ini, kKeys[i], i != kGlobal && enable < 0, false};
        g_styles[i] = read_style(keys);
        g_own[i] = i == kGlobal || enable == 1 || keys.own_set;
    }
    g_patch_dirty = true;
}

void install(const wchar_t* ini)
{
    reload(ini);
}

void on_frame()
{
    // (True Font's: a button's letters told apart; the macro bar's keys' labels drawn whole, macrotext.cpp;
    // Soulspire: rebrands native Trust menu buttons to Echoes without adding any new hook)
    const bool hook = (restyled_any() || chatfont::labels_on() || chatfont::area_on(VANA_FONT_MACROBAR) || sliders::active() ||
                       soulspire::engine().enabled()) &&
                      !g_shape_tried;
    // the nudge and flash written once asked for, and again at each change after (to put the game's back)
    const bool off = any_set([](const Style& s) { return !s.nudge || !s.flash; });
    const bool patch = g_patch_dirty && (g_patch_tried || off);
    if (!hook && !patch)
        return;
    const HMODULE game = GetModuleHandleW(L"FFXiMain.dll");
    if (game == nullptr)
        return;
    if (hook)
        install_shape_hook(game);
    if (patch) {
        if (!g_patch_tried)
            find_patches(game);
        g_patch_dirty = false;
        apply_patches(*g_style);
    }
}

bool active()
{
    return (g_shape_game != nullptr || g_plain_game != nullptr) && restyled_any();
}

int spacing_for(const void* window)
{
    const Style& style = style_for(window);
    return style.flat ? style.spacing : 0;
}

bool bar_colour(bool selected, std::uint32_t& rgb)
{
    const Style& s = *g_style;
    if (!(selected ? s.selected_fill_set : s.fill_set))
        return false;
    rgb = selected ? s.selected_fill : s.fill;
    return true;
}

bool bars_coloured()
{
    return any_set([](const Style& s) { return s.fill_set || s.selected_fill_set; });
}

void enter_window(const void* window)
{
    if (g_depth < kDepthAtMost) {
        g_marks[g_depth] = g_label_count;
        g_outer[g_depth] = g_style;
        g_style = &style_for(window); // (deeper than a mark: the window it's in's)
        apply_patches(*g_style);      // the main menu's own nudge and flash for its draw
    }
    ++g_depth;
}

bool nudging()
{
    return (g_written & 1) != 0 || g_patches[6].at == 0; // (the button place's bytes written, or the game's: not found)
}

void leave_window()
{
    if (g_depth == 0)
        return;
    --g_depth;
    if (g_depth >= kDepthAtMost)
        return; // (too deep to have a mark: its letters go with the window it's in)
    const int mark = g_marks[g_depth];
    if (mark < g_label_count)
        vanacore_gfx_button_drawing(1); // (a button's letters, drawn after its shape: True Font's buttons')
    for (int i = mark; i < g_label_count; ++i) {
        const Label& l = g_labels[i];
        itemslot::draw_game(const_cast<char*>(l.quad), l.x, l.y, l.sx, l.sy, l.colour, l.flags);
    }
    if (mark < g_label_count)
        vanacore_gfx_button_drawing(0);
    g_label_count = mark;
    g_style = g_outer[g_depth];
    apply_patches(*g_style);
}

bool on_quad(const char* quad, int& x, int& y, float sx, float sy, std::uint32_t& colour, int flags)
{
    if (!g_now.active)
        return false;
    if (starts(quad, kBar, sizeof kBar - 1)) {
        if (!g_now.flat)
            return false;
        if (g_now.wells)
            return true;
        if (!g_now.drawn) {
            g_now.drawn = true;
            if (!draw_box(quad, {x, y, sx, sy, (colour & 0xFF000000) | kNeutral})) {
                g_now.flat = false;
                return false;
            }
        }
        return true;
    }
    if (!starts(quad, kFont, sizeof kFont - 1))
        return false;
    recolour_label(quad, colour, flags);
    if (g_now.flat && !g_now.wells) { // buttons.text.x, .y: the label moved in its box (True Font's letters with it)
        x += static_cast<int>(std::lround(g_style->text_x * sx));
        y += static_cast<int>(std::lround(g_style->text_y * sy));
    }
    if (labeltext::letter(quad, x, y, sx, sy, colour))
        return true; // (a Config window's label: drawn whole, in the colour and at the place worked out here)
    return later(quad, x, y, sx, sy, colour, flags);
}

} // namespace vanacore::buttons
