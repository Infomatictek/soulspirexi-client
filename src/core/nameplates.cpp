#include "nameplates.hpp"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "../gfx/vanacore_gfx.h"
#include "d3d8.hpp"
#include "dats.hpp"
#include "game.hpp"
#include "gameui.hpp"
#include "gmicons.hpp"
#include "soulspire/echo_icons.hpp"
#include "log.hpp"
#include "nameplates_debug.hpp"
#include "pivot.hpp"
#include "plugins.hpp"
#include "scan.hpp"
#include "text.hpp"
#include "trusts.hpp"
#include "widgets.hpp"
#include "window.hpp"

namespace vanacore::nameplates {
namespace {

// FFXiMain.dll draws the names over characters' heads as screen-space quads in the 3D scene's picture
// (xi-on-mac found it, 682def6). Its letters are sprites of the "font    fontshp" set, each a cell of a
// font sheet and a size in the game's units; a letter's corners are offsets in units from the projected
// point, times an across and a down factor it takes from the picture's width and height, and the next
// letter starts the cell's width on. The factors, as it sets them:
//   call (width); and eax,0FFFFh; mov [esp+10h],eax; fild [esp+10h]; fmul [c]; fmul [esp+6E0h] (size);
//   fstp [esp+4Ch] (across); call (height); mov edi,[esp+6DCh]; and eax,0FFFFh; mov [esp+10h],eax;
//   or ecx,-1; fild [esp+10h]; xor eax,eax; repne scasb; fmul [c]; fmul [esp+6E0h] (down)
constexpr char kFactors[] = "E8 ?? ?? ?? ?? 25 FF FF 00 00 89 44 24 10 DB 44 24 10 D8 0D ?? ?? ?? ?? D8 8C 24 E0 06 00 00 "
                            "D9 5C 24 4C E8 ?? ?? ?? ?? 8B BC 24 DC 06 00 00 25 FF FF 00 00 89 44 24 10 83 C9 FF "
                            "DB 44 24 10 33 C0 F2 AE D8 0D ?? ?? ?? ?? D8 8C 24 E0 06 00 00";
constexpr std::uintptr_t kAcross = 0x14; // the first fmul [c]'s address
constexpr std::uintptr_t kDown = 0x45;   // the second's
// Each letter's sprite from the font, a __thiscall (letter; its four corners' texture coordinates, its
// size, its x and y offsets, all written):
//   push edx; push eax; push esi; push ecx; push ebx; mov ecx,<font>; call; test edi,edi; jz; cmp bl,8Eh
constexpr char kLetterCall[] = "52 50 56 51 53 B9 ?? ?? ?? ?? E8 ?? ?? ?? ?? 85 FF 0F 84 ?? ?? ?? ?? 80 FB 8E";
constexpr std::uintptr_t kLetterCallAt = 10;
// The sheet the sprites' cells are in, and the DAT it's in (the English client's; through pivot, so a
// font mod's sheet is the one measured).
constexpr wchar_t kSheetDat[] = L"ROM\\119\\51.DAT";
constexpr char kSheetName[] = "font    font    ";
// With spacing on, letters' sizes and offsets are handed over in quarter units and the factors
// quartered, so a letter can move by less than a unit (about 4 pixels at 4K).
constexpr int kFine = 4;
// A letter's cell is its core (the bright, opaque strokes) inside a half-transparent dark outline and
// some blank columns (ffxi-hd's: two to four texels each side), so side by side the cores are 5 to 7
// texels apart. Its core is where the alpha is at least this (the outline is about half), or, in a font
// whose letters are never that opaque, at the letter's own most opaque level (CatsEyeXI's italic font:
// its fill is 136, its outline 119). A cell with nothing above kBlankAlpha is a space.
constexpr std::uint8_t kCoreAlpha = 160;
constexpr std::uint8_t kBlankAlpha = 32;
constexpr int kMaxLetters = 64; // the game's arrays hold 36 (it cuts names there), and some letters take two
// Each character's name is drawn by its actor (a __thiscall on it, FFXiMain.dll+0xD0260 in CatsEyeXI's
// client): past the game's own filters (its name display setting; the target's is drawn apart), it
// works out the name and its colours, then draws it (a __thiscall too: five arguments, ret 14h):
//   push ecx; push edx; mov ecx,esi; call (the name); mov eax,[esi]; mov ecx,esi; call [eax+0D0h]
constexpr char kNameCall[] = "51 52 8B CE E8 ?? ?? ?? ?? 8B 06 8B CE FF 90 D0 00 00 00";
constexpr std::uintptr_t kNameCallAt = 4;
constexpr std::uintptr_t kActorEntity = 0x70; // the actor's entity, as those filters read it
// The line under the name (a job master's stars, glyph 0xD2, all it holds), drawn after it in the same
// routine, a __thiscall too (three arguments: where, the text, the size; ret 0Ch):
//   push edx; push eax; push ecx; mov ecx,esi; call (the line); pop edi; pop esi; add esp,118h; ret
constexpr char kLineCall[] = "52 50 51 8B CE E8 ?? ?? ?? ?? 5F 5E 81 C4 18 01 00 00 C3";
constexpr std::uintptr_t kLineCallAt = 5;
constexpr std::size_t kLineWithin = 0x80; // how far after the name's call it is
// The entity: its server id (0x78: the top byte 0 for a player), its flags (0x124: 0x8000000 in a
// linkshell) and its linkshell's colour (0x1D4, 0x00BBGGRR).
constexpr std::uintptr_t kEntityIndex = 0x74, kEntityId = 0x78, kEntityFlags1 = 0x124, kEntityShellColour = 0x1D4;
constexpr std::uint32_t kInShell = 0x8000000;
// The icons the game puts before a name (FFXiMain.dll's name builder, FUN_10097650 in CatsEyeXI's
// client): one status icon at most (the bazaar bag 0x9C among them, in place of the linkshell's pearl
// 0x92, which the game tints the colour of the call's last argument), then a space.
constexpr unsigned char kBazaar = 0x9C, kPearl = 0x92;
// The game's colours for names (its name colour table, 0xC0 alpha, channels to 0x80): a player's white,
// an NPC's green, an unclaimed monster's yellow, a monster claimed by someone else purple, by you or your
// party red. Party members and the rest have their own, left as they are.
constexpr std::uint32_t kPlayerColour = 0x808080, kNpcColour = 0x618061, kMonsterColour = 0x808061,
                        kClaimedColour = 0x804180, kMineColour = 0x804141;
// How big the game draws a name (nameplates.smooth): from its depth in front of the camera, times a scale, cut to a
// whole number (FFXiMain.dll+0x82EA8, _ftol), then (far - depth) / (far - near), 1 nearer than near; far - near is 80,
// so a name grows and shrinks in 80 steps. The depth before the cut is caught where it's cut, so the name can be sized
// from it smoothly:
//   fld [esp+0Ch]; fmul [scale]; mov [esi+4],eax; call _ftol (the depth); lea ecx,[esp+14h]; mov [esi+8],eax;
//   mov dword ptr [esi+0Ch],0
constexpr char kDepthCut[] = "D9 44 24 0C D8 0D ?? ?? ?? ?? 89 46 04 E8 ?? ?? ?? ?? 8D 4C 24 14 89 46 08 C7 46 0C 00 00 00 00";
constexpr std::uintptr_t kDepthCutAt = 13;
// The far and near depths, where the size is worked out (FFXiMain.dll+0x82F00): mov ecx,[ebx+8] (the depth);
// mov eax,[far]; cmp ecx,eax; jbe; (no name past it) ...; mov edx,[near]; xor ebx,ebx; cmp ecx,edx; jae
constexpr char kDepthRange[] = "8B 4B 08 A1 ?? ?? ?? ?? 3B C8 76 0C 5F 5E 5D 32 C0 5B 83 C4 08 C2 10 00 8B 15 ?? ?? ?? ?? 33 DB "
                               "3B CA 73 08";
constexpr std::uintptr_t kFarAt = 4, kNearAt = 0x1A;
// The local player's entity, as the game gets it for its own "only my name" filter:
//   mov ax,[index]; test ax,ax; jz; movsx eax,ax; mov eax,[eax*4+entities]; test eax,eax; jnz; xor eax,eax; ret
constexpr char kPlayerEntity[] = "66 A1 ?? ?? ?? ?? 66 85 C0 74 0E 0F BF C0 8B 04 85 ?? ?? ?? ?? 85 C0 75 02 33 C0 C3";
// nameplates.order. The game draws each actor's overlays (the names among them) walking its list of actors, with depth
// writes off: the name drawn last is on top, near or far. Walked here by distance instead, far to near, so a nearer
// name covers a farther one. The walk:
//   mov esi,[<list>]; test esi,esi; jz (what follows); fld [esi+8Ch]; fcomp [<limit>]; fnstsw ax; and eax,100h; jnz (next);
//   mov edx,[esi]; mov ecx,esi; call [edx+284h]; mov esi,[esi+54h]; test esi,esi; jnz (the fld); (what follows)
constexpr char kActorWalk[] = "8B 35 ?? ?? ?? ?? 85 F6 74 26 D9 86 8C 00 00 00 D8 1D ?? ?? ?? ?? DF E0 25 00 01 00 00 75 0A "
                              "8B 16 8B CE FF 92 84 02 00 00 8B 76 54 85 F6 75 DA";
constexpr std::uintptr_t kWalkList = 2, kWalkLimit = 18, kWalkAfter = 48;
constexpr std::size_t kActorVtableSlot = 0x284 / 4;
constexpr std::uintptr_t kActorNext = 0x54, kActorLimit = 0x8C;
constexpr std::uintptr_t kEntityDistance = 0xD8; // squared yalms from the local player
// The name routine (kNameCall's) is within this far of its start
constexpr std::uintptr_t kNameRoutineSpan = 0x400;
constexpr std::size_t kMostActors = 512; // sorted at most; the rest drawn first, in the game's order

// nameplates.font=sharp. The game lays a name out (its icons, a space, its letters) centred on the point over the head
// and draws a quad per letter from its font sheet: kCornerCall works out each of a letter's corners, kMaterialCall sets
// the letter's texture and colour (0 back ends the name there), and kDrawCall draws its quad. Sharp, the name's first
// letter of its own takes the whole text's width and those after it none, so the icons before it sit where they would;
// where the game draws that letter, the text is drawn instead, by the game's own draw with the device as the game has
// it for its letters, one quad from a picture of it at the screen's pixels; the letters after it draw nothing.
//   fstp [esp+60h]; fld [esp+5Ch]; fmul [edi]; fstp [esp+64h]; call (a corner); mov eax,[esp+54h] (the depth);
//   mov ecx,[ebp-4]; mov edx,[ebp]; mov [esi],eax; mov dword ptr [esi+4],1.0
constexpr char kCornerCall[] = "D9 5C 24 ?? D9 44 24 ?? D8 0F D9 5C 24 ?? E8 ?? ?? ?? ?? 8B 44 24 ?? 8B 4D FC 8B 55 00 "
                               "89 06 C7 46 04 00 00 80 3F";
//   push edx; push eax; mov ecx,<font>; call (the letter's texture and colour); test al,al; jz (the name's end)
constexpr char kMaterialCall[] = "52 50 B9 ?? ?? ?? ?? E8 ?? ?? ?? ?? 84 C0 0F 84";
constexpr std::uintptr_t kMaterialCallAt = 7;
// The game's renderer, its device kRendererDevice in: mov edx,[<renderer>]; push 0; mov eax,[edx+0Ch]
constexpr char kRenderer[] = "8B 15 ?? ?? ?? ?? 6A 00 8B 42 0C";
constexpr std::uintptr_t kRendererDevice = 0xC;
// The letter's quad drawn: mov ecx,[<renderer>]; lea edx,[esp+1E4h] (its 4 vertices); push 1Ch (their stride);
// push edx; push 2 (triangles); push 5 (a strip); call (the renderer's DrawPrimitiveUP, __thiscall)
constexpr char kDrawCall[] = "8B 0D ?? ?? ?? ?? 8D 94 24 ?? ?? ?? ?? 6A 1C 52 6A 02 6A 05 E8";
constexpr std::uintptr_t kDrawCallAt = 20;
constexpr std::size_t kSharpWithin = 0x100; // the material's call after the corner's, the renderer and the draw after
constexpr std::size_t kStarsWithin = 0x800; // the line under the name's own draw call, after the name's
// kFactors' calls of the width and the height the game lays names out in (its 3D scene's)
constexpr std::uintptr_t kWidthCall = 0, kHeightCall = 0x23;
// The text's letters over the game's letter cells (which hold an outline and blank rows as well), in height
constexpr float kTextShare = 0.72f;
constexpr ULONGLONG kSpriteKept = 30000; // a name's picture unused this long is let go
constexpr std::size_t kMostSprites = 1024;
// Pictures made in a frame at most (a size slider dragged makes every name's again): the rest are drawn from their
// picture at the size before, scaled, or with the game's letters if there's none, till their turn
constexpr int kPicturesPerFrame = 6;
// A name grows and shrinks with the distance, smoothly (the game's size for it goes with the depth). It's drawn from a
// picture made at the nearest of a ladder of sizes, kRung apart, scaled to its size and placed by its letters, where it
// falls, never rounded to whole pixels; the letters' shapes are the font's at any size (text::Style::scalable), so a name
// moving on to another picture of the ladder looks the same.
constexpr float kEighths = 8.0f;      // pictures' sizes are kept in eighths of a pixel
constexpr float kLowest = 6.0f;       // the ladder's lowest size, pixels
constexpr float kRung = 1.0905077f;   // each size on it this many times the one under it (2 to the 1/8)
// A picture drawn at less than kLeast or more than kMost of its size gives way to the nearest on the ladder (scaled down
// further, thin strokes come and go between pixels; up, it blurs)
constexpr float kLeast = 0.94f;
constexpr float kMost = 1.06f;
constexpr float kShownAlpha = 192.0f; // a name's alpha shown in full (its colours' 0xC0; less as it fades out)

struct Sheet {
    int width = 0, height = 0;
    std::vector<std::uint8_t> alpha; // a byte per texel, rows top down
};

struct Margins {
    float left = -1.0f, right = -1.0f; // texels from the cell's sides to its core; < 0: no core (a space)
};

// The name the game is getting letters for: it gets them all (their sizes and offsets into arrays of
// its own) before drawing any, so each letter can still be moved when the next one comes.
struct Name {
    short* first = nullptr;       // its x offsets array
    int last = -1;                // the last letter's index in it
    float right = -1.0f;          // that letter's right margin, in fine units (< 0: a space, or none)
    float total = 0.0f;           // how much narrower the name is drawn than the game laid it out
    bool seen[kMaxLetters] = {};
    short base[kMaxLetters] = {}; // each letter's x offset as the game had it, in fine units
    float shift[kMaxLetters] = {}; // how far left of its place it's drawn
};

using LetterFn = bool(__fastcall*)(void* font, void* edx, unsigned letter, float* uv, short* size, short* x, short* y);
using NameFn = void(__fastcall*)(void* actor, void* edx, std::uint32_t at, std::uint32_t name, std::uint32_t size,
                                 std::uint32_t a4, std::uint32_t a5);
using LineFn = void(__fastcall*)(void* actor, void* edx, std::uint32_t at, std::uint32_t text, std::uint32_t size);
using PlayerFn = void*(__cdecl*)();
using DrawFn = std::uint32_t(__fastcall*)(void* renderer, void* edx, unsigned prim, unsigned count,
                                          const void* vertices, unsigned stride);
using MaterialFn = bool(__fastcall*)(void* font, void* edx, unsigned letter, std::uint32_t* colour, std::uint32_t* tint);
using SizeFn = std::uint32_t(__cdecl*)(); // the low 16 bits

// Only touched on the game's thread.
bool g_searched = false;
unsigned g_searched_at = 0;      // plugins::changes() when kFactors was last looked for
std::uintptr_t g_site = 0;       // kFactors, found
const float* g_game = nullptr;   // the constant both factors multiply by
float g_across = 0.0f;           // VanaCore's, which the fmuls point at instead
float g_down = 0.0f;
float g_shape = 0.0f;            // the window's last shape
float g_fix = 1.0f;              // how much narrower than the game's the across factor is made for the shape (1: not)
bool g_sheet_tried = false;
Sheet g_sheet;
bool g_hook_tried = false;       // since kFactors was found
bool g_hooked = false;
LetterFn g_letter = nullptr;     // the game's
int g_fine = 1;                  // the units letters are handed over in: kFine with spacing on, else 1
float g_gap = 3.0f;              // texels between letters' cores
std::unordered_map<std::uint64_t, Margins> g_margins;
Name g_name;
std::wstring g_state;
bool g_names_searched = false;
Names g_names;                    // [graphics] nameplates.*, for the names' hooks
NameFn g_draw_name = nullptr;     // the game's
LineFn g_draw_line = nullptr;     // the game's
PlayerFn g_player = nullptr;      // the game's
const void* g_hidden = nullptr;   // the actor whose name was just left out: the line under it too
bool g_hide_all = false;          // an addon has vana.gameui.hide('nameplates'): no name is drawn
bool g_swallow = false;           // a hidden name that widgets sit at is being laid out: its quads draw nothing
void* const* g_renderer_slot = nullptr; // where the game keeps its renderer (kRenderer's operand), for Pass
// nameplates.smooth (kDepthCut)
bool g_depth_tried = false;
void* g_ftol = nullptr;                       // the game's _ftol
float g_depth = 0.0f;                         // the depth it cut last, before the cut
const std::uint32_t* g_far_depth = nullptr;   // where names go (0) and where they're whole (1)
const std::uint32_t* g_near_depth = nullptr;
std::uint32_t g_size_from = 0, g_size_to = 0; // the last name's size, as the game had it and sized smoothly
char g_text[256];                 // a name as drawn, its icons changed
// Players' pets, by target index: whose pet each player has (the players' entities' pet index), looked
// over every kPetsEvery ms while their colour is set.
constexpr unsigned kPlayersFrom = 0x400, kPlayersTo = 0x700; // the players' target indexes
constexpr ULONGLONG kPetsEvery = 500;
bool g_pets[0x1000] = {};
ULONGLONG g_pets_at = 0;

// nameplates.font=sharp
struct Cached {
    text::Sprite sprite;
    std::string text;
    int eighths = 0; // its letters' size, in eighths of a pixel
    ULONGLONG used = 0;
};
std::unordered_map<std::string, std::unique_ptr<Cached>> g_sprites; // names' pictures, by "<eighths>|<text>"
std::unordered_map<std::string, Cached*> g_latest;                  // each text's last made, by its text
// Each name as it's drawn, by the actor drawing it: its picture's place on the ladder (kRung; -1 none yet)
struct Held {
    std::string text;
    int rung = -1;
    ULONGLONG used = 0;
};
std::unordered_map<const void*, Held> g_held;
std::vector<std::unique_ptr<Cached>> g_retired; // in a look since changed: drawn till their text's is made again
int g_made_this_frame = 0;
std::wstring g_sprite_style;      // the font, bold, italic and spacing they're in
void* g_sprite_device = nullptr;  // the device their textures are on
ULONGLONG g_swept_at = 0;
// The name being drawn sharp
struct Sharp {
    bool on = false;
    const void* actor = nullptr;  // whose name it is
    const char* text = nullptr;   // its own letters, after its icons
    std::ptrdiff_t box = -1;      // the first one's index: where the text goes
    const short* first = nullptr; // its letters' x offsets, which the index counts in
    float size = 1.0f;            // the size the game draws it at
    float exact = 0.0f;           // its letters' height on the screen, pixels
    std::ptrdiff_t calls = 0;     // kMaterialCall's so far: the index of the letter it's for
    bool drawing = false;         // the letter the game draws next is the text's place
    bool drawn = false;           // the text is drawn: the letters after it draw nothing
    Cached* picture = nullptr;
    float scale = 1.0f;           // the picture drawn this many times its size
};
Sharp g_sharp;
bool g_sharp_tried = false;
bool g_sharp_ready = false;
// A name's plate, as the game draws it: its icon (the linkshell's pearl, the bazaar bag) and the space after it, its
// letters, and the line under it (the mastery stars). Each of them is moved by its own settings, and the whole plate by
// nameplates.plate.x/.y; with nameplates.center the icon and the space are left out of what the letters are centred by.
enum class Part { name, icon, line };
Part g_part = Part::name;         // the quad the game draws next is of this
unsigned g_icon_glyph = 0;        // ... and it's this icon's (kPearl, kBazaar, ...), not the space after it (0)
bool g_text_seen = false;         // the name's first letter past its icons has come
bool g_in_line = false;           // the line under a name is being drawn
// Where each name was last drawn, by its target's index (see name_anchor)
struct Anchor {
    float x = 0.0f, top = 0.0f, bottom = 0.0f;
    float left = 0.0f, right = 0.0f, z = 0.0f, rhw = 1.0f; // the name's edges and its first quad's depth (widgets.hpp)
    ULONGLONG at = 0;
    unsigned serial = 0;          // the name it was noted for (g_name_serial)
};
constexpr unsigned kAnchors = 0x1000;
constexpr ULONGLONG kAnchorLife = 300; // ms a name's place is kept
Anchor g_anchors[kAnchors];
unsigned g_name_index = ~0u;      // the target index of the name being drawn
unsigned g_name_serial = 0;
ULONGLONG g_anchor_seen = 0;       // when the last name was noted
// nameplates.stars.style. The line under a job master's name holds 0xD2, which the game (FFXiMain.dll+0x868E9) copies
// as a glyph per star, one quad each, in order: 0xD3 and 0xD5 the small ones, 0xD4 the big one between them (each draws
// glyph 0xAD, the small ones at half its size, all standing on one line). The line's codes, and how many quads of it
// are drawn so far
constexpr unsigned char kStarAll = 0xD2;
constexpr unsigned char kStarBig = 0xD4;
constexpr unsigned char kStarFirst = 0xD3, kStarLast = 0xD5;
constexpr int kMostStars = 8;
unsigned char g_star_codes[kMostStars] = {};
int g_star_count = 0;
int g_star_next = 0;
bool g_star_big = false; // the line has the big one
bool g_last_character = false;    // the name drawn last is a player's (its line comes right after)
bool g_plate_active = false;      // a name or its line is being drawn by VanaCore's hooks, at g_plate_size
float g_plate_size = 1.0f;        // its size, as drawn (the game's for its distance, times nameplates.size)
// The name whose letters the game is getting: how wide its icons and the space are, in the game's own units
struct Plate {
    const short* first = nullptr; // its x offsets array
    int last = -1;                // the last letter's index in it
    float prefix = 0.0f;
    bool past = false;            // the first letter past the icons has come
};
Plate g_plate;
DrawFn g_draw = nullptr;          // the game's
MaterialFn g_material = nullptr;  // the game's
SizeFn g_game_width = nullptr;    // the game's
SizeFn g_game_height = nullptr;
float g_shown_w = 0.0f;           // the size the game's 3D scene is shown at, this frame
float g_shown_h = 0.0f;
float g_ui_scale = 0.0f;          // the screen's pixels per interface pixel down, as True Font's areas have it (0: not known yet)

bool write_code(std::uintptr_t at, const void* bytes, std::size_t size)
{
    DWORD protect = 0;
    if (!VirtualProtect(reinterpret_cast<void*>(at), size, PAGE_EXECUTE_READWRITE, &protect))
        return false;
    std::memcpy(reinterpret_cast<void*>(at), bytes, size);
    VirtualProtect(reinterpret_cast<void*>(at), size, protect, &protect);
    FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(at), size);
    return true;
}

const float* operand(std::uintptr_t offset)
{
    return *reinterpret_cast<const float* const*>(g_site + offset);
}

std::uint32_t u32(const std::uint8_t* p)
{
    return p[0] | p[1] << 8 | p[2] << 16 | static_cast<std::uint32_t>(p[3]) << 24;
}

// The alpha of a DXT1, DXT3 or DXT5 picture (FFXI's names for them, reversed: "1TXD").
bool decode_alpha(const std::uint8_t* data, std::size_t size, char kind, int width, int height,
                  std::vector<std::uint8_t>& out)
{
    const int across = (width + 3) / 4, down = (height + 3) / 4;
    const std::size_t block = kind == '1' ? 8 : 16;
    if (size < static_cast<std::size_t>(across) * down * block)
        return false;
    out.assign(static_cast<std::size_t>(width) * height, 0);
    for (int by = 0; by < down; ++by)
        for (int bx = 0; bx < across; ++bx) {
            const std::uint8_t* b = data + (static_cast<std::size_t>(by) * across + bx) * block;
            std::uint8_t alpha[16];
            if (kind == '1') {
                const unsigned c0 = b[0] | b[1] << 8, c1 = b[2] | b[3] << 8;
                const std::uint32_t bits = u32(b + 4);
                for (int k = 0; k < 16; ++k)
                    alpha[k] = c0 <= c1 && (bits >> (2 * k) & 3) == 3 ? 0 : 255;
            } else if (kind == '3') {
                for (int k = 0; k < 16; ++k)
                    alpha[k] = static_cast<std::uint8_t>((b[k / 2] >> (4 * (k & 1)) & 15) * 17);
            } else {
                std::uint8_t levels[8] = {b[0], b[1]};
                for (int i = 1; i < 7; ++i)
                    levels[i + 1] = b[0] > b[1] ? static_cast<std::uint8_t>(((7 - i) * b[0] + i * b[1]) / 7)
                                    : i < 5 ? static_cast<std::uint8_t>(((5 - i) * b[0] + i * b[1]) / 5)
                                    : static_cast<std::uint8_t>(i == 5 ? 0 : 255);
                std::uint64_t bits = 0;
                for (int i = 0; i < 6; ++i)
                    bits |= static_cast<std::uint64_t>(b[2 + i]) << (8 * i);
                for (int k = 0; k < 16; ++k)
                    alpha[k] = levels[bits >> (3 * k) & 7];
            }
            for (int k = 0; k < 16; ++k) {
                const int x = bx * 4 + (k & 3), y = by * 4 + k / 4;
                if (x < width && y < height)
                    out[static_cast<std::size_t>(y) * width + x] = alpha[k];
            }
        }
    return true;
}

// The font sheet from `file`: its "font    font" picture's alpha. False if it hasn't one it can read.
bool load_sheet(const std::wstring& file, Sheet& sheet)
{
    std::vector<std::uint8_t> dat;
    if (!dats::read_file(file, dat))
        return false;
    for (std::size_t at = 0; at + 16 <= dat.size();) {
        const std::uint32_t info = u32(&dat[at + 4]);
        const std::size_t size = static_cast<std::size_t>(info >> 7 & 0x7FFFF) * 16;
        if (size < 16 || at + size > dat.size())
            break;
        // an image: flag, name, a BITMAPINFOHEADER; flag A1 is DXT: its kind, size and one more, then it
        const std::uint8_t* image = &dat[at + 16];
        if ((info & 0x7F) == 0x20 && size >= 16 + 69 && image[0] == 0xA1 &&
            std::memcmp(image + 1, kSheetName, 16) == 0 && std::memcmp(image + 58, "TXD", 3) == 0) {
            sheet.width = static_cast<int>(u32(image + 21));
            sheet.height = static_cast<int>(u32(image + 25));
            return sheet.width > 0 && sheet.width <= 4096 && sheet.height > 0 && sheet.height <= 4096 &&
                   decode_alpha(image + 69, size - 16 - 69, static_cast<char>(image[57]), sheet.width, sheet.height,
                                sheet.alpha);
        }
        at += size;
    }
    return false;
}

// The margins of the sheet's cell [c0, c1) x [r0, r1): from its sides to its core, measured once.
Margins measure(int c0, int r0, int c1, int r1)
{
    const std::uint64_t key = static_cast<std::uint64_t>(c0) | static_cast<std::uint64_t>(r0) << 16 |
                              static_cast<std::uint64_t>(c1) << 32 | static_cast<std::uint64_t>(r1) << 48;
    if (const auto found = g_margins.find(key); found != g_margins.end())
        return found->second;
    Margins margins;
    std::uint8_t most = 0;
    for (int y = r0; y < r1; ++y)
        for (int x = c0; x < c1; ++x)
            most = std::max(most, g_sheet.alpha[static_cast<std::size_t>(y) * g_sheet.width + x]);
    const std::uint8_t core = std::min(kCoreAlpha, most);
    for (int x = c0; x < c1 && most > kBlankAlpha; ++x)
        for (int y = r0; y < r1; ++y)
            if (g_sheet.alpha[static_cast<std::size_t>(y) * g_sheet.width + x] >= core) {
                if (margins.left < 0)
                    margins.left = static_cast<float>(x - c0);
                margins.right = static_cast<float>(c1 - 1 - x);
                break;
            }
    g_margins.emplace(key, margins);
    return margins;
}

// A letter as the game got it (its size and offsets in units, its cell's corners in texture
// coordinates), handed over in g_fine units. Its x offset moves it left so its core is g_gap from the
// previous letter's, the outlines overlapping (never further apart than the game has them; a space
// keeps its width); then every letter of the name so far moves back right by half of what the name
// has narrowed, so it stays centred on the point. Whole letters are drawn, outline and all.
void space(unsigned code, const float* uv, short* size, short* x, short* y)
{
    // an icon (the linkshell's pearl, the bazaar bag) is on a sheet of its own, not the font's: measured against the
    // font's cell at its uv it would pull the letters after it over the icon, so its width is kept like a space's
    const bool icon = (code & 0xFF) >= 0x80;
    const int width = icon ? 0 : size[0];
    size[0] = static_cast<short>(size[0] * g_fine);
    size[1] = static_cast<short>(size[1] * g_fine);
    *y = static_cast<short>(*y * g_fine);
    const short base = static_cast<short>(*x * g_fine);
    *x = base;

    // a letter past the last one is the same name's; anything else starts another
    std::ptrdiff_t index = g_name.first != nullptr ? x - g_name.first : -1;
    if (index <= g_name.last || index >= kMaxLetters) {
        g_name = Name{};
        g_name.first = x;
        index = 0;
    }
    g_name.last = static_cast<int>(index);

    float u0 = uv[0], u1 = uv[0], v0 = uv[1], v1 = uv[1];
    for (int k = 1; k < 4; ++k) {
        u0 = std::min(u0, uv[2 * k]), u1 = std::max(u1, uv[2 * k]);
        v0 = std::min(v0, uv[2 * k + 1]), v1 = std::max(v1, uv[2 * k + 1]);
    }
    const int c0 = static_cast<int>(std::lround(u0 * g_sheet.width)), c1 = static_cast<int>(std::lround(u1 * g_sheet.width));
    const int r0 = static_cast<int>(std::lround(v0 * g_sheet.height)), r1 = static_cast<int>(std::lround(v1 * g_sheet.height));
    Margins margins;
    float per_unit = 1.0f; // texels
    if (width > 0 && c0 >= 0 && c1 <= g_sheet.width && c1 > c0 && r0 >= 0 && r1 <= g_sheet.height && r1 > r0) {
        per_unit = static_cast<float>(c1 - c0) / size[0];
        margins = measure(c0, r0, c1, r1);
    }
    float overlap = 0.0f;
    if (margins.left >= 0 && g_name.right >= 0)
        overlap = std::max(0.0f, g_name.right + (margins.left - g_gap) / per_unit);
    g_name.total += overlap;
    g_name.seen[index] = true;
    g_name.base[index] = base;
    g_name.shift[index] = g_name.total;
    g_name.right = margins.left >= 0 ? margins.right / per_unit : -1.0f;
    for (int j = 0; j <= index; ++j)
        if (g_name.seen[j])
            g_name.first[j] = static_cast<short>(std::lround(g_name.base[j] - g_name.shift[j] + g_name.total / 2));
}

// ---- nameplates.font=sharp ----------------------------------------------------------------------------------------

void release_sprites()
{
    for (auto& [key, cached] : g_sprites)
        cached->sprite.release();
    for (auto& cached : g_retired)
        cached->sprite.release();
    g_sprites.clear();
    g_retired.clear();
    g_latest.clear();
    g_held.clear();
}

// The pictures in a look given up (another font, spacing): kept as their texts' latest till they're made again
void retire_sprites()
{
    for (auto& [key, cached] : g_sprites)
        g_retired.push_back(std::move(cached));
    g_sprites.clear();
}

// A picture let go (and its text's latest, if it was)
void forget_sprite(std::unordered_map<std::string, std::unique_ptr<Cached>>::iterator at)
{
    Cached* const cached = at->second.get();
    if (const auto latest = g_latest.find(cached->text); latest != g_latest.end() && latest->second == cached)
        g_latest.erase(latest);
    cached->sprite.release();
    g_sprites.erase(at);
}

// The names' pictures not drawn lately let go, now and then
void sweep_sprites()
{
    const ULONGLONG now = GetTickCount64();
    if (now - g_swept_at < 1000)
        return;
    g_swept_at = now;
    for (auto at = g_sprites.begin(); at != g_sprites.end();) {
        const auto next = std::next(at);
        if (now - at->second->used > kSpriteKept)
            forget_sprite(at);
        at = next;
    }
    std::erase_if(g_held, [now](const auto& held) { return now - held.second.used > kSpriteKept; });
    // a retired picture, once its text's is made again (or it's not drawn)
    for (auto at = g_retired.begin(); at != g_retired.end();) {
        Cached* const cached = at->get();
        const auto latest = g_latest.find(cached->text);
        const bool standing_in = latest != g_latest.end() && latest->second == cached;
        if (standing_in && now - cached->used <= kSpriteKept) {
            ++at;
            continue;
        }
        if (standing_in)
            g_latest.erase(latest);
        cached->sprite.release();
        at = g_retired.erase(at);
    }
}

// The picture of `text` (letters only) with letters `eighths` eighths of a pixel high, made now if it isn't yet and
// `make` (nullptr past kPicturesPerFrame this frame).
Cached* picture(const char* text, int eighths, bool make)
{
    std::string key = std::to_string(eighths);
    key += '|';
    key += text;
    auto found = g_sprites.find(key);
    if (found == g_sprites.end()) {
        if (!make || g_made_this_frame >= kPicturesPerFrame)
            return nullptr;
        ++g_made_this_frame;
        if (g_sprites.size() >= kMostSprites) {
            // the least lately drawn makes room
            auto oldest = g_sprites.begin();
            for (auto at = g_sprites.begin(); at != g_sprites.end(); ++at)
                if (at->second->used < oldest->second->used)
                    oldest = at;
            forget_sprite(oldest);
        }
        auto made = std::make_unique<Cached>();
        made->text = text;
        made->eighths = eighths;
        const float pixels = static_cast<float>(eighths) / kEighths;
        text::Style style;
        style.family = g_names.family;
        style.size = static_cast<int>(std::lround(pixels));
        style.exact_size = pixels;
        style.scalable = true;
        style.weight = g_names.bold ? FW_BOLD : FW_NORMAL;
        style.italic = g_names.italic;
        style.spacing = g_names.text_spacing * pixels / 20.0f;
        // an outline as the game's letters have, growing with them: a pixel at 12 pixels high, two at 24
        style.exact_stroke = std::max(1.0f, pixels / 12.0f);
        style.stroke = static_cast<int>(std::ceil(style.exact_stroke));
        style.stroke_color[3] = 0.9f;
        style.background[3] = 0.0f;
        style.padding = 0;
        std::wstring letters(text, text + std::strlen(text));
        if (g_names.upper) // (nameplates.uppercase; names are plain ASCII here)
            for (wchar_t& c : letters)
                c = c >= L'a' && c <= L'z' ? static_cast<wchar_t>(c - 0x20) : c;
        made->sprite.set(letters, style);
        found = g_sprites.emplace(std::move(key), std::move(made)).first;
        g_latest[found->second->text] = found->second.get();
    }
    found->second->used = GetTickCount64();
    return found->second.get();
}

// A size on the ladder (kRung), in eighths of a pixel
int rung_eighths(int rung)
{
    return static_cast<int>(std::lround(kLowest * std::pow(kRung, static_cast<float>(rung)) * kEighths));
}

// The picture `actor`'s name `text` is drawn from, its letters `exact` pixels high this frame (kRung), and the scale
// it's drawn at into `scale`. nullptr if it has none yet (the game's letters this frame).
Cached* follow(const void* actor, const char* text, float exact, float& scale)
{
    const ULONGLONG now = GetTickCount64();
    Held& held = g_held[actor];
    if (held.text != text) {
        held = Held{};
        held.text = text;
    }
    held.used = now;
    Cached* drawn = held.rung >= 0 ? picture(text, rung_eighths(held.rung), false) : nullptr;
    const float share = held.rung >= 0 ? exact * kEighths / static_cast<float>(rung_eighths(held.rung)) : 0.0f;
    if (drawn == nullptr || share > kMost || share < kLeast) {
        const int nearest = std::max(0, static_cast<int>(std::lround(std::log(exact / kLowest) / std::log(kRung))));
        if (Cached* fresh = picture(text, rung_eighths(nearest), true)) {
            drawn = fresh;
            held.rung = nearest;
        }
    }
    if (drawn == nullptr) {
        // none made this frame: the text's latest picture, at whatever size it was made
        const auto latest = g_latest.find(text);
        if (latest == g_latest.end())
            return nullptr;
        drawn = latest->second;
        drawn->used = now;
    }
    scale = exact * kEighths / static_cast<float>(drawn->eighths);
    return drawn;
}

// A letter of the name being drawn sharp, as the game got it (its size and offsets in g_fine units): the text's place,
// its first letter, takes the text's width; the letters after it none.
void sharp_letter(short* size, short* x, short* y)
{
    if (g_sharp.first == nullptr)
        g_sharp.first = x;
    const std::ptrdiff_t index = x - g_sharp.first;
    if (index < g_sharp.box)
        return;
    if (index > g_sharp.box) {
        if (g_sharp.picture != nullptr) {
            size[0] = size[1] = 0;
            *x = *y = 0;
        }
        return;
    }
    // a unit is the game's factor times the name's size times the scene's shown size, in the screen's pixels (across, as
    // narrow as the shape fix makes it: the icons and the space before the text are laid out by it)
    const float unit = *g_game * g_sharp.size / static_cast<float>(g_fine);
    const float across = unit * g_shown_w * g_fix, down = unit * g_shown_h;
    if (!(across > 0.0f) || !(down > 0.0f))
        return;
    // nameplates.fontsize, the interface's pixels at the name's full size, on the screen as True Font's areas are (so 15 px
    // here is as big as a 15 px menu's letters), times the size it's drawn at for its distance; while the interface's scale
    // isn't known yet, by the game's letters' height (18 px about as tall as theirs)
    const float wanted = g_ui_scale > 0.0f ? g_names.font_size * g_ui_scale * g_sharp.size
                                           : size[1] * down * kTextShare * g_names.font_size / 18.0f;
    const float exact = std::clamp(wanted, 6.0f, 256.0f);
    g_sharp.exact = exact;
    g_sharp.picture = follow(g_sharp.actor, g_sharp.text, exact, g_sharp.scale);
    if (g_sharp.picture == nullptr)
        return; // the game's letters this frame
    const long width = std::lround(g_sharp.picture->sprite.width() * g_sharp.scale / across);
    size[0] = static_cast<short>(std::clamp(width, 1L, 0x7FFFL));
}

// A letter's vertices as the game draws them (its FVF: position, rhw, colour, one set of texture coordinates)
struct LetterVertex {
    float x, y, z, rhw;
    DWORD colour;
    float u, v;
};
constexpr unsigned kLetterStride = sizeof(LetterVertex);

// The text where the game draws the name's first letter of its own: its letters' middle on the letter's place's, where
// it falls, at the letter's depth and in its colour, by the game's own draw (the device as the game has it for its
// letters: its texture stage ops, blending and alpha test), only with the text's picture for its texture.
void draw_text(void* renderer, const LetterVertex* letter)
{
    using d3d8::call;
    namespace slot = d3d8::slot;
    void* const device = *reinterpret_cast<void* const*>(static_cast<const char*>(renderer) + kRendererDevice);
    if (device == nullptr || g_sharp.picture == nullptr)
        return;
    if (device != g_sprite_device) {
        for (auto& [key, cached] : g_sprites)
            cached->sprite.release();
        for (auto& cached : g_retired)
            cached->sprite.release();
        g_sprite_device = device;
    }
    text::Sprite& sprite = g_sharp.picture->sprite;
    if (!sprite.upload(device) || sprite.texture() == nullptr || sprite.texture_width() == 0 ||
        sprite.texture_height() == 0)
        return;
    const float game_w = static_cast<float>(g_game_width() & 0xFFFF);
    const float game_h = static_cast<float>(g_game_height() & 0xFFFF);
    if (!(game_w > 0.0f) || !(game_h > 0.0f) || !(g_shown_w > 0.0f) || !(g_shown_h > 0.0f))
        return;
    // the game's pixel p (whole numbers at pixels' centres, as Direct3D 8 has them) is p + 0.5 of its pixels from the
    // left, times the scale on the screen
    const float kx = g_shown_w / game_w, ky = g_shown_h / game_h;
    const float w = static_cast<float>(sprite.width()), h = static_cast<float>(sprite.height());
    const float drawn_w = w * g_sharp.scale, drawn_h = h * g_sharp.scale;
    // by the letters (their advance's middle across, their line's down), not the picture's box, which pictures of
    // other sizes round differently; nothing rounded to whole pixels, which would step against the size
    const float at_x = ((letter[0].x + letter[3].x) * 0.5f + 0.5f) * kx, at_y = ((letter[0].y + letter[3].y) * 0.5f + 0.5f) * ky;
    const float left = at_x - (static_cast<float>(sprite.pen_left()) + sprite.exact_advance() * 0.5f) * g_sharp.scale;
    const float top = at_y - sprite.middle() * g_sharp.scale;
    debug::trace_row({g_sharp.actor, g_sharp.text, g_sharp.size, g_sharp.exact,
                      static_cast<float>(g_sharp.picture->eighths) / kEighths, g_sharp.scale, at_x, at_y, left, top, drawn_w,
                      drawn_h});
    const float x0 = left / kx - 0.5f, x1 = (left + drawn_w) / kx - 0.5f;
    const float y0 = top / ky - 0.5f, y1 = (top + drawn_h) / ky - 0.5f;
    const float u = w / static_cast<float>(sprite.texture_width()), v = h / static_cast<float>(sprite.texture_height());
    debug::note_letter_state(device, letter[0].colour, letter[0].z, g_sharp.text);

    // The game's letters have their alpha multiplied up (alpha op MODULATE4X by the vertex's 0xC0: three times), for
    // its font's texels about a third opaque. The picture's own would come out three times as opaque, its smoothed
    // edges hard, and pixels at its edges popping on and off as it moves between them: the vertices' alpha taken down
    // by as much, so the picture is drawn with its own alpha (times the name's fade, which the vertex alpha holds).
    DWORD alpha_op = 0;
    call(device, slot::GetTextureStageState, DWORD{0}, d3d8::tss::AlphaOp, &alpha_op);
    const float alpha_times = alpha_op == d3d8::kTopModulate4x   ? 4.0f
                              : alpha_op == d3d8::kTopModulate2x ? 2.0f
                              : alpha_op == d3d8::kTopModulate   ? 1.0f
                                                                 : 0.0f;
    const auto colour = [alpha_times](DWORD argb) {
        if (alpha_times == 0.0f)
            return argb;
        const float alpha = std::min(255.0f, static_cast<float>(argb >> 24) * (255.0f / kShownAlpha) / alpha_times);
        return (argb & 0xFFFFFFu) | static_cast<DWORD>(std::lround(alpha)) << 24;
    };
    // the letter's own depth, rhw and colour: the game's texture stage ops scale the colour as they do its letters'
    const LetterVertex quad[4] = {{x0, y0, letter[0].z, letter[0].rhw, colour(letter[0].colour), 0.0f, 0.0f},
                                  {x1, y0, letter[1].z, letter[1].rhw, colour(letter[1].colour), u, 0.0f},
                                  {x0, y1, letter[2].z, letter[2].rhw, colour(letter[2].colour), 0.0f, v},
                                  {x1, y1, letter[3].z, letter[3].rhw, colour(letter[3].colour), u, v}};
    // The alpha test the game has for some names (0x60, for its font's) would cut the picture's smoothed edges at
    // the same place each frame, so they'd pop as it moves: only fully see-through pixels left out. Put back after.
    DWORD alpha_ref = 0;
    call(device, slot::GetRenderState, d3d8::rs::AlphaRef, &alpha_ref);
    if (alpha_ref > 1)
        call(device, slot::SetRenderState, d3d8::rs::AlphaRef, DWORD{1});
    // the picture at its pixels, smoothly when it's scaled; put back after
    struct Saved {
        DWORD type, value;
    };
    Saved saved[] = {{d3d8::tss::MagFilter, 0}, {d3d8::tss::MinFilter, 0}, {d3d8::tss::MipFilter, 0},
                     {d3d8::tss::AddressU, 0},  {d3d8::tss::AddressV, 0}};
    for (Saved& s : saved)
        call(device, slot::GetTextureStageState, DWORD{0}, s.type, &s.value);
    void* texture = nullptr;
    call(device, slot::GetTexture, DWORD{0}, &texture);
    const auto stage = [&](DWORD type, DWORD value) { call(device, slot::SetTextureStageState, DWORD{0}, type, value); };
    stage(d3d8::tss::MagFilter, d3d8::kFilterLinear);
    stage(d3d8::tss::MinFilter, d3d8::kFilterLinear);
    stage(d3d8::tss::MipFilter, d3d8::kFilterNone);
    stage(d3d8::tss::AddressU, d3d8::kAddressClamp);
    stage(d3d8::tss::AddressV, d3d8::kAddressClamp);
    call(device, slot::SetTexture, DWORD{0}, sprite.texture());
    g_draw(renderer, nullptr, d3d8::kTriangleStrip, 2u, static_cast<const void*>(quad), kLetterStride);
    call(device, slot::SetTexture, DWORD{0}, texture);
    if (texture != nullptr)
        call<ULONG>(texture, slot::Release);
    for (const Saved& s : saved)
        stage(s.type, s.value);
    if (alpha_ref > 1)
        call(device, slot::SetRenderState, d3d8::rs::AlphaRef, alpha_ref);
}

// How far, in the game's pixels, the quad of `part` is moved from where the game draws it: nameplates.plate for all of
// them, then its part's; the name's letters and its icon left of centre by half the icon's and the space's width with
// nameplates.center (the game centres those with the letters).
void plate_shift(Part part, float& dx, float& dy)
{
    const float s = g_plate_size;
    dx = g_names.plate_x * s;
    dy = g_names.plate_y * s;
    if (part == Part::line) {
        dx += g_names.stars_x * s;
        dy += g_names.stars_y * s;
        return;
    }
    if (part == Part::icon) {
        const bool bag = g_icon_glyph == kBazaar; // nameplates.bazaar.x/.y: the bag from where the pearl's put it
        // .x closes the gap to the name as 16:9 has it (g_fix 0.75); a wider window's shape fix narrows the gap but not
        // the icon, so the move narrows by the square (38 is 21 at 2.39:1), or the name's letters cover the icon
        dx += (g_names.pearl_x + (bag ? g_names.bazaar_x : 0.0f)) * s * std::min(1.0f, g_fix * g_fix * 16.0f / 9.0f);
        dy += (g_names.pearl_y + (bag ? g_names.bazaar_y : 0.0f)) * s;
    } else {
        dx += g_names.name_x * s;
        dy += g_names.name_y * s;
    }
    // a width of the game's units is that, times the horizontal factor it lays names out by (kFactors' first fmul, seen
    // as the letters' sizes are handed over in g_fine units), times the scene's width in the game's pixels
    if (g_names.centre && g_plate.prefix > 0.0f && g_site != 0 && g_game_width != nullptr) {
        const float game_w = static_cast<float>(g_game_width() & 0xFFFF);
        const float* const across = operand(kAcross);
        if (game_w > 0.0f && across != nullptr)
            dx -= g_plate.prefix * *across * static_cast<float>(g_fine) * game_w * s * 0.5f;
    }
}

// How many times as wide as the game draws it the icon whose quad this is must be drawn to be nameplates.pearl.shape wide
// over its height on the screen, whatever the window's shape: the quad is in the game's pixels, which the scene's shown
// size stretches by a factor across and another down. 1 when the sizes aren't known.
float icon_across(const LetterVertex* quad)
{
    if (g_game_width == nullptr || g_game_height == nullptr || !(g_shown_w > 0.0f) || !(g_shown_h > 0.0f))
        return 1.0f;
    const float game_w = static_cast<float>(g_game_width() & 0xFFFF);
    const float game_h = static_cast<float>(g_game_height() & 0xFFFF);
    if (!(game_w > 0.0f) || !(game_h > 0.0f))
        return 1.0f;
    float left = quad[0].x, right = quad[0].x, top = quad[0].y, bottom = quad[0].y;
    for (int i = 1; i < 4; ++i) {
        left = std::min(left, quad[i].x);
        right = std::max(right, quad[i].x);
        top = std::min(top, quad[i].y);
        bottom = std::max(bottom, quad[i].y);
    }
    const float width = right - left, height = bottom - top;
    if (!(width > 0.5f) || !(height > 0.5f))
        return 1.0f;
    const float wanted = height * g_names.pearl_shape * (g_shown_h / game_h) / (g_shown_w / game_w);
    return std::clamp(wanted / width, 0.25f, 4.0f);
}

// A quad of the name being drawn: its place added to where that name is (name_anchor)
void note_anchor(const LetterVertex* quad)
{
    float top = quad[0].y, bottom = quad[0].y, left = quad[0].x, right = quad[0].x;
    for (int k = 1; k < 4; ++k) {
        top = std::min(top, quad[k].y), bottom = std::max(bottom, quad[k].y);
        left = std::min(left, quad[k].x), right = std::max(right, quad[k].x);
    }
    Anchor& anchor = g_anchors[g_name_index];
    if (anchor.serial != g_name_serial) {
        anchor.serial = g_name_serial;
        anchor.x = (left + right) * 0.5f, anchor.top = top, anchor.bottom = bottom;
        anchor.left = left, anchor.right = right, anchor.z = quad[0].z, anchor.rhw = quad[0].rhw;
    } else {
        anchor.top = std::min(anchor.top, top), anchor.bottom = std::max(anchor.bottom, bottom);
        anchor.left = std::min(anchor.left, left), anchor.right = std::max(anchor.right, right);
    }
    anchor.at = g_anchor_seen = GetTickCount64();
}

// The quad moved by its part's settings (nameplates.plate, .name, .pearl, .stars, .center): its corners shifted in the game's
// pixels (the vertices' own units, as the text's place is worked out in), at the name's size. They're in `moved` when they are.
const void* plate_moved(LetterVertex (&moved)[4], unsigned prim, unsigned count, const void* vertices, unsigned stride)
{
    if (!g_plate_active || prim != d3d8::kTriangleStrip || count != 2 || stride != kLetterStride || vertices == nullptr)
        return vertices;
    float dx = 0.0f, dy = 0.0f;
    plate_shift(g_part, dx, dy);
    // nameplates.pearl.size (times .bazaar.size for the bag) and .shape: the icon's corners scaled about its middle
    const float size = g_names.pearl_size * (g_icon_glyph == kBazaar ? g_names.bazaar_size : 1.0f);
    const bool grow = g_part == Part::icon && size != 1.0f;
    const bool shape = g_icon_glyph != 0 && g_names.pearl_shape > 0.0f;
    if (dx == 0.0f && dy == 0.0f && !grow && !shape)
        return vertices;
    std::memcpy(moved, vertices, sizeof(moved));
    if (grow || shape) {
        const float mx = (moved[0].x + moved[1].x + moved[2].x + moved[3].x) * 0.25f;
        const float my = (moved[0].y + moved[1].y + moved[2].y + moved[3].y) * 0.25f;
        const float scale = grow ? size : 1.0f;
        const float across = shape ? scale * icon_across(moved) : scale;
        for (LetterVertex& corner : moved) {
            corner.x = mx + (corner.x - mx) * across;
            corner.y = my + (corner.y - my) * scale;
        }
    }
    for (LetterVertex& corner : moved) {
        corner.x += dx;
        corner.y += dy;
    }
    return moved;
}

// In place of the game's call drawing each letter's quad: the text in the place of the name's first letter of its
// own; nothing for the letters after it (their sizes are 0).
std::uint32_t __fastcall draw_letter(void* renderer, void* edx, unsigned prim, unsigned count, const void* vertices,
                                     unsigned stride)
{
    LetterVertex moved[4];
    vertices = plate_moved(moved, prim, count, vertices, stride);
    if (g_name_index < kAnchors && !g_in_line && prim == d3d8::kTriangleStrip && count == 2 && stride == kLetterStride &&
        vertices != nullptr)
        note_anchor(static_cast<const LetterVertex*>(vertices));
    if (g_swallow) // a hidden name that widgets sit at: laid out for its place, drawn by nothing
        return 0;
    // nameplates.gm: a staff member's GM icon as CatsEyeXI's picture of it
    const gmicons::Look gm{g_names.gm_size, g_names.gm_x * g_plate_size, g_names.gm_y * g_plate_size};
    if (g_part == Part::icon && gmicons::draw(renderer, g_draw, prim, count, vertices, stride, gm))
        return 0;
    // QA-SOUL-COMPANIONS-001: Individual Soul nameplate icon beside summoned Soul Echoes (zero new hooks).
    if (g_part == Part::name)
        soulspire::echo_icons::draw_nameplate_icon(renderer, g_draw, prim, count, vertices, stride);
    if (g_sharp.on && g_sharp.picture != nullptr) {
        if (g_sharp.drawing && prim == d3d8::kTriangleStrip && count == 2 && stride == kLetterStride &&
            vertices != nullptr) {
            g_sharp.drawing = false;
            g_sharp.drawn = true;
            draw_text(renderer, static_cast<const LetterVertex*>(vertices));
            return 0;
        }
        if (g_sharp.drawn)
            return 0;
    }
    return g_draw(renderer, edx, prim, count, vertices, stride);
}

// In place of the game's call setting each letter's texture and colour: the name's first letter of its own noted,
// for the text to be drawn in its place.
bool __fastcall material(void* font, void* edx, unsigned code, std::uint32_t* colour, std::uint32_t* tint)
{
    const unsigned c = code & 0xFF;
    if (g_in_line) {
        g_part = Part::line;
    } else if ((c >= 0x80 || c == ' ') && !g_text_seen) {
        g_part = Part::icon;
    } else {
        g_part = Part::name;
        g_text_seen = true;
    }
    g_icon_glyph = g_part == Part::icon && c >= 0x80 ? c : 0;
    gmicons::note(c);
    if (g_sharp.on && g_sharp.calls++ == g_sharp.box && g_sharp.picture != nullptr)
        g_sharp.drawing = true;
    return g_material(font, edx, code, colour, tint);
}

// A letter of the name the game is getting: while they're its icons and the space after them (past them the name's own
// letters), their widths add up to g_plate.prefix, in the game's units before g_fine. A letter past the last one is the
// same name's; anything else starts another (as space() tells them).
void note_prefix(unsigned code, int width, const short* x)
{
    std::ptrdiff_t index = g_plate.first != nullptr ? x - g_plate.first : -1;
    if (index <= g_plate.last || index >= kMaxLetters) {
        g_plate = Plate{};
        g_plate.first = x;
        index = 0;
    }
    g_plate.last = static_cast<int>(index);
    if (g_plate.past)
        return;
    const unsigned c = code & 0xFF;
    if (c >= 0x80 || c == ' ')
        g_plate.prefix += static_cast<float>(width);
    else
        g_plate.past = true;
}

// In place of the game's call drawing the quad of the line under a name (its own routine's, with its own draw call,
// FFXiMain.dll+0x86C37 in CatsEyeXI's client): moved by nameplates.plate and .stars, and each star kept, left out or
// made small by nameplates.stars.style (the big one made level with the small ones, nameplates_stars.h).
DrawFn g_draw_stars = nullptr; // the game's

#include "nameplates_stars.h" // nameplates.stars.spacing, .size: a line's stars held, drawn placed once it's done
std::uint32_t __fastcall draw_stars(void* renderer, void* edx, unsigned prim, unsigned count, const void* vertices,
                                    unsigned stride)
{
    LetterVertex moved[4];
    if (g_in_line && prim == d3d8::kTriangleStrip && count == 2 && stride == kLetterStride && vertices != nullptr) {
        const LetterVertex* const quad = static_cast<const LetterVertex*>(vertices);
        float dx = 0.0f, dy = 0.0f;
        if (g_plate_active)
            plate_shift(Part::line, dx, dy);
        const QuadBox span = box_of(quad);
        const float width = span.right - span.left;
        // the glyphs' quads come in the text's order; a glyph that draws nothing has no width
        bool shrink = false;
        if (g_names.star_style != StarStyle::game && width > 0.5f) {
            const int index = g_star_next++;
            const unsigned char code = index < g_star_count ? g_star_codes[index] : 0;
            if (g_star_big && code != 0 && code != kStarBig && g_names.star_style == StarStyle::one)
                return 0;
            shrink = code == kStarBig && g_names.star_style == StarStyle::tiny;
            if (code != 0 && code != kStarBig)
                note_small_star(quad); // (what the big one's made level with)
        }
        if (dx != 0.0f || dy != 0.0f || shrink) {
            std::memcpy(moved, vertices, sizeof(moved));
            if (shrink)
                shrink_big_star(moved);
            for (LetterVertex& corner : moved) {
                corner.x += dx;
                corner.y += dy;
            }
            vertices = moved;
        }
        if (hold_star(renderer, static_cast<const LetterVertex*>(vertices), width)) // nameplates.stars.spacing, .size
            return 0;
    }
    return g_draw_stars(renderer, edx, prim, count, vertices, stride);
}

// In place of the game's call for each letter's sprite.
bool __fastcall letter(void* font, void* edx, unsigned code, float* uv, short* size, short* x, short* y)
{
    const bool ok = g_letter(font, edx, code, uv, size, x, y);
    if (ok)
        note_prefix(code, size[0], x);
    if (ok && g_sharp.on) {
        // in g_fine units, so the text's place is as wide as the text to a quarter unit (the icons before it, which
        // the game sets beside it, don't step)
        size[0] = static_cast<short>(size[0] * g_fine);
        size[1] = static_cast<short>(size[1] * g_fine);
        *x = static_cast<short>(*x * g_fine);
        *y = static_cast<short>(*y * g_fine);
        sharp_letter(size, x, y);
    } else if (ok && g_fine > 1)
        space(code, uv, size, x, y);
    return ok;
}

bool hook_letters(HMODULE game)
{
    const std::uintptr_t site = scan::find(game, kLetterCall);
    if (site == 0) {
        log::write(L"Graphics: couldn't find where the game gets the names' letters; they keep its spacing "
                   L"([graphics] nameplates)");
        return false;
    }
    const std::uintptr_t call = site + kLetterCallAt;
    std::int32_t target = 0;
    std::memcpy(&target, reinterpret_cast<const void*>(call + 1), 4);
    // still VanaCore's (put back as it was by what rewrote it)
    if (call + 5 + target == reinterpret_cast<std::uintptr_t>(&letter))
        return true;
    g_letter = reinterpret_cast<LetterFn>(call + 5 + target);
    const std::int32_t ours = static_cast<std::int32_t>(reinterpret_cast<std::uintptr_t>(&letter) - (call + 5));
    return write_code(call + 1, &ours, 4);
}

template <typename T>
T field(const void* at, std::uintptr_t offset)
{
    T value;
    std::memcpy(&value, static_cast<const char*>(at) + offset, sizeof(value));
    return value;
}

std::uint32_t scaled(std::uint32_t size)
{
    float value;
    std::memcpy(&value, &size, sizeof(value));
    value *= g_names.size;
    std::memcpy(&size, &value, sizeof(size));
    return size;
}

// 0xRRGGBB in the game's colour scale (0x80 full)
std::uint32_t game_colour(std::uint32_t rgb)
{
    std::uint32_t out = 0;
    for (int shift = 0; shift < 24; shift += 8)
        out |= ((rgb >> shift & 0xFF) * 128 + 127) / 255 << shift;
    return out;
}

// `name` with the icons before it changed as the settings say, in g_text; nullptr if it stays as it is.
// `tint`, the pearl's colour, set when the pearl comes back in the bag's place.
const char* change_icons(const char* name, const void* entity, std::uint32_t& tint)
{
    if (g_names.bazaar && g_names.linkshell)
        return nullptr;
    std::size_t icons = 0;
    while (static_cast<unsigned char>(name[icons]) >= 0x80)
        ++icons;
    if (icons == 0 || name[icons] != ' ')
        return nullptr;
    std::size_t length = 0;
    bool changed = false;
    for (std::size_t i = 0; i < icons; ++i) {
        unsigned char icon = static_cast<unsigned char>(name[i]);
        if (icon == kBazaar && !g_names.bazaar) {
            // the bag took the pearl's place: it's back, for a player in a linkshell
            changed = true;
            if (!g_names.linkshell || (field<std::uint32_t>(entity, kEntityFlags1) & kInShell) == 0)
                continue;
            icon = kPearl;
            tint = field<std::uint32_t>(entity, kEntityShellColour);
        } else if (icon == kPearl && !g_names.linkshell) {
            changed = true;
            continue;
        }
        g_text[length++] = static_cast<char>(icon);
    }
    if (!changed)
        return nullptr;
    // the space after the icons, while any are left
    const char* rest = name + icons + (length == 0 ? 1 : 0);
    const std::size_t take = std::min(std::strlen(rest), sizeof(g_text) - 1 - length);
    std::memcpy(g_text + length, rest, take);
    g_text[length + take] = '\0';
    return g_text;
}

// In place of the game's _ftol cutting a name's depth to a whole number (kDepthCut): the depth kept first (st(0), left
// on the stack for _ftol), then the game's, which returns to the game
__declspec(naked) void depth_cut()
{
    __asm {
        fst dword ptr g_depth
        jmp dword ptr g_ftol
    }
}

// `size`, as the game passed it for a name (whole steps of its depth), worked out again from the depth before the cut,
// if that's the depth it came from (else as it was)
std::uint32_t smoothed(std::uint32_t size)
{
    if (!g_names.smooth || g_ftol == nullptr || g_far_depth == nullptr || g_near_depth == nullptr)
        return size;
    const float depth = g_depth;
    const std::uint32_t far_depth = *g_far_depth, near_depth = *g_near_depth;
    if (!(depth >= 0.0f) || !(depth < 4.0e9f) || far_depth <= near_depth)
        return size;
    // as the game works it out, from the whole number (_ftol: toward 0)
    const std::uint32_t cut = static_cast<std::uint32_t>(depth);
    if (cut > far_depth)
        return size;
    const float span = static_cast<float>(far_depth - near_depth);
    const float game = cut < near_depth ? 1.0f : static_cast<float>(far_depth - cut) / span;
    float passed;
    std::memcpy(&passed, &size, sizeof passed);
    if (std::fabs(passed - game) > 1.0e-5f)
        return size; // another depth's (or the game's own 1, for a name it always draws whole)
    const float smooth = depth < static_cast<float>(near_depth)
                             ? 1.0f
                             : std::max(0.0f, (static_cast<float>(far_depth) - depth) / span);
    std::uint32_t out;
    std::memcpy(&out, &smooth, sizeof out);
    return out;
}

// `size`, the game's for a name by its distance (1 near, toward 0 far), shrinking only nameplates.distance's share of
// what it does (0: always 1)
std::uint32_t by_distance(std::uint32_t size)
{
    if (g_names.distance >= 1.0f)
        return size;
    float value;
    std::memcpy(&value, &size, sizeof value);
    if (!(value >= 0.0f) || !(value < 1.0f))
        return size;
    value = 1.0f - g_names.distance * (1.0f - value);
    std::memcpy(&size, &value, sizeof size);
    return size;
}

// nameplates.framegen: the character a name and the line under it are drawn for (its entity's server id, else the
// actor), carried by their draws while the game draws them
struct Labelled {
    explicit Labelled(const void* actor) : on(g_names.framegen)
    {
        if (!on)
            return;
        const void* const entity = *reinterpret_cast<void* const*>(static_cast<const char*>(actor) + kActorEntity);
        const std::uint32_t id = entity != nullptr ? field<std::uint32_t>(entity, kEntityId) : 0;
        vanacore_gfx_label(id != 0 ? id : static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(actor)));
    }
    ~Labelled()
    {
        if (on)
            vanacore_gfx_label(0);
    }
    bool on;
};

// The colour the settings give a name: a trust's, a pet's, or by the game's own colour for it (kGameColour: left as it is)
std::uint32_t wanted_colour(bool player, bool trust, bool pet, std::uint32_t rgb)
{
    if (trust)
        return g_names.trusts;
    if (pet)
        return g_names.pets;
    if (player)
        return rgb == kPlayerColour ? g_names.players : kGameColour;
    return rgb == kNpcColour       ? g_names.npcs
           : rgb == kMonsterColour ? g_names.monsters
           : rgb == kClaimedColour ? g_names.claimed
           : rgb == kMineColour    ? g_names.mine
                                   : kGameColour;
}

#include "nameplates_party.h" // nameplates.hideparty, .hidealliance: the party's and alliance's members, by server id

// Whether the settings leave a name out: all of them while an addon has vana.gameui.hide('nameplates'), else by whose it is
// (nameplates.hideown, .hideparty, .hidealliance, .hideplayers, .hidetrusts, .hidepets, then by the colour the game gives
// it: .hidenpcs, .hidemonsters)
bool left_out(bool player, bool own, bool trust, bool pet, std::uint32_t rgb, std::uint32_t id)
{
    if (g_hide_all)
        return true;
    if (own)
        return g_names.hide_own;
    if (member_left_out(id))
        return true;
    if (player)
        return g_names.hide_players;
    if (trust)
        return g_names.hide_trusts;
    if (pet)
        return g_names.hide_pets;
    if (rgb == kNpcColour)
        return g_names.hide_npcs;
    if (rgb == kMonsterColour || rgb == kClaimedColour || rgb == kMineColour)
        return g_names.hide_monsters;
    return false;
}

// While a name is laid out and left undrawn (hidden, with widgets at it), and the widgets at a name once it's drawn
struct Quiet {
    explicit Quiet(bool on) { g_swallow = on; }
    ~Quiet() { g_swallow = false; }
};
struct Attached {
    explicit Attached(unsigned index) : index(index) {}
    ~Attached()
    {
        if (index >= kAnchors || !widgets::attached(index))
            return;
        const Anchor& anchor = g_anchors[index];
        if (anchor.serial != g_name_serial)
            return;
        widgets::draw_at_name(index, Slot{(anchor.left + anchor.right) * 0.5f, anchor.left, anchor.right, anchor.top,
                                          anchor.bottom, anchor.z, anchor.rhw, g_plate_size});
    }
    unsigned index;
};

// In place of the game's call drawing a character's name: its icons, size and colour as set; left out as left_out says.
void __fastcall draw_name(void* actor, void* edx, std::uint32_t at, std::uint32_t name, std::uint32_t size,
                          std::uint32_t colour, std::uint32_t tint)
{
    g_hidden = nullptr;
    g_last_character = false;
    unsigned target = ~0u; // the character's target index
    bool swallow = false;  // its name is hidden, but widgets sit at it: laid out for its place, drawn by nothing
    const void* const entity = *reinterpret_cast<void* const*>(static_cast<const char*>(actor) + kActorEntity);
    if (entity == nullptr && g_hide_all) {
        g_hidden = actor;
        return;
    }
    if (entity != nullptr) {
        const std::uint32_t id = field<std::uint32_t>(entity, kEntityId);
        const bool player = (id & 0xFF000000) == 0;
        g_last_character = player;
        const bool own = g_player != nullptr && entity == g_player();
        // a trust's colour, or a plain name's (the game's alpha kept: it fades names out)
        const std::uint32_t rgb = colour & 0xFFFFFF;
        const std::uint32_t index = field<std::uint32_t>(entity, kEntityIndex) & 0xFFF;
        target = index;
        const bool trust = !player && (g_names.trusts != kGameColour || g_names.hide_trusts) &&
                           trusts::is_trust(static_cast<std::uint16_t>(index));
        const bool pet = !player && !trust && (g_names.pets != kGameColour || g_names.hide_pets) && g_pets[index];
        if (left_out(player, own, trust, pet, rgb, id)) {
            g_hidden = actor;
            swallow = g_sharp_ready && widgets::attached(index);
            if (!swallow)
                return;
        }
        if (player) {
            if (const char* changed = change_icons(reinterpret_cast<const char*>(name), entity, tint))
                name = reinterpret_cast<std::uint32_t>(changed);
        }
        const std::uint32_t wanted = wanted_colour(player, trust, pet, rgb);
        if (wanted != kGameColour)
            colour = (colour & 0xFF000000) | game_colour(wanted);
    }
    // where its name is drawn, for name_anchor
    struct Anchoring {
        explicit Anchoring(unsigned index)
        {
            g_name_index = index;
            ++g_name_serial;
        }
        ~Anchoring() { g_name_index = ~0u; }
    } anchoring(target);
    const Labelled label(actor);
    const Quiet quiet(swallow);
    const Attached attached(target);
    const gmicons::Name gm(g_last_character && g_names.gm ? reinterpret_cast<const char*>(name) : nullptr, entity);
    const soulspire::echo_icons::NameScope echo_icon(g_last_character ? entity : nullptr);
    // nameplates.smooth and .distance: the line under it at the same size
    g_size_from = size;
    size = g_size_to = by_distance(smoothed(size));
    const std::uint32_t drawn = g_names.size != 1.0f ? scaled(size) : size;
    // nameplates.plate, .name, .pearl, .center: its quads moved at this size, while the game draws it
    g_text_seen = false;
    g_in_line = false;
    std::memcpy(&g_plate_size, &drawn, sizeof g_plate_size);
    struct Active {
        explicit Active(bool on) { g_plate_active = on; }
        ~Active() { g_plate_active = false; }
    } active(!g_names.characters_only || g_last_character);
    // nameplates.font=sharp: the letters after the icons (and the space after them) as VanaCore's text, when they're
    // all plain letters
    if (g_names.sharp && g_sharp_ready && g_hooked && g_site != 0) {
        const char* const text = reinterpret_cast<const char*>(name);
        std::size_t start = 0;
        while (static_cast<unsigned char>(text[start]) >= 0x80)
            ++start;
        if (start > 0 && text[start] == ' ')
            ++start;
        bool plain = text[start] != '\0';
        for (const char* c = text + start; plain && *c != '\0'; ++c)
            plain = *c >= 0x20 && *c < 0x7F;
        if (plain) {
            g_sharp = Sharp{};
            g_sharp.on = true;
            g_sharp.actor = actor;
            g_sharp.text = text + start;
            g_sharp.box = static_cast<std::ptrdiff_t>(start);
            std::memcpy(&g_sharp.size, &drawn, sizeof drawn);
            g_draw_name(actor, edx, at, name, drawn, colour, tint);
            g_sharp.on = false;
            return;
        }
    }
    g_draw_name(actor, edx, at, name, drawn, colour, tint);
}

// In place of the game's call drawing the line under a name: a job master's stars.
void __fastcall draw_line(void* actor, void* edx, std::uint32_t at, std::uint32_t text, std::uint32_t size)
{
    if (actor == g_hidden || !g_names.mastery)
        return;
    const Labelled label(actor);
    if (size == g_size_from)
        size = g_size_to;
    const std::uint32_t drawn = g_names.size != 1.0f ? scaled(size) : size;
    // nameplates.plate and .stars: its quads moved at its size
    std::memcpy(&g_plate_size, &drawn, sizeof g_plate_size);
    // nameplates.stars.style: which glyph each quad of this line is (the stars' codes of its text, in order)
    g_star_count = 0;
    g_star_next = 0;
    g_star_big = false;
    g_small_star_height = 0.0f;
    if (g_names.star_style != StarStyle::game && text != 0) {
        const unsigned char* const bytes = reinterpret_cast<const unsigned char*>(static_cast<std::uintptr_t>(text));
        // the text holds one 0xD2 for the three, which the game makes into 0xD3, 0xD4, 0xD5 as it copies it
        for (int i = 0; i < 36 && bytes[i] != 0 && g_star_count + 3 <= kMostStars; ++i) {
            if (bytes[i] == kStarAll) {
                for (unsigned char c = kStarFirst; c <= kStarLast; ++c)
                    g_star_codes[g_star_count++] = c;
                g_star_big = true;
            }
        }
    }
    g_in_line = true;
    g_plate_active = !g_names.characters_only || g_last_character;
    g_draw_line(actor, edx, at, text, drawn);
    draw_held_stars(g_draw_stars); // (nameplates.stars.spacing)
    g_plate_active = false;
    g_in_line = false;
}

bool redirect(std::uintptr_t call, const void* ours, void*& theirs)
{
    std::int32_t target = 0;
    std::memcpy(&target, reinterpret_cast<const void*>(call + 1), 4);
    theirs = reinterpret_cast<void*>(call + 5 + target);
    const std::int32_t to = static_cast<std::int32_t>(reinterpret_cast<std::uintptr_t>(ours) - (call + 5));
    return write_code(call + 1, &to, 4);
}

// nameplates.font=sharp's calls, in the routine kFactors is in (at g_site): each letter's texture and colour, and
// its quad's draw. False (sharp names left to the game) if one isn't there.
bool install_sharp(HMODULE game)
{
    const std::uintptr_t corner_site = scan::find(g_site, 0x800, kCornerCall);
    const std::uintptr_t material_site = corner_site != 0 ? scan::find(corner_site, kSharpWithin, kMaterialCall) : 0;
    const std::uintptr_t renderer_site = material_site != 0 ? scan::find(material_site, kSharpWithin, kRenderer) : 0;
    const std::uintptr_t draw_site = renderer_site != 0 ? scan::find(renderer_site, kSharpWithin, kDrawCall) : 0;
    if (draw_site == 0) {
        log::write(L"Graphics: couldn't find where the game draws each letter of the names over characters' heads; "
                   L"they keep its letters ([graphics] nameplates.font=sharp)");
        return false;
    }
    const auto target = [](std::uintptr_t call) {
        std::int32_t rel = 0;
        std::memcpy(&rel, reinterpret_cast<const void*>(call + 1), 4);
        return call + 5 + rel;
    };
    g_game_width = reinterpret_cast<SizeFn>(target(g_site + kWidthCall));
    g_game_height = reinterpret_cast<SizeFn>(target(g_site + kHeightCall));
    g_renderer_slot = *reinterpret_cast<void* const* const*>(renderer_site + 2);
    void* theirs = nullptr;
    // the draw's call first: VanaCore's draws the game's letters as they are till the material's call is its too
    if (!redirect(draw_site + kDrawCallAt, reinterpret_cast<const void*>(&draw_letter), theirs))
        return false;
    g_draw = reinterpret_cast<DrawFn>(theirs);
    if (!redirect(material_site + kMaterialCallAt, reinterpret_cast<const void*>(&material), theirs))
        return false;
    g_material = reinterpret_cast<MaterialFn>(theirs);
    // the line under the name has its own routine and draw call, the next of that pattern after the name's
    const std::uintptr_t stars_site = scan::find(draw_site + 1, kStarsWithin, kDrawCall);
    if (stars_site != 0 && redirect(stars_site + kDrawCallAt, reinterpret_cast<const void*>(&draw_stars), theirs))
        g_draw_stars = reinterpret_cast<DrawFn>(theirs);
    else
        log::write(L"Graphics: couldn't find where the game draws the stars under a job master's name; they stay "
                   L"where it puts them ([graphics] nameplates.stars.x)");
    log::write(L"Graphics: each letter of the names over characters' heads is drawn at FFXiMain.dll+0x%X ([graphics] "
               L"nameplates.font=sharp)",
               static_cast<unsigned>(draw_site + kDrawCallAt - reinterpret_cast<std::uintptr_t>(game)));
    return true;
}

// nameplates.order: the actors of the game's walk (kActorWalk), as they're found and as they're drawn
bool g_walk_tried = false;
const void* const* g_actor_list = nullptr; // the list's first actor
const float* g_actor_limit = nullptr;      // what an actor's [+8Ch] must reach to be drawn
std::uintptr_t g_name_site = 0;            // kNameCall's call, inside the routine that draws a character's name
std::uintptr_t g_walk_after = 0;           // where the game goes on after its walk

struct Walked {
    void* actor;
    float distance; // squared yalms; -1 for an actor that draws no name (kept in the game's order, drawn last)
    std::size_t order;
};

// The game's walk, but far to near. Each actor's own draw is what the game calls: its virtual at +284h, no arguments.
void __cdecl walk_actors()
{
    if (widgets::any_world()) // vana.world.place: before the names, in the same pass (the names go over them)
        widgets::draw_world();
    using ActorFn = void(__fastcall*)(void* actor, void* vtable);
    const float limit = *g_actor_limit;
    Walked walked[kMostActors];
    std::size_t count = 0;
    bool sorted = false;
    // the actor's draw, by what it has (nothing sorted when the setting is off)
    const auto draw = [](void* actor) {
        void* const* const vtable = field<void* const*>(actor, 0);
        reinterpret_cast<ActorFn>(vtable[kActorVtableSlot])(actor, const_cast<void**>(vtable));
    };
    for (void* actor = const_cast<void*>(*g_actor_list); actor != nullptr; actor = field<void*>(actor, kActorNext)) {
        if (!(field<float>(actor, kActorLimit) >= limit)) // not drawn (the game's compare: less than, or not a number)
            continue;
        if (!g_names.order || count == kMostActors) {
            draw(actor);
            continue;
        }
        float distance = -1.0f;
        void* const* const vtable = field<void* const*>(actor, 0);
        const std::uintptr_t routine = reinterpret_cast<std::uintptr_t>(vtable[kActorVtableSlot]);
        if (g_name_site >= routine && g_name_site - routine < kNameRoutineSpan) {
            if (const void* const entity = field<void*>(actor, kActorEntity)) {
                const float squared = field<float>(entity, kEntityDistance);
                if (squared >= 0.0f)
                    distance = squared;
            }
        }
        sorted = sorted || (count > 0 && distance > walked[count - 1].distance);
        walked[count] = {actor, distance, count};
        ++count;
    }
    // far first (the nearest, and anything without a name, over the rest); ties as the game had them
    if (sorted)
        std::sort(walked, walked + count, [](const Walked& a, const Walked& b) {
            return a.distance != b.distance ? a.distance > b.distance : a.order < b.order;
        });
    for (std::size_t i = 0; i < count; ++i)
        draw(walked[i].actor);
}

// In place of the game's walk of its actors: ours, then the game on where its walk ended.
__declspec(naked) void walk_stub()
{
    __asm {
        pushad
        call walk_actors
        popad
        jmp dword ptr g_walk_after
    }
}

// nameplates.blink=0: the target's name doesn't pulse. The game's name colour (FFXiMain.dll+0x82F97 in CatsEyeXI's client,
// in the routine that sizes a name for its distance) tests the actor's [+88h] & 4 (it's the target) and jumps past the pulse
// (its alpha times sin(16 degrees a frame) * 32 + 96, found live 2026-11-07) when it's clear: that jump made unconditional.
constexpr char kBlink[] = "F6 87 88 00 00 00 04 74 ?? 33 C9 8A 4E 03";
constexpr std::uintptr_t kBlinkJump = 7; // the je's opcode
std::uintptr_t g_blink_site = 0;
bool g_blink_tried = false;

void set_blink(HMODULE game, bool blink)
{
    if (!blink && !g_blink_tried && game != nullptr) {
        g_blink_tried = true;
        g_blink_site = scan::find(game, kBlink);
        if (g_blink_site == 0)
            log::write(L"Graphics: couldn't find where the game makes the target's name blink ([graphics] nameplates.blink)");
    }
    if (g_blink_site == 0)
        return;
    const std::uint8_t opcode = blink ? 0x74 : 0xEB; // je (the game's) or jmp
    if (*reinterpret_cast<const std::uint8_t*>(g_blink_site + kBlinkJump) != opcode &&
        write_code(g_blink_site + kBlinkJump, &opcode, 1))
        log::write(L"Graphics: the target's name %ls ([graphics] nameplates.blink)", blink ? L"blinks, as the game has it" : L"stays steady");
}

void hook_walk(HMODULE game)
{
    const std::uintptr_t walk = scan::find(game, kActorWalk);
    const std::uintptr_t name = scan::find(game, kNameCall);
    if (walk == 0 || name == 0) {
        log::write(L"Graphics: couldn't find where the game walks its characters to draw their names; they keep its "
                   L"order, nearer names may be under farther ones ([graphics] nameplates.order)");
        return;
    }
    g_actor_list = *reinterpret_cast<const void* const* const*>(walk + kWalkList);
    g_actor_limit = *reinterpret_cast<const float* const*>(walk + kWalkLimit);
    g_name_site = name + kNameCallAt;
    g_walk_after = walk + kWalkAfter;
    // jmp (our stub), over the first instruction of the walk (6 bytes); what's after it isn't reached
    std::uint8_t jump[6] = {0xE9, 0, 0, 0, 0, 0x90};
    const std::int32_t to = static_cast<std::int32_t>(reinterpret_cast<std::uintptr_t>(&walk_stub) - (walk + 5));
    std::memcpy(jump + 1, &to, sizeof to);
    if (!write_code(walk, jump, sizeof jump)) {
        log::write(L"Graphics: couldn't change how the game walks its characters to draw their names ([graphics] "
                   L"nameplates.order)");
        return;
    }
    log::write(L"Graphics: names over characters' heads are drawn far to near, the walk at FFXiMain.dll+0x%X "
               L"([graphics] nameplates.order)",
               static_cast<unsigned>(walk - reinterpret_cast<std::uintptr_t>(game)));
}

// Which entities are players' pets, now and then.
void find_pets()
{
    const ULONGLONG now = GetTickCount64();
    if (now - g_pets_at < kPetsEvery)
        return;
    g_pets_at = now;
    std::memset(g_pets, 0, sizeof(g_pets));
    const unsigned to = std::min(kPlayersTo, game::entity_count());
    game::Entity player;
    for (unsigned i = kPlayersFrom; i < to; ++i)
        if (game::entity(i, player) && player.pet_index != 0)
            g_pets[player.pet_index & 0xFFF] = true;
}

// Whether any of the names' parts is moved, or the letters centred without their icon, or an addon has widgets at them, or the
// GM icons are CatsEyeXI's: the letters' draw is hooked
bool moves(const Names& names)
{
    return widgets::any() || names.centre || names.gm || names.pearl_x != 0.0f || names.pearl_y != 0.0f ||
           names.pearl_size != 1.0f || names.bazaar_x != 0.0f || names.bazaar_y != 0.0f || names.bazaar_size != 1.0f ||
           names.plate_x != 0.0f || names.plate_y != 0.0f || names.name_x != 0.0f || names.name_y != 0.0f || names.stars_x != 0.0f ||
           names.stars_y != 0.0f || names.stars_spacing != 1.0f || names.stars_size != 1.0f ||
           names.star_style != StarStyle::game;
}

// Whether anything about the names needs the game's call drawing them hooked
bool wants_name_call(const Names& names, bool shaped)
{
    return names.hides() || g_hide_all || !names.bazaar || !names.linkshell || !names.mastery ||
           names.size != 1.0f || names.players != kGameColour || names.npcs != kGameColour ||
           names.monsters != kGameColour || names.claimed != kGameColour || names.mine != kGameColour ||
           names.trusts != kGameColour || names.pets != kGameColour || names.sharp || moves(names) || shaped ||
           names.smooth || names.distance != 1.0f || names.framegen;
}

// The names' settings in words, said in the log when they change
void log_state(const Names& names)
{
    const auto word = [](bool hidden) { return hidden ? L"hidden" : L"shown"; };
    wchar_t state[400];
    swprintf_s(state, L"%ls; yours %ls, your party's %ls, the alliance's %ls, other players' %ls, NPCs' %ls, monsters' %ls, "
                      L"trusts' %ls, pets' %ls; bazaar bags %ls, linkshell pearls %ls, mastery stars %ls; %.2fx the size, "
                      L"%.0f%% of the game's shrinking with distance",
               g_hide_all ? L"all hidden by an addon" : L"as set", word(names.hide_own), word(names.hide_party),
               word(names.hide_alliance), word(names.hide_players), word(names.hide_npcs), word(names.hide_monsters),
               word(names.hide_trusts), word(names.hide_pets),
               word(!names.bazaar), word(!names.linkshell), word(!names.mastery && g_draw_line != nullptr), names.size,
               names.distance * 100.0f);
    static std::wstring last;
    if (last != state) {
        last = state;
        log::write(L"Graphics: names over characters' heads: %ls ([graphics] nameplates)", state);
    }
}

// Hooked the first time a setting needs it; with the game's settings, the calls go on as they were.
void set_names(HMODULE game, const Names& names, bool shaped)
{
    const bool wanted = wants_name_call(names, shaped);
    if ((names.order || widgets::any_world()) && !g_walk_tried && game != nullptr) {
        g_walk_tried = true;
        hook_walk(game);
    }
    g_names.order = names.order; // the walk's, hooked without the name call
    if (wanted && !g_names_searched && game != nullptr) {
        g_names_searched = true;
        const std::uintptr_t site = scan::find(game, kNameCall);
        if (site == 0) {
            log::write(L"Graphics: couldn't find where the game draws each character's name; names stay as it "
                       L"draws them ([graphics] nameplates)");
            return;
        }
        const std::uintptr_t call = site + kNameCallAt;
        void* theirs = nullptr;
        if (!redirect(call, reinterpret_cast<const void*>(&draw_name), theirs)) {
            log::write(L"Graphics: couldn't change how the game draws each character's name ([graphics] nameplates)");
            return;
        }
        g_draw_name = reinterpret_cast<NameFn>(theirs);
        if (const std::uintptr_t player = scan::find(game, kPlayerEntity); player != 0)
            g_player = reinterpret_cast<PlayerFn>(player);
        else
            log::write(L"Graphics: couldn't find how the game finds your own character; your name stays "
                       L"([graphics] nameplates.hideown)");
        // the line under the name, just after
        const std::uintptr_t line = scan::find(call + 5, kLineWithin, kLineCall);
        if (line != 0 && redirect(line + kLineCallAt, reinterpret_cast<const void*>(&draw_line), theirs))
            g_draw_line = reinterpret_cast<LineFn>(theirs);
        else
            log::write(L"Graphics: couldn't find where the game draws the stars under a job master's name; they "
                       L"stay ([graphics] nameplates.mastery)");
        log::write(L"Graphics: each character's name is drawn at FFXiMain.dll+0x%X, the line under it %ls "
                   L"([graphics] nameplates)",
                   static_cast<unsigned>(call - reinterpret_cast<std::uintptr_t>(game)),
                   g_draw_line != nullptr ? L"just after" : L"as the game draws it");
    }
    if (g_draw_name == nullptr)
        return;
    if (names.smooth && !g_depth_tried && game != nullptr) {
        g_depth_tried = true;
        const std::uintptr_t cut = scan::find(game, kDepthCut);
        const std::uintptr_t range = scan::find(game, kDepthRange);
        void* theirs = nullptr;
        if (cut != 0 && range != 0 && redirect(cut + kDepthCutAt, reinterpret_cast<const void*>(&depth_cut), theirs)) {
            g_far_depth = *reinterpret_cast<const std::uint32_t* const*>(range + kFarAt);
            g_near_depth = *reinterpret_cast<const std::uint32_t* const*>(range + kNearAt);
            g_ftol = theirs;
            log::write(L"Graphics: names are sized smoothly by their depth, cut at FFXiMain.dll+0x%X (%u to %u) "
                       L"([graphics] nameplates.smooth)",
                       static_cast<unsigned>(cut + kDepthCutAt - reinterpret_cast<std::uintptr_t>(game)), *g_near_depth,
                       *g_far_depth);
        } else {
            log::write(L"Graphics: couldn't find where the game sizes names by their distance; they grow and shrink "
                       L"in its steps ([graphics] nameplates.smooth)");
        }
    }
    if (names.pets != kGameColour || names.hide_pets)
        find_pets();
    find_members(names);
    log_state(names);
    g_names = names;
}

// Each frame: the size the scene is shown at (the window's with the game's own Direct3D 8), for the sharp names and the
// icons' shape
void update_shown()
{
    std::uint32_t w = 0, h = 0;
    if (!vanacore_gfx_scene_shown(&w, &h)) {
        RECT client{};
        if (window::handle() != nullptr && GetClientRect(window::handle(), &client))
            w = static_cast<std::uint32_t>(client.right), h = static_cast<std::uint32_t>(client.bottom);
    }
    g_shown_w = static_cast<float>(w), g_shown_h = static_cast<float>(h);
    float sx = 0.0f, sy = 0.0f;
    g_ui_scale = vanacore_gfx_chat_scale(&sx, &sy) ? sy : 0.0f;
}

// Each frame for nameplates.font=sharp: the pictures let go when they aren't drawn any more, or are in another font now
void sharp_frame(bool sharp, const Names& names)
{
    g_made_this_frame = 0;
    debug::trace_frame(sharp ? names.trace : 0.0f);
    if (!sharp) {
        if (!g_sprites.empty())
            release_sprites();
        return;
    }
    wchar_t look[64];
    swprintf_s(look, L"|%d%d%d|%g", names.bold ? 1 : 0, names.italic ? 1 : 0, names.upper ? 1 : 0, names.text_spacing);
    std::wstring style = names.family + look;
    if (style != g_sprite_style) {
        retire_sprites();
        g_sprite_style = std::move(style);
    }
    sweep_sprites();
}

} // namespace

void apply(bool fix_wanted, bool spaced, float gap, float shape, const Names& names)
{
    g_hide_all = gameui::hidden("nameplates");
    // sharp letters are VanaCore's text: the game's letters' spacing doesn't come into it. Its shape does, for what the
    // game still draws: the pearl and the bag keep the one shape in any window (and with the game's letters), and the
    // space after them the one width
    const bool sharp = names.sharp;
    const bool fix = fix_wanted;
    const bool spacing = spaced && !sharp;
    // the parts are moved at the letters' draw, which is found the way sharp's is
    const bool shaped = (fix || spacing || sharp) && names.pearl_shape > 0.0f;
    const bool pearl = moves(names) || shaped;
    // Ashita's Nameplate plugin rewrites the same name-drawing code. Held off while VanaCore draws the names (True
    // Font, or the shape/spacing fix), so the two don't fight and VanaCore can find and patch that code; it loads
    // again when Letters is Game's and the spacing fix is off. (Nameplate also hid your own name with NameMode=2;
    // use Hide own nameplate for that.)
    if (fix || spacing || sharp || pearl)
        plugins::hold("nameplate", "VanaCore is drawing the names over characters' heads (/vana > Game Options > "
                                   "Nameplates); it loads again when Letters is Game's and the spacing fix is off");
    else
        plugins::release("nameplate");
    const HMODULE game = GetModuleHandleW(L"FFXiMain.dll");
    set_names(game, names, shaped);
    set_blink(game, names.blink);
    // with nameplates=stretched and the game's letters the icons are the game's, in the window's shape
    g_names.pearl_shape = shaped ? names.pearl_shape : 0.0f;
    update_shown();
    sharp_frame(sharp, names);
    // Looked for the first time it's wanted. Not there when something else has rewritten it (Ashita's
    // Nameplate plugin): left alone, and looked for again after a plugin loads or unloads.
    if (g_site == 0) {
        if ((!fix && !spacing && !sharp && !pearl) || game == nullptr || (g_searched && plugins::changes() == g_searched_at))
            return;
        const bool again = g_searched;
        g_searched = true;
        g_searched_at = plugins::changes();
        const std::uintptr_t site = scan::find(game, kFactors);
        const float* const across = site != 0 ? *reinterpret_cast<const float* const*>(site + kAcross) : nullptr;
        const float* const down = site != 0 ? *reinterpret_cast<const float* const*>(site + kDown) : nullptr;
        // the game's (one constant for both), or VanaCore's, put back by what had rewritten them
        const bool ours = g_game != nullptr && across == &g_across && down == &g_down;
        if (site == 0 || (across != down && !ours)) {
            if (!again)
                log::write(L"Graphics: couldn't find where the game sizes the names over characters' heads; they "
                           L"keep its shape and spacing while something else has it ([graphics] nameplates)");
            return;
        }
        g_site = site;
        if (!ours)
            g_game = across;
        g_hook_tried = false;
        log::write(L"Graphics: the game sizes the names over characters' heads at FFXiMain.dll+0x%X%ls "
                   L"([graphics] nameplates)",
                   static_cast<unsigned>(site - reinterpret_cast<std::uintptr_t>(game)),
                   again ? L", nothing else has it now" : L"");
    }
    const float* const across = operand(kAcross);
    const float* const down = operand(kDown);
    if ((across != g_game && across != &g_across) || (down != g_game && down != &g_down)) {
        log::write(L"Graphics: something else changed how the game sizes the names over characters' heads; "
                   L"[graphics] nameplates leaves them to it while it has them");
        g_site = 0;
        g_searched_at = plugins::changes();
        g_fine = 1;
        g_state.clear();
        return;
    }
    if (spacing && !g_sheet_tried) {
        g_sheet_tried = true;
        const std::wstring folder = dats::ffxi_folder();
        const std::wstring file = folder.empty() ? std::wstring() : pivot::resolve(folder + kSheetDat);
        if (!file.empty() && load_sheet(file, g_sheet))
            log::write(L"Graphics: the names' letters measured from %ls (%dx%d)", file.c_str(), g_sheet.width,
                       g_sheet.height);
        else
            log::write(L"Graphics: couldn't read the names' font from %ls%ls; they keep the game's spacing "
                       L"([graphics] nameplates)",
                       folder.c_str(), kSheetDat);
    }
    // (the letters' call is also how the icons' width is known, for centring the letters without them)
    if (((spacing && !g_sheet.alpha.empty()) || sharp || names.centre) && !g_hook_tried && game != nullptr) {
        g_hook_tried = true;
        g_hooked = hook_letters(game);
    }
    if ((sharp || pearl) && !g_sharp_tried && game != nullptr) {
        g_sharp_tried = true;
        g_sharp_ready = install_sharp(game);
    }
    const int fine = (spacing || sharp) && g_hooked ? kFine : 1;
    g_gap = spacing ? gap : 0.0f;
    if (shape > 0)
        g_shape = shape;
    g_fix = fix && g_shape > 0 ? 4.0f / 3.0f / g_shape : 1.0f;
    g_across = *g_game * g_fix / fine;
    g_down = *g_game / fine;
    const float* const want_across = fix || fine > 1 ? &g_across : g_game;
    const float* const want_down = fine > 1 ? &g_down : g_game;
    bool ok = true;
    if (across != want_across)
        ok &= write_code(g_site + kAcross, &want_across, sizeof want_across);
    if (down != want_down)
        ok &= write_code(g_site + kDown, &want_down, sizeof want_down);
    g_fine = ok ? fine : 1;
    if (!ok)
        g_fix = 1.0f;

    wchar_t state[320];
    if (sharp)
        swprintf_s(state, L"letters as VanaCore's text, %ls %g px%ls%ls%ls, %+g spacing; icons %ls", names.family.c_str(),
                   names.font_size, names.bold ? L" bold" : L"", names.italic ? L" italic" : L"", names.upper ? L" capitals" : L"",
                   names.text_spacing,
                   fix ? L"their shape kept in any window" : L"widening with the window as the game draws them");
    else
        swprintf_s(state, L"%ls; %ls", fix ? L"their shape kept in any window" : L"widening with the window as the game draws them",
                   fine > 1 ? L"letters spaced by their own widths" : L"letters a cell apart, as the game spaces them");
    if (!ok)
        log::write(L"Graphics: couldn't change how the game sizes the names over characters' heads ([graphics] "
                   L"nameplates)");
    else if (g_state != state) {
        g_state = state;
        if (spacing && fine > 1)
            log::write(L"Graphics: names over characters' heads: %ls, %g of the font's pixels between them "
                       L"([graphics] nameplates)",
                       state, gap);
        else
            log::write(L"Graphics: names over characters' heads: %ls ([graphics] nameplates)",
                       state);
    }
}

bool name_anchor(unsigned index, float& x, float& top, float& bottom)
{
    if (index >= kAnchors)
        return false;
    const Anchor& anchor = g_anchors[index];
    if (anchor.at == 0 || GetTickCount64() - anchor.at > kAnchorLife)
        return false;
    x = anchor.x, top = anchor.top, bottom = anchor.bottom;
    return true;
}

bool anchoring()
{
    return g_anchor_seen != 0 && GetTickCount64() - g_anchor_seen <= 2000;
}

bool pass(Pass& out)
{
    const char* const renderer = g_renderer_slot != nullptr ? static_cast<const char*>(*g_renderer_slot) : nullptr;
    if (renderer == nullptr || g_game_width == nullptr || g_game_height == nullptr || !(g_shown_w > 0.0f) || !(g_shown_h > 0.0f))
        return false;
    out.device = *reinterpret_cast<void* const*>(renderer + kRendererDevice);
    out.game_w = static_cast<float>(g_game_width() & 0xFFFF);
    out.game_h = static_cast<float>(g_game_height() & 0xFFFF);
    out.kx = g_shown_w / out.game_w, out.ky = g_shown_h / out.game_h;
    return out.device != nullptr && out.game_w > 0.0f && out.game_h > 0.0f;
}

} // namespace vanacore::nameplates
