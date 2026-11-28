#include "plugins.hpp"

#include <windows.h>

#include <cctype>
#include <cstdio>
#include <string>
#include <string_view>

#include "addons.hpp"
#include "binds.hpp"
#include "chat.hpp"
#include "compat/ashita/host.hpp"
#include "compat/windower/host.hpp"
#include "log.hpp"
#include "plugin_memory.hpp"

namespace vanacore::plugins {
namespace {

std::string g_install; // VanaCore's folder, with a trailing backslash (ANSI)

// Where plugins live: a folder per kind under <VanaCore>\plugins, searched in this order when a
// load doesn't say which.
struct Source {
    const char* word;   // as a command names it: /plugin load findall ashita
    const char* folder; // under VanaCore's folder
    const char* label;  // in messages: (Ashita Plugin)
};
constexpr Source kSources[] = {
    {"ashita", "plugins\\ashita\\", "Ashita"},
    {"windower", "plugins\\windower\\", "Windower"},
};

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

// Ashita plugins that can't work anywhere but Ashita, and why.
struct Refused {
    const char* name;
    const char* reason;
};
constexpr Refused kRefused[] = {
    // Ashita's own Lua host.
    {"addons", "it's Ashita's own addon host; VanaCore runs Ashita addons itself (/addon load <name>)"},
    // Finds what to hook by reading an offset out of Ashita's own packet hook code.
    {"sequencer", "it patches Ashita's own packet code, which VanaCore doesn't have"},
};

const Source* source_named(std::string_view word)
{
    for (const Source& source : kSources)
        if (word == source.word)
            return &source;
    return nullptr;
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

// Ashita plugins that ship with VanaCore, in its own folder: the build replaces them on every update, and the launcher's
// mirror of Ashita's plugins (plugins\ashita) never touches them. One here is loaded instead of a same-named one in
// plugins\ashita (CatsEyeXI's cexidats, whose Ashita copy a player may not have updated), as an Ashita plugin.
constexpr const char* kShippedFolder = "plugins\\vanacore\\";

// The source with <name>.dll (only `only`, if given), and the name as its file spells it
// ("FindAll" for /plugin load findall), and the folder it's in (under VanaCore's). Nullptr if none has it.
const Source* locate(const std::string& name, const Source* only, std::string& display, std::string& folder)
{
    const auto find = [&](const char* where) {
        WIN32_FIND_DATAA found{};
        const HANDLE handle = FindFirstFileA((g_install + where + name + ".dll").c_str(), &found);
        if (handle == INVALID_HANDLE_VALUE)
            return false;
        FindClose(handle);
        if ((found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
            return false;
        display = found.cFileName;
        display.resize(display.size() - 4); // ".dll"
        folder = where;
        return true;
    };
    if (addons::ashita_enabled() && (only == nullptr || only == &kSources[0]) && find(kShippedFolder))
        return &kSources[0];
    for (const Source& source : kSources) {
        if (&source == &kSources[0] && !addons::ashita_enabled())
            continue;
        if (only != nullptr && &source != only)
            continue;
        if (find(source.folder))
            return &source;
    }
    return nullptr;
}

std::string version_text(double version)
{
    char text[32];
    sprintf_s(text, "%.2f", version);
    std::string out(text);
    while (out.size() > 3 && out.back() == '0') // 1.10 -> 1.1, 1.00 -> 1.0
        out.pop_back();
    return out;
}

// A loaded plugin of either kind.
struct Loaded {
    const Source* source = nullptr;
    std::string display;
    std::string details; // " 1.16 by Author"
};

// Plugins held off (hold()): the name in lower case, why, and whether it was loaded or asked for since (with the kind and
// arguments it was asked for with)
struct Held {
    std::string name;
    std::string why;
    bool wanted = false;
    std::string kind;
    std::string args;
};
std::vector<Held> g_held;
unsigned g_changes = 0; // changes()
bool g_silent = false;     // [addons] plugins.silent, Ashita's SetSilentPlugins: no chat line as a plugin loads or unloads
bool g_silent_ini = false; // plugins.silent as last read: SetSilentPlugins holds until the setting changes

bool loaded(const std::string& name, Loaded& out)
{
    ashita::Info a;
    if (ashita::info(name, a)) {
        out = {&kSources[0], a.display, " " + version_text(a.version) + (a.author.empty() ? "" : " by " + a.author)};
        return true;
    }
    windower::Info w;
    if (windower::info(name, w)) {
        out = {&kSources[1], w.display, w.author.empty() ? std::string() : " by " + w.author};
        return true;
    }
    return false;
}

} // namespace

bool present(const std::string& name)
{
    std::string display, folder;
    return locate(lower(name), nullptr, display, folder) != nullptr;
}

void install(const wchar_t* install_dir, const wchar_t* settings_ini)
{
    g_install = ansi(install_dir) + "\\";
    // The folders, so it's clear where plugins go.
    CreateDirectoryA((g_install + "plugins").c_str(), nullptr);
    for (const Source& source : kSources) {
        if (&source != &kSources[0] || addons::ashita_enabled())
            CreateDirectoryA((g_install + source.folder).c_str(), nullptr);
    }
    g_silent = settings_ini != nullptr && GetPrivateProfileIntW(L"addons", L"plugins.silent", 0, settings_ini) != 0;
    g_silent_ini = g_silent;
    plugin_memory::install();
    if (addons::ashita_enabled())
        ashita::install(g_install, settings_ini != nullptr ? settings_ini : L"");
    windower::install(g_install);
    log::write(L"Plugins: from %ls\\plugins (Ashita compatibility %ls; Windower 4 and 3)",
               install_dir, addons::ashita_enabled() ? L"enabled" : L"disabled");
}

void reload(const wchar_t* ini)
{
    const bool silent = GetPrivateProfileIntW(L"addons", L"plugins.silent", 0, ini) != 0;
    if (silent == g_silent_ini)
        return;
    g_silent_ini = g_silent = silent;
    log::write(L"Plugins: [addons] plugins.silent=%d", silent ? 1 : 0);
}

bool silent()
{
    return g_silent;
}

void set_silent(bool silent)
{
    g_silent = silent;
}

bool load(const std::string& name_in, const std::string& kind, const std::string& args)
{
    const std::string name = lower(name_in);
    if (!valid_name(name)) {
        say_error("\"" + name_in + "\" isn't a plugin name");
        return false;
    }
    for (Held& held : g_held) {
        if (held.name == name) {
            held.wanted = true, held.kind = kind, held.args = args;
            say(name_in + " isn't loaded: " + held.why);
            log::write(L"Plugins: %hs isn't loaded: %hs", name.c_str(), held.why.c_str());
            return false;
        }
    }
    const Source* only = kind.empty() ? nullptr : source_named(lower(kind));
    if (!kind.empty() && only == nullptr) {
        say_error("\"" + kind + "\" isn't a plugin kind (ashita or windower)");
        return false;
    }
    if (only == &kSources[0] && !addons::ashita_enabled()) {
        say_error("[addons] ashita.enabled=0: Ashita plugin compatibility is disabled in this profile.");
        return false;
    }
    Loaded already;
    if (loaded(name, already)) {
        say(already.display + " is already loaded (/plugin reload " + name + " to restart it)");
        return false;
    }
    std::string display, folder;
    const Source* source = locate(name, only, display, folder);
    if (source == nullptr) {
        if (only != nullptr)
            say_error(std::string("No ") + only->label + " plugin " + name + ": " + only->folder + name + ".dll isn't there");
        else
            say_error("No plugin " + name + " in plugins\\ashita or plugins\\windower");
        log::write(L"Plugins: %hs not found under %hsplugins", name.c_str(), g_install.c_str());
        return false;
    }
    const std::string label = std::string("(") + source->label + " Plugin) ";
    const std::string path = g_install + folder + display + ".dll";
    const bool is_windower = std::string_view(source->word) == "windower";
    for (const Refused& refused : kRefused) {
        if (!is_windower && name == refused.name) {
            say_error(label + "Not loading " + display + ": " + refused.reason);
            log::write(L"Plugins: %hs not loaded: %hs", display.c_str(), refused.reason);
            return false;
        }
    }
    log::write(L"Plugins: loading %hs (%hs plugin%hs)", path.c_str(), source->label,
               folder == kShippedFolder ? ", shipped with VanaCore" : "");
    std::string error;
    if (is_windower ? !windower::load(display, path, error) : !ashita::load(display, path, args, error)) {
        log::write(L"Plugins: %hs wasn't loaded: %hs", display.c_str(), error.c_str());
        say_error(label + "Couldn't load " + display + ": " + error);
        return false;
    }
    ++g_changes;
    Loaded details;
    loaded(name, details);
    if (!g_silent)
        say(label + chat::colour(chat::kSuccess, "Loaded: ") + details.display + details.details);
    return true;
}

bool unload(const std::string& name_in)
{
    const std::string name = lower(name_in);
    Loaded details;
    if (!loaded(name, details)) {
        say(name_in + " isn't loaded");
        return false;
    }
    if (details.source == &kSources[1])
        windower::unload(name);
    else
        ashita::unload(name);
    ++g_changes;
    if (!g_silent)
        say(std::string("(") + details.source->label + " Plugin) " + chat::colour(chat::kSuccess, "Unloaded: ") + details.display);
    return true;
}

void hold(const std::string& name_in, const std::string& why)
{
    const std::string name = lower(name_in);
    Held* held = nullptr;
    for (Held& each : g_held)
        if (each.name == name)
            held = &each;
    if (held == nullptr) {
        g_held.push_back({name, why});
        held = &g_held.back();
    }
    held->why = why;
    Loaded details;
    if (!loaded(name, details))
        return;
    held->wanted = true;
    held->kind = details.source->word;
    if (details.source == &kSources[1])
        windower::unload(name);
    else
        ashita::unload(name);
    ++g_changes;
    say(std::string("(") + details.source->label + " Plugin) " + details.display + " unloaded: " + why);
    log::write(L"Plugins: %hs unloaded: %hs", details.display.c_str(), why.c_str());
}

void release(const std::string& name_in)
{
    const std::string name = lower(name_in);
    for (auto it = g_held.begin(); it != g_held.end(); ++it) {
        if (it->name != name)
            continue;
        const Held held = *it;
        g_held.erase(it);
        Loaded details;
        if (held.wanted && !loaded(name, details))
            load(name, held.kind, held.args);
        return;
    }
}

void shutdown(bool as_game_thread)
{
    if (!as_game_thread) {
        log::write(L"Plugins: the game is closing on another thread than theirs: left as they are");
        return;
    }
    windower::shutdown();
    ashita::shutdown();
}

unsigned changes()
{
    return g_changes;
}

bool details(const std::string& name, Details& out)
{
    ashita::Info a;
    if (ashita::info(name, a)) {
        out = {"Ashita plugin", version_text(a.version) + (a.author.empty() ? "" : " by " + a.author) + ", for Ashita interface " +
                                    version_text(a.interface_version),
               a.crashed};
        return true;
    }
    windower::Info w;
    if (windower::info(name, w)) {
        out = {"Windower plugin", w.author.empty() ? std::string() : "by " + w.author, w.crashed};
        return true;
    }
    return false;
}

std::vector<std::pair<std::string, std::string>> loaded_list()
{
    std::vector<std::pair<std::string, std::string>> out;
    for (const ashita::Info& plugin : ashita::list())
        out.emplace_back(plugin.display, "ashita");
    for (const windower::Info& plugin : windower::list())
        out.emplace_back(plugin.display, "windower");
    return out;
}

void command(const std::vector<std::string>& parts)
{
    const std::string action = parts.size() > 1 ? lower(parts[1]) : "";
    if (action == "list") {
        std::string names;
        for (const ashita::Info& plugin : ashita::list())
            names += (names.empty() ? "" : ", ") + plugin.display + " " + version_text(plugin.version) + " (Ashita " +
                     version_text(plugin.interface_version) + (plugin.crashed ? ", crashed" : "") + ")";
        for (const windower::Info& plugin : windower::list())
            names += (names.empty() ? "" : ", ") + plugin.display + " (Windower" + (plugin.crashed ? ", crashed" : "") + ")";
        say(names.empty() ? "No plugins are loaded" : "Loaded: " + names);
        return;
    }
    std::vector<std::string> names(parts.size() > 2 ? parts.begin() + 2 : parts.end(), parts.end());
    // A last word naming a kind says where to load from: /plugin load findall ashita.
    std::string kind;
    if (names.size() > 1 && source_named(lower(names.back())) != nullptr) {
        kind = lower(names.back());
        names.pop_back();
    }
    if ((action != "load" && action != "unload" && action != "reload") || names.empty()) {
        say("/plugin load <name> [ashita|windower], /plugin unload <name>, /plugin reload <name>, /plugin list");
        return;
    }
    for (const std::string& name : names) {
        if (action == "load") {
            load(name, kind);
        } else if (action == "unload") {
            if (addons::required(lower(name), true)) {
                say(lower(name) + " is required by SoulspireXI and stays loaded (/plugin reload " + lower(name) + " reloads it)");
                continue;
            }
            unload(name);
        } else {
            // Back from where it was loaded, unless the command names the folder.
            Loaded before;
            const bool was_loaded = loaded(lower(name), before);
            if (was_loaded)
                unload(name);
            load(name, !kind.empty() ? kind : was_loaded ? before.source->word : "");
        }
    }
}

bool on_command(const std::string& line, int mode, bool injected, Commands which, std::string& by)
{
    if (ashita::on_command(line, mode, injected, which, by))
        return true;
    // Windower's take only // lines, and those reach the addons first (addons.cpp)
    if (which != Commands::BeforeAddons && windower::on_command(line)) {
        by = "a Windower plugin";
        return true;
    }
    return false;
}

bool on_mouse(int type, int x, int y, int delta, WPARAM wparam)
{
    const bool blocked = ashita::on_mouse(type, x, y, wparam);
    return windower::on_mouse(type, x, y, delta, blocked) || blocked;
}

bool on_key(WPARAM key, LPARAM flags)
{
    // While typing in the chat line (keyboard.blockbindsduringinput on), plugins get no key presses, as binds wait then.
    // Releases still go. Their DirectInput callbacks (Thirdparty's sent keys) are untouched.
    if ((flags & (1u << 31)) == 0 && binds::typing_blocks())
        return false;
    const bool blocked = ashita::on_key(key, flags);
    return windower::on_key(key, flags, blocked) || blocked;
}

bool wants_gamepad()
{
    return ashita::wants_gamepad();
}

void on_controller_state(void* device, DWORD size, void* data)
{
    ashita::on_controller_state(device, size, data);
}

void on_controller_data(void* device, DWORD object_size, void* data, DWORD* count)
{
    ashita::on_controller_data(device, object_size, data, count);
}

bool on_controller_button(std::uint32_t& offset, std::int32_t& value, bool injected)
{
    return ashita::on_controller_button(offset, value, injected);
}

void on_xinput_state(DWORD user, void* state)
{
    ashita::on_xinput_state(user, state);
}

bool on_xinput_button(std::uint32_t& button, std::int32_t& value, bool injected)
{
    return ashita::on_xinput_button(button, value, injected);
}

void raise_event(const std::string& name, const std::string& data)
{
    ashita::raise_event(name, data);
}

void on_keyboard_state(void* device, DWORD size, void* data)
{
    ashita::on_keyboard_state(device, size, data);
}

void on_keyboard_data(void* device, DWORD object_size, void* data, DWORD* count, DWORD capacity, DWORD flags)
{
    ashita::on_keyboard_data(device, object_size, data, count, capacity, flags);
}

bool on_text(bool incoming, chat::Text& text)
{
    return ashita::on_text(incoming, text);
}

bool on_unhandled(std::string_view line)
{
    return windower::on_unhandled(line);
}

void on_device(void* device)
{
    ashita::on_device(device);
    windower::on_device(device);
}

void on_present(void*, const RECT* source, const RECT* dest, HWND window, const RGNDATA* dirty)
{
    ashita::on_present(source, dest, window, dirty);
}

void on_scene(void* device, bool begin)
{
    ashita::on_scene(device, begin);
}

void draw_objects(void* device)
{
    ashita::draw(device);
}

void draw(void* device)
{
    windower::draw(device);
}

void on_received(const std::uint8_t* buffer, std::size_t size)
{
    windower::on_received(buffer, size);
}

bool wants_packets()
{
    return ashita::wants_packets() || windower::wants_packets();
}

bool on_packet(bool incoming, std::uint16_t id, std::uint32_t size, const std::uint8_t* data, std::uint8_t* modified,
               std::uint32_t chunk_size, const std::uint8_t* chunk, bool injected, bool blocked,
               const std::vector<std::string>& skip)
{
    blocked = ashita::on_packet(incoming, id, size, data, modified, chunk_size, chunk, injected, blocked, skip);
    return windower::on_packet(incoming, id, size, data, modified, injected, blocked);
}

} // namespace vanacore::plugins

namespace vanacore::ashita {

// IPluginManager::Load: a plugin loading another, from plugins\ashita.
bool plugin_load(const char* name, const char* args)
{
    return name != nullptr && plugins::load(name, "ashita", args != nullptr ? args : "");
}

} // namespace vanacore::ashita
