#include "window.hpp"

#include <tlhelp32.h>
#include <windowsx.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cwchar>
#include <string>
#include <vector>

#include "addons.hpp"
#include "binds.hpp"
#include "camera.hpp"
#include "chat.hpp"
#include "clipboard.hpp"
#include "game.hpp"
#include "graphics.hpp"
#include "gui.hpp"
#include "hideui.hpp"
#include "imports.hpp"
#include "ini.hpp"
#include "input.hpp"
#include "log.hpp"
#include "soulspire/soulspire.hpp"
#include "mousediag.hpp"
#include "overlay.hpp"
#include "packets.hpp"
#include "plugins.hpp"
#include "registry.hpp"
#include "screenshot.hpp"
#include "shutdown.hpp"
#include "ui.hpp"
#include "uiscale.hpp"
#include "mapwin.hpp"

namespace vanacore::window {
namespace {

HWND g_window = nullptr;
WNDPROC g_original = nullptr;
bool g_captured = false; // a plugin or addon took a press: the drag stays with the window, its moves
                          // reach the game without the buttons

// [ffxi] mode: FFXI's four, as its window mode setting (0034) numbers them. Unset: that setting's.
enum class Mode { Fullscreen, Windowed, Borderless, BorderlessFullscreen, Unset };
const wchar_t* const kModeNames[] = {L"full screen", L"windowed", L"borderless windowed", L"borderless full screen"};

struct Layout {
    Mode mode = Mode::Unset;
    int width = 0, height = 0; // the client area; 0 as the game made it
    int kept_width = 0, kept_height = 0; // [window] lastsize: the client area its edges were dragged to; 0 none
    bool resizable = false;
    bool topmost = false;
    bool operator==(const Layout&) const = default;
};
Layout g_layout;            // as applied
bool g_applied = false;
bool g_mouse_scale = true;  // mouse.scale
bool g_keep_awake = false;  // [window] keepawake, as told to Windows (keep_awake below)
bool g_keep_awake_told = false;
bool g_close_shutdown = true; // [window] close.shutdown
ULONGLONG g_close_asked = 0;  // when a close last ran /shutdown (GetTickCount64), 0 never
ULONGLONG g_close_passed = 0; // when a close with no character in the game was last left to the game, 0 never
ULONGLONG g_press_last = 0;   // when the last close was counted as a press (GetTickCount64), 0 never
int g_press_count = 0;        // closes in a row, each within kPressGap of the one before
bool g_close_added = false;  // Close put back in the window's menu (close_menu)
LONG g_game_style = 0;      // the window's style and extended style as the game made it
LONG g_game_ex_style = 0;
RECT g_before_fullscreen{}; // where the window was before it covered its monitor

void remember(); // [window] position=last: where the window is now, kept (below)
SIZE g_size_before{}; // the client size as a move or size of the window began (WM_ENTERSIZEMOVE)
bool g_moving = false; // in Windows' move or size loop (an edge or the title bar held)
void remember_size(); // [window] resizable: the size the window's edges were dragged to, kept (below)

// A client position (window pixels) in `width` x `height` pixels.
POINT scaled(HWND window, POINT at, unsigned width, unsigned height)
{
    RECT client{};
    if (width != 0 && height != 0 && GetClientRect(window, &client) && client.right > 0 && client.bottom > 0) {
        at.x = MulDiv(at.x, static_cast<int>(width), client.right);
        at.y = MulDiv(at.y, static_cast<int>(height), client.bottom);
    }
    return at;
}

// A mouse message's client position as the game should see it: in its own resolution (the back
// buffer's), where the window's client area is another size. The game has no way to know the
// window's size (it never asks), so without this it would take the window's pixels for its own.
LPARAM to_game(HWND window, LPARAM lparam)
{
    unsigned width = 0, height = 0;
    ui::game_screen(width, height);
    if (!g_mouse_scale)
        return lparam;
    const POINT at = scaled(window, {GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)}, width, height);
    return MAKELPARAM(static_cast<WORD>(static_cast<short>(at.x)), static_cast<WORD>(static_cast<short>(at.y)));
}

bool held(int key)
{
    return (GetKeyState(key) & 0x8000) != 0;
}

// Print Screen is a key bind (binds.cpp: /bind printscreen /screenshot, as Ashita has it), not a hook of its own. It had two: a
// low-level one on a thread of its own, which left Windows' own screen capture opening over the game anyway, and with Wine
// a second low-level keyboard hook behind the game's let a letter reach the game before the Shift pressed with it (Linux /
// Proton, Wine's +hook log, 2026-10-25).

// The foreground window is one of this process's (the game's window, or a box of its own)
bool process_in_front()
{
    const HWND front = GetForegroundWindow();
    if (front == nullptr)
        return false;
    DWORD process = 0;
    GetWindowThreadProcessId(front, &process);
    return process == GetCurrentProcessId();
}

// ---- The mouse over VanaCore's own windows ----------------------------------------------------
// A press, release or wheel turn over a box that keeps the mouse from the game (the /vana window,
// ui::blocks_mouse), and the release of a press that began over one, never reach the game at all: a
// low-level mouse hook takes them before Windows queues them for any window, and hands them to
// VanaCore's windows as a message of its own (kUiMouse). The game reads the mouse in its own message
// loop, before any window procedure, so taking the click there is the only way it can't see it,
// whatever it reads (messages, the key state, DirectInput). Movement always goes through: the
// cursor moves as ever. The hook runs on a thread of its own, so the game's busy moments (loading a
// zone) never hold up the mouse.

constexpr UINT kUiMouse = WM_APP + 0x3A7; // wparam: the mouse message, the wheel's turn above it; lparam: client x, y
// FFXiMain's "run a frame now": a thread of its own posts it every 333 ms (0x10001C00). The game's loop takes it from the
// queue itself; only another message loop (Windows moving or sizing the window) hands it to the window procedure, which
// runs a whole frame of the game for it (0x10016A93 -> 0x10001B50, no guard against running inside one).
constexpr UINT kGameTick = WM_USER + 1;
// Borderless windowed with resizable edges: the window has no frame at all (Windows draws a sizing
// frame without a title bar as a white bar along the top), the whole of it is the picture, and its
// edges are found in the picture's outer pixels (edge_at).
std::atomic<bool> g_edges_inside{false};
std::atomic<bool> g_was_in_front{false}; // the game's window has been the foreground window
std::atomic<unsigned> g_ui_buttons{0};     // buttons pressed over VanaCore's windows and not yet let go
bool g_after_modal = false; // a modal window over the game closed with a button held (a double click): its release isn't the game's
bool g_unstick = false;     // ... closed: once every button is up, a button the game still holds is let go for it (release_stuck)
// The buttons the game's window procedure was given the press of and not the release yet (bit 1 left, 2 right, 4 middle, 8
// X). The game TOGGLES a button's held state on each press or release message it gets (FFXiMain+0x12452F: held ^= 1), so
// a release whose press went elsewhere (the Open dialog closed on the double click's press) turned the button on: held,
// the camera stuck, clicks going through.
unsigned g_game_down = 0;

// A button message's bit in g_game_down
unsigned game_bit(UINT message)
{
    return message <= WM_LBUTTONDBLCLK ? 1u : message <= WM_RBUTTONDBLCLK ? 2u : message <= WM_MBUTTONDBLCLK ? 4u : 8u;
}

unsigned g_capture_buttons = 0; // the mouse hook's: buttons the keybinds window's capture took, their releases taken too
bool g_let_go = false; // release_game_buttons: the game was told the buttons are up; its moves show none until the release
std::atomic<bool> g_block_mouse{false};          // vana.window.block_mouse: no mouse button reaches the game
std::atomic<ULONGLONG> g_block_mouse_until{0};   // ... off: nor until then and every button is up (GetTickCount64)

bool any_button_down()
{
    for (int key : {VK_LBUTTON, VK_RBUTTON, VK_MBUTTON, VK_XBUTTON1, VK_XBUTTON2})
        if ((GetAsyncKeyState(key) & 0x8000) != 0)
            return true;
    return false;
}
HHOOK g_mouse_hook = nullptr;
DWORD g_hook_thread = 0; // hook_thread's id

constexpr UINT kToFront = WM_APP + 0x3A8; // to hook_thread: bring the game's window to the front
std::atomic<bool> g_logged_to_front{false};

// The game's window to the front, for a press over VanaCore's windows the mouse hook took while it was in
// the background: Windows brings a window forward on the press it's clicked with, and this one never got
// it. Windows lets the process that gave it the last input take the foreground: an empty mouse input first
// (no movement, no buttons).
void to_front()
{
    const HWND window = g_window;
    if (window == nullptr || GetForegroundWindow() == window)
        return;
    INPUT nothing{};
    nothing.type = INPUT_MOUSE;
    SendInput(1, &nothing, sizeof nothing);
    if (!SetForegroundWindow(window) && !g_logged_to_front.exchange(true))
        log::write(L"Window: couldn't bring the game to the front for a click on VanaCore's windows (Windows refused it)");
}

unsigned button_of(WPARAM message, DWORD data)
{
    switch (message) {
    case WM_LBUTTONDOWN: case WM_LBUTTONUP: return MK_LBUTTON;
    case WM_RBUTTONDOWN: case WM_RBUTTONUP: return MK_RBUTTON;
    case WM_MBUTTONDOWN: case WM_MBUTTONUP: return MK_MBUTTON;
    case WM_XBUTTONDOWN: case WM_XBUTTONUP: return HIWORD(data) == XBUTTON2 ? MK_XBUTTON2 : MK_XBUTTON1;
    default: return 0;
    }
}

LRESULT CALLBACK mouse_hook(int code, WPARAM message, LPARAM info)
{
    const HWND window = g_window;
    if (code != HC_ACTION || window == nullptr)
        return CallNextHookEx(nullptr, code, message, info);
    const auto* event = reinterpret_cast<const MSLLHOOKSTRUCT*>(info);
    const bool press = message == WM_LBUTTONDOWN || message == WM_RBUTTONDOWN || message == WM_MBUTTONDOWN ||
                       message == WM_XBUTTONDOWN;
    const bool release = message == WM_LBUTTONUP || message == WM_RBUTTONUP || message == WM_MBUTTONUP ||
                         message == WM_XBUTTONUP;
    const bool wheel = message == WM_MOUSEWHEEL || message == WM_MOUSEHWHEEL;
    if (!press && !release && !wheel)
        return CallNextHookEx(nullptr, code, message, info);
    // The game in the background: a press or wheel turn is only its when its window is the one under the
    // mouse (not another in front of it). A press there over VanaCore's windows is taken too: it's the
    // click that brings the game back (to_front), and the game mustn't see it either.
    const bool in_front = GetForegroundWindow() == window;
    // A modal window over the game (an addon's file dialog) disables it: a press or wheel turn is the modal's, even when
    // Windows left the game as the foreground window and VanaCore's windows lie under the dialog (a double click on a file
    // was taken for them and never reached the dialog, which held the game's thread). A release still goes the way its
    // press did, below.
    if (!release && !IsWindowEnabled(window))
        return CallNextHookEx(nullptr, code, message, info);
    if (!in_front && !release && GetAncestor(WindowFromPoint(event->pt), GA_ROOT) != GetAncestor(window, GA_ROOT))
        return CallNextHookEx(nullptr, code, message, info);
    POINT at = event->pt;
    RECT client{};
    bool over = false;
    const bool inside = ScreenToClient(window, &at) && GetClientRect(window, &client) && PtInRect(&client, at);
    if (inside) {
        unsigned width = 0, height = 0;
        ui::screen(width, height);
        const POINT in_ui = scaled(window, at, width, height);
        over = window::mouse_blocked() || ui::blocks_mouse(in_ui.x, in_ui.y);
        // a left press on a map's box is VanaCore's ([client] map.fullscreen=0, regionmap.fullscreen=0: it drags the map)
        if (message == WM_LBUTTONDOWN && !over)
            over = mapwin::takes_press(in_ui.x, in_ui.y);
        // A wheel turn over Ashita's Minimap zooms its map: the plugin's alone. A plugin hears of it from the window's
        // messages, after the game may have read it through DirectInput (and zoomed the camera), so it's taken here, with the
        // mouse over VanaCore's windows, and handed to the plugins from the game's thread.
        if (wheel && !over) {
            unsigned game_width = 0, game_height = 0;
            ui::game_screen(game_width, game_height);
            const POINT in_game = scaled(window, at, game_width, game_height);
            over = graphics::plugin_wheel_over(in_game.x, in_game.y);
        }
        // a wheel turn over the in-game map zooms it ([client] map.wheel): the map's, not the camera's
        if (wheel && !over)
            over = mapwin::wheel_over(in_ui.x, in_ui.y);
    }
    const unsigned button = button_of(message, event->mouseData);
    // The keybinds window's "press a key" (binds::capture_mouse): a mouse button pressed, or the wheel turned or tilted,
    // over the game in front is its answer, over the capture box as anywhere, and neither it nor a button's release goes
    // any further
    if ((press || wheel) && in_front && inside && binds::capture_mouse(static_cast<UINT>(message), event->mouseData)) {
        g_capture_buttons |= button;
        return 1;
    }
    if (release && (g_capture_buttons & button) != 0) {
        g_capture_buttons &= ~button;
        return 1;
    }
    // A wheel bind (/bind wheelup ..., ^wheeldown, wheelleft): the turn is taken here, where the game's DirectInput mouse
    // never sees it, and the command runs on the game's thread. What's over VanaCore's windows (or a plugin's map) is
    // theirs first, as a button's press is.
    if (wheel && !over && in_front && inside && binds::take_wheel(static_cast<UINT>(message), event->mouseData)) {
        mousediag::on_taken(static_cast<UINT>(message), "a key bind (the wheel)", at.x, at.y);
        return 1;
    }
    bool ours = false;
    if (press && over) {
        g_ui_buttons |= button;
        ours = true;
    } else if (release && (g_ui_buttons & button) != 0) {
        g_ui_buttons &= ~button;
        ours = true;
    } else if (wheel && over) {
        ours = true;
    }
    if (!ours)
        return CallNextHookEx(nullptr, code, message, info);
    if (press && !in_front)
        PostThreadMessageW(GetCurrentThreadId(), kToFront, 0, 0); // once this returns: Windows waits on the hook
    const WPARAM what = (message & 0xFFFF) | (wheel ? (static_cast<WPARAM>(HIWORD(event->mouseData)) << 16) : 0);
    mousediag::on_taken(static_cast<UINT>(message), "VanaCore's mouse hook (over one of its windows)", at.x, at.y);
    PostMessageW(window, kUiMouse, what, MAKELPARAM(static_cast<WORD>(static_cast<short>(at.x)), static_cast<WORD>(static_cast<short>(at.y))));
    return 1; // Windows goes no further with it: no window, no key state, no DirectInput sees it
}

// VanaCore's low-level hook (the mouse over its windows) on a thread of its own.
// Windows runs a low-level hook on the thread that set it, and every mouse move
// waits for it: on the game's thread it'd wait for its next frame (or a zone's loading),
// and the rest of the PC would lag with it. This thread does nothing else.
DWORD WINAPI hook_thread(void*)
{
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&mouse_hook), &self);
    g_mouse_hook = SetWindowsHookExW(WH_MOUSE_LL, &mouse_hook, self, 0);
    log::write(L"Window: Ctrl+V pastes and Ctrl+C copies in the chat line");
    if (g_mouse_hook == nullptr)
        log::write(L"Window: couldn't hook the mouse (error %lu): clicks on VanaCore's windows may reach the game",
                   GetLastError());
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (msg.message == kToFront)
            to_front();
    }
    if (g_mouse_hook != nullptr)
        UnhookWindowsHookEx(g_mouse_hook);
    g_mouse_hook = nullptr;
    return 0;
}

// A mouse message for plugins and addons, as Windower gives it them: type = the message - 0x200 (0
// move, 1 left down, 2 left up, 4 right down, 5 right up, 7 middle down, 8 middle up, 10 wheel),
// where on the screen, the wheel's turn. Plugins get it in the game's pixels (as they draw), addons
// in VanaCore's interface's (ui::screen). True when one took it: the game doesn't see it.
bool mouse(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
    POINT at{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
    const bool wheel = message == WM_MOUSEWHEEL || message == WM_MOUSEHWHEEL;
    if (wheel)
        ScreenToClient(window, &at); // wheel messages say where on the screen
    unsigned width = 0, height = 0, game_width = 0, game_height = 0;
    ui::screen(width, height);
    ui::game_screen(game_width, game_height);
    const POINT in_ui = scaled(window, at, width, height), in_game = scaled(window, at, game_width, game_height);
    const int type = static_cast<int>(message - WM_MOUSEFIRST);
    const int delta = wheel ? GET_WHEEL_DELTA_WPARAM(wparam) : 0;
    const bool press = message == WM_LBUTTONDOWN || message == WM_RBUTTONDOWN || message == WM_MBUTTONDOWN;
    const bool release = message == WM_LBUTTONUP || message == WM_RBUTTONUP || message == WM_MBUTTONUP;
    // ImGui's windows, under VanaCore's own (a press or wheel over one of those is theirs): a press or
    // wheel over one is ImGui's alone, as is the release of a press it took
    const bool by_gui = !((press || wheel) && ui::boxes_block_mouse(in_ui.x, in_ui.y)) &&
                        gui::on_mouse(type, in_ui.x, in_ui.y, delta);
    if (wheel && !by_gui && mapwin::on_wheel(in_ui.x, in_ui.y, delta)) // [client] map.wheel: the map zooms
        return true;
    const bool by_plugins = !by_gui && plugins::on_mouse(type, in_game.x, in_game.y, delta, wparam);
    const bool by_addons = addons::on_mouse(type, in_ui.x, in_ui.y, in_game.x, in_game.y, delta, by_plugins || by_gui,
                                            by_gui ? &gui::taken_by() : nullptr);
    const bool taken = by_addons || by_plugins || by_gui;
    if (taken && (press || release))
        mousediag::on_taken(message, by_gui ? "ImGui" : by_plugins ? "a plugin" : "an addon", in_ui.x, in_ui.y);
    // the game reads the mouse through DirectInput too: what a plugin or addon took, it doesn't get there
    if (taken && !by_gui && (press || wheel))
        input::mouse_taken(press, wheel);
    // A drag that leaves the window still ends here.
    constexpr WPARAM kButtons = MK_LBUTTON | MK_RBUTTON | MK_MBUTTON | MK_XBUTTON1 | MK_XBUTTON2;
    if (taken && press && !g_captured) {
        SetCapture(window);
        g_captured = true;
    } else if (release && g_captured && (wparam & kButtons) == 0) {
        ReleaseCapture();
        g_captured = false;
    }
    return taken;
}

// The edge or corner of the window under `lparam` (screen x, y) for WM_NCHITTEST, as thick as
// Windows' own sizing frame; HTNOWHERE inside them.
LRESULT edge_at(HWND window, LPARAM lparam)
{
    RECT r{};
    if (!GetWindowRect(window, &r))
        return HTNOWHERE;
    const int x = GET_X_LPARAM(lparam), y = GET_Y_LPARAM(lparam);
    const int edge = GetSystemMetrics(SM_CXSIZEFRAME) + GetSystemMetrics(SM_CXPADDEDBORDER);
    const bool left = x < r.left + edge, right = x >= r.right - edge;
    const bool top = y < r.top + edge, bottom = y >= r.bottom - edge;
    if (top)
        return left ? HTTOPLEFT : right ? HTTOPRIGHT : HTTOP;
    if (bottom)
        return left ? HTBOTTOMLEFT : right ? HTBOTTOMRIGHT : HTBOTTOM;
    return left ? HTLEFT : right ? HTRIGHT : HTNOWHERE;
}

// The game ended from its window at once, as the game's own teardown can't: what the game's logout, addons and VanaCore's
// settings would save is saved first (remember, shutdown::end_now), then the process ends. The character stays in the
// world for a while.
[[noreturn]] void end_game(const wchar_t* why)
{
    log::write(L"Window: %ls: ending the game", why);
    remember();
    shutdown::end_now(L"the game is being ended from its window", 5000);
}

// [window] close.shutdown: the window was asked to close (its X, Alt+F4, the taskbar's Close) with a character in the
// game. The window's close would end the game with the character still in the world; /shutdown is the game's own way
// out (the character logged out, the server told). True: /shutdown is run and the close goes no further. A close right
// after that (a double click) is swallowed too. /shutdown does nothing while a menu is open, and the game's own close
// doesn't close the window, so a close a moment later, while the game is shutting down, asks whether to end the game
// at once: yes saves what addons and VanaCore's settings have and ends the process.
//
// Where /shutdown can't work the game is ended at once, with no /shutdown and no question: with the server silent for
// kLost (the connection is gone). With no character in the game (the title screen, the character select, a black screen
// between zones or before the world) the game gets the close first, as it always did - the game's own exit is the right
// one at the title screen - and a second close a moment later, with the game still there, ends it.
//
// Whatever the game is doing, the third close in a row (each within kPressGap of the one before) ends it at once.
bool close_as_shutdown(HWND window)
{
    constexpr ULONGLONG kDoubleClick = 1500; // ms
    constexpr ULONGLONG kShuttingDown = 60000;
    constexpr ULONGLONG kLost = 20000; // the server quiet this long: disconnected (the game's own Failing is 2 s)
    constexpr ULONGLONG kSameClick = 50;  // a close passes through here twice (SC_CLOSE, then the WM_CLOSE sent on from it)
    constexpr ULONGLONG kPressGap = 2000; // closes this close together are one spam
    constexpr int kSpam = 3;
    static bool asking = false;
    if (!g_close_shutdown)
        return false;
    const ULONGLONG now = GetTickCount64();
    if (g_press_last == 0 || now - g_press_last >= kSameClick) {
        g_press_count = g_press_last != 0 && now - g_press_last <= kPressGap ? g_press_count + 1 : 1;
        g_press_last = now;
        if (g_press_count >= kSpam)
            end_game(L"closed three times in a row");
    }
    if (!chat::ready() || game::local_player_name().empty()) {
        // it passes through here twice (SC_CLOSE, then the WM_CLOSE the game sends on), so a close within kDoubleClick
        // of the first is that one, not a second
        if (g_close_passed != 0 && now - g_close_passed >= kDoubleClick && now - g_close_passed < kShuttingDown)
            end_game(L"closed again with no character in the game and the game still here");
        if (g_close_passed == 0 || now - g_close_passed >= kShuttingDown)
            g_close_passed = now;
        return false;
    }
    if (packets::server_silence() >= kLost)
        end_game(L"closed with the server silent (disconnected), where /shutdown doesn't work");
    if (g_close_asked != 0 && now - g_close_asked < kShuttingDown) {
        if (asking || now - g_close_asked < kDoubleClick)
            return true;
        asking = true;
        const int answer = MessageBoxW(window,
                                       L"The game hasn't closed: /shutdown doesn't work while a menu is open.\n\n"
                                       L"End the game now? VanaCore and addons save their settings first, but the game's own "
                                       L"unsaved settings may be lost, and your character stays in the world for a while.",
                                       L"VanaCore", MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2 | MB_TOPMOST | MB_SETFOREGROUND);
        asking = false;
        if (answer != IDYES) {
            g_close_asked = GetTickCount64(); // the next close asks again
            return true;
        }
        end_game(L"closed again after /shutdown and the player chose to end the game");
    }
    g_close_asked = now;
    log::write(L"Window: closed with a character in the game: running /shutdown ([window] close.shutdown); close it again to close the game at once");
    chat::run(chat::kDefaultMode, "/shutdown");
    return true;
}

// The game takes its mouse buttons from its window's messages (it makes no DirectInput mouse): a button it holds, with
// every button up, got no release (a modal window closed on the double click's press, and the game synced the button
// held when its window came back; the release then went by). It's let go for it: the game's own window procedure gets
// the release, at the cursor. Enter did nothing, and walking or the camera stuck, until the next click.
void release_stuck()
{
    if (!g_unstick)
        return;
    for (int key : {VK_LBUTTON, VK_RBUTTON, VK_MBUTTON, VK_XBUTTON1, VK_XBUTTON2})
        if ((GetAsyncKeyState(key) & 0x8000) != 0)
            return;
    if (window::mouse_blocked())
        return;
    g_unstick = false;
    const unsigned held = hideui::game_mouse_buttons();
    if (held == 0)
        return;
    hideui::clear_game_mouse_buttons();
    g_game_down = 0;
    log::write(L"Window: the game held mouse button(s) %u after a modal window closed, with every button up: let go for it",
               held);
}

LRESULT CALLBACK procedure(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
    mousediag::on_entry(message, lparam); // [logging] mouse
    // A modal loop inside VanaCore's frame (an addon's file dialog or message box) would run the game's frame inside its
    // own Present every 333 ms, and addons' frames inside the open one: vanatest's Import opened a file dialog in each,
    // over and over, until the game crashed. The game's frame goes on when the loop ends.
    if (message == kGameTick && (overlay::in_present() || gui::in_frame()))
        return 0;
    // A modal window over the game (an addon's file dialog, a message box) disables its window while it's open: what the
    // mouse and keyboard do in it doesn't reach the game, nor the release of the press that closed it (a double click on a
    // file lets go over the game).
    if (message == WM_ENABLE) {
        input::modal(wparam == FALSE);
        g_after_modal = wparam != FALSE && (GetAsyncKeyState(VK_LBUTTON) < 0 || GetAsyncKeyState(VK_RBUTTON) < 0 ||
                                            GetAsyncKeyState(VK_MBUTTON) < 0 || GetAsyncKeyState(VK_XBUTTON1) < 0 ||
                                            GetAsyncKeyState(VK_XBUTTON2) < 0);
        if (wparam != FALSE)
            g_unstick = true;
    }
    constexpr WPARAM kHeld = MK_LBUTTON | MK_RBUTTON | MK_MBUTTON | MK_XBUTTON1 | MK_XBUTTON2;
    // let go off the game (over the title bar after a double click on it, or outside the window): the mouse over it again
    // with no button held ends the wait, so the next press is the game's
    if (g_after_modal && message == WM_MOUSEMOVE && (wparam & kHeld) == 0)
        g_after_modal = false;
    const bool button_release = message == WM_LBUTTONUP || message == WM_RBUTTONUP || message == WM_MBUTTONUP ||
                                message == WM_XBUTTONUP;
    const bool button_press = message >= WM_LBUTTONDOWN && message <= WM_MOUSELAST && message != WM_MOUSEWHEEL &&
                              message != WM_MOUSEHWHEEL && !button_release;
    // A modal window over the game just closed (input::modal_quiet, g_after_modal): no press reaches the game, nor
    // VanaCore's windows. A release always does: the game holds a button until its release comes, whatever it was told
    // of the press (release_stuck), and a release of a button it doesn't hold does nothing.
    if (button_press && input::modal_quiet())
        return 0;
    if (g_after_modal && button_press) {
        if ((wparam & kHeld) == 0)
            g_after_modal = false;
        return 0;
    }
    if (button_release && g_after_modal && (wparam & kHeld) == 0)
        g_after_modal = false;
    // vana.window.block_mouse (the Import and Export windows): no button, press or release, reaches the game. ImGui still
    // has them (the window's own buttons); the game is told every button is up when it ends (release_stuck).
    if ((button_press || button_release) && window::mouse_blocked()) {
        g_unstick = true;
        mouse(window, message, wparam, lparam);
        return 0;
    }
    if (message == WM_MOUSEMOVE || button_release)
        release_stuck();
    if (g_edges_inside) {
        // no frame: the client area is the whole window
        if (message == WM_NCCALCSIZE && wparam != 0)
            return 0;
        if (message == WM_NCHITTEST) {
            if (const LRESULT edge = edge_at(window, lparam); edge != HTNOWHERE)
                return edge;
        }
    }
    if (message == kUiMouse) {
        // a press, release or wheel turn the mouse hook took for VanaCore's windows (the game never had it)
        const UINT what = LOWORD(wparam);
        const bool wheel = what == WM_MOUSEWHEEL || what == WM_MOUSEHWHEEL;
        LPARAM at = lparam;
        WPARAM state = 0;
        if (wheel) {
            POINT screen{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
            ClientToScreen(window, &screen); // wheel messages say where on the screen
            at = MAKELPARAM(static_cast<WORD>(static_cast<short>(screen.x)), static_cast<WORD>(static_cast<short>(screen.y)));
            state = MAKEWPARAM(0, HIWORD(wparam));
        } else if (what == WM_LBUTTONDOWN || what == WM_RBUTTONDOWN || what == WM_MBUTTONDOWN || what == WM_XBUTTONDOWN) {
            state = what == WM_LBUTTONDOWN ? MK_LBUTTON : what == WM_RBUTTONDOWN ? MK_RBUTTON : what == WM_MBUTTONDOWN ? MK_MBUTTON : MK_XBUTTON1;
        }
        mouse(window, what, state, at);
        return 0;
    }
    if (message == binds::kRunPending) { // binds the keyboard hook took with a Windows key held: run on this thread
        binds::run_pending();
        return 0;
    }
    // /freecam's turn (right button held): the movement is the camera's before VanaCore's windows', the cursor stays hidden, and
    // the release ends it and still goes on to them (ImGui had the press; without it, no window took the mouse again)
    const bool fly_release = message == WM_RBUTTONUP && camera::fly_dragging() && camera::fly_mouse(message, wparam);
    if (message == WM_MOUSEMOVE && camera::fly_dragging() && camera::fly_mouse(message, wparam))
        return 0;
    if (message == WM_SETCURSOR && camera::fly_dragging()) {
        SetCursor(nullptr);
        return TRUE;
    }
    if (message == WM_SETCURSOR && LOWORD(lparam) == HTCLIENT && soulspire::wants_visible_cursor()) {
        SetCursor(LoadCursorW(nullptr, IDC_ARROW));
        return TRUE;
    }
    if ((message >= WM_MOUSEFIRST && message <= WM_MOUSELAST && mouse(window, message, wparam, lparam)) || fly_release)
        return 0;
    // /freecam: the wheel (the camera's speed), the right button and the mouse's movement while it's held (the camera's turn) are
    // the free camera's; the game doesn't zoom, turn its camera or cancel with them
    if (message >= WM_MOUSEFIRST && message <= WM_MOUSELAST && camera::fly_mouse(message, wparam))
        return 0;
    // Mouse binds (/bind mbutton ...): a press nothing above took, and a release whose press the game didn't get (one it
    // got is its own, or the button sticks held). The game doesn't see it, nor its DirectInput mouse the press.
    if ((button_press || (button_release && (g_game_down & game_bit(message)) == 0)) && binds::on_mouse(message, wparam)) {
        if (button_press)
            input::mouse_taken(true, false);
        mousediag::on_taken(message, "a key bind", GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam));
        return message == WM_XBUTTONDOWN || message == WM_XBUTTONUP || message == WM_XBUTTONDBLCLK ? TRUE : 0;
    }
    // The game gets a release only for a press it got (see g_game_down): it toggles, and a lone release is a press.
    if (message >= WM_LBUTTONDOWN && message <= WM_XBUTTONDBLCLK && message != WM_MOUSEWHEEL && message != WM_MOUSEHWHEEL) {
        const unsigned bit = game_bit(message);
        const bool up = message == WM_LBUTTONUP || message == WM_RBUTTONUP || message == WM_MBUTTONUP || message == WM_XBUTTONUP;
        if (!up)
            g_game_down |= bit;
        else if ((g_game_down & bit) == 0)
            return 0;
        else
            g_game_down &= ~bit;
    }
    if (g_let_go && (message == WM_LBUTTONUP || message == WM_LBUTTONDOWN || (message == WM_MOUSEMOVE && (wparam & MK_LBUTTON) == 0)))
        g_let_go = false;
    if (message == WM_MOUSEMOVE && (g_captured || g_let_go)) // dragging in VanaCore's window: the game sees no button held
        wparam &= ~static_cast<WPARAM>(MK_LBUTTON | MK_RBUTTON | MK_MBUTTON | MK_XBUTTON1 | MK_XBUTTON2);
    // Typing in an ImGui text box: the keys and characters are ImGui's (Print Screen stays VanaCore's)
    const bool key = message == WM_KEYDOWN || message == WM_SYSKEYDOWN || message == WM_KEYUP || message == WM_SYSKEYUP;
    if (((key && wparam != VK_SNAPSHOT) || message == WM_CHAR) && gui::on_key(message, wparam, lparam))
        return 0;
    // Key binds (those the game's loop took from the queue were asked already, input.cpp), then plugins' keyboard
    // callbacks, then [ui] hide.key: a key one takes is kept from the game's DirectInput keyboard too, until it's let go
    if (key && (binds::on_key(message, wparam, lparam) || plugins::on_key(wparam, lparam) ||
                addons::on_key(wparam, lparam, false) || hideui::on_key(message, wparam, lparam))) {
        if (message == WM_KEYDOWN || message == WM_SYSKEYDOWN)
            input::hide_key(static_cast<unsigned>((lparam >> 16 & 0x7F) | ((lparam & (1 << 24)) != 0 ? 0x80 : 0)));
        return 0;
    }
    switch (message) {
    case WM_NCLBUTTONDOWN:
    case WM_NCRBUTTONDOWN:
    case WM_NCMBUTTONDOWN: {
        // A press on the window's frame (its title bar, edges, buttons): Windows moves or sizes the
        // window, in a loop that returns when the button is let go. The game doesn't see that mouse
        // (input.cpp), during or after.
        input::frame_press(true);
        const LRESULT result = CallWindowProcA(g_original, window, message, wparam, lparam);
        input::frame_press(false);
        return result;
    }
    case WM_NCLBUTTONDBLCLK:
    case WM_NCRBUTTONDBLCLK:
    case WM_NCMBUTTONDBLCLK: {
        // A double click on the title bar maximizes (or restores) the window at once, with the button still down: let go,
        // it was over the game, which took it for a press of its own and turned the camera on. The game sees neither the
        // press (its DirectInput mouse, input.cpp) nor the window's mouse until the button is let go, as after a modal.
        input::frame_press(true);
        const LRESULT result = CallWindowProcA(g_original, window, message, wparam, lparam);
        input::frame_press(false);
        input::mouse_taken(true, false);
        g_after_modal = GetAsyncKeyState(VK_LBUTTON) < 0 || GetAsyncKeyState(VK_RBUTTON) < 0 || GetAsyncKeyState(VK_MBUTTON) < 0;
        return result;
    }
    case WM_ENTERSIZEMOVE: {
        input::frame_press(true);
        RECT client{};
        GetClientRect(window, &client);
        g_size_before = {client.right, client.bottom};
        g_moving = true;
        break;
    }
    case WM_EXITSIZEMOVE:
        g_moving = false;
        input::frame_press(false);
        remember();
        remember_size();
        break;
    case WM_KEYDOWN:
        if (held(VK_CONTROL) && !held(VK_MENU)) {
            // taken for the chat line: the game reads its keyboard through DirectInput too, so the
            // key is hidden there until it's let go (V is its first-person view)
            const unsigned scan_code = static_cast<unsigned>(lparam >> 16 & 0xFF);
            if (wparam == 'V' && clipboard::paste(window)) {
                input::hide_key(scan_code);
                return 0;
            }
            if (wparam == 'C' && clipboard::copy(window)) {
                input::hide_key(scan_code);
                return 0;
            }
        }
        break;
    case WM_CHAR:
        // The control characters Ctrl+V and Ctrl+C type: not for the chat line.
        if (wparam == 0x16 || wparam == 0x03)
            return 0;
        break;
    case WM_ACTIVATE:
        if (LOWORD(wparam) != WA_INACTIVE)
            g_was_in_front = true;
        break;
    case WM_DEVICECHANGE:
        // a device came or went (DBT_DEVNODES_CHANGED): the game's next controller search runs for real
        if (wparam == 0x0007)
            input::device_changed();
        break;
    case WM_SYSCOMMAND:
        // [window] keepawake: Windows asking the window whether the screen saver may start or the display go off
        // (it asks the window in front): no. keep_awake() stops Windows counting the idle time as well.
        if (g_keep_awake && ((wparam & 0xFFF0) == SC_SCREENSAVE || (wparam & 0xFFF0) == SC_MONITORPOWER))
            return 0;
        // the title bar's X and Alt+F4 come as this; the game's own handling may never turn them into WM_CLOSE
        if ((wparam & 0xFFF0) == SC_CLOSE && close_as_shutdown(window))
            return 0;
        break;
    case WM_CLOSE: // PostMessage and Task Manager's End task send this one alone
        if (close_as_shutdown(window))
            return 0;
        break;
    case WM_DESTROY:
        remember();
        // the game closing: addons and plugins unloaded while it still runs
        shutdown::run(L"the game's window is closing");
        if (g_hook_thread != 0) // it takes its hooks off as it ends
            PostThreadMessageW(g_hook_thread, WM_QUIT, 0, 0);
        break;
    default:
        break;
    }
    // [ui] aspect: the mouse over the interface, where the game drew it before it was squeezed
    const LPARAM sent = lparam;
    lparam = graphics::mouse(window, message, wparam, lparam);
    // and in the game's pixels when the window isn't the game's size (wheel messages say where on
    // the screen, not the window)
    if (message >= WM_MOUSEFIRST && message <= WM_MOUSELAST && message != WM_MOUSEWHEEL && message != WM_MOUSEHWHEEL)
        lparam = to_game(window, lparam);
    mousediag::on_message(message, sent, lparam); // [logging] mouse
    return CallWindowProcA(g_original, window, message, wparam, lparam);
}

} // namespace

void install(HWND window)
{
    if (window == nullptr || g_window != nullptr)
        return;
    g_window = window;
    g_original = reinterpret_cast<WNDPROC>(SetWindowLongPtrA(window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&procedure)));
    if (g_original == nullptr) {
        log::write(L"Window: couldn't take over the game window's messages (error %lu): no copy and paste",
                   GetLastError());
        g_window = nullptr;
        return;
    }
    if (HANDLE thread = CreateThread(nullptr, 0, &hook_thread, nullptr, 0, &g_hook_thread))
        CloseHandle(thread);
    else
        log::write(L"Window: couldn't start the thread for VanaCore's mouse hook (error %lu)", GetLastError());
}

bool was_in_front()
{
    return g_was_in_front || (g_window != nullptr && GetForegroundWindow() == g_window);
}

bool in_background()
{
    return g_window != nullptr && !process_in_front();
}

HWND handle()
{
    return g_window;
}

namespace {

// ---- The frame the game thinks its window has --------------------------------------------------
// FFXiMain.dll corrects the mouse for the window's frame and title bar: from the window's rect and
// GetSystemMetrics' frame (SM_CXSIZEFRAME, SM_CYSIZEFRAME) and caption (SM_CYCAPTION) sizes it scales the
// position of the mouse by the window's size over the client area's (a 2560x1600 window with a 4 px frame and a
// 29 px title bar: y * 1600 / 1563, 24 px low at y=1000, 33 at y=1400), the menus' hit areas included
// (the title screen's Agree and Disagree, the character list). The sizes are zero in the game's own borderless
// mode (3), but VanaCore hands the game a windowed one (the game makes a window; VanaCore takes its frame off),
// so the game corrects for a frame that isn't there. A window with no title bar has no frame or title bar to
// correct for: the game reads zero. A windowed one's frame isn't GetSystemMetrics' sizes either: those are a sizing
// frame's, where its edges are fixed unless [window] resizable; a sizing frame has Windows' padded border
// (SM_CXPADDEDBORDER) besides; and the frame is at the DPI of the monitor the window is on, where GetSystemMetrics
// answers at the main one's. The game took its client area for a few pixels more or less than it is, and every click
// landed that much off (more on a scaled display). So the game reads its window's frame as measured from its rect and
// its client area: each side's width, the bottom edge's height and the rest of the top as the title bar (zero for a
// window with none, as before). A window too tall for the screen with its title bar (one the game's resolution high on
// a screen that size), whose client area Windows makes shorter, is measured as it is, so the game scales the mouse from
// what's left to its resolution. While the window is minimized (its rect is then Windows' place for minimized windows),
// the last measure stands.
decltype(&GetSystemMetrics) g_get_system_metrics = nullptr;
std::atomic<std::uint32_t> g_frame{0}; // the last measure: side, bottom edge, title bar, 10 bits each
std::atomic<bool> g_frame_known{false};
std::atomic<std::uint32_t> g_frame_logged{~0u};

// The window's frame: each side's width, the bottom edge's height and the top's less that (its title bar). False while
// it's minimized or has no size.
bool measure_frame(HWND window, int& side, int& edge, int& caption)
{
    RECT outer{}, client{};
    POINT origin{0, 0};
    if (IsIconic(window) || !GetWindowRect(window, &outer) || !GetClientRect(window, &client) || client.right <= 0 ||
        client.bottom <= 0 || !ClientToScreen(window, &origin))
        return false;
    const LONG left = origin.x - outer.left, right = outer.right - (origin.x + client.right);
    const LONG top = origin.y - outer.top, bottom = outer.bottom - (origin.y + client.bottom);
    if (left < 0 || right < 0 || bottom < 0 || top < bottom || top > 1023 || left + right > 2046)
        return false;
    side = static_cast<int>((left + right) / 2), edge = static_cast<int>(bottom), caption = static_cast<int>(top - bottom);
    return true;
}

int WINAPI system_metrics(int index)
{
    const HWND window = g_window;
    if ((index != SM_CXSIZEFRAME && index != SM_CYSIZEFRAME && index != SM_CYCAPTION) || window == nullptr)
        return g_get_system_metrics(index);
    std::uint32_t frame = 0;
    if (int side = 0, edge = 0, caption = 0; measure_frame(window, side, edge, caption)) {
        frame = static_cast<std::uint32_t>(side) | static_cast<std::uint32_t>(edge) << 10 |
                static_cast<std::uint32_t>(caption) << 20;
        g_frame = frame;
        g_frame_known = true;
        if (g_frame_logged.exchange(frame) != frame) {
            if (frame == 0)
                log::write(L"Window: the game reads no frame or title bar for its window (it has none), so its mouse "
                           L"isn't scaled for them");
            else
                log::write(L"Window: the game reads its window's frame as it is (%d px at the sides, %d at the bottom, a %d px "
                           L"title bar, where Windows' sizes say %d, %d and %d), so it scales the mouse from the window's own "
                           L"client area",
                           side, edge, caption, g_get_system_metrics(SM_CXSIZEFRAME), g_get_system_metrics(SM_CYSIZEFRAME),
                           g_get_system_metrics(SM_CYCAPTION));
        }
    } else if (g_frame_known) {
        frame = g_frame;
    } else {
        return (GetWindowLongW(window, GWL_STYLE) & WS_CAPTION) == 0 ? 0 : g_get_system_metrics(index);
    }
    const std::uint32_t value = index == SM_CXSIZEFRAME ? frame : index == SM_CYSIZEFRAME ? frame >> 10 : frame >> 20;
    return static_cast<int>(value & 0x3FF);
}

void on_game_module(HMODULE module)
{
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(module, path, MAX_PATH);
    const wchar_t* name = wcsrchr(path, L'\\');
    if (_wcsicmp(name != nullptr ? name + 1 : path, L"FFXiMain.dll") == 0)
        imports::hook(module, "USER32.dll", "GetSystemMetrics", &system_metrics, g_get_system_metrics);
}

} // namespace

void hook_metrics()
{
    if (!imports::for_each_module(&on_game_module))
        log::write(L"Window: couldn't watch the game load: its mouse may be scaled for a frame the window doesn't have");
}

namespace {

// What made the game DPI unaware before dpi_aware asked: Windows' compatibility layers for VanaCore.exe (its Properties >
// Compatibility, kept in AppCompatFlags\Layers; __COMPAT_LAYER, also passed down from the program that started it), else
// the DLLs in the game from outside Windows' and VanaCore's folders (one may have set it first)
std::wstring dpi_forced_by()
{
    wchar_t exe[MAX_PATH] = {}, text[512] = {};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring found;
    if (const DWORD n = GetEnvironmentVariableW(L"__COMPAT_LAYER", text, 512); n > 0 && n < 512)
        found = L", __COMPAT_LAYER \"" + std::wstring(text) + L"\"";
    for (const auto& [root, name] : {std::pair{HKEY_CURRENT_USER, L"HKCU"}, std::pair{HKEY_LOCAL_MACHINE, L"HKLM"}}) {
        HKEY key = nullptr;
        if (RegOpenKeyExW(root, L"Software\\Microsoft\\Windows NT\\CurrentVersion\\AppCompatFlags\\Layers", 0,
                          KEY_QUERY_VALUE | KEY_WOW64_64KEY, &key) != ERROR_SUCCESS)
            continue;
        DWORD type = 0, bytes = sizeof text - sizeof(wchar_t);
        if (RegQueryValueExW(key, exe, nullptr, &type, reinterpret_cast<BYTE*>(text), &bytes) == ERROR_SUCCESS && type == REG_SZ)
            text[bytes / sizeof(wchar_t)] = L'\0', found += L", " + std::wstring(name) + L" \"" + text + L"\"";
        RegCloseKey(key);
    }
    if (!found.empty())
        return L"Windows' compatibility settings for it (" + found.substr(2) + L"): in VanaCore.exe's Properties > Compatibility "
               L"> Change high DPI settings (and the launcher's, as they pass down), untick Override high DPI scaling behavior";
    wchar_t windows[MAX_PATH] = {}, own[MAX_PATH] = {};
    const size_t windows_len = GetWindowsDirectoryW(windows, MAX_PATH);
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&dpi_forced_by), &self);
    GetModuleFileNameW(self, own, MAX_PATH);
    const size_t own_len = std::wstring_view(own).find_last_of(L'\\') + 1;
    std::wstring dlls;
    if (const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, 0); snapshot != INVALID_HANDLE_VALUE) {
        MODULEENTRY32W entry{sizeof entry};
        for (BOOL more = Module32FirstW(snapshot, &entry); more; more = Module32NextW(snapshot, &entry))
            if (_wcsnicmp(entry.szExePath, windows, windows_len) != 0 && _wcsnicmp(entry.szExePath, own, own_len) != 0)
                dlls += (dlls.empty() ? L"" : L", ") + std::wstring(entry.szExePath);
        CloseHandle(snapshot);
    }
    return L"not VanaCore.exe's compatibility settings (it has none); DLLs in the game from outside Windows' and VanaCore's "
           L"folders: " + (dlls.empty() ? std::wstring(L"none") : dlls);
}

} // namespace

void dpi_aware(const wchar_t* ini)
{
    if (ini != nullptr && ini[0] != L'\0' && GetPrivateProfileIntW(L"window", L"dpiaware", 1, ini) == 0) {
        log::write(L"Window: dpiaware=0: on a scaled display Windows draws the game smaller and stretches it");
        return;
    }
    const HMODULE user32 = LoadLibraryW(L"user32.dll");
    // Windows 10 1703 on: per monitor, version 2 (the title bar scales too); then 8.1's; then Vista's
    // (the main monitor's scale only)
    using SetContext = BOOL(WINAPI*)(HANDLE);
    using SetAwareness = HRESULT(WINAPI*)(int);
    using SetAware = BOOL(WINAPI*)();
    const wchar_t* how = nullptr;
    DWORD error = 0;
    if (const auto set_context = reinterpret_cast<SetContext>(GetProcAddress(user32, "SetProcessDpiAwarenessContext"))) {
        const HANDLE per_monitor_v2 = reinterpret_cast<HANDLE>(static_cast<LONG_PTR>(-4)); // DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2
        if (set_context(per_monitor_v2))
            how = L"per monitor";
        else
            error = GetLastError();
    } else if (const HMODULE shcore = LoadLibraryW(L"shcore.dll")) {
        const auto set_awareness = reinterpret_cast<SetAwareness>(GetProcAddress(shcore, "SetProcessDpiAwareness"));
        const HRESULT result = set_awareness != nullptr ? set_awareness(2) : E_NOTIMPL; // PROCESS_PER_MONITOR_DPI_AWARE
        if (SUCCEEDED(result))
            how = L"per monitor";
        else
            error = static_cast<DWORD>(result);
    }
    if (how == nullptr && error == 0) {
        if (const auto set_aware = reinterpret_cast<SetAware>(GetProcAddress(user32, "SetProcessDPIAware")); set_aware != nullptr && set_aware())
            how = L"the main monitor's scale";
    }
    if (how == nullptr) {
        // already set (the .exe's compatibility settings, say): say what it is
        using GetContext = HANDLE(WINAPI*)();
        using Awareness = int(WINAPI*)(HANDLE);
        const auto get_context = reinterpret_cast<GetContext>(GetProcAddress(user32, "GetThreadDpiAwarenessContext"));
        const auto awareness = reinterpret_cast<Awareness>(GetProcAddress(user32, "GetAwarenessFromDpiAwarenessContext"));
        const int now = get_context != nullptr && awareness != nullptr ? awareness(get_context()) : -1;
        if (now > 0)
            log::write(L"Window: the game was already DPI aware (%ls; the .exe's compatibility settings, say): it draws "
                       L"at the display's own pixels",
                       now == 2 ? L"per monitor" : L"the main monitor's scale");
        else
            log::write(L"Window: couldn't make the game DPI aware (error 0x%lX: already set): on a scaled display Windows "
                       L"stretches it, or (Parallels at 200%%) shows it in the top-left quarter of the screen. Set by %ls",
                       error, dpi_forced_by().c_str());
        return;
    }
    UINT dpi = 96;
    if (const auto system_dpi = reinterpret_cast<UINT(WINAPI*)()>(GetProcAddress(user32, "GetDpiForSystem")))
        dpi = system_dpi();
    log::write(L"Window: DPI aware (%ls): the game draws at the display's own pixels%ls", how,
               dpi != 96 ? (L" (the display is scaled " + std::to_wstring(MulDiv(static_cast<int>(dpi), 100, 96)) +
                            L"%: a window is its size in real pixels, so it looks smaller than when Windows stretched it)")
                               .c_str()
                         : L"");
}

float dpi_scale()
{
    static const auto dpi_for_window = reinterpret_cast<UINT(WINAPI*)(HWND)>(
        GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow"));
    const HWND window = handle();
    const UINT dpi = window != nullptr && dpi_for_window != nullptr ? dpi_for_window(window) : 96;
    return dpi > 96 ? static_cast<float>(dpi) / 96.0f : 1.0f;
}

namespace {

// [ffxi]'s mode and resolution.game (the rest of the window's settings are [window]'s)
std::wstring read_ffxi(const wchar_t* ini, const wchar_t* key)
{
    wchar_t value[64] = {};
    GetPrivateProfileStringW(L"ffxi", key, L"", value, 64, ini);
    return value;
}

Layout read_layout(const wchar_t* ini)
{
    Layout layout;
    const std::wstring text = read_ffxi(ini, L"mode");
    const int mode = parse_mode(text.c_str());
    if (mode >= 0 && registry::registry_overrides("0034"))
        log::write(L"Window: [ffxi] mode=%ls is ignored: [ffxi.registry] 0034 sets the window mode "
                   L"(registry.override=1)",
                   text.c_str());
    else if (mode >= 0)
        layout.mode = static_cast<Mode>(mode);
    else if (!text.empty() && text != L"-1")
        log::write(L"Window: [ffxi] mode=%ls isn't fullscreen, windowed, borderless or borderless fullscreen; the "
                   L"game's window mode (0034)",
                   text.c_str());
    // the windowed modes' size: [ffxi] resolution.game, read again so it changes while the game runs
    // (the game's own resolution at the next start); registry.cpp logs one that isn't a size. With
    // [ffxi.registry] 0001 overriding it, the game's resolution as it made it.
    // auto and auto-<percent> are the resolution of the monitor the window is on; unset is auto.
    std::wstring size = read_ffxi(ini, L"resolution.game");
    if (size.empty())
        size = L"auto";
    long width = 0, height = 0;
    if (!registry::registry_overrides("0001") && registry::parse_resolution(size, handle(), width, height) && width >= 320 &&
        height >= 240 && width <= 16384 && height <= 16384)
        layout.width = static_cast<int>(width), layout.height = static_cast<int>(height);
    layout.resizable = GetPrivateProfileIntW(L"window", L"resizable", 0, ini) != 0;
    // auto-taskbar is the monitor's work area: with a title bar and borders (windowed) the window's inside is that less
    // them, so the whole window fills it (frame_for's frame for it is the work area, kept there)
    std::wstring compact;
    for (const wchar_t c : size)
        if (c != L' ' && c != L'\t')
            compact += c;
    const int game_mode = registry::window_mode();
    const Mode mode_now = layout.mode != Mode::Unset            ? layout.mode
                          : game_mode >= 0 && game_mode <= 3 ? static_cast<Mode>(game_mode)
                                                             : Mode::Windowed;
    if (layout.width != 0 && _wcsicmp(compact.c_str(), L"auto-taskbar") == 0 && mode_now == Mode::Windowed) {
        const HWND window = handle();
        RECT frame{0, 0, 0, 0};
        AdjustWindowRectEx(&frame, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX |
                                       (layout.resizable ? WS_THICKFRAME | WS_MAXIMIZEBOX : 0),
                           FALSE, static_cast<DWORD>((g_game_ex_style != 0 ? g_game_ex_style : window != nullptr ? GetWindowLongW(window, GWL_EXSTYLE) : 0) &
                                                     ~(WS_EX_CLIENTEDGE | WS_EX_STATICEDGE)));
        layout.width = std::max(320, layout.width - static_cast<int>(frame.right - frame.left));
        layout.height = std::max(240, layout.height - static_cast<int>(frame.bottom - frame.top));
    }
    wchar_t kept[64] = {};
    GetPrivateProfileStringW(L"window", L"lastsize", L"", kept, 64, ini);
    if (long w = 0, h = 0; kept[0] != L'\0' && registry::parse_resolution(kept, nullptr, w, h) && w >= 320 && h >= 240 &&
                           w <= 16384 && h <= 16384)
        layout.kept_width = static_cast<int>(w), layout.kept_height = static_cast<int>(h);
    layout.topmost = GetPrivateProfileIntW(L"window", L"topmost", 0, ini) != 0;
    return layout;
}

// The frame for a client area of width x height at the window's top-left, kept on its monitor.
RECT frame_for(HWND window, LONG style, LONG ex_style, int width, int height)
{
    RECT now{};
    GetWindowRect(window, &now);
    RECT frame{0, 0, width, height};
    AdjustWindowRectEx(&frame, static_cast<DWORD>(style), FALSE, static_cast<DWORD>(ex_style));
    const LONG w = frame.right - frame.left, h = frame.bottom - frame.top;
    RECT at{now.left, now.top, now.left + w, now.top + h};
    MONITORINFO monitor{sizeof monitor};
    if (GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST), &monitor)) {
        const RECT& area = monitor.rcWork;
        if (at.right > area.right)
            OffsetRect(&at, area.right - at.right, 0);
        if (at.bottom > area.bottom)
            OffsetRect(&at, 0, area.bottom - at.bottom);
        if (at.left < area.left)
            OffsetRect(&at, area.left - at.left, 0);
        if (at.top < area.top)
            OffsetRect(&at, 0, area.top - at.top);
    }
    return at;
}

bool covers_monitor(HWND window)
{
    RECT frame{};
    MONITORINFO monitor{sizeof monitor};
    return GetWindowRect(window, &frame) &&
           GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST), &monitor) &&
           EqualRect(&frame, &monitor.rcMonitor) && (GetWindowLongW(window, GWL_STYLE) & WS_CAPTION) == 0;
}

// Windows 11 draws a border and rounds the corners of top-level windows; a borderless one gets
// neither. No effect on Windows 10.
void dwm_frame(HWND window, bool borderless)
{
    static const auto set = reinterpret_cast<HRESULT(WINAPI*)(HWND, DWORD, LPCVOID, DWORD)>(
        GetProcAddress(LoadLibraryW(L"dwmapi.dll"), "DwmSetWindowAttribute"));
    if (set == nullptr)
        return;
    const DWORD border = borderless ? 0xFFFFFFFEu : 0xFFFFFFFFu; // DWMWA_COLOR_NONE : DWMWA_COLOR_DEFAULT
    const DWORD corners = borderless ? 1u : 0u;                   // DWMWCP_DONOTROUND : DWMWCP_DEFAULT
    set(window, 34, &border, sizeof border);                      // DWMWA_BORDER_COLOR
    set(window, 33, &corners, sizeof corners);                    // DWMWA_WINDOW_CORNER_PREFERENCE
}

// The mode the window is in: [ffxi]'s, else the game's own setting (the launcher's), as the game read it
Mode mode_of(const Layout& layout)
{
    if (layout.mode != Mode::Unset)
        return layout.mode;
    const int game = registry::window_mode();
    return game >= 0 && game <= 3 ? static_cast<Mode>(game) : Mode::Windowed;
}

// ---- Where the window is: [window] position, /vana window ------------------------------------

std::wstring g_ini;       // vanacore.ini, as apply() last had it
bool g_exclusive = false; // the device is Windows' own exclusive full screen: the window is its
SIZE g_made{};            // the client size the window was last made at in a windowed mode (0: the game's resolution)
std::wstring g_position;  // the position setting the window was last put by (lower case)
std::string g_character;  // the character last logged in ("" before one has)
bool g_remember = false;  // position=last
HMONITOR g_monitor = nullptr;          // the monitor the window was on at the last poll (follow_monitor)
long g_monitor_w = 0, g_monitor_h = 0; // its resolution then
RECT g_monitor_work{};                 // its work area then (the screen less the taskbar)

std::wstring lower(const std::wstring& text)
{
    std::wstring out;
    for (const wchar_t c : text)
        if (c != L' ' && c != L'\t')
            out += static_cast<wchar_t>(towlower(c));
    return out;
}

std::string lower(std::string text)
{
    for (char& c : text)
        c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    return text;
}

// A character's name as a key (FFXI's names are letters)
std::wstring wide(const std::string& text)
{
    std::wstring out;
    for (const char c : text)
        out += static_cast<wchar_t>(static_cast<unsigned char>(c));
    return out;
}

// a setting's value or key for the chat log (ASCII: numbers, names)
std::string ascii(const std::wstring& text)
{
    std::string out;
    for (const wchar_t c : text)
        out += c < 0x80 ? static_cast<char>(c) : '?';
    return out;
}

// [window] key, lower case without spaces ("" unset)
std::wstring setting(const std::wstring& key)
{
    wchar_t value[128] = {};
    if (!g_ini.empty())
        GetPrivateProfileStringW(L"window", key.c_str(), L"", value, 128, g_ini.c_str());
    return lower(value);
}

// The position the window goes by: the character's own, else [window] position
std::wstring wanted()
{
    if (!g_character.empty())
        if (std::wstring own = setting(L"position." + wide(g_character)); !own.empty())
            return own;
    return setting(L"position");
}

std::wstring position_key()
{
    if (!g_character.empty() && !setting(L"position." + wide(g_character)).empty())
        return L"[window] position." + wide(g_character);
    return L"[window] position";
}

// "x,y" (or "x y")
bool parse_point(const std::wstring& text, POINT& at)
{
    const wchar_t* p = text.c_str();
    wchar_t* end = nullptr;
    const long x = std::wcstol(p, &end, 10);
    if (end == p || (*end != L',' && *end != L' '))
        return false;
    p = end;
    while (*p == L',' || *p == L' ')
        ++p;
    const long y = std::wcstol(p, &end, 10);
    if (end == p || *end != L'\0')
        return false;
    at = {x, y};
    return true;
}

BOOL CALLBACK add_monitor(HMONITOR monitor, HDC, LPRECT, LPARAM list)
{
    MONITORINFO info{sizeof info};
    if (GetMonitorInfoW(monitor, &info))
        reinterpret_cast<std::vector<Monitor>*>(list)->push_back({info.rcMonitor, info.rcWork, (info.dwFlags & MONITORINFOF_PRIMARY) != 0});
    return TRUE;
}

// out of a minimized or maximized window first, so it can be put somewhere
void unfold(HWND window)
{
    if (IsIconic(window) || IsZoomed(window))
        ShowWindow(window, SW_RESTORE);
}

// The window covering `monitor` (full screen without Windows' exclusive mode)
void cover(HWND window, HMONITOR monitor)
{
    MONITORINFO info{sizeof info};
    if (GetMonitorInfoW(monitor, &info))
        SetWindowPos(window, nullptr, info.rcMonitor.left, info.rcMonitor.top, info.rcMonitor.right - info.rcMonitor.left,
                     info.rcMonitor.bottom - info.rcMonitor.top, SWP_NOZORDER | SWP_NOACTIVATE);
}

// The window in the middle of `monitor`'s work area (its title bar kept on it); `full`, covering it
bool centre(HMONITOR monitor, bool full)
{
    const HWND window = g_window;
    if (window == nullptr || g_exclusive)
        return false;
    unfold(window);
    if (full) {
        cover(window, monitor);
        return true;
    }
    MONITORINFO info{sizeof info};
    RECT frame{};
    if (!GetMonitorInfoW(monitor, &info) || !GetWindowRect(window, &frame))
        return false;
    const RECT& work = info.rcWork;
    const LONG x = std::max(work.left, work.left + (work.right - work.left - (frame.right - frame.left)) / 2);
    const LONG y = std::max(work.top, work.top + (work.bottom - work.top - (frame.bottom - frame.top)) / 2);
    SetWindowPos(window, nullptr, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    return true;
}

// The window's top-left at `at`, or with `full` the monitor at `at` covered. One that would be on no
// monitor at all (one since unplugged) goes to the middle of the main one.
bool move_to(POINT at, bool full)
{
    const HWND window = g_window;
    if (window == nullptr || g_exclusive)
        return false;
    unfold(window);
    if (full) {
        cover(window, MonitorFromPoint(at, MONITOR_DEFAULTTONEAREST));
        return true;
    }
    RECT frame{};
    GetWindowRect(window, &frame);
    const RECT to{at.x, at.y, at.x + frame.right - frame.left, at.y + frame.bottom - frame.top};
    if (MonitorFromRect(&to, MONITOR_DEFAULTTONULL) == nullptr) {
        log::write(L"Window: %ld,%ld is on no monitor: the middle of the main one", at.x, at.y);
        return centre(MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY), false);
    }
    SetWindowPos(window, nullptr, at.x, at.y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    return true;
}

// The window where a position setting says: x,y, center or last (`what`: the setting, for the log)
bool put(const std::wstring& position, bool full, const std::wstring& what)
{
    if (position.empty() || g_window == nullptr)
        return false;
    if (position == L"center" || position == L"centre") {
        if (!centre(MonitorFromWindow(g_window, MONITOR_DEFAULTTONEAREST), full))
            return false;
        log::write(L"Window: in the middle of its monitor (%ls)", what.c_str());
        return true;
    }
    std::wstring point = position;
    if (position == L"last") {
        point = setting(L"last");
        if (point.empty())
            return false; // nothing kept yet
    }
    POINT at{};
    if (!parse_point(point, at)) {
        log::write(L"Window: %ls=%ls isn't x,y, center or last", what.c_str(), position.c_str());
        return false;
    }
    if (!move_to(at, full))
        return false;
    log::write(L"Window: at %ld,%ld (%ls%ls)", at.x, at.y, what.c_str(), position == L"last" ? L"=last" : L"");
    return true;
}

// The character logged in, "" before login (or while the game has no player)
std::string character()
{
    return game::local_player_name();
}

void say(const std::string& text)
{
    chat::write(chat::kDefaultMode, chat::header() + text, "VanaCore");
}

void remember()
{
    const HWND window = g_window;
    RECT frame{};
    if (!g_remember || window == nullptr || g_ini.empty() || IsIconic(window) || IsZoomed(window) || !GetWindowRect(window, &frame))
        return;
    const std::wstring at = std::to_wstring(frame.left) + L"," + std::to_wstring(frame.top);
    if (setting(L"last") != at)
        ini::set(g_ini, L"window", L"last", at.c_str());
}

// [window] lastsize: the client size the window's edges were dragged to ([window] resizable), so the next start (and a
// change of mode or topmost) makes the window that size again; the /vana window's Resolution shows it. The game's own
// resolution ([ffxi] resolution.game) stays: one the size of a small window, stretched when the window is made larger
// (maximized, full screen), blurs everything. A move alone keeps nothing.
void remember_size()
{
    const HWND window = g_window;
    RECT client{};
    if (!g_applied || !g_layout.resizable || g_exclusive || window == nullptr || g_ini.empty() || IsIconic(window) ||
        IsZoomed(window) || covers_monitor(window) || !GetClientRect(window, &client) || client.right < 320 ||
        client.bottom < 240 || client.right > 16384 || client.bottom > 16384 ||
        (client.right == g_size_before.cx && client.bottom == g_size_before.cy))
        return;
    const std::wstring size = std::to_wstring(client.right) + L"x" + std::to_wstring(client.bottom);
    if (!ini::set(g_ini, L"window", L"lastsize", size.c_str()))
        return;
    // the window is that size already: apply() has nothing to make again
    g_layout.kept_width = client.right, g_layout.kept_height = client.bottom;
    g_made = {client.right, client.bottom};
    log::write(L"Window: dragged to %ldx%ld, which the next start keeps ([window] lastsize)", client.right, client.bottom);
}

// /vana window info: where the window is, and on which monitor
void say_place()
{
    Place now;
    if (!place(now))
        return;
    std::string text = "Window: " + std::to_string(now.client_width) + " x " + std::to_string(now.client_height);
    if (now.covering)
        text += ", covering monitor " + std::to_string(now.monitor);
    else
        text += " at " + std::to_string(now.frame.left) + ", " + std::to_string(now.frame.top) + " (monitor " +
                std::to_string(now.monitor) + ")";
    if (now.topmost)
        text += ", over other windows";
    if (now.minimized)
        text += ", minimized";
    if (now.maximized)
        text += ", maximized";
    say(text);
}

void say_monitors()
{
    const std::vector<Monitor> list = monitors();
    for (size_t i = 0; i < list.size(); ++i) {
        const RECT& a = list[i].area;
        say("  monitor " + std::to_string(i + 1) + ": " + std::to_string(a.right - a.left) + " x " +
            std::to_string(a.bottom - a.top) + " at " + std::to_string(a.left) + ", " + std::to_string(a.top) +
            (list[i].main ? " (main)" : ""));
    }
}

void usage()
{
    say("/vana window: pos <x> <y> | center | monitor <n> | size <w> <h> | front | top [on|off] | frame | max | min | "
        "restore | save [char] | forget [char] | info");
    say("  save: where it is now becomes where the game starts it ([window] position; char: this character's, at "
        "its login)");
}

// Brings the game window in front of the others, as Ashita's /bringtofront: Windows lets a process
// that isn't in front take the foreground only with the input of the one that is
void bring_front(HWND window)
{
    const HWND front = GetForegroundWindow();
    const DWORD self = GetCurrentThreadId();
    const DWORD other = front != nullptr ? GetWindowThreadProcessId(front, nullptr) : 0;
    const bool attached = other != 0 && other != self && AttachThreadInput(self, other, TRUE);
    if (IsIconic(window))
        ShowWindow(window, SW_RESTORE);
    SetWindowPos(window, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
    SetForegroundWindow(window);
    SetFocus(window);
    if (attached)
        AttachThreadInput(self, other, FALSE);
}

bool number(const std::vector<std::string>& parts, size_t i, long& out)
{
    if (i >= parts.size() || parts[i].empty())
        return false;
    char* end = nullptr;
    out = std::strtol(parts[i].c_str(), &end, 10);
    return end != nullptr && *end == '\0';
}

} // namespace

bool front_process(unsigned long pid)
{
    // its window of the class this game's has (FFXI's), else its biggest visible top-level one
    wchar_t own_class[64] = {};
    if (g_window != nullptr)
        GetClassNameW(g_window, own_class, 64);
    struct Find {
        DWORD pid;
        const wchar_t* own_class;
        HWND found = nullptr;
        bool same_class = false;
        long long area = -1;
    } find{pid, own_class};
    EnumWindows(
        [](HWND window, LPARAM param) -> BOOL {
            Find& find = *reinterpret_cast<Find*>(param);
            DWORD owner = 0;
            GetWindowThreadProcessId(window, &owner);
            if (owner != find.pid || !IsWindowVisible(window) || GetWindow(window, GW_OWNER) != nullptr)
                return TRUE;
            wchar_t name[64] = {};
            GetClassNameW(window, name, 64);
            const bool same_class = find.own_class[0] != L'\0' && std::wcscmp(name, find.own_class) == 0;
            RECT rect{};
            GetWindowRect(window, &rect);
            const long long area = static_cast<long long>(rect.right - rect.left) * (rect.bottom - rect.top);
            if ((same_class && !find.same_class) || (same_class == find.same_class && area > find.area)) {
                find.found = window;
                find.same_class = same_class;
                find.area = area;
            }
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&find));
    if (find.found == nullptr)
        return false;
    AllowSetForegroundWindow(pid);
    bring_front(find.found);
    return GetForegroundWindow() == find.found;
}

int parse_mode(const wchar_t* text)
{
    if (text == nullptr || text[0] == L'-') // -1: not a mode (dashes go below, in borderless-fullscreen)
        return -1;
    std::wstring key;
    for (const wchar_t* c = text; *c != L'\0'; ++c)
        if (*c != L' ' && *c != L'-' && *c != L'_')
            key += static_cast<wchar_t>(towlower(*c));
    if (key == L"0" || key == L"fullscreen")
        return 0;
    if (key == L"1" || key == L"windowed")
        return 1;
    if (key == L"2" || key == L"borderless" || key == L"borderlesswindowed")
        return 2;
    if (key == L"3" || key == L"borderlessfullscreen")
        return 3;
    return -1;
}

namespace {

// [window] keepawake: tells Windows this thread (the game's: apply() runs on it, from the device's making and from
// Present) needs the display, so the screen saver, the display's sleep and the lock that follows them don't come
// while the game runs, as they don't with the game's own full screen. ES_CONTINUOUS keeps that until it's told
// otherwise or the thread ends. Off: Windows counts the idle time as it does for anything.
void keep_awake(bool on)
{
    if (g_keep_awake_told && on == g_keep_awake)
        return;
    const EXECUTION_STATE before =
        SetThreadExecutionState(on ? ES_CONTINUOUS | ES_DISPLAY_REQUIRED | ES_SYSTEM_REQUIRED : ES_CONTINUOUS);
    if (before == 0) {
        log::write(L"Window: Windows wouldn't take the keep awake setting (keepawake=%d)", on ? 1 : 0);
        return;
    }
    g_keep_awake = on;
    g_keep_awake_told = true;
    log::write(on ? L"Window: the screen saver and the display's sleep are held off while the game runs ([window] keepawake)"
                  : L"Window: the screen saver and the display's sleep are Windows' own ([window] keepawake=0)");
}

// [window] close.shutdown: the game takes Close out of its window's menu (RemoveMenu SC_CLOSE, FFXiMain VA 0x1001656E),
// which greys the title bar's X and stops Alt+F4. With close.shutdown on, a close runs /shutdown, so Close goes back in
// at the bottom, as Windows has it; off, the menu is the game's again.
void close_menu(bool on)
{
    HMENU menu = GetSystemMenu(g_window, FALSE);
    if (menu == nullptr)
        return;
    const bool there = GetMenuState(menu, SC_CLOSE, MF_BYCOMMAND) != static_cast<UINT>(-1);
    if (on == there)
        return;
    if (on) {
        // the game left the separator above Close
        const int count = GetMenuItemCount(menu);
        if (count > 0 && (GetMenuState(menu, count - 1, MF_BYPOSITION) & MF_SEPARATOR) == 0)
            AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, SC_CLOSE, L"&Close\tAlt+F4");
        g_close_added = true;
    } else if (g_close_added) {
        RemoveMenu(menu, SC_CLOSE, MF_BYCOMMAND);
        g_close_added = false;
    } else {
        return;
    }
    // the X is drawn from the menu: redraw the title bar
    SetWindowPos(g_window, nullptr, 0, 0, 0, 0, SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    log::write(on ? L"Window: Close is back in the window's menu, its X runs /shutdown ([window] close.shutdown)"
                  : L"Window: Close is out of the window's menu again, as the game has it ([window] close.shutdown=0)");
}

} // namespace

void apply(const wchar_t* ini, bool exclusive)
{
    if (g_window == nullptr || ini == nullptr || ini[0] == L'\0')
        return;
    g_ini = ini;
    g_exclusive = exclusive;
    g_mouse_scale = GetPrivateProfileIntW(L"window", L"mouse.scale", 1, ini) != 0;
    keep_awake(GetPrivateProfileIntW(L"window", L"keepawake", 1, ini) != 0);
    g_close_shutdown = GetPrivateProfileIntW(L"window", L"close.shutdown", 1, ini) != 0;
    close_menu(g_close_shutdown);
    // where it is: put there when the game starts (before the mode, so full screen covers the monitor
    // it's then on) and when the setting changes; position=last only begins keeping it
    g_remember = setting(L"position") == L"last";
    if (const std::wstring position = wanted(); !g_applied) {
        g_position = position;
        if (!exclusive)
            put(position, false, position_key());
    } else if (position != g_position) {
        g_position = position;
        if (position == L"last")
            remember();
        else if (!exclusive)
            put(position, covers_monitor(g_window), position_key());
    }
    Layout layout = read_layout(ini);
    if (g_applied && layout == g_layout)
        return;
    const HWND window = g_window;
    if (!g_applied) {
        g_game_style = GetWindowLongW(window, GWL_STYLE);
        g_game_ex_style = GetWindowLongW(window, GWL_EXSTYLE);
    }
    const bool first = !g_applied;
    const Mode mode_before = mode_of(g_layout);
    // resolution.game changed while the game runs (the Client tab's Resolution): the window takes it, not the size its
    // edges were dragged to
    if (!first && (layout.width != g_layout.width || layout.height != g_layout.height) && layout.kept_width != 0) {
        ini::set(g_ini, L"window", L"lastsize", nullptr);
        layout.kept_width = layout.kept_height = 0;
    }
    g_applied = true;
    g_layout = layout;

    const Mode mode = mode_of(layout);
    // the frame is drawn at the window's size ([graphics] resolution=window) or resolution.game's (=game) as it's set
    // now - the game lays itself out at the resolution it started with, its draws scaled to this - stretched over the
    // screen in full screen. In a window too: a game started at 1280x720 and set to 3840x2160 drew 1280x720 stretched.
    graphics::render_size(static_cast<unsigned>(layout.width), static_cast<unsigned>(layout.height));
    // the game as it made itself: nothing to do
    if (first && layout.width == 0 && !layout.resizable && !layout.topmost &&
        (mode == Mode::Windowed || (mode == Mode::Fullscreen && exclusive)))
        return;
    if (exclusive) {
        log::write(L"Window: the game is full screen on Windows' own Direct3D 8, which owns its window: [ffxi] "
                   L"mode and resolution.game apply from the next start (or with renderer=d3d12 or d3d11)");
        return;
    }
    // Full screen without an exclusive device (VanaCore's Direct3D 12, or a window made before the
    // setting changed) is borderless over the monitor.
    const bool cover = mode == Mode::Fullscreen || mode == Mode::BorderlessFullscreen;
    // Laid out again for another reason (moved to another monitor, a new resolution): the frame stays as it is, so one an
    // addon took off or put back (the move addon's /windowframe, another program) isn't undone. A new [ffxi] mode sets it.
    const bool keep_frame = !first && !cover && mode == mode_before;
    const bool framed_now = (GetWindowLongW(window, GWL_STYLE) & WS_CAPTION) != 0;
    const bool borderless = keep_frame ? !framed_now : mode != Mode::Windowed;
    const bool frame_kept = borderless != (mode != Mode::Windowed);
    const Mode shown = frame_kept ? (borderless ? Mode::Borderless : Mode::Windowed) : mode;
    // a new mode or size while maximized (Windows maximizes a resizable window when its monitor resets): out of it
    // first, else it stays maximized and the size picked never shows
    if (!first)
        unfold(window);
    const bool was_covering = covers_monitor(window);

    LONG style = GetWindowLongW(window, GWL_STYLE) & ~(WS_OVERLAPPEDWINDOW | WS_POPUP);
    // The game's window has a 2-pixel sunken client edge (WS_EX_CLIENTEDGE): maximised, Windows pushes only the
    // frame off the screen, so the edge stayed as a pale strip down the sides
    LONG ex_style = g_game_ex_style & ~(WS_EX_CLIENTEDGE | WS_EX_STATICEDGE);
    if (borderless)
        ex_style &= ~(WS_EX_DLGMODALFRAME | WS_EX_WINDOWEDGE);
    RECT at{};
    if (cover) {
        MONITORINFO monitor{sizeof monitor};
        if (!GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST), &monitor))
            return;
        if (!was_covering)
            GetWindowRect(window, &g_before_fullscreen);
        style |= WS_POPUP;
        at = monitor.rcMonitor;
    } else {
        style |= borderless ? WS_POPUP : (WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX);
        if (layout.resizable)
            style |= borderless ? WS_THICKFRAME : (WS_THICKFRAME | WS_MAXIMIZEBOX);
        int width = layout.width, height = layout.height;
        if (width == 0 && was_covering && !IsRectEmpty(&g_before_fullscreen)) {
            // back from covering the monitor: where it was
            SetWindowPos(window, nullptr, g_before_fullscreen.left, g_before_fullscreen.top, 0, 0,
                         SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        }
        // resizable: the size its edges were last dragged to ([window] lastsize, remember_size)
        if (layout.resizable && layout.kept_width != 0)
            width = layout.kept_width, height = layout.kept_height;
        if (width == 0 || height == 0) {
            // the game's resolution
            unsigned w = 0, h = 0;
            ui::game_screen(w, h);
            width = static_cast<int>(w), height = static_cast<int>(h);
        }
        if (width == 0 || height == 0) {
            RECT client{};
            GetClientRect(window, &client);
            width = client.right, height = client.bottom;
        }
        // resizable borderless: no frame, so the window is the picture's size
        at = frame_for(window, borderless ? style & ~WS_THICKFRAME : style, ex_style, width, height);
        g_made = {width, height};
    }
    g_edges_inside = borderless && !cover && layout.resizable;
    // over other windows only with topmost=1 (else the Windows key, Alt+Tab and the desktop come over it)
    ex_style &= ~WS_EX_TOPMOST;
    SetWindowLongW(window, GWL_STYLE, style);
    SetWindowLongW(window, GWL_EXSTYLE, ex_style);
    dwm_frame(window, borderless);
    SetWindowPos(window, layout.topmost ? HWND_TOPMOST : HWND_NOTOPMOST, at.left, at.top, at.right - at.left,
                 at.bottom - at.top, SWP_FRAMECHANGED | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    RECT client{};
    GetClientRect(window, &client);
    log::write(L"Window: %ls%ls, %ldx%ld%ls%ls%ls", first ? L"" : L"now ", kModeNames[static_cast<int>(shown)], client.right,
               client.bottom, layout.resizable && !cover ? L", its edges can be dragged" : L"",
               layout.topmost ? L", over other windows" : L"",
               frame_kept                 ? L" (its frame as an addon or another program left it, not [ffxi] mode's)"
               : layout.mode == Mode::Unset ? L" (the game's window mode, 0034)"
                                            : L" ([ffxi] mode)");
    // Windows keeps a window with a title bar no taller than the screen: one the screen's height (1920x1080 on a
    // 1920x1080 screen) has a shorter client area than it was made with
    if (!cover && !layout.resizable && !IsIconic(window) && (client.right != g_made.cx || client.bottom != g_made.cy))
        log::write(L"Window: %ldx%ld with its title bar and borders doesn't fit the screen: Windows left %ldx%ld of it; the "
                   L"interface and the mouse are still worked out as %ldx%ld, the picture shown in what's left",
                   g_made.cx, g_made.cy, client.right, client.bottom, g_made.cx, g_made.cy);
}

namespace {

// The window on another monitor (moved there, or the monitor's own resolution changed): [ffxi] resolution.game and
// resolution.ui follow it. auto and auto-<percent> are worked out again for the monitor it's on now (apply, uiscale), and
// a size that was the old monitor's own (the /vana window's "(screen)") becomes the new one's. Not while the window is
// being dragged (it lands first), minimized, or on Windows' own exclusive full screen.
void follow_monitor()
{
    const HWND window = g_window;
    if (window == nullptr || g_exclusive || g_moving || IsIconic(window))
        return;
    const HMONITOR now = MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST);
    long width = 0, height = 0;
    if (!registry::parse_resolution(L"auto", window, width, height))
        return;
    // the taskbar back (it's gone for a moment while a monitor resets): resolution.game=auto-taskbar was worked out
    // without it, so the window covered it
    MONITORINFO info{sizeof info};
    const RECT work = GetMonitorInfoW(now, &info) ? info.rcWork : RECT{};
    const bool work_changed = !EqualRect(&work, &g_monitor_work);
    g_monitor_work = work;
    if (now == g_monitor && width == g_monitor_w && height == g_monitor_h) {
        if (work_changed && _wcsicmp(read_ffxi(g_ini.c_str(), L"resolution.game").c_str(), L"auto-taskbar") == 0)
            apply(g_ini.c_str(), g_exclusive);
        return;
    }
    const bool first = g_monitor == nullptr;
    const long was_w = g_monitor_w, was_h = g_monitor_h;
    g_monitor = now, g_monitor_w = width, g_monitor_h = height;
    if (first)
        return;
    const std::wstring was = std::to_wstring(was_w) + L"x" + std::to_wstring(was_h);
    const std::wstring size = std::to_wstring(width) + L"x" + std::to_wstring(height);
    std::vector<ini::Change> changes;
    for (const wchar_t* key : {L"resolution.game", L"resolution.ui"})
        if (was != size && lower(read_ffxi(g_ini.c_str(), key)) == was)
            changes.push_back({L"ffxi", key, size});
    log::write(L"Window: now on a %ldx%ld monitor (%ls before)%ls", width, height, was.c_str(),
               changes.empty() ? L"" : changes.size() == 2 ? L": [ffxi] resolution.game and resolution.ui go with it"
               : changes[0].key == L"resolution.game"      ? L": [ffxi] resolution.game goes with it"
                                                           : L": [ffxi] resolution.ui goes with it");
    if (!changes.empty())
        ini::set(g_ini, changes);
    apply(g_ini.c_str(), g_exclusive);
    uiscale::refresh();
}

} // namespace

void poll()
{
    if (g_window == nullptr || !g_applied)
        return;
    follow_monitor();
    // a character logging in (one that isn't the last: zoning, or logging out and in again, leaves the window be)
    const std::string name = character();
    if (name.empty() || name == g_character)
        return;
    g_character = name;
    const std::wstring position = wanted();
    if (position == g_position)
        return;
    g_position = position;
    if (position != L"last" && !g_exclusive)
        put(position, covers_monitor(g_window), position_key());
}

std::vector<Monitor> monitors()
{
    std::vector<Monitor> list;
    EnumDisplayMonitors(nullptr, nullptr, &add_monitor, reinterpret_cast<LPARAM>(&list));
    std::sort(list.begin(), list.end(), [](const Monitor& a, const Monitor& b) {
        return a.area.left != b.area.left ? a.area.left < b.area.left : a.area.top < b.area.top;
    });
    return list;
}

void block_mouse(bool on, unsigned lease_ms)
{
    if (on) {
        // a lease: blocked for lease_ms more, so a window nobody draws any more (the panel closed with it open) can't leave the
        // mouse blocked
        g_block_mouse = true;
        g_block_mouse_until = GetTickCount64() + lease_ms;
        return;
    }
    if (g_block_mouse.exchange(false))
        g_block_mouse_until = GetTickCount64() + 500;
}

bool mouse_blocked()
{
    const ULONGLONG until = g_block_mouse_until;
    if (until == 0)
        return false;
    if (GetTickCount64() < until || (!g_block_mouse && any_button_down()))
        return true;
    g_block_mouse = false;
    g_block_mouse_until = 0;
    return false;
}

void release_game_buttons()
{
    hideui::clear_game_mouse_buttons();
    g_game_down = 0; // (its release is dropped: a lone release is a press to it)
    g_let_go = true;
}

bool to_monitor(int number)
{
    const std::vector<Monitor> list = monitors();
    if (g_window == nullptr || g_exclusive || number < 1 || number > static_cast<int>(list.size()))
        return false;
    const RECT& area = list[static_cast<size_t>(number - 1)].area;
    const HMONITOR monitor =
        MonitorFromPoint(POINT{area.left + (area.right - area.left) / 2, area.top + (area.bottom - area.top) / 2}, MONITOR_DEFAULTTONEAREST);
    const bool full = covers_monitor(g_window);
    if (!centre(monitor, full))
        return false;
    log::write(L"Window: to monitor %d", number);
    // its resolutions with it now, not at the next poll; a window made another size by that is put in the middle again
    follow_monitor();
    if (!full)
        centre(monitor, false);
    remember();
    return true;
}

bool place(Place& out)
{
    const HWND window = g_window;
    if (window == nullptr)
        return false;
    out = {};
    GetWindowRect(window, &out.frame);
    RECT client{};
    GetClientRect(window, &client);
    out.client_width = client.right;
    out.client_height = client.bottom;
    out.topmost = (GetWindowLongW(window, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0;
    out.minimized = IsIconic(window) != FALSE;
    out.maximized = IsZoomed(window) != FALSE;
    out.covering = covers_monitor(window);
    MONITORINFO info{sizeof info};
    if (GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST), &info)) {
        const std::vector<Monitor> list = monitors();
        for (size_t i = 0; i < list.size(); ++i)
            if (EqualRect(&list[i].area, &info.rcMonitor))
                out.monitor = static_cast<int>(i + 1);
    }
    out.character = g_character;
    return true;
}

bool made_size(unsigned& width, unsigned& height)
{
    const HWND window = g_window;
    if (window == nullptr || IsIconic(window) || IsZoomed(window) || (GetWindowLongW(window, GWL_STYLE) & WS_THICKFRAME) != 0 ||
        covers_monitor(window))
        return false;
    width = static_cast<unsigned>(g_made.cx), height = static_cast<unsigned>(g_made.cy);
    if (width == 0 || height == 0) // left as the game made it: at its resolution
        ui::game_screen(width, height);
    return width != 0 && height != 0;
}

bool resized_client(unsigned& width, unsigned& height)
{
    const HWND window = g_window;
    RECT client{};
    if (window == nullptr || !g_applied || g_exclusive || !g_layout.resizable || g_layout.kept_width == 0 ||
        IsIconic(window) || covers_monitor(window) || !GetClientRect(window, &client) || client.right <= 0 || client.bottom <= 0)
        return false;
    width = static_cast<unsigned>(client.right), height = static_cast<unsigned>(client.bottom);
    return true;
}

bool command(const std::vector<std::string>& parts)
{
    const std::string what = parts.size() > 1 ? lower(parts[1]) : "";
    if (what.empty())
        return false; // the /vana window's Client tab
    if (what == "help" || what == "?") {
        usage();
        return true;
    }
    const HWND window = g_window;
    if (window == nullptr) {
        say("the game's window isn't there yet");
        return true;
    }
    if (what == "info" || what == "where") {
        say_place();
        say_monitors();
        const std::wstring position = setting(L"position");
        say("  starts " + (position.empty() ? std::string("where the game puts it") : "at " + ascii(position)) +
            " ([window] position)");
        if (!g_character.empty())
            if (const std::wstring own = setting(L"position." + wide(g_character)); !own.empty())
                say("  " + g_character + "'s: " + ascii(own) + " ([window] position." + g_character + ")");
        return true;
    }
    if (g_exclusive) {
        say("the game is full screen on Windows' own Direct3D 8, which keeps its window where it is (renderer=d3d12 or "
            "d3d11, or a windowed mode, lets it move)");
        return true;
    }
    const bool full = covers_monitor(window);
    long a = 0, b = 0;
    const bool bare = parts.size() == 3 && number(parts, 1, a) && number(parts, 2, b); // /vana window <x> <y>
    if (bare || what == "pos" || what == "position" || what == "move") {
        if (!bare && !(parts.size() == 4 && number(parts, 2, a) && number(parts, 3, b))) {
            say("/vana window pos <x> <y>: the window's top-left, in the screen's pixels (full screen: the monitor there)");
            return true;
        }
        move_to(POINT{a, b}, full);
        remember();
        say_place();
        return true;
    }
    if (what == "center" || what == "centre") {
        if (full) {
            say("the window covers its monitor (full screen): /vana window monitor <n> moves it to another");
            return true;
        }
        centre(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST), false);
        remember();
        say_place();
        return true;
    }
    if (what == "monitor" || what == "screen") {
        const std::vector<Monitor> list = monitors();
        if (!number(parts, 2, a) || a < 1 || a > static_cast<long>(list.size())) {
            say("/vana window monitor <n>: the middle of monitor n (full screen: covering it), from 1 at the left");
            say_monitors();
            return true;
        }
        to_monitor(static_cast<int>(a));
        say_place();
        return true;
    }
    if (what == "size") {
        if (full) {
            say("the window covers its monitor (full screen): its size is the monitor's");
            return true;
        }
        if (!number(parts, 2, a) || !number(parts, 3, b) || a < 320 || b < 240 || a > 16384 || b > 16384) {
            say("/vana window size <width> <height>: the picture's size, until the game closes (the Client tab's "
                "Resolution keeps one)");
            return true;
        }
        unfold(window);
        RECT frame{0, 0, a, b};
        if (!g_edges_inside) // resizable borderless has no frame: the window is the picture
            AdjustWindowRectEx(&frame, static_cast<DWORD>(GetWindowLongW(window, GWL_STYLE)), FALSE,
                               static_cast<DWORD>(GetWindowLongW(window, GWL_EXSTYLE)));
        SetWindowPos(window, nullptr, 0, 0, frame.right - frame.left, frame.bottom - frame.top,
                     SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        g_made = {a, b};
        say_place();
        return true;
    }
    if (what == "front" || what == "bringtofront") {
        bring_front(window);
        return true;
    }
    if (what == "top" || what == "topmost") {
        const std::string how = parts.size() > 2 ? lower(parts[2]) : "";
        const bool now = (GetWindowLongW(window, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0;
        const bool on = how == "on" || how == "1" ? true : how == "off" || how == "0" ? false : !now;
        SetWindowPos(window, on ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        say(on ? "Window: over other windows until the game closes ([window] topmost=1, the Client tab's Always on top, "
                 "keeps it)"
               : "Window: no longer over other windows");
        return true;
    }
    if (what == "frame" || what == "border" || what == "windowframe") {
        // the frame on or off: windowed and borderless windowed, [ffxi] mode (full screen goes to windowed)
        const bool framed = mode_of(g_layout) == Mode::Windowed;
        if (!ini::set(g_ini, L"ffxi", L"mode", framed ? L"borderless" : L"windowed")) {
            say("couldn't write vanacore.ini");
            return true;
        }
        apply(g_ini.c_str(), g_exclusive);
        say(std::string("Window: ") + (framed ? "borderless windowed" : "windowed") + " ([ffxi] mode)");
        return true;
    }
    if (what == "max" || what == "maximize" || what == "min" || what == "minimize" || what == "restore") {
        ShowWindow(window, what.starts_with("max") ? SW_MAXIMIZE : what.starts_with("min") ? SW_MINIMIZE : SW_RESTORE);
        return true;
    }
    if (what == "save" || what == "forget") {
        const std::string whose = parts.size() > 2 ? lower(parts[2]) : "";
        const bool own = !whose.empty() && (whose == "char" || whose == "character" || whose == lower(g_character));
        if (own && g_character.empty()) {
            say("no character has logged in yet");
            return true;
        }
        const std::wstring key = own ? L"position." + wide(g_character) : L"position";
        const std::string name = "[window] " + ascii(key);
        RECT frame{};
        if (what == "save" && (IsIconic(window) || IsZoomed(window) || !GetWindowRect(window, &frame))) {
            say("restore the window first (it's minimized or maximized)");
            return true;
        }
        const std::wstring at = std::to_wstring(frame.left) + L"," + std::to_wstring(frame.top);
        if (!ini::set(g_ini, L"window", key.c_str(), what == "save" ? at.c_str() : nullptr)) {
            say("couldn't write vanacore.ini");
            return true;
        }
        g_remember = setting(L"position") == L"last";
        g_position = wanted(); // where it is now: nothing to move
        if (what == "save")
            say("Window: starts at " + std::to_string(frame.left) + ", " + std::to_string(frame.top) +
                (full ? " (full screen on that monitor)" : "") + (own ? " when " + g_character + " logs in" : "") + " (" +
                name + ")");
        else
            say("Window: " + name + " taken out" + (own ? "" : ": it starts where the game puts it"));
        return true;
    }
    usage();
    return true;
}

} // namespace vanacore::window
