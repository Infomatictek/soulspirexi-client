#include "gamepad.hpp"
#include "gui.hpp"

#define DIRECTINPUT_VERSION 0x0800
#include <windows.h>

#include <Xinput.h>
#include <dinput.h>

#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "addons.hpp"
#include "camera.hpp"
#include "log.hpp"
#include "menulog.hpp"
#include "plugins.hpp"

namespace vanacore::gamepad {
namespace {

std::mutex g_lock; // the queues (QueueButtonData can come from any thread)
std::deque<std::pair<std::uint8_t, std::int16_t>> g_xinput_queue;
std::deque<std::pair<DWORD, DWORD>> g_dinput_queue;
bool g_track_xinput = false;
bool g_track_dinput = false;

bool listening()
{
    return plugins::wants_gamepad() || addons::wants_gamepad();
}

// A change through plugins, then addons: true if blocked; `button` and `value` as they leave it
bool raise(bool xinput, std::uint32_t& button, std::int32_t& value, bool injected)
{
    const std::uint32_t was_button = button;
    const std::int32_t was_value = value;
    bool blocked = xinput ? plugins::on_xinput_button(button, value, injected) : plugins::on_controller_button(button, value, injected);
    const bool by_plugin = blocked;
    blocked = addons::on_gamepad_button(xinput, button, value, blocked, injected);
    if (menulog::on()) { // a recording runs: the button and who kept it from the game
        const char* event = xinput ? "'s xinput_button" : "'s dinput_button";
        const std::string by = by_plugin                       ? std::string("a plugin")
                               : !blocked                      ? std::string()
                               : addons::blocked_by().empty() ? std::string("an addon") + event
                                                               : addons::blocked_by() + event;
        menulog::pad(xinput, was_button, was_value, injected, by.empty() ? nullptr : by.c_str());
    }
    return blocked;
}

// ---- XInput ----------------------------------------------------------------------------------------------------------

constexpr std::uint32_t kXControls = 22;

std::int32_t x_get(const XINPUT_GAMEPAD& pad, std::uint32_t control)
{
    if (control < 16)
        return (pad.wButtons >> control) & 1;
    switch (control) {
    case 16: return pad.bLeftTrigger;
    case 17: return pad.bRightTrigger;
    case 18: return pad.sThumbLX;
    case 19: return pad.sThumbLY;
    case 20: return pad.sThumbRX;
    default: return pad.sThumbRY;
    }
}

void x_set(XINPUT_GAMEPAD& pad, std::uint32_t control, std::int32_t value)
{
    if (control < 16) {
        pad.wButtons = static_cast<WORD>(value != 0 ? pad.wButtons | (1u << control) : pad.wButtons & ~(1u << control));
        return;
    }
    const auto clamp = [](std::int32_t v, std::int32_t lo, std::int32_t hi) { return v < lo ? lo : v > hi ? hi : v; };
    switch (control) {
    case 16: pad.bLeftTrigger = static_cast<BYTE>(clamp(value, 0, 255)); break;
    case 17: pad.bRightTrigger = static_cast<BYTE>(clamp(value, 0, 255)); break;
    case 18: pad.sThumbLX = static_cast<SHORT>(clamp(value, -32768, 32767)); break;
    case 19: pad.sThumbLY = static_cast<SHORT>(clamp(value, -32768, 32767)); break;
    case 20: pad.sThumbRX = static_cast<SHORT>(clamp(value, -32768, 32767)); break;
    case 21: pad.sThumbRY = static_cast<SHORT>(clamp(value, -32768, 32767)); break;
    default: break;
    }
}

// Inside its dead zone. The sticks only, as Ashita has it: a trigger raises each change, or one let go through the
// threshold leaves handlers holding it (tCrossBar's L2/R2 are down while the value is above 0)
bool x_quiet(std::uint32_t control, std::int32_t value)
{
    if (control == 18 || control == 19)
        return std::abs(value) <= XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE;
    if (control == 20 || control == 21)
        return std::abs(value) <= XINPUT_GAMEPAD_RIGHT_THUMB_DEADZONE;
    return false;
}

struct XPad {
    bool seen = false;
    XINPUT_GAMEPAD read{}; // as the controller had it last time
    XINPUT_GAMEPAD game{}; // as the game was given it
};
XPad g_xpads[XUSER_MAX_COUNT];

// ---- DirectInput -----------------------------------------------------------------------------------------------------
// DIJOYSTATE: 8 axes (LONG), 4 hats (DWORD), then the buttons (BYTE): 32 of them, 128 in DIJOYSTATE2 (whose velocities
// and the rest after them go to the game as they are)

constexpr DWORD kHats = 32, kButtons = 48;

struct DPad {
    std::vector<std::uint8_t> read, game;
};
std::map<void*, DPad> g_dpads;
void* g_last_dpad = nullptr; // QueueButtonData's: the controller last read

std::int32_t d_get(const std::vector<std::uint8_t>& state, DWORD offset)
{
    if (offset >= kButtons)
        return state[offset];
    std::int32_t value;
    std::memcpy(&value, state.data() + offset, 4);
    return value;
}

void d_set(std::vector<std::uint8_t>& state, DWORD offset, std::int32_t value)
{
    if (offset >= state.size())
        return;
    if (offset >= kButtons)
        state[offset] = static_cast<std::uint8_t>(value != 0 ? 0x80 : 0);
    else if (offset + 4 <= state.size())
        std::memcpy(state.data() + offset, &value, 4);
}

// The next control's offset after `offset` (axes and hats 4 bytes apart, buttons 1)
DWORD d_next(DWORD offset)
{
    return offset < kButtons ? offset + 4 : offset + 1;
}

// ---- [camera] view.reset: the game's Change View --------------------------------------------------------------------
// The third of the game's button map (padsin000, [ffxi] gamepad.buttons) is its Change View, the first-person view: a button
// by the game's numbers. With XInput on (padmode000's sixth, [ffxi] gamepad.mode) they're the launcher's (cexi-launcher-v2,
// GamePad.ButtonRegistryID): 0 B, 1 X, 2 Y, 3 A, 4-7 the D-pad right, left, up, down, 8 LB, 9 LT, 10 the left stick pressed,
// 11 RB, 12 RT, 13 the right stick pressed (a normal install's), 14 Start, 15 Back; without it, a DirectInput button's.
// Hidden from the game after the handlers, as camera.cpp says (camera::view_button), from each XInput controller (0-3) and
// DirectInput's (4).
constexpr int kViewSlot = 2;
constexpr int kNormalView = 13;
constexpr std::uint32_t kXView[16] = {13, 14, 15, 12, 3, 2, 0, 1, 8, 16, 6, 9, 17, 7, 4, 5}; // the game's number -> XInput's
constexpr int kDSource = XUSER_MAX_COUNT;
std::string g_game_mode, g_game_buttons; // under g_lock: as the game was last given them ("" a normal install's)
std::atomic<int> g_view_x{static_cast<int>(kXView[kNormalView])}; // Change View's XInput control; -1 not an XInput one
std::atomic<int> g_view_d{-1};                                   // its DIJOYSTATE offset (DIJOFS_BUTTON(n)); -1 none

// The value at `index` (from 0) of a comma separated list, or `fallback`
int field(const std::string& text, int index, int fallback)
{
    std::size_t at = 0;
    for (int i = 0; i < index; ++i) {
        at = text.find(',', at);
        if (at == std::string::npos)
            return fallback;
        ++at;
    }
    char* end = nullptr;
    const long value = std::strtol(text.c_str() + at, &end, 10);
    return end != text.c_str() + at ? static_cast<int>(value) : fallback;
}

// Change View in what the game reads from XInput controller `user`
void view_x(DWORD user, XINPUT_GAMEPAD& pad)
{
    const int control = g_view_x.load(std::memory_order_relaxed);
    if (control < 0)
        return;
    const std::int32_t value = x_get(pad, static_cast<std::uint32_t>(control));
    const bool down = control < 16 ? value != 0 : value > XINPUT_GAMEPAD_TRIGGER_THRESHOLD;
    if (camera::view_button(static_cast<int>(user), down))
        x_set(pad, static_cast<std::uint32_t>(control), 0);
}

// In a DirectInput controller's state (`end`: past its buttons)
void view_d(std::uint8_t* bytes, DWORD end)
{
    const int offset = g_view_d.load(std::memory_order_relaxed);
    if (offset < 0 || static_cast<DWORD>(offset) >= end)
        return;
    if (camera::view_button(kDSource, (bytes[offset] & 0x80) != 0))
        bytes[offset] = 0;
}

// In its buffered reads: the button's events dropped while it's hidden
void view_d_data(std::uint8_t* bytes, DWORD object_size, DWORD& count)
{
    const int offset = g_view_d.load(std::memory_order_relaxed);
    if (offset < 0)
        return;
    DWORD kept = 0;
    for (DWORD i = 0; i < count; ++i) {
        const auto* entry = reinterpret_cast<const DWORD*>(bytes + i * object_size); // dwOfs, dwData
        if (entry[0] == static_cast<DWORD>(offset) && camera::view_button(kDSource, (entry[1] & 0x80) != 0))
            continue;
        if (kept != i)
            std::memmove(bytes + kept * object_size, bytes + i * object_size, object_size);
        ++kept;
    }
    count = kept;
}

// ---- ImGui -----------------------------------------------------------------------------------------------------------
// ImGui 1.92's gamepad keys
constexpr int kKeyStart = 632, kKeyBack = 633, kKeyFaceLeft = 634, kKeyFaceRight = 635, kKeyFaceUp = 636, kKeyFaceDown = 637,
              kKeyDpadLeft = 638, kKeyDpadRight = 639, kKeyDpadUp = 640, kKeyDpadDown = 641, kKeyL1 = 642, kKeyR1 = 643,
              kKeyL2 = 644, kKeyR2 = 645, kKeyL3 = 646, kKeyR3 = 647, kKeyLStickLeft = 648, kKeyLStickRight = 649,
              kKeyLStickUp = 650, kKeyLStickDown = 651, kKeyRStickLeft = 652, kKeyRStickRight = 653, kKeyRStickUp = 654,
              kKeyRStickDown = 655;
constexpr int kKeyCount = 24;
bool g_key_down[kKeyCount];
float g_key_analog[kKeyCount];
std::atomic<ULONGLONG> g_last_read{0};  // GetTickCount64() of the last read
std::atomic<ULONGLONG> g_last_input{0}; // ... of the last read with something pressed or moved
constexpr int kSlotConfirm = 7, kSlotCancel = 5, kSlotMenu = 6, kSlotOptions = 8;

// one key to ImGui, when it changes (ImGui drops duplicates too)
void imgui_key(int key, bool down, float analog = -1.0f)
{
    const int i = key - kKeyStart;
    if (i < 0 || i >= kKeyCount)
        return;
    const float value = analog < 0.0f ? (down ? 1.0f : 0.0f) : analog;
    if (g_key_down[i] == down && std::abs(g_key_analog[i] - value) < 0.001f)
        return;
    g_key_down[i] = down;
    g_key_analog[i] = value;
    gui::gamepad_key(key, down, analog);
}

// a stick's axis as ImGui's two keys, past XInput's dead zone
void stick(int key_negative, int key_positive, SHORT raw, SHORT dead_zone)
{
    float f = raw / 32767.0f;
    if (std::abs(raw) <= dead_zone)
        f = 0.0f;
    f = f < -1.0f ? -1.0f : f > 1.0f ? 1.0f : f;
    imgui_key(key_negative, f < 0.0f, f < 0.0f ? -f : 0.0f);
    imgui_key(key_positive, f > 0.0f, f > 0.0f ? f : 0.0f);
}

Bindings bindings_locked()
{
    Bindings b;
    b.xinput = g_game_mode.empty() || field(g_game_mode, 5, 0) == 1;
    const auto slot = [&](int index) {
        if (g_game_buttons.empty())
            return -1;
        const int v = field(g_game_buttons, index, -1);
        if (v < 0)
            return -1;
        if (b.xinput)
            return v < 16 ? static_cast<int>(kXView[v]) : -1;
        return v;
    };
    b.confirm = slot(kSlotConfirm);
    b.cancel = slot(kSlotCancel);
    b.menu = slot(kSlotMenu);
    b.options = slot(kSlotOptions);
    return b;
}

// XInput controller 0's read as ImGui's keys: the face buttons by the game's map (A, B, X, Y as a normal install's)
void feed_x(const XINPUT_GAMEPAD& pad)
{
    Bindings b;
    {
        const std::lock_guard lock(g_lock);
        b = bindings_locked();
    }
    const auto down = [&](int control) {
        if (control < 0)
            return false;
        const std::int32_t v = x_get(pad, static_cast<std::uint32_t>(control));
        return control < 16 ? v != 0 : v > XINPUT_GAMEPAD_TRIGGER_THRESHOLD;
    };
    const int confirm = b.xinput && b.confirm >= 0 ? b.confirm : 12;
    const int cancel = b.xinput && b.cancel >= 0 ? b.cancel : 13;
    imgui_key(kKeyFaceDown, down(confirm));
    imgui_key(kKeyFaceRight, down(cancel));
    // X is not fed: held, it is ImGui's windowing mode (the screen dimmed, windows cycled); addons read it themselves
    imgui_key(kKeyFaceUp, confirm != 15 && cancel != 15 && down(15));
    imgui_key(kKeyDpadUp, down(0));
    imgui_key(kKeyDpadDown, down(1));
    imgui_key(kKeyDpadLeft, down(2));
    imgui_key(kKeyDpadRight, down(3));
    imgui_key(kKeyStart, down(4));
    imgui_key(kKeyBack, down(5));
    imgui_key(kKeyL3, down(6));
    imgui_key(kKeyR3, down(7));
    imgui_key(kKeyL1, down(8));
    imgui_key(kKeyR1, down(9));
    imgui_key(kKeyL2, pad.bLeftTrigger > XINPUT_GAMEPAD_TRIGGER_THRESHOLD, pad.bLeftTrigger / 255.0f);
    imgui_key(kKeyR2, pad.bRightTrigger > XINPUT_GAMEPAD_TRIGGER_THRESHOLD, pad.bRightTrigger / 255.0f);
    stick(kKeyLStickLeft, kKeyLStickRight, pad.sThumbLX, XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE);
    stick(kKeyLStickDown, kKeyLStickUp, pad.sThumbLY, XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE);
    stick(kKeyRStickLeft, kKeyRStickRight, pad.sThumbRX, XINPUT_GAMEPAD_RIGHT_THUMB_DEADZONE);
    stick(kKeyRStickDown, kKeyRStickUp, pad.sThumbRY, XINPUT_GAMEPAD_RIGHT_THUMB_DEADZONE);
    const ULONGLONG now = GetTickCount64();
    g_last_read = now;
    if (pad.wButtons != 0 || pad.bLeftTrigger > XINPUT_GAMEPAD_TRIGGER_THRESHOLD || pad.bRightTrigger > XINPUT_GAMEPAD_TRIGGER_THRESHOLD ||
        std::abs(pad.sThumbLX) > XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE || std::abs(pad.sThumbLY) > XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE ||
        std::abs(pad.sThumbRX) > XINPUT_GAMEPAD_RIGHT_THUMB_DEADZONE || std::abs(pad.sThumbRY) > XINPUT_GAMEPAD_RIGHT_THUMB_DEADZONE)
        g_last_input = now;
}

// A DirectInput controller's read as ImGui's keys: its buttons by the game's map, its first hat the D-pad
void feed_d(const std::uint8_t* bytes, DWORD end)
{
    Bindings b;
    {
        const std::lock_guard lock(g_lock);
        b = bindings_locked();
    }
    const auto down = [&](int button) {
        if (button < 0 || static_cast<DWORD>(kButtons + button) >= end)
            return false;
        return (bytes[kButtons + button] & 0x80) != 0;
    };
    // a normal pad's: 0 A, 1 B, 2 X, 3 Y, 4 LB, 5 RB, 6 Back, 7 Start, 8 and 9 the sticks pressed
    const int confirm = !b.xinput && b.confirm >= 0 ? b.confirm : 0;
    const int cancel = !b.xinput && b.cancel >= 0 ? b.cancel : 1;
    imgui_key(kKeyFaceDown, down(confirm));
    imgui_key(kKeyFaceRight, down(cancel));
    // the third face button is not fed (ImGui's windowing mode); addons read it themselves
    imgui_key(kKeyFaceUp, confirm != 3 && cancel != 3 && down(3));
    imgui_key(kKeyL1, down(4));
    imgui_key(kKeyR1, down(5));
    imgui_key(kKeyBack, down(6));
    imgui_key(kKeyStart, down(7));
    imgui_key(kKeyL3, down(8));
    imgui_key(kKeyR3, down(9));
    DWORD hat = 0xFFFFFFFF;
    std::memcpy(&hat, bytes + kHats, 4);
    const bool centred = hat == 0xFFFFFFFF || hat == 0xFFFF || hat > 36000;
    const int a = centred ? -1 : static_cast<int>(hat / 100);
    imgui_key(kKeyDpadUp, a >= 0 && (a < 68 || a > 292));
    imgui_key(kKeyDpadRight, a > 22 && a < 158);
    imgui_key(kKeyDpadDown, a > 112 && a < 248);
    imgui_key(kKeyDpadLeft, a > 202 && a < 338);
    const ULONGLONG now = GetTickCount64();
    g_last_read = now;
    bool any = !centred;
    for (DWORD i = kButtons; i < end && !any; ++i)
        any = (bytes[i] & 0x80) != 0;
    if (any)
        g_last_input = now;
}

// the game reads the controller at rest while an addon has it
void rest_d(std::uint8_t* bytes, DWORD end)
{
    for (DWORD offset = kHats; offset < kButtons; offset += 4) {
        const DWORD centred = 0xFFFFFFFF;
        std::memcpy(bytes + offset, &centred, 4);
    }
    std::memset(bytes + kButtons, 0, end - kButtons);
}

} // namespace

Bindings bindings()
{
    const std::lock_guard lock(g_lock);
    return bindings_locked();
}

double idle_seconds()
{
    const ULONGLONG at = g_last_input.load();
    if (at == 0)
        return 86400.0;
    return (GetTickCount64() - at) / 1000.0;
}

bool present()
{
    const ULONGLONG at = g_last_read.load();
    return at != 0 && GetTickCount64() - at < 2000;
}

void on_xinput_state(DWORD user, void* state_data)
{
    if (user >= XUSER_MAX_COUNT || state_data == nullptr)
        return;
    auto* state = static_cast<XINPUT_STATE*>(state_data);
    plugins::on_xinput_state(user, state);
    addons::on_xinput_state(user, state);
    XPad& pad = g_xpads[user];
    std::deque<std::pair<std::uint8_t, std::int16_t>> queued;
    if (user == 0) {
        const std::lock_guard lock(g_lock);
        queued.swap(g_xinput_queue);
    }
    if (!pad.seen || (queued.empty() && !listening())) {
        pad.seen = true;
        pad.read = pad.game = state->Gamepad;
        view_x(user, state->Gamepad);
        if (user == 0) {
            feed_x(state->Gamepad);
            if (gui::wants_gamepad())
                state->Gamepad = XINPUT_GAMEPAD{};
        }
        return;
    }
    for (const auto& [button, value] : queued) {
        std::uint32_t control = button;
        std::int32_t to = value;
        if (control < kXControls && !raise(true, control, to, true) && control < kXControls)
            x_set(pad.game, control, to);
    }
    for (std::uint32_t control = 0; control < kXControls; ++control) {
        const std::int32_t now = x_get(state->Gamepad, control), before = x_get(pad.read, control);
        if (now == before)
            continue;
        if (!g_track_xinput && x_quiet(control, now) && x_quiet(control, before)) {
            x_set(pad.game, control, now); // inside the dead zone: the game reads it, nothing is raised
            continue;
        }
        std::uint32_t changed = control;
        std::int32_t to = now;
        if (!raise(true, changed, to, false) && changed < kXControls)
            x_set(pad.game, changed, to);
    }
    pad.read = state->Gamepad;
    state->Gamepad = pad.game;
    view_x(user, state->Gamepad);
    if (user == 0) {
        feed_x(state->Gamepad);
        if (gui::wants_gamepad())
            state->Gamepad = XINPUT_GAMEPAD{};
    }
}

void on_dinput_state(void* device, DWORD size, void* state_data)
{
    if (state_data == nullptr || (size != sizeof(DIJOYSTATE) && size != sizeof(DIJOYSTATE2)))
        return;
    plugins::on_controller_state(device, size, state_data);
    addons::on_dinput_state(device, size, state_data);
    auto* bytes = static_cast<std::uint8_t*>(state_data);
    const DWORD buttons = size == sizeof(DIJOYSTATE) ? 32 : 128;
    const DWORD end = kButtons + buttons;
    DPad& pad = g_dpads[device];
    g_last_dpad = device;
    std::deque<std::pair<DWORD, DWORD>> queued;
    {
        const std::lock_guard lock(g_lock);
        queued.swap(g_dinput_queue);
    }
    if (pad.read.size() != size || (queued.empty() && !listening())) {
        pad.read.assign(bytes, bytes + size);
        pad.game = pad.read;
        view_d(bytes, end);
        feed_d(bytes, end);
        if (gui::wants_gamepad())
            rest_d(bytes, end);
        return;
    }
    for (const auto& [offset, value] : queued) {
        std::uint32_t control = offset;
        std::int32_t to = static_cast<std::int32_t>(value);
        if (control < end && !raise(false, control, to, true) && control < end)
            d_set(pad.game, control, to);
    }
    const std::vector<std::uint8_t> now_state(bytes, bytes + size);
    for (DWORD offset = 0; offset < end; offset = d_next(offset)) {
        const std::int32_t now = d_get(now_state, offset), before = d_get(pad.read, offset);
        if (now == before)
            continue;
        if (!g_track_dinput && offset < kHats && std::abs(now - before) < 512) {
            d_set(pad.game, offset, now); // a small move of an axis: the game reads it, nothing is raised
            continue;
        }
        std::uint32_t control = offset;
        std::int32_t to = offset >= kButtons ? (now & 0x80) : now;
        if (!raise(false, control, to, false) && control < end)
            d_set(pad.game, control, to);
    }
    // what's past the buttons (DIJOYSTATE2's velocities, forces) as it came
    pad.read = now_state;
    std::memcpy(pad.game.data() + end, now_state.data() + end, size - end);
    std::memcpy(bytes, pad.game.data(), size);
    view_d(bytes, end);
    feed_d(bytes, end);
    if (gui::wants_gamepad())
        rest_d(bytes, end);
}

void on_dinput_data(void* device, DWORD object_size, void* data, DWORD* count)
{
    if (data == nullptr || count == nullptr || object_size < sizeof(DWORD) * 2)
        return;
    plugins::on_controller_data(device, object_size, data, count);
    auto* bytes = static_cast<std::uint8_t*>(data);
    if (*count == 0 || !listening()) {
        view_d_data(bytes, object_size, *count);
        if (gui::wants_gamepad())
            *count = 0; // buffered reads: nothing happened, while an addon has the pad
        return;
    }
    DWORD kept = 0;
    for (DWORD i = 0; i < *count; ++i) {
        auto* entry = reinterpret_cast<DWORD*>(bytes + i * object_size); // dwOfs, dwData
        std::uint32_t control = entry[0];
        std::int32_t value = static_cast<std::int32_t>(entry[1]);
        if (raise(false, control, value, false))
            continue;
        entry[0] = control;
        entry[1] = static_cast<DWORD>(value);
        if (kept != i)
            std::memmove(bytes + kept * object_size, bytes + i * object_size, object_size);
        ++kept;
    }
    *count = kept;
    view_d_data(bytes, object_size, *count);
    if (gui::wants_gamepad())
        *count = 0;
}

void queue_dinput(DWORD offset, DWORD value)
{
    const std::lock_guard lock(g_lock);
    g_dinput_queue.emplace_back(offset, value);
}

void queue_xinput(std::uint8_t button, std::int16_t value)
{
    const std::lock_guard lock(g_lock);
    g_xinput_queue.emplace_back(button, value);
}

bool track_dead_zone(bool xinput)
{
    return xinput ? g_track_xinput : g_track_dinput;
}

void set_track_dead_zone(bool xinput, bool track)
{
    (xinput ? g_track_xinput : g_track_dinput) = track;
}

void game_settings(const std::string& mode, const std::string& buttons)
{
    const std::lock_guard lock(g_lock);
    if (!mode.empty())
        g_game_mode = mode;
    if (!buttons.empty())
        g_game_buttons = buttons;
    const bool xinput = g_game_mode.empty() || field(g_game_mode, 5, 0) == 1;
    const int button = g_game_buttons.empty() ? kNormalView : field(g_game_buttons, kViewSlot, -1);
    const int x = xinput && button >= 0 && button < 16 ? static_cast<int>(kXView[button]) : -1;
    const int d = !xinput && button >= 0 && button < 128 ? static_cast<int>(DIJOFS_BUTTON(button)) : -1;
    if (x == g_view_x.load() && d == g_view_d.load())
        return;
    g_view_x = x;
    g_view_d = d;
    if (x >= 0)
        log::write(L"Gamepad: the game's Change View is its button %d, XInput's control %d ([camera] view.reset)", button, x);
    else if (d >= 0)
        log::write(L"Gamepad: the game's Change View is DirectInput's button %d ([camera] view.reset)", button);
    else
        log::write(L"Gamepad: the game's Change View is %d, no button [camera] view.reset can take", button);
}

} // namespace vanacore::gamepad
