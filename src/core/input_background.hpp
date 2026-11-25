/* The game's controllers in the background: inactive_switch, controllers_quiet, let_go and settle_inactive_switch, the game's
 * own "Works while the game is inactive" switch found in FFXiMain.dll and held to. input.cpp includes this inside its namespace
 * just after the controller search, after once, whose name it uses. It is no file of its own. */
#pragma once

// ---- Controllers in the background ------------------------------------------------------------
// The game's own "Works while the game is inactive" switch (gamepad.mode's fifth; /vana > Client > Gamepads) says whether
// its controllers work while it's in the background. The game acts on it itself (a DirectInput controller opened for the
// foreground only; XInputEnable(FALSE) when its window loses the keyboard focus), and VanaCore holds the reads to it as
// well, by window::in_background(): with the switch off and the game in the background, its controllers read as let go
// (centred, nothing pressed), plugins' and addons' controller events too. Two games on one PC and one pad: only the one
// in front moves.
// The switch is FFXiMain's byte, found where the game's own code keeps it: FUN_10005600 in the 2025-12-26 client puts it
// aside (0) while the game's gamepad config tool runs from the game, and back when the tool closes. Its bytes are checked
// first; with another client only the game's own handling is left.
// Located by runtime byte-pattern scan in FFXiMain.dll (fail-closed if unmatched) instead of a fixed RVA.
constexpr char kSwitchCodePattern[] = "8A 44 24 04 33 D2 3A C2 A0";
constexpr BYTE kSwitchCodeHead[] = {0x8A, 0x44, 0x24, 0x04, 0x33, 0xD2, 0x3A, 0xC2, 0xA0};

struct InactiveSwitch {
    volatile BYTE* now = nullptr;         // what the game acts on (0 while put aside)
    volatile BYTE* wanted = nullptr;      // the switch as set (what it goes back to)
    volatile BYTE* aside = nullptr;       // 1 while the game's config tool has it put aside
    volatile BYTE* pad_changed = nullptr; // the pad's per-frame update opens the controller again
};

// The address an instruction at code + at refers to: its opcode bytes, then the address.
volatile BYTE* operand(const BYTE* code, DWORD at, std::initializer_list<BYTE> opcode)
{
    for (const BYTE byte : opcode)
        if (code[at++] != byte)
            return nullptr;
    DWORD address = 0;
    std::memcpy(&address, code + at, sizeof address);
    return reinterpret_cast<volatile BYTE*>(static_cast<std::uintptr_t>(address));
}

bool in_image(const volatile BYTE* address, std::uintptr_t base, std::size_t size)
{
    const auto at = reinterpret_cast<std::uintptr_t>(address);
    return at >= base && at - base < size;
}

// The switch's bytes from FFXiMain.dll's code (`game`, its base): 1 found, 2 not this client's, 0 not yet. FFXiMain.dll
// decrypts itself and only then has its addresses moved to where it's loaded, so a read in between (PlayOnline's thread
// reads the controller while the game loads) finds the code but addresses at its preferred base, 0x10000000, which
// PlayOnline's PolHook.dll can have taken: the DLL loaded elsewhere, nothing was there and the game crashed at start.
int read_switch_code(HMODULE module, InactiveSwitch& out)
{
    const auto* game = reinterpret_cast<const BYTE*>(module);
    const std::uintptr_t site = scan::find(module, kSwitchCodePattern);
    if (site == 0)
        return 2;
    __try {
        const BYTE* code = reinterpret_cast<const BYTE*>(site);
        if (std::memcmp(code, kSwitchCodeHead, sizeof kSwitchCodeHead) != 0 || code[0x51] != 0xC2 || code[0x52] != 0x04)
            return 2;
        InactiveSwitch found;
        found.aside = operand(code, 0x08, {0xA0});
        found.now = operand(code, 0x13, {0xA0});
        found.wanted = operand(code, 0x1E, {0xA2});
        found.pad_changed = operand(code, 0x4C, {0xA2});
        // the same three again where they're put back
        volatile BYTE* const wanted = operand(code, 0x30, {0xA0});
        volatile BYTE* const aside = operand(code, 0x35, {0x88, 0x15});
        volatile BYTE* const now = operand(code, 0x3B, {0xA2});
        if (found.aside == nullptr || found.now == nullptr || found.wanted == nullptr || found.pad_changed == nullptr ||
            wanted == nullptr || aside == nullptr || now == nullptr)
            return 2;
        // one not moved yet (a read while they're being moved too): not yet
        const auto base = reinterpret_cast<std::uintptr_t>(game);
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(game);
        const std::size_t size = reinterpret_cast<const IMAGE_NT_HEADERS*>(game + dos->e_lfanew)->OptionalHeader.SizeOfImage;
        for (const volatile BYTE* address : {found.aside, found.now, found.wanted, found.pad_changed, wanted, aside, now})
            if (!in_image(address, base, size))
                return 0;
        if (wanted != found.wanted || aside != found.aside || now != found.now)
            return 2;
        out = found;
        return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 2;
    }
}

// Looked for from a controller read or a live apply, from when FFXiMain.dll is there until its code is ready to say
// (read_switch_code).
const InactiveSwitch& inactive_switch()
{
    static InactiveSwitch found;
    static std::atomic<int> state{0}; // 0 not looked for (or not yet), 1 found, 2 not this client's
    if (state.load(std::memory_order_acquire) == 0) {
        const HMODULE game = GetModuleHandleW(L"FFXiMain.dll");
        if (game == nullptr)
            return found;
        InactiveSwitch read;
        const int result = read_switch_code(game, read);
        if (result == 0)
            return found; // looked for again at the next read
        if (result == 1)
            found = read;
        else
            log::write(L"Input: this client's gamepad \"works while inactive\" switch isn't where VanaCore knows it; the game "
                       L"alone keeps its controllers from working in the background");
        state.store(result, std::memory_order_release);
    }
    return found;
}

std::atomic<bool> g_logged_quiet{false};

// True: the game's controllers read as let go now (the game in the background, its switch off; or gamepads disabled).
bool controllers_quiet()
{
    if (g_pads_off.load(std::memory_order_relaxed))
        return true;
    const InactiveSwitch& found = inactive_switch();
    if (found.now == nullptr || *found.now != 0 || !window::in_background())
        return false;
    once(g_logged_quiet, L"the game is in the background with its gamepad switch \"works while inactive\" off: its "
                         L"controllers read as let go until it's in front again");
    return true;
}

// A DIJOYSTATE (or DIJOYSTATE2, which starts the same) as the controller let go reads: each axis in the middle of its
// range (the device's, as the game set it), the hats centred, nothing pressed.
void let_go(IDirectInputDevice8A* device, void* data, DWORD size)
{
    if (size < sizeof(DIJOYSTATE))
        return;
    auto* bytes = static_cast<BYTE*>(data);
    std::memset(bytes, 0, size);
    for (DWORD offset = 0; offset < offsetof(DIJOYSTATE, rgdwPOV); offset += sizeof(LONG)) { // lX to rglSlider[1]
        DIPROPRANGE range{};
        range.diph.dwSize = sizeof range;
        range.diph.dwHeaderSize = sizeof range.diph;
        range.diph.dwObj = offset;
        range.diph.dwHow = DIPH_BYOFFSET;
        if (SUCCEEDED(device->GetProperty(DIPROP_RANGE, &range.diph))) {
            const LONG middle = range.lMin + (range.lMax - range.lMin) / 2;
            std::memcpy(bytes + offset, &middle, sizeof middle);
        }
    }
    std::memset(bytes + offsetof(DIJOYSTATE, rgdwPOV), 0xFF, sizeof(DIJOYSTATE::rgdwPOV));
}

// The game's mode setter, called while it plays (initial = 0), puts the switch aside as if its config tool were open,
// until the tool closes: with none open, the switch stayed off whatever /vana set. It's put back at once here, and a
// switch that changed opens the controller again (the game opens a DirectInput one for the background or not by it).
void settle_inactive_switch(int before)
{
    const InactiveSwitch& found = inactive_switch();
    if (found.now == nullptr || *found.aside != 0)
        return;
    const BYTE wanted = *found.wanted;
    *found.now = wanted;
    if (before >= 0 && wanted != before) {
        *found.pad_changed = 1;
        log::write(L"Input: the gamepad works while the game is inactive: %ls", wanted != 0 ? L"on" : L"off");
    }
}
