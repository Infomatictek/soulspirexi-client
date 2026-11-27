#include "addons.hpp"

#include <windows.h>
#include <shellapi.h>

#include <lua.hpp>

#include <vanacore/log_file.hpp>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <cstring>
#include <iterator>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include "abtest.hpp"
#include "addonpkg.hpp"
#include "addon_fault.hpp"
#include "addon_trace.hpp"
#include "debugrec.hpp"
#include "game.hpp"
#include "gameevents.hpp"
#include "gamesave.hpp"
#include "gameui.hpp"
#include "groundmarks.hpp"
#include "graphics.hpp"
#include "gui.hpp"
#include "ipc.hpp"
#include "aliases.hpp"
#include "binds.hpp"
#include "camera.hpp"
#include "chat.hpp"
#include "clipboard.hpp"
#include "commands.hpp"
#include "craftlog.hpp"
#include "ini.hpp"
#include "log.hpp"
#include "soulspire/soulspire.hpp"
#include "lua_api.hpp"
#include "lua_net.hpp"
#include "compat/ashita/host.hpp"
#include "packets.hpp"
#include "packetwatch.hpp"
#include "perf.hpp"
#include "hwmon.hpp"
#include "stats.hpp"
#include "plugins.hpp"
#include "raise.hpp"
#include "resource.hpp"
#include "screenshot.hpp"
#include "ui.hpp"
#include "usercaches.hpp"
#include "window.hpp"

namespace vanacore::addons {
namespace {

struct Addon {
    lua_api::Context context;
    lua_State* L = nullptr;
    int hooks = LUA_NOREF;  // what lua/vana.lua returned: raise, frame
    int errors = 0;         // reported so far
    ULONGLONG streak_start = 0; // when the current run of errors began (ms)
    ULONGLONG last_error_at = 0; // when the last error was (ms)
    std::string last_error; // the last one shown, so a repeating error is shown once
    std::vector<ErrorEntry> recent; // each kind of error reported, for the stats window's addon pane
    bool unload_requested = false;
    std::string unload_reason; // why it's going, for the chat line (empty: nothing to add)
    bool unload_raised = false; // its unload event raised already (the game closing): not again
    bool minimap_dimmed = false; // the last 'minimap' opacity event it raised left the Minimap plugin faded
    bool crashed = false; // a hardware exception in its code: its state is never touched again
    int busy = 0;         // calls into its Lua in progress (a line it prints reaches the others' text_in, not its own)
    bool packets = false; // it listens for 'packet_in' or 'packet_out'
    bool text = false;    // ... 'text_in' or 'text_out'
    bool gamepad = false; // ... 'xinput_button' or 'dinput_button'
    bool keys = false;    // ... 'key_state' or 'key_data'
    bool mouse = false;   // ... 'mouse'
    bool key = false;     // ... 'key'
    bool draws = false;   // ... 'd3d_dp', 'd3d_dpup', 'd3d_dip' or 'd3d_dipup' (counted in g_draw_listeners)
    bool scenes = false;  // ... 'beginscene' or 'endscene' (counted in g_scene_listeners)
    std::uint32_t game_events = 0; // ... the game events (gameevents.hpp: 'zone_change', 'main_menu_open', ...), as bits
    int state = 3;       // as Ashita's addons number it: 3 loading, 1 running, 4 unloading, 5 reloading (0 crashed)
    std::string version, author, description, link; // its own addon.version, ... as it loaded (for loaded())
    std::string display;  // the name as its folder spells it
    lua_api::Flavour source = lua_api::Flavour::VanaCore; // the folder it came from
};

// an addon crashing (addon_fault.hpp)
using addon_fault::describe;
using addon_fault::Fault;
using addon_fault::guarded_close;
using addon_fault::guarded_pcall;
using addon_fault::kCrashed;

// An addon that keeps erroring (usually on every frame) for this long is broken, and is unloaded.
// Shorter bursts are left alone: some addons fail for a moment while they start up.
constexpr ULONGLONG kErrorStreakMs = 5000;
// Errors further apart than this start a new streak.
constexpr ULONGLONG kErrorGapMs = 1000;
// The kinds of error each addon keeps for its window in /vana stats.
constexpr size_t kRecentErrors = 20;
std::mutex g_errors_lock; // an addon's list of them (report writes it, the stats window reads it)
// The events VanaCore raises; addons can listen for others, which never fire (yet).
constexpr const char* kEvents[] = {"load",          "unload",        "command",  "present",  "mouse",
                                   "key",           "packet_in",     "packet_out", "text_in", "text_out",
                                   "xinput_button", "dinput_button", "xinput_state", "dinput_state", "key_state", "key_data", "plugin_event",
                                   "unhandled",     "ipc",           "d3d_dp",   "d3d_dpup", "d3d_dip",  "d3d_dipup",
                                   "beginscene",    "endscene",      "gameui_button", "gameui_replaced", "gameui_trigger",
                                   "gameui_window"};

std::vector<std::unique_ptr<Addon>> g_addons; // in load order
std::deque<std::pair<std::string, std::string>> g_plugin_events; // plugin events for 'plugin_event', next frame
struct GameuiEvent {
    std::string key, event, element;
    int trigger = 0; // 'gameui_trigger' (queue_trigger): the trigger's id, the window event, the item and its name, caught
    int key_code = 0, item = 0;
    std::string name;
    bool caught = false;
};
std::deque<GameuiEvent> g_gameui_events; // gameui.cpp's events for one addon each, next frame (queue_event)

// An event of g_gameui_events into the table on the top: e.element; 'gameui_window''s e.what; 'gameui_trigger''s e.trigger,
// e.menu, e.key, e.item, e.name, e.caught
void push_gameui_event(lua_State* L, const GameuiEvent& queued)
{
    lua_pushlstring(L, queued.element.data(), queued.element.size());
    lua_setfield(L, -2, "element");
    if (queued.trigger == 0) {
        if (!queued.name.empty()) { // 'gameui_window' (queue_window): 'open', 'change', 'close'
            lua_pushlstring(L, queued.name.data(), queued.name.size());
            lua_setfield(L, -2, "what");
        }
        return;
    }
    lua_pushinteger(L, queued.trigger);
    lua_setfield(L, -2, "trigger");
    lua_pushlstring(L, queued.element.data(), queued.element.size());
    lua_setfield(L, -2, "menu");
    lua_pushinteger(L, queued.key_code);
    lua_setfield(L, -2, "key");
    lua_pushinteger(L, queued.item);
    lua_setfield(L, -2, "item");
    lua_pushlstring(L, queued.name.data(), queued.name.size());
    lua_setfield(L, -2, "name");
    lua_pushboolean(L, queued.caught);
    lua_setfield(L, -2, "caught");
}
std::string g_install;                        // VanaCore's folder, with a trailing backslash (ANSI)
std::wstring g_settings_ini;
std::vector<std::string> g_unraised; // events addons listen for that VanaCore doesn't raise, logged once
int g_depth = 0;                     // Lua calls in progress: addons are only closed at 0
bool g_closing = false;              // the game is closing and every addon with it: nothing is put back in the plugins
std::atomic<int> g_draw_listeners{0}; // addons listening for a draw call event (Addon::draws), read at each draw
std::atomic<int> g_scene_listeners{0}; // ... for a scene's begin or end (Addon::scenes), read at each scene
bool g_silent = false;               // /addon silent ([addons] silent): no chat line for an addon loaded or unloaded
bool g_silent_ini = false;           // [addons] silent as last read: /addon silent holds until the setting changes
bool g_ashita_enabled = true;        // [addons] ashita.enabled (default 1; 0 in Modern Native profile)

// Addons held off (hold()): the name in lower case, why, and whether it was loaded or asked for since
struct Held {
    std::string name;
    std::string why;
    bool wanted = false;
};
std::vector<Held> g_held;

// Lua runs on the game thread (frames and its command line). The lock keeps a second thread out, and
// the log says if one ever turns up.
std::recursive_mutex g_lock;
DWORD g_thread = 0;
bool g_other_thread_logged = false;

void enter_thread(const char* from)
{
    const DWORD thread = GetCurrentThreadId();
    if (g_thread == 0)
        g_thread = thread;
    if (thread != g_thread && !g_other_thread_logged) {
        g_other_thread_logged = true;
        log::write(L"Addons: %hs on thread %lu, but Lua first ran on thread %lu", from, thread, g_thread);
    }
}

std::string ansi(const std::wstring& text)
{
    const int length = WideCharToMultiByte(CP_ACP, 0, text.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string out(length > 1 ? static_cast<size_t>(length) : 1, '\0');
    WideCharToMultiByte(CP_ACP, 0, text.c_str(), -1, out.data(), length, nullptr, nullptr);
    out.resize(out.size() - 1);
    return out;
}

std::string lower(std::string_view text)
{
    std::string out(text);
    for (char& c : out)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

void say(std::string_view text)
{
    chat::write(chat::kDefaultMode, chat::header() + std::string(text), "VanaCore");
}

void say_error(std::string_view text)
{
    say(chat::colour(chat::kError, text));
}

Addon* find(std::string_view name)
{
    for (auto& addon : g_addons)
        if (addon->context.name == name)
            return addon.get();
    return nullptr;
}

Addon* owner(lua_State* L)
{
    const lua_api::Context* context = &lua_api::context(L);
    for (auto& addon : g_addons)
        if (&addon->context == context)
            return addon.get();
    return nullptr;
}

// The first line of an error, for the chat log, with a script's path cut back to the addons folder:
// "...\build\Debug\addons\x\x.lua:3: ..." becomes "addons\x\x.lua:3: ...".
std::string chat_line(std::string_view message)
{
    std::string line(message.substr(0, message.find('\n')));
    const bool path = line.starts_with("...") || (line.size() > 2 && line[1] == ':' && line[2] == '\\');
    const size_t script = line.find(".lua:");
    const size_t folder = script == std::string::npos ? std::string::npos : line.rfind("addons\\", script);
    if (path && folder != std::string::npos)
        line.erase(0, folder);
    return line;
}

// An error in an addon: all of it in the log, its first line in the chat log (once, if it repeats).
void report(Addon& addon, std::string_view what, std::string_view message, std::string_view details)
{
    ++addon.errors;
    {
        const std::lock_guard lock(g_errors_lock);
        SYSTEMTIME clock;
        GetLocalTime(&clock);
        char time[16];
        std::snprintf(time, sizeof time, "%02u:%02u:%02u", clock.wHour, clock.wMinute, clock.wSecond);
        const std::string text(details);
        auto same = std::find_if(addon.recent.begin(), addon.recent.end(), [&](const ErrorEntry& e) { return e.details == text; });
        if (same != addon.recent.end()) {
            ++same->count;
            same->time = time;
        } else {
            if (addon.recent.size() >= kRecentErrors)
                addon.recent.erase(addon.recent.begin());
            addon.recent.push_back({time, std::string(what), text, 1});
        }
    }
    const std::string first_line = chat_line(message);
    if (first_line != addon.last_error) {
        log::write(L"Addon %hs: error in %hs: %hs", addon.context.name.c_str(), std::string(what).c_str(),
                   std::string(details).c_str());
        say_error(addon.display + ": " + first_line);
        addon.last_error = first_line;
    }
    if (addon.crashed && !addon.unload_requested) {
        addon.unload_requested = true;
        addon.unload_reason = "it crashed; the game carries on";
    }
    const ULONGLONG now = GetTickCount64();
    if (addon.last_error_at == 0 || now - addon.last_error_at > kErrorGapMs)
        addon.streak_start = now;
    addon.last_error_at = now;
    if (now - addon.streak_start >= kErrorStreakMs && !addon.unload_requested) {
        addon.unload_requested = true;
        addon.unload_reason = std::to_string(kErrorStreakMs / 1000) + " seconds of errors, the last: " + first_line;
    }
}

// native.report(what, message, traceback), for lua/vana.lua.
int native_report(lua_State* L)
{
    const char* what = luaL_optstring(L, 1, "?");
    const char* message = luaL_optstring(L, 2, "error");
    const char* details = luaL_optstring(L, 3, message);
    if (Addon* addon = owner(L))
        report(*addon, what, message, details);
    return 0;
}

// native.subscribe(event): an addon listens for `event`.
int native_subscribe(lua_State* L)
{
    const char* event = luaL_checkstring(L, 1);
    if (std::strcmp(event, "packet_in") == 0 || std::strcmp(event, "packet_out") == 0)
        if (Addon* addon = owner(L))
            addon->packets = true;
    if (std::strcmp(event, "text_in") == 0 || std::strcmp(event, "text_out") == 0)
        if (Addon* addon = owner(L))
            addon->text = true;
    if (std::strcmp(event, "xinput_button") == 0 || std::strcmp(event, "dinput_button") == 0 ||
        std::strcmp(event, "xinput_state") == 0 || std::strcmp(event, "dinput_state") == 0)
        if (Addon* addon = owner(L))
            addon->gamepad = true;
    if (std::strcmp(event, "key_state") == 0 || std::strcmp(event, "key_data") == 0)
        if (Addon* addon = owner(L))
            addon->keys = true;
    if (std::strcmp(event, "mouse") == 0)
        if (Addon* addon = owner(L))
            addon->mouse = true;
    if (std::strcmp(event, "key") == 0)
        if (Addon* addon = owner(L))
            addon->key = true;
    if (std::strcmp(event, "d3d_dp") == 0 || std::strcmp(event, "d3d_dpup") == 0 || std::strcmp(event, "d3d_dip") == 0 ||
        std::strcmp(event, "d3d_dipup") == 0)
        if (Addon* addon = owner(L); addon != nullptr && !addon->draws) {
            addon->draws = true;
            ++g_draw_listeners;
        }
    if (std::strcmp(event, "beginscene") == 0 || std::strcmp(event, "endscene") == 0)
        if (Addon* addon = owner(L); addon != nullptr && !addon->scenes) {
            addon->scenes = true;
            ++g_scene_listeners;
        }
    if (const int game_event = gameevents::index(event); game_event >= 0) {
        if (Addon* addon = owner(L))
            addon->game_events |= 1u << game_event;
        return 0;
    }
    for (const char* raised : kEvents)
        if (std::strcmp(event, raised) == 0)
            return 0;
    for (const std::string& known : g_unraised)
        if (known == event)
            return 0;
    g_unraised.emplace_back(event);
    const Addon* addon = owner(L);
    log::write(L"Addons: %hs listens for the '%hs' event, which VanaCore doesn't raise yet", addon ? addon->context.name.c_str() : "?",
               event);
    return 0;
}

// The error handler for calls into Lua: the message with a stack traceback.
int traceback(lua_State* L)
{
    const char* message = lua_tostring(L, 1);
    luaL_traceback(L, L, message != nullptr ? message : "(error object is not a string)", 1);
    return 1;
}

// Calls the function under the top `nargs` values. False after an error, with the message and
// traceback in `error`.
bool call(Addon& addon, int nargs, int nresults, std::string& error)
{
    lua_State* L = addon.L;
    const int base = lua_gettop(L) - nargs;
    lua_pushcfunction(L, &traceback);
    lua_insert(L, base);
    // an Ashita addon's fonts and primitives are its own (freed when it goes)
    const bool ashita_addon = addon.context.flavour == lua_api::Flavour::Ashita;
    const std::string outer = ashita_addon ? ashita::set_calling_addon(addon.context.name) : std::string();
    // /vana stats' addon window: this call's time, by the event it ran for (the first argument of a raise, else the frame)
    // (a timed trace times every addon's, for the timed one's share; a traced addon's Lua memory too, before and after)
    const bool traced = addon_trace::on(addon.context.name);
    const bool trace_timed = traced || addon_trace::timing();
    const bool timed = stats::collecting() || trace_timed;
    LARGE_INTEGER started{};
    std::string event = "frame";
    double kb_before = 0;
    if (timed) {
        QueryPerformanceCounter(&started);
        if (nargs >= 1 && lua_type(L, base + 2) == LUA_TSTRING)
            event = lua_tostring(L, base + 2);
        if (traced)
            kb_before = lua_gc(L, LUA_GCCOUNT, 0) + lua_gc(L, LUA_GCCOUNTB, 0) / 1024.0;
    }
    ++g_depth;
    ++addon.busy;
    Fault fault;
    const int status = guarded_pcall(L, nargs, nresults, base, fault);
    --addon.busy;
    --g_depth;
    if (timed) {
        LARGE_INTEGER finished;
        QueryPerformanceCounter(&finished);
        LARGE_INTEGER frequency;
        QueryPerformanceFrequency(&frequency);
        const double ms = static_cast<double>(finished.QuadPart - started.QuadPart) * 1000.0 / static_cast<double>(frequency.QuadPart);
        if (stats::collecting())
            stats::add_addon_event(addon.context.name, event, ms);
        if (trace_timed) {
            // (a crashed state is never touched again)
            const bool memory = traced && status != kCrashed;
            addon_trace::event(addon.context.name, event, ms, kb_before,
                               memory ? lua_gc(L, LUA_GCCOUNT, 0) + lua_gc(L, LUA_GCCOUNTB, 0) / 1024.0 : kb_before);
        }
    }
    if (ashita_addon)
        ashita::set_calling_addon(outer);
    if (status == kCrashed) {
        // Where the state's stack stands is unknown now: nothing more runs in it.
        addon.crashed = true;
        error = "crashed: " + describe(fault);
        return false;
    }
    lua_remove(L, base);
    if (status == 0)
        return true;
    const char* message = lua_tostring(L, -1);
    error = message != nullptr ? message : "unknown error";
    lua_pop(L, 1);
    return false;
}

// Reports an error from call() against `what`.
void report_call(Addon& addon, std::string_view what, const std::string& error)
{
    report(addon, what, error, error);
}

// Pushes hooks[name] (a function from lua/vana.lua).
void push_hook(Addon& addon, const char* name)
{
    lua_rawgeti(addon.L, LUA_REGISTRYINDEX, addon.hooks);
    lua_getfield(addon.L, -1, name);
    lua_remove(addon.L, -2);
}

// Raises `event` (no arguments) in the addon.
void raise(Addon& addon, const char* event)
{
    push_hook(addon, "raise");
    lua_pushstring(addon.L, event);
    std::string error;
    if (!call(addon, 1, 0, error))
        report_call(addon, event, error);
}

// Raises 'packet_in' (`incoming`) or 'packet_out' in the addon, with the table Ashita's addons have (see on_packet): true
// if the packet is blocked, by it or before (`blocked`). What it changes in e.data_modified is copied to `modified`.
bool raise_packet(Addon& addon, bool incoming, std::uint16_t id, std::uint32_t size, const std::uint8_t* data,
                  std::uint8_t* modified, std::uint32_t chunk_size, const std::uint8_t* chunk, bool injected, bool blocked)
{
    lua_State* L = addon.L;
    push_hook(addon, "raise");
    lua_pushstring(L, incoming ? "packet_in" : "packet_out");
    lua_createtable(L, 0, 13);
    lua_pushinteger(L, id);
    lua_setfield(L, -2, "id");
    lua_pushinteger(L, size);
    lua_setfield(L, -2, "size");
    lua_pushinteger(L, packets::buffer_number(incoming)); // VanaCore's own: which buffer (compat\ashita.lua's SimpleLog fix)
    lua_setfield(L, -2, "vana_chunk");
    lua_pushlstring(L, reinterpret_cast<const char*>(data), size);
    lua_setfield(L, -2, "data");
    lua_pushlightuserdata(L, const_cast<std::uint8_t*>(data));
    lua_setfield(L, -2, "data_raw");
    const std::string before(reinterpret_cast<const char*>(modified), size);
    const bool was_blocked = blocked;
    lua_pushlstring(L, before.data(), before.size());
    lua_setfield(L, -2, "data_modified");
    lua_pushlightuserdata(L, modified); // the addon may write it through the FFI
    lua_setfield(L, -2, "data_modified_raw");
    lua_pushinteger(L, chunk_size);
    lua_setfield(L, -2, "chunk_size");
    if (chunk != nullptr) {
        lua_pushlstring(L, reinterpret_cast<const char*>(chunk), chunk_size);
        lua_setfield(L, -2, "chunk_data");
        lua_pushlightuserdata(L, const_cast<std::uint8_t*>(chunk));
        lua_setfield(L, -2, "chunk_data_raw");
    }
    lua_pushboolean(L, injected);
    lua_setfield(L, -2, "injected");
    lua_pushboolean(L, blocked);
    lua_setfield(L, -2, "blocked");
    lua_pushvalue(L, -1);
    const int event = luaL_ref(L, LUA_REGISTRYINDEX);
    std::string error;
    if (call(addon, 2, 1, error)) {
        blocked = blocked || lua_toboolean(L, -1) != 0;
        lua_pop(L, 1);
        lua_rawgeti(L, LUA_REGISTRYINDEX, event);
        lua_getfield(L, -1, "blocked");
        blocked = blocked || lua_toboolean(L, -1) != 0;
        lua_pop(L, 1);
        // a new data_modified string: what the game gets (or sends), as far as the packet goes
        lua_getfield(L, -1, "data_modified");
        size_t length = 0;
        const char* now = lua_tolstring(L, -1, &length);
        if (now != nullptr && (length != before.size() || std::memcmp(now, before.data(), length) != 0))
            std::memcpy(modified, now, length < size ? length : size);
        lua_pop(L, 2);
    } else {
        report_call(addon, incoming ? "packet_in" : "packet_out", error);
    }
    addon_trace::packet_delivered(addon.context.name, incoming, id, size, blocked && !was_blocked);
    if (incoming && blocked && !was_blocked)
        packetwatch::blocked("addon " + addon.context.name, id);
    if (!addon.crashed)
        luaL_unref(L, LUA_REGISTRYINDEX, event);
    return blocked;
}

HMODULE self()
{
    HMODULE module = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&self), &module);
    return module;
}

// Pushes one of the Lua scripts built into VanaCore.Core.dll as a function.
bool load_script(lua_State* L, int id, const char* name, std::string& error)
{
    const HMODULE module = self();
    const HRSRC found = FindResourceW(module, MAKEINTRESOURCEW(id), MAKEINTRESOURCEW(10)); // RT_RCDATA
    const HGLOBAL data = found != nullptr ? LoadResource(module, found) : nullptr;
    const char* bytes = data != nullptr ? static_cast<const char*>(LockResource(data)) : nullptr;
    if (bytes == nullptr) {
        error = std::string(name) + " is missing from VanaCore.Core.dll";
        return false;
    }
    if (luaL_loadbuffer(L, bytes, SizeofResource(module, found), name) != 0) {
        error = lua_tostring(L, -1);
        lua_pop(L, 1);
        return false;
    }
    return true;
}

bool read_file(const std::string& path, std::string& contents)
{
    HANDLE file = CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return false;
    LARGE_INTEGER size{};
    bool ok = GetFileSizeEx(file, &size) && size.QuadPart < 64 * 1024 * 1024;
    if (ok) {
        contents.resize(static_cast<size_t>(size.QuadPart));
        DWORD read = 0;
        ok = contents.empty() || (ReadFile(file, contents.data(), static_cast<DWORD>(contents.size()), &read, nullptr) &&
                                  read == contents.size());
    }
    CloseHandle(file);
    return ok;
}

// Standard libraries and VanaCore's native API, in a protected call (they allocate).
int open_libraries(lua_State* L)
{
    auto* addon = static_cast<Addon*>(lua_touserdata(L, 1));
    luaL_openlibs(L);
    lua_api::open(L, addon->context);
    return 0;
}

using lua_api::Flavour;

void remove(Addon& addon);

bool g_command_injected = false; // the command line being run came from a script, macro or addon, not typed
bool g_in_frame = false;          // on_frame is running: the game's Present
bool g_login_line = false;        // the script line being run is boot.txt's, held for a login (ScriptLine::login)
struct FrameCommand {
    std::vector<std::string> parts;
    bool typed; // typed in the chat line, not run by a script, macro or addon
};
std::deque<FrameCommand> g_frame_commands; // /addon load and reload run outside a frame, for the next one
bool valid_name(std::string_view name);

const char* kind(Flavour flavour)
{
    switch (flavour) {
    case Flavour::Ashita: return "Ashita";
    case Flavour::Windower: return "Windower";
    default: return "VanaCore";
    }
}

// Which API an addon's main file is written for, by the names it uses: vana.* is VanaCore's own;
// otherwise windower.* and _addon.* against ashita.*, AshitaCore and addon.*, the most used winning.
// `otherwise` (its folder's kind) when it uses none of them. An addon in Ashita's or Windower's folder is
// never VanaCore's: a vana.* there is its own (fancycompass's `local vana = require('lib.vana')`).
Flavour detect(const std::string& source, Flavour otherwise)
{
    const auto count = [&](std::string_view word) {
        int found = 0;
        for (size_t at = source.find(word); at != std::string::npos; at = source.find(word, at + 1)) {
            const char before = at == 0 ? ' ' : source[at - 1];
            if (!std::isalnum(static_cast<unsigned char>(before)) && before != '_' && before != '.')
                ++found;
        }
        return found;
    };
    if (otherwise == Flavour::VanaCore && count("vana.") > 0)
        return Flavour::VanaCore;
    const int windower = count("windower.") + count("_addon.");
    const int ashita = count("ashita.") + count("AshitaCore") + count("addon.");
    if (windower == 0 && ashita == 0)
        return otherwise;
    return windower > ashita ? Flavour::Windower : Flavour::Ashita;
}

// Makes the addon's state and runs lua/vana.lua, its kind's compatibility script and `source` (the
// addon's file). False with what went wrong in `error`.
bool start(Addon& addon, const std::string& file, const std::string& source, std::string& error)
{
    lua_State* L = addon.L;
    if (lua_cpcall(L, &open_libraries, &addon) != 0) {
        error = lua_tostring(L, -1);
        lua_pop(L, 1);
        return false;
    }
    // lua/vana.lua gets the native functions it needs, and returns the hooks VanaCore calls.
    if (!load_script(L, IDR_LUA_VANA, "@vana.lua", error))
        return false;
    lua_newtable(L);
    lua_pushcfunction(L, &native_report);
    lua_setfield(L, -2, "report");
    lua_pushcfunction(L, &native_subscribe);
    lua_setfield(L, -2, "subscribe");
    if (!call(addon, 1, 1, error))
        return false;
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        error = "vana.lua didn't return its hooks";
        return false;
    }
    addon.hooks = luaL_ref(L, LUA_REGISTRYINDEX);

    // lua/settings.lua: vana.settings and vana.json, given the hooks (it saves after the addon's own unload handlers) and
    // the addon's name as its folder spells it (settings\addons\Sidekick, not sidekick)
    if (!load_script(L, IDR_LUA_SETTINGS, "@settings.lua", error))
        return false;
    lua_rawgeti(L, LUA_REGISTRYINDEX, addon.hooks);
    lua_pushlstring(L, addon.display.data(), addon.display.size());
    if (!call(addon, 2, 0, error))
        return false;

    // lua/net.lua: vana.promise, vana.http, vana.ws and vana.async, given the hooks (its pump runs each frame) and the
    // engine's calls for this addon (lua_net.cpp)
    if (!load_script(L, IDR_LUA_NET, "@net.lua", error))
        return false;
    lua_rawgeti(L, LUA_REGISTRYINDEX, addon.hooks);
    lua_net::push(L);
    if (!call(addon, 2, 0, error))
        return false;

    if (addon.context.flavour == Flavour::Ashita &&
        (!load_script(L, IDR_LUA_ASHITA, "@compat\\ashita.lua", error) || !call(addon, 0, 0, error)))
        return false;
    if (addon.context.flavour == Flavour::Windower &&
        (!load_script(L, IDR_LUA_WINDOWER, "@compat\\windower.lua", error) || !call(addon, 0, 0, error)))
        return false;

    // Named by its full path, as loadfile would: some addons find their files from it
    // (debug.getinfo(...).source).
    const std::string chunk = "@" + file;
    if (luaL_loadbuffer(L, source.data(), source.size(), chunk.c_str()) != 0) {
        error = lua_tostring(L, -1);
        lua_pop(L, 1);
        return false;
    }
    return call(addon, 0, 0, error);
}

// The addon's own `<table>.<field>` string (addon.version, _addon.author, ...), or empty.
std::string addon_field(Addon& addon, const char* field)
{
    std::string value;
    lua_getglobal(addon.L, addon.context.flavour == Flavour::Windower ? "_addon" : "addon");
    if (lua_istable(addon.L, -1)) {
        lua_getfield(addon.L, -1, field);
        if (lua_type(addon.L, -1) == LUA_TSTRING || lua_type(addon.L, -1) == LUA_TNUMBER)
            value = lua_tostring(addon.L, -1);
        lua_pop(addon.L, 1);
    }
    lua_pop(addon.L, 1);
    return value;
}

// Where addons live: a folder per kind under <VanaCore>\addons, searched in this order when a load
// doesn't say which. Windower's is laid out as Windower's own addons folder (its addons beside
// libs\ and res\), which Windower's libraries rely on.
struct Source {
    Flavour kind;
    const char* folder; // under VanaCore's folder
    const char* word;   // as a command names it: /addon load aspect windower
};
constexpr Source kSources[] = {
    {Flavour::VanaCore, "addons\\vanacore\\", "vanacore"},
    {Flavour::Ashita, "addons\\ashita\\", "ashita"},
    {Flavour::Windower, "addons\\windower\\", "windower"},
};

const Source* source_named(std::string_view word)
{
    for (const Source& source : kSources)
        if (word == source.word)
            return &source;
    return nullptr;
}

const Source& source_of(Flavour kind)
{
    for (const Source& source : kSources)
        if (source.kind == kind)
            return source;
    return kSources[0];
}

// The source with <name>\<name>.lua (only `only`, if given), and the name as its folder spells it
// ("DistancePlus" for /addon load distanceplus). Nullptr if none has it.
const Source* locate(const std::string& name, const Source* only, std::string& display)
{
    for (const Source& source : kSources) {
        if (only != nullptr && &source != only)
            continue;
        WIN32_FIND_DATAA found{};
        const HANDLE handle = FindFirstFileA((g_install + source.folder + name).c_str(), &found);
        if (handle == INVALID_HANDLE_VALUE)
            continue;
        FindClose(handle);
        const std::string file = g_install + source.folder + found.cFileName + "\\" + found.cFileName + ".lua";
        if ((found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0 && GetFileAttributesA(file.c_str()) != INVALID_FILE_ATTRIBUTES) {
            display = found.cFileName;
            return &source;
        }
    }
    return nullptr;
}

// The zone-in packet (0x00A) of the login boot.txt's addons waited for ([addons] delay.login), to an Ashita addon that
// loaded after it: on Ashita it would have loaded at the title screen and seen it, and logincmd (which runs a
// character's profile when a zone-in names the character) does nothing without it. Only while the character is still in
// the zone it names: after a zone-out (0x00B) the next zone-in reaches the addon as any packet does. It comes as the game
// got it, not injected; the game has it already, so blocking or changing it does nothing.
void replay_zone_in(Addon& addon)
{
    double in_at = 0;
    double out_at = 0;
    const std::string packet = packets::last(true, 0x00A, &in_at);
    packets::last(true, 0x00B, &out_at);
    if (packet.empty() || out_at > in_at || !addon.packets || addon.unload_requested || addon.crashed)
        return;
    log::write(L"Addons: %hs loaded after the login ([addons] delay.login): it gets the login's zone-in packet (0x00A) now",
               addon.display.c_str());
    // a copy with zeros after it, as packets.cpp hands them out (kPluginPacket)
    std::vector<std::uint8_t> data(0x200, 0);
    std::memcpy(data.data(), packet.data(), packet.size());
    std::vector<std::uint8_t> modified = data;
    const auto size = static_cast<std::uint32_t>(packet.size());
    raise_packet(addon, true, 0x00A, size, data.data(), modified.data(), size, data.data(), false, false);
}

// Loads addon `name` from `only`'s folder, or the first that has it.
bool load(const std::string& name, const Source* only = nullptr)
{
    if (!valid_name(name)) {
        say_error("\"" + name + "\" isn't an addon name");
        return false;
    }
    for (Held& held : g_held) {
        if (held.name == lower(name)) {
            held.wanted = true;
            say(name + " isn't loaded: " + held.why);
            log::write(L"Addons: %hs isn't loaded: %hs", name.c_str(), held.why.c_str());
            return false;
        }
    }
    if (Addon* loaded = find(name)) {
        say(loaded->display + " is already loaded (" + kind(loaded->context.flavour) + "; /addon reload " + name +
            " to restart it)");
        return false;
    }
    std::string display;
    const Source* source = locate(name, only, display);
    if (source == nullptr) {
        if (only != nullptr)
            say_error(std::string("No ") + kind(only->kind) + " addon " + name + ": " + only->folder + name + "\\" + name +
                      ".lua isn't there");
        else
            say_error("No addon " + name + " in addons\\vanacore, addons\\ashita or addons\\windower");
        log::write(L"Addons: %hs not found under %hsaddons", name.c_str(), g_install.c_str());
        return false;
    }
    const std::string folder = g_install + source->folder + display + "\\";
    const std::string file = folder + display + ".lua";
    std::string source_text;
    if (!read_file(file, source_text)) {
        say_error(std::string("Couldn't read ") + source->folder + display + "\\" + display + ".lua");
        return false;
    }
    if (source_text.starts_with("\xEF\xBB\xBF")) // UTF-8 byte order mark
        source_text.erase(0, 3);
    auto created = std::make_unique<Addon>();
    // The kind its code shows, or its folder's when the code doesn't say.
    const Flavour detected_flavour = detect(source_text, source->kind);
    if (!g_ashita_enabled && (source->kind == Flavour::Ashita || detected_flavour == Flavour::Ashita)) {
        const std::string msg = "[addons] ashita.enabled=0: Ashita compatibility is disabled in this profile; refusing to load Ashita addon '" + display + "'.";
        say_error(msg);
        log::write(L"Addons: %hs", msg.c_str());
        return false;
    }
    created->context = {detected_flavour, name, source->word, folder, g_install, g_settings_ini};
    created->display = display;
    created->source = source->kind;
    created->L = luaL_newstate();
    if (created->L == nullptr) {
        say_error("Couldn't load " + display + ": out of memory");
        return false;
    }
    Addon& addon = *created;
    g_addons.push_back(std::move(created));
    // free to change the game's interface (vana.gameui) from its first line; remove() puts back what it changed
    gameui::attach(gameui::key(source->word, name), display);
    const std::string label = std::string("(") + kind(addon.context.flavour) + " Addon) ";
    log::write(L"Addons: loading %hs (%hs addon)", file.c_str(), kind(addon.context.flavour));

    std::string error;
    bool loaded = start(addon, file, source_text, error);
    if (loaded) {
        raise(addon, "load");
        loaded = addon.errors == 0; // its load event failed; the error is shown already
    } else {
        log::write(L"Addons: %hs failed to start: %hs", display.c_str(), error.c_str());
        say_error(label + "Couldn't load " + display + ": " + chat_line(error));
    }
    if (!loaded) {
        addon.state = 0;
        remove(addon);
        say_error(display + " wasn't loaded");
        return false;
    }
    addon.state = 1;
    const std::string version = addon_field(addon, "version");
    const std::string author = addon_field(addon, "author");
    addon.version = version;
    addon.author = author;
    addon.description = addon_field(addon, "desc");
    addon.link = addon_field(addon, "link");
    std::string details = version.empty() ? "" : " " + version;
    if (!author.empty())
        details += " by " + author;
    log::write(L"Addons: loaded %hs%hs (%hs addon)", display.c_str(), details.c_str(), kind(addon.context.flavour));
    if (addon_trace::on(addon.context.name)) {
        std::string listens;
        const auto note = [&](bool listening, const char* what) {
            if (listening)
                listens += (listens.empty() ? "" : ", ") + std::string(what);
        };
        note(addon.packets, "packets");
        note(addon.text, "chat text");
        note(addon.mouse, "mouse");
        note(addon.key, "keys");
        note(addon.keys, "key state");
        note(addon.gamepad, "gamepad");
        addon_trace::loaded(addon.context.name, listens);
    }
    if (!g_silent)
        say(label + chat::colour(chat::kSuccess, "Loaded: ") + display + details);
    if (g_login_line && addon.context.flavour == Flavour::Ashita)
        replay_zone_in(addon);
    return true;
}

// What an addon left in another plugin goes with it: an addon that faded the Minimap plugin (minimapmon: opacity event
// 0x01, a command and six floats) is gone, so nothing would ever bring the map back. Put it to full, as an addon's own
// unload handler would have.
void restore_plugins(Addon& addon)
{
    if (!addon.minimap_dimmed || g_closing)
        return;
    addon.minimap_dimmed = false;
    const std::uint32_t command = 1;
    const float opacity[6] = {1, 1, 1, 1, 1, 1};
    std::string data(sizeof(command) + sizeof(opacity), '\0');
    std::memcpy(data.data(), &command, sizeof(command));
    std::memcpy(data.data() + sizeof(command), opacity, sizeof(opacity));
    plugins::raise_event("minimap", data);
    log::write(L"Addons: %hs left the minimap faded; its opacity is back to full", addon.context.name.c_str());
}

void unload(Addon& addon, bool quiet = false, std::string reason = {})
{
    const std::string display = addon.display;
    const std::string label = std::string("(") + kind(addon.context.flavour) + " Addon) ";
    addon.state = reason == "reloading" ? 5 : 4;
    if (!addon.crashed && !addon.unload_raised)
        raise(addon, "unload");
    restore_plugins(addon);
    const std::string traced_name = addon.context.name;
    remove(addon);
    log::write(L"Addons: unloaded %hs%hs%hs", display.c_str(), reason.empty() ? "" : ": ", reason.c_str());
    addon_trace::unloaded(traced_name, reason);
    if (!quiet && !g_silent)
        say(label + chat::colour(chat::kSuccess, "Unloaded: ") + display + (reason.empty() ? "" : " (" + reason + ")"));
}

void remove(Addon& addon)
{
    if (addon.draws) {
        addon.draws = false;
        --g_draw_listeners;
    }
    if (addon.scenes) {
        addon.scenes = false;
        --g_scene_listeners;
    }
    ui::destroy_owned(addon.context.name);
    gameui::release(gameui::key(addon.context.source, addon.context.name)); // the game's interface as it was before it
    groundmarks::release(addon.context.name); // vana.world.marks
    raise::game_on_top(gameui::key(addon.context.source, addon.context.name), false); // vana.ui.game_on_top
    soulspire::on_addon_unloaded(addon.context.name);
    if (addon.context.flavour == Flavour::Ashita)
        ashita::release_addon(addon.context.name);
    Fault fault;
    if (addon.crashed)
        log::write(L"Addons: %hs's Lua state is left as it is after its crash (its memory stays allocated)",
                   addon.context.name.c_str());
    else if (addon.L != nullptr && !guarded_close(addon.L, fault))
        log::write(L"Addons: %hs crashed while closing (%hs); its Lua state is left as it is", addon.context.name.c_str(),
                   describe(fault).c_str());
    addon.L = nullptr;
    for (auto it = g_addons.begin(); it != g_addons.end(); ++it) {
        if (it->get() == &addon) {
            g_addons.erase(it);
            return;
        }
    }
}

bool valid_name(std::string_view name)
{
    if (name.empty() || name.size() > 64)
        return false;
    for (const char c : name)
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_' && c != '-' && c != '.')
            return false;
    return name.find("..") == std::string_view::npos;
}

// Unloads addons that asked to go (or broke) while Lua was running.
void process_requests()
{
    if (g_depth != 0)
        return;
    for (size_t i = 0; i < g_addons.size();) {
        if (g_addons[i]->unload_requested)
            unload(*g_addons[i], false, g_addons[i]->unload_reason);
        else
            ++i;
    }
}

std::vector<std::string> words(std::string_view text)
{
    std::vector<std::string> out;
    size_t at = 0;
    while (at < text.size()) {
        while (at < text.size() && std::isspace(static_cast<unsigned char>(text[at])))
            ++at;
        size_t end = at;
        while (end < text.size() && !std::isspace(static_cast<unsigned char>(text[end])))
            ++end;
        if (end > at)
            out.emplace_back(text.substr(at, end - at));
        at = end;
    }
    return out;
}

// An addon's state by the name Ashita's /addon info gives it (Addon::state).
const char* state_name(const Addon& addon)
{
    if (addon.crashed)
        return "Error";
    switch (addon.state) {
    case 0: return "Error";
    case 1: return "Ok";
    case 2: return "Released";
    case 3: return "Loading";
    case 4: return "Unloading";
    case 5: return "Reloading";
    default: return "Unknown State";
    }
}

// /addon exec <name> <lua>: the Lua run in the addon's state (what it prints goes to the chat log, as its own does).
void exec(Addon& addon, const std::string& code)
{
    if (addon.crashed) {
        say_error(addon.display + " crashed: its Lua can't run");
        return;
    }
    const std::lock_guard lock(g_lock);
    lua_State* L = addon.L;
    std::string error;
    if (luaL_loadbuffer(L, code.data(), code.size(), "=/addon exec") != 0) {
        error = lua_tostring(L, -1);
        lua_pop(L, 1);
    } else if (call(addon, 0, 0, error)) {
        error.clear();
    }
    if (!error.empty())
        say_error(addon.display + ": " + chat_line(error));
    process_requests();
}

// /addon info <name>: what Ashita's shows.
void addon_info(Addon& addon)
{
    const auto line = [](const char* label, const std::string& value) {
        if (!value.empty())
            say(std::string(label) + value);
    };
    say(std::string("Addon Information: ") + addon.display + " (" + kind(addon.context.flavour) + ")");
    line("File: ", addon.context.folder + addon.display + ".lua");
    if (!addon.crashed) {
        line("Version: ", addon_field(addon, "version"));
        line("Author: ", addon_field(addon, "author"));
        line("Description: ", addon_field(addon, "desc"));
        line("Link: ", addon_field(addon, "link"));
        char memory[32];
        std::snprintf(memory, sizeof(memory), "%.2f MB",
                      (lua_gc(addon.L, LUA_GCCOUNT, 0) * 1024.0 + lua_gc(addon.L, LUA_GCCOUNTB, 0)) / (1024.0 * 1024.0));
        line("Memory: ", memory);
    }
    line("State: ", state_name(addon));
}

// /addon link <name>: its addon.link (a web address) in the browser.
void addon_link(Addon& addon)
{
    const std::string link = addon.crashed ? std::string() : addon_field(addon, "link");
    if (link.rfind("http://", 0) != 0 && link.rfind("https://", 0) != 0) {
        say_error("Addon '" + addon.display + "' does not have a homepage link.");
        return;
    }
    ShellExecuteA(nullptr, "open", link.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

// /addon load|unload|reload <name>... [vanacore|ashita|windower], /addon list; and Ashita's others: silent [0|1],
// unloadall (all but VanaCore's own, which are its interface), kill <name> (unloaded without its unload event),
// exec <name> <lua>, info <name>, link <name>.
bool is_required(std::string_view name, bool plugin); // below, with the scripts

// The addons that load at start without a boot.txt line: VanaCore's window (/vana), which [addons] vana=0 switches off
// (default 1); the key binds window (keybinds), which is always on: /addon unload leaves it alone (reload works); and
// VanaStore (vanastore), the addon store, which [addons] vanastore=0 switches off like /vana. VanaStore isn't in this
// repository: CatsAndBoats/cexi-addons publishes it into the launcher's VanaCore build (addons\vanacore\vanastore), and
// an install without it skips it. Every other one, freecam and loadscreen too, is boot.txt's like any addon: the
// default boot.txt loads them for a new install, and nothing adds them to an existing one (they're on once switched on).
constexpr const char* kOwnDefaults[] = {"vana", "keybinds", "vanastore"};
constexpr const char* kOwnAlwaysOn[] = {"keybinds"};

bool is_own_default(std::string_view name)
{
    for (const char* each : kOwnDefaults)
        if (name == each)
            return true;
    return false;
}

bool is_always_on(std::string_view name)
{
    for (const char* each : kOwnAlwaysOn)
        if (name == each)
            return true;
    return false;
}

// Writes `contents` to `path` through a temporary file beside it: the old file stays whole if the write fails.
bool write_script_file(const std::string& path, const std::string& contents)
{
    const std::string temp = path + ".tmp";
    HANDLE file = CreateFileA(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return false;
    DWORD written = 0;
    const bool ok = contents.empty() || (WriteFile(file, contents.data(), static_cast<DWORD>(contents.size()), &written, nullptr) &&
                                         written == contents.size());
    CloseHandle(file);
    if (!ok || !MoveFileExA(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        DeleteFileA(temp.c_str());
        return false;
    }
    return true;
}

// scripts\boot.txt follows a typed /addon load or /addon unload, as it follows the /vana window's switches (vana.lua's
// persist): on adds a `/addon load <name> <kind>` line unless one loads it already; off takes the name out of each line
// that does. `name` is lower case. The ones that load at start by themselves (kOwnDefaults) and CatsEyeXI's required
// addon aren't boot.txt's. `plugin`: the `/plugin load` lines instead (only off: a typed /plugin unload takes the plugin
// out).
void persist_boot(const std::string& name, bool on, bool plugin = false)
{
    if (!plugin && is_always_on(name))
        return;
    // VanaCore's window and VanaStore load by themselves at start, unless switched off: [addons] <name>=0 remembers
    // that. Off also takes out a boot.txt line from before (VanaStore switched on before it loaded by itself), which
    // would load it again at login
    if (!plugin && is_own_default(name)) {
        if (!g_settings_ini.empty())
            ini::set(g_settings_ini, L"addons", std::wstring(name.begin(), name.end()).c_str(), on ? L"1" : L"0");
        if (on)
            return;
    }
    if (plugin && on)
        return;
    if (is_required(name, plugin))
        return;
    const std::string word = plugin ? "plugin" : "addon";
    const std::string path = g_install + "scripts\\boot.txt";
    std::string text;
    const bool had = read_file(path, text);
    if (!had && !on)
        return;
    const bool bom = text.starts_with("\xEF\xBB\xBF");
    if (bom)
        text.erase(0, 3);
    const bool crlf = text.find("\r\n") != std::string::npos;
    std::vector<std::string> lines;
    for (size_t start = 0; start < text.size();) {
        size_t end = text.find('\n', start);
        if (end == std::string::npos)
            end = text.size();
        std::string line = text.substr(start, end - start);
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        lines.push_back(std::move(line));
        start = end + 1;
    }
    bool changed = false;
    bool listed = false;
    for (size_t i = lines.size(); i-- > 0;) {
        const std::vector<std::string> parts = words(lines[i]);
        if (parts.size() < 3 || (lower(parts[0]) != "/" + word && lower(parts[0]) != "//" + word) || lower(parts[1]) != "load")
            continue;
        std::vector<std::string> kept;
        bool found = false;
        for (size_t at = 2; at < parts.size(); ++at) {
            if (lower(parts[at]) == name)
                found = true;
            else
                kept.push_back(parts[at]);
        }
        if (!found)
            continue;
        listed = true;
        if (on)
            continue;
        if (kept.empty() || (kept.size() == 1 && source_named(lower(kept[0])) != nullptr)) {
            lines.erase(lines.begin() + static_cast<std::ptrdiff_t>(i)); // nothing but the kind is left
        } else {
            std::string rest;
            for (const std::string& item : kept)
                rest += " " + item;
            lines[i] = "/" + word + " load" + rest;
        }
        changed = true;
    }
    if (on && !listed) {
        const Addon* addon = find(name);
        if (addon == nullptr)
            return;
        lines.push_back("/addon load " + addon->display + " " + source_of(addon->source).word);
        changed = true;
    }
    if (!changed)
        return;
    CreateDirectoryA((g_install + "scripts").c_str(), nullptr);
    std::string out = bom ? "\xEF\xBB\xBF" : "";
    for (const std::string& line : lines)
        out += line + (crlf ? "\r\n" : "\n");
    if (!write_script_file(path, out)) {
        log::write(L"Addons: couldn't write scripts\\boot.txt for %hs", name.c_str());
        return;
    }
    log::write(L"Addons: scripts\\boot.txt %hs %hs", on ? "now loads" : "no longer loads", name.c_str());
    if (!g_silent)
        say(name + (on ? " is in scripts\\boot.txt: it loads at the next start" : " is out of scripts\\boot.txt"));
}

// /addon unload of CatsEyeXI's required addon or keybinds (always on), said so; and an old boot.txt line loading one that
// loaded at start already (keybinds; vanastore, switched on before it loaded by itself), without a word. True: leave
// `name` as it is.
bool stays_as_is(const std::string& action, const std::string& name, bool loaded, bool typed)
{
    if (action == "unload" && is_required(name, false)) {
        say(name + " is required by SoulspireXI and stays loaded (/addon reload " + name + " reloads it)");
        return true;
    }
    if (action == "unload" && is_always_on(name)) {
        say(name + " is part of VanaCore and stays loaded (/addon reload " + name + " reloads it)");
        return true;
    }
    return action == "load" && is_own_default(name) && loaded && !typed;
}

void addon_command(const std::vector<std::string>& parts, bool typed)
{
    const std::string action = parts.size() > 1 ? lower(parts[1]) : "";
    if (action == "silent") {
        g_silent = parts.size() > 2 ? parts[2] != "0" : !g_silent;
        say(std::string("Silent addons is now: ") + (g_silent ? "Enabled" : "Disabled"));
        return;
    }
    if (action == "unloadall") {
        std::vector<Addon*> going;
        for (const auto& addon : g_addons)
            if (addon->context.flavour != Flavour::VanaCore && addon->source != Flavour::VanaCore)
                going.push_back(addon.get());
        for (auto it = going.rbegin(); it != going.rend(); ++it)
            unload(**it, false, "/addon unloadall");
        if (going.empty())
            say("No addons to unload (VanaCore's own stay)");
        return;
    }
    if (action == "exec" || action == "info" || action == "link" || action == "kill") {
        Addon* addon = parts.size() > 2 ? find(lower(parts[2])) : nullptr;
        if (parts.size() < 3 || (action == "exec" && parts.size() < 4)) {
            say(action == "exec" ? "/addon exec <name> <lua>" : "/addon " + action + " <name>");
            return;
        }
        if (addon == nullptr) {
            say_error("Failed to locate addon: " + parts[2]);
            return;
        }
        if (action == "exec") {
            const bool dev_enabled =
                !g_settings_ini.empty() && GetPrivateProfileIntW(L"developer", L"enabled", 0, g_settings_ini.c_str()) != 0;
            if (!dev_enabled) {
                say_error("/addon exec is disabled in player builds (set [developer] enabled=1 in vanacore.ini).");
                return;
            }
            // An addon's code runs at the game's Present, as its present event does (see below)
            if (!g_in_frame) {
                g_frame_commands.push_back({parts, typed});
                return;
            }
            std::string code;
            for (size_t i = 3; i < parts.size(); ++i)
                code += (i > 3 ? " " : "") + parts[i];
            exec(*addon, code);
        } else if (action == "info") {
            addon_info(*addon);
        } else if (action == "link") {
            addon_link(*addon);
        } else {
            addon->unload_raised = true; // forced: its unload event isn't raised
            unload(*addon, false, "killed");
        }
        return;
    }
    if (action == "list") {
        if (g_addons.empty()) {
            say("No addons are loaded");
            return;
        }
        std::string names;
        for (const auto& addon : g_addons)
            names += (names.empty() ? "" : ", ") + addon->display + " (" + kind(addon->context.flavour) + ")";
        say("Loaded: " + names);
        return;
    }
    std::vector<std::string> names;
    for (size_t i = 2; i < parts.size(); ++i)
        names.push_back(lower(parts[i]));
    // A last word naming a kind says where to load from: /addon load aspect windower.
    const Source* only = names.size() > 1 ? source_named(names.back()) : nullptr;
    if (only != nullptr)
        names.pop_back();
    if ((action != "load" && action != "unload" && action != "reload") || names.empty()) {
        say("/addon load <name> [vanacore|ashita|windower], /addon unload <name>, /addon reload <name>, /addon list, "
            "/addon unloadall, /addon kill <name>, /addon exec <name> <lua>, /addon info <name>, /addon link <name>, "
            "/addon silent [0|1]");
        return;
    }
    // An addon starts at the game's Present, as its present event runs. Typed in the chat line it's mid-frame, the
    // device's viewport the interface's (smaller than the screen with a lower resolution.ui): an addon that keeps the
    // screen's size as it loads (kazenoeye) put everything that much up and left.
    if (!g_in_frame && action != "unload") {
        g_frame_commands.push_back({parts, typed});
        return;
    }
    for (const std::string& name : names) {
        Addon* addon = find(name);
        if (stays_as_is(action, name, addon != nullptr, typed))
            continue;
        if (action == "load") {
            if (load(name, only) && typed)
                persist_boot(name, true);
        } else if (addon == nullptr) {
            say(name + " isn't loaded");
            if (action == "reload")
                load(name, only);
            else if (typed)
                persist_boot(name, false); // one that failed to load or was killed still leaves boot.txt
        } else {
            // A reload comes from the same folder unless it says otherwise.
            const Source* from = only != nullptr ? only : &source_of(addon->source);
            unload(*addon, false,
                   action == "reload" ? "reloading"
                   : g_command_injected ? "/addon unload, run by a script, macro or addon"
                                        : "/addon unload");
            if (action == "reload")
                load(name, from);
            else if (typed)
                persist_boot(name, false);
        }
    }
}

////////////////////////////////////////////////////////////////////////////////////////////////////
// Scripts: <VanaCore>\scripts\<name>.txt, a command per line. boot.txt runs on the first frame.

struct ScriptLine {
    ULONGLONG at; // when it's due (GetTickCount64)
    std::string line;
    bool login = false; // boot.txt's, held for a login (Lines::Rest), or a script one of those ran: see replay_zone_in
};
std::deque<ScriptLine> g_script_lines; // queued, in order
bool g_booted = false;
bool g_boot_pending = false;   // boot.txt waits for a character to log in ([addons] delay.login)
ULONGLONG g_next_login_check = 0;
bool g_caches_checked = false; // the character's sort-order caches are looked at once per game start, at its login
ULONGLONG g_next_cache_check = 0;

bool run_line(std::string_view line);

// Which of a script's lines run: all of them; only its /plugin lines (no waits, they run now); or all but those. boot.txt
// is run in the last two when it waits for a login: plugins load as the game starts, as they do on Ashita (CatsEyeXI's
// cexidats and cexislots patch the client, and the client reads the character's inventory and its sort-order caches at
// login: with those patched after it, custom items end up in the caches again at every login), and addons wait.
enum class Lines { All, Plugins, Rest };

// Server-required extensions forced on at boot. SoulspireXI leaves this empty so Phase 1 minimal-login testing
// and progressive certification control plugin/addon loading explicitly via scripts\boot.txt.
constexpr std::initializer_list<const char*> kRequiredPlugins = {};
constexpr std::initializer_list<const char*> kRequiredAddons = {};

bool is_required(std::string_view name, bool plugin)
{
    if (plugin) {
        for (const char* each : kRequiredPlugins)
            if (name == each)
                return true;
        return false;
    }
    for (const char* each : kRequiredAddons)
        if (name == each)
            return true;
    return false;
}

// Where `name` comes in the required order (plugins, then addons); past the end if it isn't required.
int required_rank(std::string_view name)
{
    int rank = 0;
    for (const char* each : kRequiredPlugins) {
        if (name == each)
            return rank;
        ++rank;
    }
    for (const char* each : kRequiredAddons) {
        if (name == each)
            return rank;
        ++rank;
    }
    return rank;
}

// A script line that loads a required extension ("/plugin load cexidats ashita", "//addon load customcolors"): its
// name in lower case, or empty.
std::string required_load(const std::string& line, bool& plugin)
{
    const std::vector<std::string> parts = words(line);
    if (parts.size() < 3 || lower(parts[1]) != "load")
        return {};
    const std::string command = lower(parts[0]);
    plugin = command == "/plugin" || command == "//plugin";
    if (!plugin && command != "/addon" && command != "//addon")
        return {};
    const std::string name = lower(parts[2]);
    return is_required(name, plugin) ? name : std::string();
}

// boot.txt's lines queued from `from` on (at `at`, waits after): the ones loading required extensions go first, in the
// required order and before any wait, and a required one it doesn't load is put in front when its file is there
// (`which` says whether plugins, addons or both are being run).
void required_first(size_t from, ULONGLONG at, Lines which)
{
    std::vector<ScriptLine> lines(g_script_lines.begin() + static_cast<std::ptrdiff_t>(from), g_script_lines.end());
    g_script_lines.erase(g_script_lines.begin() + static_cast<std::ptrdiff_t>(from), g_script_lines.end());
    std::vector<std::pair<int, std::string>> first; // rank, line
    std::vector<ScriptLine> rest;
    std::vector<std::string> seen;
    for (ScriptLine& line : lines) {
        bool plugin = false;
        const std::string name = required_load(line.line, plugin);
        if (name.empty() || std::find(seen.begin(), seen.end(), name) != seen.end()) {
            rest.push_back(std::move(line));
            continue;
        }
        seen.push_back(name);
        first.emplace_back(required_rank(name), std::move(line.line));
    }
    if (which != Lines::Rest)
        for (const char* name : kRequiredPlugins)
            if (std::find(seen.begin(), seen.end(), name) == seen.end() && plugins::present(name)) {
                log::write(L"Addons: scripts\\boot.txt doesn't load %hs, which SoulspireXI requires: it's loaded first", name);
                first.emplace_back(required_rank(name), std::string("/plugin load ") + name);
            }
    if (which != Lines::Plugins)
        for (const char* name : kRequiredAddons) {
            std::string display;
            if (std::find(seen.begin(), seen.end(), name) == seen.end() && locate(name, nullptr, display) != nullptr) {
                log::write(L"Addons: scripts\\boot.txt doesn't load %hs, which SoulspireXI requires: it's loaded first", name);
                first.emplace_back(required_rank(name), std::string("/addon load ") + name);
            }
        }
    std::stable_sort(first.begin(), first.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    for (auto& [rank, line] : first)
        g_script_lines.push_back({at, std::move(line)});
    for (ScriptLine& line : rest)
        g_script_lines.push_back(std::move(line));
}

// Queues the script's commands. "/wait <seconds>" (or "//wait") delays the ones after it; lines
// starting with '#' are comments.
// Ashita's script arguments: "%1%" in a line is the first word after the script's name (/exec name a b), "%2%" the
// second, and so on. False, with `error`, when a line asks for one that wasn't given (`what`: "script", or "script string"
// for /execstr's, as Ashita says it).
bool substitute_arguments(std::string& line, const std::vector<std::string>& args, std::string& error,
                          std::string_view what = "script")
{
    size_t at = 0;
    while ((at = line.find('%', at)) != std::string::npos) {
        const size_t end = line.find('%', at + 1);
        if (end == std::string::npos)
            break;
        const std::string number = line.substr(at + 1, end - at - 1);
        if (number.empty() || number.size() > 6 || number.find_first_not_of("0123456789") != std::string::npos) {
            ++at;
            continue;
        }
        const size_t index = std::stoul(number);
        if (index == 0 || index > args.size()) {
            error = "Argument error inside of " + std::string(what) + ": %" + number + "% was asked for, and " +
                    (args.empty() ? std::string("no arguments were given") : std::to_string(args.size()) + " were given");
            return false;
        }
        line.replace(at, end - at + 1, args[index - 1]);
        at += args[index - 1].size();
    }
    return true;
}

// The script scripts\<name>.txt as text; false when there's no such script
bool read_script(std::string name, std::string& text, bool quiet_if_missing)
{
    if (name.find('.') == std::string::npos)
        name += ".txt";
    if (!valid_name(name)) {
        say_error("\"" + name + "\" isn't a script name");
        return false;
    }
    if (!read_file(g_install + "scripts\\" + name, text)) {
        if (!quiet_if_missing)
            say_error("No script scripts\\" + name);
        log::write(L"Scripts: %hsscripts\\%hs not found", g_install.c_str(), name.c_str());
        return false;
    }
    if (text.starts_with("\xEF\xBB\xBF"))
        text.erase(0, 3);
    return true;
}

// How long a wait line holds up the lines after it, in milliseconds, as Ashita has them: "/wait <seconds>", "/sleep" and
// "/pause" alike (a second with no number), Windower's "//wait <seconds>". -1 when the line isn't one, or is /sleep or
// /pause switched off (Client > Commands: commands.hpp), which then run as any other line
LONGLONG wait_of(const std::vector<std::string>& parts)
{
    if (parts.empty())
        return -1;
    const std::string command = lower(parts[0]);
    if ((command != "//wait" && command != "/wait" && command != "/sleep" && command != "/pause") || !commands::on(command))
        return -1;
    const double seconds = parts.size() > 1 ? std::atof(parts[1].c_str()) : 1.0;
    return static_cast<LONGLONG>((seconds > 0 ? seconds : 0) * 1000);
}

// Queues a script's lines from `text`, after `at` (waits move it): "/wait", "/sleep" and "/pause" delay the ones after,
// "/include <name>" puts another script's lines in its place, "%1%" is an argument. Lines starting with '#' are comments.
bool queue_script_text(const std::string& text, const std::vector<std::string>& args, Lines which, int depth, ULONGLONG& at,
                       int& count)
{
    size_t start = 0;
    while (start < text.size()) {
        size_t end = text.find('\n', start);
        if (end == std::string::npos)
            end = text.size();
        std::string line = text.substr(start, end - start);
        start = end + 1;
        const size_t first = line.find_first_not_of(" \t\r");
        if (first == std::string::npos || line[first] == '#')
            continue;
        line = line.substr(first, line.find_last_not_of(" \t\r") - first + 1);
        std::string error;
        if (!substitute_arguments(line, args, error)) {
            say_error(error);
            return false;
        }
        const std::vector<std::string> parts = words(line);
        const std::string command = lower(parts[0]);
        const bool plugin_line = command == "/plugin" || command == "//plugin";
        if ((which == Lines::Plugins && !plugin_line) || (which == Lines::Rest && plugin_line))
            continue;
        if (const LONGLONG wait = wait_of(parts); wait >= 0) {
            at += static_cast<ULONGLONG>(wait);
            continue;
        }
        if (command == "/include") {
            std::string included;
            if (parts.size() < 2 || depth >= 8) {
                say_error(parts.size() < 2 ? "/include <script>" : "/include: scripts include each other too deep");
                continue;
            }
            if (read_script(parts[1], included, false) && !queue_script_text(included, args, which, depth + 1, at, count))
                return false;
            continue;
        }
        g_script_lines.push_back({at, line});
        ++count;
    }
    return true;
}

// Queues the script's commands. "/exec <name> [args...]": see queue_script_text.
bool run_script(std::string name, bool quiet_if_missing, Lines which = Lines::All,
                const std::vector<std::string>& args = {})
{
    std::string text;
    if (!read_script(name, text, quiet_if_missing))
        return false;
    ULONGLONG at = GetTickCount64();
    if (!g_script_lines.empty() && g_script_lines.back().at > at)
        at = g_script_lines.back().at; // after another script's waiting lines
    const ULONGLONG start = at;
    const size_t from = g_script_lines.size();
    int count = 0;
    const bool ok = queue_script_text(text, args, which, 0, at, count);
    const std::string lowered = lower(name);
    if (lowered == "boot.txt" || lowered == "boot")
        required_first(from, start, which);
    if (which == Lines::Rest || g_login_line)
        for (size_t i = from; i < g_script_lines.size(); ++i)
            g_script_lines[i].login = true;
    log::write(L"Scripts: scripts\\%hs, %d commands", name.c_str(), count);
    return ok;
}

// Runs the queued script lines that are due.
void run_scripts()
{
    const ULONGLONG now = GetTickCount64();
    while (!g_script_lines.empty() && g_script_lines.front().at <= now) {
        const std::string line = g_script_lines.front().line;
        const bool login = g_script_lines.front().login;
        g_script_lines.pop_front();
        const bool outer = g_login_line;
        g_login_line = login;
        run_line(line);
        g_login_line = outer;
    }
}

// A line queued after everything queued so far, waits included
void queue_after(const std::string& line)
{
    ULONGLONG at = GetTickCount64();
    if (!g_script_lines.empty() && g_script_lines.back().at > at)
        at = g_script_lines.back().at;
    g_script_lines.push_back({at, line});
}

// A line due at `at`, after the queued lines due by then: the queue stays in time order
void queue_at(ULONGLONG at, std::string line)
{
    const auto place = std::upper_bound(g_script_lines.begin(), g_script_lines.end(), at,
                                        [](ULONGLONG due, const ScriptLine& queued) { return due < queued.at; });
    g_script_lines.insert(place, {at, std::move(line)});
}

// Commands in order, as Ashita runs a bind's or a script string's: "/wait <seconds>" among them ("/sleep", "/pause"; a
// second with no number) holds up the ones after it. Those before any wait run now (at the next frame when `queued`), the
// rest at their time, without waiting for a script's lines.
void run_lines(const std::vector<std::string>& lines, bool queued)
{
    const ULONGLONG now = GetTickCount64();
    ULONGLONG at = now;
    for (const std::string& line : lines) {
        if (const LONGLONG wait = wait_of(words(line)); wait >= 0)
            at += static_cast<ULONGLONG>(wait);
        else if (at > now)
            queue_at(at, line);
        else if (queued)
            chat::run(1, line);
        else
            chat::execute(1, line);
    }
}

// Ashita's ExecuteScriptString, which its /execstr runs: `text` split at each ';', each part trimmed (empty ones and ones
// starting with '#' skipped) and "%1%" in it the first of `args`, then run as run_lines runs them. An argument that wasn't
// given stops it there.
void run_script_string(const std::string& text, const std::vector<std::string>& args, bool queued)
{
    std::vector<std::string> lines;
    size_t start = 0;
    while (start <= text.size()) {
        size_t end = text.find(';', start);
        if (end == std::string::npos)
            end = text.size();
        std::string part = text.substr(start, end - start);
        start = end + 1;
        const size_t first = part.find_first_not_of(" \t\r\n");
        if (first == std::string::npos || part[first] == '#')
            continue;
        part = part.substr(first, part.find_last_not_of(" \t\r\n") - first + 1);
        std::string error;
        if (!substitute_arguments(part, args, error, "script string")) {
            say_error(error);
            break;
        }
        lines.push_back(std::move(part));
    }
    run_lines(lines, queued);
}

// //keybinds apply: scripts\keybinds.txt, the binds the keybinds window saved (its changes to what boot.txt and the addons
// bound), run without a word in the chat log. Queued after boot.txt's lines at every start.
void apply_saved_binds()
{
    binds::note_base(); // what boot.txt and the addons bound: the keybinds window /unbinds only those in the file
    std::string text;
    if (!read_file(g_install + "scripts\\keybinds.txt", text)) {
        log::write(L"Binds: no scripts\\keybinds.txt (the keybinds window makes it)");
        return;
    }
    if (text.starts_with("\xEF\xBB\xBF"))
        text.erase(0, 3);
    const int count = binds::apply_file(text);
    log::write(L"Binds: scripts\\keybinds.txt, %d lines applied", count);
}

// VanaCore's own commands: /addon, /plugin, /exec, //screenshot (and //addon, //plugin, //exec). False if
// the line isn't one of them.
// /vana lua <addon> <code>: the code run in a loaded addon's own Lua (as an expression first: "/vana lua fancychat
// #b.OriginalBuffer"), its results said in the chat log, a table's fields one level down. For finding out why an addon
// misbehaves while it runs.
void lua_eval(std::string_view line)
{
    // the code: the line after its third word, as typed (its spaces kept)
    size_t at = 0;
    for (int word = 0; word < 3; ++word) {
        while (at < line.size() && (line[at] == ' ' || line[at] == '\t'))
            ++at;
        while (at < line.size() && line[at] != ' ' && line[at] != '\t')
            ++at;
    }
    while (at < line.size() && (line[at] == ' ' || line[at] == '\t'))
        ++at;
    const std::vector<std::string> parts = words(line);
    const std::string code(line.substr(at));
    if (parts.size() < 4 || code.empty()) {
        say("/vana lua <addon> <code>: runs the code in the addon's Lua and says what it returns");
        return;
    }
    const std::lock_guard lock(g_lock);
    Addon* addon = nullptr;
    for (auto& each : g_addons)
        if (lower(each->context.name) == lower(parts[2]))
            addon = each.get();
    if (addon == nullptr || addon->crashed || addon->unload_requested) {
        say_error(parts[2] + " isn't loaded");
        return;
    }
    enter_thread("/vana lua");
    lua_State* L = addon->L;
    const int top = lua_gettop(L);
    const std::string expression = "return " + code;
    if (luaL_loadbuffer(L, expression.data(), expression.size(), "=/vana lua") != 0) {
        lua_pop(L, 1);
        if (luaL_loadbuffer(L, code.data(), code.size(), "=/vana lua") != 0) {
            say_error(lua_tostring(L, -1) != nullptr ? lua_tostring(L, -1) : "doesn't compile");
            lua_settop(L, top);
            return;
        }
    }
    std::string error;
    if (!call(*addon, 0, LUA_MULTRET, error)) {
        say_error(error);
        if (!addon->crashed)
            lua_settop(L, top);
        return;
    }
    const int results = lua_gettop(L) - top;
    static const char kShow[] = R"(
local function show(v, depth)
    local t = type(v)
    if t == 'string' then
        return string.format('%q', #v > 160 and v:sub(1, 160) .. '...' or v)
    end
    if t ~= 'table' or depth > 0 then
        return tostring(v)
    end
    local parts, n = {}, 0
    for k, x in pairs(v) do
        n = n + 1
        if n > 30 then
            parts[#parts + 1] = '...'
            break
        end
        parts[#parts + 1] = tostring(k) .. '=' .. show(x, depth + 1)
    end
    return '{#' .. #v .. ' ' .. table.concat(parts, ', ') .. '}'
end
local out = {}
for i = 1, select('#', ...) do
    out[i] = show((select(i, ...)), 0)
end
return #out == 0 and '(nothing)' or table.concat(out, '   ')
)";
    if (luaL_loadbuffer(L, kShow, sizeof kShow - 1, "=/vana lua") != 0) {
        lua_settop(L, top);
        return;
    }
    lua_insert(L, top + 1);
    if (!call(*addon, results, 1, error)) {
        say_error(error);
        if (!addon->crashed)
            lua_settop(L, top);
        return;
    }
    const char* text = lua_tostring(L, -1);
    say(text != nullptr ? text : "?");
    lua_settop(L, top);
    process_requests();
}

// /vana logs [on|off]: [logging] enabled, written to vanacore.ini and applied now, without a restart (no word: the other
// way round). The player alone sees the answer (chat::echo).
void logs_command(const std::string& option)
{
    const bool now = log::enabled();
    bool want = !now;
    if (option == "on" || option == "1")
        want = true;
    else if (option == "off" || option == "0")
        want = false;
    else if (!option.empty()) {
        chat::echo(std::string("/vana logs [on|off]: logging is ") + (now ? "on" : "off") + "; no word turns it the other way");
        return;
    }
    if (!g_settings_ini.empty())
        ini::set(g_settings_ini, L"logging", L"enabled", want ? L"1" : L"0");
    if (!log::set_enabled(want)) {
        chat::echo("logging couldn't be turned on: VanaCore's folder isn't known");
        return;
    }
    chat::echo(want ? "logging is on: logs\\vanacore.log from here on (crash dumps and the DLSS helper's log start with the game)"
                    : "logging is off: nothing is logged, printed or dumped");
}

// /vana dlss5 [on|off]: [graphics] dlss5 (DLSS 5: the 3D scene through NVIDIA's neural renderer before DLSS), written to
// the settings file and applied now, as the /vana window's DLSS 5 switch does (graphics::reload); no word, the other way
// round. DLSS 5 runs inside DLSS, so the answer says when DLSS itself is off, FSR is the upscaler, or DLSS 5 can't run here
// (graphics::Info::dlss5_why): the key is written all the same and takes effect once it can. The player alone sees the
// answer (chat::echo).
void dlss5_command(const std::string& option)
{
    if (g_settings_ini.empty()) {
        chat::echo("/vana dlss5: there's no vanacore.ini to change");
        return;
    }
    const bool now = GetPrivateProfileIntW(L"graphics", L"dlss5", 0, g_settings_ini.c_str()) != 0;
    bool want = !now;
    if (option == "on" || option == "1")
        want = true;
    else if (option == "off" || option == "0")
        want = false;
    else if (!option.empty()) {
        chat::echo(std::string("/vana dlss5 [on|off]: DLSS 5 is ") + (now ? "on" : "off") + "; no word turns it the other way");
        return;
    }
    if (!ini::set(g_settings_ini, L"graphics", L"dlss5", want ? L"1" : L"0")) {
        chat::echo("DLSS 5 couldn't be changed: the settings file couldn't be written");
        return;
    }
    graphics::reload();
    log::write(L"Graphics: DLSS 5 %ls (/vana dlss5)", want ? L"on" : L"off");
    if (!want) {
        chat::echo("DLSS 5 is off ([graphics] dlss5=0).");
        return;
    }
    const graphics::Info info = graphics::info();
    std::string text = "DLSS 5 is on: the 3D scene goes through it before DLSS ([graphics] dlss5=1).";
    if (!info.dlss5)
        text += " It can't run here" + (info.dlss5_why.empty() ? std::string() : ": " + info.dlss5_why) +
                "; the setting is kept for when it can.";
    else if (info.upscaler == "fsr")
        text += " FSR is the upscaler ([graphics] fsr): DLSS 5 runs through DLSS, so it waits for that.";
    else if (info.upscaler.empty())
        text += " DLSS itself is off ([graphics] dlss): DLSS 5 runs through it, so turn DLSS on too.";
    chat::echo(text);
}

// The game's name for status effect `id` ("buffs.names": the status DAT, by id), or null
constexpr int kStatusIds = 1024;
const char* status_name(int id)
{
    return id > 0 && id < kStatusIds ? ashita::resource_string("buffs.names", static_cast<uint32_t>(id), 0) : nullptr;
}

// /cancel <status name or id>: the effect taken off you, as the debuff addon's /cancel does, so a macro's "/cancel
// invisible" works without the addon: the status cancel packet (0x0F1, 8 bytes, the id at +4) sent as the game sends it
// from a status icon's menu. The name is the rest of the line (Max HP Boost), matched whole against the game's status
// names, else as the start of exactly one (/cancel invis). Alone, says what's on you. The packet goes whether the effect
// shows on you or not: the server does the cancelling (one that can't be cancelled is left), and the player's list can be
// a moment behind a buff that's just landed, which a macro cancels right after casting.
void cancel_command(std::string_view line)
{
    const size_t space = line.find_first_of(" \t");
    std::string want = space == std::string_view::npos ? std::string() : std::string(line.substr(space));
    const size_t first = want.find_first_not_of(" \t\r\n"), last = want.find_last_not_of(" \t\r\n");
    want = first == std::string::npos ? std::string() : want.substr(first, last - first + 1);
    if (want.empty()) {
        std::string on;
        game::Player player;
        if (game::player(player)) {
            for (const std::int16_t id : player.buffs) {
                if (id <= 0 || id == 255)
                    continue;
                const char* name = status_name(id);
                on += (on.empty() ? "" : ", ") + (name != nullptr && *name != '\0' ? std::string(name) : std::string("status")) +
                      " (" + std::to_string(id) + ")";
            }
        }
        say("/cancel <status name or id> takes an effect off you (/cancel invisible, /cancel 69). " +
            (on.empty() ? std::string("Nothing is on you now.") : "On you now: " + on + "."));
        return;
    }
    int id = -1;
    char* end = nullptr;
    const long number = std::strtol(want.c_str(), &end, 10);
    if (end != want.c_str() && *end == '\0') {
        id = static_cast<int>(number);
    } else {
        const std::string lowered = lower(want);
        int prefix = -1, prefixes = 0;
        for (int i = 1; i < kStatusIds && id < 0; ++i) {
            const char* name = status_name(i);
            if (name == nullptr || *name == '\0')
                continue;
            const std::string candidate = lower(name);
            if (candidate == lowered) {
                id = i;
            } else if (candidate.compare(0, lowered.size(), lowered) == 0) {
                prefix = i;
                ++prefixes;
            }
        }
        if (id < 0 && prefixes == 1)
            id = prefix;
        if (id < 0) {
            say_error(prefixes > 1 ? "/cancel: \"" + want + "\" starts more than one status name: give more of it."
                                   : "/cancel: no status effect is called \"" + want + "\".");
            return;
        }
    }
    if (id <= 0 || id >= kStatusIds) {
        say_error("/cancel: a status id is 1 to 1023.");
        return;
    }
    if (!packets::ready()) {
        say_error("/cancel: not in the game yet.");
        return;
    }
    const std::uint8_t packet[8] = {0xF1, 0x04, 0x00, 0x00, static_cast<std::uint8_t>(id & 0xFF),
                                    static_cast<std::uint8_t>(id >> 8), 0x00, 0x00};
    packets::inject(false, 0x0F1, sizeof packet, packet);
}

bool vanacore_command(std::string_view line)
{
    if (binds::command(line))
        return true;
    const std::vector<std::string> parts = words(line);
    if (parts.empty() || !commands::on(parts[0])) // switched off in Client > Commands: the addons', plugins' and game's
        return false;
    const std::string command = lower(parts[0]);
    if (command == "//addon" || command == "/addon") {
        addon_command(parts, !g_command_injected);
        return true;
    }
    if (command == "//exec" || command == "/exec") {
        if (parts.size() < 2)
            say("/exec <script> [args...]: runs scripts\\<script>.txt, %1% in it is the first argument");
        else
            run_script(parts[1], false, Lines::All, std::vector<std::string>(parts.begin() + 2, parts.end()));
        return true;
    }
    // Ashita's /execstr <string> [args...]: the string is a script (commands split at ;, run as ExecuteScriptString runs
    // them), "%1%" in it the first word after it. Quote the string to give it arguments: /execstr "/echo %1%" hello
    if (command == "/execstr" || command == "//execstr") {
        const size_t at = line.find_first_of(" \t");
        std::string body = at == std::string_view::npos ? std::string() : std::string(line.substr(at));
        const size_t first = body.find_first_not_of(" \t\r\n");
        if (first == std::string::npos) {
            say("/execstr <string> [args...]: runs the string as a script (\"%1%\" in it is the first argument)");
            return true;
        }
        body.erase(0, first);
        std::vector<std::string> args;
        if (body[0] == '"') {
            const size_t close = body.find('"', 1);
            if (close != std::string::npos) {
                args = words(std::string_view(body).substr(close + 1));
                body = body.substr(1, close - 1);
            }
        }
        run_script_string(body, args, true);
        return true;
    }
    // Typed alone, a wait has nothing to hold up: in a script run_script reads it and delays the lines after it.
    if (command == "/wait" || command == "/sleep" || command == "/pause" || command == "//sleep" || command == "//pause")
        return true;
    // Ashita's clipboard commands for the chat line: /copy puts what's typed on the clipboard, /paste inserts the
    // clipboard where the caret is
    if (command == "/copy" || command == "//copy") {
        bool open = false;
        std::string typed;
        chat::input_line(open, typed);
        if (typed.empty())
            return true;
        if (!OpenClipboard(nullptr)) {
            say_error("/copy: couldn't open the clipboard");
            return true;
        }
        EmptyClipboard();
        if (HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, typed.size() + 1)) {
            if (void* to = GlobalLock(memory)) {
                std::memcpy(to, typed.c_str(), typed.size() + 1);
                GlobalUnlock(memory);
                if (SetClipboardData(CF_TEXT, memory) == nullptr)
                    GlobalFree(memory);
            } else {
                GlobalFree(memory);
            }
        }
        CloseClipboard();
        return true;
    }
    if (command == "/paste" || command == "//paste") {
        // in the game's Shift-JIS (CF_TEXT is in the code page of the keyboard it was copied with, not always Shift-JIS)
        std::string text = clipboard::get_text(window::handle());
        for (char& c : text)
            if (c == '\r' || c == '\n' || c == '\t')
                c = ' '; // the chat line is one line
        if (!text.empty())
            ashita::input_insert(text);
        return true;
    }
    // Ashita's /exit closes the game as /shutdown does; /terminate ends the process at once
    if (command == "/exit" || command == "//exit") {
        log::write(L"Commands: /exit");
        chat::run(1, "/shutdown");
        return true;
    }
    if (command == "/terminate" || command == "//terminate") {
        log::write(L"Commands: /terminate");
        gamesave::run(); // the macro book and set in use: the game writes them only at its logout
        TerminateProcess(GetCurrentProcess(), 0);
        return true;
    }
    if (command == "//plugin" || command == "/plugin") {
        plugins::command(parts);
        // Typed, /plugin unload takes the plugin out of scripts\boot.txt as /addon unload does its addon (a kind last isn't one)
        if (!g_command_injected && parts.size() > 2 && lower(parts[1]) == "unload") {
            size_t end = parts.size();
            if (end > 3 && source_named(lower(parts[end - 1])) != nullptr)
                --end;
            for (size_t i = 2; i < end; ++i)
                persist_boot(lower(parts[i]), false, true);
        }
        return true;
    }
    // /keybinds: the keybinds window's (an addon); VanaCore runs only its start-up apply
    if ((command == "//keybinds" || command == "/keybinds") && parts.size() > 1 && lower(parts[1]) == "apply") {
        apply_saved_binds();
        return true;
    }
    // /screenshot: a shot (hide / show: without / with VanaCore's drawing); "open" opens the folder (no /ss: players' own)
    if (command == "//screenshot" || command == "/screenshot") {
        const std::string option = parts.size() > 1 ? lower(parts[1]) : "";
        if (option == "open") {
            screenshot::open_folder();
            return true;
        }
        screenshot::request(option == "hide" ? 0 : option == "show" ? 1 : -1);
        return true;
    }
    if (command == "//abtest" || command == "/abtest") {
        abtest::command(parts);
        return true;
    }
    // /camtrace: the camera's recent walls and lock to the log (camera.cpp)
    if (command == "//camtrace" || command == "/camtrace") {
        camera::dump_trace();
        say("Camera trace written to vanacore.log.");
        return true;
    }
    // /freelook [on|off]: the camera turns without turning your character (camera.cpp); alone, the other way
    if (command == "//freelook" || command == "/freelook") {
        const std::string option = parts.size() > 1 ? lower(parts[1]) : "";
        const int want = option == "on" || option == "1" ? 1 : option == "off" || option == "0" ? 0 : -1;
        const int on = camera::free_look(want);
        if (on < 0)
            say_error("Free look couldn't be found in the game.");
        else
            say(on ? "Free look on: you walk the way the camera faced; turn it freely. /freelook again turns it off."
                   : "Free look off: you walk the way the camera faces.");
        return true;
    }
    // /freecam [on|off]: the camera detached from the character, flown with the keyboard and mouse while the character stands
    // still (camera.cpp); alone, the other way
    if (command == "//freecam" || command == "/freecam") {
        const std::string option = parts.size() > 1 ? lower(parts[1]) : "";
        // the freecam addon's own (its window and sequencer, the binds R and P make, and its Weather and Time): left to it
        if (option == "window" || option == "open" || option == "close" || option == "key" || option == "play" ||
            option == "stop" || option == "rewind" || option == "seq" || option == "weather" || option == "time")
            return false;
        // /freecam float [on|off]: the free camera glides and turns with some lag
        if (option == "float" || option == "floaty") {
            const std::string value = parts.size() > 2 ? lower(parts[2]) : "";
            const int floaty = camera::free_cam_float(value == "on" || value == "1" ? 1 : value == "off" || value == "0" ? 0 : -1);
            say(floaty ? "Floaty free camera on: it glides and turns with some lag ([camera] freecam.lag sets how much). "
                         "/freecam float again turns it off."
                       : "Floaty free camera off: it moves and turns at once.");
            return true;
        }
        const int want = option == "on" || option == "1" ? 1 : option == "off" || option == "0" ? 0 : -1;
        const int on = camera::free_cam(want);
        if (on == -1)
            say_error("The free camera couldn't be found in the game.");
        else if (on == -2)
            say_error("The free camera needs a character in the world.");
        else
            say(on ? "Free camera on (your character stands still): W A S D fly, E or Space up, Q or C down, Shift fast, wheel "
                     "for speed, right button or arrows to look. /freecam again returns."
                   : "Free camera off.");
        return true;
    }
    // Ashita's /fillmode <solid|wireframe|point> (or 3, 2, 1: a D3DFILLMODE): how the world is drawn (graphics.cpp); alone,
    // says which is on. The game's interface and VanaCore's own windows stay solid, so the game can still be played in it.
    // Typed, it says what it did; from an addon, a plugin, a script or a bind (g_command_injected) it says nothing: an
    // addon switching the fill mode back and forth as it drew filled the chat with the line (2026-10-24)
    if (command == "//fillmode" || command == "/fillmode") {
        static constexpr const char* kNames[] = {"", "point", "wireframe", "solid"};
        const std::string option = parts.size() > 1 ? lower(parts[1]) : "";
        const int want = option == "solid" || option == "3"                            ? 3
                         : option == "wireframe" || option == "wire" || option == "2" ? 2
                         : option == "point" || option == "points" || option == "1"   ? 1
                                                                                      : 0;
        const bool typed = !g_command_injected;
        if (want == 0) {
            if (typed)
                say(std::string("Fill mode: ") + kNames[graphics::fill_mode()] +
                    ". /fillmode solid, wireframe or point (or 3, 2, 1) draws the world that way; the interface stays as it is.");
            return true;
        }
        if (!graphics::set_fill_mode(want)) {
            static bool logged = false; // an addon's call on the game's own renderer: the log once, not the chat each time
            if (typed)
                say_error("/fillmode needs VanaCore's renderer ([graphics] renderer=d3d12 or d3d11).");
            else if (!logged) {
                logged = true;
                log::write(L"Addons: /fillmode from an addon, plugin or script needs VanaCore's renderer ([graphics] renderer=d3d12 or d3d11)");
            }
        } else if (typed) {
            say(want == 2 ? "Fill mode: wireframe. The world is drawn as its triangles' edges; /fillmode solid puts it back."
                : want == 1
                    ? "Fill mode: point. The world is drawn as its triangles' corners, a pixel each; /fillmode solid puts it back."
                    : "Fill mode: solid, as the game draws the world.");
        }
        return true;
    }
    // /cancel <status name or id> (cancel_command above): VanaCore's runs before the debuff addon's, which does the same;
    // the addon's /debuff is still its own
    if (command == "//cancel" || command == "/cancel") {
        cancel_command(line);
        return true;
    }
    // /vana's own subcommands VanaCore runs (the rest are the /vana window's). /vana debug alone is the window's (its Debug
    // window: vana\debug.lua); /vana debug <seconds> or stop records frames here
    if ((command == "//vana" || command == "/vana") && parts.size() > 1) {
        const std::string sub = lower(parts[1]);
        if (sub == "abtest" || (sub == "debug" && parts.size() > 2)) {
            std::vector<std::string> rest(parts.begin() + 1, parts.end());
            for (size_t i = 1; i < rest.size(); ++i)
                if (sub == "debug")
                    rest[i] = lower(rest[i]);
            if (sub == "abtest")
                abtest::command(rest);
            else
                debugrec::command(rest);
            return true;
        }
        if (sub == "lua") {
            const bool dev_enabled =
                !g_settings_ini.empty() && GetPrivateProfileIntW(L"developer", L"enabled", 0, g_settings_ini.c_str()) != 0;
            if (!dev_enabled) {
                say_error("/vana lua is disabled in player builds (set [developer] enabled=1 in vanacore.ini).");
                return true;
            }
            lua_eval(line);
            return true;
        }
        if (sub == "telemetry") {
            hwmon::command(std::vector<std::string>(parts.begin() + 1, parts.end()));
            return true;
        }
        if (sub == "stats") {
            stats::command(std::vector<std::string>(parts.begin() + 1, parts.end()));
            return true;
        }
        if (sub == "graph") {
            perf::command(std::vector<std::string>(parts.begin() + 1, parts.end()));
            return true;
        }
        if (sub == "logs") {
            logs_command(parts.size() > 2 ? lower(parts[2]) : "");
            return true;
        }
        // /vana dlss5 [on|off]: DLSS 5's switch ([graphics] dlss5), applied now; alone, the other way round
        if (sub == "dlss5") {
            dlss5_command(parts.size() > 2 ? lower(parts[2]) : "");
            return true;
        }
        // /vana trace <addon...|off|list>: logs what an addon does, to find out why it doesn't work; /vana trace <addon>
        // <seconds>: that long, and a report of its cost and the game's frames (addon_trace.hpp)
        if (sub == "trace") {
            addon_trace::command(std::vector<std::string>(parts.begin() + 2, parts.end()));
            return true;
        }
        // /vana window <what>: where the game window is (/vana window alone opens the /vana window's Client tab)
        if (sub == "window" && parts.size() > 2)
            return window::command(std::vector<std::string>(parts.begin() + 1, parts.end()));
    }
    if (command == "//wait") {
        say("//wait <seconds> only works in scripts");
        return true;
    }
    return false;
}

// A script's line: VanaCore's commands here, anything else through the game's command line (where
// addons' command events see it too).
bool run_line(std::string_view line)
{
    g_command_injected = true; // a script's line (this doesn't come through on_command)
    if (vanacore_command(line))
        return true;
    if (!chat::ready()) {
        log::write(L"Scripts: can't run \"%hs\": FFXI's command line isn't hooked", std::string(line).c_str());
        return false;
    }
    chat::run(1, std::string(line));
    return true;
}

// Each addon's 'unhandled' event (e.command: a // command nothing took, without the //), which Windower addons have as
// 'unhandled command' (shortcuts' //cure3 <t>). Every addon hears it; true if one took it (returned true or set
// e.blocked).
bool raise_unhandled(std::string_view command)
{
    bool taken = false;
    for (size_t i = 0; i < g_addons.size(); ++i) {
        Addon& addon = *g_addons[i];
        if (addon.unload_requested)
            continue;
        lua_State* L = addon.L;
        push_hook(addon, "raise");
        lua_pushliteral(L, "unhandled");
        lua_createtable(L, 0, 2);
        lua_pushlstring(L, command.data(), command.size());
        lua_setfield(L, -2, "command");
        lua_pushvalue(L, -1);
        const int event = luaL_ref(L, LUA_REGISTRYINDEX);
        std::string error;
        if (call(addon, 2, 1, error)) {
            taken = taken || lua_toboolean(L, -1) != 0;
            lua_pop(L, 1);
            lua_rawgeti(L, LUA_REGISTRYINDEX, event);
            lua_getfield(L, -1, "blocked");
            taken = taken || lua_toboolean(L, -1) != 0;
            lua_pop(L, 2);
        } else {
            report_call(addon, "unhandled", error);
        }
        if (!addon.crashed)
            luaL_unref(L, LUA_REGISTRYINDEX, event);
    }
    process_requests();
    return taken;
}

// Every command line the game is about to run: VanaCore's commands, then each addon's 'command'
// event (e.command, e.mode, e.injected; a handler sets e.blocked = true, or returns true, to keep
// it from the game).
bool on_command(const std::string& line, int mode, bool injected)
{
    const std::lock_guard lock(g_lock);
    enter_thread("a command line");
    size_t start = 0;
    while (start < line.size() && (line[start] == ' ' || line[start] == '\t'))
        ++start;
    const std::string_view text = std::string_view(line).substr(start);
    g_command_injected = injected;
    // Ashita's /alias, also from inside an addon's call (a LuAshitacast profile's ParseCommand as it loads)
    if (aliases::command(text)) {
        addon_trace::command_line(line, mode, injected, "/alias");
        return true;
    }
    if (g_depth == 0 && vanacore_command(text)) {
        addon_trace::command_line(line, mode, injected, "VanaCore");
        return true;
    }
    // Plugins around addons as in Ashita: those with a negative priority first, the rest after (plugins.hpp). A // line
    // is the addons' own (Windower's commands, //tsup), which an Ashita plugin may take for itself (Thirdparty takes every
    // // line, so no Windower addon's command ever came): those go to the addons first, and to the plugins only if none
    // took it.
    const bool slashes = text.starts_with("//");
    std::string plugin;
    if (!slashes && plugins::on_command(line, mode, injected, plugins::Commands::BeforeAddons, plugin)) {
        addon_trace::command_line(line, mode, injected, "plugin " + plugin);
        return true;
    }
    if (addon_trace::any() && text.starts_with('/'))
        addon_trace::command_line(line, mode, injected, "");

    bool blocked = false;
    for (size_t i = 0; i < g_addons.size(); ++i) {
        Addon& addon = *g_addons[i];
        if (addon.unload_requested)
            continue;
        const bool was_blocked = blocked;
        lua_State* L = addon.L;
        push_hook(addon, "raise");
        lua_pushliteral(L, "command");
        lua_createtable(L, 0, 4);
        lua_pushlstring(L, line.data(), line.size());
        lua_setfield(L, -2, "command");
        lua_pushinteger(L, mode);
        lua_setfield(L, -2, "mode");
        lua_pushboolean(L, injected);
        lua_setfield(L, -2, "injected");
        lua_pushboolean(L, blocked);
        lua_setfield(L, -2, "blocked");
        lua_pushvalue(L, -1);
        const int event = luaL_ref(L, LUA_REGISTRYINDEX);
        std::string error;
        if (call(addon, 2, 1, error)) {
            blocked = blocked || lua_toboolean(L, -1) != 0;
            lua_pop(L, 1);
            lua_rawgeti(L, LUA_REGISTRYINDEX, event);
            lua_getfield(L, -1, "blocked");
            blocked = blocked || lua_toboolean(L, -1) != 0;
            lua_pop(L, 2);
        } else {
            report_call(addon, "command", error);
        }
        if (addon_trace::on(addon.context.name) && text.starts_with('/'))
            addon_trace::command_reached(addon.context.name, line, blocked && !was_blocked, error);
        if (!addon.crashed)
            luaL_unref(L, LUA_REGISTRYINDEX, event);
    }
    process_requests();
    if (blocked)
        return true;
    if (plugins::on_command(line, mode, injected, slashes ? plugins::Commands::All : plugins::Commands::AfterAddons, plugin)) {
        addon_trace::command_line(line, mode, injected, "plugin " + plugin, true);
        return true;
    }
    // Nothing handled a VanaCore command: a Windower plugin may know it; if not, say so, rather
    // than send it to the game as chat.
    if (text.starts_with("//") && plugins::on_unhandled(text.substr(2)))
        return true;
    if (text.starts_with("//") && raise_unhandled(text.substr(2)))
        return true;
    if (text.starts_with("//")) {
        const std::vector<std::string> parts = words(text);
        say_error("Unknown command " + (parts.empty() ? std::string("//") : parts[0]) +
                  " (/addon, /plugin, /exec, /bind, //screenshot)");
        return true;
    }
    return false;
}

// A line of text coming into the chat log, or going to the game (chat.cpp): plugins first (Ashita's
// HandleIncomingText/HandleOutgoingText), then each addon's 'text_in' or 'text_out' event (e.mode, e.indent, e.message,
// e.mode_modified, e.indent_modified, e.message_modified, e.injected, e.blocked: Ashita's; a handler changes the
// _modified ones, sets e.blocked = true or returns true). True: blocked.
bool on_text(bool incoming, chat::Text& text)
{
    if (incoming)
        craftlog::on_text(text.mode, text.message); // the server's lines about crystals and synthesis, as they came
    bool blocked = plugins::on_text(incoming, text);
    const std::lock_guard lock(g_lock);
    // A line an addon adds while it runs (print, AddChatMessage) goes to the other addons' text_in, as in Ashita
    // (FancyChat shows what other addons print, and its own timestamp lines), not back into the addon adding it; a
    // few lines deep at most (two addons answering each other's lines would go on for ever).
    static int nested = 0;
    if (nested >= 4)
        return blocked;
    struct Nest {
        Nest() { ++nested; }
        ~Nest() { --nested; }
    } nest;
    enter_thread("a line of text");
    for (size_t i = 0; i < g_addons.size(); ++i) {
        Addon& addon = *g_addons[i];
        if (addon.unload_requested || !addon.text || addon.busy != 0 || addon.crashed)
            continue;
        lua_State* L = addon.L;
        push_hook(addon, "raise");
        lua_pushstring(L, incoming ? "text_in" : "text_out");
        lua_createtable(L, 0, 8);
        lua_pushinteger(L, text.mode);
        lua_setfield(L, -2, "mode");
        lua_pushinteger(L, text.mode_modified);
        lua_setfield(L, -2, "mode_modified");
        if (incoming) {
            lua_pushboolean(L, text.indent);
            lua_setfield(L, -2, "indent");
            lua_pushboolean(L, text.indent_modified);
            lua_setfield(L, -2, "indent_modified");
        }
        lua_pushlstring(L, text.message.data(), text.message.size());
        lua_setfield(L, -2, "message");
        lua_pushlstring(L, text.modified.data(), text.modified.size());
        lua_setfield(L, -2, "message_modified");
        lua_pushboolean(L, text.injected);
        lua_setfield(L, -2, "injected");
        lua_pushboolean(L, blocked);
        lua_setfield(L, -2, "blocked");
        lua_pushvalue(L, -1);
        const int event = luaL_ref(L, LUA_REGISTRYINDEX);
        std::string error;
        if (call(addon, 2, 1, error)) {
            blocked = blocked || lua_toboolean(L, -1) != 0;
            lua_pop(L, 1);
            lua_rawgeti(L, LUA_REGISTRYINDEX, event);
            lua_getfield(L, -1, "blocked");
            blocked = blocked || lua_toboolean(L, -1) != 0;
            lua_pop(L, 1);
            lua_getfield(L, -1, "message_modified");
            size_t length = 0;
            if (const char* now = lua_tolstring(L, -1, &length))
                text.modified.assign(now, length);
            lua_pop(L, 1);
            lua_getfield(L, -1, "mode_modified");
            if (lua_isnumber(L, -1))
                text.mode_modified = static_cast<int>(lua_tointeger(L, -1));
            lua_pop(L, 1);
            if (incoming) {
                lua_getfield(L, -1, "indent_modified");
                text.indent_modified = lua_toboolean(L, -1) != 0;
                lua_pop(L, 1);
            }
            lua_pop(L, 1);
        } else {
            report_call(addon, incoming ? "text_in" : "text_out", error);
        }
        if (!addon.crashed)
            luaL_unref(L, LUA_REGISTRYINDEX, event);
    }
    process_requests();
    return blocked;
}

// The addon that blocked the event raise_table raised last on this thread (blocked_by)
thread_local std::string g_blocked_by;

// Raises `event` in each addon `wants` picks, with a table `fill` makes; `read` reads it back after each handler (a
// handler returning true, or setting e.blocked, blocks). True: blocked.
template <typename Wants, typename Fill, typename Read>
bool raise_table(const char* event, Wants wants, Fill fill, Read read, bool blocked)
{
    const std::lock_guard lock(g_lock);
    g_blocked_by.clear();
    if (g_depth != 0)
        return blocked;
    enter_thread(event);
    for (size_t i = 0; i < g_addons.size(); ++i) {
        Addon& addon = *g_addons[i];
        if (addon.unload_requested || addon.crashed || !wants(addon))
            continue;
        const bool was_blocked = blocked;
        lua_State* L = addon.L;
        push_hook(addon, "raise");
        lua_pushstring(L, event);
        lua_createtable(L, 0, 6);
        fill(L);
        lua_pushboolean(L, blocked);
        lua_setfield(L, -2, "blocked");
        lua_pushvalue(L, -1);
        const int table = luaL_ref(L, LUA_REGISTRYINDEX);
        std::string error;
        if (call(addon, 2, 1, error)) {
            blocked = blocked || lua_toboolean(L, -1) != 0;
            lua_pop(L, 1);
            lua_rawgeti(L, LUA_REGISTRYINDEX, table);
            lua_getfield(L, -1, "blocked");
            blocked = blocked || lua_toboolean(L, -1) != 0;
            lua_pop(L, 1);
            read(L);
            lua_pop(L, 1);
            if (blocked && !was_blocked)
                g_blocked_by = addon.context.name;
        } else {
            report_call(addon, event, error);
        }
        if (!addon.crashed)
            luaL_unref(L, LUA_REGISTRYINDEX, table);
    }
    process_requests();
    return blocked;
}

// What changed in the game since the last frame (gameevents.cpp: 'zone_change', 'main_menu_open', ...), each to the addons
// listening for it. Nothing is read while none listens.
void raise_game_events()
{
    std::uint32_t wanted = 0;
    for (const auto& addon : g_addons)
        if (!addon->unload_requested && !addon->crashed)
            wanted |= addon->game_events;
    static std::vector<gameevents::Event> events;
    events.clear();
    gameevents::poll(wanted, events);
    for (const gameevents::Event& event : events) {
        const std::uint32_t bit = 1u << gameevents::index(event.name);
        raise_table(
            event.name, [bit](const Addon& addon) { return (addon.game_events & bit) != 0; },
            [&](lua_State* L) {
                for (const gameevents::Field& field : event.fields) {
                    if (field.kind == gameevents::Field::Text)
                        lua_pushlstring(L, field.text.data(), field.text.size());
                    else
                        lua_pushnumber(L, field.number);
                    lua_setfield(L, -2, field.key);
                }
            },
            [](lua_State*) {}, false);
    }
}

} // namespace

bool required(std::string_view name, bool plugin)
{
    return is_required(name, plugin);
}

void run_commands(const std::vector<std::string>& lines)
{
    const std::lock_guard lock(g_lock);
    run_lines(lines, true);
}

void execute_script_string(const std::string& script, const std::string& args, bool queued)
{
    const std::lock_guard lock(g_lock);
    run_script_string(script, words(args), queued);
}

void execute_script(const std::string& name, const std::string& args)
{
    const std::lock_guard lock(g_lock);
    run_script(name, false, Lines::All, words(args));
}

void install(const wchar_t* install_dir, const wchar_t* settings_ini)
{
    g_install = ansi(install_dir) + "\\";
    g_settings_ini = settings_ini != nullptr ? settings_ini : L"";
    addon_trace::install(install_dir, g_settings_ini.c_str());
    chat::set_command_handler(&on_command);
    chat::set_text_handler(&on_text);
    log::write(L"Addons: from %ls\\addons (vanacore, ashita, windower), with %hs; scripts\\boot.txt runs at start",
               install_dir, LUAJIT_VERSION);
    wchar_t names[256] = {};
    if (!g_settings_ini.empty())
        GetPrivateProfileStringW(L"addons", L"load", L"", names, 256, g_settings_ini.c_str());
    if (names[0] != L'\0')
        log::write(L"Addons: [addons] load=%ls is no longer read: put /addon load lines in scripts\\boot.txt", names);
    g_silent = !g_settings_ini.empty() && GetPrivateProfileIntW(L"addons", L"silent", 0, g_settings_ini.c_str()) != 0;
    g_silent_ini = g_silent;
    g_ashita_enabled = g_settings_ini.empty() || GetPrivateProfileIntW(L"addons", L"ashita.enabled", 1, g_settings_ini.c_str()) != 0;
    if (!g_ashita_enabled)
        log::write(L"Addons: [addons] ashita.enabled=0 — Ashita addon compatibility layer is DISABLED");
    addonpkg::start(install_dir); // .vcaddon opens with this VanaCore; addons\install\ waits for packages
}

bool ashita_enabled()
{
    return g_ashita_enabled;
}

void reload(const wchar_t* ini)
{
    const bool silent = GetPrivateProfileIntW(L"addons", L"silent", 0, ini) != 0;
    if (silent == g_silent_ini)
        return;
    g_silent_ini = g_silent = silent;
    log::write(L"Addons: [addons] silent=%d", silent ? 1 : 0);
}

std::vector<ErrorEntry> recent_errors(const std::string& name)
{
    const std::lock_guard lock(g_lock);
    Addon* addon = find(name);
    if (addon == nullptr)
        return {};
    const std::lock_guard errors(g_errors_lock);
    return addon->recent;
}

bool info(const std::string& name, Info& out)
{
    const std::lock_guard lock(g_lock);
    Addon* addon = find(name);
    if (addon == nullptr || addon->unload_requested)
        return false;
    out = {};
    out.kind = kind(addon->context.flavour);
    out.errors = addon->errors;
    out.crashed = addon->crashed;
    if (!addon->crashed)
        out.memory_kb = static_cast<double>(lua_gc(addon->L, LUA_GCCOUNT, 0)) + static_cast<double>(lua_gc(addon->L, LUA_GCCOUNTB, 0)) / 1024.0;
    if (addon->packets)
        out.listens.push_back("packets");
    if (addon->text)
        out.listens.push_back("chat text");
    if (addon->mouse)
        out.listens.push_back("mouse");
    if (addon->key)
        out.listens.push_back("keys");
    if (addon->keys)
        out.listens.push_back("keyboard state");
    if (addon->gamepad)
        out.listens.push_back("controllers");
    if (addon->draws)
        out.listens.push_back("draw calls");
    if (addon->scenes)
        out.listens.push_back("scenes");
    if (addon->game_events != 0)
        out.listens.push_back("game events");
    return true;
}

namespace {

std::string g_profiled; // the addon being sampled

// Runs a chunk in the addon's state, with its hooks table (lua/vana.lua's) as `...`; its string result, or empty with
// `error`.
bool run_chunk(Addon& addon, const char* code, std::string& result, std::string& error)
{
    lua_State* L = addon.L;
    if (luaL_loadstring(L, code) != 0) {
        error = lua_tostring(L, -1) != nullptr ? lua_tostring(L, -1) : "?";
        lua_pop(L, 1);
        return false;
    }
    lua_rawgeti(L, LUA_REGISTRYINDEX, addon.hooks);
    if (!call(addon, 1, 1, error))
        return false;
    size_t length = 0;
    const char* text = lua_tolstring(L, -1, &length);
    result.assign(text != nullptr ? text : "", length);
    lua_pop(L, 1);
    return true;
}

// The ticks while the addon isn't running pile up, and jit.profile hands them all over at the first instruction it runs
// next: the top of the hook VanaCore called it through (vana.lua's frame or raise). Those are dropped, or the hook's
// first line takes 90% of the samples and "C" most of the VM's time.
constexpr char kProfileStart[] = R"LUA(
local hooks = ...
local ok, prof = pcall(require, 'jit.profile')
if not ok then return 'jit.profile is not in this LuaJIT' end
local entries = {}
for _, fn in pairs(type(hooks) == 'table' and hooks or {}) do
    if type(fn) == 'function' then entries[fn] = true end
end
local getinfo = debug.getinfo
__vana_prof = { lines = {}, funcs = {}, states = {}, total = 0 }
local started, err = pcall(prof.start, 'li1', function (thread, samples, vmstate)
    local p = __vana_prof
    if p == nil then return end
    local top = getinfo(thread, 0, 'f')
    if top ~= nil and entries[top.func] and getinfo(thread, 1, 'f') == nil then return end
    local line = prof.dumpstack(thread, 'pl', 1)
    local func = prof.dumpstack(thread, 'F', 1)
    p.lines[line] = (p.lines[line] or 0) + samples
    p.funcs[func] = (p.funcs[func] or 0) + samples
    p.states[vmstate] = (p.states[vmstate] or 0) + samples
    p.total = p.total + samples
end)
if not started then __vana_prof = nil; return tostring(err) end
return ''
)LUA";

constexpr char kProfileStop[] = R"LUA(
local ok, prof = pcall(require, 'jit.profile')
if ok then pcall(prof.stop) end
local p = __vana_prof
__vana_prof = nil
if p == nil then return '' end
local out = { 'T\t' .. p.total }
for state, n in pairs(p.states) do out[#out + 1] = 'S\t' .. state .. '\t' .. n end
local function top(tbl, tag)
    local list = {}
    for key, n in pairs(tbl) do list[#list + 1] = { key, n } end
    table.sort(list, function (a, b) return a[2] > b[2] end)
    for i = 1, math.min(#list, 25) do
        local key = list[i][1]
        if key == '' then key = '(C code, or no Lua function)' end
        out[#out + 1] = tag .. '\t' .. list[i][2] .. '\t' .. (key:gsub('[\t\r\n]', ' '))
    end
end
top(p.lines, 'L')
top(p.funcs, 'F')
return table.concat(out, '\n')
)LUA";

} // namespace

bool profile_start(const std::string& name, std::string& error)
{
    const std::lock_guard lock(g_lock);
    if (!g_profiled.empty()) {
        error = "already sampling " + g_profiled;
        return false;
    }
    Addon* addon = find(name);
    if (addon == nullptr || addon->unload_requested || addon->crashed) {
        error = "it isn't loaded";
        return false;
    }
    std::string result;
    if (!run_chunk(*addon, kProfileStart, result, error))
        return false;
    if (!result.empty()) {
        error = result;
        return false;
    }
    g_profiled = name;
    return true;
}

bool profile_stop(const std::string& name, std::string& report, std::string& error)
{
    const std::lock_guard lock(g_lock);
    if (g_profiled != name) {
        error = "not sampling it";
        return false;
    }
    g_profiled.clear();
    Addon* addon = find(name);
    if (addon == nullptr || addon->unload_requested || addon->crashed) {
        error = "it isn't loaded any more";
        return false;
    }
    return run_chunk(*addon, kProfileStop, report, error);
}

void hold(const std::string& name, const std::string& why)
{
    const std::lock_guard lock(g_lock);
    const std::string key = lower(name);
    Held* held = nullptr;
    for (Held& each : g_held)
        if (each.name == key)
            held = &each;
    if (held == nullptr) {
        g_held.push_back({key, why, false});
        held = &g_held.back();
    }
    held->why = why;
    Addon* const addon = find(key);
    if (addon == nullptr || addon->unload_requested)
        return;
    held->wanted = true;
    say(addon->display + " unloaded: " + why);
    if (g_depth == 0)
        unload(*addon, true);
    else
        addon->unload_requested = true;
}

void release(const std::string& name)
{
    const std::lock_guard lock(g_lock);
    const std::string key = lower(name);
    for (auto it = g_held.begin(); it != g_held.end(); ++it) {
        if (it->name != key)
            continue;
        const bool wanted = it->wanted;
        g_held.erase(it);
        if (wanted && find(key) == nullptr)
            load(key);
        return;
    }
}

std::vector<Loaded> loaded()
{
    const std::lock_guard lock(g_lock);
    std::vector<Loaded> out;
    for (const auto& addon : g_addons) {
        if (addon->unload_requested)
            continue;
        Loaded entry;
        entry.name = addon->display;
        entry.source = source_of(addon->source).word;
        entry.file = addon->context.folder + addon->display + ".lua";
        entry.state = addon->crashed ? 0 : addon->state;
        entry.version = addon->version;
        entry.author = addon->author;
        entry.description = addon->description;
        entry.link = addon->link;
        // (its details as it loaded: another addon's Lua, maybe in the middle of a task, isn't touched for them; the
        // memory count only reads its allocator's total, and a crashed one's state is never touched again)
        if (!addon->crashed && addon->L != nullptr)
            entry.memory = static_cast<double>(lua_gc(addon->L, LUA_GCCOUNT, 0)) * 1024.0 + lua_gc(addon->L, LUA_GCCOUNTB, 0);
        out.push_back(std::move(entry));
    }
    return out;
}

bool eval(const std::string& name, const std::string& code, int depth, std::string& output, std::string& error)
{
    // Runs the compiled code (its first argument) with print caught, and describes what it returned:
    // ok, what it printed and returned, the error.
    static const char kRunner[] = R"LUA(
local fn, depth, traceback = ...
local printed = {}
local saved = print
print = function(...)
    local parts = {}
    for i = 1, select('#', ...) do parts[i] = tostring((select(i, ...))) end
    printed[#printed + 1] = table.concat(parts, '\t')
end
local results = {n = 0}
local function keep(ok, ...)
    results.n = select('#', ...)
    for i = 1, results.n do results[i] = (select(i, ...)) end
    return ok
end
local ok = keep(xpcall(fn, traceback))
print = saved
local seen = {}
local function key_text(k)
    if type(k) == 'string' and k:match('^[%a_][%w_]*$') then return k end
    if type(k) == 'string' then return string.format('[%q]', k) end
    return '[' .. tostring(k) .. ']'
end
local function show(v, level, indent)
    local t = type(v)
    if t == 'string' then
        if #v > 4000 then v = v:sub(1, 4000) .. '...' end
        return string.format('%q', v)
    end
    if t == 'number' then
        if v == math.floor(v) and v >= 0x10000 and v <= 0xFFFFFFFF then return string.format('%d (0x%08X)', v, v) end
        return tostring(v)
    end
    if t ~= 'table' then return tostring(v) end
    if seen[v] then return '<cycle ' .. tostring(v) .. '>' end
    if level >= depth then return tostring(v) end
    seen[v] = true
    local keys = {}
    for k in pairs(v) do keys[#keys + 1] = k end
    table.sort(keys, function(a, b)
        local ta, tb = type(a), type(b)
        if ta ~= tb then return ta < tb end
        if ta == 'number' or ta == 'string' then return a < b end
        return tostring(a) < tostring(b)
    end)
    if #keys == 0 then seen[v] = nil return '{}' end
    local lines, inner = {}, indent .. '  '
    for i, k in ipairs(keys) do
        if i > 200 then lines[#lines + 1] = inner .. '... ' .. (#keys - 200) .. ' more' break end
        lines[#lines + 1] = inner .. key_text(k) .. ' = ' .. show(v[k], level + 1, inner)
    end
    seen[v] = nil
    return '{\n' .. table.concat(lines, ',\n') .. '\n' .. indent .. '}'
end
local out = {}
if #printed > 0 then out[#out + 1] = table.concat(printed, '\n') end
if not ok then return false, table.concat(out, '\n'), tostring(results[1]) end
for i = 1, results.n do out[#out + 1] = show(results[i], 0, '') end
return true, table.concat(out, '\n'), nil
)LUA";

    const std::lock_guard lock(g_lock);
    enter_thread("the MCP server's lua");
    const std::string wanted = lower(name);
    const auto named = [&]() -> Addon* {
        for (auto& each : g_addons)
            if (lower(each->context.name) == wanted && !each->unload_requested)
                return each.get();
        return nullptr;
    };
    Addon* addon = named();
    if (addon != nullptr && addon->crashed && wanted == "mcp" && g_depth == 0) {
        remove(*addon); // its own state, crashed: a fresh one
        addon = nullptr;
    }
    if (addon == nullptr && wanted == "mcp") {
        load("mcp", &source_of(Flavour::VanaCore));
        addon = named();
    }
    if (addon == nullptr) {
        error = name + " isn't loaded" + (wanted == "mcp" ? " (is addons\\vanacore\\mcp\\mcp.lua there?)" : "");
        return false;
    }
    if (addon->crashed) {
        error = addon->display + " crashed earlier, so its Lua can't run (/addon reload " + addon->display + ")";
        return false;
    }
    lua_State* L = addon->L;
    const int top = lua_gettop(L);
    const std::string expression = "return " + code;
    if (luaL_loadbuffer(L, expression.data(), expression.size(), "=mcp") != 0) {
        lua_pop(L, 1);
        if (luaL_loadbuffer(L, code.data(), code.size(), "=mcp") != 0) {
            error = lua_tostring(L, -1) != nullptr ? lua_tostring(L, -1) : "doesn't compile";
            lua_settop(L, top);
            return false;
        }
    }
    if (luaL_loadbuffer(L, kRunner, sizeof kRunner - 1, "=mcp runner") != 0) {
        error = lua_tostring(L, -1) != nullptr ? lua_tostring(L, -1) : "the runner doesn't compile";
        lua_settop(L, top);
        return false;
    }
    lua_insert(L, -2); // runner(code, depth, traceback): the addon's own `debug` may not be the library (SimpleLog's is false)
    lua_pushinteger(L, depth);
    lua_pushcfunction(L, &traceback);
    std::string failure;
    if (!call(*addon, 3, 3, failure)) {
        error = failure;
        if (!addon->crashed)
            lua_settop(L, top);
        process_requests();
        return false;
    }
    const bool ok = lua_toboolean(L, -3) != 0;
    size_t length = 0;
    const char* text = lua_tolstring(L, -2, &length);
    output.assign(text != nullptr ? text : "", length);
    if (!ok) {
        const char* message = lua_tostring(L, -1);
        error = message != nullptr ? message : "error";
    }
    lua_settop(L, top);
    process_requests();
    return ok;
}

int ask(const std::string& name, const char* code)
{
    const std::lock_guard lock(g_lock);
    enter_thread("a number from an addon's lua");
    const std::string wanted = lower(name);
    Addon* addon = nullptr;
    for (auto& each : g_addons)
        if (lower(each->context.name) == wanted && !each->unload_requested && !each->crashed) {
            addon = each.get();
            break;
        }
    if (addon == nullptr || addon->L == nullptr)
        return 0;
    lua_State* L = addon->L;
    const int top = lua_gettop(L);
    if (luaL_loadbuffer(L, code, std::strlen(code), "=vanacore ask") != 0) {
        lua_settop(L, top);
        return 0;
    }
    std::string failure;
    int value = 0;
    if (call(*addon, 0, 1, failure))
        value = lua_isnumber(L, -1) ? static_cast<int>(lua_tointeger(L, -1)) : lua_toboolean(L, -1) != 0 ? 1 : 0;
    if (!addon->crashed)
        lua_settop(L, top);
    process_requests();
    return value;
}

int ask_numbers(const std::string& name, const char* code, double* values, int most)
{
    const std::lock_guard lock(g_lock);
    enter_thread("numbers from an addon's lua");
    const std::string wanted = lower(name);
    Addon* addon = nullptr;
    for (auto& each : g_addons)
        if (lower(each->context.name) == wanted && !each->unload_requested && !each->crashed) {
            addon = each.get();
            break;
        }
    if (addon == nullptr || addon->L == nullptr || most <= 0)
        return 0;
    lua_State* L = addon->L;
    const int top = lua_gettop(L);
    if (luaL_loadbuffer(L, code, std::strlen(code), "=vanacore ask") != 0) {
        lua_settop(L, top);
        return 0;
    }
    std::string failure;
    int count = 0;
    if (call(*addon, 0, most, failure))
        while (count < most && lua_type(L, -most + count) == LUA_TNUMBER) {
            values[count] = lua_tonumber(L, -most + count);
            ++count;
        }
    if (!addon->crashed)
        lua_settop(L, top);
    process_requests();
    return count;
}

bool on_mouse(int type, int x, int y, int game_x, int game_y, int delta, bool blocked, const std::string* by_gui)
{
    const std::lock_guard lock(g_lock);
    if (g_depth != 0)
        return false; // a message pumped while Lua runs: not into Lua again
    enter_thread("the mouse");
    // The keys held with it: 1 Shift, 2 Ctrl, 4 Alt
    const int modifiers = ((GetKeyState(VK_SHIFT) & 0x8000) != 0 ? 1 : 0) | ((GetKeyState(VK_CONTROL) & 0x8000) != 0 ? 2 : 0) |
                          ((GetKeyState(VK_MENU) & 0x8000) != 0 ? 4 : 0);
    bool taken = false;
    for (size_t i = 0; i < g_addons.size(); ++i) {
        Addon& addon = *g_addons[i];
        // ImGui's (see addons.hpp): timers' wheel handler, with the /vana window over its panel, scrolled the panel's
        // opacity and failed ("Math namespace does not contain a definition for: 1", its opacity is a number). Over its own
        // window it hears it: fancychat scrolls its chat window by its wheel event, not ImGui's.
        if (addon.unload_requested || !addon.mouse ||
            (by_gui != nullptr && addon.context.flavour == Flavour::Ashita && *by_gui != "ashita/" + addon.context.name))
            continue;
        lua_State* L = addon.L;
        push_hook(addon, "raise");
        lua_pushliteral(L, "mouse");
        lua_pushinteger(L, type);
        const bool game_pixels = addon.context.flavour == Flavour::Ashita;
        lua_pushinteger(L, game_pixels ? game_x : x);
        lua_pushinteger(L, game_pixels ? game_y : y);
        lua_pushinteger(L, delta);
        lua_pushboolean(L, blocked || taken);
        lua_pushinteger(L, modifiers);
        std::string error;
        if (call(addon, 7, 1, error)) {
            taken = taken || lua_toboolean(L, -1) != 0;
            lua_pop(L, 1);
        } else {
            report_call(addon, "mouse", error);
        }
    }
    process_requests();
    return taken;
}

bool on_key(WPARAM wparam, LPARAM lparam, bool blocked)
{
    const std::lock_guard lock(g_lock);
    if (g_depth != 0)
        return false; // a message pumped while Lua runs: not into Lua again
    enter_thread("the keyboard");
    // While typing in the chat line (keyboard.blockbindsduringinput on), Ashita and Windower addons get no key presses, as
    // binds wait then: XIUI's hotbar keys fired as their digits were typed. Releases still go, so a key held before is let
    // go. VanaCore's own (the /vana search boxes) still get them, and key_data / key_state are untouched.
    const bool typing = (lparam & (1u << 31)) == 0 && binds::typing_blocks();
    bool taken = false;
    for (size_t i = 0; i < g_addons.size(); ++i) {
        Addon& addon = *g_addons[i];
        if (addon.unload_requested || !addon.key || (typing && addon.context.flavour != lua_api::Flavour::VanaCore))
            continue;
        lua_State* L = addon.L;
        push_hook(addon, "raise");
        lua_pushliteral(L, "key");
        lua_pushinteger(L, static_cast<lua_Integer>(wparam));
        lua_pushinteger(L, static_cast<lua_Integer>(static_cast<std::uint32_t>(lparam)));
        lua_pushboolean(L, blocked || taken);
        std::string error;
        if (call(addon, 4, 1, error)) {
            taken = taken || lua_toboolean(L, -1) != 0;
            lua_pop(L, 1);
        } else {
            report_call(addon, "key", error);
        }
    }
    process_requests();
    return taken;
}

bool wants_packets()
{
    const std::lock_guard lock(g_lock);
    for (const auto& addon : g_addons)
        if (addon->packets && !addon->unload_requested && !addon->crashed)
            return true;
    return false;
}

bool on_packet(bool incoming, std::uint16_t id, std::uint32_t size, const std::uint8_t* data, std::uint8_t* modified,
               std::uint32_t chunk_size, const std::uint8_t* chunk, bool injected, bool blocked,
               const std::vector<std::string>& skip)
{
    const std::lock_guard lock(g_lock);
    if (g_depth != 0)
        return blocked; // a packet while Lua runs (an addon's own call): not into Lua again
    enter_thread("a packet");
    for (size_t i = 0; i < g_addons.size(); ++i) {
        Addon& addon = *g_addons[i];
        if (addon.unload_requested || !addon.packets)
            continue;
        // Ashita runs an addon's handler for the packet it adds inside its Add...Packet call, where addons like
        // LuAshitacast (which block a cast and add it again once the gear is changed) have a flag that ignores it.
        // VanaCore adds it to the next buffer, with the flag long cleared, so the addon is left out here instead.
        if (!skip.empty() && addon.context.flavour == lua_api::Flavour::Ashita &&
            std::find(skip.begin(), skip.end(), "addon " + addon.context.name) != skip.end())
            continue;
        blocked = raise_packet(addon, incoming, id, size, data, modified, chunk_size, chunk, injected, blocked);
    }
    process_requests();
    return blocked;
}

void on_frame()
{
    const std::lock_guard lock(g_lock);
    enter_thread("a frame");
    g_in_frame = true;
    // /addon load and reload run since the last frame (see addon_command)
    while (!g_frame_commands.empty()) {
        const FrameCommand command = std::move(g_frame_commands.front());
        g_frame_commands.pop_front();
        addon_command(command.parts, command.typed);
    }
    if (!g_booted) {
        g_booted = true;
        // VanaCore's own window (/vana) and VanaStore, unless switched off ([addons] vana=0, vanastore=0; one whose
        // folder isn't there is skipped), and keybinds, always, before what boot.txt loads
        std::string display;
        for (const char* name : kOwnDefaults) {
            const std::wstring key(name, name + strlen(name));
            const bool on = is_always_on(name) || g_settings_ini.empty() ||
                            GetPrivateProfileIntW(L"addons", key.c_str(), 1, g_settings_ini.c_str()) != 0;
            if (on && locate(name, &source_of(Flavour::VanaCore), display) != nullptr)
                load(name, &source_of(Flavour::VanaCore));
        }
        // [addons] delay.login (default 1): boot.txt's addons wait for a character to be in the world, once per game; its
        // plugins load now (see Lines)
        if (!g_settings_ini.empty() && GetPrivateProfileIntW(L"addons", L"delay.login", 1, g_settings_ini.c_str()) != 0) {
            g_boot_pending = true;
            log::write(L"Addons: scripts\\boot.txt's plugins load now; the rest waits for a character to log in ([addons] delay.login)");
            run_script("boot.txt", true, Lines::Plugins);
        } else {
            run_script("boot.txt", true);
            queue_after("//keybinds apply");
        }
    }
    if (g_boot_pending && GetTickCount64() >= g_next_login_check) {
        g_next_login_check = GetTickCount64() + 250;
        const std::string character = game::local_player_name();
        if (!character.empty()) {
            g_boot_pending = false;
            log::write(L"Addons: %hs is in; running the rest of scripts\\boot.txt", character.c_str());
            run_script("boot.txt", true, Lines::Rest);
            queue_after("//keybinds apply");
        }
    }
    // Once per game start, when a character is in: a warning if its sort-order caches list a slot twice (see usercaches.hpp)
    if (!g_caches_checked && GetTickCount64() >= g_next_cache_check) {
        g_next_cache_check = GetTickCount64() + 250;
        if (const std::uint32_t character = game::local_player_id(); character != 0) {
            g_caches_checked = true;
            const std::string caches = usercaches::duplicated(character);
            if (!caches.empty())
                say_error("Possible inventory corruption (" + caches + "). If you see duplicated items, please message support on Discord.");
        }
    }
    run_scripts();
    // plugin events since the last frame: 'plugin_event'
    while (!g_plugin_events.empty()) {
        const auto [name, data] = std::move(g_plugin_events.front());
        g_plugin_events.pop_front();
        raise_table(
            "plugin_event", [](const Addon&) { return true; },
            [&](lua_State* L) {
                lua_pushlstring(L, name.data(), name.size());
                lua_setfield(L, -2, "name");
                lua_pushlstring(L, data.data(), data.size());
                lua_setfield(L, -2, "data");
                lua_pushlightuserdata(L, const_cast<char*>(data.data()));
                lua_setfield(L, -2, "data_raw");
                lua_pushinteger(L, static_cast<lua_Integer>(data.size()));
                lua_setfield(L, -2, "size");
            },
            [](lua_State*) {}, false);
    }
    // gameui.cpp's since the last frame: 'gameui_button', 'gameui_replaced' (e.element) and 'gameui_trigger' (e.trigger,
    // e.menu, e.key, e.item, e.name, e.caught), each to its one addon
    while (!g_gameui_events.empty()) {
        const GameuiEvent queued = std::move(g_gameui_events.front());
        g_gameui_events.pop_front();
        raise_table(
            queued.event.c_str(),
            [&](const Addon& addon) { return gameui::key(addon.context.source, addon.context.name) == queued.key; },
            [&](lua_State* L) { push_gameui_event(L, queued); }, [](lua_State*) {}, false);
    }
    // messages other VanaCore instances' addons sent since the last frame (ipc.hpp): 'ipc' (e.message), to the addon of
    // the sender's name
    for (const ipc::Message& message : ipc::receive()) {
        raise_table(
            "ipc", [&](const Addon& addon) { return lower(addon.context.name) == message.addon; },
            [&](lua_State* L) {
                lua_pushlstring(L, message.text.data(), message.text.size());
                lua_setfield(L, -2, "message");
            },
            [](lua_State*) {}, false);
    }
    raise_game_events(); // 'zone_change', 'job_change', 'main_menu_open', ... (vana.on)
    for (size_t i = 0; i < g_addons.size(); ++i) {
        Addon& addon = *g_addons[i];
        if (addon.unload_requested)
            continue;
        const stats::AddonScope timing(addon.context.name); // /vana stats: each addon's frame, when it's open
        gui::addon_frame_begin(); // an error between its Begin and End stays its own
        push_hook(addon, "frame");
        std::string error;
        if (!call(addon, 0, 0, error))
            report_call(addon, "frame", error);
        gui::addon_frame_end();
    }
    process_requests();
    if (addon_trace::any()) {
        std::vector<std::string> names;
        for (const auto& addon : g_addons)
            if (!addon->unload_requested)
                names.push_back(lower(addon->context.name));
        addon_trace::tick(names);
    }
    g_in_frame = false;
}

bool wants_gamepad()
{
    const std::lock_guard lock(g_lock);
    for (const auto& addon : g_addons)
        if (addon->gamepad && !addon->unload_requested && !addon->crashed)
            return true;
    return false;
}

bool wants_draws()
{
    return g_draw_listeners.load(std::memory_order_relaxed) > 0;
}

bool wants_scenes()
{
    return g_scene_listeners.load(std::memory_order_relaxed) > 0;
}

void on_scene(bool begin, bool back_buffer)
{
    if (!wants_scenes())
        return;
    const std::lock_guard lock(g_lock);
    if (g_depth != 0)
        return;
    const char* event = begin ? "beginscene" : "endscene";
    enter_thread(event);
    for (size_t i = 0; i < g_addons.size(); ++i) {
        Addon& addon = *g_addons[i];
        if (addon.unload_requested || addon.crashed || !addon.scenes)
            continue;
        const stats::AddonScope timing(addon.context.name);
        push_hook(addon, "raise");
        lua_pushstring(addon.L, event);
        lua_pushboolean(addon.L, back_buffer);
        std::string error;
        if (!call(addon, 2, 0, error))
            report_call(addon, event, error);
    }
    process_requests();
}

namespace {

// A draw call's event to the addons listening for draws: true if one blocked it.
template <typename Fill>
bool raise_draw(const char* event, Fill fill)
{
    if (!wants_draws())
        return false;
    return raise_table(event, [](const Addon& addon) { return addon.draws; }, fill, [](lua_State*) {}, false);
}

void set_integer(lua_State* L, const char* name, std::uint32_t value)
{
    lua_pushnumber(L, value);
    lua_setfield(L, -2, name);
}

void set_pointer(lua_State* L, const char* name, const void* value)
{
    lua_pushlightuserdata(L, const_cast<void*>(value));
    lua_setfield(L, -2, name);
}

} // namespace

bool on_draw_primitive(std::uint32_t type, std::uint32_t start_vertex, std::uint32_t count)
{
    return raise_draw("d3d_dp", [&](lua_State* L) {
        set_integer(L, "primitive_type", type);
        set_integer(L, "start_vertex", start_vertex);
        set_integer(L, "primitive_count", count);
    });
}

bool on_draw_indexed_primitive(std::uint32_t type, std::uint32_t min_index, std::uint32_t num_vertices, std::uint32_t start_index,
                               std::uint32_t count)
{
    return raise_draw("d3d_dip", [&](lua_State* L) {
        set_integer(L, "primitive_type", type);
        set_integer(L, "min_index", min_index);
        set_integer(L, "num_vertices", num_vertices);
        set_integer(L, "start_index", start_index);
        set_integer(L, "primitive_count", count);
    });
}

bool on_draw_primitive_up(std::uint32_t type, std::uint32_t count, const void* vertices, std::uint32_t stride)
{
    return raise_draw("d3d_dpup", [&](lua_State* L) {
        set_integer(L, "primitive_type", type);
        set_integer(L, "primitive_count", count);
        set_pointer(L, "vertex_stream_zero_data", vertices);
        set_integer(L, "vertex_stream_zero_stride", stride);
    });
}

bool on_draw_indexed_primitive_up(std::uint32_t type, std::uint32_t min_index, std::uint32_t num_vertices, std::uint32_t count,
                                  const void* indices, std::uint32_t index_format, const void* vertices, std::uint32_t stride)
{
    return raise_draw("d3d_dipup", [&](lua_State* L) {
        set_integer(L, "primitive_type", type);
        set_integer(L, "min_vertex_index", min_index);
        set_integer(L, "num_vertex_indices", num_vertices);
        set_integer(L, "primitive_count", count);
        set_pointer(L, "index_data", indices);
        set_integer(L, "index_data_format", index_format);
        set_pointer(L, "vertex_stream_zero_data", vertices);
        set_integer(L, "vertex_stream_zero_stride", stride);
    });
}

bool on_gamepad_button(bool xinput, std::uint32_t& button, std::int32_t& value, bool blocked, bool injected)
{
    if (!wants_gamepad())
        return blocked;
    return raise_table(
        xinput ? "xinput_button" : "dinput_button", [](const Addon& addon) { return addon.gamepad; },
        [&](lua_State* L) {
            lua_pushinteger(L, button);
            lua_setfield(L, -2, "button");
            lua_pushinteger(L, value);
            lua_setfield(L, -2, "state");
            lua_pushboolean(L, injected);
            lua_setfield(L, -2, "injected");
        },
        [&](lua_State* L) {
            lua_getfield(L, -1, "button");
            if (lua_isnumber(L, -1))
                button = static_cast<std::uint32_t>(lua_tointeger(L, -1));
            lua_pop(L, 1);
            lua_getfield(L, -1, "state");
            if (lua_isnumber(L, -1))
                value = static_cast<std::int32_t>(lua_tointeger(L, -1));
            lua_pop(L, 1);
        },
        blocked);
}

// Each XInputGetState read (gamepad.cpp), as Ashita's 'xinput_state': e.user (the controller, 0-3) and e.state, the
// XINPUT_STATE the game is about to read (16 bytes, a pointer for the FFI: XIUI casts it to XINPUT_STATE*), which a
// handler may change in place. Ashita 4.30 has `state_modified` too, the copy an addon writes to hide a button from the
// game (XIUI's crossbar strips L2/R2 and its face buttons there): the same buffer here, which is what the game reads.
void on_xinput_state(std::uint32_t user, void* state)
{
    if (!wants_gamepad())
        return;
    raise_table(
        "xinput_state", [](const Addon& addon) { return addon.gamepad; },
        [&](lua_State* L) {
            lua_pushinteger(L, user);
            lua_setfield(L, -2, "user");
            lua_pushlightuserdata(L, state);
            lua_setfield(L, -2, "state");
            lua_pushlightuserdata(L, state);
            lua_setfield(L, -2, "state_modified");
        },
        [](lua_State*) {}, false);
}

void on_dinput_state(void* device, DWORD size, void* state)
{
    if (state == nullptr || !wants_gamepad())
        return;
    raise_table(
        "dinput_state", [](const Addon& addon) { return addon.gamepad; },
        [&](lua_State* L) {
            lua_pushlightuserdata(L, device);
            lua_setfield(L, -2, "device");
            lua_pushinteger(L, size);
            lua_setfield(L, -2, "size");
            lua_pushlightuserdata(L, state);
            lua_setfield(L, -2, "state");
            lua_pushlightuserdata(L, state);
            lua_setfield(L, -2, "state_modified");
        },
        [](lua_State*) {}, false);
}

bool wants_key_data()
{
    const std::lock_guard lock(g_lock);
    for (const auto& addon : g_addons)
        if (addon->keys && !addon->unload_requested && !addon->crashed)
            return true;
    return false;
}

void on_key_state(void* keys, DWORD size)
{
    if (keys == nullptr || !wants_key_data())
        return;
    raise_table(
        "key_state", [](const Addon& addon) { return addon.keys; },
        [&](lua_State* L) {
            lua_pushlstring(L, static_cast<const char*>(keys), size);
            lua_setfield(L, -2, "data");
            lua_pushlightuserdata(L, keys); // the addon may write it through the FFI: what the game reads
            lua_setfield(L, -2, "data_raw");
            lua_pushinteger(L, size);
            lua_setfield(L, -2, "size");
        },
        [](lua_State*) {}, false);
}

const std::string& blocked_by()
{
    return g_blocked_by;
}

bool on_key_data(DWORD key, bool down, bool blocked)
{
    return raise_table(
        "key_data", [](const Addon& addon) { return addon.keys; },
        [&](lua_State* L) {
            lua_pushinteger(L, key);
            lua_setfield(L, -2, "key");
            lua_pushboolean(L, down);
            lua_setfield(L, -2, "down");
            lua_pushboolean(L, false);
            lua_setfield(L, -2, "injected");
        },
        [](lua_State*) {}, blocked);
}

void note_plugin_event(lua_State* L, const std::string& name, const std::string& data)
{
    // minimap's opacity event: a command (1) and the map, frame, arrow, monsters, npcs and players opacities
    if (name != "minimap" || data.size() < 4 + 6 * sizeof(float))
        return;
    std::uint32_t command = 0;
    std::memcpy(&command, data.data(), sizeof(command));
    if (command != 1)
        return;
    float opacity[6];
    std::memcpy(opacity, data.data() + sizeof(command), sizeof(opacity));
    bool dimmed = false;
    for (const float each : opacity)
        dimmed = dimmed || each < 1.0f;
    if (Addon* addon = owner(L))
        addon->minimap_dimmed = dimmed;
}

void queue_plugin_event(const std::string& name, const std::string& data)
{
    const std::lock_guard lock(g_lock);
    if (g_plugin_events.size() < 256)
        g_plugin_events.emplace_back(name, data);
}

void queue_event(const std::string& key, const std::string& event, const std::string& element)
{
    const std::lock_guard lock(g_lock);
    if (g_gameui_events.size() < 256)
        g_gameui_events.push_back({key, event, element});
}

void queue_trigger(const std::string& key, int trigger, const std::string& menu, int event, int item, const std::string& name,
                   bool caught)
{
    const std::lock_guard lock(g_lock);
    if (g_gameui_events.size() < 256)
        g_gameui_events.push_back({key, "gameui_trigger", menu, trigger, event, item, name, caught});
}

void queue_window(const std::string& key, const std::string& window, const char* what)
{
    const std::lock_guard lock(g_lock);
    if (g_gameui_events.size() < 256)
        g_gameui_events.push_back({key, "gameui_window", window, 0, 0, 0, what, false});
}

void shutdown(bool as_game_thread)
{
    const std::lock_guard lock(g_lock);
    const bool close = as_game_thread && g_depth == 0;
    g_closing = true;
    for (size_t i = g_addons.size(); i-- > 0;) {
        if (i >= g_addons.size())
            continue;
        Addon& addon = *g_addons[i];
        if (close) {
            unload(addon, true);
        } else if (!addon.crashed && !addon.unload_raised) {
            raise(addon, "unload");
            addon.unload_raised = addon.unload_requested = true;
        }
    }
}

bool is_game_thread()
{
    return g_thread == 0 || GetCurrentThreadId() == g_thread;
}

} // namespace vanacore::addons
