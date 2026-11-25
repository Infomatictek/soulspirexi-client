#include "input.hpp"

#define DIRECTINPUT_VERSION 0x0800
#include <windows.h>

#include <dinput.h>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <map>
#include <initializer_list>
#include <mutex>

#include "addonbox.hpp"
#include "addons.hpp"
#include "binds.hpp"
#include "camera.hpp"
#include "gamepad.hpp"
#include "chat.hpp"
#include "gui.hpp"
#include "imports.hpp"
#include "log.hpp"
#include "menulog.hpp"
#include "outline.hpp"
#include "plugins.hpp"
#include "registry.hpp"
#include "scan.hpp"
#include "ui.hpp"
#include "window.hpp"

namespace vanacore::input {
namespace {

struct Settings {
    bool disable_enumeration = false; // gamepad.disableenumeration
    bool windows_key = true;          // keyboard.windowskeyenabled
    bool hook_thread = true;          // keyboard.hookthread (hook_thread_setting)
    bool unhook_mouse = true;         // mouse.unhook
} g_settings;

// gamepad.disable (/vana > Client > Gamepads > Gamepad: Disable), live: as gamepad.disableenumeration, and a controller
// the game already has reads as let go
std::atomic<bool> g_pads_off{false};

bool pads_off()
{
    return g_settings.disable_enumeration || g_pads_off.load(std::memory_order_relaxed);
}

HMODULE g_self = nullptr; // VanaCore.Core.dll, home of the wrapped keyboard hook procedures

void once(std::atomic<bool>& logged, const wchar_t* message)
{
    if (!logged.exchange(true))
        log::write(L"Input: %ls", message);
}

// ---- DirectInput: controller search ------------------------------------------------------------

// Vtable slots (dinput.h's declaration order).
constexpr int kCreateDevice = 3;   // IDirectInput8A
constexpr int kEnumDevices = 4;    // IDirectInput8A
constexpr int kGetDeviceState = 9; // IDirectInputDevice8A
constexpr int kGetDeviceData = 10; // IDirectInputDevice8A

// IID_IDirectInput8A; the game is ANSI throughout, and only that interface's vtable is hooked.
constexpr GUID kDirectInput8A = {0xBF798030, 0x483A, 0x4DA2, {0xAA, 0x99, 0x5D, 0x64, 0xED, 0x36, 0x97, 0x00}};

using DirectInput8CreateFn = HRESULT(WINAPI*)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN);
using EnumDevicesFn = HRESULT(STDMETHODCALLTYPE*)(IDirectInput8A*, DWORD, LPDIENUMDEVICESCALLBACKA, LPVOID, DWORD);
using CreateDeviceFn = HRESULT(STDMETHODCALLTYPE*)(IDirectInput8A*, REFGUID, LPDIRECTINPUTDEVICE8A*, LPUNKNOWN);
using GetDeviceStateFn = HRESULT(STDMETHODCALLTYPE*)(IDirectInputDevice8A*, DWORD, LPVOID);
using GetDeviceDataFn = HRESULT(STDMETHODCALLTYPE*)(IDirectInputDevice8A*, DWORD, LPDIDEVICEOBJECTDATA, LPDWORD, DWORD);

DirectInput8CreateFn g_direct_input_create = nullptr;
EnumDevicesFn g_enum_devices = nullptr;
CreateDeviceFn g_create_device = nullptr;
std::atomic<bool> g_logged_search{false};

// Joysticks, gamepads, wheels, flight sticks: DI8DEVCLASS_GAMECTRL's members.
bool is_controller(DWORD device_type)
{
    const DWORD type = GET_DIDEVICE_TYPE(device_type);
    return type >= DI8DEVTYPE_JOYSTICK && type <= DI8DEVTYPE_1STPERSON;
}

struct SearchFilter {
    LPDIENUMDEVICESCALLBACKA callback;
    LPVOID context;
};

BOOL CALLBACK skip_controllers(LPCDIDEVICEINSTANCEA device, LPVOID context)
{
    const auto* filter = static_cast<const SearchFilter*>(context);
    return is_controller(device->dwDevType) ? DIENUM_CONTINUE : filter->callback(device, filter->context);
}

// With the gamepad on and none of its controllers opened (or its window not in front: the game drops the device and
// makes it again), the game searches again every second or two, on the render thread. Each search is two DirectInput
// searches (force feedback ones first, when that switch is on), ~60-80 ms each on one PC (2026-10-11), and its
// callback asks WMI whether each controller found is really an XInput one, ~850 ms per controller (a keyboard dongle
// that shows up as a "controller" is enough). That is the game's, retail's too. Both are answered from memory here:
// - the WMI answer, kept per product (FUN_10008990's, below): it doesn't change for a product;
// - the search: a controller search's result (by type and flags) is handed to the game's callback again, as the search
//   would, for [input] gamepad.searchinterval seconds or until Windows says a device came or went (WM_DEVICECHANGE,
//   input::device_changed). The game's callback then does what it would: picks its controller, makes the device.
//   gamepad.searchinterval=0 runs every search.
constexpr ULONGLONG kDefaultRetrySeconds = 300;
std::atomic<ULONGLONG> g_retry_ms{kDefaultRetrySeconds * 1000}; // gamepad.searchinterval; 0 = never answered from memory
std::atomic<unsigned> g_device_epoch{0};     // +1 each time a device comes or goes: the searches kept are stale
std::atomic<int> g_logged_searches{0};
std::atomic<bool> g_logged_reuse{false};

std::mutex g_opened_guids_lock;
std::vector<GUID> g_opened_guids; // the instance GUIDs the game made controller devices for (CreateDevice)

// a controller search's result: every device it listed, in order
struct KeptSearch {
    DWORD type;
    DWORD flags;
    unsigned epoch;
    ULONGLONG at; // GetTickCount64()
    std::vector<DIDEVICEINSTANCEA> devices;
};
std::mutex g_searches_lock;
std::vector<KeptSearch> g_searches;

struct SearchRecord {
    LPDIENUMDEVICESCALLBACKA callback;
    LPVOID context;
    bool stopped; // the game's callback said DIENUM_STOP: the rest is only recorded
    std::vector<DIDEVICEINSTANCEA> devices;
};

// The whole list is recorded, past where the game's callback stops it: the next time, with another pad picked, the
// game may want a device further on.
BOOL CALLBACK record_search(LPCDIDEVICEINSTANCEA device, LPVOID context)
{
    auto* record = static_cast<SearchRecord*>(context);
    record->devices.push_back(*device);
    if (!record->stopped && record->callback(device, record->context) == DIENUM_STOP)
        record->stopped = true;
    return DIENUM_CONTINUE;
}

// FFXiMain's "is this DirectInput controller an XInput one" (Microsoft's IsXInputDevice sample, a WMI query
// over every PnP device). Located via runtime byte-pattern scan (fail-closed if unmatched) instead of a fixed RVA.
constexpr char kIsXInputPattern[] = "81 EC 84 00 00 00 53 55 56 57 33 DB B9 13 00 00 00";
constexpr size_t kIsXInputPrologue = 6; // SUB ESP,0x84
using IsXInputFn = BOOL(__cdecl*)(const GUID*);
IsXInputFn g_is_xinput = nullptr; // the game's, through the trampoline
std::atomic<bool> g_is_xinput_tried{false};
std::mutex g_xinput_answers_lock;
std::vector<std::pair<GUID, BOOL>> g_xinput_answers;

BOOL __cdecl is_xinput_device(const GUID* product)
{
    if (product == nullptr)
        return g_is_xinput(product);
    {
        const std::lock_guard lock(g_xinput_answers_lock);
        for (const auto& [known, answer] : g_xinput_answers)
            if (IsEqualGUID(known, *product))
                return answer;
    }
    const BOOL answer = g_is_xinput(product);
    const std::lock_guard lock(g_xinput_answers_lock);
    g_xinput_answers.emplace_back(*product, answer);
    return answer;
}

// From the game's first controller search, on its thread: nothing runs the check meanwhile.
void hook_is_xinput()
{
    const HMODULE game = GetModuleHandleW(L"FFXiMain.dll");
    const std::uintptr_t site = game != nullptr ? scan::find(game, kIsXInputPattern) : 0;
    if (site == 0) {
        log::write(L"Input: this client's XInput check pattern wasn't found; each controller search still asks WMI");
        return;
    }
    if (!imports::detour(reinterpret_cast<IsXInputFn>(site), kIsXInputPrologue, &is_xinput_device,
                         g_is_xinput)) {
        log::write(L"Input: the game's XInput check couldn't be hooked; each controller search still asks WMI");
        return;
    }
    log::write(L"Input: the game's XInput check (a slow WMI query) runs once per controller product (+0x%X)",
               static_cast<unsigned>(site - reinterpret_cast<std::uintptr_t>(game)));
}

HRESULT STDMETHODCALLTYPE enum_devices(IDirectInput8A* self, DWORD type, LPDIENUMDEVICESCALLBACKA callback,
                                       LPVOID context, DWORD flags)
{
    if (!pads_off() && callback != nullptr) {
        const bool pads_only = type == DI8DEVCLASS_GAMECTRL || is_controller(type);
        if (pads_only && !g_is_xinput_tried.exchange(true))
            hook_is_xinput();
        const ULONGLONG retry_ms = g_retry_ms.load(std::memory_order_relaxed);
        if (!pads_only || retry_ms == 0)
            return g_enum_devices(self, type, callback, context, flags);
        const unsigned epoch = g_device_epoch.load(std::memory_order_relaxed);
        const ULONGLONG now = GetTickCount64();
        std::vector<DIDEVICEINSTANCEA> kept;
        bool reuse = false;
        {
            const std::lock_guard lock(g_searches_lock);
            for (const KeptSearch& search : g_searches) {
                if (search.type == type && search.flags == flags && search.epoch == epoch && now - search.at < retry_ms) {
                    kept = search.devices;
                    reuse = true;
                }
            }
        }
        if (reuse) {
            once(g_logged_reuse, L"the game's repeated controller searches get the last search's result until a device is "
                                 L"plugged in or removed (each real one costs it a DirectInput search, and a slow WMI "
                                 L"query per controller)");
            for (const DIDEVICEINSTANCEA& device : kept)
                if (callback(&device, context) == DIENUM_STOP)
                    break;
            return DI_OK;
        }
        SearchRecord record{callback, context, false, {}};
        const HRESULT result = g_enum_devices(self, type, &record_search, &record, flags);
        const size_t found = record.devices.size();
        if (SUCCEEDED(result)) {
            const std::lock_guard lock(g_searches_lock);
            g_searches.erase(std::remove_if(g_searches.begin(), g_searches.end(),
                                            [&](const KeptSearch& search) {
                                                return search.type == type && search.flags == flags;
                                            }),
                             g_searches.end());
            g_searches.push_back({type, flags, epoch, now, std::move(record.devices)});
        }
        if (g_logged_searches.fetch_add(1) < 6)
            log::write(L"Input: the game's controller search (type 0x%X, flags 0x%X) found %u controller(s) in %llu ms",
                       type, flags, static_cast<unsigned>(found), GetTickCount64() - now);
        return result;
    }
    if (!pads_off() || callback == nullptr)
        return g_enum_devices(self, type, callback, context, flags);
    once(g_logged_search, L"the game's controller search finds nothing (gamepad.disableenumeration=1 or gamepad.disable=1)");
    // A controller-only search is skipped outright: that slow search is the stutter.
    if (type == DI8DEVCLASS_GAMECTRL || is_controller(type))
        return DI_OK;
    SearchFilter filter{callback, context};
    return g_enum_devices(self, type, &skip_controllers, &filter, flags);
}

#include "input_background.hpp" // controllers in the background: the game's "works while inactive" switch

// ---- DirectInput: the mouse over VanaCore's own windows ----------------------------------------
// FFXI reads the mouse's buttons, wheel and movement through DirectInput, not window messages, so a
// click the /vana window takes from the messages would still reach the game (dragging a slider
// turned the camera). While the cursor is over a box an addon marked block_mouse, or a press that
// began over one is still held, the mouse device reads no buttons and no wheel. Its movement still
// goes through, so the game's cursor keeps following the mouse.

// GUID_SysMouse
constexpr GUID kSysMouse = {0x6F1D2B60, 0xD5A0, 0x11CF, {0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00}};

IDirectInputDevice8A* g_mouse = nullptr; // the game's mouse device (the vtable is every device's)
GetDeviceStateFn g_get_device_state = nullptr;
GetDeviceDataFn g_get_device_data = nullptr;
std::atomic<bool> g_taken_press{false};    // a press a plugin or addon took: the game reads no buttons till they're up
std::atomic<ULONGLONG> g_wheel_until{0};   // a wheel turn one took: the game reads no wheel until then (GetTickCount64)
std::atomic<bool> g_frame_press{false}; // a press on the window's frame (window.cpp): Windows moves it
std::atomic<bool> g_drop_buffered{false}; // ... ended: drop what DirectInput buffered meanwhile
std::atomic<bool> g_modal{false};        // a modal window over the game (window.cpp): it reads no mouse or keyboard
std::atomic<bool> g_drop_keys{false};    // ... closed: drop what the keyboard buffered meanwhile
std::atomic<ULONGLONG> g_modal_quiet_until{0}; // ... closed: no mouse buttons for the game till then (GetTickCount64)
bool g_was_down = false;    // a mouse button was down at the last read
bool g_press_on_ui = false; // ... and it went down over a block_mouse box
std::atomic<bool> g_logged_ui_mouse{false};
std::atomic<bool> g_logged_frame{false};
std::atomic<bool> g_logged_di_mouse{false};
std::atomic<bool> g_logged_di_state{false};
std::atomic<bool> g_logged_di_data{false};

bool buttons_down()
{
    for (int key : {VK_LBUTTON, VK_RBUTTON, VK_MBUTTON, VK_XBUTTON1, VK_XBUTTON2})
        if ((GetAsyncKeyState(key) & 0x8000) != 0)
            return true;
    return false;
}

// True when VanaCore's UI has the mouse: it's over a block_mouse box with no button held, or a press
// that began over one is still held. A press that began over the game stays the game's.
bool ui_has_mouse()
{
    bool over = false;
    const HWND window = window::handle();
    POINT at{};
    if (window != nullptr && GetForegroundWindow() == window && GetCursorPos(&at) && ScreenToClient(window, &at)) {
        unsigned width = 0, height = 0;
        ui::screen(width, height);
        RECT client{};
        if (width != 0 && height != 0 && GetClientRect(window, &client) && client.right > 0 && client.bottom > 0) {
            at.x = MulDiv(at.x, static_cast<int>(width), client.right); // back buffer pixels, as ui draws
            at.y = MulDiv(at.y, static_cast<int>(height), client.bottom);
        }
        over = ui::blocks_mouse(at.x, at.y);
    }
    const bool down = buttons_down();
    if (down && !g_was_down)
        g_press_on_ui = over;
    g_was_down = down;
    const bool has = down ? g_press_on_ui : over;
    if (has)
        once(g_logged_ui_mouse, L"the mouse over VanaCore's windows doesn't reach the game (DirectInput)");
    return has;
}

// A press a plugin or addon took (FFXIDB's minimap, dragged): the game's buttons read up until
// they all are.
bool press_taken()
{
    if (!g_taken_press)
        return false;
    if (!buttons_down()) {
        g_taken_press = false;
        return false;
    }
    return true;
}

// A wheel turn a plugin or addon took (FFXIDB's zoom) doesn't turn the game's camera too.
bool wheel_taken()
{
    return GetTickCount64() < g_wheel_until;
}

// ---- DirectInput: the keyboard while ImGui has it --------------------------------------------
// Typing in an ImGui text box (gui::wants_keyboard) mustn't move the character or open menus: the
// game's keyboard reads every key up meanwhile, and its buffered presses are dropped (releases are
// kept, so a key held before stops).

// GUID_SysKeyboard
constexpr GUID kSysKeyboard = {0x6F1D2B61, 0xD5A0, 0x11CF, {0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00}};
IDirectInputDevice8A* g_keyboard = nullptr;
std::mutex g_controllers_lock;
// the game's DirectInput controllers (gamepad.cpp's events): each one's instance GUID and when the game last read it
struct GameController {
    GUID guid{};
    ULONGLONG read_at = 0;
};
std::map<IDirectInputDevice8A*, GameController> g_controllers;
constexpr ULONGLONG kReadingMs = 2000; // read this lately: the game uses it now (/vana's "In use")

bool is_game_controller(IDirectInputDevice8A* device)
{
    const std::lock_guard lock(g_controllers_lock);
    return g_controllers.count(device) != 0;
}

// is_game_controller, for a read the game made of it: kept as its last
bool game_controller_read(IDirectInputDevice8A* device)
{
    const std::lock_guard lock(g_controllers_lock);
    const auto found = g_controllers.find(device);
    if (found == g_controllers.end())
        return false;
    found->second.read_at = GetTickCount64();
    return true;
}
std::atomic<bool> g_logged_di_keyboard{false};
std::atomic<bool> g_logged_gui_keyboard{false};
// The keys the game's keyboard reads as up until they are (hide_key, and the reads below): their presses dropped, each
// hidden until a read shows it released (a state read with it up, or its release in the buffer). By DirectInput key
// code (DIK_*).
std::atomic<bool> g_hidden_keys[256];

// Ctrl+V and Ctrl+C in the open chat line are VanaCore's paste and copy (window.cpp): their V and C hidden from the
// game's keyboard (V is its first-person view).
bool chat_shortcut(unsigned key)
{
    if (key != DIK_V && key != DIK_C)
        return false;
    if ((GetAsyncKeyState(VK_CONTROL) & 0x8000) == 0 || (GetAsyncKeyState(VK_MENU) & 0x8000) != 0)
        return false;
    bool open = false;
    std::string line;
    return chat::input_line(open, line) && open;
}

// The modifiers held, as binds count them: the keys as they are now (GetAsyncKeyState), which is what the game's
// keyboard reads, not as of the last window message
unsigned modifiers_now()
{
    const auto held = [](int key) { return (GetAsyncKeyState(key) & 0x8000) != 0; };
    unsigned modifiers = 0;
    if (held(VK_CONTROL))
        modifiers |= binds::kCtrl;
    if (held(VK_MENU))
        modifiers |= binds::kAlt;
    if (held(VK_SHIFT))
        modifiers |= binds::kShift;
    if (held(VK_LWIN) || held(VK_RWIN))
        modifiers |= binds::kWin;
    if (held(VK_APPS))
        modifiers |= binds::kApps;
    return modifiers;
}

// An arrow key (alone or with Shift, not Ctrl or Alt) while the HUD Manager's picked boxes are up: it moves them
// (outline.cpp; Addon View's, addonbox.cpp), and the game's camera, character and menus don't get it
bool hud_arrow(unsigned key, unsigned modifiers)
{
    return (key == DIK_LEFT || key == DIK_RIGHT || key == DIK_UP || key == DIK_DOWN) &&
           (modifiers & (binds::kCtrl | binds::kAlt)) == 0 && (outline::takes_arrow() || addonbox::takes_arrow());
}

// A key a read of the game's keyboard shows pressed that's kept from the game from that read on, whichever comes first,
// the read or the key's window message (which hides it too, hide_key): a key a bind or a capture takes, and Ctrl+V or
// Ctrl+C in the open chat line. The game reads its keyboard once a frame, and a read that landed after a bound key went
// down but before its message was taken from the queue let the game act on the key: Alt+P bound to /follow opened the
// party chat every few presses, a bound numpad key moved the camera or a menu. The message alone hid it too late.
bool kept_from_game(unsigned key, unsigned modifiers)
{
    return chat_shortcut(key) || binds::takes_key(key, modifiers) || hud_arrow(key, modifiers) ||
           camera::takes_key(key, modifiers); // /freecam: the keys it flies with
}

// A whole read (GetDeviceState, 256 bytes) as DirectInput gave it, before plugins add the keys they hold down: the keys
// pressed that are kept from the game are hidden from here on.
void hide_note_state(const BYTE* keys, DWORD size)
{
    const DWORD count = std::min<DWORD>(size, 256);
    unsigned modifiers = 0;
    bool known = false;
    for (unsigned key = 0; key < count; ++key) {
        if ((keys[key] & 0x80) == 0 || g_hidden_keys[key])
            continue;
        if (!known) {
            modifiers = modifiers_now();
            known = true;
        }
        if (kept_from_game(key, modifiers))
            g_hidden_keys[key] = true;
    }
}

// A whole read as the game gets it: the hidden keys up; one seen up isn't hidden any more.
void hide_in_state(BYTE* keys, DWORD size)
{
    const DWORD count = std::min<DWORD>(size, 256);
    for (unsigned key = 0; key < count; ++key) {
        if (!g_hidden_keys[key])
            continue;
        if ((keys[key] & 0x80) != 0)
            keys[key] = 0;
        else
            g_hidden_keys[key] = false;
    }
}

// A buffered read (GetDeviceData) as DirectInput gave it, before plugins add the keys they send: `hide[key]` set for each
// press in it that's kept from the game. In the read's order, so a key let go and pressed again within one read is asked
// about again. DIDEVICEOBJECTDATA: dwOfs (the key), dwData (0x80 set: pressed).
void hide_note_buffer(const BYTE* bytes, DWORD object_size, DWORD count, bool (&hide)[256])
{
    bool hidden[256];
    for (unsigned key = 0; key < 256; ++key)
        hidden[key] = g_hidden_keys[key];
    unsigned modifiers = 0;
    bool known = false;
    for (DWORD i = 0; i < count; ++i) {
        const auto* event = reinterpret_cast<const DWORD*>(bytes + i * object_size);
        const DWORD key = event[0];
        if (key >= 256)
            continue;
        if ((event[1] & 0x80) == 0) {
            hidden[key] = false;
            continue;
        }
        if (hidden[key])
            continue;
        if (!known) {
            modifiers = modifiers_now();
            known = true;
        }
        if (kept_from_game(key, modifiers)) {
            hide[key] = true;
            hidden[key] = true;
        }
    }
}

// A buffered read as the game gets it: the hidden keys' events dropped, a release ending its key's hiding; a press
// hide_note_buffer marked hides its key from here on, and is dropped.
void hide_in_buffer(BYTE* bytes, DWORD object_size, DWORD& count, const bool (&hide)[256])
{
    DWORD kept = 0;
    for (DWORD i = 0; i < count; ++i) {
        const auto* event = reinterpret_cast<const DWORD*>(bytes + i * object_size);
        const DWORD key = event[0];
        const bool down = (event[1] & 0x80) != 0;
        if (key < 256) {
            if (g_hidden_keys[key]) {
                if (!down)
                    g_hidden_keys[key] = false;
                continue;
            }
            if (down && hide[key]) {
                g_hidden_keys[key] = true;
                continue;
            }
        }
        if (kept != i)
            std::memmove(bytes + kept * object_size, bytes + i * object_size, object_size);
        ++kept;
    }
    count = kept;
}

bool gui_has_keyboard()
{
    const bool has = gui::wants_keyboard();
    if (has)
        once(g_logged_gui_keyboard, L"typing in an ImGui window doesn't reach the game (DirectInput keyboard)");
    return has;
}

// A buffered event (DIDEVICEOBJECTDATA: dwOfs, dwData) that's the release of a button or key from `first` to `last`.
bool released(const BYTE* event, DWORD first, DWORD last)
{
    const auto* fields = reinterpret_cast<const DWORD*>(event);
    return fields[0] >= first && fields[0] <= last && (fields[1] & 0x80) == 0;
}

// What a device buffered, read out and dropped, all but the releases of the buttons or keys from `first` to `last`: those
// are what the read returns (a press the game saw is let go for it). `empty`: nothing's left in the buffer.
HRESULT drain(IDirectInputDevice8A* device, DWORD object_size, LPDIDEVICEOBJECTDATA data, LPDWORD count, DWORD flags,
              DWORD first, DWORD last, bool& empty)
{
    const DWORD room = *count;
    HRESULT result = DI_OK;
    DWORD kept = 0;
    empty = false;
    auto* bytes = reinterpret_cast<BYTE*>(data);
    for (int reads = 0; reads < 64 && kept == 0; ++reads) {
        DWORD n = room;
        result = g_get_device_data(device, object_size, data, &n, flags & ~DIGDD_PEEK);
        if (FAILED(result) || n == 0 || data == nullptr) {
            empty = true;
            break;
        }
        if (object_size < 2 * sizeof(DWORD))
            continue;
        for (DWORD i = 0; i < n; ++i) {
            if (!released(bytes + i * object_size, first, last))
                continue;
            if (kept != i)
                std::memmove(bytes + kept * object_size, bytes + i * object_size, object_size);
            ++kept;
        }
    }
    *count = kept;
    return result;
}

// The mouse buttons the game holds down, by what its buffered reads gave it (bit n: DIMOFS_BUTTON0 + n)
unsigned g_game_buttons = 0;

// Button n of DirectInput's mouse held now (DirectInput's buttons are the physical ones; GetAsyncKeyState's are swapped
// with the buttons)
bool button_held(unsigned n)
{
    static constexpr int kKeys[] = {VK_LBUTTON, VK_RBUTTON, VK_MBUTTON, VK_XBUTTON1, VK_XBUTTON2};
    if (n >= 5)
        return false;
    int key = kKeys[n];
    if (n < 2 && GetSystemMetrics(SM_SWAPBUTTON) != 0)
        key = kKeys[1 - n];
    return (GetAsyncKeyState(key) & 0x8000) != 0;
}

// A buffered mouse read as the game gets it, last of all: while modal_quiet no press, a release only of a button the game
// holds, and a button the game holds that's up now is let go for it (a release it never got, the double click on a file in
// an import's dialog, left it holding a click: Enter did nothing, the camera or walking stuck).
void game_buttons(BYTE* bytes, DWORD object_size, DWORD& count, DWORD capacity)
{
    if (bytes == nullptr || object_size < 4 * sizeof(DWORD))
        return;
    const bool quiet = modal_quiet();
    DWORD kept = 0;
    for (DWORD i = 0; i < count; ++i) {
        const auto* event = reinterpret_cast<const DWORD*>(bytes + i * object_size); // dwOfs, dwData, dwTimeStamp, dwSequence
        if (event[0] >= DIMOFS_BUTTON0 && event[0] <= DIMOFS_BUTTON7) {
            const unsigned bit = 1u << (event[0] - DIMOFS_BUTTON0);
            if ((event[1] & 0x80) != 0) {
                if (quiet)
                    continue;
                g_game_buttons |= bit;
            } else {
                if ((g_game_buttons & bit) == 0)
                    continue;
                g_game_buttons &= ~bit;
            }
        }
        if (kept != i)
            std::memmove(bytes + kept * object_size, bytes + i * object_size, object_size);
        ++kept;
    }
    count = kept;
    for (unsigned n = 0; n < 8 && g_game_buttons != 0; ++n) {
        const unsigned bit = 1u << n;
        if ((g_game_buttons & bit) == 0 || button_held(n) || count >= capacity)
            continue;
        auto* event = reinterpret_cast<DWORD*>(bytes + count * object_size);
        std::memset(event, 0, object_size);
        event[0] = DIMOFS_BUTTON0 + n;
        event[2] = GetTickCount();
        event[3] = count > 0 ? reinterpret_cast<const DWORD*>(bytes + (count - 1) * object_size)[3] : 0;
        ++count;
        g_game_buttons &= ~bit;
        log::write(L"Input: the game held mouse button %u with it up (a release it never got): let go for it", n);
    }
}

// DIMOUSESTATE / DIMOUSESTATE2: lX, lY, lZ, then the buttons. Over the UI: no wheel, no buttons.
// While the window's frame is pressed: nothing at all. The keyboard's 256 keys: all up while ImGui
// has it.
std::atomic<bool> g_logged_pad_kept{false};
std::atomic<bool> g_logged_reacquired{false};

// A read of the game's keyboard or mouse that failed for want of its acquisition (`result`): acquired again, true, when the
// game's window is in front. They're opened for the foreground and lose it when another window takes the focus, one of the
// game's own process too (Windows' Open dialog from a Browse button, an addon's message box), and the game acquires them again
// only as Windows tells it it's back in front (WM_ACTIVATEAPP), which isn't sent when the focus comes back from a window of
// the same process: no key or mouse button worked until Alt+Tab (reported 2026-11-06)
bool reacquired(IDirectInputDevice8A* device, HRESULT result)
{
    if ((result != DIERR_NOTACQUIRED && result != DIERR_INPUTLOST) || device == nullptr || (device != g_keyboard && device != g_mouse))
        return false;
    const HWND game = window::handle();
    if (game == nullptr || GetForegroundWindow() != game || FAILED(device->Acquire()))
        return false;
    once(g_logged_reacquired, L"the game's keyboard or mouse had lost DirectInput's acquisition with its window in front (a "
                              L"window of its own, a file dialog, had the focus): acquired again");
    return true;
}

HRESULT STDMETHODCALLTYPE get_device_state(IDirectInputDevice8A* device, DWORD size, LPVOID data)
{
    HRESULT result = g_get_device_state(device, size, data);
    if (reacquired(device, result))
        result = g_get_device_state(device, size, data);
    // A controller the game opened for the foreground only loses its acquisition when the game goes in the background;
    // the game then releases it and waits 90 frames (FUN_10005A40) before searching and opening it again, which was the
    // pause before the pad worked after tabbing back. It's acquired again here instead, and while the game is in the
    // background (where it can't be) it reads as let go: the game keeps it, and has it on its first read in front.
    if ((result == DIERR_NOTACQUIRED || result == DIERR_INPUTLOST) && device != nullptr && data != nullptr &&
        device != g_keyboard && device != g_mouse && is_game_controller(device)) {
        if (SUCCEEDED(device->Acquire()))
            result = g_get_device_state(device, size, data);
        if (FAILED(result) && window::in_background()) {
            once(g_logged_pad_kept, L"the game's controller reads as let go while the game is in the background (it's "
                                    L"kept, not opened again, so it works at once when the game is back in front)");
            let_go(device, data, size);
            result = DI_OK;
        }
    }
    const bool keyboard = SUCCEEDED(result) && device == g_keyboard && device != nullptr && data != nullptr;
    const bool gui_keyboard = keyboard && gui_has_keyboard();
    // the keys pressed that are kept from the game, by the keyboard as it is (the keys plugins hold down aren't asked about)
    if (keyboard && !gui_keyboard)
        hide_note_state(static_cast<const BYTE*>(data), size);
    // plugins' keyboard callbacks first (keys they hold down), addons' key_state, then VanaCore's own keeping keys from the game
    if (keyboard) {
        plugins::on_keyboard_state(device, size, data);
        if (menulog::on() && size >= 256) {
            // a recording runs: the menu keys held that an addon's key_state lets go, logged
            std::uint8_t before[256];
            std::memcpy(before, data, sizeof before);
            addons::on_key_state(data, size);
            menulog::key_state(before, static_cast<const std::uint8_t*>(data));
        } else {
            addons::on_key_state(data, size);
        }
    }
    if (SUCCEEDED(result) && device != nullptr && data != nullptr && device != g_keyboard && device != g_mouse &&
        game_controller_read(device)) {
        if (controllers_quiet())
            let_go(device, data, size);
        gamepad::on_dinput_state(device, size, data);
    }
    if (gui_keyboard || (keyboard && g_modal))
        std::memset(data, 0, size);
    else if (keyboard)
        hide_in_state(static_cast<BYTE*>(data), size);
    if (device == g_mouse)
        once(g_logged_di_state, L"the game reads the mouse through DirectInput (GetDeviceState)");
    if (SUCCEEDED(result) && device == g_mouse && data != nullptr && size > 8) {
        if (g_frame_press || g_modal)
            std::memset(data, 0, size);
        else if (modal_quiet() || ui_has_mouse() || press_taken())
            std::memset(static_cast<BYTE*>(data) + 8, 0, size - 8);
        else if (wheel_taken())
            std::memset(static_cast<BYTE*>(data) + 8, 0, 4); // lZ
    }
    return result;
}

// Buffered: over the UI, the wheel's (DIMOFS_Z) and the buttons' events are dropped, X and Y kept.
HRESULT get_device_data_kept(IDirectInputDevice8A* device, DWORD object_size, LPDIDEVICEOBJECTDATA data, LPDWORD count,
                              DWORD flags)
{
    if (device == g_mouse)
        once(g_logged_di_data, L"the game reads the mouse through DirectInput (GetDeviceData)");
    if (device == g_mouse && count != nullptr && modal_quiet()) {
        // a modal window is (or just was) over the game: every buffered event goes, the buttons' releases too (the game
        // never saw their presses)
        bool empty = false;
        return drain(device, object_size, data, count, flags, 1, 0, empty);
    }
    if (device == g_mouse && count != nullptr && (g_frame_press || g_modal || g_drop_buffered)) {
        // the window's frame is (or was) pressed, or a modal window is (or was) over the game: everything buffered meanwhile
        // goes but the buttons' releases (one the game saw pressed must reach it)
        bool empty = false;
        const HRESULT result = drain(device, object_size, data, count, flags, DIMOFS_BUTTON0, DIMOFS_BUTTON7, empty);
        if (empty)
            g_drop_buffered = false;
        once(g_logged_frame, L"a press on the window's frame (moving or sizing it) doesn't reach the game (DirectInput)");
        return result;
    }
    if (device == g_keyboard && device != nullptr && count != nullptr && (g_modal || g_drop_keys)) {
        // a modal window is (or was) over the game: what was typed in it goes but the releases
        bool empty = false;
        const HRESULT result = drain(device, object_size, data, count, flags, 0, 255, empty);
        if (empty && !g_modal)
            g_drop_keys = false;
        // a release that reaches the game ends its key's hiding (hide_in_buffer's, not run here): the key held as the modal
        // closed (Enter on a message box) is let go, and its next press is the game's
        if (SUCCEEDED(result) && data != nullptr && object_size >= 2 * sizeof(DWORD)) {
            const auto* bytes = reinterpret_cast<const BYTE*>(data);
            for (DWORD i = 0; i < *count; ++i) {
                const DWORD key = reinterpret_cast<const DWORD*>(bytes + i * object_size)[0];
                if (key < 256)
                    g_hidden_keys[key] = false;
            }
        }
        return result;
    }
    const DWORD capacity = count != nullptr ? *count : 0;
    const HRESULT result = g_get_device_data(device, object_size, data, count, flags);
    const bool keys = SUCCEEDED(result) && device == g_keyboard && device != nullptr && data != nullptr && count != nullptr &&
                      object_size >= 2 * sizeof(DWORD);
    const bool gui_keyboard = keys && gui_has_keyboard();
    // the presses that are kept from the game, by the keyboard as it is (the keys plugins send aren't asked about)
    bool hide[256] = {};
    if (keys && *count != 0 && !gui_keyboard)
        hide_note_buffer(reinterpret_cast<const BYTE*>(data), object_size, *count, hide);
    if (SUCCEEDED(result) && device != nullptr && data != nullptr && count != nullptr && device != g_keyboard &&
        device != g_mouse && game_controller_read(device)) {
        if (controllers_quiet())
            *count = 0;
        gamepad::on_dinput_data(device, object_size, data, count);
    }
    // plugins' keyboard callbacks first (keys they send), addons' key_data, then VanaCore's own keeping keys from the game
    if (SUCCEEDED(result) && device == g_keyboard && device != nullptr && data != nullptr && count != nullptr) {
        plugins::on_keyboard_data(device, object_size, data, count, capacity, flags);
        if (*count != 0 && object_size >= 2 * sizeof(DWORD) && (flags & DIGDD_PEEK) == 0 && addons::wants_key_data()) {
            auto* bytes = reinterpret_cast<BYTE*>(data);
            DWORD kept = 0;
            for (DWORD i = 0; i < *count; ++i) {
                const auto* event = reinterpret_cast<const DWORD*>(bytes + i * object_size); // dwOfs, dwData
                if (addons::on_key_data(event[0], (event[1] & 0x80) != 0, false)) {
                    if (menulog::on()) {
                        const std::string by = addons::blocked_by().empty() ? std::string("an addon's key_data")
                                                                            : addons::blocked_by() + "'s key_data";
                        menulog::key(event[0], (event[1] & 0x80) != 0, by.c_str());
                    }
                    continue;
                }
                if (kept != i)
                    std::memmove(bytes + kept * object_size, bytes + i * object_size, object_size);
                ++kept;
            }
            *count = kept;
        }
    }
    if (keys && *count != 0 && gui_keyboard) {
        // DIDEVICEOBJECTDATA: dwOfs (the key), dwData (0x80 set: pressed). Presses go, releases stay.
        auto* bytes = reinterpret_cast<BYTE*>(data);
        DWORD kept = 0;
        for (DWORD i = 0; i < *count; ++i) {
            const DWORD state = *reinterpret_cast<const DWORD*>(bytes + i * object_size + sizeof(DWORD));
            if ((state & 0x80) != 0) {
                if ((flags & DIGDD_PEEK) == 0)
                    menulog::key(*reinterpret_cast<const DWORD*>(bytes + i * object_size), true,
                                 "an ImGui window that has the keyboard (VanaCore's or an addon's)");
                continue;
            }
            if (kept != i)
                std::memmove(bytes + kept * object_size, bytes + i * object_size, object_size);
            ++kept;
        }
        *count = kept;
        return result;
    }
    if (keys && *count != 0 && menulog::on() && (flags & DIGDD_PEEK) == 0) {
        // a recording runs: the presses a key bind keeps from the game (hide_in_buffer drops them), and the menu keys that
        // reach it
        const auto* bytes = reinterpret_cast<const BYTE*>(data);
        for (DWORD i = 0; i < *count; ++i) {
            const auto* event = reinterpret_cast<const DWORD*>(bytes + i * object_size);
            const bool down = (event[1] & 0x80) != 0;
            const bool bound = event[0] < 256 && (g_hidden_keys[event[0]] || (down && hide[event[0]]));
            menulog::key(event[0], down, bound ? "a key bind (VanaCore keeps its key from the game)" : nullptr);
        }
    }
    if (keys && *count != 0)
        hide_in_buffer(reinterpret_cast<BYTE*>(data), object_size, *count, hide);
    const bool all_but_movement = device == g_mouse && (ui_has_mouse() || press_taken());
    if (SUCCEEDED(result) && device == g_mouse && data != nullptr && count != nullptr && *count != 0 &&
        object_size >= sizeof(DWORD) && (all_but_movement || wheel_taken())) {
        auto* bytes = reinterpret_cast<BYTE*>(data);
        DWORD kept = 0;
        for (DWORD i = 0; i < *count; ++i) {
            const DWORD offset = *reinterpret_cast<const DWORD*>(bytes + i * object_size); // dwOfs
            // a button's release goes through: one the game saw pressed, let go over VanaCore's windows, stayed held for it
            // (the right button held is the game's cancel: Enter did nothing until it was clicked again)
            if (all_but_movement ? offset >= DIMOFS_Z && !(object_size >= 2 * sizeof(DWORD) && released(bytes + i * object_size, DIMOFS_BUTTON0, DIMOFS_BUTTON7))
                                 : offset == DIMOFS_Z)
                continue;
            if (kept != i)
                std::memmove(bytes + kept * object_size, bytes + i * object_size, object_size);
            ++kept;
        }
        *count = kept;
    }
    return result;
}

HRESULT STDMETHODCALLTYPE get_device_data(IDirectInputDevice8A* device, DWORD object_size, LPDIDEVICEOBJECTDATA data,
                                          LPDWORD count, DWORD flags)
{
    const DWORD capacity = count != nullptr ? *count : 0;
    HRESULT result = get_device_data_kept(device, object_size, data, count, flags);
    if (reacquired(device, result)) {
        if (count != nullptr)
            *count = capacity;
        result = get_device_data_kept(device, object_size, data, count, flags);
    }
    if (SUCCEEDED(result) && device == g_mouse && device != nullptr && data != nullptr && count != nullptr &&
        (flags & DIGDD_PEEK) == 0)
        game_buttons(reinterpret_cast<BYTE*>(data), object_size, *count, capacity);
    return result;
}

HRESULT STDMETHODCALLTYPE create_device(IDirectInput8A* self, REFGUID guid, LPDIRECTINPUTDEVICE8A* device,
                                        LPUNKNOWN outer)
{
    const HRESULT result = g_create_device(self, guid, device, outer);
    if (SUCCEEDED(result) && device != nullptr && *device != nullptr) {
        // the mouse: the system mouse by its GUID, or any device that says it's a mouse
        bool mouse = IsEqualGUID(guid, kSysMouse) != 0;
        if (!mouse) {
            DIDEVCAPS caps{};
            caps.dwSize = sizeof caps;
            mouse = SUCCEEDED((*device)->GetCapabilities(&caps)) && GET_DIDEVICE_TYPE(caps.dwDevType) == DI8DEVTYPE_MOUSE;
        }
        if (mouse) {
            once(g_logged_di_mouse, L"the game made a DirectInput mouse; over VanaCore's windows it reads no buttons");
            g_mouse = *device;
            imports::hook_vtable(*device, kGetDeviceState, &get_device_state, g_get_device_state);
            imports::hook_vtable(*device, kGetDeviceData, &get_device_data, g_get_device_data);
        } else if (DIDEVCAPS caps{sizeof caps}; SUCCEEDED((*device)->GetCapabilities(&caps)) && is_controller(caps.dwDevType)) {
            // a controller: its reads go through gamepad.cpp (plugins' and addons' controller events)
            {
                const std::lock_guard lock(g_controllers_lock);
                g_controllers[*device] = GameController{guid, 0};
            }
            bool known = false;
            {
                const std::lock_guard lock(g_opened_guids_lock);
                known = std::any_of(g_opened_guids.begin(), g_opened_guids.end(),
                                    [&](const GUID& opened) { return IsEqualGUID(opened, guid) != 0; });
                if (!known)
                    g_opened_guids.push_back(guid);
            }
            // The game makes the device again every second or two while its window isn't in front (seen: 136 times in a
            // session): the first time a device is opened is what the log is for
            if (!known) {
                wchar_t name[MAX_PATH] = L"?";
                if (DIDEVICEINSTANCEA info{sizeof info}; SUCCEEDED((*device)->GetDeviceInfo(&info)))
                    MultiByteToWideChar(CP_ACP, 0, info.tszInstanceName, -1, name, MAX_PATH);
                log::write(L"Input: the game opened a DirectInput controller \"%ls\" {%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X} "
                           L"(type 0x%X)",
                           name, guid.Data1, guid.Data2, guid.Data3, guid.Data4[0], guid.Data4[1], guid.Data4[2],
                           guid.Data4[3], guid.Data4[4], guid.Data4[5], guid.Data4[6], guid.Data4[7], caps.dwDevType);
            }
            imports::hook_vtable(*device, kGetDeviceState, &get_device_state, g_get_device_state);
            imports::hook_vtable(*device, kGetDeviceData, &get_device_data, g_get_device_data);
        } else if (IsEqualGUID(guid, kSysKeyboard)) {
            once(g_logged_di_keyboard, L"the game made a DirectInput keyboard; while ImGui has the keyboard it reads no keys");
            g_keyboard = *device;
            imports::hook_vtable(*device, kGetDeviceState, &get_device_state, g_get_device_state);
            imports::hook_vtable(*device, kGetDeviceData, &get_device_data, g_get_device_data);
        }
    }
    return result;
}

HRESULT WINAPI direct_input_create(HINSTANCE instance, DWORD version, REFIID iid, LPVOID* object, LPUNKNOWN outer)
{
    const HRESULT result = g_direct_input_create(instance, version, iid, object, outer);
    if (SUCCEEDED(result) && object != nullptr && *object != nullptr && IsEqualIID(iid, kDirectInput8A)) {
        imports::hook_vtable(*object, kEnumDevices, &enum_devices, g_enum_devices);
        imports::hook_vtable(*object, kCreateDevice, &create_device, g_create_device);
    }
    return result;
}

// ---- XInput, through FFXI's xinputdll.dll --------------------------------------------------

// FFXiMain.dll looks these up with GetProcAddress. They are __cdecl, not WINAPI.
using XInputGetStateFn = DWORD(__cdecl*)(DWORD user, void* state);

decltype(&GetProcAddress) g_get_proc_address = nullptr;
XInputGetStateFn g_xinput_get_state = nullptr;
std::atomic<bool> g_logged_xinput_off{false};

DWORD __cdecl xinput_get_state(DWORD, void* state)
{
    once(g_logged_xinput_off, L"XInput reports no controllers (gamepad.disableenumeration=1 or gamepad.disable=1)");
    if (state != nullptr)
        std::memset(state, 0, 16); // XINPUT_STATE, cleared as xinputdll.dll does
    return ERROR_DEVICE_NOT_CONNECTED;
}

// The game's XInput reads: plugins' and addons' controller events (gamepad.cpp)
// A controller that isn't there is slow to ask about, and the game asks every frame: once it says not connected, the
// next reads of that slot answer the same for kXInputAbsentMs without asking (a pad plugged in is found within that).
constexpr ULONGLONG kXInputAbsentMs = 2000;
std::atomic<ULONGLONG> g_xinput_absent_until[4]{};
std::atomic<ULONGLONG> g_xinput_read_at[4]{}; // when the game last read a connected pad from each slot (/vana's "In use")

DWORD __cdecl xinput_read(DWORD user, void* state)
{
    if (g_pads_off.load(std::memory_order_relaxed))
        return xinput_get_state(user, state);
    const bool tracked = user < 4;
    if (tracked && state != nullptr && GetTickCount64() < g_xinput_absent_until[user].load(std::memory_order_relaxed)) {
        std::memset(state, 0, 16); // XINPUT_STATE, cleared as xinputdll.dll does
        return ERROR_DEVICE_NOT_CONNECTED;
    }
    const DWORD result = g_xinput_get_state(user, state);
    if (tracked)
        g_xinput_absent_until[user].store(result == ERROR_DEVICE_NOT_CONNECTED ? GetTickCount64() + kXInputAbsentMs : 0,
                                          std::memory_order_relaxed);
    if (result == ERROR_SUCCESS && tracked)
        g_xinput_read_at[user].store(GetTickCount64(), std::memory_order_relaxed);
    if (result == ERROR_SUCCESS && state != nullptr) {
        if (controllers_quiet()) {
            // XINPUT_STATE: the packet number, then the pad, let go. A different packet number, so a reader that skips
            // an unchanged one sees the change.
            auto* bytes = static_cast<BYTE*>(state);
            DWORD packet = 0;
            std::memcpy(&packet, bytes, sizeof packet);
            ++packet;
            std::memcpy(bytes, &packet, sizeof packet);
            std::memset(bytes + sizeof packet, 0, 12);
        }
        gamepad::on_xinput_state(user, state);
    }
    return result;
}

FARPROC WINAPI get_proc_address(HMODULE module, LPCSTR name)
{
    const FARPROC proc = g_get_proc_address(module, name);
    if (proc == nullptr || IS_INTRESOURCE(name))
        return proc;
    if (g_settings.disable_enumeration && std::strcmp(name, "XInputGetState_") == 0)
        return reinterpret_cast<FARPROC>(static_cast<XInputGetStateFn>(&xinput_get_state));
    if (std::strcmp(name, "XInputGetState_") == 0) {
        g_xinput_get_state = reinterpret_cast<XInputGetStateFn>(proc);
        return reinterpret_cast<FARPROC>(static_cast<XInputGetStateFn>(&xinput_read));
    }
    return proc;
}

// ---- Keyboard hooks: the Windows keys ------------------------------------------------------

// The game blocks the Windows keys with a keyboard hook. Each one it installs gets one of these
// slots: VanaCore installs slot N's procedure instead, which hands every key but the Windows keys
// to the game's procedure (all of them with keyboard.windowskeyenabled=0).
// A low-level hook runs on the thread that set it, and every key pressed anywhere on the PC waits for
// it: set on the game's thread, typing in any program would wait for the game's next frame (or a
// zone's loading). So a low-level one is set on a thread of its own, which only answers it.
struct KeyboardHook {
    HOOKPROC proc = nullptr; // the game's
    int type = 0;
    HHOOK handle = nullptr;
    DWORD thread = 0; // a low-level hook's own thread (it takes the hook off as it ends)
};
constexpr int kKeyboardSlots = 4;
KeyboardHook g_keyboard_hooks[kKeyboardSlots];
std::mutex g_keyboard_mutex;
std::atomic<bool> g_logged_windows_key{false};
std::atomic<bool> g_logged_no_slot{false};
std::atomic<bool> g_logged_hook_thread{false};
std::atomic<bool> g_logged_win_bind{false};

decltype(&SetWindowsHookExA) g_set_windows_hook = nullptr;
decltype(&UnhookWindowsHookEx) g_unhook_windows_hook = nullptr;

// A bound key pressed with a Windows key held (@f1): Windows takes Win+F1 (its help), Win+G (the Game Bar) and the rest
// for itself before the game's window sees them, so binds on them are answered here, in the first low-level hook's
// thread (binds::take_win_key queues the command for the game's). A bound one goes to no one else; an unbound one goes
// on as ever (the Windows keys' own setting: to Windows, or swallowed by the game's hook). Hooks see every key on the PC:
// only the game being in front takes one.
constexpr ULONG_PTR kMaskInfo = 0x56414E41; // the keys VanaCore sends so the Start menu doesn't open (mask_release)
std::atomic<int> g_win_slot{-1};            // the hook that answers them
unsigned g_win_held = 0;                    // 1 the left Windows key down, 2 the right (that hook's thread's)
bool g_win_bound = false;                   // a bind took a key while they were down: their release is masked
std::map<DWORD, unsigned> g_win_taken;      // keys a bind took with one held, with the modifiers they went down with

unsigned held_modifiers()
{
    const auto held = [](int key) { return (GetAsyncKeyState(key) & 0x8000) != 0; };
    unsigned modifiers = 0;
    if (held(VK_CONTROL))
        modifiers |= binds::kCtrl;
    if (held(VK_MENU))
        modifiers |= binds::kAlt;
    if (held(VK_SHIFT))
        modifiers |= binds::kShift;
    if (held(VK_APPS))
        modifiers |= binds::kApps;
    if (g_win_held != 0)
        modifiers |= binds::kWin;
    return modifiers;
}

bool is_modifier(DWORD key)
{
    switch (key) {
    case VK_SHIFT: case VK_LSHIFT: case VK_RSHIFT: case VK_CONTROL: case VK_LCONTROL: case VK_RCONTROL:
    case VK_MENU: case VK_LMENU: case VK_RMENU: case VK_APPS:
        return true;
    default:
        return false;
    }
}

// Windows opens the Start menu when a Windows key goes up with no other key pressed since it went down, and it never saw
// the key a bind took. So that Windows key's release is held back and sent again behind a key that does nothing
// (unassigned 0xE8, as AutoHotkey's menu mask): mask down, mask up, the Windows key up, in that order whatever Windows
// does with keys sent from inside a hook. The mask sent once as the bind fired wasn't enough: the Start menu opened on
// every release all the same (borderless windowed, Win+1 to Win+0 bound by tHotBar). Sent with
// keyboard.windowskeyenabled=0 too: the game's hook (FFXiMain+0x1100) swallows the Windows keys only in its full screen,
// so in a window Windows still gets them. True: sent, the real release is kept; false: Windows gets the real one.
bool mask_release(const KBDLLHOOKSTRUCT& key)
{
    INPUT keys[3] = {};
    for (INPUT& input : keys) {
        input.type = INPUT_KEYBOARD;
        input.ki.wVk = 0xE8;
        input.ki.dwExtraInfo = kMaskInfo;
    }
    keys[1].ki.dwFlags = KEYEVENTF_KEYUP;
    keys[2].ki.wVk = static_cast<WORD>(key.vkCode);
    keys[2].ki.wScan = static_cast<WORD>(key.scanCode);
    keys[2].ki.dwFlags = KEYEVENTF_KEYUP | ((key.flags & LLKHF_EXTENDED) != 0 ? KEYEVENTF_EXTENDEDKEY : 0);
    return SendInput(3, keys, sizeof(INPUT)) == 3; // not all of it: the real release goes on, so the key isn't left down
}

// True: the hook keeps the key from everyone (a bind took it, or it's a Windows key's release mask_release sent again).
bool win_bind(const KBDLLHOOKSTRUCT& key, bool down)
{
    const DWORD vk = key.vkCode;
    if (vk == VK_LWIN || vk == VK_RWIN) {
        const unsigned bit = vk == VK_LWIN ? 1u : 2u;
        if (down) {
            g_win_held |= bit;
            return false;
        }
        g_win_held &= ~bit;
        const bool bound = g_win_bound;
        if (g_win_held == 0)
            g_win_bound = false;
        return bound && mask_release(key);
    }
    const unsigned code = binds::message_key(vk, key.scanCode, (key.flags & LLKHF_EXTENDED) != 0);
    if (const auto taken = g_win_taken.find(vk); taken != g_win_taken.end()) {
        // its repeats and its release are kept too; the release runs the key's up binds
        if (!down) {
            const unsigned modifiers = taken->second;
            g_win_taken.erase(taken);
            binds::take_win_key(code, false, false, modifiers);
        }
        return true;
    }
    if (!down || g_win_held == 0 || is_modifier(vk))
        return false;
    const HWND game = window::handle();
    if (game == nullptr || GetForegroundWindow() != game)
        return false;
    const unsigned modifiers = held_modifiers();
    if (!binds::take_win_key(code, true, false, modifiers))
        return false;
    g_win_taken[vk] = modifiers;
    g_win_bound = true;
    once(g_logged_win_bind, L"a key bound with the Windows key (@) is kept from Windows and the game");
    return true;
}

template <int N>
LRESULT CALLBACK keyboard_proc(int code, WPARAM wparam, LPARAM lparam)
{
    const KeyboardHook& hook = g_keyboard_hooks[N];
    if (hook.proc == nullptr) // unhooked while a call was on its way
        return CallNextHookEx(nullptr, code, wparam, lparam);
    if (code == HC_ACTION && hook.type == WH_KEYBOARD_LL) {
        const auto& key = *reinterpret_cast<const KBDLLHOOKSTRUCT*>(lparam);
        if (key.dwExtraInfo == kMaskInfo) // VanaCore's own (mask_release): to Windows, not the game's hook
            return CallNextHookEx(nullptr, code, wparam, lparam);
        if (g_win_slot == N && win_bind(key, wparam == WM_KEYDOWN || wparam == WM_SYSKEYDOWN))
            return 1;
    }
    if (code >= 0 && g_settings.windows_key) {
        const DWORD key = hook.type == WH_KEYBOARD_LL ? reinterpret_cast<const KBDLLHOOKSTRUCT*>(lparam)->vkCode
                                                      : static_cast<DWORD>(wparam);
        if (key == VK_LWIN || key == VK_RWIN)
            return CallNextHookEx(nullptr, code, wparam, lparam);
    }
    return hook.proc(code, wparam, lparam);
}

HOOKPROC kKeyboardProcsFor(int slot);

// A low-level hook's thread: sets slot N's procedure, says so, and answers it until told to end.
struct HookStart {
    int slot;
    HANDLE ready;
    HHOOK handle;
};

DWORD WINAPI hook_thread(void* param)
{
    auto* start = static_cast<HookStart*>(param);
    MSG msg;
    PeekMessageW(&msg, nullptr, 0, 0, PM_NOREMOVE); // its message queue, before anyone posts to it
    const HHOOK handle = g_set_windows_hook(WH_KEYBOARD_LL, kKeyboardProcsFor(start->slot), g_self, 0);
    start->handle = handle;
    SetEvent(start->ready); // `start` is the caller's: not touched after this
    if (handle == nullptr)
        return 0;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
    }
    g_unhook_windows_hook(handle);
    return 0;
}
constexpr HOOKPROC kKeyboardProcs[kKeyboardSlots] = {&keyboard_proc<0>, &keyboard_proc<1>, &keyboard_proc<2>,
                                                     &keyboard_proc<3>};
HOOKPROC kKeyboardProcsFor(int slot)
{
    return kKeyboardProcs[slot];
}

HHOOK WINAPI set_windows_hook(int type, HOOKPROC proc, HINSTANCE module, DWORD thread)
{
    // Only hooks that run inside the game can be wrapped: low-level keyboard hooks (always: their own
    // thread), and keyboard hooks on one of its own threads for the Windows keys. (A global WH_KEYBOARD
    // hook is loaded into other programs.)
    const bool low_level = type == WH_KEYBOARD_LL;
    if (proc == nullptr || !(low_level || (g_settings.windows_key && type == WH_KEYBOARD && thread != 0)))
        return g_set_windows_hook(type, proc, module, thread);
    std::lock_guard lock(g_keyboard_mutex);
    for (int i = 0; i < kKeyboardSlots; ++i) {
        KeyboardHook& slot = g_keyboard_hooks[i];
        if (slot.proc != nullptr)
            continue;
        slot.proc = proc;
        slot.type = type;
        if (low_level && g_settings.hook_thread) {
            HookStart start{i, CreateEventW(nullptr, TRUE, FALSE, nullptr), nullptr};
            HANDLE worker = start.ready != nullptr ? CreateThread(nullptr, 0, &hook_thread, &start, 0, &slot.thread) : nullptr;
            if (worker != nullptr) {
                WaitForSingleObject(start.ready, INFINITE);
                CloseHandle(worker);
            }
            if (start.ready != nullptr)
                CloseHandle(start.ready);
            slot.handle = worker != nullptr ? start.handle : nullptr;
            if (slot.handle == nullptr) {
                // no thread for it: set on the game's thread, as it asked
                slot.thread = 0;
                slot.handle = g_set_windows_hook(type, kKeyboardProcs[i], g_self, thread);
            }
        } else {
            slot.handle = g_set_windows_hook(type, kKeyboardProcs[i], g_self, thread);
        }
        if (slot.handle == nullptr) {
            slot = KeyboardHook{};
            return nullptr;
        }
        if (low_level) {
            int none = -1;
            g_win_slot.compare_exchange_strong(none, i);
        }
        if (g_settings.windows_key)
            once(g_logged_windows_key, L"the game's keyboard hook lets the Windows keys through (keyboard.windowskeyenabled=1)");
        if (low_level && slot.thread != 0)
            once(g_logged_hook_thread, L"the game's low-level keyboard hook runs on a thread of its own, so the rest of "
                                       L"the PC doesn't wait on the game's frames");
        else if (low_level)
            once(g_logged_hook_thread, L"the game's low-level keyboard hook runs on the game's thread (keyboard.hookthread: "
                                       L"0, or Wine's default)");
        return slot.handle;
    }
    once(g_logged_no_slot, L"the game installed more keyboard hooks than VanaCore wraps; the Windows keys may stay blocked");
    return g_set_windows_hook(type, proc, module, thread);
}

BOOL WINAPI unhook_windows_hook(HHOOK handle)
{
    {
        std::lock_guard lock(g_keyboard_mutex);
        for (KeyboardHook& slot : g_keyboard_hooks) {
            if (slot.handle != handle || handle == nullptr || slot.thread == 0)
                continue;
            // a low-level hook's thread takes it off as it ends
            const BOOL posted = PostThreadMessageW(slot.thread, WM_QUIT, 0, 0);
            int answering = static_cast<int>(&slot - g_keyboard_hooks);
            g_win_slot.compare_exchange_strong(answering, -1);
            slot = KeyboardHook{};
            return posted;
        }
    }
    const BOOL result = g_unhook_windows_hook(handle);
    std::lock_guard lock(g_keyboard_mutex);
    for (KeyboardHook& slot : g_keyboard_hooks)
        if (slot.handle == handle && handle != nullptr) {
            int answering = static_cast<int>(&slot - g_keyboard_hooks);
            g_win_slot.compare_exchange_strong(answering, -1);
            slot = KeyboardHook{};
        }
    return result;
}

// ---- The front: the game never takes it back ---------------------------------------------------
// The game brings its window to the front with SetForegroundWindow. Once its window has been in
// front, a call while another program's window is there (the Start menu the Windows key opened, a
// window clicked or Alt+Tabbed to, the desktop) is refused: Windows would grant it, as the game had
// the last key press, and the Start menu or the other window would flicker and drop behind the game.

decltype(&SetForegroundWindow) g_set_foreground_window = nullptr;
std::atomic<bool> g_logged_foreground{false};

BOOL WINAPI set_foreground_window(HWND target)
{
    if (window::handle() != nullptr && window::was_in_front()) {
        DWORD owner = 0;
        if (const HWND front = GetForegroundWindow(); front != nullptr)
            GetWindowThreadProcessId(front, &owner);
        if (owner != GetCurrentProcessId()) {
            once(g_logged_foreground, L"kept the game from taking the front back from another window (the Start menu, "
                                      L"Alt+Tab, the desktop)");
            return FALSE;
        }
    }
    return g_set_foreground_window(target);
}

// ---- Mouse: cursor moves ---------------------------------------------------------------------

decltype(&SetCursorPos) g_set_cursor_pos = nullptr;
std::atomic<bool> g_logged_cursor{false};

BOOL WINAPI set_cursor_pos(int, int)
{
    once(g_logged_cursor, L"kept the game from moving the mouse cursor (mouse.unhook=1)");
    return TRUE;
}

// ---- The game's message loop ---------------------------------------------------------------------
// FFXiMain's loop acts on WM_CHAR and key messages as PeekMessageA hands them over (a Space opens the chat line, a
// character goes into it), before its window procedure ever sees them. VanaCore's window procedure blocking them while
// ImGui has the keyboard (window.cpp) never reached that: typing in an ImGui text box still opened the game's chat line.
// So the messages ImGui takes are made WM_NULL as they come out of the queue, which the window procedure then never sees.

decltype(&PeekMessageA) g_peek_message = nullptr;
std::atomic<bool> g_logged_peek{false};

BOOL WINAPI peek_message(LPMSG message, HWND window, UINT first, UINT last, UINT remove)
{
    const BOOL result = g_peek_message(message, window, first, last, remove);
    if (result == 0 || message == nullptr || (remove & PM_REMOVE) == 0 || message->hwnd == nullptr ||
        message->hwnd != window::handle())
        return result;
    const UINT what = message->message;
    const bool key = what == WM_KEYDOWN || what == WM_SYSKEYDOWN || what == WM_KEYUP || what == WM_SYSKEYUP;
    // the same messages window.cpp gives ImGui first (Print Screen stays VanaCore's)
    if (((key && message->wParam != VK_SNAPSHOT) || what == WM_CHAR) && gui::on_key(what, message->wParam, message->lParam)) {
        once(g_logged_peek, L"typing in an ImGui window doesn't reach the game's message loop either (PeekMessageA)");
        // A press makes its WM_CHAR when it goes through TranslateMessage, which the game's loop does after this: with the
        // press gone (WM_NULL) ImGui would get no text. So it's translated here, and the characters it posts come back
        // through this hook to ImGui in their turn.
        if (what == WM_KEYDOWN || what == WM_SYSKEYDOWN)
            TranslateMessage(message);
        message->message = WM_NULL;
        return result;
    }
    // Key binds, before the loop acts on the key: a / press opens the chat line with a / typed (FUN_101b1150) before
    // it's dispatched, so the window procedure's binds saw the line open and let it be. A key a bind takes is a WM_NULL
    // (no character from TranslateMessage either), and kept from the game's DirectInput keyboard until it's let go.
    if (key && binds::on_queued_key(what, message->wParam, message->lParam)) {
        if (what == WM_KEYDOWN || what == WM_SYSKEYDOWN)
            hide_key(static_cast<unsigned>((message->lParam >> 16 & 0x7F) | ((message->lParam & (1 << 24)) != 0 ? 0x80 : 0)));
        message->message = WM_NULL;
        return result;
    }
    // An arrow key moving the HUD Manager's picked boxes (ImGui has had it above): kept from the game's loop, and
    // from its keyboard until it's let go, as a bind's key
    const unsigned scan = static_cast<unsigned>((message->lParam >> 16 & 0x7F) | ((message->lParam & (1 << 24)) != 0 ? 0x80 : 0));
    if (key && hud_arrow(scan, modifiers_now())) {
        if (what == WM_KEYDOWN || what == WM_SYSKEYDOWN)
            hide_key(scan);
        message->message = WM_NULL;
    } else if (what == WM_KEYDOWN && camera::takes_letter(scan, modifiers_now())) {
        // /freecam's W A S D, Q E C: with the game's keyboard set to Full a letter's press opened the chat line and typed
        // itself (reported). The press never reaches the loop, so it makes no character either.
        message->message = WM_NULL;
    }
    return result;
}

// ---- Wiring ----------------------------------------------------------------------------------

bool is_game_main(HMODULE module)
{
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(module, path, MAX_PATH);
    const wchar_t* name = wcsrchr(path, L'\\');
    return _wcsicmp(name != nullptr ? name + 1 : path, L"FFXiMain.dll") == 0;
}

void on_module(HMODULE module)
{
    if (imports::is_windows_module(module) || module == g_self)
        return;
    // always: the mouse over VanaCore's windows (and the controller settings, when they're on)
    imports::hook(module, "DINPUT8.dll", "DirectInput8Create", &direct_input_create, g_direct_input_create);
    // always: the game's XInput reads are how plugins and addons get controller events (xinput_button, IXInput), and
    // where they read as let go in the background (controllers_quiet); this hook is where it finds XInputGetState_. It
    // was once installed only with some [input] settings on, and with them off no XInput pad raised an event: UberMap's
    // D-pad did nothing.
    if (is_game_main(module))
        imports::hook(module, "KERNEL32.dll", "GetProcAddress", &get_proc_address, g_get_proc_address);
    // always: what ImGui takes of the keyboard doesn't reach the game's own message loop
    if (is_game_main(module))
        imports::hook(module, "USER32.dll", "PeekMessageA", &peek_message, g_peek_message);
    // always: a low-level keyboard hook goes on a thread of its own (the Windows keys as set)
    imports::hook(module, "USER32.dll", "SetWindowsHookExA", &set_windows_hook, g_set_windows_hook);
    imports::hook(module, "USER32.dll", "UnhookWindowsHookEx", &unhook_windows_hook, g_unhook_windows_hook);
    if (g_settings.unhook_mouse)
        imports::hook(module, "USER32.dll", "SetCursorPos", &set_cursor_pos, g_set_cursor_pos);
    if (g_settings.windows_key)
        imports::hook(module, "USER32.dll", "SetForegroundWindow", &set_foreground_window, g_set_foreground_window);
}

bool setting(const wchar_t* ini, const wchar_t* key, bool fallback)
{
    return GetPrivateProfileIntW(L"input", key, fallback ? 1 : 0, ini) != 0;
}

bool present(const wchar_t* ini, const wchar_t* key)
{
    wchar_t value[8] = {};
    return GetPrivateProfileStringW(L"input", key, L"", value, 8, ini) > 0;
}

bool under_wine()
{
    return GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "wine_get_version") != nullptr;
}

// keyboard.hookthread: 1 the game's low-level keyboard hook on a thread of its own, 0 on the game's. Unset (or auto): its
// own, except under Wine, where it stays on the game's thread as in retail (a Linux / Proton letter came before its Shift
// with two low-level hooks on two threads: window.cpp's Print Screen hook, gone now; the thread alone didn't fix it).
bool hook_thread_setting(const wchar_t* ini)
{
    wchar_t value[8] = {};
    GetPrivateProfileStringW(L"input", L"keyboard.hookthread", L"", value, 8, ini);
    if (wcscmp(value, L"1") == 0)
        return true;
    if (wcscmp(value, L"0") == 0)
        return false;
    return !under_wine();
}

// ---- The controllers Windows lists, for /vana > Client > Gamepads ---------------------------------------

struct Listed {
    GUID instance;
    DWORD product = 0; // its product GUID's first part: MAKELONG(USB vendor id, product id)
    input::Controller info;
};

std::mutex g_list_lock;
std::vector<Listed> g_listed;
bool g_listed_xinput[4] = {};
bool g_listed_once = false;
std::atomic<bool> g_list_stale{true}; // a device came or went since the last search
IDirectInput8A* g_probe = nullptr;

std::string guid_text(const GUID& g)
{
    char text[48];
    std::snprintf(text, sizeof text, "{%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}", g.Data1, g.Data2, g.Data3,
                  g.Data4[0], g.Data4[1], g.Data4[2], g.Data4[3], g.Data4[4], g.Data4[5], g.Data4[6], g.Data4[7]);
    return text;
}

// A DirectInput of its own for searching: the game's can't be used from here, and the search through ours goes to
// DirectInput's own EnumDevices, not VanaCore's hook of it (the repeated-search throttle must not answer this one).
IDirectInput8A* list_probe()
{
    if (g_probe != nullptr)
        return g_probe;
    DirectInput8CreateFn create = g_direct_input_create;
    if (create == nullptr) {
        if (const HMODULE module = LoadLibraryW(L"dinput8.dll"); module != nullptr)
            create = reinterpret_cast<DirectInput8CreateFn>(GetProcAddress(module, "DirectInput8Create"));
    }
    void* object = nullptr;
    if (create == nullptr || FAILED(create(GetModuleHandleW(nullptr), 0x0800, kDirectInput8A, &object, nullptr)))
        return nullptr;
    g_probe = static_cast<IDirectInput8A*>(object);
    return g_probe;
}

BOOL CALLBACK list_collect(LPCDIDEVICEINSTANCEA device, LPVOID context)
{
    if (is_controller(device->dwDevType)) {
        Listed listed;
        listed.instance = device->guidInstance;
        listed.product = device->guidProduct.Data1;
        listed.info.guid = guid_text(device->guidInstance);
        listed.info.name = device->tszInstanceName;
        listed.info.product = device->tszProductName;
        listed.info.type = device->dwDevType;
        static_cast<std::vector<Listed>*>(context)->push_back(std::move(listed));
    }
    return DIENUM_CONTINUE;
}

void list_xinput(bool (&connected)[4])
{
    using GetStateFn = DWORD(WINAPI*)(DWORD, void*);
    static const GetStateFn get_state = [] {
        HMODULE module = LoadLibraryW(L"xinput1_4.dll");
        if (module == nullptr)
            module = LoadLibraryW(L"xinput9_1_0.dll");
        return module != nullptr ? reinterpret_cast<GetStateFn>(GetProcAddress(module, "XInputGetState")) : nullptr;
    }();
    for (DWORD slot = 0; slot < 4; ++slot) {
        unsigned char state[16] = {};
        connected[slot] = get_state != nullptr && get_state(slot, state) == ERROR_SUCCESS;
    }
}

// The listed controllers that are XInput ones: a HID device with "IG_" in its name (\\?\HID#VID_045E&PID_028E&IG_00#...)
// is, as XInput itself goes by. Microsoft's IsXInputDevice asks WMI the same, which the game does at ~0.3-0.8 s a
// controller; Windows' raw input list costs nothing. Matched by USB ids to the product GUID's first part.
void mark_xinput(std::vector<Listed>& listed)
{
    UINT count = 0;
    if (listed.empty() || GetRawInputDeviceList(nullptr, &count, sizeof(RAWINPUTDEVICELIST)) != 0 || count == 0)
        return;
    std::vector<RAWINPUTDEVICELIST> devices(count);
    count = GetRawInputDeviceList(devices.data(), &count, sizeof(RAWINPUTDEVICELIST));
    if (count == static_cast<UINT>(-1))
        return;
    for (UINT i = 0; i < count; ++i) {
        char name[512] = {};
        UINT size = sizeof name;
        if (devices[i].dwType != RIM_TYPEHID ||
            GetRawInputDeviceInfoA(devices[i].hDevice, RIDI_DEVICENAME, name, &size) == static_cast<UINT>(-1))
            continue;
        name[sizeof name - 1] = '\0';
        CharUpperBuffA(name, static_cast<DWORD>(std::strlen(name)));
        const char* vid = std::strstr(name, "VID_");
        const char* pid = std::strstr(name, "PID_");
        if (std::strstr(name, "IG_") == nullptr || vid == nullptr || pid == nullptr)
            continue;
        const DWORD product = MAKELONG(std::strtoul(vid + 4, nullptr, 16), std::strtoul(pid + 4, nullptr, 16));
        for (Listed& pad : listed)
            if (pad.product == product)
                pad.info.xinput = true;
    }
}

} // namespace

Gamepads gamepads(bool refresh)
{
    const std::lock_guard lock(g_list_lock);
    if (refresh || g_list_stale.exchange(false) || !g_listed_once) {
        g_listed.clear();
        if (IDirectInput8A* probe = list_probe()) {
            if (g_enum_devices != nullptr)
                g_enum_devices(probe, DI8DEVCLASS_GAMECTRL, &list_collect, &g_listed, DIEDFL_ATTACHEDONLY);
            else
                probe->EnumDevices(DI8DEVCLASS_GAMECTRL, &list_collect, &g_listed, DIEDFL_ATTACHEDONLY);
        }
        mark_xinput(g_listed);
        list_xinput(g_listed_xinput);
        g_listed_once = true;
    }
    Gamepads out;
    std::copy(std::begin(g_listed_xinput), std::end(g_listed_xinput), std::begin(out.xinput));
    const ULONGLONG now = GetTickCount64();
    for (int slot = 0; slot < 4; ++slot) {
        const ULONGLONG at = g_xinput_read_at[slot].load(std::memory_order_relaxed);
        out.xinput_reading[slot] = at != 0 && now - at < kReadingMs;
    }
    const std::lock_guard opened_lock(g_opened_guids_lock);
    const std::lock_guard controllers_lock(g_controllers_lock);
    for (const Listed& listed : g_listed) {
        Controller info = listed.info;
        info.opened = std::any_of(g_opened_guids.begin(), g_opened_guids.end(),
                                  [&](const GUID& guid) { return IsEqualGUID(guid, listed.instance) != 0; });
        info.reading = std::any_of(g_controllers.begin(), g_controllers.end(), [&](const auto& entry) {
            return IsEqualGUID(entry.second.guid, listed.instance) != 0 && entry.second.read_at != 0 &&
                   now - entry.second.read_at < kReadingMs;
        });
        out.controllers.push_back(std::move(info));
    }
    return out;
}

// The game's own way to change the pad it uses, FFXiMain's FUN_100055a0 (found 2026-10-11 in the 2025-12-26 client; its
// settings loader FUN_10002840 calls it with padguid000, and retail's in-game gamepad menu goes the same way): it stores
// the GUID (four dwords, the GUID's own bytes) in the pad globals and, unless its last argument is non-zero, raises the
// "pad changed" flag, which makes the pad's per-frame update release the device and make a new one from that GUID on its
// next frame. __stdcall, RET 0x14. A zero GUID is the game's Auto-detect. The function's bytes are checked first: a
// different client build isn't touched.
namespace {

// PUSH ESI; PUSH EDI; MOV ECX,4; LEA EDI,[ESP+0xC]; MOV ESI,<globals> ... RET 0x14 (no C++ objects: __try)
bool pad_code_known(const BYTE* code)
{
    static const BYTE kHead[] = {0x56, 0x57, 0xB9, 0x04, 0x00, 0x00, 0x00, 0x8D, 0x7C, 0x24, 0x0C, 0xBE};
    __try {
        return std::memcmp(code, kHead, sizeof kHead) == 0 && code[0x4E] == 0xC2 && code[0x4F] == 0x14 && code[0x50] == 0x00;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool call_set_pad_guid(const BYTE* code, const DWORD (&parts)[4])
{
    using SetPadGuid = void(__stdcall*)(DWORD, DWORD, DWORD, DWORD, char);
    __try {
        reinterpret_cast<SetPadGuid>(const_cast<BYTE*>(code))(parts[0], parts[1], parts[2], parts[3], 0);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// The game's other pad setters (padmode000, padsin000, padexsin000). Checked strictly against the
// audited 2025-12-26 PE header before ever probing fixed RVAs, plus full prologue & RET validation,
// so newer retail FFXiMain.dll builds (such as 0x6AE818B4) fail closed cleanly rather than invoking
// unverified fixed RVAs.
struct PadSetter {
    DWORD rva;
    BYTE head[4];
    DWORD ret_at;
    BYTE ret;   // the RET imm16's value (bytes: C2 xx 00)
};
constexpr PadSetter kModeSetter = {0x5490, {0x83, 0xEC, 0x20, 0xA0}, 0x106, 0x0C};
constexpr PadSetter kButtonsSetter = {0x5130, {0x8B, 0x54, 0x24, 0x04}, 0x1ED, 0x08};
constexpr PadSetter kButtonsExSetter = {0x5320, {0x8B, 0x54, 0x24, 0x04}, 0x162, 0x08};

bool is_fixed_rva_pad_build(HMODULE game)
{
    if (game == nullptr)
        return false;
    __try {
        const auto* base = reinterpret_cast<const BYTE*>(game);
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
        return nt->FileHeader.TimeDateStamp == 0x694E5965u && nt->OptionalHeader.SizeOfImage == 0x00BDF000u;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool pad_setter_known(HMODULE game, const BYTE* code, const PadSetter& setter)
{
    if (!is_fixed_rva_pad_build(game))
        return false;
    __try {
        return std::memcmp(code, setter.head, sizeof setter.head) == 0 && code[setter.ret_at] == 0xC2 &&
               code[setter.ret_at + 1] == setter.ret && code[setter.ret_at + 2] == 0x00;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool call_pad_text_setter(const BYTE* code, char* text, DWORD length)
{
    using Setter = void(__stdcall*)(char*, DWORD);
    __try {
        reinterpret_cast<Setter>(const_cast<BYTE*>(code))(text, length);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool call_pad_mode_setter(const BYTE* code, char* text, DWORD length)
{
    using Setter = void(__stdcall*)(char*, DWORD, char);
    __try {
        reinterpret_cast<Setter>(const_cast<BYTE*>(code))(text, length, 0);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

} // namespace

bool select_gamepad(const std::string& text, std::string& why)
{
    // {8-4-4-4-12 hex digits}
    std::string digits;
    for (const char c : text)
        if (c != '{' && c != '}' && c != '-' && c != ' ')
            digits += c;
    const auto hex = [&](size_t at, size_t count, unsigned long long& out) {
        out = 0;
        for (size_t i = at; i < at + count; ++i) {
            const char c = digits[i];
            const int v = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
            if (v < 0)
                return false;
            out = out * 16 + static_cast<unsigned>(v);
        }
        return true;
    };
    unsigned long long d1 = 0, d2 = 0, d3 = 0, tail[8] = {};
    bool parsed = digits.size() == 32 && hex(0, 8, d1) && hex(8, 4, d2) && hex(12, 4, d3);
    for (size_t i = 0; parsed && i < 8; ++i)
        parsed = hex(16 + i * 2, 2, tail[i]);
    if (!parsed) {
        why = "not a GUID";
        return false;
    }
    GUID guid = {static_cast<unsigned long>(d1), static_cast<WORD>(d2), static_cast<WORD>(d3),
                 {static_cast<BYTE>(tail[0]), static_cast<BYTE>(tail[1]), static_cast<BYTE>(tail[2]),
                  static_cast<BYTE>(tail[3]), static_cast<BYTE>(tail[4]), static_cast<BYTE>(tail[5]),
                  static_cast<BYTE>(tail[6]), static_cast<BYTE>(tail[7])}};

    const HMODULE game = GetModuleHandleW(L"FFXiMain.dll");
    if (game == nullptr) {
        why = "the game's module isn't loaded";
        return false;
    }
    const std::uintptr_t site = scan::find(game, "56 57 B9 04 00 00 00 8D 7C 24 0C BE");
    const BYTE* code = reinterpret_cast<const BYTE*>(site);
    if (site == 0 || !pad_code_known(code)) {
        why = "this client's pad code isn't the one it was written for";
        return false;
    }
    DWORD parts[4];
    std::memcpy(parts, &guid, sizeof parts);
    if (!call_set_pad_guid(code, parts)) {
        why = "the game's pad code faulted";
        return false;
    }
    g_list_stale.store(true, std::memory_order_relaxed);
    log::write(L"Input: the gamepad is changed to {%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X} (the game opens it on "
               L"its next frame)",
               guid.Data1, guid.Data2, guid.Data3, guid.Data4[0], guid.Data4[1], guid.Data4[2], guid.Data4[3],
               guid.Data4[4], guid.Data4[5], guid.Data4[6], guid.Data4[7]);
    return true;
}

bool apply_gamepad(const GamepadSettings& settings, std::string& why)
{
    const HMODULE game = GetModuleHandleW(L"FFXiMain.dll");
    if (game == nullptr) {
        why = "the game's module isn't loaded";
        return false;
    }
    const BYTE* base = reinterpret_cast<const BYTE*>(game);
    const BYTE* mode_code = base + kModeSetter.rva;
    const BYTE* buttons_code = base + kButtonsSetter.rva;
    const BYTE* buttons_ex_code = base + kButtonsExSetter.rva;
    // all checked before any is called: the client either is the one these were found in, or nothing changes
    if (!pad_setter_known(game, mode_code, kModeSetter) || !pad_setter_known(game, buttons_code, kButtonsSetter) ||
        !pad_setter_known(game, buttons_ex_code, kButtonsExSetter)) {
        why = "this client's pad code isn't the one it was written for";
        return false;
    }
    const auto run = [&](const std::string& text, const BYTE* code, bool mode) {
        if (text.empty())
            return true;
        std::vector<char> buffer(text.begin(), text.end());
        buffer.push_back('\0');
        const DWORD length = static_cast<DWORD>(text.size());
        return mode ? call_pad_mode_setter(code, buffer.data(), length) : call_pad_text_setter(code, buffer.data(), length);
    };
    const InactiveSwitch& inactive = inactive_switch();
    const int inactive_before = inactive.now != nullptr && *inactive.aside == 0 ? *inactive.now : -1;
    if (!run(settings.mode, mode_code, true) || !run(settings.buttons, buttons_code, false) ||
        !run(settings.buttons_ex, buttons_ex_code, false)) {
        why = "the game's pad code faulted";
        return false;
    }
    if (!settings.mode.empty())
        settle_inactive_switch(inactive_before);
    log::write(L"Input: the gamepad's settings are applied live (mode %ls, buttons %ls, second map %ls)",
               settings.mode.empty() ? L"unchanged" : L"set", settings.buttons.empty() ? L"unchanged" : L"set",
               settings.buttons_ex.empty() ? L"unchanged" : L"set");
    gamepad::game_settings(settings.mode, settings.buttons); // where its Change View is now ([camera] view.reset)
    if (!settings.guid.empty())
        return select_gamepad(settings.guid, why); // last: it raises the pad-changed flag with everything above in place
    return true;
}

namespace {

// The mode setter's first read is MOV AL,[the gamepad switch] (A0, then the address, relocated with the module): the
// switches' bytes, the gamepad's first and XInput's 4 on (FUN_10005490's 0x10453AC4 and 0x10453AC8)
bool read_pad_switches(HMODULE game, const BYTE* code, bool& on, bool& xinput)
{
    __try {
        const auto base = reinterpret_cast<uintptr_t>(game);
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(game);
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
        DWORD at = 0;
        std::memcpy(&at, code + 4, sizeof at);
        if (at < base || at + 5 > base + nt->OptionalHeader.SizeOfImage)
            return false;
        on = *reinterpret_cast<const volatile BYTE*>(at) != 0;
        xinput = *reinterpret_cast<const volatile BYTE*>(at + 4) != 0;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

} // namespace

GameGamepad game_gamepad()
{
    GameGamepad out;
    const HMODULE game = GetModuleHandleW(L"FFXiMain.dll");
    if (game == nullptr)
        return out;
    const BYTE* code = reinterpret_cast<const BYTE*>(game) + kModeSetter.rva;
    if (pad_setter_known(game, code, kModeSetter))
        out.known = read_pad_switches(game, code, out.on, out.xinput);
    return out;
}

void device_changed()
{
    g_list_stale.store(true, std::memory_order_relaxed);
    g_device_epoch.fetch_add(1, std::memory_order_relaxed);
    for (auto& until : g_xinput_absent_until)
        until.store(0, std::memory_order_relaxed);
}

void disable_gamepads(bool off)
{
    if (g_pads_off.exchange(off) == off)
        return;
    device_changed(); // the game's next controller search runs for real, not from a kept result
    log::write(off ? L"Input: gamepads are disabled (gamepad.disable): the game's controller search finds nothing, and "
                     L"its controllers read as let go"
                   : L"Input: gamepads are enabled again");
}

bool gamepads_disabled()
{
    return pads_off();
}

void install(const wchar_t* ini)
{
    g_settings.disable_enumeration = setting(ini, L"gamepad.disableenumeration", false);
    g_pads_off.store(setting(ini, L"gamepad.disable", false));
    if (g_pads_off.load())
        log::write(L"Input: gamepad.disable=1: no controller works");
    g_settings.windows_key = setting(ini, L"keyboard.windowskeyenabled", true);
    g_settings.hook_thread = hook_thread_setting(ini);
    g_settings.unhook_mouse = setting(ini, L"mouse.unhook", true);
    log::write(L"Input: gamepad.disableenumeration=%d keyboard.windowskeyenabled=%d keyboard.hookthread=%d%ls mouse.unhook=%d",
               g_settings.disable_enumeration, g_settings.windows_key, g_settings.hook_thread,
               under_wine() ? L" (under Wine)" : L"", g_settings.unhook_mouse);
    // Ashita's key, which the launcher writes from the same box of FFXI's gamepad config tool as gamepad.mode's fifth
    // switch. VanaCore took it to mean "always", over the switch: turned off in /vana, the pad still worked in the
    // background. The switch alone says now.
    if (present(ini, L"gamepad.allowbackground"))
        log::write(L"Input: gamepad.allowbackground is not used; the gamepad's \"works while inactive\" switch (gamepad.mode, "
                   L"/vana > Client > Gamepads) says whether it works in the background");
    const UINT interval = GetPrivateProfileIntW(L"input", L"gamepad.searchinterval", kDefaultRetrySeconds, ini);
    g_retry_ms.store(static_cast<ULONGLONG>(std::min<UINT>(interval, 3600)) * 1000);
    log::write(L"Input: gamepad.searchinterval=%u", std::min<UINT>(interval, 3600));
    for (const wchar_t* key : {L"keyboard.blockinput", L"mouse.blockinput"})
        if (present(ini, key))
            log::write(L"Input: %ls isn't supported yet; ignored", key);
    // the pad settings the game starts with (vanacore.ini's, else a normal install's): where its Change View is, for [camera]
    // view.reset; apply_gamepad gives the changes
    const auto ascii = [](const std::wstring& text) {
        std::string out;
        for (const wchar_t c : text)
            out += static_cast<char>(c);
        return out;
    };
    gamepad::game_settings(ascii(registry::setting("padmode000")), ascii(registry::setting("padsin000")));

    // DirectInput is always watched: the mouse over VanaCore's windows mustn't reach the game
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&install), &g_self);
    if (!imports::for_each_module(&on_module))
        log::write(L"Input: could not watch modules load; [input] is ignored");
}

void frame_press(bool down)
{
    if (!down && g_frame_press)
        g_drop_buffered = true;
    g_frame_press = down;
}

void modal(bool open)
{
    if (open == g_modal)
        return;
    if (!open) {
        // what was buffered meanwhile goes; the buttons and keys still held (the press that closed it) stay hidden till let go
        g_drop_buffered = true;
        g_drop_keys = true;
        g_taken_press = true;
        for (int vk = 1; vk < 256; ++vk) {
            if (vk == VK_LBUTTON || vk == VK_RBUTTON || vk == VK_MBUTTON || vk == VK_XBUTTON1 || vk == VK_XBUTTON2 ||
                (GetAsyncKeyState(vk) & 0x8000) == 0)
                continue;
            const UINT scan = MapVirtualKeyW(static_cast<UINT>(vk), MAPVK_VK_TO_VSC_EX); // 0xE0xx: an extended key
            if ((scan & 0xFF) != 0)
                hide_key((scan & 0xFF) | ((scan & 0xFF00) != 0 ? 0x80 : 0));
        }
        g_modal_quiet_until = GetTickCount64() + 500;
        log::write(L"Input: a modal window over the game closed: what the mouse and keyboard did in it doesn't reach the game");
    }
    g_modal = open;
}

bool modal_quiet()
{
    if (g_modal)
        return true;
    const ULONGLONG until = g_modal_quiet_until;
    if (until == 0)
        return false;
    if (GetTickCount64() < until || buttons_down())
        return true;
    g_modal_quiet_until = 0;
    return false;
}

void hide_key(unsigned scan_code)
{
    if (scan_code != 0 && scan_code < 256)
        g_hidden_keys[scan_code] = true;
}

void mouse_taken(bool press, bool wheel)
{
    if (press)
        g_taken_press = true;
    if (wheel)
        g_wheel_until = GetTickCount64() + 150; // the game's next reads (a few frames)
}

} // namespace vanacore::input
