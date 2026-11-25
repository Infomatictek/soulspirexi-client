#pragma once

#include <windows.h>

#include <string>
#include <vector>

// Dear ImGui (third_party/imgui, 1.92.3 WIP docking at a28cb615: the version Ashita 4.30 builds in), for
// Ashita plugins' IGuiManager and Ashita addons' imgui: one context for all of them, drawn over the
// game after plugins' fonts and primitives and under VanaCore's own window. ImGui works in the game's
// pixels (the back buffer's), as Ashita's does, so an addon's windows line up with its fonts,
// primitives and sprites and its mouse events, which are in the game's pixels too; what it draws is
// scaled onto VanaCore's interface (ui::screen), and VanaCore's own ImGui windows are scaled back up
// so they keep their size (layout::set_own_scale). Set up as Ashita sets up its own: Agave at 18, 24
// and 32 px with Font Awesome 6 merged in (fonts\imgui\), config\imgui.ini. Game thread only, except
// where noted.
namespace vanacore::gui {

// Once, from VanaCoreInstall: where fonts\imgui\ and config\imgui.ini are, and the settings file ([client] imgui.font: the
// font the text draws in instead of Agave, read when the first frame makes the atlas).
void install(const wchar_t* install_dir, const wchar_t* settings_ini);

// Each frame, from Present: a new ImGui frame before plugins and addons draw with it (their Present
// events), in the game's `game_width` x `game_height` pixels, `width` x `height` the interface's; then
// its end, then its drawing (between text::begin and text::end, in `width` x `height` interface
// pixels, under graphics::own_pixels).
void begin_frame(void* device, HWND window, unsigned width, unsigned height, unsigned game_width, unsigned game_height);
void end_frame();
void draw(void* device, unsigned width, unsigned height);
// The device is changing: its textures go.
void release();
// Between begin_frame and end_frame: ImGui's widgets can be used (not from a command handler).
bool in_frame();

// Around one addon's frame (addons.cpp): ImGui's state noted before it and put back after, so an addon whose Lua errors
// between a Begin and its End (a Push and its Pop) doesn't leave them open for every addon after it that frame. Not nested.
void addon_frame_begin();
void addon_frame_end();

// VanaCore's ImGui context (ImGuiContext*), which every plugin and addon draws with.
void* context();

// IGuiManager::GetVisible/SetVisible: false hides every ImGui window (they're still built).
bool visible();
void set_visible(bool visible);

// A mouse message (window.cpp: its type is the message - WM_MOUSEFIRST, x and y in interface
// pixels). True when ImGui takes it: a press or wheel over one of its windows, or the release of
// a press it took. The game and other handlers don't see those.
bool on_mouse(int type, int x, int y, int delta);
// A key or character message for the game window. True when ImGui takes it (a text box or other
// item has the keyboard): the game doesn't see it.
bool on_key(UINT message, WPARAM wparam, LPARAM lparam);

// True when (x, y), in interface pixels, is over an ImGui window that takes the mouse, as last
// drawn, and not over one of the game's windows drawn over ImGui's (set_covered). Any thread.
bool blocks_mouse(int x, int y);
// ... and whose window is on top there, as layout::owner_of has it ("ashita/fancychat"; "" for VanaCore's C++ ones).
bool blocks_mouse(int x, int y, std::string* owner);
// Whose window the last message on_mouse took (true) was over; for a release, the press it took. Game thread.
const std::string& taken_by();
// Where the game's windows are drawn over ImGui's (raise.cpp: the auto-translate list, the windows over
// fancychat's chat), in the game's pixels, set each frame: the mouse there is the game's. ImGui's
// windows under them don't take a press there and aren't hovered, unless a popup is open or ImGui holds
// a press (a drag).
void set_covered(const std::vector<RECT>& rects);
// True when (x, y), in the game's pixels, is in one of those this frame, with no popup open (the plugin API's
// game_window_at: a plugin's mouse callback lets it through). Any thread.
bool game_window_at(float x, float y);
// The rectangles of the ImGui windows shown in the last frame whose names start with `prefix`, as
// they're drawn (their scale with them), in the game's pixels, added to `out`.
void shown_windows(const char* prefix, std::vector<RECT>& out);
// ... and of the top-level ImGui windows (not their children) shown in the last frame that belong to one of `owners`
// (blocks_mouse's owner: "ashita/fancychat", "vanacore/hello"), the same way.
void shown_windows_of(const std::vector<std::string>& owners, std::vector<RECT>& out);
// ... and of VanaCore's own top-level windows and popups (/vana, the HUD Manager: raise_own_windows' kind) shown in the last
// frame, the same way. A game window drawn over ImGui's (raise.cpp) stays under them.
void own_windows(std::vector<RECT>& out);
// True while an ImGui item has the keyboard (a text box being typed in), as of the last frame: the
// game's DirectInput keyboard reads nothing meanwhile. Any thread.
bool wants_keyboard();

// The controller (gamepad.cpp): a gamepad key of ImGui's (632 Start ... 655 right stick down) as its read has it, with
// its analog value for a stick or trigger (-1 for a button)
void gamepad_key(int key, bool down, float analog);
// vana.gamepad.nav: an addon window has the controller for the next frame (held as long as it keeps saying so); ImGui
// navigates with it, the game reads it at rest
void want_gamepad(bool on);
bool wants_gamepad();
// Authoritative cursor ownership helpers for VanaCore-native Soulspire UI (never calls Win32 ShowCursor).
void set_cursor_override(bool visible);
void restore_cursor();
// In ImGui's frame: the mouse over (x0, y0)-(x1, y1), in the game's pixels, is ImGui's after this frame as over one of
// its windows (a press there isn't the game's). For what VanaCore draws with no window: buffwin's box being dragged.
void take_mouse(float x0, float y0, float x1, float y1);

} // namespace vanacore::gui
