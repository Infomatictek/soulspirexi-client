#include "partylist.hpp"

#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>

#include "log.hpp"
#include "scan.hpp"
#include "soulspire/echo_icons.hpp"
#include "soulspire/state.hpp"

namespace vanacore::partylist {
namespace {

// A member's row of the party list (VA 0x10200C50, tpbar.cpp) and of the alliance lists (VA 0x10200E40, __thiscall on the
// list, ret 10h: a flag, x, y, the member) draw its name and numbers alike (found live 2026-11-06, MCP disassembly):
//  - the HP number's colour comes from 0x10200AD0 (__thiscall on the list, ret 4: the member): the menu resources' colour 0
//    at 75% HP and over (white), 1 under 75 (yellow), 2 under 50 (orange), 3 under 25 (red);
//  - the name is drawn by 0x10200AA0 (__thiscall on the list, ret 10h: x, y, the text, the colour; white, grey for a member
//    in another zone), which prints it in the name plates' big letters;
//  - the MP number by the string draw 0x101C82B0 (__cdecl: x, y, the text, the colour, 0, scale across, scale down; right-
//    aligned at x), its colour pushed as it is (push 80808080h);
//  - the TP number, when Config shows it (or party.tp: tpbar.cpp), by 0x10201020 (__thiscall on the list, ret 0Ch: x, y, the
//    TP): "%d" through the same string draw, grey 0x80606060, blue 0x802060D0 from 1000.
// The party list's pet row (under the members) draws its pet's TP by 0x10201020 too, and its name by 0x101C84B0 (__cdecl:
// 66h, x, y, the text, the colour). The colours are 0xAARRGGBB, 0x80 a channel at full (the letters are drawn MODULATE2X).
// VanaCore points those calls at itself, and writes the MP's pushed colour, only while a setting wants them.
enum List { kParty, kAlliance, kLists };

struct Pattern {
    const char* bytes;
    std::ptrdiff_t at; // the call's (E8) or the push's (68) offset in it
};

// Each list's (the party list's, the alliance lists'):
// the HP colour's call: cmp word ptr [esi+34h],0; jne <another zone>; push esi; mov ecx,ebp; call <HP colour>; the row's x, y
constexpr Pattern kHpColour[kLists] = {{"66 83 7E 34 00 0F 85 ?? ?? ?? ?? 56 8B CD E8 ?? ?? ?? ?? 8B 5C 24 3C 8B 7C 24 38", 14},
                                       {"66 83 7E 34 00 0F 85 ?? ?? ?? ?? 56 8B CD E8 ?? ?? ?? ?? 8B 7C 24 40 8B 5C 24 3C", 14}};
// the name's call for a member in this zone: lea reg,[esi+6]; push <white>; push reg; push x, y; mov ecx,ebp; call <name>
constexpr Pattern kName[kLists] = {{"8D 56 06 68 ?? ?? ?? ?? 52 53 8B CD 57 E8", 13}, {"8D 46 06 8B CD 68 ?? ?? ?? ?? 50 57 53 E8", 13}};
// ... for one in another zone (grey)
constexpr Pattern kAway[kLists] = {{"8D 4E 06 68 ?? ?? ?? ?? 51 53 57 8B CD E8", 13}, {"8D 46 06 68 ?? ?? ?? ?? 50 57 53 8B CD E8", 13}};
// the MP number's colour: its push (the party list's: lea eax,[ebx+0Ah]; push <colour>; push edx; push eax; lea eax,[edi+62h])
constexpr Pattern kMpColour[kLists] = {{"8D 43 0A 68 ?? ?? ?? ?? 52 50 8D 47 62 50 E8", 3}, {"68 ?? ?? ?? ?? 8D 4F 08 50 8D 53 68 51 52 E8", 0}};
// the TP number's call: lea reg,[x+30h]; push the TP's y and x; mov ecx,ebp; call <TP number>
constexpr Pattern kTp[kLists] = {{"8D 57 30 50 52 8B CD E8", 7}, {"8D 53 30 51 52 8B CD E8", 7}};
// The pet row's TP number's call and its name's (the party list's)
constexpr Pattern kPetTp = {"8D 4B 30 50 51 8B CF E8", 7};
constexpr Pattern kPetName = {"6A 66 E8 ?? ?? ?? ?? 83 C4 14 B0 01 5B 5F 5E C2 08 00", 2};
// The TP number itself: mov eax,[esp+0Ch]; sub esp,20h; cmp eax,3E8h; push esi; mov esi,<grey>; jl +5; mov esi,<blue>;
// push eax ... and its call of the string draw at +45h
constexpr char kTpNumber[] = "8B 44 24 0C 83 EC 20 3D E8 03 00 00 56 BE ?? ?? ?? ?? 7C 05 BE ?? ?? ?? ?? 50";
constexpr std::ptrdiff_t kTpGrey = 0x0E;
constexpr std::ptrdiff_t kTpBlue = 0x15;
constexpr std::ptrdiff_t kTpDraw = 0x45;

constexpr std::uint8_t kCall = 0xE8;
constexpr std::uint8_t kPush = 0x68;
constexpr std::ptrdiff_t kHpPercent = 0x30; // a member's HP, in percent
constexpr unsigned kHealthy = 75;           // the game's white from here (colour 0)
constexpr int kReadyTp = 1000;

using HpColour = std::uint32_t(__thiscall*)(void* list, const std::uint8_t* member);
using Name = void(__thiscall*)(void* list, int x, int y, const char* text, std::uint32_t colour);
using PetName = void(__cdecl*)(int room, int x, int y, const char* text, std::uint32_t colour);
using Text = void(__cdecl*)(int x, int y, const char* text, std::uint32_t colour, int flag, float sx, float sy);

// A call in the game's code that VanaCore points at itself while a setting wants it
struct Call {
    std::uintptr_t at = 0;     // the call (0: not found)
    std::int32_t original = 0; // its own offset
    bool ours = false;
};
// A colour the game's code pushes as it is (push imm32)
struct Push {
    std::uintptr_t at = 0; // the push (0: not found)
    std::uint32_t original = 0;
    std::uint32_t now = 0;
};
struct Rows {
    Call hp, name, away, tp;
    Push mp;
};

// A list's settings: its names in capitals; its numbers' colours as the game's (0x80 a channel at full), 0 the game's own
struct Want {
    bool upper = false;
    std::uint32_t hp = 0, mp = 0, tp = 0;
};

Want g_want[kLists];
Rows g_rows[kLists];
Call g_pet_tp, g_pet_name;
HpColour g_hp = nullptr;
Name g_name = nullptr;
PetName g_pet = nullptr;
Text g_text = nullptr;
std::uint32_t g_grey = 0x80606060; // the game's TP colours, as found
std::uint32_t g_blue = 0x802060D0;
bool g_searched = false; // FFXiMain.dll was looked through
bool g_pending = false;  // the settings changed since they were last put into the game's code

// The game's colour for #RRGGBB: each channel 0x80 at full, opaque (0x80)
std::uint32_t game_colour(unsigned long rgb)
{
    std::uint32_t out = 0x80000000u;
    for (int shift = 0; shift < 24; shift += 8)
        out |= ((static_cast<std::uint32_t>(rgb >> shift) & 0xFFu) * 128u + 127u) / 255u << shift;
    return out;
}

// A quarter darker: the game's TP grey under 1000 is its white so
std::uint32_t darker(std::uint32_t colour)
{
    std::uint32_t out = colour & 0xFF000000u;
    for (int shift = 0; shift < 24; shift += 8)
        out |= ((colour >> shift & 0xFFu) * 3u / 4u) << shift;
    return out;
}

// [client] <list>.<what>.color (#RRGGBB) as the game's colour, 0 unset (or not a colour)
std::uint32_t read_colour(const wchar_t* ini, const wchar_t* list, const wchar_t* what)
{
    wchar_t key[48] = {}, text[32] = {};
    std::swprintf(key, 48, L"%ls.%ls.color", list, what);
    GetPrivateProfileStringW(L"client", key, L"", text, 32, ini);
    const wchar_t* at = text;
    while (*at == L' ' || *at == L'#')
        ++at;
    wchar_t* end = nullptr;
    const unsigned long rgb = std::wcstoul(at, &end, 16);
    return end == at + 6 ? game_colour(rgb) : 0u;
}

// A name in capitals: a-z made A-Z, a two-byte Shift-JIS character left whole; one too long for the room as the game has it
const char* capitals(const char* text)
{
    static char out[64];
    if (text == nullptr)
        return text;
    std::size_t n = 0;
    for (; text[n] != '\0' && n + 2 < sizeof out; ++n) {
        const auto c = static_cast<unsigned char>(text[n]);
        if (((c >= 0x81 && c <= 0x9F) || (c >= 0xE0 && c <= 0xFC)) && text[n + 1] != '\0') {
            out[n] = text[n];
            out[n + 1] = text[n + 1];
            ++n;
            continue;
        }
        out[n] = static_cast<char>(c >= 'a' && c <= 'z' ? c - ('a' - 'A') : c);
    }
    if (text[n] != '\0')
        return text;
    out[n] = '\0';
    return out;
}

std::uint32_t hp_colour(int list, void* self, const std::uint8_t* member)
{
    const std::uint32_t game = g_hp(self, member);
    const std::uint32_t own = g_want[list].hp;
    return own != 0 && member != nullptr && member[kHpPercent] >= kHealthy ? own : game;
}
std::uint32_t __fastcall party_hp(void* self, void* /*edx*/, const std::uint8_t* member) { return hp_colour(kParty, self, member); }
std::uint32_t __fastcall alliance_hp(void* self, void* /*edx*/, const std::uint8_t* member)
{
    return hp_colour(kAlliance, self, member);
}

void __fastcall party_name(void* self, void* /*edx*/, int x, int y, const char* text, std::uint32_t colour)
{
    char echo_buf[64] = {};
    int draw_x = x;
    const char* shown = soulspire::echo_icons::format_party_echo_name(x, y, text, echo_buf, sizeof(echo_buf), draw_x);
    g_name(self, draw_x, y, g_want[kParty].upper ? capitals(shown) : shown, colour);
}
void __fastcall alliance_name(void* self, void* /*edx*/, int x, int y, const char* text, std::uint32_t colour)
{
    g_name(self, x, y, g_want[kAlliance].upper ? capitals(text) : text, colour);
}
void __cdecl pet_name(int room, int x, int y, const char* text, std::uint32_t colour)
{
    g_pet(room, x, y, g_want[kParty].upper ? capitals(text) : text, colour);
}

std::uint32_t tp_colour_of(int list, int tp)
{
    const std::uint32_t own = g_want[list].tp;
    if (own == 0)
        return tp >= kReadyTp ? g_blue : g_grey;
    return tp >= kReadyTp ? own : darker(own);
}

// The game's TP number (0x10201020) in the list's colour: "%d" through the string draw, right-aligned at x, as it prints it
void tp_number(int list, int x, int y, int tp)
{
    char text[16] = {};
    std::snprintf(text, sizeof text, "%d", tp);
    g_text(x, y, text, tp_colour_of(list, tp), 0, 1.0f, 1.0f);
}
void __fastcall party_tp(void* /*self*/, void* /*edx*/, int x, int y, int tp) { tp_number(kParty, x, y, tp); }
void __fastcall alliance_tp(void* /*self*/, void* /*edx*/, int x, int y, int tp) { tp_number(kAlliance, x, y, tp); }

using HpHook = std::uint32_t(__fastcall*)(void*, void*, const std::uint8_t*);
using NameHook = void(__fastcall*)(void*, void*, int, int, const char*, std::uint32_t);
using TpHook = void(__fastcall*)(void*, void*, int, int, int);
const HpHook kHpHooks[kLists] = {&party_hp, &alliance_hp};
const NameHook kNameHooks[kLists] = {&party_name, &alliance_name};
const TpHook kTpHooks[kLists] = {&party_tp, &alliance_tp};

std::uintptr_t call_target(std::uintptr_t at)
{
    std::int32_t rel = 0;
    std::memcpy(&rel, reinterpret_cast<const void*>(at + 1), 4);
    return at + 5 + static_cast<std::uintptr_t>(rel);
}

// The call `p` finds, if it goes where the others of its kind do (`to`: 0 for the first found, which sets it)
Call find_call(HMODULE game, const Pattern& p, std::uintptr_t& to)
{
    Call call;
    const std::uintptr_t site = scan::find(game, p.bytes);
    const std::uintptr_t at = site + p.at;
    if (site == 0 || *reinterpret_cast<const std::uint8_t*>(at) != kCall || (to != 0 && call_target(at) != to))
        return call;
    to = call_target(at);
    call.at = at;
    std::memcpy(&call.original, reinterpret_cast<const void*>(at + 1), 4);
    return call;
}

Push find_push(HMODULE game, const Pattern& p)
{
    Push push;
    const std::uintptr_t site = scan::find(game, p.bytes);
    if (site == 0 || *reinterpret_cast<const std::uint8_t*>(site + p.at) != kPush)
        return push;
    push.at = site + p.at;
    std::memcpy(&push.original, reinterpret_cast<const void*>(push.at + 1), 4);
    push.now = push.original;
    return push;
}

// The TP number's own code (where its calls go, `tp`): its colours and its string draw. False: it isn't what it should be.
bool find_text(HMODULE game, std::uintptr_t tp)
{
    const std::uintptr_t at = scan::find(game, kTpNumber);
    if (at == 0 || at != tp || *reinterpret_cast<const std::uint8_t*>(at + kTpDraw) != kCall)
        return false;
    g_grey = scan::operand(at, kTpGrey);
    g_blue = scan::operand(at, kTpBlue);
    g_text = reinterpret_cast<Text>(call_target(at + kTpDraw));
    return true;
}

int found_of(const Call& a, const Call& b) { return (a.at != 0 ? 1 : 0) + (b.at != 0 ? 1 : 0); }

void say_found()
{
    const Rows &p = g_rows[kParty], &a = g_rows[kAlliance];
    log::write(L"Party list: the names' draws found %d of 4 and the pet's %d of 1, the HP colours %d of 2, the MP colours %d of 2, "
               L"the TP numbers %d of 2 and the pet's %d of 1",
               found_of(p.name, p.away) + found_of(a.name, a.away), g_pet_name.at != 0 ? 1 : 0, found_of(p.hp, a.hp),
               (p.mp.at != 0 ? 1 : 0) + (a.mp.at != 0 ? 1 : 0), found_of(p.tp, a.tp), g_pet_tp.at != 0 ? 1 : 0);
}

// The game's row code, the first time it's wanted once FFXiMain.dll is there
bool found()
{
    if (g_searched)
        return true;
    const HMODULE game = GetModuleHandleW(L"FFXiMain.dll");
    if (game == nullptr)
        return false;
    g_searched = true;
    std::uintptr_t hp = 0, name = 0, tp = 0, pet = 0;
    for (int list = 0; list < kLists; ++list) {
        Rows& rows = g_rows[list];
        rows.hp = find_call(game, kHpColour[list], hp);
        rows.name = find_call(game, kName[list], name);
        rows.away = find_call(game, kAway[list], name);
        rows.tp = find_call(game, kTp[list], tp);
        rows.mp = find_push(game, kMpColour[list]);
    }
    g_pet_tp = find_call(game, kPetTp, tp);
    g_pet_name = find_call(game, kPetName, pet);
    g_hp = reinterpret_cast<HpColour>(hp);
    g_name = reinterpret_cast<Name>(name);
    g_pet = reinterpret_cast<PetName>(pet);
    if (tp != 0 && !find_text(game, tp)) { // (the TP numbers stay the game's)
        g_rows[kParty].tp = g_rows[kAlliance].tp = g_pet_tp = Call{};
        log::write(L"Party list: the TP number at +0x%X isn't as expected: its colour stays the game's",
                   static_cast<unsigned>(tp - reinterpret_cast<std::uintptr_t>(game)));
    }
    say_found();
    return true;
}

// A call's offset or a push's colour (4 bytes), written in one store (the game's thread runs this code: on_frame is on it)
void write_code(std::uintptr_t at, std::uint32_t value)
{
    DWORD protect = 0;
    if (!VirtualProtect(reinterpret_cast<void*>(at), 4, PAGE_EXECUTE_READWRITE, &protect)) {
        log::write(L"Party list: the game's code at %p couldn't be written (error %lu)", reinterpret_cast<void*>(at), GetLastError());
        return;
    }
    *reinterpret_cast<volatile std::uint32_t*>(at) = value;
    VirtualProtect(reinterpret_cast<void*>(at), 4, protect, &protect);
    FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<const void*>(at), 4);
}

// `call` to `ours` (on) or back to the game's
void point(Call& call, const void* ours, bool on)
{
    if (call.at == 0 || call.ours == on)
        return;
    const std::uintptr_t to = reinterpret_cast<std::uintptr_t>(ours);
    write_code(call.at + 1, on ? static_cast<std::uint32_t>(to - (call.at + 5)) : static_cast<std::uint32_t>(call.original));
    call.ours = on;
}

// `push`'s colour `own`, or the game's (0)
void paint(Push& push, std::uint32_t own)
{
    const std::uint32_t want = own != 0 ? own : push.original;
    if (push.at == 0 || push.now == want)
        return;
    write_code(push.at + 1, want);
    push.now = want;
}

void apply()
{
    if (!found())
        return;
    g_pending = false;
    for (int list = 0; list < kLists; ++list) {
        const Want& want = g_want[list];
        Rows& rows = g_rows[list];
        point(rows.hp, reinterpret_cast<const void*>(kHpHooks[list]), want.hp != 0);
        const bool hook_name = want.upper || (list == kParty && soulspire::engine().enabled());
        point(rows.name, reinterpret_cast<const void*>(kNameHooks[list]), hook_name);
        point(rows.away, reinterpret_cast<const void*>(kNameHooks[list]), want.upper);
        point(rows.tp, reinterpret_cast<const void*>(kTpHooks[list]), want.tp != 0);
        paint(rows.mp, want.mp);
    }
    point(g_pet_tp, reinterpret_cast<const void*>(kTpHooks[kParty]), g_want[kParty].tp != 0);
    point(g_pet_name, reinterpret_cast<const void*>(&pet_name), g_want[kParty].upper);
}

void read(const wchar_t* ini, int list, const wchar_t* prefix)
{
    Want& want = g_want[list];
    wchar_t key[48] = {};
    std::swprintf(key, 48, L"%ls.names.upper", prefix);
    want.upper = GetPrivateProfileIntW(L"client", key, 0, ini) != 0;
    want.hp = read_colour(ini, prefix, L"hp");
    want.mp = read_colour(ini, prefix, L"mp");
    want.tp = read_colour(ini, prefix, L"tp");
}

} // namespace

void reload(const wchar_t* ini)
{
    read(ini, kParty, L"party");
    read(ini, kAlliance, L"alliance");
    g_pending = true; // (written on the game's thread: on_frame)
}

void install(const wchar_t* ini)
{
    reload(ini);
}

void on_frame()
{
    if (g_pending)
        apply();
}

std::uint32_t tp_colour(int tp, std::uint32_t game)
{
    return g_want[kParty].tp != 0 ? tp_colour_of(kParty, tp) : game;
}

} // namespace vanacore::partylist
