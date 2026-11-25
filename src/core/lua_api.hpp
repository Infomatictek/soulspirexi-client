#pragma once

#include <string>

struct lua_State;

// The native half of the `vana` table every addon sees (the rest is lua/vana.lua).
namespace vanacore::lua_api {

// The API an addon is written for, told apart by the names its main file uses (addons.cpp).
enum class Flavour { VanaCore, Ashita, Windower };

// What the native API knows about the addon a Lua state belongs to.
struct Context {
    Flavour flavour = Flavour::VanaCore;
    std::string name;         // the addon's folder name, lower case
    std::string source;       // the folder it came from: "vanacore", "ashita" or "windower" (gameui.hpp's key with name)
    std::string folder;       // <VanaCore>\addons\<name>\ (ANSI, the encoding Lua's io library uses)
    std::string install;      // VanaCore's folder, with a trailing backslash (ANSI)
    std::wstring settings_ini; // vanacore.ini
    int imgui_numbers = 0;     // whose numbers its ImGui calls pass: 181 (it loaded Ashita 4.16's libs\imgui.lua), 192, 0 not known yet
    double ashita_interface = 4.30; // the Ashita interface an Ashita addon sees ([addons] ashita.interface): 4.30 (default) or 4.16
    double ashita_libs = 4.30;      // the Ashita libraries it loads ([addons] ashita.libs): 4.30 (the default: 4.30's, with ImGui 1.81's names over them) or 4.16
};

// Fills in vana.version, vana.log, vana.memory, vana.fs, vana.chat, vana.ui, vana.game, vana.gameui (gameui.hpp),
// vana.encoding, vana.imgui (lua_imgui.hpp), vana.config, vana.time, vana.system and vana.paths;
// the `addon` table (name, path)
// except for Windower addons; and package.path/cpath: the addon's folder, then its kind's
// libraries (addons\vanacore\libs; addons\windower\libs, then VanaCore's copy of Windower's,
// addons\vanacore\libs\windower\libs; for Ashita addons VanaCore's own:
// addons\vanacore\libs\ashita-4.30, or -4.16 with [addons] ashita.libs=4.16, then addons\vanacore\libs\ashita
// shared by both, then Ashita's addons root (addons\ashita\?.lua); never addons\ashita\libs, and no
// DLLs but the addon's own folder's).
// Ashita addons' context.ashita_interface and ashita_libs are set here from vanacore.ini. `context` must outlive the
// state.
void open(lua_State* L, Context& context);

// The context open() was given for this state.
Context& context(lua_State* L);

} // namespace vanacore::lua_api
