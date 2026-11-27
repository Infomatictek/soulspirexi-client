#include "chat.hpp"
#include "addons.hpp"

#include <windows.h>
#include <intrin.h>

#include <vanacore/log_file.hpp>

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <deque>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "../gfx/vanacore_gfx.h"
#include "aliases.hpp"
#include "crash.hpp"
#include "game.hpp"
#include "gui.hpp"
#include "alliancelist.hpp"
#include "buffwin.hpp"
#include "buttons.hpp"
#include "buttonspace.hpp"
#include "castbar.hpp"
#include "chatfont.hpp"
#include "chatpages.hpp"
#include "hudwin.hpp"
#include "gametext.hpp"
#include "gameui.hpp"
#include "gamewin.hpp"
#include "helpbar.hpp"
#include "itemrarity.hpp"
#include "itemslot.hpp"
#include "itemtips.hpp"
#include "imports.hpp"
#include "itemlist.hpp"
#include "log.hpp"
#include "labeltext.hpp"
#include "soulspire/echoes_rebrand.hpp"
#include "soulspire/state.hpp"
#include "listtext.hpp"
#include "macrotext.hpp"
#include "menulog.hpp"
#include "mcp.hpp"
#include "outline.hpp"
#include "phraselist.hpp"
#include "pluginapi.hpp"
#include "profilewin.hpp"
#include "raise.hpp"
#include "scan.hpp"
#include "targetwin.hpp"
#include "ui.hpp"
#include "uispace.hpp"
#include "window.hpp"
#include "winimage.hpp"

namespace vanacore::chat {
namespace {

// int __cdecl (const char* line, int mode): the game's command line. Everything typed into the
// chat box, and every line a macro or menu runs, goes through it; it runs "/" commands and sends
// the rest as chat. It starts `sub esp, 0x404`.
using ParseInput = int(__cdecl*)(const char* line, int mode);
constexpr char kParsePattern[] = "81 EC 04 04 00 00 8B 8C 24 08 04 00 00 55 56 57 8D 84 24 10 02 00 00 68 00 02 00 00 "
                                 "50 33 ED 51 8B 0D ?? ?? ?? ?? 89 6C 24 18 E8";
constexpr size_t kParsePrologue = 6;

// void __cdecl (int mode, const char* text): adds a line to the chat log, the game's own wrapper
// around its chat log object's add-line method. `mov ecx, [chat log]` at +0x17; the global holding
// the chat log is null until the game makes it.
using WriteLine = void(__cdecl*)(int mode, const char* text);
constexpr char kWritePattern[] = "8A 44 24 04 8B 54 24 08 88 44 24 04 33 C0 50 6A 01 8D 4C 24 0C 50 51 8B 0D ?? ?? ?? ?? "
                                 "52 88 44 24 19 88 44 24 1A E8 ?? ?? ?? ?? C3";
constexpr size_t kWriteLogOffset = 0x19;

// unsigned char __thiscall (chat log, const char* text, const unsigned char* mode, int, char, int): the chat log's
// add-line method, which every line the game writes into the chat log goes through (the writer above calls it too,
// with 0, 1, 0). `mode` is 3 bytes: the line's mode, its flags (the indent: & 0x1F), one more. Returns 1 for a line it
// kept out, as for one it added. Reached through the writer's call at +0x26; starts sub esp, 0x844 (6 bytes).
using AddLine = unsigned char(__fastcall*)(void* log, void* edx, const char* text, const unsigned char* mode, int a, int b, int c);
constexpr size_t kWriteCall = 0x26;
constexpr size_t kAddLinePrologue = 6;

// mov ecx,[input] / push ... / call [edx+28h]: the global holding the chat manager, whose input
// line object is at +0x7E30. The line's state is at +0xF108 (1 before it's first used, 3 once it has been, closed; 4 or 5
// while typing, which is the game's own "typing" check, FUN_101b43f0), its text at +0x7E30+0x6C4 and its length at
// +0x7E30+0x888.
constexpr char kInputPattern[] = "8B 0D ?? ?? ?? ?? 6A ?? 53 53 8B 11 53 6A ?? 6A ?? 53 6A";
constexpr size_t kInputOffset = 2;
constexpr size_t kInputState = 0xF108;
constexpr size_t kInputText = 0x7E30 + 0x6C4;
constexpr size_t kInputLength = 0x7E30 + 0x888;
constexpr size_t kInputCapacity = 0x888 - 0x6C4;

// Longest piece of a line handed to the game at once (its buffers are a few KB, and some bytes,
// such as auto-translate phrases, expand).
constexpr size_t kMaxLine = 240;
constexpr size_t kMaxWaiting = 200;

ParseInput g_parse = nullptr; // the game's own, through the trampoline
void** g_input = nullptr;     // the global holding the chat manager
WriteLine g_write = nullptr;
void** g_chat_log = nullptr;
CommandHandler g_handler = nullptr;
AddLine g_add_line = nullptr; // the game's own, through the trampoline
TextHandler g_text_handler = nullptr;
bool g_in_text = false;   // a text handler running: lines it writes go straight in
bool g_writing = false;   // flush() writing VanaCore's own lines (injected)
bool g_installed = false;
bool g_injecting = false; // running a line from run()
bool g_in_handler = false;
bool g_lines_dropped = false;
std::deque<std::pair<int, std::string>> g_waiting; // lines for the chat log, oldest first
std::deque<std::pair<int, std::string>> g_commands;

// "/shutdown" - typed, or a macro's or a menu's: the game is about to quit
bool is_shutdown(const char* line)
{
    while (*line == ' ' || *line == '\t')
        ++line;
    constexpr char kCommand[] = "/shutdown";
    constexpr size_t kLength = sizeof kCommand - 1;
    return _strnicmp(line, kCommand, kLength) == 0 && (line[kLength] == '\0' || line[kLength] == ' ' || line[kLength] == '\t');
}

#include "chat_raw.h" // raw_command, handled_line: Ashita's /raw, and a plugin's /raw copy of a macro's line run in its place

int __cdecl parse_input(const char* line, int mode)
{
    if (const char* raw = raw_command(line))
        return g_parse(raw, mode);
    // An alias (/alias /dt /lac fwd dt) is put in its command's place before anything else sees the line
    std::string expanded;
    if (line != nullptr && aliases::expand(line, expanded))
        line = expanded.c_str();
    if (line != nullptr && is_shutdown(line))
        crash::shutting_down(); // its teardown faults every time: no crash dump for it
    if (line != nullptr && g_handler != nullptr && !g_in_handler) {
        const size_t queued = g_commands.size();
        g_in_handler = true;
        bool handled = false;
        try {
            handled = g_handler(line, mode & 0xFF, g_injecting);
        } catch (...) {
            log::write(L"Chat: the command handler failed on a line; the game runs it");
        }
        g_in_handler = false;
        if (handled)
            return handled_line(queued, mode);
    }
    // A line no command took, about to go to the game (chat, or the game's own commands): the text handler may change it
    // or block it
    if (line != nullptr && g_text_handler != nullptr && !g_in_text && !g_in_handler) {
        Text text;
        text.mode = text.mode_modified = mode & 0xFF;
        text.message = text.modified = line;
        text.injected = g_injecting;
        g_in_text = true;
        bool blocked = false;
        try {
            blocked = g_text_handler(false, text);
        } catch (...) {
            log::write(L"Chat: the text handler failed on a line going out; the game gets it");
        }
        g_in_text = false;
        if (blocked)
            return 0;
        if (text.modified != text.message || text.mode_modified != text.mode)
            return g_parse(text.modified.c_str(), (mode & ~0xFF) | (text.mode_modified & 0xFF));
    }
    return g_parse(line, mode);
}

// [client] chatlog=1: every line that reaches the chat log is also appended to logs\chatlog.<character>.<date>.log
// (the character is the local player, the date the local day, so a new file starts at midnight). The setting is
// read again every couple of seconds, so it applies while the game runs.
std::string plain(std::string_view text);
std::wstring g_chatlog_dir, g_chatlog_ini;
bool g_chatlog_on = false;
bool g_stamps = false; // [client] chat.timestamps=1: a time in front of each line in the chat log (not in the file)
int g_stamp_style = 0;       // [client] chat.timestamps.style: 0 square brackets, 1 round, 2 none (" - " after the time)
bool g_stamp_seconds = true; // [client] chat.timestamps.seconds
bool g_stamp_12h = false;    // [client] chat.timestamps.hour12
bool g_stamp_system = true;  // [client] chat.timestamps.system: system, NPC and server lines too (the game gives them an indent)
float g_window_opacity = 1.0f; // [client] window.opacity (percent): every window's background
float g_chat_opacity = 1.0f;   // [client] chat.opacity (percent): the chat windows' (the all-windows one doesn't apply to them)
float g_border_opacity = 1.0f; // [client] window.border.opacity: every window's frame
int g_window_tint = 0, g_chat_tint = 0;                 // [client] window.color / chat.color: set
std::uint32_t g_window_tint_rgb = 0, g_chat_tint_rgb = 0; // ... as 0xRRGGBB
int g_border_tint = 0, g_chat_border_tint = 0;               // [client] window.border.color / chat.border.color
std::uint32_t g_border_tint_rgb = 0, g_chat_border_tint_rgb = 0;
float g_chat_border_opacity = 1.0f; // [client] chat.border.opacity
// [client] input.match: the chat line's window looks as every window does ("windows", the game's way), as the chat windows
// do ("chat"), or its own way ("off": input.opacity, input.border.opacity, input.color, input.border.color)
int g_input_match = 0; // 0 windows, 1 chat, 2 off
float g_input_opacity = 1.0f, g_input_border_opacity = 1.0f;
int g_input_tint = 0, g_input_border_tint = 0;
std::uint32_t g_input_tint_rgb = 0, g_input_border_tint_rgb = 0;
// [client] window.border.solid / chat.border.solid / input.border.solid: frames drawn as a solid line in the frame colour
int g_border_solid = 0, g_chat_border_solid = 0, g_input_border_solid = 0;
// [client] window.corners (square, cut, round) and window.corners.size (1-32 of the interface's pixels): the windows' corners,
// on those with a solid frame or none (d3d8_corners.h)
int g_corner_shape = 0, g_corner_size = 6;
int g_chat_shadow = 0;               // [client] chat.shadow: 1 a shadow under the chat's text, 2 an outline round it
float g_chat_shadow_opacity = 0.8f; // [client] chat.shadow.opacity (percent): how dark
bool g_no_indent = false;     // [client] chat.noindent=1: no hanging indent on the lines the game wraps
// [client] chat.history: how many sent lines Up and Down bring back in the chat line (9 in the game, up to 100)
constexpr int kHistoryRetail = 9, kHistoryDefault = 20, kHistoryMax = 100;
int g_history_limit = kHistoryDefault;
// [client] chat.scrollback: how many lines the chat window scrolls back through (100-12500: the game saves 1000 a window,
// chatpages.cpp has it save as many as this asks; 0 leaves the game's own scrolling, which stops at the rows it holds)
constexpr int kScrollbackMin = 100, kScrollbackMax = 12500;
int g_scrollback_lines = kScrollbackMax;
bool g_stamp_upper = false;  // [client] chat.timestamps.ampm=upper: 8:34PM instead of 8:34pm
bool g_stamp_ampm_space = false; // ...=upper_spaced / lower_spaced: 8:34 PM
// [client] chat.timestamps.color=#RRGGBB: the time in that colour (not set: the line's own). The game's text colours are
// codes ("\x1E" and a byte) looked up in a table by FFXiMain's 0x10129840; this code isn't in the table, so that lookup is
// hooked to answer for it.
constexpr int kStampColourId = 0xF5;
bool g_stamp_colour = false;
std::uint32_t g_stamp_abgr = 0x80808080;

// [client] chat.prefix=1: "[Linkshell] " and the like in front of the lines of the chat modes below. Their numbers are
// the chat log's own mode byte, as the text events have it: one for your own lines, one for others' (Windower's
// res/chat.lua outgoing/incoming). Seen in the game: 11 yell, 212 Unity (players and Apururu's notices), 214 and 217
// (its message of the day) linkshell 2; 121, the server's system lines ("Active Bonus: ...") get none. [client]
// chat.modes.debug=1 logs each line's mode to vanacore.log, to check the rest against what the game does.
// chat.prefix.short=1: the short names ([LS 2], [P], [T]).
bool g_prefix = false;
bool g_prefix_short = false;
bool g_mode_debug = false;
struct ModeName {
    int mode;
    const char* name;
    const char* brief;
    bool linkshell; // the game starts the line with the linkshell's number: "[2]<Name> text"
};
constexpr ModeName kModeNames[] = {
    {1, "Say", "S"}, {2, "Shout", "Sh"}, {3, "Yell", "Y"}, {4, "Tell", "T"}, {5, "Party", "P"}, {6, "Linkshell", "LS", true}, {7, "Emote", "Em"},
    {9, "Say", "S"}, {10, "Shout", "Sh"}, {11, "Yell", "Y"}, {12, "Tell", "T"}, {13, "Party", "P"}, {14, "Linkshell", "LS", true}, {15, "Emote", "Em"},
    {211, "Unity", "U"}, {212, "Unity", "U"}, {213, "Linkshell 2", "LS 2", true}, {214, "Linkshell 2", "LS 2", true}, {217, "Linkshell 2", "LS 2", true},
    {219, "Assist", "A"}, {220, "Assist", "A"}, {221, "Assist", "A"}, {222, "Assist", "A"},
};
const ModeName* mode_entry(int mode)
{
    for (const ModeName& entry : kModeNames)
        if (entry.mode == mode)
            return &entry;
    return nullptr;
}
const char* mode_name(int mode)
{
    const ModeName* entry = mode_entry(mode);
    return entry == nullptr ? nullptr : g_prefix_short ? entry->brief : entry->name;
}

// "[Linkshell 2] " (or "[LS 2] ") and the line, without the game's own "[2]" in front of a linkshell line: the prefix says
// which linkshell. Colour codes before the number stay.
std::string with_prefix(const ModeName& entry, std::string_view text)
{
    std::string out = std::string("[") + (g_prefix_short ? entry.brief : entry.name) + "] ";
    size_t at = 0;
    while (at + 1 < text.size() && (text[at] == '\x1E' || text[at] == '\x1F'))
        at += 2;
    if (entry.linkshell && text.size() - at >= 3 && text[at] == '[' && (text[at + 1] == '1' || text[at + 1] == '2') && text[at + 2] == ']')
        return out.append(text.substr(0, at)).append(text.substr(at + 3));
    return out.append(text);
}

// "[20:34:05] ", "(8:34:05pm) " or "8:34pm - ": the time now as the settings above have it, with its separator
std::string timestamp()
{
    SYSTEMTIME time;
    GetLocalTime(&time);
    char clock[24];
    if (g_stamp_12h) {
        const unsigned hour = time.wHour % 12 == 0 ? 12 : time.wHour % 12;
        const char* half = g_stamp_upper ? (time.wHour >= 12 ? "PM" : "AM") : (time.wHour >= 12 ? "pm" : "am");
        const char* gap = g_stamp_ampm_space ? " " : "";
        if (g_stamp_seconds)
            sprintf_s(clock, "%u:%02u:%02u%s%s", hour, time.wMinute, time.wSecond, gap, half);
        else
            sprintf_s(clock, "%u:%02u%s%s", hour, time.wMinute, gap, half);
    } else if (g_stamp_seconds) {
        sprintf_s(clock, "%02u:%02u:%02u", time.wHour, time.wMinute, time.wSecond);
    } else {
        sprintf_s(clock, "%02u:%02u", time.wHour, time.wMinute);
    }
    std::string stamp = g_stamp_style == 1 ? std::string("(") + clock + ")" : g_stamp_style == 2 ? std::string(clock) + " -" : std::string("[") + clock + "]";
    if (g_stamp_colour) // the game's colour code, ours (its colour is looked up in colour_hook), then back to the line's
        stamp = std::string("\x1E") + static_cast<char>(kStampColourId) + stamp + "\x1E\x01";
    return stamp + " ";
}

// Whether the line starts with a stamp of ours already. The game hands a system line it was given back to its add-line method
// (mode 190 copies for its other logs), by then with the stamp on; those aren't stamped again.
bool has_stamp(const char* text)
{
    const char* at = text;
    while ((at[0] == '\x1E' || at[0] == '\x1F') && at[1] != '\0')
        at += 2;
    const auto digits = [&](int least, int most) {
        int count = 0;
        while (count < most && at[count] >= '0' && at[count] <= '9')
            ++count;
        if (count < least)
            return false;
        at += count;
        return true;
    };
    if (g_stamp_style != 2 && *at++ != (g_stamp_style == 1 ? '(' : '['))
        return false;
    if (!digits(1, 2) || *at++ != ':' || !digits(2, 2))
        return false;
    if (*at == ':' && (++at, !digits(2, 2)))
        return false;
    if (*at == ' ' && (at[1] == 'a' || at[1] == 'A' || at[1] == 'p' || at[1] == 'P'))
        ++at;
    if ((at[0] == 'a' || at[0] == 'A' || at[0] == 'p' || at[0] == 'P') && (at[1] == 'm' || at[1] == 'M'))
        at += 2;
    return g_stamp_style == 1 ? *at == ')' : g_stamp_style == 2 ? at[0] == ' ' && at[1] == '-' : *at == ']';
}
ULONGLONG g_chatlog_checked = 0;
std::string g_chatlog_name;
std::wstring g_chatlog_path;
FILE* g_chatlog_file = nullptr;

// [client] chat.lineheight: the chat window's row spacing, 16 pixels in the game (FFXiMain's chat draw, 0x10160da0,
// steps each row by an immediate 0x10: `add edi,-10h`, `sub edi,10h`, `add eax,-10h`, `sub edx,10h`). The four
// immediates are patched in place, so the rows spread out (the window's own height stays: fewer rows show).
constexpr int kLineHeightDefault = 16;
constexpr int kLineHeightMax = 64;
struct LineSite {
    const char* pattern;
    bool negative; // the immediate is the negated height (an `add`)
    std::uintptr_t at = 0;
};
LineSite g_line_sites[4] = {
    {"83 C7 F0 8B 4C 24 10 8D 54 24 14 8D 44 24 34 52 8B 49 18 50 E8", true},
    {"83 EF 10 3B EB 7C ?? EB 08 8B 44 24 28 89 44 24 34 E8", false},
    {"83 C0 F0 89 44 24 2C 8B 44 24 10 8D 4C 24 30 8D 54 24 34 51 8B 48 18 52", true},
    {"83 EA 10 3B C1 89 44 24 14 89 54 24 2C 0F 8C ?? ?? ?? ?? E8", false},
};
bool g_line_found = false;
int g_line_height = kLineHeightDefault;

// The chat window's own sizing (0x10161490): its height is lines * 16 less 128 (8 rows' worth) over the base the window has
// (`sub edi,8 ... shl edi,4` is (lines - 8) * 16). With another row spacing L the part per line is L: `imul edi,edi,L` and
// `add edi,-128` take their places (the same bytes), so a full window is as tall as the lines in it.
std::uintptr_t g_height_site = 0;
void write_code(std::uintptr_t at, const void* bytes, size_t size);

void apply_line_height(int height)
{
    if (!g_line_found || height == g_line_height)
        return;
    for (const LineSite& site : g_line_sites) {
        auto* immediate = reinterpret_cast<std::uint8_t*>(site.at + 2);
        DWORD old = 0;
        if (!VirtualProtect(immediate, 1, PAGE_EXECUTE_READWRITE, &old))
            continue;
        *immediate = static_cast<std::uint8_t>(site.negative ? -height : height);
        VirtualProtect(immediate, 1, old, &old);
        FlushInstructionCache(GetCurrentProcess(), immediate, 1);
    }
    g_line_height = height;
    if (g_height_site != 0) {
        const bool game = height == kLineHeightDefault;
        const std::uint8_t first[3] = {0x6B, 0xFF, static_cast<std::uint8_t>(height)}; // imul edi,edi,height
        const std::uint8_t first_game[3] = {0x83, 0xEF, 0x08};                         // sub edi,8
        const std::uint8_t second[3] = {0x83, 0xC7, 0x80};                             // add edi,-128
        const std::uint8_t second_game[3] = {0xC1, 0xE7, 0x04};                        // shl edi,4
        write_code(g_height_site, game ? first_game : first, 3);
        write_code(g_height_site + 7, game ? second_game : second, 3);
    }
    log::write(L"Chat: row spacing %d px ([client] chat.lineheight)", height);
}

using ColourLookup = std::uint32_t(__cdecl*)(int id);
ColourLookup g_colour_lookup = nullptr; // the game's own: the colour (four bytes) of text colour code `id`

std::uint32_t __cdecl colour_hook(int id)
{
    if ((id & 0xFFFF) == kStampColourId)
        return g_stamp_abgr;
    return g_colour_lookup(id);
}

// chat.noindent: the hanging indent on the second line of a wrapped message isn't in the line's flags. The add-line method
// (0x10127740) asks a table of message modes (0x1036c890: say, shout, tell, party, linkshell, system... the ones marked 1)
// whether the mode indents, and hands that to the wrapper (0x10129f60, wrap mode 1: later lines get the indent string
// 0x1036d128 and a narrower width). While add_line runs, that answer is no.
using IndentLookup = std::uint32_t(__cdecl*)(std::uint32_t mode);
IndentLookup g_indent_lookup = nullptr; // the game's own (also asked elsewhere, by 0x101361b0, which is left alone)
int g_in_add_line = 0;                  // add_line is running: the game's add-line method is below us

std::uint32_t __cdecl indent_hook(std::uint32_t mode)
{
    if (g_no_indent && g_in_add_line > 0)
        return 0;
    return g_indent_lookup(mode);
}

// [client] menu.rows: how many options an NPC's selection menu shows at once (the game's is 3). FFXiMain's event choice
// window (open 0x1014cb60, draw 0x1014cf70, cursor 0x1014ce40, keys 0x1014d1a0/0x1014d2a0) has the 3 in eight
// immediates, patched here. The menu's resource only lays out three rows (the position routine, 0x10118500, finds no
// row 4), so for the rows after the third the position is made from the third's, one row pitch further down each; and
// when the menu opens its window is made taller (upward, the bottom stays by the chat) with the game's own resize, and the
// three rows' positions moved up with it.
constexpr int kMenuRowsDefault = 3;
constexpr int kMenuRowsMax = 12;
struct MenuPatch {
    int offset;    // of the immediate, from the start of the match
    int minus;     // 0: the immediate is the row count; 1: its negative; 2: the row count less one, negated
};
struct MenuSite {
    const char* pattern;
    MenuPatch patches[2];
    std::uintptr_t at = 0;
};
MenuSite g_menu_sites[] = {
    {"32 0F BF FF 83 C7 03 3B FE 7C 07", {{6, 0}, {-1, 0}}},
    {"3B FE 7C 07 83 C6 FD 66 89 70 32 66 83 78 32 00", {{6, 1}, {-1, 0}}},
    {"BF FA 2B FE 83 FF 03 7C 07 8D 72 FE", {{6, 0}, {11, 2}}},
    {"24 10 74 67 83 FB 03 7D 62 8D 44 24 10 8B CD 50 E8", {{6, 0}, {-1, 0}}},
    {"FF FF EB 56 8D 47 03 6A 01 50 8B CE E8 ?? ?? ?? ??", {{6, 0}, {-1, 0}}},
    {"FF FF EB 47 8D 4F FD 6A 01 51 8B CE E8 ?? ?? ?? ??", {{6, 1}, {-1, 0}}},
    {"BF C0 2B C2 83 F8 03 7C 3A 41 66 89 4E 32 EB 33 66", {{6, 0}, {-1, 0}}},
};
bool g_menu_found = false;
int g_menu_rows = kMenuRowsDefault;

// thiscall functions of the menu code, called through __fastcall (the same stack; the unused edx)
using Locate = void(__fastcall*)(void* ctrl, void* edx, short* rect, int id, char a, char b);
using Resize = void(__fastcall*)(void* ctrl, void* edx, int x, int y, int w, int h, char a, char b, char c);
using GetRect = void(__fastcall*)(void* ctrl, void* edx, short* rect);
using Fit = void(__fastcall*)(char* self);
Locate g_locate = nullptr; // the game's own (the trampoline): where row `id` of window `ctrl` is, as left, top, right, bottom
Resize g_resize = nullptr;
GetRect g_get_rect = nullptr;
Fit g_fit = nullptr;
void* g_menu_ctrl = nullptr;   // the window the menu draws in
ULONGLONG g_menu_seen = 0;     // when its rows were last drawn: the cursor and the like ask for rows in the same frames
short g_menu_shift = 0;        // how far the first three rows moved up with the window (they move with it: 0)
short g_menu_extra = 0;        // how much taller the window is than the game's, and so how far its position goes up
int g_menu_want = kMenuRowsDefault; // rows the open menu's window has: the setting, or fewer when the list is shorter
char* g_menu_self = nullptr;   // the menu object, whose copy of the window's rect (+0x540) its title is drawn from
short g_menu_pitch = 16;       // from one row to the next
void* g_grown_ctrl = nullptr;  // the window made taller last, and how tall it was made
short g_grown_height = 0;
constexpr short kUnset = 0x7FFF;

// Whether `ctrl` is a selection menu's window: its layout (+4) is named (at +0x46) "menu    query   ", an NPC's event
// choices, or "menu    gmtell  ", the client's own _CUSTOM_MENU: a menu the server sends as text (CatsEyeXI's warps, its
// Field Manual and Survival Guide choices), shown through a second CTkQueryControl of the game's (0x1015d7f0, from
// 0x100a3e60) whose window is named for the GM tell's layout. Both controls open, draw and step their window with the
// one code (0x1014cb60, 0x1014cf70, patched above), so the rows are theirs alike. g_menu_ctrl is kept after the menu
// closes, and the game frees a closed window and makes every window in a block of the same size (operator new 0xd4), so
// a window opened within the hooks' half second can be made at that address. Taken for the menu, 0.8.55's
// menu_follow_list would make it taller, set its count of buttons and write the rect into its owner at +0x540; the hooks
// below act on the menu's window only when it's still a selection menu's. (The check knew "query" alone at first: a
// custom menu with menu.rows above 3 had its window made taller by the open, and its rows past the third, which the
// position hook wouldn't place, drawn on the third: the EXP Guide addon, 2026-10-25.)
constexpr char kQueryName[] = "menu    query   ";
constexpr char kCustomName[] = "menu    gmtell  ";

// The layout (+4) of window `ctrl`, or null for no window, no layout, or one that can't be read
const char* layout_of(const void* ctrl)
{
    if (ctrl == nullptr)
        return nullptr;
    __try {
        const char* layout = *reinterpret_cast<const char* const*>(static_cast<const char*>(ctrl) + 4);
        return layout != nullptr && layout[0x46] != 0 ? layout : nullptr;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

bool is_query(const void* ctrl)
{
    const char* layout = layout_of(ctrl);
    return layout != nullptr && (std::memcmp(layout + 0x46, kQueryName, 16) == 0 || std::memcmp(layout + 0x46, kCustomName, 16) == 0);
}

// The height the game makes the window `ctrl`: its layout's own rect (+0x10: left, top, right, bottom, in interface
// units; the gmtell layout's is 16,208-382,296, 88 tall with rows 20 apart), which the game's placing of the window and
// VanaCore's resize both leave alone. 0.8.55.46 took the shortest window it had seen as the game's height, and a window
// the game gives a new question (CreateDrawMenu takes the last one's, which can be closing, its frame shrinking) could
// leave that short for the rest of the session: every window then counted as taller than its list wanted, none was made
// taller, and the rows past the third were drawn under the window's bottom edge, over the chat (Lunexi's Field Manual and
// Survival Guide, 2026-10-25). 0 without a layout, or a height that can't be a menu's
int base_height_of(const void* ctrl)
{
    const char* layout = layout_of(ctrl);
    if (layout == nullptr)
        return 0;
    __try {
        const int height = *reinterpret_cast<const short*>(layout + 0x16) - *reinterpret_cast<const short*>(layout + 0x12);
        return height >= 40 && height <= 400 ? height : 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

// `ctrl` is the window the menu draws in, drawn in the last half second, and a selection menu's window still
bool menu_window(const void* ctrl)
{
    return ctrl != nullptr && ctrl == g_menu_ctrl && GetTickCount64() - g_menu_seen < 500 && is_query(ctrl);
}

// `rect` as the game puts it for row `id`, or as it was before the call if the window has no such row
bool g_in_locate = false; // locate_raw running: the button lookup answers as the game's own does

bool locate_raw(void* ctrl, void* edx, short* rect, int id, char a, char b)
{
    short saved[4] = {rect[0], rect[1], rect[2], rect[3]};
    rect[0] = rect[1] = rect[2] = rect[3] = kUnset;
    g_in_locate = true;
    g_locate(ctrl, edx, rect, id, a, b);
    g_in_locate = false;
    if (rect[0] != kUnset || rect[1] != kUnset)
        return true;
    std::memcpy(rect, saved, sizeof saved);
    return false;
}

// The game's lookup of a window's button by its id (0 when it has none). The menu's window has the three its resource
// gives it, and the cursor (0x101189b0) stays where it is when the button it's asked to go to isn't there: ids past the
// third answer with the third (its state is only the highlight), so the cursor can go down the rows.
using FindButton = void*(__fastcall*)(void* ctrl, void* edx, int id);
FindButton g_find_button = nullptr;

void* __fastcall find_button_hook(void* ctrl, void* edx, int pushed)
{
    // a short pushed as a dword: the mouse's SetCursol call leaves a pointer's high half above it
    const short id = static_cast<short>(pushed);
    void* found = g_find_button(ctrl, edx, pushed);
    if (found == nullptr && !g_in_locate && id > kMenuRowsDefault && id <= g_menu_want && menu_window(ctrl))
        found = g_find_button(ctrl, edx, kMenuRowsDefault);
    return found;
}

// The mouse's pick of a window's button (0x101188b0, IsOnButton: the button under the point, by each button's place, or
// -1). The menu's window has three, so a point on a row past the third is asked again moved up by the rows between: if
// the third is there, it's that row.
using OnButton = short(__fastcall*)(void* ctrl, void* edx, const int* point, char* enabled);
OnButton g_on_button = nullptr;

short __fastcall on_button_hook(void* ctrl, void* edx, const int* point, char* enabled)
{
    int mapped[2];
    if (buffwin::map_point(ctrl, point, mapped)) // buffs.scale: the status icons are drawn bigger or smaller than laid out
        return g_on_button(ctrl, edx, mapped, enabled);
    const short found = g_on_button(ctrl, edx, point, enabled);
    if (found != -1 || point == nullptr || g_menu_want <= kMenuRowsDefault || !menu_window(ctrl))
        return found;
    for (int id = kMenuRowsDefault + 1; id <= g_menu_want; ++id) {
        const int moved[2] = {point[0], point[1] - (id - kMenuRowsDefault) * g_menu_pitch};
        if (g_on_button(ctrl, edx, moved, enabled) == kMenuRowsDefault)
            return static_cast<short>(id);
    }
    return found;
}

// Where the arrow keys take the cursor: each of a menu's buttons has the button Up, Down, Left and Right go to in its
// resource (0x1011ae20, on the window's menu data). Button 3's Down is itself (the list scrolls instead), and buttons past
// the third have none. For the menu's window, Up and Down step one row while there's a row to step to; which key is
// which comes from button 2's own (Down goes to 3, Up to 1).
using JumpTag = short(__fastcall*)(void* data, void* edx, int cursor, int key);
JumpTag g_jump_tag = nullptr;

// Whether `data` is the menu data of the window the menu draws in. g_menu_ctrl is kept after the menu closes and its
// window is freed, so it's only read while it was drawn lately, and a fault reads as no.
bool is_menu_data(const void* data)
{
    if (!menu_window(g_menu_ctrl))
        return false;
    __try {
        return data == *reinterpret_cast<void* const*>(static_cast<const char*>(g_menu_ctrl) + 4);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

short __fastcall jump_tag_hook(void* data, void* edx, int cursor, int key)
{
    short rows_result = 0;
    if (itemlist::jump_tag(data, cursor, key, g_jump_tag, rows_result)) { // a list VanaCore gave rows past the game's ten
        menulog::key_step(data, cursor, key, rows_result, "VanaCore's list rows, items.rows");
        return rows_result;
    }
    const auto from = static_cast<short>(cursor);
    if (g_menu_want > kMenuRowsDefault && from >= 1 && from <= g_menu_want && is_menu_data(data)) {
        const short probe = g_jump_tag(data, edx, 2, key);
        const int step = probe == 3 ? 1 : probe == 1 ? -1 : 0;
        if (step != 0) {
            const int to = from + step;
            const auto result = static_cast<short>(to < 1 || to > g_menu_want ? from : to); // the edge: the list scrolls
            menulog::key_step(data, cursor, key, result, "VanaCore's selection menu rows, menu.rows");
            return result;
        }
        if (from > kMenuRowsDefault) {
            menulog::key_step(data, cursor, key, from, "VanaCore's selection menu rows, menu.rows: Left and Right stay");
            return from; // Left and Right stay where they are
        }
    }
    const short result = g_jump_tag(data, edx, cursor, key);
    menulog::key_step(data, cursor, key, result, "the game");
    return result;
}

// The cursor's arrow takes its place from the button it's on (0x10119a30: the button's x, y plus the arrow's own offset).
// For a row past the third the button is the third (find_button_hook), so the arrow goes down the rows between.
using PlaceCursor = void(__fastcall*)(void* cursor, void* edx, void* button);
PlaceCursor g_place_cursor = nullptr;
// The game's re-placing of a window's cursor on button `id` (0x10117d70, which SetCursol calls): done each frame the
// menu's cursor is past the third row, as coming back to the menu (from one it opened) places it before a frame is drawn
using Reposition = void(__fastcall*)(void* ctrl, void* edx, int id);
Reposition g_reposition = nullptr;

void __fastcall place_cursor_hook(void* cursor, void* edx, void* button)
{
    g_place_cursor(cursor, edx, button);
    if (button == nullptr || g_menu_want <= kMenuRowsDefault || !menu_window(g_menu_ctrl))
        return;
    const short id = *reinterpret_cast<const short*>(static_cast<const char*>(g_menu_ctrl) + 0x4C);
    if (id <= kMenuRowsDefault || id > g_menu_want || button != g_find_button(g_menu_ctrl, nullptr, kMenuRowsDefault))
        return;
    *reinterpret_cast<short*>(static_cast<char*>(cursor) + 0x22) += static_cast<short>((id - kMenuRowsDefault) * g_menu_pitch);
}

void __fastcall locate_hook(void* ctrl, void* edx, short* rect, int id, char a, char b)
{
    id = static_cast<short>(id); // a short pushed as a dword
    if (ctrl == nullptr || ctrl != g_menu_ctrl || g_menu_rows <= kMenuRowsDefault || GetTickCount64() - g_menu_seen > 250 ||
        !is_query(ctrl)) {
        g_locate(ctrl, edx, rect, id, a, b);
        return;
    }
    bool found = locate_raw(ctrl, edx, rect, id, a, b);
    if (!found && id > kMenuRowsDefault && id <= g_menu_rows)
        if (locate_raw(ctrl, edx, rect, kMenuRowsDefault, 0, 0)) { // the third row unselected
            short down = static_cast<short>((id - kMenuRowsDefault) * g_menu_pitch);
            short across = 0;
            // the selected row's nudge, as the game's own (0x10118500) gives its current button (none with
            // buttons.selected.nudge=0, which takes it out of the game's)
            const auto* window = static_cast<const char*>(ctrl);
            if (a != 0 && *reinterpret_cast<const short*>(window + 0x4C) == id && window[0x63] != 0 && buttons::nudging()) {
                const void* third = g_find_button(ctrl, edx, kMenuRowsDefault);
                const int type = third != nullptr ? *reinterpret_cast<const int*>(static_cast<const char*>(third) + 0x10) : 0;
                int nudge = type == 5 ? 1 : -1;
                if (type != 5 && b != 0 && *reinterpret_cast<const short*>(window + 0x5E) > 0)
                    nudge += 2;
                across = static_cast<short>(nudge);
                down += static_cast<short>(nudge);
            }
            rect[0] += across;
            rect[2] += across;
            rect[1] += down;
            rect[3] += down;
            found = true;
        }
    if (found && id >= 1 && id <= g_menu_rows) {
        rect[1] += g_menu_shift;
        rect[3] += g_menu_shift;
    }
}

void raise_buttons(void* ctrl);
char* handler_of(void* ctrl);

// The draw loop's call to the position routine
void __fastcall draw_locate(void* ctrl, void* edx, short* rect, int id, char a, char b)
{
    id = static_cast<short>(id);
    g_menu_ctrl = ctrl;
    g_menu_seen = GetTickCount64();
    char* self = id == 1 && g_menu_rows > kMenuRowsDefault && ctrl == g_grown_ctrl ? handler_of(ctrl) : nullptr;
    if (self != nullptr) {
        // the title follows the window; its own menu object's copy of the rect (g_menu_self is the one kept from the
        // last fitting, which may be a menu since closed and freed)
        g_get_rect(ctrl, nullptr, reinterpret_cast<short*>(self + 0x540));
        raise_buttons(ctrl);
        const short cursor = *reinterpret_cast<const short*>(static_cast<const char*>(ctrl) + 0x4C);
        if (cursor > kMenuRowsDefault && cursor <= g_menu_want && g_reposition != nullptr)
            g_reposition(ctrl, nullptr, cursor); // place_cursor_hook puts the arrow on its row
    }
    locate_hook(ctrl, edx, rect, id, a, b);
}

// The window's count of buttons (the cursor's limit: 0x101189b0 clamps to it) and its scroll bar's rows shown, up to the
// rows the menu shows. Done each frame too, as the game puts the cursor back where it was (coming back from a menu it
// opened) before a frame is drawn.
void raise_buttons(void* ctrl)
{
    auto* count = reinterpret_cast<short*>(static_cast<char*>(ctrl) + 0x58);
    if (*count < g_menu_want)
        *count = static_cast<short>(g_menu_want);
    if (void* bar = *reinterpret_cast<void**>(static_cast<char*>(ctrl) + 0x88))
        *reinterpret_cast<int*>(static_cast<char*>(bar) + 0xC) = g_menu_want;
}

// The menu object (CTkQueryControl in the PS2 client's names) of the menu's window `ctrl`: the window's +0xC, whose own
// +8 is the window. Null for a window that isn't a menu's
char* handler_of(void* ctrl)
{
    __try {
        char* self = *reinterpret_cast<char**>(static_cast<char*>(ctrl) + 0xC);
        return self != nullptr && *reinterpret_cast<void**>(self + 8) == ctrl ? self : nullptr;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

// How many rows the menu `self` wants its window to show for the options it has now (+0x24): the setting, or fewer when
// the list is shorter, never fewer than the game's three; the setting while the list isn't there yet (0 options)
int rows_wanted(const char* self)
{
    const int total = *reinterpret_cast<const int*>(self + 0x24);
    const int want = total > 0 && total < g_menu_rows ? total : g_menu_rows;
    return want < kMenuRowsDefault ? kMenuRowsDefault : want;
}

// How many rows the menu's window `ctrl` has: the game's three, and one more for each row pitch it stands taller than the
// window the game makes (base_height_of)
int rows_of(void* ctrl)
{
    short rect[4] = {};
    g_get_rect(ctrl, nullptr, rect);
    const int height = rect[3] - rect[1], base = base_height_of(ctrl);
    if (base <= 0 || g_menu_pitch <= 0 || height <= base)
        return kMenuRowsDefault;
    return kMenuRowsDefault + (height - base) / g_menu_pitch;
}

// The menu `self`'s window `ctrl`, which has `have` rows, given `want`: made the game's height and a row pitch more for
// each row past the third (taller or shorter than it is: a window taken from a closing menu can be any height between),
// upward, with the game's own resize, which stores the size in the window's frame (flags 1, 0, 0: as its position routine
// does each frame, from that stored size); the game's own placing of the window uses the height it has, so its bottom
// stays at the chat's top. The window's count of buttons (the cursor's limit) and its scroll bar's rows shown follow, and
// the menu's copy of the window's rect (+0x540), which its title is drawn from
void fit_rows(char* self, void* ctrl, int have, int want)
{
    short rect[4] = {};
    g_get_rect(ctrl, nullptr, rect);
    if (rect[2] <= rect[0] || rect[3] <= rect[1] || want == have)
        return;
    short first[4] = {}, second[4] = {};
    // the row pitch from the game's first two rows, unselected (flags 0): the selected row is nudged, which would put a
    // unit in the pitch
    if (locate_raw(ctrl, nullptr, first, 1, 0, 0) && locate_raw(ctrl, nullptr, second, 2, 0, 0)) {
        const int pitch = second[1] - first[1];
        if (pitch >= 4 && pitch <= 100)
            g_menu_pitch = static_cast<short>(pitch);
    }
    const int base = base_height_of(ctrl);
    // the height for `want` rows: the game's and the rows past the third; by the rows' difference without a layout to say
    const int height = base > 0 ? base + (want - kMenuRowsDefault) * g_menu_pitch : rect[3] - rect[1] + (want - have) * g_menu_pitch;
    const int extra = height - (rect[3] - rect[1]);
    g_menu_ctrl = ctrl;
    g_menu_self = self;
    g_menu_want = want;
    g_menu_extra = static_cast<short>((want - kMenuRowsDefault) * g_menu_pitch);
    g_menu_seen = GetTickCount64();
    *reinterpret_cast<short*>(static_cast<char*>(ctrl) + 0x58) = static_cast<short>(want);
    if (void* bar = *reinterpret_cast<void**>(static_cast<char*>(ctrl) + 0x88))
        *reinterpret_cast<int*>(static_cast<char*>(bar) + 0xC) = want;
    g_resize(ctrl, nullptr, rect[0], rect[3] - height, rect[2] - rect[0], height, 1, 0, 0);
    short grown[4] = {};
    g_get_rect(ctrl, nullptr, grown);
    std::memcpy(self + 0x540, grown, sizeof grown); // what the title is drawn from
    g_menu_shift = 0;
    g_grown_ctrl = ctrl;
    g_grown_height = static_cast<short>(grown[3] - grown[1]);
    log::write(L"Chat: selection menu made %d units %ls, to %d rows for %d options, rows %d apart; window was %d,%d-%d,%d, now %d,%d-%d,%d",
               extra < 0 ? -extra : extra, extra < 0 ? L"shorter" : L"taller", want, *reinterpret_cast<const int*>(self + 0x24),
               g_menu_pitch, rect[0], rect[1], rect[2], rect[3], grown[0], grown[1], grown[2], grown[3]);
}

// The menu's open routine's call to the window-fitting routine (CTkQueryControl::OnInitialUpdatePrimitive's, in the PS2
// client's names). It runs as the window's primitive is made, and again for a window that's up: when the chat log it sits
// above moves (FancyChat pins the log closed, the player opens and closes it: the window is placed again by the height it
// has), and when the game gives a new question the last one's window (CreateDrawMenu, 0x1015d080, takes the window of
// the name that's still there, one closing, resets its primitive and sets it up again). The window is fitted to the list
// it has now, whatever it was fitted to before; growing a window that's already as tall as its list wants would add the
// rows twice
void __fastcall menu_opened(char* self)
{
    g_fit(self);
    if (g_menu_rows <= kMenuRowsDefault)
        return;
    void* ctrl = *reinterpret_cast<void**>(self + 8);
    if (ctrl == nullptr)
        return;
    short rect[4] = {};
    g_get_rect(ctrl, nullptr, rect);
    if (rect[2] <= rect[0] || rect[3] <= rect[1])
        return;
    const int height = rect[3] - rect[1];
    const int have = rows_of(ctrl), want = rows_wanted(self);
    if (menulog::on()) {
        char text[192];
        std::snprintf(text, sizeof text, "a selection menu set up in @%08X (%.8s): %d options, cursor %d, %d rows (%d units tall, "
                      "the game's %d), %d wanted", static_cast<unsigned>(reinterpret_cast<std::uintptr_t>(ctrl)),
                      layout_of(ctrl) != nullptr ? layout_of(ctrl) + 0x46 + 8 : "?", *reinterpret_cast<const int*>(self + 0x24),
                      *reinterpret_cast<const short*>(static_cast<char*>(ctrl) + 0x4C), have, height, base_height_of(ctrl), want);
        menulog::note(text);
    }
    if (want != have) {
        fit_rows(self, ctrl, have, want);
        return;
    }
    if (have > kMenuRowsDefault) { // made taller already, as its list wants: kept
        g_grown_ctrl = ctrl;
        g_grown_height = static_cast<short>(height);
        g_menu_ctrl = ctrl;
        g_menu_self = self;
        g_menu_want = have;
        g_menu_seen = GetTickCount64();
        raise_buttons(ctrl);
    }
}

// Each frame, before the menu's window is drawn: the window made taller when its list has grown past its rows. The
// options can come after the window (the game makes the window, then fills the list), and a new question given the last
// one's window (menu_opened) found the last question's list in it. With the window short of its list, the game, told the
// setting's rows a page, moved its cursor down rows the window hadn't: the cursor sound played, the highlight stayed on
// the window's last row, and the options below were out of reach (the stuck NPC menus reported 2026-10-24: a longer list
// after a shorter one, with menu.rows above 3). A window taller than its list is made shorter the same way: a custom menu's
// options come after its window, which the open made the setting's rows tall (ten rows of box for six destinations, the
// EXP Guide addon, 2026-10-25)
void menu_follow_list(void* ctrl)
{
    if (g_menu_rows <= kMenuRowsDefault || !menu_window(ctrl))
        return;
    char* self = handler_of(ctrl);
    if (self == nullptr)
        return;
    const int have = rows_of(ctrl), want = rows_wanted(self);
    if (want != have)
        fit_rows(self, ctrl, have, want);
}

void write_code(std::uintptr_t at, const void* bytes, size_t size)
{
    DWORD old = 0;
    if (!VirtualProtect(reinterpret_cast<void*>(at), size, PAGE_EXECUTE_READWRITE, &old))
        return;
    std::memcpy(reinterpret_cast<void*>(at), bytes, size);
    VirtualProtect(reinterpret_cast<void*>(at), size, old, &old);
    FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(at), size);
}

// The target of the `call rel32` at `call`
std::uintptr_t call_target(std::uintptr_t call)
{
    std::int32_t rel = 0;
    std::memcpy(&rel, reinterpret_cast<const void*>(call + 1), 4);
    return call + 5 + rel;
}

// Points the `call rel32` at `call` to `to`
void redirect_call(std::uintptr_t call, void* to)
{
    const auto rel = static_cast<std::int32_t>(reinterpret_cast<std::uintptr_t>(to) - (call + 5));
    write_code(call + 1, &rel, 4);
}

void apply_menu_rows(int rows)
{
    if (!g_menu_found || rows == g_menu_rows)
        return;
    for (const MenuSite& site : g_menu_sites)
        for (const MenuPatch& patch : site.patches) {
            if (patch.offset < 0)
                continue;
            const int value = patch.minus == 0 ? rows : patch.minus == 1 ? -rows : -(rows - 1);
            const auto byte = static_cast<std::uint8_t>(value);
            write_code(site.at + patch.offset, &byte, 1);
        }
    g_menu_rows = rows;
    g_grown_ctrl = nullptr; // the next menu to open is made to the new size
    g_grown_height = 0;
    log::write(L"Chat: selection menus show %d options ([client] menu.rows)", rows);
}

// [client] chat.move=1: the chat window (log 1) where chat.x and chat.y put its left and bottom edges and chat.width says,
// each a percent of the game's UI area; chat2.* the same for the second one (log 2). A chat window object
// (CTkDrawMessageWindow; log 1's is held at 0x105ecc9c, log 2's at 0x105ecca0) has its window (CTkMenuCtrlData) at +8, null
// while it isn't shown, whose rectangle is +0x3a left, +0x3c top, +0x3e right, +0x40 bottom; its text width is +0x32 (the
// game's: the UI's width less 160, at most +0x52, less with two logs), the window 14 wider. The game's resize (0x1011a1a0:
// x, y, w, h, 1, 0, 0) stores the place in the window's frame, which the game puts the window back to every frame, and the
// chat's growing (0x10161490) keeps its left and bottom: so it stays. The chat's layout (0x10161850: when it's set up, and
// the Config log settings) puts it back where the game's layout has it, so that's hooked and the place set again after it.
// And log 2 is put on log 1 every frame (0x101607b8: above it, or beside it, as Config's option 0xb4 says), through a call
// of the window's SetLocate (0x10118d10) that's skipped while log 2 is moved.
using ChatLayout = void(__fastcall*)(char* window, void* edx);
using SetLocate = void(__fastcall*)(void* ctrl, void* edx, int x, int y);
using ConfigValue = int(__cdecl*)(int id);
ChatLayout g_chat_layout = nullptr; // the game's own, through the trampoline
Resize g_chat_resize = nullptr;
SetLocate g_set_locate = nullptr;
ConfigValue g_config_value = nullptr; // the game's Config option `id` (FUN_10192310)
const short* g_ui_origin = nullptr;   // the UI area's left and top (0x105ecc14)
const short* g_ui_end = nullptr;      // its right and bottom (0x105ecc18)
struct ChatLog {
    char** global;  // where the game holds its window object
    bool move;
    double x, y, width; // [client] chat.x, chat.y, chat.width (chat2.* for log 2), percent
};
ChatLog g_logs[2] = {{nullptr, false, 0.5, 99.0, 95.0}, {nullptr, false, 0.5, 85.0, 45.0}};
// While fancychat stands in for the game's chat (loaded, its Show with legacy chat off: kFancyChatAsk's 8), chat.move and
// chat2.move rest and the logs keep the game's own place. fancychat takes log 1 for opened by the player (and hides its
// own chat, while its Show with legacy chat is off) whenever that log's top (+0x42) is over the UI's bottom less 34, which a
// log moved up is even closed: reported in game 2026-11-10, chat.y=96.67 put it at 1121 of 1163, fancychat's line 1129,
// so fancychat's chat was never drawn. Its logs are kept closed and unseen meanwhile; the move comes back when it goes.
bool g_place_rests = false;
bool log_moved(int index) { return g_logs[index].move && !g_place_rests; }

// Log `index`'s window (CTkMenuCtrlData), or null (not shown, or +8 still pointing at memory another window has now: a
// window's +0xc is the object it belongs to)
char* chat_ctrl(int index, char** window_out = nullptr)
{
    char* window = g_logs[index].global != nullptr ? *g_logs[index].global : nullptr;
    if (window_out != nullptr)
        *window_out = window;
    char* ctrl = window != nullptr ? *reinterpret_cast<char**>(window + 8) : nullptr;
    return ctrl != nullptr && *reinterpret_cast<char**>(ctrl + 0xc) == window ? ctrl : nullptr;
}

void place_chat(int index)
{
    if (g_chat_resize == nullptr)
        return;
    const ChatLog& log = g_logs[index];
    __try {
        char* window = nullptr;
        char* ctrl = chat_ctrl(index, &window);
        if (ctrl == nullptr)
            return;
        const int ui_left = g_ui_origin[0], ui_top = g_ui_origin[1];
        const int ui_width = g_ui_end[0] - ui_left, ui_height = g_ui_end[1] - ui_top;
        if (ui_width <= 0 || ui_height <= 0)
            return;
        const short* rect = reinterpret_cast<const short*>(ctrl + 0x3a);
        const int height = rect[3] - rect[1];
        // as wide as the game makes one log alone (two share the width otherwise)
        const int game_widest = *reinterpret_cast<const short*>(window + 0x52);
        const int widest = ui_width - 160 > game_widest ? ui_width - 160 : game_widest;
        int text = static_cast<int>(ui_width * log.width / 100.0 + 0.5) - 14;
        text = text > widest ? widest : text < 160 ? 160 : text;
        int x = ui_left + static_cast<int>(ui_width * log.x / 100.0 + 0.5);
        x = x > ui_left + ui_width - (text + 14) ? ui_left + ui_width - (text + 14) : x < ui_left ? ui_left : x;
        int bottom = ui_top + static_cast<int>(ui_height * log.y / 100.0 + 0.5);
        bottom = bottom > ui_top + ui_height ? ui_top + ui_height : bottom < ui_top + 32 ? ui_top + 32 : bottom;
        if (rect[0] == x && rect[3] == bottom && rect[2] - rect[0] == text + 14 &&
            *reinterpret_cast<const short*>(window + 0x32) == text)
            return;
        *reinterpret_cast<short*>(window + 0x32) = static_cast<short>(text);
        g_chat_resize(ctrl, nullptr, x, bottom - height, text + 14, height, 1, 0, 0);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

void __fastcall chat_layout_hook(char* window, void* edx)
{
    g_chat_layout(window, edx);
    for (int index = 0; index < 2; ++index)
        if (log_moved(index) && g_logs[index].global != nullptr && window == *g_logs[index].global)
            place_chat(index);
}

// log 2's every-frame place on log 1, left out while log 2 is moved
void __fastcall log2_follow(void* ctrl, void* edx, int x, int y)
{
    if (!log_moved(1))
        g_set_locate(ctrl, edx, x, y);
}

// The log the game stands the compass and the windows over the chat on (0x1015e6b0): log 2 when it's shown, unless Config
// puts it beside log 1 (option 0xb4 = 2)
int top_log()
{
    if (chat_ctrl(1) != nullptr && (g_config_value == nullptr || g_config_value(0xB4) != 2))
        return 1;
    return 0;
}

// A place given as percents of the UI area (x across, y down) in the UI's pixels; false while the UI area isn't known
bool ui_point(double x_percent, double y_percent, int& x, int& y)
{
    if (g_ui_origin == nullptr || g_ui_end == nullptr)
        return false;
    const int width = g_ui_end[0] - g_ui_origin[0], height = g_ui_end[1] - g_ui_origin[1];
    if (width <= 0 || height <= 0)
        return false;
    x = g_ui_origin[0] + static_cast<int>(width * x_percent / 100.0 + 0.5);
    y = g_ui_origin[1] + static_cast<int>(height * y_percent / 100.0 + 0.5);
    return true;
}

// The compass and clock (KaCompass, held at 0x106321e4): the game keeps its bottom (+0x2a) on the top log's top (the layout,
// 0x1015e730, through 0x1021d4e0, when that top moves); its left (+0x28) is a constant (88) the layout sets now and then
// (0x1021d4c0). Both setters are hooked, so what the game sets is known and ours goes in its place at once (no frame drawn
// with the game's), and each frame puts ours there too.
// [client] chat.compass=1: kept as far right of the top log's left as the game's is of that log's own place in the layout
// (the window's first rectangle, +0x42). compass.move=1: where compass.x (its left) and compass.y (its bottom) say, percents
// of the UI area, whatever the chat does.
using CompassSet = void(__cdecl*)(int value);
CompassSet g_compass_set_left = nullptr;   // the game's setters, through the trampolines
CompassSet g_compass_set_bottom = nullptr;
char** g_compass = nullptr;
bool g_compass_on = false;      // chat.compass
bool g_compass_custom = false;  // compass.move
double g_compass_px = 3.0, g_compass_py = 90.0; // compass.x, compass.y
bool g_compass_known = false;   // the game's values below are known
short g_compass_game_x = 0, g_compass_game_bottom = 0;
short g_compass_follow = 0;     // chat.compass: how far right of the game's left
short g_compass_x = 0, g_compass_bottom = 0; // compass.move: ours, in the UI's pixels
bool g_compass_touched = false; // the last frame put ours there
// [client] compass.hide=1: the compass not drawn, by the byte Arielfy's FancyCompass found (and UIBegone patches after
// it), 0x24 bytes past `xor eax,eax; mov ax,[ecx+..]; dec eax; cmp eax,0E3h`: 1 as the game has it, 0 and the compass
// isn't drawn. What the code around it does isn't worked out here, so it's only written while it reads what VanaCore
// left there (1, or the 0 it wrote), and put back to 1 when the setting goes off. Whether the clock (KaCompass draws
// both) goes with it isn't known.
std::uintptr_t g_compass_switch = 0; // the byte
bool g_compass_hide = false;         // compass.hide
bool g_compass_hidden = false;       // the byte is 0, put there by VanaCore

void apply_compass_hide()
{
    if (g_compass_switch == 0 || g_compass_hide == g_compass_hidden)
        return;
    const std::uint8_t now = *reinterpret_cast<const volatile std::uint8_t*>(g_compass_switch);
    if (now != (g_compass_hidden ? 0 : 1)) { // something else changed it (FancyCompass): left to it
        log::write(L"Chat: the compass's switch reads %u, not %u: [client] compass.hide leaves it alone", now,
                   g_compass_hidden ? 0u : 1u);
        g_compass_switch = 0;
        return;
    }
    const std::uint8_t value = g_compass_hide ? 0 : 1;
    write_code(g_compass_switch, &value, 1);
    g_compass_hidden = g_compass_hide;
}

short compass_left(short game)
{
    return g_compass_custom ? g_compass_x : g_compass_on ? static_cast<short>(game + g_compass_follow) : game;
}

short compass_bottom(short game)
{
    return g_compass_custom ? g_compass_bottom : game;
}

void __cdecl compass_left_hook(int value)
{
    g_compass_game_x = static_cast<short>(value);
    g_compass_set_left(compass_left(g_compass_game_x));
}

void __cdecl compass_bottom_hook(int value)
{
    g_compass_game_bottom = static_cast<short>(value);
    g_compass_set_bottom(compass_bottom(g_compass_game_bottom));
}

void place_compass()
{
    __try {
        char* compass = g_compass != nullptr ? *g_compass : nullptr;
        if (compass == nullptr)
            return;
        const bool ours = g_compass_on || g_compass_custom;
        if (!ours && !g_compass_touched) {
            g_compass_known = false; // the game's own: read again when one's turned on
            return;
        }
        g_compass_touched = ours; // turned off: this frame puts the game's back
        auto* x = reinterpret_cast<short*>(compass + 0x28);
        auto* bottom = reinterpret_cast<short*>(compass + 0x2a);
        if (!g_compass_known) { // as the game has them, before anything of ours
            g_compass_game_x = *x;
            g_compass_game_bottom = *bottom;
            g_compass_known = true;
        }
        g_compass_follow = 0;
        if (g_compass_on)
            if (char* ctrl = chat_ctrl(top_log()))
                g_compass_follow = static_cast<short>(*reinterpret_cast<const short*>(ctrl + 0x3a) - *reinterpret_cast<const short*>(ctrl + 0x42));
        int px = 0, py = 0;
        if (g_compass_custom && ui_point(g_compass_px, g_compass_py, px, py))
            g_compass_x = static_cast<short>(px), g_compass_bottom = static_cast<short>(py);
        *x = compass_left(g_compass_game_x);
        *bottom = compass_bottom(g_compass_game_bottom);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

// The clock under the compass: KaCompass's DrawTime (0x1021e8b0, PS2 KaCompass::DrawTime; __thiscall, x and y, ret 8),
// called by its draw with x the compass's middle (+0x28) and y its top (+0x2a less +0x2c). While the compass's +0x2e is set
// it draws the Vana'diel time and position ("%d:%02d  %s") with YkDrawString at x - 0x32, y + 0x1a, the day of the week's
// icon at x - 0x44 beside it, and the weather's icon at x - 0x44, y - 0x2a above.
// [client] clock.hide=1: not drawn. clock.move=1: the line put at clock.x and clock.y (percents of the UI: X its left edge,
// middle or right edge by clock.align, left, centre or right; Y its top), the weather's icon with it.
using ClockDraw = void(__fastcall*)(void* compass, void* edx, int x, int y);
using DrawString = int(__cdecl*)(int x, int y, const char* text, unsigned color);
ClockDraw g_clock_draw = nullptr;   // the game's, through the trampoline
DrawString g_draw_string = nullptr; // YkDrawString (0x101c7ee0): returns where the text ends
bool g_clock_hide = false;          // clock.hide
bool g_clock_move = false;          // clock.move
int g_clock_align = 0;              // clock.align: 0 left, 1 centre, 2 right
double g_clock_px = 3.0, g_clock_py = 92.0; // clock.x, clock.y
int g_clock_text_w = 64;            // the text's width as last drawn (the game's units)
constexpr int kClockIcon = 0x44;    // the day's icon: this far left of x; the text 0x32
constexpr int kClockText = 0x32;
constexpr int kClockDown = 0x1a;    // the line: this far below y
constexpr int kClockHigh = 16;      // and about this high

// the line's left and top, where clock.x/y put it, from the width it last had
bool clock_place(int& left, int& top)
{
    int x = 0, y = 0;
    if (!ui_point(g_clock_px, g_clock_py, x, y))
        return false;
    const int wide = kClockIcon - kClockText + g_clock_text_w;
    left = g_clock_align == 2 ? x - wide : g_clock_align == 1 ? x - wide / 2 : x;
    top = y;
    return true;
}

int __cdecl clock_text(int x, int y, const char* text, unsigned color)
{
    const int end = g_draw_string(x, y, text, color);
    const int wide = static_cast<short>(end) - x;
    if (wide > 0 && wide < 1000)
        g_clock_text_w = wide;
    return end;
}

void __fastcall clock_draw_hook(void* compass, void* edx, int x, int y)
{
    if (g_clock_hide)
        return;
    int left = 0, top = 0;
    if (g_clock_move && clock_place(left, top))
        x = left + kClockIcon, y = top - kClockDown;
    vanacore_gfx_clock_drawing(1); // True Font: its letters the clock's (clock.font)
    g_clock_draw(compass, edx, x, y);
    vanacore_gfx_clock_drawing(0);
}

#include "chat_clock.h" // keep_clock, install_clock_always: clock.always, the clock kept while a menu is up

// The chat line Enter opens (vtable 0x103358f8, held at 0x1062e0f8): its window at +8 while it's open, made again each time
// it opens at the game's place. The window is only touched while it's the line's own (its +0xc, the object it belongs to, is
// the line): +8 can still point at the closed line's window after the game has given that memory to another window.
// [client] input.move=1: its left at input.x and its bottom at input.y (percents of the UI area), put there each frame it's
// somewhere else, and before its window's draw (ctrl_draw_hook), so the frame it opens isn't drawn at the game's place
// first; its width stays the game's.
char** g_input_line = nullptr;
bool g_input_move = false;
double g_input_x = 0.5, g_input_y = 99.0;

void place_input(bool game)
{
    if (g_input_line == nullptr || g_chat_resize == nullptr)
        return;
    __try {
        char* line = *g_input_line;
        char* ctrl = line != nullptr ? *reinterpret_cast<char**>(line + 8) : nullptr;
        if (ctrl == nullptr || *reinterpret_cast<char**>(ctrl + 0xc) != line)
            return;
        const short* rect = reinterpret_cast<const short*>(ctrl + 0x3a);
        const int width = rect[2] - rect[0], height = rect[3] - rect[1];
        int x = 0, bottom = 0;
        if (game) { // the layout's place (the window's first rectangle)
            x = *reinterpret_cast<const short*>(ctrl + 0x42);
            bottom = *reinterpret_cast<const short*>(ctrl + 0x48);
        } else if (!ui_point(g_input_x, g_input_y, x, bottom)) {
            return;
        }
        const int right = g_ui_end[0], lowest = g_ui_end[1];
        x = x > right - width ? right - width : x < g_ui_origin[0] ? g_ui_origin[0] : x;
        bottom = bottom > lowest ? lowest : bottom < g_ui_origin[1] + height ? g_ui_origin[1] + height : bottom;
        if (rect[0] == x && rect[3] == bottom)
            return;
        g_chat_resize(ctrl, nullptr, x, bottom - height, width, height, 1, 0, 0);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

// [client] input.hide=1: the chat line's window (its frame, Say and what's typed) is kept off the screen, for an addon
// that shows the line its own way (fancychat's input bar). The game draws each window in CTkMenuCtrlData::OnDraw
// (0x101173d0: the frame and buttons, then the object's own drawing); it's detoured, and the line's window (while it's the
// line's own, as place_input checks) is put at kHiddenLeft, off the left of the screen, before its draw: the game makes
// the window again at its place each time the line opens, and this runs before that first draw, so it's never seen. The
// draw itself goes on: the line's object lays its text out in it (the display copy at +0x7E30+0x6E04 of the chat manager,
// which Ashita's GetInputTextDisplay reads and fancychat's bar shows), so a draw that's skipped instead leaves that copy
// stale and the bar empty. Typing goes on as before. Off again, the window goes back where the game or input.move has it.
bool g_input_hide = false;
constexpr int kHiddenLeft = -4000; // a short, well off the left of any UI area
// fancychat's input bar (its Chat Window > Enable input chat bar) hides the line by itself, with nothing to set in
// VanaCore, as it needs nothing in Ashita: the setting is read from the addon's own state (the table its lib/state.lua
// holds) twice a second, with whether it keeps the game's chat windows closed (below) and whether its Prevent obstructing
// FFXI UI is on (menus.ontop, below). The bar is a mirror of the line, so the line is still typed into and laid out.
bool g_input_hide_addon = false;
// An addon's hide of the chat line or the chat log (vana.gameui.hide('chat_input'), ('chat_log'),
// gameui.hpp) is asked of gameui at the draw and the calc themselves, never cached: the line's window is made again at
// its place each time the line opens and drawn in the same game frame, and on_frame runs after that frame (at Present),
// so a hide read there (as it was, twice a second, until 2026-10-21) could be a frame to half a second behind, and the
// line showed at its place for that long at every open while fancychat's own path (above) never did.
bool g_input_was_hidden = false; // input_hidden() as of the last frame: off again, the line goes back on the screen
bool g_chat_pinned = false; // fancychat (not Show with legacy chat, not auto-hidden) keeps the game's chat windows closed
                            // (an addon's vana.gameui.hide('chat_log') is asked in chat_calc_hook)
// [client] chat.hide=1, chat2.hide=1: log 1's or log 2's window kept closed the same way (the pin below) with no addon,
// so it isn't seen: its lines never show, the chat line opening or Escape included. The chat line itself stays.
bool g_chat_hide[2] = {};
ULONGLONG g_input_hide_asked = 0;
constexpr char kFancyChatAsk[] =
    "local ok, bits = pcall(function()\n"
    "    local state = package.loaded['lib.state']\n"
    "    local settings = state and state.allSettings\n"
    "    local chat = state and state.fcw and state.fcw[1]\n"
    "    local bits = 0\n"
    "    if settings ~= nil and type(settings.InputBar) == 'table' and settings.InputBar[1] == true then bits = bits + 1 end\n"
    "    if settings ~= nil and chat ~= nil and chat.LoggedIn == true and not chat.Closing and not chat.Zoning\n"
    "        and not chat.HideChat and not (type(settings.ShowWithLegacy) == 'table' and settings.ShowWithLegacy[1]) then\n"
    "        bits = bits + 2\n"
    "    end\n"
    "    if settings ~= nil and type(settings.EnabledChatMove) == 'table' and settings.EnabledChatMove[1] == true then\n"
    "        bits = bits + 4\n"
    "    end\n"
    "    if settings ~= nil and not (type(settings.ShowWithLegacy) == 'table' and settings.ShowWithLegacy[1]) then\n"
    "        bits = bits + 8\n"
    "    end\n"
    "    return bits\n"
    "end)\n"
    "return ok and bits or 0";
#include "chat_legacy.h" // unlatch_legacy: fancychat's "legacy chat opened" put back while VanaCore pins log 1
bool input_hidden() { return g_input_hide || g_input_hide_addon || gameui::hidden("chat_input"); }

// [client] input.hidetab=1: no mode tab over the chat line (Say, Shout, Linkshell 2 and the linkshell's name, Party). The
// line's draw (FsTextInputDisplay::OnDrawPrimitive, 0x101B6730) lays the line out, then, while it has the mode's shape
// (+0x30), draws that shape 12 above the line and the linkshell's or party's name beside it; its `je` past all that when
// there's no shape is made a `jmp`. The shape is still made and freed as the mode changes.
std::uintptr_t g_tab_site = 0;   // the je (0F 84 rel32)
std::uint8_t g_tab_game[6] = {}; // ... as the game has it
bool g_tab_hidden = false;

void apply_tab(bool hide)
{
    if (g_tab_site == 0 || hide == g_tab_hidden)
        return;
    if (hide) {
        std::int32_t rel = 0;
        std::memcpy(&rel, g_tab_game + 2, 4);
        std::uint8_t jump[6] = {0xE9, 0, 0, 0, 0, 0x90};
        rel += 1; // the same place from a 5-byte jmp
        std::memcpy(jump + 1, &rel, 4);
        write_code(g_tab_site, jump, 6);
    } else {
        write_code(g_tab_site, g_tab_game, 6);
    }
    g_tab_hidden = hide;
}

// fancychat keeps the game's chat windows closed by zeroing their line timer (+0x34 of CTkDrawMessageWindow:
// how long the lines shown stay, lines = timer / the erase time at +0x4e) every frame, from its present callback:
// after the game's frame. The window's calc (CTkDrawMessageWindow's, 0x101611a0, from the game's OnCalc: `open` is 1
// the frame the chat line opens) sets the timer to the full count, works the lines out from it and eases the window's
// height toward them (0x10161780, then the resize 0x1011a1a0), so the window grew a step and was drawn before the pin,
// the compass and menus that stand on it moving with it, and eased back next frame: a flicker and a bounce at every
// open. The calc is detoured: while fancychat pins (or chat.hide / chat2.hide asks for that log), the timer
// is zeroed and `open` dropped before it, so the window's target stays closed and nothing moves.
// The calc also shows the window's least lines (+0x50: 0x10160f70 raises the timer to them, after it was zeroed) when
// the cancel key is pressed (Escape: its check 0x10123670(0x3b, 0x95, 1, -1), 0x53e bytes into the calc) or the byte at
// +0x59 is set, so Escape pressed over and over opened the window a step at a time. While pinned, +0x59 is zeroed too
// and that call of the key check answers "not pressed" (the call is pointed at chat_cancel_check, which asks the game's
// own check otherwise); the key goes on doing all it does elsewhere.
using ChatCalc = void(__fastcall*)(char* window, void* edx, char open);
ChatCalc g_chat_calc = nullptr; // the game's, through the trampoline
using KeyCheck = bool(__cdecl*)(int a, int b, int c, int d);
KeyCheck g_key_check = nullptr; // the game's, the calc's call of it pointed at chat_cancel_check
bool g_pinned_calc = false; // a pinned chat window's calc is running

bool __cdecl chat_cancel_check(int a, int b, int c, int d)
{
    return !g_pinned_calc && g_key_check(a, b, c, d);
}

bool is_chat_window(const char* window)
{
    for (const ChatLog& log : g_logs)
        if (log.global != nullptr && *log.global == window)
            return true;
    return false;
}

// Which log `window` (CTkDrawMessageWindow) is, or -1
int chat_log_of(const char* window)
{
    for (int index = 0; index < 2; ++index)
        if (g_logs[index].global != nullptr && *g_logs[index].global == window)
            return index;
    return -1;
}

// Whether log `which` (chat_log_of) is kept closed: by fancychat, [client] chat.hide / chat2.hide, or an addon's vana.gameui.hide
bool chat_log_pinned(int which)
{
    return which >= 0 && (g_chat_pinned || g_chat_hide[which] || gameui::hidden("chat_log"));
}

void __fastcall chat_calc_hook(char* window, void* edx, char open)
{
    const int which = chat_log_of(window);
    const bool pinned = chat_log_pinned(which);
    if (pinned) {
        *reinterpret_cast<int*>(window + 0x34) = 0;
        window[0x59] = 0;
        open = 0;
    }
    g_pinned_calc = pinned;
    g_chat_calc(window, edx, open);
    g_pinned_calc = false;
}
using CtrlDraw = void(__fastcall*)(void* ctrl, void* edx);
CtrlDraw g_ctrl_draw = nullptr; // the game's, through the trampoline

bool is_input_ctrl(const void* ctrl)
{
    if (g_input_line == nullptr || ctrl == nullptr)
        return false;
    __try {
        const char* line = *g_input_line;
        return line != nullptr && *reinterpret_cast<const char* const*>(line + 8) == ctrl &&
               *reinterpret_cast<const char* const*>(static_cast<const char*>(ctrl) + 0xc) == line;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// The line's window off the screen (the game's resize stores the place in the window's frame, which the game puts the
// window back to every frame, so once is enough until the line opens again)
void hide_input(char* ctrl)
{
    if (g_chat_resize == nullptr)
        return;
    __try {
        const short* rect = reinterpret_cast<const short*>(ctrl + 0x3a);
        if (rect[0] <= kHiddenLeft)
            return;
        g_chat_resize(ctrl, nullptr, kHiddenLeft, rect[1], rect[2] - rect[0], rect[3] - rect[1], 1, 0, 0);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

// [client] menus.ontop (on unless 0): the game's windows fancychat's chat would cover (an NPC's selection menu, the
// details of an item or a spell, a shop's or the Mog House's lists, the action menu, the macro bar, any other) are drawn
// over it: each game window drawn over one of its chat windows, their tabs or its input bar is laid over everything
// (raise.cpp), whatever fancychat's own settings. With its Prevent obstructing FFXI UI (its Chat Window tab) on, the chat
// also stays where it is. fancychat by itself slides its chat out of their way while some are up (lib/render.lua: right,
// to 400 of the interface's units as it reads them, for an NPC's dialogue, going by its reads of the game's menus, which
// catch some dialogues and not others). Its render loop asks the global SetTargetPosX (lib/parser.lua) where to slide to;
// VanaCore gives it one that answers "nowhere" (x 0, and mvc.targetposY past the screen's bottom, so neither its sideways
// nor its upward slide is taken, nor its second chat's). Its own setting stays as the player has it (SetTargetPosX is
// never saved); menus.ontop=0 or the setting off gives it its SetTargetPosX back. Both only while the windows can go on
// top (the window draw hooked, the interface's area known), so the chat never stays under them.
// An addon can ask for the same over its own ImGui windows (vana.ui.game_on_top, raise::game_on_top): the game windows
// that meet any of its windows are laid over everything the same way, whether fancychat is there or not. menus.ontop=0
// turns that off too.
bool g_menus_ontop = true;
bool g_raise_over = false; // menus.ontop, and the windows can go on top
bool g_slide_held = false; // fancychat's SetTargetPosX is VanaCore's
bool g_slide_failed = false; // logged: it has none
constexpr char kHoldSlide[] =
    "local ok, held = pcall(function()\n"
    "    if type(SetTargetPosX) ~= 'function' then return 0 end\n"
    "    if SetTargetPosX ~= __vanacore_slide then\n"
    "        __vanacore_slide_game = SetTargetPosX\n"
    "        __vanacore_slide = function()\n"
    "            local state = package.loaded['lib.state']\n"
    "            if state ~= nil and type(state.mvc) == 'table' then state.mvc.targetposY = math.huge end\n"
    "            return 0\n"
    "        end\n"
    "        SetTargetPosX = __vanacore_slide\n"
    "    end\n"
    "    return 1\n"
    "end)\n"
    "return ok and held or 0";
constexpr char kFreeSlide[] =
    "if __vanacore_slide ~= nil and SetTargetPosX == __vanacore_slide then SetTargetPosX = __vanacore_slide_game end\n"
    "__vanacore_slide, __vanacore_slide_game = nil, nil\n"
    "return 1";

// fancychat's windows (ChatBG, ChatTabs and the second chat's, InputBar), then those of the addons that ask
// (vana.ui.game_on_top), as the last ImGui frame drew them, in the interface's units: a game window drawn over one goes on
// top. g_over_fancy: fancychat's (the game's chat windows, which it stands for and keeps closed, don't go over those).
constexpr int kMostOver = 16;
int g_over[kMostOver][4] = {};
bool g_over_fancy[kMostOver] = {};
int g_over_count = 0;
constexpr int kOverSaid = 16;
short g_over_said[kOverSaid][4] = {}; // the windows raised so far, logged (the first 16 places)
int g_over_said_count = 0;

// Twice a second, with the ask: fancychat's SetTargetPosX held or given back
void hold_slide(bool on)
{
    if (on) {
        const bool held = addons::ask("fancychat", kHoldSlide) == 1;
        if (held && !g_slide_held)
            log::write(L"Chat: fancychat's Prevent obstructing FFXI UI is on: the game's windows go over its chat, which "
                       L"stays where it is ([client] menus.ontop)");
        if (!held && !g_slide_failed) {
            g_slide_failed = true;
            log::write(L"Chat: fancychat's chat slide couldn't be held (no SetTargetPosX in it): it slides out of the "
                       L"game's windows' way as it does in Ashita");
        }
        g_slide_held = held;
    } else if (g_slide_held) {
        addons::ask("fancychat", kFreeSlide);
        g_slide_held = false;
        log::write(L"Chat: fancychat slides its chat out of the game's windows' way again");
    }
}

// VanaCore's own windows (/vana, the HUD Manager) as the last ImGui frame drew them, in the interface's units: a game window
// that meets one stays under it (raise_over_chat), as they're over every addon's window
int g_own[kMostOver][4] = {};
int g_own_count = 0;

// `r` (the game's pixels) in the interface's units, `width` x `height` the game's screen; false if they can't be told
bool units_of(const RECT& r, unsigned width, unsigned height, int* out)
{
    float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    if (!uispace::to_units(static_cast<float>(r.left), static_cast<float>(r.top), width, height, x0, y0) ||
        !uispace::to_units(static_cast<float>(r.right), static_cast<float>(r.bottom), width, height, x1, y1))
        return false;
    out[0] = static_cast<int>(std::floor(x0)), out[1] = static_cast<int>(std::floor(y0));
    out[2] = static_cast<int>(std::ceil(x1)), out[3] = static_cast<int>(std::ceil(y1));
    return true;
}

// `windows` (the game's pixels) as places to lay game windows over, in the interface's units
void add_over(const std::vector<RECT>& windows, bool fancy)
{
    unsigned width = 0, height = 0;
    ui::game_screen(width, height);
    for (const RECT& r : windows) {
        if (g_over_count >= kMostOver || !units_of(r, width, height, g_over[g_over_count]))
            continue;
        g_over_fancy[g_over_count++] = fancy;
    }
}

// Each frame: where fancychat's windows and those of the addons that ask are (menus.ontop, vana.ui.game_on_top)
void find_over()
{
    g_over_count = 0;
    if (!g_raise_over)
        return;
    // the HUD Manager's boxes are up: the game's windows stay where the game drew them. A window laid over everything went
    // over its own box too (an open HP gauge, the Spell info box beside the Magic list: the box the size of the window, so
    // none showed, reported 2026-10-31 with YunaUI's vana.ui.game_on_top)
    if (outline::showing())
        return;
    static std::vector<RECT> windows;
    windows.clear();
    gui::shown_windows("FancyChat_Chat", windows); // FancyChat_ChatBG_<name>, ChatTabs_, ChatBG2_, ChatTabs2_
    gui::shown_windows("FancyChat_InputBar", windows);
    static std::vector<RECT> plugins_plain;
    plugins_plain.clear();
    pluginapi::over(windows, plugins_plain); // plugins' (VanaCore's plugin API: game_on_top, game_on_top_areas)
    add_over(windows, true);
    windows.clear();
    gui::shown_windows_of(raise::game_on_top_owners(), windows);
    add_over(windows, false);
    add_over(plugins_plain, false);
    g_own_count = 0;
    if (g_over_count == 0)
        return;
    windows.clear();
    gui::own_windows(windows);
    unsigned width = 0, height = 0;
    ui::game_screen(width, height);
    for (const RECT& r : windows)
        if (g_own_count < kMostOver && units_of(r, width, height, g_own[g_own_count]))
            ++g_own_count;
}

// The window `ctrl` as drawn (its rect, interface units); `chat`: it's one of the chat windows fancychat stands for (and
// keeps closed), which don't go over fancychat's. The chat line is off the screen while fancychat's bar shows it; without
// the bar, it goes on top too.
// A window under kMinRaised (one text row is 16) in either direction holds nothing to see: the chat log with no lines
// showing is 6 high, the status icons with none 4, a closed menu's last frames a line. Its copy is only the picture behind
// the addon's window, which laid over it cuts a line across it (Yunas's UI, 2026-10-26), so it stays where it is.
// A closed window (state 0xE at +0x10) is no more than that: the game still draws it for its close wait and then its frame
// going, shrinking to nothing, with its rect unchanged all the while, so the full-size copy of the area it leaves would
// hide the addon's window for those frames (the Commands menu over YunaUI's minimap).
// The help bar's windows with their frames faded out by the game (helpbar::idle) are no more than that: the title box, up in
// the manager at its full size while no menu is open, was laid over the addon's window, the HUD Manager's panel and the box
// it outlines, as a patch of the scene (reported 2026-10-30).
// A chat window kept closed (chat_log_pinned) and the chat line kept off the screen (input_hidden) are no more than that: the
// game opens its logs to the full height at login and at a zone-in (the logs' reset to the layout's rect) and they ease
// shut over the frames after, and a hide that holds them shut can't show a line in them. The frame of one of those, laid
// over the addon's window, was a dark empty box with an outline stuck above YunaUI (its support log, 2026-10-31: both logs
// at 126 high 0.3 s after a zone-in, chat.hide=1 on).
constexpr int kMinRaised = 16;
constexpr std::ptrdiff_t kCtrlState = 0x10;
constexpr std::uint32_t kCtrlClosed = 0xE;
bool over_rect(const void* ctrl, short* rect, bool& chat)
{
    __try {
        if (*reinterpret_cast<const std::uint32_t*>(static_cast<const char*>(ctrl) + kCtrlState) == kCtrlClosed ||
            helpbar::idle(ctrl))
            return false;
        const char* object = *reinterpret_cast<const char* const*>(static_cast<const char*>(ctrl) + 0xc);
        chat = is_chat_window(object);
        if (chat_log_pinned(chat_log_of(object)) || (is_input_ctrl(ctrl) && input_hidden()))
            return false;
        const short* drawn = reinterpret_cast<const short*>(static_cast<const char*>(ctrl) + 0x3a);
        for (int i = 0; i < 4; ++i)
            rect[i] = drawn[i];
        return rect[2] - rect[0] >= kMinRaised && rect[3] - rect[1] >= kMinRaised;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void raise_over_chat(const void* ctrl)
{
    short rect[4] = {};
    bool chat = false;
    if (!over_rect(ctrl, rect, chat))
        return;
    // VanaCore's own windows are over every addon's and over the game's: one that meets /vana or the HUD Manager stays under
    // them, where the game drew it (laid over everything, it covered the panel, its Move widget and the box it outlines)
    for (int i = 0; i < g_own_count; ++i) {
        const int* own = g_own[i];
        if (rect[0] < own[2] && own[0] < rect[2] && rect[1] < own[3] && own[1] < rect[3])
            return;
    }
    for (int i = 0; i < g_over_count; ++i) {
        const int* over = g_over[i];
        if (chat && g_over_fancy[i])
            continue;
        if (rect[0] < over[2] && over[0] < rect[2] && rect[1] < over[3] && over[1] < rect[3]) {
            raise::add(rect);
            bool said = false;
            for (int j = 0; j < g_over_said_count && !said; ++j)
                said = std::memcmp(g_over_said[j], rect, sizeof rect) == 0;
            if (!said && g_over_said_count < kOverSaid) {
                std::memcpy(g_over_said[g_over_said_count++], rect, sizeof rect);
                log::write(L"Chat: a game window at %d,%d-%d,%d goes over %hs ([client] menus.ontop)", rect[0], rect[1], rect[2],
                           rect[3], g_over_fancy[i] ? "fancychat's chat" : "an addon's window (vana.ui.game_on_top)");
            }
            return;
        }
    }
}

// A window ctrl_draw_hook leaves out of the draw (its <id>.hide, an empty item details box, an addon's hide...) that's
// closed still has what the game's draw does first: its calc, its close timers and its delete once they're over
// (hudwin::left_out). Left out without it, a closed window stayed in the manager's list for good: an item details box
// closed empty as a trade finished, and every trade after it cancelled by the game (its trade check counts closed windows
// too) until a zone or relog (reported 2026-11-06)
struct LeftOut {
    void* ctrl;
    bool drawn = false;
    explicit LeftOut(void* window) : ctrl(window) {}
    ~LeftOut()
    {
        if (!drawn)
            hudwin::left_out(ctrl);
    }
    LeftOut(const LeftOut&) = delete;
    LeftOut& operator=(const LeftOut&) = delete;
};

void __fastcall ctrl_draw_hook(void* ctrl, void* edx)
{
    LeftOut left_out{ctrl}; // (made first: its calc runs after the frame and the titles below are set back)
    // [client] buttons.spacing: a window made since spaced (made taller) before skip places it, so it's put by the size
    // it's drawn at from its first draw. Spaced after, a window placed by its bottom (the Action Menu low on the screen
    // with action.move, the HUD Manager keeping it in the area) was put by its height as made, grew up from there, and
    // went down to its place the next frame (reported 2026-11-05)
    buttonspace::drawing(ctrl);
    if (hudwin::skip(ctrl)) // [client] party.move, action.move, buffs.hide, macrobar.hide, ...: placed (hidden or not), or left out
        return;
    // [client] action.background, mainmenu.background: the window drawn without its frame, its flag set back after the
    // draw (or once a check below leaves the window out)
    struct FrameBack {
        void* ctrl;
        std::uint8_t pieces;
        ~FrameBack() { hudwin::frame_back(ctrl, pieces); }
    } const frame_back{ctrl, hudwin::frameless(ctrl)};
    // [client] titles.hide: the menu's title tab and its picture's title left out of the draw, set back after it
    struct TitleBack {
        hudwin::TitleBack saved;
        ~TitleBack() { hudwin::title_back(saved); }
    } title_back;
    hudwin::untitled(ctrl, title_back.saved);
    const profilewin::Drawing profile(ctrl); // [client] profile.clean, profile.title.*: the Profile panel's picture, put back after
    if (is_input_ctrl(ctrl)) { // the chat line's window, before its draw (its first too: the game makes it again at its place each time the line opens)
        if (input_hidden())
            hide_input(static_cast<char*>(ctrl)); // input.hide, fancychat's bar or an addon's hide: off the screen, never seen
        else if (g_input_move)
            place_input(false); // input.move: where the setting puts it
    }
    if (helpbar::skip(ctrl)) // [client] helpbar.* and network.panel: a window of the help bar left out
        return;
    if (alliancelist::skip(ctrl)) // [client] party.hide, alliance1.hide, alliance2.hide: those lists left out
        return;
    // [client] target.hide: the Target window put off the screen, not left out (the arrow over the target is drawn from its
    // own draw)
    const bool target_hidden = targetwin::hide(ctrl);
    if (!target_hidden)
        targetwin::before_draw(ctrl); // [client] target.move, .width, ...: the Target window made at the game's place, put right before its first draw
    if (castbar::skip(ctrl)) // [client] castbar.hide: the cast bar left out
        return;
    if (itemtips::empty(ctrl, chatfont::details_window(layout_of(ctrl)))) // [client] tips.empty.hide: an item's details with no
        return; // item (an empty slot), or the comment box with no comment (the Check window's, no bazaar message), left out

    // a window an addon hides (vana.gameui.hide); the party list showing the fishing gauge is drawn all the same
    if (!target_hidden && !alliancelist::gauge(ctrl) && gameui::skip(ctrl))
        return;
    // vana.gameui.window: what the window draws kept for an addon that asked; one hidden by its layout's name drawn into nothing
    const gamewin::Drawing drawn(ctrl);
    menu_follow_list(ctrl); // [client] menu.rows: a selection menu's window made taller for a list that grew after it opened
    const void* outer = itemslot::enter_window(ctrl); // [client] items.slot=hide: which window a square is drawn in
    const void* buff_outer = buffwin::enter_window(ctrl); // [client] buffs.scale: the status icons' square and arrow
    const char* layout = layout_of(ctrl);
    const chatfont::Area area(layout); // True Font: the text in it in its area's font (items.font, help.font, input.font)
    const macrotext::Window macro_bar(layout); // ... the macro bar's names and keys' labels VanaCore's own text, drawn whole
    const labeltext::Window whole_labels(ctrl, layout); // ... and the Config windows' titles, settings' names and options
    const listtext::Window whole_names(ctrl, layout);   // ... and the item lists' names (windows noted as they're drawn)
    const itemrarity::Details details(chatfont::details_window(layout) ? ctrl : nullptr); // tips.rarity: its item's colour
    // [client] window.corners: what the window draws inside its background (text, rows, item squares) kept inside its cut or
    // round corners (reported in game 2026-11-06: letters and item squares stood out past them)
    struct CornerScope {
        CornerScope() { vanacore_gfx_window_drawing(1); }
        ~CornerScope() { vanacore_gfx_window_drawing(0); }
        CornerScope(const CornerScope&) = delete;
        CornerScope& operator=(const CornerScope&) = delete;
    } const corners;
    left_out.drawn = true; // (the game's draw runs its calc itself)
    g_ctrl_draw(ctrl, edx);
    buffwin::leave_window(buff_outer);
    itemslot::leave_window(outer);
    phraselist::on_draw(ctrl); // the auto-translate list: where it was drawn, to lay it over addons' drawing
    if (g_over_count > 0)
        raise_over_chat(ctrl); // a game window over fancychat's chat: the same (menus.ontop)
}

// Windows the game stands over the chat (an NPC's selection menu and the like): the layout's 0x1015e4c0 puts such a window's
// bottom on the top of the window it goes with (edi, that window's object), at its own left. Two calls go into it:
// - menu_left, for `movsx ebp, word ptr [esp+10h]` (the window's own left; its rectangle is at [esp+10h], the one it goes
//   with at [esp+18h]): notes whether it goes with a chat window (log 1 or 2), and with [client] chat.menus=1 gives the chat's
//   left;
// - menu_place, for `cmp edi,10h / jge / mov edi,10h` (its top no higher than 16), 63h bytes on: with [client] menus.move=1
//   and a window over the chat, its left at menus.x and its bottom at menus.y (percents of the UI area). Either way its right
//   ([esp+14h], which the marker at +0x88 is put by after it) goes along with it. A window too tall to fit over the chat is
//   pinned at the top by the game and left there, at its own left: Status draws its numbers at that place, not its window's.
std::uintptr_t g_menu_left_site = 0;
constexpr std::uintptr_t kMenuPlaceOffset = 0x63;
bool g_menu_patched = false;
char* g_menu_anchor = nullptr;  // log 1's window object while the calls are in
char* g_menu_anchor2 = nullptr; // log 2's
unsigned char g_menu_follow = 0;    // chat.menus
unsigned char g_menu_custom = 0;    // menus.move
unsigned char g_menu_over_chat = 0; // the window being placed goes with a chat window
int g_menu_x = 0, g_menu_bottom = 0; // menus.move's place, in the UI's pixels
// the last window placed over the chat, for the menus' box (outline): its size, and where the game (or chat.menus) put it
int g_menu_seen_w = 280, g_menu_seen_h = 90, g_menu_seen_left = 0, g_menu_seen_top = 0;
double g_menu_px = 0.5, g_menu_py = 90.0; // menus.x, menus.y
constexpr std::uint8_t kMenuLeftGame[5] = {0x0F, 0xBF, 0x6C, 0x24, 0x10};
constexpr std::uint8_t kMenuPlaceGame[10] = {0x83, 0xFF, 0x10, 0x7D, 0x05, 0xBF, 0x10, 0x00, 0x00, 0x00};

__declspec(naked) void menu_left()
{
    __asm {
        movsx ebp, word ptr [esp + 14h] ; the window's own left (the call's return address is on the stack)
        mov byte ptr g_menu_over_chat, 0
        cmp edi, g_menu_anchor
        je over_chat
        cmp edi, g_menu_anchor2
        jne done
    over_chat:
        mov byte ptr g_menu_over_chat, 1
        cmp byte ptr g_menu_follow, 0
        je done
        movsx ebp, word ptr [esp + 1Ch] ; the chat's left
    done:
        ret
    }
}

__declspec(naked) void menu_place()
{
    __asm {
        cmp edi, 10h                    ; the game's: its top no higher than 16
        jge fits
        mov edi, 10h
        movsx ebp, word ptr [esp + 14h] ; too tall to go over the chat (Status): the game pins it at the top, where its
        ret                             ; contents are drawn whatever its window does, so it's left as the game has it
    fits:
        cmp byte ptr g_menu_over_chat, 0
        je done
        mov g_menu_seen_left, ebp       ; noted for the box (eax and ecx are free here: move_right takes them too)
        mov g_menu_seen_top, edi
        movsx eax, word ptr [esp + 18h] ; the window's right
        movsx ecx, word ptr [esp + 14h] ; its left
        sub eax, ecx
        mov g_menu_seen_w, eax
        movsx eax, word ptr [esp + 1Ah] ; its bottom
        movsx ecx, word ptr [esp + 16h] ; its top
        sub eax, ecx
        mov g_menu_seen_h, eax
        cmp byte ptr g_menu_custom, 0
        je move_right
        mov ebp, g_menu_x
        movsx eax, word ptr [esp + 1Ah] ; the window's bottom
        movsx ecx, word ptr [esp + 16h] ; its top
        sub eax, ecx
        mov edi, g_menu_bottom
        sub edi, eax
    move_right:
        movsx eax, word ptr [esp + 14h] ; the window's left
        movsx ecx, word ptr [esp + 18h] ; its right
        sub ecx, eax
        add ecx, ebp
        mov word ptr [esp + 18h], cx
    done:
        ret
    }
}

// The layout's manager (0x105ecb98, its anchors' holders at +0x8c): it puts the windows over the chat again when the top of
// the window they go with moves, noting each anchor's top at +0xa0 (five shorts). A change of ours that doesn't move that top
// (menus.x/y, the chat's left with chat.menus) spoils the notes, so the next frame's layout puts them again: an open menu
// follows the slider.
char* g_layout = nullptr;
std::uint64_t g_menu_signature = 0;

void replace_menus()
{
    if (g_layout == nullptr)
        return;
    std::uint64_t signature = (std::uint64_t(g_menu_follow) << 1 | g_menu_custom) << 62;
    if (g_menu_custom)
        signature ^= std::uint64_t(std::uint32_t(g_menu_x)) << 24 ^ std::uint32_t(g_menu_bottom);
    if (g_menu_follow)
        for (int index = 0; index < 2; ++index)
            if (const char* ctrl = chat_ctrl(index))
                signature ^= std::uint64_t(std::uint16_t(*reinterpret_cast<const short*>(ctrl + 0x3a))) << (40 + index * 8);
    if (signature == g_menu_signature)
        return;
    g_menu_signature = signature;
    for (int slot = 0; slot < 5; ++slot)
        *reinterpret_cast<short*>(g_layout + 0xa0 + slot * 2) = -32768;
}

void apply_menu_patches(bool on)
{
    if (g_menu_left_site == 0 || on == g_menu_patched)
        return;
    const std::uintptr_t place = g_menu_left_site + kMenuPlaceOffset;
    if (on) {
        const auto call_to = [](std::uintptr_t at, void* to, std::uint8_t* code) {
            code[0] = 0xE8;
            const auto rel = static_cast<std::int32_t>(reinterpret_cast<std::uintptr_t>(to) - (at + 5));
            std::memcpy(code + 1, &rel, 4);
        };
        std::uint8_t left[5] = {}, placed[10] = {0, 0, 0, 0, 0, 0x90, 0x90, 0x90, 0x90, 0x90};
        call_to(g_menu_left_site, reinterpret_cast<void*>(&menu_left), left);
        call_to(place, reinterpret_cast<void*>(&menu_place), placed);
        write_code(g_menu_left_site, left, 5);
        write_code(place, placed, 10);
    } else {
        write_code(g_menu_left_site, kMenuLeftGame, 5);
        write_code(place, kMenuPlaceGame, 10);
    }
    g_menu_patched = on;
}

#include "chat_place.h" // game_place, apply_chat_place, set_place_rests: chat.move and chat2.move, resting under fancychat
#include "chat_watch.h" // watch_logs_before/after: "Chat watch:" lines in vanacore.log as a log's window moves or is made again

// The local player's name, for the chat log's file (every two seconds at most)
ULONGLONG g_name_checked = 0;
FILETIME g_chatlog_ini_time{};
bool g_chatlog_ini_read = false;

void refresh_character_name()
{
    game::PartyMember members[18];
    game::Alliance alliance;
    if (game::party(members, alliance) && members[0].active && !members[0].name.empty())
        g_chatlog_name = members[0].name;
}

// [client] window.corners / window.corners.size, read by chatlog_refresh
static void read_corners()
{
    wchar_t shape[16] = {};
    GetPrivateProfileStringW(L"client", L"window.corners", L"square", shape, 16, g_chatlog_ini.c_str());
    g_corner_shape = _wcsicmp(shape, L"cut") == 0 ? 1 : _wcsicmp(shape, L"round") == 0 ? 2 : 0;
    g_corner_size = std::clamp(static_cast<int>(GetPrivateProfileIntW(L"client", L"window.corners.size", 6, g_chatlog_ini.c_str())), 1, 32);
}

// ... handed to the drawing when they change, by publish_window_opacity
static void publish_corners()
{
    static int last[2] = {-1, -1};
    if (last[0] == g_corner_shape && last[1] == g_corner_size)
        return;
    last[0] = g_corner_shape, last[1] = g_corner_size;
    vanacore_gfx_set_window_corners(g_corner_shape, static_cast<float>(g_corner_size));
}

void chatlog_refresh()
{
    const ULONGLONG now = GetTickCount64();
    if (g_chatlog_checked != 0 && now - g_chatlog_checked < 100)
        return;
    g_chatlog_checked = now;
    // the settings are read again when the ini was saved (its time changed): a slider moved in the options shows at once
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!g_chatlog_ini.empty() && GetFileAttributesExW(g_chatlog_ini.c_str(), GetFileExInfoStandard, &data)) {
        const bool same = g_chatlog_ini_read && CompareFileTime(&data.ftLastWriteTime, &g_chatlog_ini_time) == 0;
        g_chatlog_ini_time = data.ftLastWriteTime;
        g_chatlog_ini_read = true;
        if (same) {
            if (g_chatlog_on && now - g_name_checked >= 2000) {
                g_name_checked = now;
                refresh_character_name();
            }
            return;
        }
    }
    if (!g_chatlog_ini.empty()) {
        int height = static_cast<int>(GetPrivateProfileIntW(L"client", L"chat.lineheight", kLineHeightDefault, g_chatlog_ini.c_str()));
        apply_line_height(height < 8 ? 8 : height > kLineHeightMax ? kLineHeightMax : height);
        const int rows = static_cast<int>(GetPrivateProfileIntW(L"client", L"menu.rows", kMenuRowsDefault, g_chatlog_ini.c_str()));
        apply_menu_rows(rows < kMenuRowsDefault ? kMenuRowsDefault : rows > kMenuRowsMax ? kMenuRowsMax : rows);
        const auto number = [](const wchar_t* key, double fallback, double low, double high) {
            wchar_t text[32] = {};
            GetPrivateProfileStringW(L"client", key, L"", text, 32, g_chatlog_ini.c_str());
            wchar_t* end = nullptr;
            const double value = std::wcstod(text, &end);
            if (end == text)
                return fallback;
            return value < low ? low : value > high ? high : value;
        };
        apply_chat_place(0, GetPrivateProfileIntW(L"client", L"chat.move", 0, g_chatlog_ini.c_str()) != 0,
                         number(L"chat.x", 0.5, 0, 100), number(L"chat.y", 99, 0, 100), number(L"chat.width", 95, 5, 100));
        apply_chat_place(1, GetPrivateProfileIntW(L"client", L"chat2.move", 0, g_chatlog_ini.c_str()) != 0,
                         number(L"chat2.x", 0.5, 0, 100), number(L"chat2.y", 85, 0, 100), number(L"chat2.width", 45, 5, 100));
        const auto on = [](const wchar_t* key) { return GetPrivateProfileIntW(L"client", key, 0, g_chatlog_ini.c_str()) != 0; };
        g_compass_on = on(L"chat.compass");
        g_compass_custom = g_compass_set_left != nullptr && on(L"compass.move");
        g_compass_px = number(L"compass.x", 3, 0, 100), g_compass_py = number(L"compass.y", 90, 0, 100);
        g_compass_hide = on(L"compass.hide");
        apply_compass_hide();
        g_clock_hide = on(L"clock.hide");
        g_clock_move = on(L"clock.move");
        g_clock_always = on(L"clock.always");
        g_clock_px = number(L"clock.x", 3, 0, 100), g_clock_py = number(L"clock.y", 92, 0, 100);
        {
            wchar_t align[16] = {};
            GetPrivateProfileStringW(L"client", L"clock.align", L"left", align, 16, g_chatlog_ini.c_str());
            g_clock_align = _wcsicmp(align, L"right") == 0                                       ? 2
                            : _wcsicmp(align, L"centre") == 0 || _wcsicmp(align, L"center") == 0 ? 1
                                                                                                 : 0;
        }
        g_chat_hide[0] = on(L"chat.hide");
        g_chat_hide[1] = on(L"chat2.hide");
        g_menu_follow = on(L"chat.menus") ? 1 : 0;
        g_menu_custom = on(L"menus.move") ? 1 : 0;
        g_menu_px = number(L"menus.x", 0.5, 0, 100), g_menu_py = number(L"menus.y", 90, 0, 100);
        apply_menu_patches(g_menu_follow != 0 || g_menu_custom != 0);
        const bool input = on(L"input.move");
        g_input_x = number(L"input.x", 0.5, 0, 100), g_input_y = number(L"input.y", 99, 0, 100);
        if (g_input_move && !input)
            place_input(true); // back where the game puts it
        g_input_move = input;
        g_menus_ontop = GetPrivateProfileIntW(L"client", L"menus.ontop", 1, g_chatlog_ini.c_str()) != 0;
        const bool hide = on(L"input.hide");
        if (g_input_hide && !hide && !g_input_hide_addon)
            place_input(!g_input_move); // back on the screen: where the game puts it, or where input.move does
        g_input_hide = hide;
        apply_tab(on(L"input.hidetab"));
    }
    if (!g_chatlog_ini.empty()) {
        const auto percent = [](const wchar_t* key) {
            const int value = static_cast<int>(GetPrivateProfileIntW(L"client", key, 100, g_chatlog_ini.c_str()));
            return (value < 0 ? 0 : value > 100 ? 100 : value) / 100.0f;
        };
        g_window_opacity = percent(L"window.opacity");
        g_chat_opacity = percent(L"chat.opacity");
        g_border_opacity = percent(L"window.border.opacity");
        const auto colour_of = [](const wchar_t* key, std::uint32_t& rgb) {
            wchar_t text[16] = {};
            GetPrivateProfileStringW(L"client", key, L"", text, 16, g_chatlog_ini.c_str());
            if (text[0] != L'#' || wcslen(text) < 7)
                return 0;
            text[7] = L'\0';
            wchar_t* end = nullptr;
            rgb = static_cast<std::uint32_t>(std::wcstoul(text + 1, &end, 16));
            return end != nullptr && *end == L'\0' ? 1 : 0;
        };
        g_window_tint = colour_of(L"window.color", g_window_tint_rgb);
        g_chat_tint = colour_of(L"chat.color", g_chat_tint_rgb);
        g_border_tint = colour_of(L"window.border.color", g_border_tint_rgb);
        g_chat_border_tint = colour_of(L"chat.border.color", g_chat_border_tint_rgb);
        g_chat_border_opacity = percent(L"chat.border.opacity");
        const auto flag = [](const wchar_t* key) { return GetPrivateProfileIntW(L"client", key, 0, g_chatlog_ini.c_str()) != 0 ? 1 : 0; };
        g_border_solid = flag(L"window.border.solid");
        g_chat_border_solid = flag(L"chat.border.solid");
        g_input_border_solid = flag(L"input.border.solid");
        read_corners();
        // chat.match=1: the chat windows take the Windows values (opacities and colours) whatever their own say
        if (GetPrivateProfileIntW(L"client", L"chat.match", 0, g_chatlog_ini.c_str()) != 0) {
            g_chat_opacity = g_window_opacity;
            g_chat_border_opacity = g_border_opacity;
            g_chat_tint = g_window_tint, g_chat_tint_rgb = g_window_tint_rgb;
            g_chat_border_tint = g_border_tint, g_chat_border_tint_rgb = g_border_tint_rgb;
            g_chat_border_solid = g_border_solid;
        }
        wchar_t match[16] = {};
        GetPrivateProfileStringW(L"client", L"input.match", L"windows", match, 16, g_chatlog_ini.c_str());
        g_input_match = _wcsicmp(match, L"chat") == 0 ? 1 : _wcsicmp(match, L"off") == 0 ? 2 : 0;
        g_input_opacity = percent(L"input.opacity");
        g_input_border_opacity = percent(L"input.border.opacity");
        g_input_tint = colour_of(L"input.color", g_input_tint_rgb);
        g_input_border_tint = colour_of(L"input.border.color", g_input_border_tint_rgb);
        g_chat_shadow = static_cast<int>(GetPrivateProfileIntW(L"client", L"chat.shadow", 0, g_chatlog_ini.c_str()));
        const int shadow = static_cast<int>(GetPrivateProfileIntW(L"client", L"chat.shadow.opacity", 80, g_chatlog_ini.c_str()));
        g_chat_shadow_opacity = (shadow < 0 ? 0 : shadow > 100 ? 100 : shadow) / 100.0f;
    }
    g_no_indent = !g_chatlog_ini.empty() && GetPrivateProfileIntW(L"client", L"chat.noindent", 0, g_chatlog_ini.c_str()) != 0;
    const int history = g_chatlog_ini.empty() ? kHistoryDefault
                                              : static_cast<int>(GetPrivateProfileIntW(L"client", L"chat.history", kHistoryDefault, g_chatlog_ini.c_str()));
    g_history_limit = history < kHistoryRetail ? kHistoryRetail : history > kHistoryMax ? kHistoryMax : history;
    const int scrollback = g_chatlog_ini.empty() ? kScrollbackMax
                                                 : static_cast<int>(GetPrivateProfileIntW(L"client", L"chat.scrollback", kScrollbackMax, g_chatlog_ini.c_str()));
    g_scrollback_lines = scrollback <= 0 ? 0 : scrollback < kScrollbackMin ? kScrollbackMin : scrollback > kScrollbackMax ? kScrollbackMax : scrollback;
    chatpages::set_lines(g_scrollback_lines);
    g_prefix = !g_chatlog_ini.empty() && GetPrivateProfileIntW(L"client", L"chat.prefix", 0, g_chatlog_ini.c_str()) != 0;
    g_prefix_short = !g_chatlog_ini.empty() && GetPrivateProfileIntW(L"client", L"chat.prefix.short", 0, g_chatlog_ini.c_str()) != 0;
    g_mode_debug = !g_chatlog_ini.empty() && GetPrivateProfileIntW(L"client", L"chat.modes.debug", 0, g_chatlog_ini.c_str()) != 0;
    g_stamps = !g_chatlog_ini.empty() && GetPrivateProfileIntW(L"client", L"chat.timestamps", 0, g_chatlog_ini.c_str()) != 0;
    if (g_stamps) {
        wchar_t style[16] = {};
        GetPrivateProfileStringW(L"client", L"chat.timestamps.style", L"square", style, 16, g_chatlog_ini.c_str());
        g_stamp_style = _wcsicmp(style, L"round") == 0 ? 1 : _wcsicmp(style, L"none") == 0 ? 2 : 0;
        g_stamp_seconds = GetPrivateProfileIntW(L"client", L"chat.timestamps.seconds", 1, g_chatlog_ini.c_str()) != 0;
        g_stamp_12h = GetPrivateProfileIntW(L"client", L"chat.timestamps.hour12", 0, g_chatlog_ini.c_str()) != 0;
        wchar_t ampm[16] = {};
        GetPrivateProfileStringW(L"client", L"chat.timestamps.ampm", L"lower", ampm, 16, g_chatlog_ini.c_str());
        g_stamp_upper = _wcsnicmp(ampm, L"upper", 5) == 0;
        g_stamp_ampm_space = wcsstr(ampm, L"_spaced") != nullptr;
        g_stamp_system = GetPrivateProfileIntW(L"client", L"chat.timestamps.system", 1, g_chatlog_ini.c_str()) != 0;
        wchar_t colour[16] = {};
        GetPrivateProfileStringW(L"client", L"chat.timestamps.color", L"", colour, 16, g_chatlog_ini.c_str());
        unsigned rgb = 0;
        wchar_t* end = nullptr;
        g_stamp_colour = false;
        if (colour[0] == L'#' && wcslen(colour) >= 7) {
            colour[7] = L'\0';
            rgb = static_cast<unsigned>(std::wcstoul(colour + 1, &end, 16));
            if (end != nullptr && *end == L'\0') {
                // the game's colours are Direct3D's 0xAARRGGBB (in cnf.dat as B, G, R, A bytes), alpha 0x80; the text is drawn
                // with its colour doubled, so a channel of 0x80 is full and anything over it is the same (white is 80808080)
                const auto channel = [](unsigned c) { return static_cast<std::uint32_t>((c * 128 + 127) / 255); };
                g_stamp_abgr = 0x80000000u | channel((rgb >> 16) & 0xFF) << 16 | channel((rgb >> 8) & 0xFF) << 8 | channel(rgb & 0xFF);
                g_stamp_colour = true;
            }
        }
    }
    g_chatlog_on = !g_chatlog_ini.empty() && GetPrivateProfileIntW(L"client", L"chatlog", 0, g_chatlog_ini.c_str()) != 0;
    if (!g_chatlog_on) {
        if (g_chatlog_file != nullptr) {
            std::fclose(g_chatlog_file);
            g_chatlog_file = nullptr;
            g_chatlog_path.clear();
        }
        return;
    }
    g_name_checked = now;
    refresh_character_name();
}

void chatlog_save(std::string_view line, int mode)
{
    mcp::on_chat(line, mode); // the MCP server's chat_log tool
    chatlog_refresh();
    if (!g_chatlog_on || g_chatlog_name.empty() || g_chatlog_dir.empty())
        return;
    std::string text = plain(line);
    if (text.find('\xFD') != std::string::npos)
        text = parse_auto_translate(text, false);
    if (g_prefix && !g_writing && !text.empty()) // not on VanaCore's own lines: they're mode 1 but not /say
        if (const ModeName* entry = mode_entry(mode))
            text = with_prefix(*entry, text);
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r'))
        text.pop_back();
    // the game's text is Shift-JIS (with its own row for ñ, é, ¿: gametext.hpp); the file is UTF-8
    const std::string utf8 = gametext::to_utf8(text);
    SYSTEMTIME time;
    GetLocalTime(&time);
    std::wstring name;
    for (const char c : g_chatlog_name)
        name += std::wcschr(L"\\/:*?\"<>|", static_cast<wchar_t>(static_cast<unsigned char>(c))) != nullptr
                    ? L'_'
                    : static_cast<wchar_t>(static_cast<unsigned char>(c));
    wchar_t file[96];
    swprintf_s(file, L".%04u-%02u-%02u.log", time.wYear, time.wMonth, time.wDay);
    const std::wstring path = g_chatlog_dir + L"chatlog." + name + file;
    if (path != g_chatlog_path || g_chatlog_file == nullptr) {
        if (g_chatlog_file != nullptr)
            std::fclose(g_chatlog_file);
        g_chatlog_file = nullptr;
        g_chatlog_path = path;
        CreateDirectoryW(g_chatlog_dir.c_str(), nullptr);
        if (_wfopen_s(&g_chatlog_file, path.c_str(), L"ab") != 0)
            g_chatlog_file = nullptr;
        if (g_chatlog_file == nullptr) {
            g_chatlog_on = false; // retried at the next check
            g_chatlog_checked = GetTickCount64();
            log::write(L"Chat: logs\\chatlog.*.log couldn't be opened; no chat log is saved for now");
            return;
        }
    }
    std::fprintf(g_chatlog_file, "[%02u:%02u:%02u] %s\n", time.wHour, time.wMinute, time.wSecond, utf8.c_str());
    std::fflush(g_chatlog_file);
}

unsigned char __fastcall add_line(void* log, void* edx, const char* line, const unsigned char* mode, int a, int b, int c)
{
    if (line == nullptr || mode == nullptr)
        return g_add_line(log, edx, line, mode, a, b, c);
    // SoulspireXI (QA-SOUL-COMPANIONS-001): Rebrand native FFXI client DAT Trust/Alter Ego system
    // messages to Soul Echoes terminology without modifying player chat channels or adding hooks.
    std::string echoes_line;
    if (soulspire::engine().enabled() && soulspire::is_system_rebrand_chat_mode(mode[0])) {
        echoes_line = soulspire::rebrand_echoes_text(line);
        if (echoes_line != line)
            line = echoes_line.c_str();
    }
    chatlog_refresh();
    struct InAddLine { // (and chat.font's widths for the game's line wrap: chatfont::hold)
        InAddLine() { ++g_in_add_line, chatfont::hold(true); }
        ~InAddLine() { --g_in_add_line, chatfont::hold(false); }
    } in_add_line;
    if (g_mode_debug)
        log::write(L"Chat: mode %u (%hs), flags %02X %02X: %.70hs", static_cast<unsigned>(mode[0]),
                   mode_name(mode[0]) != nullptr ? mode_name(mode[0]) : "?", static_cast<unsigned>(mode[1]),
                   static_cast<unsigned>(mode[2]), plain(line).c_str());
    // chat.timestamps and chat.prefix: "[HH:MM:SS] " and "[Linkshell] " go in front of a line (not a continuation of the
    // one above it); the saved file has its own stamp
    const auto add = [&](const char* text, const unsigned char* given) {
        // chat.noindent is in indent_hook (the flags' low five bits aren't the wrap indent: they mark system/NPC lines)
        const unsigned char* flags = given;
        const bool indented = (given[1] & 0x1F) != 0;
        if (g_stamps && has_stamp(text)) // the game feeding back a line already stamped (and given its prefix)
            return g_add_line(log, edx, text, flags, a, b, c);
        // not VanaCore's own lines (g_writing): they use mode 1 so they're the default colour, but they aren't /say
        const ModeName* entry = g_prefix && !g_writing ? mode_entry(flags[0]) : nullptr;
        if ((!g_stamps && entry == nullptr) || (indented && !g_stamp_system) || text[0] == '\0')
            return g_add_line(log, edx, text, flags, a, b, c);
        std::string stamped = g_stamps ? timestamp() : std::string();
        stamped += entry != nullptr ? with_prefix(*entry, text) : std::string(text);
        return g_add_line(log, edx, stamped.c_str(), flags, a, b, c);
    };
    if (g_text_handler == nullptr || g_in_text) {
        chatlog_save(line, mode[0]);
        return add(line, mode);
    }
    Text text;
    // All three bytes, as Ashita gives them (e.mode): the mode, then its flags. Addons go by the flags: Points takes a
    // line only when e.mode > 600 (the game's system lines are flags 03: mode 140 comes as 0x38C), timestamp keeps
    // e.mode & 0xFFFFFF00 when it changes the mode. Only the mode itself (the low byte) is taken back from mode_modified.
    text.mode = text.mode_modified = mode[0] | mode[1] << 8 | mode[2] << 16;
    text.indent = text.indent_modified = (mode[1] & 0x1F) != 0;
    text.message = line;
    // The game ends a server message with a newline ("[PartyFinder] ... LFM\n", "You defeated a venture target.\n"), which
    // it draws nothing for. Plugins and addons get the line without it: one that colours the line to its end
    // (customcolors) would otherwise put its closing colour code after the newline, and the game draws a code on its
    // own as an empty row (the blank line under every coloured venture and Party Finder message). A line nothing
    // changed goes to the game as it came.
    while (!text.message.empty() && (text.message.back() == '\n' || text.message.back() == '\r'))
        text.message.pop_back();
    text.modified = text.message;
    text.injected = g_writing;
    g_in_text = true;
    bool blocked = false;
    try {
        blocked = g_text_handler(true, text);
    } catch (...) {
        log::write(L"Chat: the text handler failed on a line coming in; the chat log gets it");
    }
    g_in_text = false;
    if (blocked)
        return 1;
    chatlog_save(text.modified, text.mode_modified & 0xFF);
    if (text.modified == text.message && (text.mode_modified & 0xFF) == mode[0] && text.indent_modified == text.indent)
        return add(line, mode);
    unsigned char changed[4] = {mode[0], mode[1], mode[2], 0};
    changed[0] = static_cast<unsigned char>(text.mode_modified);
    if (text.indent_modified != text.indent)
        changed[1] = static_cast<unsigned char>((changed[1] & ~0x1F) | (text.indent_modified ? 1 : 0));
    return add(text.modified.c_str(), changed);
}

// Bytes that belong together: a colour code, a Shift-JIS character, an auto-translate phrase, one of the game's 7F codes
// (7F FC and 7F FB around a player's name in a chat line: "[2]<\x7F\xFCName\x7F\xFB> text").
size_t unit_length(std::string_view text, size_t at)
{
    const auto byte = static_cast<unsigned char>(text[at]);
    size_t length = 1;
    if (byte == 0x1E || byte == 0x1F || byte == 0x7F || (byte >= 0x81 && byte <= 0x9F) || (byte >= 0xE0 && byte <= 0xFC))
        length = 2;
    else if (byte == 0xFD)
        length = 6;
    return at + length <= text.size() ? length : text.size() - at;
}

// The text without colour codes (or 7F codes), for the log.
std::string plain(std::string_view text)
{
    std::string out;
    for (size_t at = 0; at < text.size();) {
        const size_t length = unit_length(text, at);
        const auto byte = static_cast<unsigned char>(text[at]);
        if (byte != 0x1E && byte != 0x1F && byte != 0x7F)
            out.append(text.substr(at, length));
        at += length;
    }
    return out;
}

void flush()
{
    if (g_write == nullptr || g_chat_log == nullptr || *g_chat_log == nullptr)
        return;
    while (!g_waiting.empty()) {
        const auto [mode, line] = std::move(g_waiting.front());
        g_waiting.pop_front();
        g_writing = true;
        g_write(mode, line.c_str());
        g_writing = false;
    }
}

// The chat windows' rectangles for [client] chat.opacity: the game's chat window object (what its draw, 0x10160da0, is called
// on) has the window, whose rectangle is the background's. The objects are remembered as they draw; the rectangles are
// handed to the graphics layer each frame, which finds the backgrounds inside them.
using LogDraw = void(__fastcall*)(void* window, void* edx, int a, int b, int c, int d, int e, int f);
LogDraw g_log_draw = nullptr;
void* g_chat_windows[2] = {};

void __fastcall log_draw_hook(void* window, void* edx, int a, int b, int c, int d, int e, int f)
{
    if (window != g_chat_windows[0] && window != g_chat_windows[1])
        g_chat_windows[1] = std::exchange(g_chat_windows[0], window);
    const chatfont::Drawing drawing; // chat.font: the window's text laid out and drawn by the font's letters
    g_log_draw(window, edx, a, b, c, d, e, f);
}

// The window's rectangle (left, top, right, bottom) of the chat window object `window`; false if it can't be read
bool chat_window_rect(void* window, float* out)
{
    __try {
        void* ctrl = *reinterpret_cast<void**>(static_cast<char*>(window) + 8);
        if (ctrl == nullptr || g_get_rect == nullptr)
            return false;
        short rect[4] = {};
        g_get_rect(ctrl, nullptr, rect);
        if (rect[2] <= rect[0] || rect[3] <= rect[1])
            return false;
        for (int i = 0; i < 4; ++i)
            out[i] = rect[i];
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void publish_window_opacity()
{
    static bool known = false;
    static float last_all = -1.0f, last_chat = -1.0f, last_border = -1.0f, last_chat_border = -1.0f;
    static int last_tint[2] = {-1, -1}, last_btint[2] = {-1, -1};
    static std::uint32_t last_rgb[2] = {}, last_brgb[2] = {};
    if (last_btint[0] != g_border_tint || last_btint[1] != g_chat_border_tint || last_brgb[0] != g_border_tint_rgb ||
        last_brgb[1] != g_chat_border_tint_rgb) {
        last_btint[0] = g_border_tint, last_btint[1] = g_chat_border_tint;
        last_brgb[0] = g_border_tint_rgb, last_brgb[1] = g_chat_border_tint_rgb;
        vanacore_gfx_set_border_colours(g_border_tint, g_border_tint_rgb, g_chat_border_tint, g_chat_border_tint_rgb);
    }
    if (last_tint[0] != g_window_tint || last_tint[1] != g_chat_tint || last_rgb[0] != g_window_tint_rgb || last_rgb[1] != g_chat_tint_rgb) {
        last_tint[0] = g_window_tint, last_tint[1] = g_chat_tint;
        last_rgb[0] = g_window_tint_rgb, last_rgb[1] = g_chat_tint_rgb;
        vanacore_gfx_set_window_colours(g_window_tint, g_window_tint_rgb, g_chat_tint, g_chat_tint_rgb);
    }
    if (!known || last_all != g_window_opacity || last_chat != g_chat_opacity || last_border != g_border_opacity ||
        last_chat_border != g_chat_border_opacity) {
        known = true;
        last_all = g_window_opacity;
        last_chat = g_chat_opacity;
        last_border = g_border_opacity;
        last_chat_border = g_chat_border_opacity;
        vanacore_gfx_set_window_opacity(g_window_opacity, g_chat_opacity);
        vanacore_gfx_set_border_opacity(g_border_opacity, g_chat_border_opacity);
    }
    static int last_shadow = -1;
    static float last_shadow_opacity = -1.0f;
    if (last_shadow != g_chat_shadow || last_shadow_opacity != g_chat_shadow_opacity) {
        last_shadow = g_chat_shadow, last_shadow_opacity = g_chat_shadow_opacity;
        vanacore_gfx_set_chat_shadow(g_chat_shadow, g_chat_shadow_opacity);
    }
    float rects[8] = {};
    unsigned count = 0;
    for (void*& window : g_chat_windows) {
        if (window == nullptr)
            continue;
        if (chat_window_rect(window, rects + 4 * count))
            ++count;
        else
            window = nullptr;
    }
    vanacore_gfx_set_chat_rects(rects, count);

    // the chat line's window (input.match): every window's look, the chat windows', or its own
    float look[2] = {g_window_opacity, g_border_opacity};
    int tint[2] = {g_window_tint, g_border_tint};
    std::uint32_t rgb[2] = {g_window_tint_rgb, g_border_tint_rgb};
    if (g_input_match == 1)
        look[0] = g_chat_opacity, look[1] = g_chat_border_opacity, tint[0] = g_chat_tint, tint[1] = g_chat_border_tint,
        rgb[0] = g_chat_tint_rgb, rgb[1] = g_chat_border_tint_rgb;
    else if (g_input_match == 2)
        look[0] = g_input_opacity, look[1] = g_input_border_opacity, tint[0] = g_input_tint, tint[1] = g_input_border_tint,
        rgb[0] = g_input_tint_rgb, rgb[1] = g_input_border_tint_rgb;
    // (its own colour unset: the one every window has, as the chat windows' fall back to it)
    if (!tint[0])
        tint[0] = g_window_tint, rgb[0] = g_window_tint_rgb;
    if (!tint[1])
        tint[1] = g_border_tint, rgb[1] = g_border_tint_rgb;
    static float last_look[2] = {-1.0f, -1.0f};
    static int last_itint[2] = {-1, -1};
    static std::uint32_t last_irgb[2] = {};
    if (last_look[0] != look[0] || last_look[1] != look[1] || last_itint[0] != tint[0] || last_itint[1] != tint[1] ||
        last_irgb[0] != rgb[0] || last_irgb[1] != rgb[1]) {
        last_look[0] = look[0], last_look[1] = look[1], last_itint[0] = tint[0], last_itint[1] = tint[1];
        last_irgb[0] = rgb[0], last_irgb[1] = rgb[1];
        vanacore_gfx_set_input_look(look[0], look[1], tint[0], rgb[0], tint[1], rgb[1]);
    }
    const int input_solid = g_input_match == 1 ? g_chat_border_solid : g_input_match == 2 ? g_input_border_solid : g_border_solid;
    static int last_solid[3] = {-1, -1, -1};
    if (last_solid[0] != g_border_solid || last_solid[1] != g_chat_border_solid || last_solid[2] != input_solid) {
        last_solid[0] = g_border_solid, last_solid[1] = g_chat_border_solid, last_solid[2] = input_solid;
        vanacore_gfx_set_border_solid(g_border_solid, g_chat_border_solid, input_solid);
    }
    publish_corners();
    float input[4] = {};
    bool open = false;
    if (g_input_line != nullptr) {
        __try {
            char* line = *g_input_line;
            char* ctrl = line != nullptr ? *reinterpret_cast<char**>(line + 8) : nullptr;
            open = ctrl != nullptr && *reinterpret_cast<char**>(ctrl + 0xc) == line && chat_window_rect(line, input);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            open = false;
        }
    }
    vanacore_gfx_set_input_rect(open ? input : nullptr);
}

// [client] chat.history: the lines Up and Down bring back in the chat line. The game's history (FsHistory, a member of the
// text input's phrase constructor) is 10 slots of 151 bytes inside the object: slot 0 the line being typed, 1..9 the sent
// ones; +0x5EC the slots' lengths, +0x614 the count of sent lines (add stops it at 9), +0x618 the slot Up and Down are on.
// Its prev and next only move +0x618 between 0 and +0x614, so they stay; the constructor (clears it), add (Enter: shifts
// the slots down), get, set and reset are replaced, and the slots are kept in a deque of their own for each object, as many
// as asked for, with the count written back to the object. An empty line, or the same as the one sent before, isn't kept.
using HistoryText = int(__fastcall*)(char* history, void* edx, const char* text, int length);
using HistoryGet = int(__fastcall*)(char* history, void* edx, char* buffer, int size);
using HistoryReset = int(__fastcall*)(char* history);
using HistoryCtor = void*(__fastcall*)(char* history);
constexpr size_t kHistoryCount = 0x614, kHistorySelected = 0x618;
constexpr int kHistoryLine = 150; // the longest line a slot holds
HistoryCtor g_history_ctor = nullptr;
std::unordered_map<char*, std::deque<std::string>> g_histories;

std::deque<std::string>& history_slots(char* history)
{
    std::deque<std::string>& slots = g_histories[history];
    if (slots.empty())
        slots.emplace_back();
    return slots;
}

void* __fastcall history_ctor(char* history)
{
    g_histories.erase(history);
    return g_history_ctor(history);
}

int __fastcall history_add(char* history, void*, const char* text, int length)
{
    if (length > kHistoryLine)
        return -1;
    std::deque<std::string>& slots = history_slots(history);
    slots.emplace_front(text != nullptr && length > 0 ? std::string(text, static_cast<size_t>(length)) : std::string());
    // the line just sent is slot 1 now
    if (slots.size() > 1 && (slots[1].empty() || (slots.size() > 2 && slots[1] == slots[2])))
        slots.erase(slots.begin() + 1);
    while (slots.size() > static_cast<size_t>(g_history_limit) + 1)
        slots.pop_back();
    *reinterpret_cast<int*>(history + kHistoryCount) = static_cast<int>(slots.size()) - 1;
    return 1;
}

int __fastcall history_get(char* history, void*, char* buffer, int size)
{
    const std::deque<std::string>& slots = history_slots(history);
    const int selected = *reinterpret_cast<int*>(history + kHistorySelected);
    if (buffer == nullptr || size <= 0 || selected < 0 || selected >= static_cast<int>(slots.size()) || slots[selected].empty())
        return 0;
    const std::string& line = slots[selected];
    const size_t count = (std::min)(line.size(), static_cast<size_t>(size) - 1);
    std::memcpy(buffer, line.data(), count);
    buffer[count] = 0;
    return static_cast<int>(count);
}

int __fastcall history_set(char* history, void*, const char* text, int)
{
    if (text == nullptr)
        return 1;
    std::deque<std::string>& slots = history_slots(history);
    const int selected = *reinterpret_cast<int*>(history + kHistorySelected);
    if (selected >= 0 && selected < static_cast<int>(slots.size()))
        slots[selected].assign(text, strnlen(text, kHistoryLine));
    return 1;
}

int __fastcall history_reset(char* history)
{
    history_slots(history).front().clear();
    *reinterpret_cast<int*>(history + kHistorySelected) = 0;
    return 1;
}

// The functions sit together; add's start (push ebx; mov ebx,ecx; push ebp; push edi; lea edi,[ebx+4]; push 54Fh; ...
// push 24h) finds them, and the others' first instructions are checked at their places before any is hooked.
void install_history(HMODULE game)
{
    const std::uintptr_t add = scan::find(game, "53 8B D9 55 57 8D 7B 04 68 4F 05 00 00 8D 83 9B 00 00 00 57 50 E8 ?? ?? ?? ?? "
                                                "8D AB EC 05 00 00 6A 24 8D 8B F0 05 00 00 55 51 E8");
    struct Site {
        std::intptr_t offset;
        std::vector<std::uint8_t> game; // the instructions the hook covers
        void* hook;
    };
    const Site sites[] = {
        {-0x70, {0x8B, 0xD1, 0x33, 0xC0, 0x57, 0x8D, 0x4A, 0x04}, reinterpret_cast<void*>(&history_ctor)},
        {0x240, {0x8B, 0xD1, 0x57, 0xB9, 0x25, 0x00, 0x00, 0x00, 0x33, 0xC0, 0x8D, 0x7A, 0x04}, reinterpret_cast<void*>(&history_reset)},
        {0x110, {0x55, 0x56, 0x8B, 0x74, 0x24, 0x0C, 0x8B, 0xE9, 0x85, 0xF6}, reinterpret_cast<void*>(&history_set)},
        {0x90, {0x8B, 0xD1, 0x56, 0x8B, 0x82, 0x18, 0x06, 0x00, 0x00}, reinterpret_cast<void*>(&history_get)},
        {0, {0x53, 0x8B, 0xD9, 0x55, 0x57, 0x8D, 0x7B, 0x04}, reinterpret_cast<void*>(&history_add)},
    };
    bool found = add != 0;
    for (const Site& site : sites)
        found = found && std::memcmp(reinterpret_cast<const void*>(add + site.offset), site.game.data(), site.game.size()) == 0;
    if (!found) {
        log::write(L"Chat: FFXI's chat line history wasn't found in FFXiMain.dll: [client] chat.history does nothing");
        return;
    }
    for (const Site& site : sites) {
        void* original = imports::detour(reinterpret_cast<void*>(add + site.offset), site.game.size(), site.hook);
        if (original == nullptr) {
            log::write(L"Chat: FFXI's chat line history couldn't be hooked: it may not keep its lines");
            return;
        }
        if (site.offset < 0)
            g_history_ctor = reinterpret_cast<HistoryCtor>(original);
    }
    log::write(L"Chat: chat line history hooked at %ls", describe_address(reinterpret_cast<void*>(add)).c_str());
}

// [client] chat.scrollback: scrolling the chat window up past the rows it holds. The window holds 100 rows (the chat object at
// [0x10575FE8]: +0 the line number of its top row, +4 its bottom row's, +0x10 how many lines there have been, +0x64044 the
// scroller over the saved lines). Scrolling up past the top asks the saved lines (50 to a page, 20 pages, written to TEMP as
// the game goes: 1,000 lines) for the line before the top (0x1012A190), and scrolling down past the bottom for the one after
// it (0x1012A270). The saved lines answer only in their history mode (the pager's byte +0x60; the scroller's enter, 0x1018D680,
// puts its cursor on the last line and sets it, leave, 0x1018D6B0, frees the loaded pages and clears it), which the game
// sets only round its own rebuild of the rows (opening the log, 0x101606E0) and while the log window is open: a window whose
// rows had been rebuilt had nothing behind them, and scrolling up stopped at about 50. Both asks now put the mode on first
// when there's a line to fetch, and leave it on. The game's own enter then has to start from the last line, as with the mode
// off, so an enter finding a mode VanaCore put on leaves it and enters again.
using ScrollbackFetch = char(__fastcall*)(char* ring, void* edx, int kind, int arg);
using ScrollbackMode = int(__fastcall*)(char* scroller);
constexpr size_t kScrollerOffset = 0x64044, kPagerOffset = 0xC, kPagerMode = 0x60;
ScrollbackFetch g_fetch_older = nullptr, g_fetch_newer = nullptr;
ScrollbackMode g_history_enter = nullptr, g_history_leave = nullptr;
char* g_history_ours = nullptr; // the scroller whose history mode VanaCore put on
int g_scrollback_logged = 0;

char* ring_scroller(char* ring)
{
    __try {
        return *reinterpret_cast<char**>(ring + kScrollerOffset);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

// the scroller's saved lines' history mode: 1 on, 0 off, -1 if the scroller isn't as expected
int history_mode(char* scroller)
{
    __try {
        char* pager = scroller != nullptr ? *reinterpret_cast<char**>(scroller + kPagerOffset) : nullptr;
        return pager != nullptr ? *reinterpret_cast<std::uint8_t*>(pager + kPagerMode) != 0 : -1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

// whether the rows have a line to fetch above their top (older) or below their bottom
bool rows_have_more(char* ring, bool older)
{
    __try {
        const int top = *reinterpret_cast<int*>(ring), bottom = *reinterpret_cast<int*>(ring + 4);
        const int total = *reinterpret_cast<int*>(ring + 0x10);
        return older ? top >= 1 : total - 1 > bottom;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// whether the line above the rows' top is within the lines chat.scrollback allows: the lines from it to the last are
// total - top + 1 (the rows' top and the total are line numbers in the saved lines, which drop their oldest page's as it goes)
bool scrollback_allows(char* ring)
{
    __try {
        return *reinterpret_cast<int*>(ring + 0x10) - *reinterpret_cast<int*>(ring) < g_scrollback_lines;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return true;
    }
}

void scrollback_prepare(char* ring, bool older)
{
    if (g_scrollback_lines <= 0 || !rows_have_more(ring, older))
        return;
    char* scroller = ring_scroller(ring);
    if (history_mode(scroller) != 0)
        return;
    g_history_enter(scroller);
    g_history_ours = scroller;
    if (g_scrollback_logged < 8) {
        ++g_scrollback_logged;
        log::write(L"Chat: scrolling %ls the chat window's rows: its saved lines' history mode put on", older ? L"up past" : L"down past");
    }
}

char __fastcall scrollback_older(char* ring, void* edx, int kind, int arg)
{
    if (g_scrollback_lines > 0 && (!scrollback_allows(ring) || chatpages::known_no_older(ring, kind)))
        return 0; // as at the first line (or a tab's first line of its kind): scrolling stops
    scrollback_prepare(ring, true);
    const char found = g_fetch_older(ring, edx, kind, arg);
    chatpages::note_older(ring, kind, found != 0);
    return found;
}

char __fastcall scrollback_newer(char* ring, void* edx, int kind, int arg)
{
    scrollback_prepare(ring, false);
    return g_fetch_newer(ring, edx, kind, arg);
}

int __fastcall scrollback_enter(char* scroller)
{
    if (g_history_ours == scroller) {
        g_history_ours = nullptr;
        if (history_mode(scroller) == 1)
            g_history_leave(scroller);
    }
    return g_history_enter(scroller);
}

int __fastcall scrollback_leave(char* scroller)
{
    if (g_history_ours == scroller)
        g_history_ours = nullptr;
    return g_history_leave(scroller);
}

void install_scrollback(HMODULE game)
{
    // the starts of the four: sub esp,44h; push ebx; push ebp; mov ebp,ecx; push esi; push edi; mov [esp+13h],0 (older reads
    // [ebp] next, newer [ebp+10h]); mov eax,ecx; mov ecx,[eax+0Ch]; mov dl,[ecx+60h] (enter); mov ecx,[ecx+0Ch]; mov al,[ecx+60h] (leave)
    const std::uintptr_t older = scan::find(game, "83 EC 44 53 55 8B E9 56 57 C6 44 24 13 00 8B 45 00 85 C0 7F 0C 32 C0");
    const std::uintptr_t newer = scan::find(game, "83 EC 44 53 55 8B E9 56 57 C6 44 24 13 00 8B 4D 10 8B 45 04 49 3B C1 7C");
    const std::uintptr_t enter = scan::find(game, "8B C1 8B 48 0C 8A 51 60 84 D2 74 06 B8 01 00 00 00 C3 8B 50 08 85 D2 7E");
    const std::uintptr_t leave = scan::find(game, "8B 49 0C 8A 41 60 84 C0 75 06 B8 01 00 00 00 C3 E9 ?? ?? ?? ??");
    if (older == 0 || newer == 0 || enter == 0 || leave == 0) {
        log::write(L"Chat: FFXI's saved chat lines weren't found in FFXiMain.dll: [client] chat.scrollback does nothing");
        return;
    }
    if (!imports::detour(reinterpret_cast<ScrollbackMode>(enter), 10, &scrollback_enter, g_history_enter) ||
        !imports::detour(reinterpret_cast<ScrollbackMode>(leave), 8, &scrollback_leave, g_history_leave) ||
        !imports::detour(reinterpret_cast<ScrollbackFetch>(older), 9, &scrollback_older, g_fetch_older) ||
        !imports::detour(reinterpret_cast<ScrollbackFetch>(newer), 9, &scrollback_newer, g_fetch_newer)) {
        log::write(L"Chat: FFXI's saved chat lines couldn't be hooked: scrolling the chat back may not work");
        return;
    }
    log::write(L"Chat: chat window scrollback hooked at %ls", describe_address(reinterpret_cast<void*>(older)).c_str());
}

void install()
{
    HMODULE game = GetModuleHandleW(L"FFXiMain.dll");
    if (game == nullptr) {
        log::write(L"Chat: FFXiMain.dll isn't loaded: no /addon commands or chat output");
        return;
    }
    const std::uintptr_t parse = scan::find(game, kParsePattern);
    if (parse == 0)
        log::write(L"Chat: FFXI's command line wasn't found in FFXiMain.dll: /addon commands won't work");
    else if (!imports::detour(reinterpret_cast<ParseInput>(parse), kParsePrologue, &parse_input, g_parse))
        log::write(L"Chat: FFXI's command line couldn't be hooked: /addon commands won't work");
    else
        log::write(L"Chat: command line hooked at %ls", describe_address(reinterpret_cast<void*>(parse)).c_str());

    g_line_found = true;
    for (LineSite& site : g_line_sites) {
        site.at = scan::find(game, site.pattern);
        g_line_found = g_line_found && site.at != 0;
    }
    if (!g_line_found)
        log::write(L"Chat: FFXI's chat row spacing wasn't found in FFXiMain.dll: [client] chat.lineheight does nothing");

    const std::uintptr_t colours = scan::find(game, "A0 ?? ?? ?? ?? 33 C9 84 C0 74 ?? 0F BF 54 24 04 25 FF 00 00 00 3B C2");
    if (colours == 0 || !imports::detour(reinterpret_cast<ColourLookup>(colours), 9, &colour_hook, g_colour_lookup))
        log::write(L"Chat: FFXI's text colours weren't found in FFXiMain.dll: [client] chat.timestamps.color does nothing");

    const std::uintptr_t indent = scan::find(game, "A1 ?? ?? ?? ?? 33 C9 66 85 C0 74 ?? 8B 54 24 04 81 E2 FF 00 00 00 0F BF C0 3B C2");
    if (indent == 0 || !imports::detour(reinterpret_cast<IndentLookup>(indent), 7, &indent_hook, g_indent_lookup))
        log::write(L"Chat: FFXI's wrapped line indent wasn't found in FFXiMain.dll: [client] chat.noindent does nothing");

    const std::uintptr_t draw_chat = scan::find(game, "83 EC 0C A1 ?? ?? ?? ?? 89 4C 24 00 53 55 8A 48 08 56 84 C9 57");
    if (draw_chat != 0)
        imports::detour(reinterpret_cast<LogDraw>(draw_chat), 12, &log_draw_hook, g_log_draw);
    log::write(L"Chat: chat window tracking for [client] chat.opacity: %ls", g_log_draw != nullptr ? L"ok" : L"not found");
    g_height_site = scan::find(game, "83 EF 08 8D 4C 24 28 C1 E7 04 66 2B 7E 26");
    if (g_height_site == 0)
        log::write(L"Chat: FFXI's chat window height wasn't found in FFXiMain.dll: it won't follow [client] chat.lineheight");

    // the chat's layout (mov eax,[UI right/bottom]; push esi; push edi; mov edi,[UI left/top] ... its width at +0x32; the
    // resize call at +0x5a), and Config log settings' calls of it, after `mov ecx,[log 1]` and `mov ecx,[log 2]`
    const std::uintptr_t layout = scan::find(game, "A1 ?? ?? ?? ?? 56 57 8B 3D ?? ?? ?? ?? 8B F1 2B C7 05 60 FF FF FF 66 8B 4E 52 66 89 46 32");
    const std::uintptr_t log1 = scan::find(game, "8B 0D ?? ?? ?? ?? 8B 54 24 0C 52 8B 01 FF 50 5C 8B 0D ?? ?? ?? ?? E8");
    const std::uintptr_t log2 = scan::find(game, "8B 0D ?? ?? ?? ?? 56 8B 01 FF 50 5C 8B 0D ?? ?? ?? ?? E8");
    if (layout != 0 && log1 != 0 && *reinterpret_cast<const std::uint8_t*>(layout + 0x5A) == 0xE8 && call_target(log1 + 0x16) == layout) {
        g_ui_end = *reinterpret_cast<const short* const*>(layout + 1);
        g_ui_origin = *reinterpret_cast<const short* const*>(layout + 9);
        g_logs[0].global = *reinterpret_cast<char***>(log1 + 2);
        if (log2 != 0 && call_target(log2 + 0x12) == layout)
            g_logs[1].global = *reinterpret_cast<char***>(log2 + 2);
        g_chat_resize = reinterpret_cast<Resize>(call_target(layout + 0x5A));
        imports::detour(reinterpret_cast<ChatLayout>(layout), 7, &chat_layout_hook, g_chat_layout);
    }
    if (g_chat_layout == nullptr)
        log::write(L"Chat: FFXI's chat window layout wasn't found in FFXiMain.dll: [client] chat.move does nothing");
    // log 2's every-frame place on log 1: Config option 0xb4 (push 0B4h; call), then the call of SetLocate at +0x48
    const std::uintptr_t follow = scan::find(game, "68 B4 00 00 00 E8 ?? ?? ?? ?? 83 C4 04 48 74 ?? 48 75 ?? 0F BF 44 24 1A 0F BF 54 24 12 "
                                                   "0F BF 4C 24 18 2B C2 0F BF 54 24 0E EB ?? 0F BF 44 24 0E 0F BF 54 24 12 0F BF 4C 24 14 "
                                                   "2B C2 0F BF 54 24 16 03 C2 50 51 8B 4E 08 E8");
    if (follow != 0 && g_logs[1].global != nullptr) {
        g_config_value = reinterpret_cast<ConfigValue>(call_target(follow + 5));
        g_set_locate = reinterpret_cast<SetLocate>(call_target(follow + 0x48));
        redirect_call(follow + 0x48, &log2_follow);
    } else {
        log::write(L"Chat: FFXI's second chat window wasn't found in FFXiMain.dll: [client] chat2.move does nothing");
    }
    // the compass's setters of its left (+0x28) and bottom (+0x2a): mov eax,[compass]; test eax,eax; je; mov cx,[esp+4];
    // mov [eax+28h],cx; ret
    const std::uintptr_t compass = scan::find(game, "A1 ?? ?? ?? ?? 85 C0 74 09 66 8B 4C 24 04 66 89 48 28 C3");
    const std::uintptr_t compass_bottom = scan::find(game, "A1 ?? ?? ?? ?? 85 C0 74 09 66 8B 4C 24 04 66 89 48 2A C3");
    if (compass != 0) {
        g_compass = *reinterpret_cast<char***>(compass + 1);
        if (compass_bottom != 0 && *reinterpret_cast<char***>(compass_bottom + 1) == g_compass &&
            imports::detour(reinterpret_cast<CompassSet>(compass_bottom), 5, &compass_bottom_hook, g_compass_set_bottom))
            imports::detour(reinterpret_cast<CompassSet>(compass), 5, &compass_left_hook, g_compass_set_left);
    }
    if (g_compass_set_left == nullptr)
        log::write(L"Chat: FFXI's compass wasn't found in FFXiMain.dll: [client] chat.compass and compass.move do nothing");
    const std::uintptr_t compass_switch = scan::find(game, "33 C0 66 8B 81 ?? ?? ?? ?? 48 3D E3 00 00 00");
    if (compass_switch != 0 && *reinterpret_cast<const std::uint8_t*>(compass_switch + 0x24) == 1) {
        g_compass_switch = compass_switch + 0x24;
        apply_compass_hide(); // compass.hide as read before this
    } else {
        log::write(L"Chat: the compass's switch wasn't found in FFXiMain.dll (or isn't as the game has it): [client] "
                   L"compass.hide does nothing");
    }
    // the clock's draw: mov al,[ecx+2Eh]; sub esp,40h; test al,al; push ebx/esi/edi; je; and its YkDrawString call 5Eh in
    // (push 80808080h; lea edx,[edi+1Ah]; push ecx; lea eax,[ebx-32h]; push edx; push eax; call)
    const std::uintptr_t clock = scan::find(game, "8A 41 2E 83 EC 40 84 C0 53 56 57 0F 84 ?? ?? ?? ?? E8");
    static const std::uint8_t kClockCall[] = {0x68, 0x80, 0x80, 0x80, 0x80, 0x8D, 0x57, 0x1A,
                                              0x51, 0x8D, 0x43, 0xCE, 0x52, 0x50, 0xE8};
    if (clock != 0 && std::memcmp(reinterpret_cast<const void*>(clock + 0x50), kClockCall, sizeof kClockCall) == 0 &&
        imports::detour(reinterpret_cast<ClockDraw>(clock), 6, &clock_draw_hook, g_clock_draw)) {
        g_draw_string = reinterpret_cast<DrawString>(call_target(clock + 0x5E));
        redirect_call(clock + 0x5E, &clock_text);
    } else {
        log::write(L"Chat: the compass's clock wasn't found in FFXiMain.dll: [client] clock.hide and clock.move do nothing");
    }
    install_clock_always(game, clock);
    // where the layout places windows over the chat: their left (movsx ebp,[esp+10h]), and 63h bytes on their top
    // the chat line's mode tab (input.hidetab): mov [esi+2Ch],edi (the mode); mov eax,[esi+30h] (its shape); xor edi,edi;
    // cmp eax,edi; je the end
    if (const std::uintptr_t tab = scan::find(game, "89 7E 2C 8B 46 30 33 FF 3B C7 0F 84 ?? ?? ?? ?? 8D 4C 24 14 66 89 7C 24 14 51 8B 4E 08")) {
        g_tab_site = tab + 10;
        std::memcpy(g_tab_game, reinterpret_cast<const void*>(g_tab_site), 6);
    } else {
        log::write(L"Chat: the chat line's mode tab wasn't found in FFXiMain.dll: [client] input.hidetab does nothing");
    }
    g_menu_left_site = scan::find(game, "0F BF 6C 24 ?? 89 4C 24 24 8B 4F 08 89 44 24 20 E8");
    if (g_menu_left_site != 0 && std::memcmp(reinterpret_cast<const void*>(g_menu_left_site + kMenuPlaceOffset), kMenuPlaceGame,
                                             sizeof kMenuPlaceGame) != 0)
        g_menu_left_site = 0;
    if (g_menu_left_site == 0)
        log::write(L"Chat: FFXI's menu placing over the chat wasn't found in FFXiMain.dll: [client] chat.menus and menus.move do nothing");
    // the layout's manager: `mov ecx, manager` before the walk of its windows that puts them over their anchors
    const std::uintptr_t layout_walk = scan::find(game, "B9 ?? ?? ?? ?? E8 ?? ?? ?? ?? 85 C0 89 44 24 10 74 ?? 8D 54 24 10 B9 ?? ?? ?? ?? 52 E8 "
                                                         "?? ?? ?? ?? 8B F0 85 F6 74 ?? 8B 04 BD");
    if (layout_walk != 0 && g_logs[0].global != nullptr) {
        char* manager = *reinterpret_cast<char**>(layout_walk + 1);
        if (*reinterpret_cast<char***>(manager + 0x8c) == g_logs[0].global) // its first anchor is log 1
            g_layout = manager;
    }
    if (g_layout == nullptr)
        log::write(L"Chat: FFXI's window layout wasn't found in FFXiMain.dll: an open menu moves at its next placing, not as a slider moves");
    // the chat line's object (mov eax,[line]; cmp eax,ebx; je; lea ebx,[eax+18h]; pop edi; mov [esi+0F104h],ebx)
    const std::uintptr_t chat_line = scan::find(game, "A1 ?? ?? ?? ?? 3B C3 74 03 8D 58 18 5F 89 9E 04 F1 00 00");
    if (chat_line != 0)
        g_input_line = *reinterpret_cast<char***>(chat_line + 1);
    else
        log::write(L"Chat: FFXI's chat input line wasn't found in FFXiMain.dll: [client] input.move does nothing");
    // the game's draw of a window: sub esp,24h; push ebx; push ebp; push esi; push edi; mov ebp,ecx; call; mov ax,[ebp+56h]
    const std::uintptr_t ctrl_draw = scan::find(game, "83 EC 24 53 55 56 57 8B E9 E8 ?? ?? ?? ?? 66 8B 45 56 33 FF 66 3B C7 7E 05 48 66 89 45 56");
    if (ctrl_draw == 0 || !imports::detour(reinterpret_cast<CtrlDraw>(ctrl_draw), 5, &ctrl_draw_hook, g_ctrl_draw))
        log::write(L"Chat: FFXI's window draw wasn't found in FFXiMain.dll: [client] input.hide does nothing");
    // the chat window's calc: mov eax,[global]; sub esp,24h; push ebp; push esi; mov esi,ecx; mov cl,[eax+8]; test cl,cl; je
    const std::uintptr_t chat_calc = scan::find(game, "A1 ?? ?? ?? ?? 83 EC 24 55 56 8B F1 8A 48 08 84 C9 0F 84 ?? ?? ?? ?? 8B 16 8B CE FF 52 4C");
    if (chat_calc == 0 || !imports::detour(reinterpret_cast<ChatCalc>(chat_calc), 5, &chat_calc_hook, g_chat_calc))
        log::write(L"Chat: FFXI's chat window calc wasn't found in FFXiMain.dll: the chat window can bounce open under "
                   L"fancychat, and [client] chat.hide and chat2.hide do nothing");
    // the calc's cancel-key check: mov ecx,..; call; test al,al; jne; push -1; push 1; push 95h; push 3Bh; call (+0x1d)
    const std::uintptr_t cancel = scan::find(game, "B9 ?? ?? ?? ?? E8 ?? ?? ?? ?? 84 C0 0F 85 ?? ?? ?? ?? 6A FF 6A 01 68 95 00 00 00 6A 3B "
                                                   "E8 ?? ?? ?? ?? 83 C4 10 84 C0 E9");
    if (g_chat_calc != nullptr && cancel != 0 && cancel - chat_calc == 0x521) {
        g_key_check = reinterpret_cast<KeyCheck>(call_target(cancel + 0x1d));
        redirect_call(cancel + 0x1d, &chat_cancel_check);
    } else {
        log::write(L"Chat: FFXI's chat window cancel key wasn't found in FFXiMain.dll: Escape can open the chat window a "
                   L"step under fancychat");
    }
    install_history(game);
    install_scrollback(game);

    g_menu_found = true;
    for (MenuSite& site : g_menu_sites) {
        site.at = scan::find(game, site.pattern);
        g_menu_found = g_menu_found && site.at != 0;
    }
    const std::uintptr_t open_at = scan::find(game, "8B CE E8 ?? ?? ?? ?? 8B 4E 08 6A FF 53 E8");
    const std::uintptr_t draw_at = scan::find(game, "51 8B 4E 08 52 8B F8 E8 ?? ?? ?? ?? 8B 44 24 16 8B 4C 24 14");
    const std::uintptr_t resize_at = scan::find(game, "66 8B 86 40 05 00 00 51 8B 4E 08 50 E8 ?? ?? ?? ??");
    const std::uintptr_t rect_at = scan::find(game, "8B 4E 08 8D 86 40 05 00 00 50 E8 ?? ?? ?? ??");
    g_menu_found = g_menu_found && open_at != 0 && draw_at != 0 && resize_at != 0 && rect_at != 0;
    if (g_menu_found) {
        const std::uintptr_t locate = call_target(draw_at + 7);
        static const std::uint8_t kLocatePrologue[8] = {0x8B, 0x44, 0x24, 0x08, 0x56, 0x50, 0x8B, 0xF1};
        g_menu_found = std::memcmp(reinterpret_cast<const void*>(locate), kLocatePrologue, sizeof kLocatePrologue) == 0 &&
                       imports::detour(reinterpret_cast<Locate>(locate), sizeof kLocatePrologue, &locate_hook, g_locate);
        if (g_menu_found) {
            g_fit = reinterpret_cast<Fit>(call_target(open_at + 2));
            g_resize = reinterpret_cast<Resize>(call_target(resize_at + 12));
            g_get_rect = reinterpret_cast<GetRect>(call_target(rect_at + 10));
            const std::uintptr_t find = scan::find(game, "51 56 8D 71 14 57 8B CE E8 ?? ?? ?? ?? 85 C0 89 44 24 08 74 25 66 8B 7C 24 10 8D 44 24 08 8B CE 50 E8 ?? ?? ?? ?? 66 83 38 01 75 ?? 66 39 78 3A 74");
            if (find != 0)
                imports::detour(reinterpret_cast<FindButton>(find), 8, &find_button_hook, g_find_button);
            const std::uintptr_t jump = scan::find(game, "66 8B 54 24 04 56 0F BF 71 18 0F BF C2 57 8D 78 FF 3B F7 5F 5E 7E ?? 66 85 D2 74 ?? "
                                                         "8B 49 04 8D 04 80 8D 44 81 EC 8B 00 66 39 50 12 75 ?? 0F BF 4C 24 08 49 83 F9 03 77");
            if (jump != 0)
                imports::detour(reinterpret_cast<JumpTag>(jump), 6, &jump_tag_hook, g_jump_tag);
            g_menu_found = g_find_button != nullptr && g_jump_tag != nullptr;
            // sub esp,8; push ebx; push ebp; push esi; mov esi,ecx; mov ecx,[mouse]; push edi; mov [esp+10h],esi; fld [ecx+48h]
            const std::uintptr_t on_button = scan::find(game, "83 EC 08 53 55 56 8B F1 8B 0D ?? ?? ?? ?? 57 89 74 24 10 D9 41 48 D8 1D "
                                                              "?? ?? ?? ?? DF E0 25 00 41 00 00");
            if (on_button == 0 || !imports::detour(reinterpret_cast<OnButton>(on_button), 8, &on_button_hook, g_on_button))
                log::write(L"Chat: FFXI's mouse pick of a menu's button wasn't found in FFXiMain.dll: the mouse picks only a selection menu's first 3 rows");
            const std::uintptr_t place_at = scan::find(game, "8B 44 24 04 85 C0 74 33 8B 51 14 85 D2 74 2C 56 8B 70 0C 8B 36 66 8B 76 06");
            if (place_at != 0)
                imports::detour(reinterpret_cast<PlaceCursor>(place_at), 6, &place_cursor_hook, g_place_cursor);
            const std::uintptr_t cursor_at = scan::find(game, "83 EC 08 56 57 8B 7C 24 14 8B F1 66 85 FF 7F 05 BF 01 00 00 00 66 8B 46 58");
            if (cursor_at != 0 && *reinterpret_cast<const std::uint8_t*>(cursor_at + 0x76) == 0xE8)
                g_reposition = reinterpret_cast<Reposition>(call_target(cursor_at + 0x76));
            redirect_call(open_at + 2, &menu_opened);
            redirect_call(draw_at + 7, &draw_locate);
        }
    }
    if (!g_menu_found)
        log::write(L"Chat: FFXI's selection menu wasn't found in FFXiMain.dll: [client] menu.rows does nothing");

    const std::uintptr_t input = scan::find(game, kInputPattern);
    if (input == 0)
        log::write(L"Chat: FFXI's chat line wasn't found in FFXiMain.dll: no copy and paste");
    else
        g_input = *reinterpret_cast<void***>(input + kInputOffset);

    const std::uintptr_t writer = scan::find(game, kWritePattern);
    if (writer == 0) {
        log::write(L"Chat: FFXI's chat log wasn't found in FFXiMain.dll: addon messages go to the log only");
    } else {
        g_write = reinterpret_cast<WriteLine>(writer);
        g_chat_log = *reinterpret_cast<void***>(writer + kWriteLogOffset);
        log::write(L"Chat: chat log writer at %ls", describe_address(reinterpret_cast<void*>(writer)).c_str());
        // its call into the chat log's add-line method: every line coming in (text events)
        const auto* call = reinterpret_cast<const std::uint8_t*>(writer + kWriteCall);
        std::int32_t relative = 0;
        std::memcpy(&relative, call + 1, 4);
        const std::uintptr_t method = writer + kWriteCall + 5 + relative;
        const auto* start = reinterpret_cast<const std::uint8_t*>(method);
        if (call[0] != 0xE8 || start[0] != 0x81 || start[1] != 0xEC)
            log::write(L"Chat: the chat log's add-line method isn't as expected: no incoming text events");
        else if (!imports::detour(reinterpret_cast<AddLine>(method), kAddLinePrologue, &add_line, g_add_line))
            log::write(L"Chat: the chat log's add-line method couldn't be hooked: no incoming text events");
        else
            log::write(L"Chat: incoming text hooked at %ls", describe_address(reinterpret_cast<void*>(method)).c_str());
    }
}

} // namespace

std::string colour(int code, std::string_view text)
{
    std::string out = "\x1E";
    out += static_cast<char>(code);
    out += text;
    out += "\x1E\x01";
    return out;
}

std::string header(std::string_view name)
{
    return "\x1E\x51[\x1E\x06" + std::string(name) + "\x1E\x51]\x1E\x01 ";
}

void install_log(const wchar_t* install_dir, const wchar_t* ini)
{
    g_chatlog_dir = install_dir != nullptr ? install_dir : L"";
    if (!g_chatlog_dir.empty() && g_chatlog_dir.back() != L'\\')
        g_chatlog_dir += L'\\';
    if (!g_chatlog_dir.empty())
        g_chatlog_dir += L"logs\\";
    g_chatlog_ini = ini != nullptr ? ini : L"";
    winimage::install(ini, install_dir); // [client] window.image: a picture in place of the window backgrounds (their look's here)
}

void set_command_handler(CommandHandler handler)
{
    g_handler = handler;
}

void set_text_handler(TextHandler handler)
{
    g_text_handler = handler;
}

void write(int mode, std::string_view text, std::string_view source)
{
    log::write(L"%hs: %hs", std::string(source).c_str(), plain(text).c_str());
    size_t start = 0;
    while (start <= text.size()) {
        size_t end = text.find('\n', start);
        if (end == std::string_view::npos)
            end = text.size();
        std::string_view line = text.substr(start, end - start);
        if (!line.empty() && line.back() == '\r')
            line.remove_suffix(1);
        // Pieces of at most kMaxLine bytes, never splitting a character or colour code, and a line too long for one
        // broken at its last space (the space left out) rather than inside a word, unless that leaves under half a piece.
        do {
            size_t length = 0, space = 0;
            while (length < line.size()) {
                const size_t unit = unit_length(line, length);
                if (length + unit > kMaxLine)
                    break;
                if (unit == 1 && line[length] == ' ')
                    space = length;
                length += unit;
            }
            if (length == 0 && !line.empty())
                length = line.size() < kMaxLine ? line.size() : kMaxLine;
            size_t skip = 0;
            if (length < line.size() && space >= kMaxLine / 2) {
                length = space;
                skip = 1;
            }
            if (g_waiting.size() >= kMaxWaiting) {
                g_waiting.pop_front();
                if (!g_lines_dropped)
                    log::write(L"Chat: more than %zu lines waiting for the chat log; dropping the oldest", kMaxWaiting);
                g_lines_dropped = true;
            }
            g_waiting.emplace_back(mode, std::string(line.substr(0, length)));
            line.remove_prefix(length + skip);
        } while (!line.empty());
        start = end + 1;
    }
    flush();
}

void echo(std::string_view text, std::string_view source)
{
    write(kDefaultMode, header(source) + std::string(text), source);
}

void run(int mode, std::string line)
{
    g_commands.emplace_back(mode, std::move(line));
}

void on_frame()
{
    if (!g_installed) {
        g_installed = true;
        install();
    }
    watch_logs_before(); // (before anything of VanaCore's moves a log)
    chatlog_refresh();
    for (int index = 0; index < 2; ++index)
        if (log_moved(index))
            place_chat(index); // a new chat window (logging in, log 2 turned on) or UI area: put in place again
    watch_logs_after();
    place_compass();
    keep_clock();
    const ULONGLONG tick = GetTickCount64();
    if (tick - g_input_hide_asked >= 500) {
        g_input_hide_asked = tick;
        const int bits = addons::ask("fancychat", kFancyChatAsk);
        g_input_hide_addon = (bits & 1) != 0;
        g_chat_pinned = (bits & 2) != 0 && g_chat_calc != nullptr;
        unlatch_legacy();
        set_place_rests((bits & 8) != 0);
        int left = 0, top = 0, right = 0, bottom = 0;
        g_raise_over = g_menus_ontop && g_ctrl_draw != nullptr && uispace::area(left, top, right, bottom);
        hold_slide(g_raise_over && (bits & 4) != 0);
    }
    // the bar turned off, fancychat unloaded, or an addon's hide dropped (any frame): back on the screen
    const bool hidden_now = input_hidden();
    if (g_input_was_hidden && !hidden_now)
        place_input(!g_input_move);
    g_input_was_hidden = hidden_now;
    find_over();
    if (g_input_move && !input_hidden())
        place_input(false); // it's made again at the game's place each time it opens (hidden, the draw hook keeps it off the screen)
    g_menu_anchor = g_menu_patched && g_logs[0].global != nullptr ? *g_logs[0].global : nullptr;
    g_menu_anchor2 = g_menu_patched && g_logs[1].global != nullptr ? *g_logs[1].global : nullptr;
    int menu_x = 0, menu_bottom = 0;
    if (g_menu_custom && ui_point(g_menu_px, g_menu_py, menu_x, menu_bottom))
        g_menu_x = menu_x, g_menu_bottom = menu_bottom;
    replace_menus(); // an open menu goes where the settings now put it
    publish_window_opacity();
    flush();
    if (g_commands.empty())
        return;
    std::deque<std::pair<int, std::string>> commands;
    commands.swap(g_commands);
    // Mode 2 is the game's macro stack (Ashita's QueueCommand(2, line)): last in, first out. An addon that runs a macro's
    // lines through it queues them last line first (XIUI's hotbar does, "queue in reverse order"), so the lines queued
    // together, one after another, run in reverse to be in the macro's order.
    for (auto at = commands.begin(); at != commands.end();) {
        if ((at->first & 0xFF) != 2) {
            ++at;
            continue;
        }
        auto end = at;
        while (end != commands.end() && (end->first & 0xFF) == 2)
            ++end;
        std::reverse(at, end);
        at = end;
    }
    for (const auto& [mode, line] : commands) {
        if (g_parse == nullptr) {
            log::write(L"Chat: can't run \"%hs\": FFXI's command line isn't hooked", plain(line).c_str());
            continue;
        }
        g_injecting = true;
        parse_input(line.c_str(), mode);
        g_injecting = false;
    }
}

void execute(int mode, const std::string& line)
{
    if (g_parse == nullptr || g_in_handler) {
        run(mode, line);
        return;
    }
    const bool injecting = g_injecting;
    g_injecting = true;
    parse_input(line.c_str(), mode);
    g_injecting = injecting;
}

bool ready()
{
    return g_parse != nullptr;
}

void preview(const std::string& key, double value)
{
    const auto within = [value](double low, double high) { return value < low ? low : value > high ? high : value; };
    for (int index = 0; index < 2; ++index) {
        const std::string prefix = index == 0 ? "chat." : "chat2.";
        if (key.compare(0, prefix.size(), prefix) != 0)
            continue;
        const std::string field = key.substr(prefix.size());
        ChatLog& log = g_logs[index];
        if (field == "x")
            log.x = within(0, 100);
        else if (field == "y")
            log.y = within(0, 100);
        else if (field == "width")
            log.width = within(5, 100);
    }
    if (key == "compass.x")
        g_compass_px = within(0, 100);
    else if (key == "compass.y")
        g_compass_py = within(0, 100);
    else if (key == "clock.x")
        g_clock_px = within(0, 100);
    else if (key == "clock.y")
        g_clock_py = within(0, 100);
    else if (key == "menus.x")
        g_menu_px = within(0, 100);
    else if (key == "menus.y")
        g_menu_py = within(0, 100);
    else if (key == "input.x")
        g_input_x = within(0, 100);
    else if (key == "input.y")
        g_input_y = within(0, 100);
}

namespace {

// outline: each window's size as last seen open (the chat windows, the input line), for its box while it isn't
int g_seen_w[3] = {0, 0, 480}, g_seen_h[3] = {100, 100, 22};

// Window `which`'s frame while it's open (0 and 1 the chat windows, 2 the input line)
bool open_rect(int which, outline::Box& box)
{
    __try {
        char* ctrl = nullptr;
        if (which < 2) {
            ctrl = chat_ctrl(which);
        } else if (g_input_line != nullptr && !input_hidden()) {
            char* line = *g_input_line;
            ctrl = line != nullptr ? *reinterpret_cast<char**>(line + 8) : nullptr;
            if (ctrl != nullptr && *reinterpret_cast<char**>(ctrl + 0xc) != line)
                ctrl = nullptr;
        }
        if (ctrl == nullptr)
            return false;
        const short* rect = reinterpret_cast<const short*>(ctrl + 0x3a);
        box = {rect[0], rect[1], rect[2], rect[3]};
        return box.right > box.left && box.bottom > box.top;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// The compass's box: +0x28 is its middle (the clock's text runs out left of it), +0x2a its bottom, +0x2c its height
constexpr int kCompassHalf = 32;

// The compass (3) and the menus over the chat (4): their boxes, where compass.x/y and menus.x/y put them (Custom) or the
// game (or Follow the chat) has them, and a drag sets Custom and X and Y as it goes
bool outline_placed(int which, int ui_left, int ui_top, int ui_width, int ui_height, double* moved_x, double* moved_y)
{
    outline::Box box;
    if (which == 3) {
        char* compass = g_compass != nullptr ? *g_compass : nullptr;
        if (compass == nullptr || g_compass_set_left == nullptr)
            return false;
        __try {
            const int middle = *reinterpret_cast<const short*>(compass + 0x28);
            const int bottom = *reinterpret_cast<const short*>(compass + 0x2a);
            const int high = std::max(16, static_cast<int>(*reinterpret_cast<const short*>(compass + 0x2c)));
            box = {middle - kCompassHalf, bottom - high, middle + kCompassHalf, bottom};
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    } else {
        if (g_menu_left_site == 0)
            return false;
        const int wide = std::max(40, g_menu_seen_w), high = std::max(20, g_menu_seen_h);
        int left = 0, bottom = 0;
        if (g_menu_custom) {
            if (!ui_point(g_menu_px, g_menu_py, left, bottom))
                return false;
        } else {
            // over the top chat window, at its left (Follow the chat) or where the game last put one
            const char* ctrl = chat_ctrl(top_log());
            if (ctrl == nullptr)
                return false;
            left = g_menu_follow || g_menu_seen_top == 0 ? *reinterpret_cast<const short*>(ctrl + 0x3a) : g_menu_seen_left;
            bottom = *reinterpret_cast<const short*>(ctrl + 0x3c) - 2;
        }
        left = std::max(ui_left, std::min(left, ui_left + ui_width - wide));
        bottom = std::max(ui_top + high, std::min(bottom, ui_top + ui_height));
        box = {left, bottom - high, left + wide, bottom};
    }
    outline::Drag drag;
    if (!outline::draw(which == 3 ? "compass" : "menus", box, 0, drag))
        return false;
    if (drag.held) {
        // compass.x is where its middle goes, menus.x a menu's left; both their bottoms by Y
        const int x = drag.left + (which == 3 ? kCompassHalf : 0);
        const double px = std::max(0.0, std::min(100.0, std::round((x - ui_left) * 10000.0 / ui_width) / 100.0));
        const double py =
            std::max(0.0, std::min(100.0, std::round((drag.top + box.bottom - box.top - ui_top) * 10000.0 / ui_height) / 100.0));
        if (which == 3) {
            g_compass_custom = true;
            g_compass_px = px, g_compass_py = py;
        } else {
            g_menu_custom = 1;
            g_menu_px = px, g_menu_py = py;
            apply_menu_patches(true);
        }
        if (drag.released && moved_x != nullptr && moved_y != nullptr)
            *moved_x = px, *moved_y = py;
    } else if (drag.released && moved_x != nullptr && moved_y != nullptr) {
        *moved_x = which == 3 ? g_compass_px : g_menu_px;
        *moved_y = which == 3 ? g_compass_py : g_menu_py;
    }
    return true;
}

// The clock's line (5): where clock.x/y put it (Move on) or where the game draws it under the compass, at the width it last
// had; a drag sets Move and X and Y as it goes (X by the line's alignment)
bool outline_clock(int ui_left, int ui_top, int ui_width, int ui_height, double* moved_x, double* moved_y)
{
    if (g_clock_draw == nullptr)
        return false;
    int left = 0, top = 0;
    if (!g_clock_move || !clock_place(left, top)) {
        char* compass = g_compass != nullptr ? *g_compass : nullptr;
        if (compass == nullptr)
            return false;
        __try {
            left = *reinterpret_cast<const short*>(compass + 0x28) - kClockIcon;
            top = *reinterpret_cast<const short*>(compass + 0x2a) - *reinterpret_cast<const short*>(compass + 0x2c) + kClockDown;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }
    const int wide = kClockIcon - kClockText + g_clock_text_w;
    const outline::Box box = {left, top, left + wide, top + kClockHigh};
    outline::Drag drag;
    if (!outline::draw("clock", box, 0, drag))
        return false;
    if (drag.held) {
        const int x = drag.left + (g_clock_align == 2 ? wide : g_clock_align == 1 ? wide / 2 : 0);
        g_clock_move = true;
        g_clock_px = std::max(0.0, std::min(100.0, std::round((x - ui_left) * 10000.0 / ui_width) / 100.0));
        g_clock_py = std::max(0.0, std::min(100.0, std::round((drag.top - ui_top) * 10000.0 / ui_height) / 100.0));
    }
    if (drag.released && moved_x != nullptr && moved_y != nullptr)
        *moved_x = g_clock_px, *moved_y = g_clock_py;
    return true;
}

} // namespace

bool outline(int which, double* moved_x, double* moved_y)
{
    if (which < 0 || which > 5 || g_ui_origin == nullptr || g_ui_end == nullptr)
        return false;
    const int ui_left = g_ui_origin[0], ui_top = g_ui_origin[1];
    const int ui_width = g_ui_end[0] - ui_left, ui_height = g_ui_end[1] - ui_top;
    if (ui_width <= 0 || ui_height <= 0)
        return false;
    if (which == 5)
        return outline_clock(ui_left, ui_top, ui_width, ui_height, moved_x, moved_y);
    if (which >= 3)
        return outline_placed(which, ui_left, ui_top, ui_width, ui_height, moved_x, moved_y);
    double& x = which < 2 ? g_logs[which].x : g_input_x;
    double& y = which < 2 ? g_logs[which].y : g_input_y;
    outline::Box box;
    // (a log an addon or chat.hide keeps closed is a sliver or off the screen: its box is where its X and Y put it, at the
    // size it had open, as it is for a line that's hidden)
    if (!(which < 2 && chat_log_pinned(which)) && open_rect(which, box)) {
        g_seen_w[which] = box.right - box.left;
        g_seen_h[which] = box.bottom - box.top;
    } else {
        // where X, Y (and a chat window's Width) put it, at the size it last had
        int width = g_seen_w[which];
        if (which < 2) {
            const int text = static_cast<int>(ui_width * g_logs[which].width / 100.0 + 0.5) - 14;
            width = std::max(160, std::min(text, ui_width - 160)) + 14;
        }
        box.left = ui_left + static_cast<int>(ui_width * x / 100.0 + 0.5);
        box.left = std::max(ui_left, std::min(box.left, ui_left + ui_width - width));
        box.bottom = ui_top + static_cast<int>(ui_height * y / 100.0 + 0.5);
        box.bottom = std::max(ui_top + g_seen_h[which], std::min(box.bottom, ui_top + ui_height));
        box.right = box.left + width;
        box.top = box.bottom - g_seen_h[which];
    }
    static const char* const kIds[] = {"chat", "chat2", "input"};
    outline::Drag drag;
    if (!outline::draw(kIds[which], box, 0, drag))
        return false;
    // dragged: the window goes where the box is taken to (its move switch on) at once, by its left and bottom edges
    if (drag.held) {
        x = std::max(0.0, std::min(100.0, std::round((drag.left - ui_left) * 10000.0 / ui_width) / 100.0));
        y = std::max(0.0, std::min(100.0, std::round((drag.top + box.bottom - box.top - ui_top) * 10000.0 / ui_height) / 100.0));
        if (which < 2)
            g_logs[which].move = true;
        else
            g_input_move = true;
    }
    if (drag.released && moved_x != nullptr && moved_y != nullptr) {
        *moved_x = x;
        *moved_y = y;
    }
    return true;
}

bool top_chat(short* rect)
{
    __try {
        const char* ctrl = chat_ctrl(top_log());
        if (ctrl == nullptr)
            return false;
        std::memcpy(rect, ctrl + 0x3a, 4 * sizeof(short));
        return rect[2] > rect[0] && rect[3] > rect[1];
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool log_window(const void* ctrl)
{
    if (ctrl == nullptr)
        return false;
    __try {
        return chat_ctrl(0) == ctrl || chat_ctrl(1) == ctrl;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

namespace {

bool read(void* out, std::uintptr_t address, size_t size)
{
    __try {
        std::memcpy(out, reinterpret_cast<const void*>(address), size);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

} // namespace

std::uintptr_t input_object()
{
    std::uint32_t manager = 0;
    if (g_input == nullptr || !read(&manager, reinterpret_cast<std::uintptr_t>(g_input), 4))
        return 0;
    return manager;
}

std::string parse_auto_translate(std::string_view text, bool brackets)
{
    // The chat manager's parser (thiscall on the manager: output, its size, the text, its length, brackets, 0): reached
    // through the call a caller of it makes (Ashita's chat.parseautotranslate.this pattern, the call 0x22 bytes on)
    using Parse = int(__thiscall*)(void* manager, char* out, int out_size, const char* in, int in_length, int brackets, int zero);
    static Parse parse = nullptr;
    static bool searched = false;
    if (!searched) {
        searched = true;
        const HMODULE game = GetModuleHandleW(L"FFXiMain.dll");
        if (game != nullptr) {
            const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(game);
            const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(reinterpret_cast<const BYTE*>(game) + dos->e_lfanew);
            const std::uintptr_t at = scan::find(reinterpret_cast<std::uintptr_t>(game), nt->OptionalHeader.SizeOfImage,
                                                 "8B ?? 83 C9 FF 33 C0 6A 00 F2 AE F7 D1 49 6A 01 51 8B 0D", 0);
            if (at != 0 && *reinterpret_cast<const BYTE*>(at + 0x22) == 0xE8) {
                std::int32_t rel = 0;
                std::memcpy(&rel, reinterpret_cast<const void*>(at + 0x23), 4);
                parse = reinterpret_cast<Parse>(at + 0x27 + rel);
            }
        }
        if (parse == nullptr)
            log::write(L"Chat: the game's auto-translate parser wasn't found; phrases stay as they are");
    }
    const std::uintptr_t manager = input_object();
    if (parse == nullptr || manager == 0 || text.empty())
        return std::string(text);
    std::string in(text);
    char out[2048] = {};
    const int length = parse(reinterpret_cast<void*>(manager), out, static_cast<int>(sizeof out), in.c_str(),
                             static_cast<int>(in.size()), brackets ? 1 : 0, 0);
    if (length < 0)
        return in;
    return std::string(out, strnlen(out, sizeof out));
}

bool input_bar_addon()
{
    return g_input_hide_addon;
}

bool chat_addon_pinned()
{
    return g_chat_pinned;
}

bool raising()
{
    return g_raise_over;
}

int ui_bottom() { return g_ui_end != nullptr ? g_ui_end[1] : 0; }

std::uint32_t input_state()
{
    std::uint32_t manager = 0, state = 0;
    if (g_input == nullptr || !read(&manager, reinterpret_cast<std::uintptr_t>(g_input), 4) || manager == 0 ||
        !read(&state, manager + kInputState, 4))
        return 0;
    return state;
}

bool input_line(bool& open, std::string& text)
{
    open = false;
    text.clear();
    std::uint32_t manager = 0, state = 0, length = 0;
    if (g_input == nullptr || !read(&manager, reinterpret_cast<std::uintptr_t>(g_input), 4) || manager == 0 ||
        !read(&state, manager + kInputState, 4) || !read(&length, manager + kInputLength, 4))
        return false;
    open = state == 4 || state == 5; // typing: the game's own check (FUN_101b43f0); 1, 2 and 3 are the line closed
    char buffer[kInputCapacity];
    if (length > kInputCapacity || !read(buffer, manager + kInputText, length))
        return true;
    text.assign(buffer, strnlen(buffer, length));
    return true;
}

void menu_state(const void*& window, int& rows, int& setting, unsigned long long& since_ms)
{
    window = g_menu_ctrl;
    rows = g_menu_want;
    setting = g_menu_rows;
    since_ms = GetTickCount64() - g_menu_seen;
}

} // namespace vanacore::chat
