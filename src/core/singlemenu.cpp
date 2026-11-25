#include "singlemenu.hpp"

#include <windows.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <utility>
#include <vector>

#include "log.hpp"
#include "scan.hpp"

namespace vanacore::singlemenu {
namespace {

// The game's menus, layouts, sprite sheets and the manager's lists are configmenu.cpp's (read its notes); the same
// functions are found here by the same patterns, so the two settings stand apart.
//
// The main menu (FFXiMain 2025-12-26, PS2 TkMenuWind.cpp / TkSocial.cpp): two menus of the game's table of menus, each a
// handler object (a 17-slot table: 1 opened, 6 input, __thiscall (handler, short event, short item), ret 8; +8 its
// window) and a layout of ROM\119\51.DAT:
//   "menu    menuwind" (CTkMenuWind, made at login by its constructor 0x1013F720): items 1 Status, 2 Equipment, 3 Magic,
//     4 Items, 10 Synthesis, 5 Abilities, 6 Party, 7 Trade, 8 Search, 9 Linkshell, 11 Region Info, 12 Map, top to bottom;
//     13 and 14 the page arrows beside the frame, 15 the title tab. Its own code (opened, the per-frame check) refers to
//     items 2, 5, 6, 10, 12 and 15 by id, so those stay.
//   "menu    socialme" (CTkSocial): 1 Missions, 2 Quests, 3 Key Items, 4 View House, 5 Set Bazaar, 6 MacroPalette, 7 Config,
//     8 Help Desk, 10 Current Time, 9 Communication, 11 Shut Down, 12 Log Out; 13 to 15 as menuwind's.
// Each input switches on the event (3 Right, 4 Left, 7 and 0x1C the menu key ("-") pressed again: the flip to the other page; 5 select; 6
// cancel) and on select on the item (1 to 12 open a menu by name, run a command or call the game; 13 to 15 re-enter as a
// Left, a flip). socialme's select on 1 to 12 never touches its handler object, so it runs from menuwind's as it does
// from its own, its window open or not.
//
// The one list: menuwind's layout with socialme's items 1 to 12 after its own blocks 1 to 15 as ids 16 to 27, 192 lower
// (twelve rows of 16), the arrows and the title tab kept as 13 to 15 (the game takes an item by its block's place, so
// the ids must run 1 to 27 in block order), the arrows out of sight (there's no page to flip to: each is VanaCore's
// blank sprite, and its block is far above the window so the mouse can't land on it), every item linked up and down in
// order round the list (Right and Left to itself, as the game's), and the window 394 tall in a frame of VanaCore's own
// (the game's frames, "menu    comwin" sprite k with k + 1 wells, go to 15). menuwind's handler's slot 6: Left, Right
// and the menu key don't flip (the menu key closes the list), select on 16 to 27 is socialme's input with the item less
// 15. Slot 1: socialme's rule, Set Bazaar can't be chosen in Monstrosity.

// The window that has the help line's text (FUN_101182a0): the menu manager at +7
constexpr char kHelp[] = "53 55 56 8B E9 57 B9 ?? ?? ?? ?? E8 ?? ?? ?? ?? 3B C5 0F 85 ?? ?? ?? ?? 66 8B 45 4C 8B CD 50 E8";
constexpr std::ptrdiff_t kHelpManagerAt = 7;
// A layout's payload parsed and put on the manager's list (FUN_1015ccb0), and the first of a payload's name taken off and
// freed (FUN_1015cd00), the layout of a name it goes by (FUN_1015cd60): configmenu.cpp's
constexpr char kLayoutAdd[] = "56 57 6A 68 8B F9 E8 ?? ?? ?? ?? 83 C4 04 85 C0 74 22 8B C8 E8 ?? ?? ?? ?? 8B F0 85 F6 74 15 8B 44 24 0C "
                              "8B CE 50 E8 ?? ?? ?? ?? 56 8D 4F 1C E8 ?? ?? ?? ?? 5F 5E C2 04 00";
constexpr char kLayoutRemove[] = "8B 44 24 04 56 57 8B F9 8B 08 51 8B CF E8 ?? ?? ?? ?? 8B F0 85 F6 74 1D 56 8D 4F 1C E8 ?? ?? ?? ?? "
                                 "85 C0 74 10 8B CE E8 ?? ?? ?? ?? 56 E8 ?? ?? ?? ?? 83 C4 04 5F 5E C2 04 00";
constexpr std::ptrdiff_t kRemoveFindCall = 0xD;
constexpr std::uint8_t kFindStart[] = {0x51, 0x53, 0x56, 0x57, 0x8D, 0x79, 0x1C, 0x8B, 0xCF, 0xE8};
constexpr std::ptrdiff_t kFindNameAt = 0x2C;
constexpr std::uint8_t kFindName[] = {0x8D, 0x4E, 0x46, 0x6A, 0x10, 0x51, 0x53, 0xE8};
// A sprite sheet's payload put on the sheets' list (FUN_1011fe90) and every sheet of a name taken off (FUN_1011fed0), and
// the DAT's sheet chunks' two calls side by side (the sheets' list): configmenu.cpp's
constexpr char kSheetAdd[] = "56 57 6A 40 8B F9 E8 ?? ?? ?? ?? 83 C4 04 85 C0 74 21 8B C8 E8 ?? ?? ?? ?? 8B F0 85 F6 74 14 8B 44 24 0C "
                             "8B CE 50 E8 ?? ?? ?? ?? 56 8B CF E8 ?? ?? ?? ?? 5F 5E C2 04 00";
constexpr char kSheetRemove[] = "83 EC 0C 8B 44 24 10 56 57 8B F9 8B 08 89 7C 24 10 51 8B CF E8 ?? ?? ?? ?? 8B F0 85 F6 89 74 24 08 0F 84";
constexpr std::ptrdiff_t kSheetRemoveLayoutsAt = 0x40;
constexpr char kSheetThunks[] = "51 83 C1 30 8D 44 24 00 89 4C 24 00 50 B9 ?? ?? ?? ?? E8 ?? ?? ?? ?? 59 C3 90 90 90 90 90 90 90 "
                                "51 83 C1 30 8D 44 24 00 89 4C 24 00 50 B9 ?? ?? ?? ?? E8 ?? ?? ?? ?? 59 C3 90 90 90 90 90 90 90 "
                                "56 57 8B F1 E8";
constexpr std::ptrdiff_t kThunkSheetsAt = 14, kThunkAddCall = 18, kThunkNext = 32;

// The game's table of menus: the name, 16 zero bytes, the global holding its handler (+0x20)
constexpr char kMenuEntry[] = "6D 65 6E 75 20 20 20 20 6D 65 6E 75 77 69 6E 64 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00";
constexpr char kSocialEntry[] = "6D 65 6E 75 20 20 20 20 73 6F 63 69 61 6C 6D 65 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00";
constexpr std::ptrdiff_t kEntryGlobal = 0x20;
// menuwind's handler made (0x1013F720, its constructor): push esi; mov esi,ecx; lea ecx,[esi+4]; call; push; mov ecx; call
// ...; mov word ptr [esi+0Eh],-1; mov dword ptr [esi+8],0; mov byte ptr [esi+0Ch],1; mov dword ptr [esi],<its table>
constexpr char kMenuMake[] = "56 8B F1 8D 4E 04 E8 ?? ?? ?? ?? 68 ?? ?? ?? ?? B9 ?? ?? ?? ?? 66 C7 46 0E FF FF C7 46 08 00 00 00 00 C6 46 0C 01 "
                             "C7 06 ?? ?? ?? ?? C7 46 14 00 00 00 00 E8";
constexpr std::ptrdiff_t kMakeTableAt = 0x28;
// The two inputs (0x1013FF40, 0x1014D500): movsx eax,word ptr [esp+4]; add eax,-3; push esi; cmp eax,19h; mov esi,ecx; ja;
// xor ecx,ecx; mov cl,[eax+<events>]; jmp [ecx*4+<cases>]; then the flip: (menuwind: push edi;) push 0; push 1; push <the
// other page's name>; mov ecx,<the manager>; call <open a menu>
constexpr char kMenuInput[] = "0F BF 44 24 04 83 C0 FD 56 83 F8 19 8B F1 0F 87 ?? ?? ?? ?? 33 C9 8A 88 ?? ?? ?? ?? FF 24 8D ?? ?? ?? ?? "
                              "57 6A 00 6A 01 68 ?? ?? ?? ?? B9 ?? ?? ?? ?? E8";
constexpr std::ptrdiff_t kMenuInputNameAt = 0x29, kMenuInputManagerAt = 0x2E;
constexpr char kSocialInput[] = "0F BF 44 24 04 83 C0 FD 56 83 F8 19 8B F1 0F 87 ?? ?? ?? ?? 33 C9 8A 88 ?? ?? ?? ?? FF 24 8D ?? ?? ?? ?? "
                                "6A 00 6A 01 68 ?? ?? ?? ?? B9 ?? ?? ?? ?? E8";
constexpr std::ptrdiff_t kSocialInputNameAt = 0x28, kSocialInputManagerAt = 0x2D;
// menuwind opened (0x1013FBD0): mov ax,[<the event's>]; push esi; push edi; xor edi,edi; test ax,ax; mov esi,ecx; ...
constexpr char kMenuOpened[] = "66 A1 ?? ?? ?? ?? 56 57 33 FF 66 85 C0 8B F1 74 0A 0F BF C0 8B 3C 85 ?? ?? ?? ?? 6A 00 E8";
// socialme opened (0x1014D470): SetButtonStatus(window, 0x10, 15), (0xF, 14), (0xF, 13), ..., and in Monstrosity
// SetButtonStatus(window, 5, 5): Set Bazaar can't be chosen. SetButtonStatus (0x10118760: __thiscall window, status, item,
// ret 8) called at +0xA, the Monstrosity check (0x10097B60: no arguments, al) at +0x36.
constexpr char kSocialOpened[] = "56 8B F1 6A 0F 6A 10 8B 4E 08 E8 ?? ?? ?? ?? 8B 4E 08 6A 0E 6A 0F E8 ?? ?? ?? ?? 8B 4E 08 6A 0D 6A 0F E8 "
                                 "?? ?? ?? ?? 8B 46 08 C6 80 86 00 00 00 00 E8 ?? ?? ?? ?? E8 ?? ?? ?? ?? 84 C0 74 0C 8B 4E 08 6A 05 6A 05 E8";
constexpr std::ptrdiff_t kSetStatusCall = 0xA, kMonstrosityCall = 0x36;

constexpr int kSlots = 17, kOpenSlot = 1, kInputSlot = 6;

// The manager's lists, a node, a layout, a window, a sheet, a handler (configmenu.cpp's)
constexpr std::ptrdiff_t kWindows = 0, kLayouts = 0x1C;
constexpr std::ptrdiff_t kNodeNext = 0, kNodeItem = 0x10, kNodeSkip = 0x14;
constexpr std::ptrdiff_t kLayoutParse = 8, kLayoutName = 0x46;
constexpr std::ptrdiff_t kWindowLayout = 4;
constexpr std::ptrdiff_t kSheetName = 0x24;
constexpr std::ptrdiff_t kHandlerWindow = 8;
constexpr int kMaxNodes = 65536;
// The events
constexpr short kRight = 3, kLeft = 4, kSelect = 5, kCancel = 6, kMenuKey = 7, kMenuKeyAgain = 0x1C;

// A layout's payload (configmenu.cpp's notes): the header (name, 1, the count of blocks after the window's), then blocks
constexpr std::size_t kHeader = 0x20, kCountAt = 0x11;
constexpr std::size_t kBlockX = 2, kBlockY = 4, kBlockWidth = 0xA, kBlockHeight = 0xC;
constexpr std::size_t kWindowSprites = 0x14;
constexpr std::size_t kItemId = 0x12, kItemLinks = 0x15, kItemSprites = 0x1B;
constexpr std::size_t kSprites = 0x20, kSpriteSize = 20, kSpriteNumber = 2, kSpriteSheet = 4;
enum Link { kBefore, kAfter, kUp, kDown, kRight_, kLeft_ };
constexpr std::size_t kPayloadMax = 0x1000;

// Each page: 15 blocks after the window, items 1 to 12 88 x 16 in a 112 x 202 window whose frame is "menu    comwin"
// sprite 11 (12 wells), 13 and 14 the arrows, 15 the title tab
constexpr int kPageItems = 12, kPageBlocks = 15, kLeftArrow = 13, kRightArrow = 14, kTitle = 15;
constexpr int kItemWidth = 88, kItemHeight = 16;
constexpr int kPageHeight = 202;
constexpr int kOffset = 15; // page 2's ids: 16 to 27
constexpr int kItems = 2 * kPageItems;
constexpr int kHeight = kPageHeight + kPageItems * kItemHeight; // 394
constexpr char kMenuName[17] = "menu    menuwind";
constexpr char kSocialName[17] = "menu    socialme";
constexpr char kFrameSheet[] = "menu    comwin"; // the first 14 of its name: the parse ends it for the language
constexpr int kSetBazaar = 5 + kOffset;
constexpr int kArrowsY = -0x2000; // the arrows' blocks: far above the window, out of the mouse's reach

// VanaCore's sheet: sprite 0 the frame with 24 wells, as the game's frames are made: the window's background
// ("menu    newtex", light at the top, dim at the bottom), then each well's two halves ("menu    buttonto"); sprite 1
// blank (one clear texel), the arrows' look.
constexpr char kSheet[17] = "menu    vcmainmn";
constexpr int kFrameSprite = 0, kBlankSprite = 1;
constexpr char kBackTexture[17] = "menu    newtex  ";
constexpr char kWellTexture[17] = "menu    buttonto";
constexpr std::uint8_t kBackTop[4] = {0x7F, 0x7F, 0x7F, 0x7F};
constexpr std::uint8_t kBackBottom[4] = {0x40, 0x40, 0x40, 0x40};
constexpr std::uint8_t kWellColour[4] = {0x00, 0x00, 0x00, 0x70};
constexpr std::uint8_t kClear[4] = {0x00, 0x00, 0x00, 0x00};
constexpr std::uint8_t kFrameBlend[4] = {0x01, 0x00, 0x01, 0x01};
constexpr int kFrameWidth = 112;
constexpr std::size_t kPartSize = 61;

using Payload = void(__thiscall*)(void* list, const void** payload);
using Opened = void(__thiscall*)(void* handler);
using Input = void(__thiscall*)(void* handler, int event, int item);
using SetStatus = void(__thiscall*)(void* window, int status, int item);
using Check = char(__cdecl*)();

bool g_on = false;       // [client] singlemenu
bool g_force = false;    // [client] singlemenu.force (explicit override when soulspire_native.dll is loaded)
bool g_native_conflict_logged = false;
bool g_searched = false; // FFXiMain.dll looked at
bool g_found = false;
bool g_failed = false; // something wasn't as known: the game's menu is left alone from then on
ULONGLONG g_next = 0;

char* g_manager = nullptr;
char* g_sheets = nullptr;
char** g_menu_handler = nullptr;   // the global holding menuwind's handler
char** g_social_handler = nullptr; // and socialme's
void* g_table = nullptr;           // menuwind's handler's table, the game's
Payload g_layout_add = nullptr;
Payload g_layout_remove = nullptr;
Payload g_sheet_add = nullptr;
Payload g_sheet_remove = nullptr;
Opened g_menu_opened = nullptr;
Input g_menu_input = nullptr;
Input g_social_input = nullptr;
SetStatus g_set_status = nullptr;
Check g_monstrosity = nullptr;

// VanaCore's, in memory that stays (the game keeps pointers into a payload while it's on its list)
alignas(16) std::uint8_t g_layout[kPayloadMax];   // the one list
alignas(16) std::uint8_t g_original[kPayloadMax]; // menuwind's, as it was: put back when the setting goes off
std::size_t g_original_size = 0;
alignas(16) std::uint8_t g_sheet[2 + 16 * 4 + 2 + 1 + (1 + 2 * kItems) * kPartSize + 1 + kPartSize + 16];
std::size_t g_sheet_size = 0;
void* g_ours[kSlots] = {};

bool g_added = false;
char* g_added_at = nullptr;
bool g_sheet_in = false;

template <typename T> T& at(void* base, std::ptrdiff_t offset)
{
    return *reinterpret_cast<T*>(static_cast<char*>(base) + offset);
}

template <typename T> T operand(std::uintptr_t site, std::ptrdiff_t offset)
{
    return *reinterpret_cast<const T*>(site + offset);
}

std::uintptr_t call_target(std::uintptr_t call)
{
    return call + 5 + *reinterpret_cast<const std::int32_t*>(call + 1);
}

int u16(const std::uint8_t* p)
{
    return p[0] | p[1] << 8;
}

int s16(const std::uint8_t* p)
{
    return static_cast<std::int16_t>(u16(p));
}

void put16(std::uint8_t* p, int value)
{
    p[0] = static_cast<std::uint8_t>(value);
    p[1] = static_cast<std::uint8_t>(value >> 8);
}

// ---- VanaCore's sheet ----

void sheet_put(const void* bytes, std::size_t size)
{
    std::memcpy(g_sheet + g_sheet_size, bytes, size);
    g_sheet_size += size;
}

// A part of a sprite (configmenu.cpp's): a quad from x0, y0 to x1, y1 of the image's texels from u, v, each corner's colour
void sheet_part(int x0, int y0, int x1, int y1, int u, int v, const std::uint8_t (&top)[4], const std::uint8_t (&bottom)[4],
                const char* image)
{
    std::uint8_t part[kPartSize] = {};
    const int corners[12] = {x0, y0, x1, y0, x0, y1, x1, y1, x1 - x0, y1 - y0, u, v};
    for (int i = 0; i < 12; ++i)
        put16(part + 2 * i, corners[i]);
    for (int corner = 0; corner < 4; ++corner)
        std::memcpy(part + 25 + 4 * corner, corner < 2 ? top : bottom, 4);
    std::memcpy(part + 41, kFrameBlend, 4);
    std::memcpy(part + 45, image, 16);
    sheet_put(part, sizeof part);
}

// The sheet's payload: its name, its two images, two sprites: the frame for 24 items (the game's are made so: wells 16
// apart from 8 down, each a 48-wide left half and a 40-wide right half of the button texture), then the blank one
void make_sheet()
{
    g_sheet_size = 0;
    sheet_put(kSheet, 16);
    const std::uint8_t images = 2;
    sheet_put(&images, 1);
    sheet_put(kBackTexture, 16);
    sheet_put(kWellTexture, 16);
    const std::uint8_t sprites[2] = {2, 0};
    sheet_put(sprites, 2);
    const auto parts = static_cast<std::uint8_t>(1 + 2 * kItems);
    sheet_put(&parts, 1);
    sheet_part(0, 0, kFrameWidth, kHeight, 0, 0, kBackTop, kBackBottom, kBackTexture);
    for (int i = 0; i < kItems; ++i) {
        const int y = 8 + kItemHeight * i;
        sheet_part(66, y, 106, y + kItemHeight, 0, 16, kWellColour, kWellColour, kWellTexture);
        sheet_part(18, y, 66, y + kItemHeight, 16, 0, kWellColour, kWellColour, kWellTexture);
    }
    const std::uint8_t blank = 1;
    sheet_put(&blank, 1);
    sheet_part(0, 0, 1, 1, 0, 0, kClear, kClear, kWellTexture);
}

// ---- the game's lists (configmenu.cpp's) ----

char* find(std::ptrdiff_t list, std::ptrdiff_t name_at, const char* name, char* owner)
{
    int count = 0;
    for (char* node = at<char*>(owner, list); node != nullptr && count < kMaxNodes; node = at<char*>(node, kNodeNext), ++count) {
        char* item = at<std::uint8_t>(node, kNodeSkip) == 0 ? at<char*>(node, kNodeItem) : nullptr;
        if (item != nullptr && std::memcmp(item + name_at, name, 16) == 0)
            return item;
    }
    return nullptr;
}

char* find_sheet(const char* name)
{
    return find(0, kSheetName, name, g_sheets);
}

const std::uint8_t* payload_of(char* layout)
{
    char* parse = at<char*>(layout, kLayoutParse);
    const auto* block = parse != nullptr ? at<const std::uint8_t*>(parse, 0) : nullptr;
    return block != nullptr ? block - kHeader : nullptr;
}

// The layouts of a name: the first (the one the game opens and the one taken off by name), ours, and how many
struct Look {
    char* first = nullptr;
    char* ours = nullptr;
    int named = 0;
};

Look look(const char* name)
{
    Look found;
    int count = 0;
    for (char* node = at<char*>(g_manager, kLayouts); node != nullptr && count < kMaxNodes; node = at<char*>(node, kNodeNext), ++count) {
        char* layout = at<std::uint8_t>(node, kNodeSkip) == 0 ? at<char*>(node, kNodeItem) : nullptr;
        if (layout == nullptr || std::memcmp(layout + kLayoutName, name, 16) != 0)
            continue;
        if (found.first == nullptr)
            found.first = layout;
        ++found.named;
        if (payload_of(layout) == g_layout)
            found.ours = layout;
    }
    return found;
}

bool in_use(const char* layout)
{
    int count = 0;
    for (char* node = at<char*>(g_manager, kWindows); node != nullptr && count < kMaxNodes; node = at<char*>(node, kNodeNext), ++count) {
        char* window = at<char*>(node, kNodeItem);
        if (window != nullptr && at<char*>(window, kWindowLayout) == layout)
            return true;
    }
    return false;
}

void call(Payload function, void* list, const void* payload)
{
    function(list, &payload);
}

bool add_sheet()
{
    if (find_sheet(kSheet) == nullptr)
        call(g_sheet_add, g_sheets, g_sheet);
    g_sheet_in = find_sheet(kSheet) != nullptr;
    return g_sheet_in;
}

void remove_sheet()
{
    call(g_sheet_remove, g_sheets, kSheet);
    g_sheet_in = false;
}

void fail(const wchar_t* what)
{
    log::write(L"Single main menu: the game's main menu isn't the one VanaCore knows (%ls): it's left as two pages", what);
    g_failed = true;
}

bool g_waiting_said = false;
void wait_for(const wchar_t* what)
{
    if (!g_waiting_said)
        log::write(L"Single main menu: waiting to change the game's main menu (%ls): tried again", what);
    g_waiting_said = true;
}

// ---- the pages ----

// How long a layout's payload is: its DAT chunk's length, from the chunk's header just before it (configmenu.cpp's), or
// the copy VanaCore put back. 0 if it isn't a layout chunk.
std::size_t room_of(const std::uint8_t* payload)
{
    if (payload == g_original)
        return g_original_size;
    const std::uint32_t info = *reinterpret_cast<const std::uint32_t*>(payload - 0xC);
    const std::size_t length = ((info >> 7) & 0x7FFFF) * 16;
    if ((info & 0x7F) != 0x30 || length <= 16 || length - 16 > kPayloadMax)
        return 0;
    return length - 16;
}

// A page read: where each block starts and how long it is, by id (0 the window's), and its size. nullptr if it's the one
// known, else what isn't.
struct Page {
    std::size_t at[kPageBlocks + 1] = {};
    std::size_t size[kPageBlocks + 1] = {};
    std::size_t total = 0;
};

const wchar_t* read_page(const std::uint8_t* p, std::size_t room, const char* name, Page& page)
{
    if (room < kHeader || std::memcmp(p, name, 16) != 0 || p[0x10] != 1 || p[kCountAt] != kPageBlocks)
        return L"its header";
    std::size_t offset = kHeader;
    for (int b = 0; b <= kPageBlocks; ++b) {
        const std::size_t size = offset + 2 <= room ? static_cast<std::size_t>(u16(p + offset)) : 0;
        if (size < 0x20 || size % 16 != 0 || offset + size > room)
            return L"its blocks";
        if (b != 0 && u16(p + offset + kItemId) != b) // an item is the block of its number
            return L"its items' ids";
        page.at[b] = offset;
        page.size[b] = size;
        offset += size;
    }
    page.total = offset;
    // the window: 202 tall, its frame the frame sheet's sprite 11
    const std::uint8_t* window = p + page.at[0];
    if (s16(window + kBlockHeight) != kPageHeight || window[kWindowSprites] < 1 || u16(window + kSprites) != 0 ||
        s16(window + kSprites + kSpriteNumber) != kPageItems - 1 ||
        std::memcmp(window + kSprites + kSpriteSheet, kFrameSheet, sizeof kFrameSheet - 1) != 0)
        return L"its frame";
    // the items: 88 x 16, one above the other down the same left edge
    const int x = s16(p + page.at[1] + kBlockX);
    for (int id = 1; id <= kPageItems; ++id) {
        const std::uint8_t* item = p + page.at[id];
        if (s16(item + kBlockX) != x || s16(item + kBlockWidth) != kItemWidth || s16(item + kBlockHeight) != kItemHeight ||
            s16(item + kBlockY) < 0 || s16(item + kBlockY) + kItemHeight > kPageHeight)
            return L"its items";
    }
    // the title tab: no way to it
    for (int link = kBefore; link <= kLeft_; ++link)
        if (p[page.at[kTitle] + kItemLinks + link] != 0xFF)
            return L"its title";
    // the arrows: each a sprite
    for (int id = kLeftArrow; id <= kRightArrow; ++id)
        if (p[page.at[id] + kItemSprites] < 1 || page.size[id] < kSprites + kSpriteSize)
            return L"its arrows";
    return nullptr;
}

// The one list: menuwind's header and window (394 tall, its frame VanaCore's), its blocks 1 to 15 in place (the arrows
// and the title tab stay, nothing links to the arrows), socialme's items 1 to 12 as 16 to 27 under them. The game takes an
// item by its place (the block after the window's n is item n: its neighbour lookup, FUN_1011ae20, reads that place and
// ends the cursor on 0 if the id isn't n), so the ids run 1 to 27 in block order. The arrows (13 and 14, drawn beside the
// frame as the pages' Left and Right) have no page to flip to, so each is VanaCore's blank sprite with its block far
// above the window, where the mouse can't land on it. Items 1 to 12 and 16 to 27 are linked top to bottom round the
// list. Its size, or 0.
std::size_t make_list(const std::uint8_t* menu, const Page& mp, const std::uint8_t* social, const Page& sp)
{
    std::size_t size = kHeader + mp.size[0];
    for (int id = 1; id <= kPageBlocks; ++id)
        size += mp.size[id];
    for (int id = 1; id <= kPageItems; ++id)
        size += sp.size[id];
    if (size > kPayloadMax)
        return 0;
    std::memset(g_layout, 0, sizeof g_layout);
    std::memcpy(g_layout, menu, kHeader);
    g_layout[kCountAt] = static_cast<std::uint8_t>(kPageBlocks + kPageItems);
    std::size_t offset = kHeader;
    auto put = [&](const std::uint8_t* from, std::size_t length) {
        std::uint8_t* to = g_layout + offset;
        std::memcpy(to, from, length);
        offset += length;
        return to;
    };
    std::uint8_t* window = put(menu + mp.at[0], mp.size[0]);
    put16(window + kBlockHeight, kHeight);
    put16(window + kSprites + kSpriteNumber, kFrameSprite);
    std::memcpy(window + kSprites + kSpriteSheet, kSheet, 16);
    // the items, and their order top to bottom (page 1's, then page 2's)
    std::uint8_t* items[kPageBlocks + kPageItems + 1] = {};
    std::vector<std::pair<int, int>> order; // y, id
    for (int id = 1; id <= kPageBlocks; ++id) {
        items[id] = put(menu + mp.at[id], mp.size[id]);
        if (id <= kPageItems)
            order.push_back({s16(items[id] + kBlockY), id});
    }
    for (int id = kLeftArrow; id <= kRightArrow; ++id) { // out of sight
        put16(items[id] + kBlockY, kArrowsY);
        put16(items[id] + kSprites + kSpriteNumber, kBlankSprite);
        std::memcpy(items[id] + kSprites + kSpriteSheet, kSheet, 16);
    }
    for (int id = 1; id <= kPageItems; ++id) {
        std::uint8_t* item = put(social + sp.at[id], sp.size[id]);
        put16(item + kItemId, id + kOffset);
        put16(item + kBlockY, s16(item + kBlockY) + kPageItems * kItemHeight);
        items[id + kOffset] = item;
        order.push_back({s16(item + kBlockY), id + kOffset});
    }
    std::sort(order.begin(), order.end());
    for (std::size_t i = 0; i < order.size(); ++i) {
        const int id = order[i].second;
        const auto before = static_cast<std::uint8_t>(order[(i + order.size() - 1) % order.size()].second);
        const auto after = static_cast<std::uint8_t>(order[(i + 1) % order.size()].second);
        const std::uint8_t links[6] = {before, after, before, after, static_cast<std::uint8_t>(id), static_cast<std::uint8_t>(id)};
        std::memcpy(items[id] + kItemLinks, links, sizeof links);
    }
    return offset;
}

// The one list put in: both pages read from their layouts, the sheet in, menuwind's layout taken off and ours put on. Not
// while the main menu is open.
void add_list(const Look& found)
{
    char* game = found.first;
    if (game == nullptr || in_use(game))
        return;
    if (found.named != 1) {
        wait_for(L"more than one main menu of its name");
        return;
    }
    char* social = look(kSocialName).first;
    if (social == nullptr) {
        wait_for(L"its second page");
        return;
    }
    const std::uint8_t* menu = payload_of(game);
    const std::uint8_t* page2 = payload_of(social);
    if (menu == nullptr || page2 == nullptr)
        return;
    const std::size_t menu_room = room_of(menu), social_room = room_of(page2);
    if (menu_room == 0 || social_room == 0) {
        fail(L"its DAT chunks");
        return;
    }
    Page mp, sp;
    if (const wchar_t* odd = read_page(menu, menu_room, kMenuName, mp)) {
        fail(odd);
        return;
    }
    if (const wchar_t* odd = read_page(page2, social_room, kSocialName, sp)) {
        fail(odd);
        return;
    }
    if (menu != g_original) {
        std::memcpy(g_original, menu, mp.total);
        g_original_size = mp.total;
        menu = g_original;
    }
    if (make_list(menu, mp, page2, sp) == 0) {
        fail(L"its size");
        return;
    }
    make_sheet();
    if (!add_sheet()) {
        fail(L"VanaCore's sheet wasn't taken");
        return;
    }
    call(g_layout_remove, g_manager, kMenuName); // the game's: the first of the name
    Look now = look(kMenuName);
    if (now.first == nullptr)
        call(g_layout_add, g_manager, g_layout);
    now = look(kMenuName);
    if (now.ours == nullptr || now.first != now.ours) {
        if (look(kMenuName).first == nullptr)
            call(g_layout_add, g_manager, g_original);
        if (look(kMenuName).ours == nullptr)
            remove_sheet();
        fail(L"its layout wasn't replaced");
        return;
    }
    g_added = true;
    g_added_at = now.ours;
    g_waiting_said = false;
    log::write(L"Single main menu: the main menu's two pages are one list ([client] singlemenu)");
}

// The game's two pages back: ours off the list, menuwind's back from its copy, the sheet off. Not while it's open.
void take_out(const Look& found)
{
    if (in_use(found.ours))
        return;
    if (found.first != found.ours) {
        wait_for(L"another main menu of its name");
        return;
    }
    call(g_layout_remove, g_manager, kMenuName);
    if (look(kMenuName).first == nullptr)
        call(g_layout_add, g_manager, g_original);
    remove_sheet();
    g_added = false;
    g_added_at = nullptr;
    log::write(L"Single main menu: the main menu's two pages are back ([client] singlemenu=0)");
}

void set_table(bool ours)
{
    char* handler = *g_menu_handler;
    if (handler == nullptr)
        return;
    void*& table = at<void*>(handler, 0);
    if (ours && table == g_table)
        table = g_ours;
    else if (!ours && table == static_cast<void*>(g_ours))
        table = g_table;
}

// ---- menuwind's handler's slots ----

// The handler's window is one made from the one list (not the game's page, open as the two are swapped)
bool ours_open(char* window)
{
    char* layout = window != nullptr ? at<char*>(window, kWindowLayout) : nullptr;
    return layout != nullptr && payload_of(layout) == g_layout;
}

// Opened: as the game opens page 1, then socialme's rule for its items: Set Bazaar can't be chosen in Monstrosity
void __fastcall single_opened(char* handler, void*)
{
    g_menu_opened(handler);
    char* window = at<char*>(handler, kHandlerWindow);
    if (ours_open(window) && g_monstrosity() != 0)
        g_set_status(window, 5, kSetBazaar);
}

// An event on the list (after the window's own: the cursor moved, the select sound played): no page flip; the menu key
// closes it; page 2's items are socialme's input (the item as it has it); the rest as the game does it.
void __fastcall single_input(char* handler, void*, int event, int item)
{
    const short e = static_cast<short>(event), chosen = static_cast<short>(item);
    if (ours_open(at<char*>(handler, kHandlerWindow))) {
        if (e == kRight || e == kLeft)
            return;
        if (e == kMenuKey || e == kMenuKeyAgain) {
            g_menu_input(handler, kCancel, item);
            return;
        }
        if (e == kSelect && chosen >= kPageItems + 1 && chosen <= kTitle)
            return; // (the arrows and title: a flip)
        if (e == kSelect && chosen > kOffset && chosen <= kOffset + kPageItems) {
            char* social = *g_social_handler;
            if (social != nullptr && at<void**>(social, 0)[kInputSlot] == reinterpret_cast<void*>(g_social_input))
                g_social_input(social, kSelect, chosen - kOffset);
            return;
        }
    }
    g_menu_input(handler, event, item);
}

// ---- finding it all ----

bool in_image(std::uintptr_t address, std::uintptr_t base, std::size_t size)
{
    return address >= base && address < base + size;
}

bool found()
{
    if (g_searched)
        return g_found;
    const HMODULE game = GetModuleHandleW(L"FFXiMain.dll");
    if (game == nullptr)
        return false;
    g_searched = true;
    const auto base = reinterpret_cast<std::uintptr_t>(game);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + reinterpret_cast<const IMAGE_DOS_HEADER*>(game)->e_lfanew);
    const std::size_t size = nt->OptionalHeader.SizeOfImage;
    const std::uintptr_t help = scan::find(game, kHelp);
    const std::uintptr_t layout_add = scan::find(game, kLayoutAdd);
    const std::uintptr_t layout_remove = scan::find(game, kLayoutRemove);
    const std::uintptr_t sheet_add = scan::find(game, kSheetAdd);
    const std::uintptr_t sheet_remove = scan::find(game, kSheetRemove);
    const std::uintptr_t thunks = scan::find(game, kSheetThunks);
    const std::uintptr_t menu_entry = scan::find(base, size, kMenuEntry);
    const std::uintptr_t social_entry = scan::find(base, size, kSocialEntry);
    const std::uintptr_t make = scan::find(game, kMenuMake);
    const std::uintptr_t menu_input = scan::find(game, kMenuInput);
    const std::uintptr_t social_input = scan::find(game, kSocialInput);
    const std::uintptr_t menu_opened = scan::find(game, kMenuOpened);
    const std::uintptr_t social_opened = scan::find(game, kSocialOpened);
    const wchar_t* missing = help == 0            ? L"the help line"
                             : layout_add == 0    ? L"the layouts' add"
                             : layout_remove == 0 ? L"the layouts' removal"
                             : sheet_add == 0     ? L"the sheets' add"
                             : sheet_remove == 0  ? L"the sheets' removal"
                             : thunks == 0        ? L"the sheets' list"
                             : menu_entry == 0    ? L"the main menu's entry"
                             : social_entry == 0  ? L"the second page's entry"
                             : make == 0          ? L"the main menu's handler"
                             : menu_input == 0    ? L"the main menu's input"
                             : social_input == 0  ? L"the second page's input"
                             : menu_opened == 0   ? L"the main menu opened"
                             : social_opened == 0 ? L"the second page opened"
                                                  : nullptr;
    if (missing == nullptr) {
        const auto manager = operand<std::uintptr_t>(help, kHelpManagerAt);
        const auto sheets = operand<std::uintptr_t>(thunks, kThunkSheetsAt);
        const auto menu_global = operand<std::uintptr_t>(menu_entry, kEntryGlobal);
        const auto social_global = operand<std::uintptr_t>(social_entry, kEntryGlobal);
        const auto table = operand<std::uintptr_t>(make, kMakeTableAt);
        const std::uintptr_t layout_find = call_target(layout_remove + kRemoveFindCall);
        if (!in_image(manager, base, size) || operand<std::uintptr_t>(menu_input, kMenuInputManagerAt) != manager ||
            operand<std::uintptr_t>(social_input, kSocialInputManagerAt) != manager ||
            operand<std::uintptr_t>(sheet_remove, kSheetRemoveLayoutsAt) != manager + static_cast<std::uintptr_t>(kLayouts))
            missing = L"the menu manager";
        else if (!in_image(layout_find, base, size) ||
                 std::memcmp(reinterpret_cast<const void*>(layout_find), kFindStart, sizeof kFindStart) != 0 ||
                 std::memcmp(reinterpret_cast<const void*>(layout_find + kFindNameAt), kFindName, sizeof kFindName) != 0)
            missing = L"the layout of a name";
        else if (!in_image(sheets, base, size) || operand<std::uintptr_t>(thunks, kThunkNext + kThunkSheetsAt) != sheets ||
                 call_target(thunks + kThunkAddCall) != sheet_add || call_target(thunks + kThunkNext + kThunkAddCall) != sheet_remove)
            missing = L"the sheets' list";
        else if (!in_image(operand<std::uintptr_t>(menu_input, kMenuInputNameAt), base, size) ||
                 !in_image(operand<std::uintptr_t>(social_input, kSocialInputNameAt), base, size) ||
                 std::memcmp(operand<const char*>(menu_input, kMenuInputNameAt), kSocialName, 16) != 0 ||
                 std::memcmp(operand<const char*>(social_input, kSocialInputNameAt), kMenuName, 16) != 0)
            missing = L"the pages' flip"; // each input opens the other page by name
        else if (!in_image(menu_global, base, size) || !in_image(social_global, base, size) || !in_image(table, base, size))
            missing = L"the main menu's handlers";
        if (missing == nullptr) {
            void* const* slots = reinterpret_cast<void* const*>(table);
            if (reinterpret_cast<std::uintptr_t>(slots[kOpenSlot]) != menu_opened ||
                reinterpret_cast<std::uintptr_t>(slots[kInputSlot]) != menu_input)
                missing = L"the main menu's handler's table";
        }
        const std::uintptr_t set_status = call_target(social_opened + kSetStatusCall);
        const std::uintptr_t monstrosity = call_target(social_opened + kMonstrosityCall);
        if (missing == nullptr && (!in_image(set_status, base, size) || !in_image(monstrosity, base, size)))
            missing = L"the second page's opened";
        if (missing == nullptr) {
            g_manager = reinterpret_cast<char*>(manager);
            g_sheets = reinterpret_cast<char*>(sheets);
            g_menu_handler = reinterpret_cast<char**>(menu_global);
            g_social_handler = reinterpret_cast<char**>(social_global);
            g_table = reinterpret_cast<void*>(table);
            g_set_status = reinterpret_cast<SetStatus>(set_status);
            g_monstrosity = reinterpret_cast<Check>(monstrosity);
        }
    }
    if (missing != nullptr) {
        log::write(L"Single main menu: %ls isn't as VanaCore knows it in FFXiMain.dll: the main menu is left as two pages", missing);
        return false;
    }
    g_layout_add = reinterpret_cast<Payload>(layout_add);
    g_layout_remove = reinterpret_cast<Payload>(layout_remove);
    g_sheet_add = reinterpret_cast<Payload>(sheet_add);
    g_sheet_remove = reinterpret_cast<Payload>(sheet_remove);
    g_menu_opened = reinterpret_cast<Opened>(menu_opened);
    g_menu_input = reinterpret_cast<Input>(menu_input);
    g_social_input = reinterpret_cast<Input>(social_input);
    void* const* slots = static_cast<void* const*>(g_table);
    for (int slot = 0; slot < kSlots; ++slot)
        g_ours[slot] = slots[slot];
    g_ours[kOpenSlot] = reinterpret_cast<void*>(&single_opened);
    g_ours[kInputSlot] = reinterpret_cast<void*>(&single_input);
    g_found = true;
    log::write(L"Single main menu: the main menu found in FFXiMain.dll (its handler +0x%X, the second page's +0x%X)",
               static_cast<unsigned>(reinterpret_cast<std::uintptr_t>(g_menu_handler) - base),
               static_cast<unsigned>(reinterpret_cast<std::uintptr_t>(g_social_handler) - base));
    return true;
}

} // namespace

void reload(const wchar_t* ini)
{
    g_on = GetPrivateProfileIntW(L"client", L"singlemenu", 0, ini) != 0; // taken up by on_frame
    g_force = GetPrivateProfileIntW(L"client", L"singlemenu.force", 0, ini) != 0;
}

void install(const wchar_t* ini)
{
    reload(ini);
}

void on_frame()
{
    if (g_on && !g_force && GetModuleHandleW(L"soulspire_native.dll") != nullptr) {
        if (!g_native_conflict_logged) {
            g_native_conflict_logged = true;
            log::write(L"Single main menu: disabled because soulspire_native.dll owns menuwind Button ID 16 "
                       L"(set [client] singlemenu.force=1 only for debugging)");
        }
        g_on = false;
    }
    if (g_failed || (!g_on && !g_added && !g_sheet_in))
        return;
    // every frame, one pointer: a handler the game made again (logging in) has our slots at once
    if (g_found)
        set_table(g_on && g_added);
    const ULONGLONG now = GetTickCount64();
    if (now < g_next)
        return;
    g_next = now + 500;
    if (!found()) {
        g_failed = g_searched;
        return;
    }
    const Look list = look(kMenuName);
    if (g_added && list.ours == nullptr) {
        // the game let its menus go (logging out, a reload), ours with them: put in again when they're back
        g_added = false;
        log::write(L"Single main menu: the game's menus were let go; the one list goes back in when they're loaded");
    }
    if (list.ours == nullptr && g_sheet_in && (g_added_at == nullptr || !in_use(g_added_at))) {
        remove_sheet();
        g_added_at = nullptr;
    }
    if (g_on && list.ours == nullptr && !g_sheet_in)
        add_list(list);
    else if (!g_on && list.ours != nullptr)
        take_out(list);
    set_table(!g_failed && g_on && g_added);
}

} // namespace vanacore::singlemenu
