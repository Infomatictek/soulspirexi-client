#pragma once

#include <windows.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

struct lua_State;

// Lua addons: <VanaCore>\addons\<name>\<name>.lua, each in its own LuaJIT state, loaded and
// unloaded in game with /addon load|unload|reload <name> and /addon list.
namespace vanacore::addons {

// Addons come from <install_dir>\addons and can read `settings_ini` (vanacore.ini). Takes over
// FFXI's command line (chat.hpp) for /addon commands and addons' command events.
void install(const wchar_t* install_dir, const wchar_t* settings_ini);

// [addons] silent read again (vanacore.ini changed: /vana's switch); /addon silent's choice holds until it changes
void reload(const wchar_t* ini);

// [addons] ashita.enabled (default 1): false when Ashita compatibility is explicitly disabled (Modern Native profile).
bool ashita_enabled();

// Whether `name` (lower case) is one of CatsEyeXI's required plugins (cexidats, cexislots) or addons (customcolors):
// boot.txt loads them first, and /plugin unload and /addon unload leave them alone.
bool required(std::string_view name, bool plugin);

// A bind's commands (binds.cpp), in order, as Ashita runs a bind: "/wait <seconds>" among them ("/sleep", "/pause"; a
// second with no number) holds up the ones after it. Those before any wait run at the next frame, the rest at their
// time, without waiting for a script's lines. On the game's thread.
void run_commands(const std::vector<std::string>& lines);

// Ashita's IChatManager::ExecuteScriptString, which /execstr runs: `script` split at each ';' (each part trimmed; empty
// ones and ones starting with '#' skipped), "%1%" in a part the first word of `args`, then run as run_commands runs a
// bind's, except that those before any wait run now unless `queued`. An argument that wasn't given stops it there.
void execute_script_string(const std::string& script, const std::string& args, bool queued);
// IChatManager::ExecuteScript: scripts\<name>.txt as "/exec <name> <args>" runs it, queued either way (its waits never
// hold up the caller).
void execute_script(const std::string& name, const std::string& args);

// Once per frame, from Present: each addon's tasks and 'present' event.
void on_frame();

// A mouse event (see window.cpp): each addon's 'mouse' event, (type, x, y, delta, blocked) - where in
// VanaCore's interface's pixels (x, y), or for an Ashita addon the game's (game_x, game_y), as its
// fonts and sprites are drawn. True when one took it (a handler returned true). `by_gui`: an ImGui window took it (a
// press or wheel turn over one, the /vana window's too, or the release of a press it took), whose (gui::taken_by,
// "ashita/fancychat"); null when none did. Ashita addons don't hear that, unless it's their own window: their handlers
// don't look at e.blocked (Ashita's own fonts and primitives libraries, equipmon), so they'd act on a click or wheel turn
// meant for the window over them, but fancychat scrolls its own chat window by its 'mouse' event's wheel, not ImGui's.
// Windower's and VanaCore's hear it as blocked.
bool on_mouse(int type, int x, int y, int game_x, int game_y, int delta, bool blocked, const std::string* by_gui);

// A key message (WM_KEYDOWN, WM_KEYUP and the SYS ones): each addon's 'key' event, (wparam, lparam,
// blocked). True when one took it.
bool on_key(WPARAM wparam, LPARAM lparam, bool blocked);

// True while an addon listens for 'packet_in' or 'packet_out' (packets.cpp passes the game's packets
// through them).
bool wants_packets();

// Controllers' changes (gamepad.cpp): addons' 'xinput_button' or 'dinput_button' (e.button, e.state, e.blocked,
// e.injected; a handler changes button and state, or blocks it). True: blocked.
bool wants_gamepad();
bool on_gamepad_button(bool xinput, std::uint32_t& button, std::int32_t& value, bool blocked, bool injected);
// Each XInputGetState read: addons' 'xinput_state' (e.user, e.state: the XINPUT_STATE, changed in place)
void on_xinput_state(std::uint32_t user, void* state);
// Each DirectInput GetDeviceState read: addons' 'dinput_state' (e.device, e.size, e.state, e.state_modified)
void on_dinput_state(void* device, DWORD size, void* state);

// The game's DirectInput keyboard: each state read (addons' 'key_state': e.data, the 256 keys; e.data_raw, where they
// are, to change; e.size) and each buffered key event ('key_data': e.key, e.down, e.blocked, e.injected; true:
// blocked).
void on_key_state(void* keys, DWORD size);
bool wants_key_data();
bool on_key_data(DWORD key, bool down, bool blocked);

// The addon whose handler blocked the last event raised on this thread (its name; empty when none did, or what blocked
// it came before the addons), for menulog.cpp's "kept from the game by"
const std::string& blocked_by();

// The game's scenes (overlay.cpp: its BeginScene, after the real call, and its EndScene, before it; not the ones VanaCore
// and the plugins begin in Present): addons' 'beginscene' and 'endscene' (back_buffer: whether the scene draws on the
// back buffer, not a shadow map or the like), Ashita's d3d_beginscene and d3d_endscene. wants_scenes() is lock-free, for
// each scene: true while an addon listens for one.
bool wants_scenes();
void on_scene(bool begin, bool back_buffer);

// The game's draw calls (overlay.cpp), as Ashita's addons have them: 'd3d_dp' (DrawPrimitive: e.primitive_type,
// e.start_vertex, e.primitive_count), 'd3d_dip' (DrawIndexedPrimitive: e.primitive_type, e.min_index, e.num_vertices,
// e.start_index, e.primitive_count), 'd3d_dpup' (DrawPrimitiveUP: e.primitive_type, e.primitive_count,
// e.vertex_stream_zero_data, e.vertex_stream_zero_stride) and 'd3d_dipup' (DrawIndexedPrimitiveUP: e.primitive_type,
// e.min_vertex_index, e.num_vertex_indices, e.primitive_count, e.index_data, e.index_data_format,
// e.vertex_stream_zero_data, e.vertex_stream_zero_stride), the pointers as light userdata. True: blocked, the draw
// skipped. wants_draws() is lock-free, for each draw: true while an addon listens for one of them.
bool wants_draws();
bool on_draw_primitive(std::uint32_t type, std::uint32_t start_vertex, std::uint32_t count);
bool on_draw_indexed_primitive(std::uint32_t type, std::uint32_t min_index, std::uint32_t num_vertices, std::uint32_t start_index,
                               std::uint32_t count);
bool on_draw_primitive_up(std::uint32_t type, std::uint32_t count, const void* vertices, std::uint32_t stride);
bool on_draw_indexed_primitive_up(std::uint32_t type, std::uint32_t min_index, std::uint32_t num_vertices, std::uint32_t count,
                                  const void* indices, std::uint32_t index_format, const void* vertices, std::uint32_t stride);

// What the addon of `L` raised as a plugin event, kept for what it leaves in another plugin (the Minimap's opacity is put
// back to full when the addon that faded it unloads). Game thread.
void note_plugin_event(lua_State* L, const std::string& name, const std::string& data);

// A plugin event (IPluginManager::RaiseEvent): addons' 'plugin_event' (e.name, e.data, e.data_raw, e.size), at the next
// frame.
void queue_plugin_event(const std::string& name, const std::string& data);

// An event for one addon at the next frame, the one whose gameui key (gameui.hpp: "<source>/<name>") is `key`:
// 'gameui_button' and 'gameui_replaced' (e.element: its button, or the game's item it took over, chosen in the game's
// menus). Dropped if it isn't loaded by then. Game thread.
void queue_event(const std::string& key, const std::string& event, const std::string& element);
// 'gameui_trigger' for the addon of `key` at the next frame: e.trigger (the trigger's id), e.menu (the layout's name,
// "menuwind"), e.key (the window event: 5 select, ...), e.item, e.name (the item's, when known), e.caught (the game's
// own action was kept from its handler). Game thread.
void queue_trigger(const std::string& key, int trigger, const std::string& menu, int event, int item, const std::string& name,
                   bool caught);
// 'gameui_window' for the addon of `key` at the next addons' frame (gamewin.cpp, for vana.gameui.watch): e.element the
// window's name ("configwi"), e.what 'open', 'change' or 'close'. Game thread.
void queue_window(const std::string& key, const std::string& window, const char* what);

// The game is closing (shutdown.cpp): each addon's unload event, the last loaded first. On the game's thread, outside
// any Lua call, each is unloaded as /addon unload does (without its chat line); otherwise only the events are raised
// (the states left as they are: the process is ending).
void shutdown(bool as_game_thread);

// The MCP server's lua tool: runs `code` in loaded addon `name`'s Lua (as an expression first, else as statements), and
// gives what it printed and what it returned (tables shown `depth` levels deep) in `output`. "mcp" (addons\vanacore\mcp)
// is loaded the first time it's asked for, and again after a crash. False with `error` when the addon isn't loaded or the
// code fails. Game thread.
bool eval(const std::string& name, const std::string& code, int depth, std::string& output, std::string& error);

// A number from loaded addon `name`'s Lua: `code` is a chunk that returns it (true counts 1; chat.cpp asks fancychat
// whether its input bar is on and whether it keeps the game's chat closed). 0 when the addon isn't loaded, has crashed,
// or the code fails. Game thread.
int ask(const std::string& name, const char* code);

// As ask(), for a chunk that returns up to `most` numbers (phraselist.cpp asks fancychat where its input bar is): they go
// into `values`, and the count before the first that isn't a number is returned. 0 as ask() gives 0.
int ask_numbers(const std::string& name, const char* code, double* values, int most);

// An addon held off while a VanaCore feature does its job (xicamera while True Zoom is on: camera.cpp). hold() unloads it
// if it's loaded and refuses /addon load for it (boot.txt's too), saying `why` in the chat log; release() lets it load
// again, and loads it if holding it unloaded it or refused a load. Game thread, outside addons' Lua.
void hold(const std::string& name, const std::string& why);
void release(const std::string& name);

// The thread Lua runs on (the game's: its frames and command line); true too before any Lua has run.
bool is_game_thread();
// A packet in or out (`size` bytes, header first; `modified` what the game gets or sends, which an
// addon may change): each addon's 'packet_in' or 'packet_out' event, with a table as Ashita's addons
// have it (id, size, data, data_modified, data_raw, data_modified_raw, chunk_size, chunk_data,
// injected, blocked). True when it's blocked. `skip`: the Ashita addons ("addon <name>") that don't see it: the ones
// that added it (see packets::inject).
bool on_packet(bool incoming, std::uint16_t id, std::uint32_t size, const std::uint8_t* data, std::uint8_t* modified,
               std::uint32_t chunk_size, const std::uint8_t* chunk, bool injected, bool blocked,
               const std::vector<std::string>& skip = {});

// The addons loaded now, in load order: each its name as its folder spells it, the folder it came from ("vanacore",
// "ashita" or "windower"), its own details (addon.version, .author, .desc, .link; _addon's for a Windower addon), its
// file, its state as Ashita numbers them (0 error: it crashed, 1 running, 3 loading, 4 unloading, 5 reloading; 2,
// released, is never seen) and what its Lua state holds in bytes. From Lua running on the game's thread (the vana
// window's Addons tab, Ashita's AddonManager).
struct Loaded {
    std::string name;
    std::string source;
    std::string version, author, description, link;
    std::string file;
    int state = 1;
    double memory = 0;
};
std::vector<Loaded> loaded();

// What /vana stats' addon window shows about a loaded addon (the name as the stats tree has it).
struct Info {
    std::string kind;              // "VanaCore", "Ashita" or "Windower"
    double memory_kb = 0;          // what its Lua state holds
    int errors = 0;                // reported so far
    bool crashed = false;
    std::vector<std::string> listens; // the kinds of event it listens for beyond the frame: packets, text, keys, ...
};
bool info(const std::string& name, Info& out);

// One kind of error an addon reported (the same error again counts up `count`), for the addon window's list of them.
struct ErrorEntry {
    std::string time;    // when it was last reported (HH:MM:SS)
    std::string what;    // the event or task it was in
    std::string details; // the message and its traceback, as the log has it
    int count = 1;
};
// The distinct errors the addon reported, oldest first (the last 20 kinds). Empty if it isn't loaded.
std::vector<ErrorEntry> recent_errors(const std::string& name);

// A sampling profile of the addon's Lua (LuaJIT's jit.profile, every millisecond, run inside its state): only one at a
// time. start() begins it; stop() ends it and gives its report: "T\t<samples>", "S\t<VM state>\t<samples>", then the top
// "L\t<samples>\t<file:line>" and "F\t<samples>\t<function>" lines. False, with `error`, when it can't.
bool profile_start(const std::string& name, std::string& error);
bool profile_stop(const std::string& name, std::string& report, std::string& error);

} // namespace vanacore::addons
