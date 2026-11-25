#include <windows.h>

#include <vanacore/boot_info.hpp>
#include <vanacore/version.hpp>

#include <cstddef>
#include <cstring>
#include <cwchar>
#include <string>

#include "addons.hpp"
#include "abtest.hpp"
#include "zoneload.hpp"
#include "debugrec.hpp"
#include "aliases.hpp"
#include "alliancelist.hpp"
#include "castbar.hpp"
#include "hudwin.hpp"
#include "tpbar.hpp"
#include "noblink.hpp"
#include "fishing.hpp"
#include "autologin.hpp"
#include "macrobar.hpp"
#include "menuhover.hpp"
#include "binds.hpp"
#include "camera.hpp"
#include "commands.hpp"
#include "crash.hpp"
#include "chat.hpp"
#include "chatfont.hpp"
#include "chattabs.hpp"
#include "client.hpp"
#include "configmenu.hpp"
#include "gamesettings.hpp"
#include "singlemenu.hpp"
#include "equipfirst.hpp"
#include "sound.hpp"
#include "soundslots.hpp"
#include "partymenu.hpp"
#include "discord.hpp"
#include "framepacing.hpp"
#include "framerate.hpp"
#include "frametime.hpp"
#include "game_files.hpp"
#include "font_folder.hpp"
#include "game_config.hpp"
#include "graphics.hpp"
#include "gui.hpp"
#include "hideui.hpp"
#include "itemcount.hpp"
#include "itemlist.hpp"
#include "spelllist.hpp"
#include "itemrarity.hpp"
#include "itemslot.hpp"
#include "layout.hpp"
#include "input.hpp"
#include "mousediag.hpp"
#include "loader.hpp"
#include "log.hpp"
#include "mcp.hpp"
#include "helpbar.hpp"
#include "targetwin.hpp"
#include "mapwin.hpp"
#include "buffwin.hpp"
#include "netstat.hpp"
#include "statuswindow.hpp"
#include "profilewin.hpp"
#include "shutdown.hpp"
#include "overlay.hpp"
#include "packetflow.hpp"
#include "packets.hpp"
#include "packetwatch.hpp"
#include "pivot.hpp"
#include "perf.hpp"
#include "hwmon.hpp"
#include "inicache.hpp"
#include "stats.hpp"
#include "plugins.hpp"
#include "registry.hpp"
#include "screenshot.hpp"
#include "session.hpp"
#include "settings.hpp"
#include "uiscale.hpp"
#include "window.hpp"

namespace {

vanacore::BootInfo g_boot{};
bool g_installed = false;

// The settings file's old keys moved, its session copy made (settings.hpp) and its path put in g_boot.settings_ini in
// place of the file's, and VanaCore's reads of the copy answered from memory (inicache.hpp; none without a copy)
void install_settings()
{
    if (g_boot.settings_ini[0] == L'\0')
        return;
    vanacore::registry::migrate(g_boot.settings_ini);
    const std::wstring copy =
        vanacore::settings::install(g_boot.settings_ini, std::wstring(g_boot.install_dir) + L"\\vanacore.ini");
    wcsncpy_s(g_boot.settings_ini, copy.c_str(), _TRUNCATE);
    vanacore::inicache::install(vanacore::settings::copy());
}

// What measures and records the game for the player and for us (/vana abtest, debug, stats; the zone load timeline)
void install_diagnostics()
{
    vanacore::abtest::install(g_boot.settings_ini, g_boot.install_dir);
    vanacore::debugrec::install(g_boot.install_dir);
    vanacore::perf::install(g_boot.install_dir);
    vanacore::stats::install(g_boot.install_dir);
    vanacore::hwmon::install(g_boot.install_dir);
    vanacore::zoneload::install(g_boot.settings_ini, g_boot.install_dir); // after pivot's: its CreateFile hooks call pivot's
}

} // namespace

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID reserved)
{
    // Keep DllMain minimal (loader lock); real start-up happens in VanaCoreInstall.
    if (reason == DLL_PROCESS_ATTACH)
        DisableThreadLibraryCalls(module);
    // The process is ending normally (a crash never gets here): say so, without waiting on a lock
    // a thread killed on the way out may have held.
    if (reason == DLL_PROCESS_DETACH && reserved != nullptr && g_installed)
        vanacore::log::write_from_crash(L"The game process is exiting");
    return TRUE;
}

// Called by VanaCore.exe on a remote thread after the DLL has loaded and before the game's
// main thread resumes. Exported undecorated via VanaCore.Core.def. Returns 0 on success.
extern "C" DWORD WINAPI VanaCoreInstall(LPVOID param)
{
    const auto* boot = static_cast<const vanacore::BootInfo*>(param);
    if (boot == nullptr)
        return 1;
    if (boot->size != sizeof(vanacore::BootInfo) || boot->version != vanacore::kBootInfoVersion) {
        // Every build's startup info begins with the install folder, so the log can still say why.
        if (boot->size >= offsetof(vanacore::BootInfo, settings_ini)) {
            vanacore::log::open(boot->install_dir);
            vanacore::log::write(L"VanaCore.Core v%ls: VanaCore.exe is from a different build (startup info v%u, %u "
                                 L"bytes; this DLL needs v%u, %u bytes). Copy both files from the same build.",
                                 vanacore::kVersion, boot->version, boot->size, vanacore::kBootInfoVersion,
                                 static_cast<unsigned>(sizeof(vanacore::BootInfo)));
        }
        return 1;
    }
    g_boot = *boot;
    g_boot.server[std::size(g_boot.server) - 1] = L'\0';
    vanacore::log::install(g_boot.install_dir); // so /vana logs can turn logging on while the game runs

    // [logging] enabled (on by default): logs\vanacore.log. With it off nothing is logged, printed or dumped.
    // crashdumps: how much a crash leaves behind (crash.hpp).
    const wchar_t* ini = g_boot.settings_ini;
    const bool logging = ini[0] != L'\0' && GetPrivateProfileIntW(L"logging", L"enabled", 1, ini) != 0;
    const int dumps = logging ? static_cast<int>(GetPrivateProfileIntW(L"logging", L"crashdumps", 1, ini)) : 0;
    if (logging)
        vanacore::log::open(g_boot.install_dir);
    if (g_boot.attached != 0)
        vanacore::log::mirror_to_console();
    vanacore::log::write(L"VanaCore.Core v%ls loaded into process %lu (crashdumps=%d)", vanacore::kVersion,
                         GetCurrentProcessId(), dumps);
    vanacore::client::set_server(g_boot.server); // the server's host (gmicons_staff.h's GM_LIST is by it)
    // DEP on for good, before the game's own DLLs load. Windows already keeps it on for VanaCore.exe (it's
    // NX-compatible), so this changes nothing there. Wine instead makes the whole process's data memory executable
    // once a module that isn't NX-compatible loads, and PlayOnline's polcore.dll and app.dll aren't. Under CrossOver
    // on a Mac (DXMT) that had macOS reclaim and pin again, on every frame, the GPU memory the CPU shares with the
    // graphics card, for the first ~3 minutes after every start: the logos and the agreement screen at 1-2 FPS. On a
    // Steam Deck (Proton) it instead stopped PlayOnline starting at all (polcore 0x80040154, VanaCore 0.8.16).
    // So: on when Wine's host is macOS (ntdll's wine_get_host_version says Darwin), off elsewhere, and [client] dep
    // (1 or 0; the /vana Client page's "DEP at start") overrides either way
    {
        wchar_t dep_setting[16] = L"";
        if (ini[0] != L'\0')
            GetPrivateProfileStringW(L"client", L"dep", L"", dep_setting, 16, ini);
        const bool forced_on = wcscmp(dep_setting, L"1") == 0;
        const bool forced_off = wcscmp(dep_setting, L"0") == 0;
        const char* host = nullptr;
        using HostVersion = void(__cdecl*)(const char**, const char**);
        if (const auto wine_host =
                reinterpret_cast<HostVersion>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "wine_get_host_version")))
            wine_host(&host, nullptr);
        const bool mac_host = host != nullptr && _stricmp(host, "Darwin") == 0;
        using SetDep = BOOL(WINAPI*)(DWORD);
        if (const auto set_dep = forced_on || (mac_host && !forced_off)
                                     ? reinterpret_cast<SetDep>(
                                           GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "SetProcessDEPPolicy"))
                                     : nullptr) {
            const BOOL dep_on = set_dep(1 /* PROCESS_DEP_ENABLE */);
            vanacore::log::write(L"Memory: DEP on for good%ls (%ls)",
                                 dep_on ? L"" : L" already (Windows keeps it on for VanaCore.exe)",
                                 forced_on ? L"[client] dep=1" : L"Wine on macOS");
        } else if (host != nullptr) {
            vanacore::log::write(L"Memory: DEP left as Wine has it (%ls)",
                                 forced_off ? L"[client] dep=0" : L"not a macOS host");
        }
    }
    // From here the game works from a session copy of its settings file (settings.hpp: --ini's, else vanacore.ini):
    // [logging] was read from the file above, its old keys are moved first, and everything below is given the copy's path
    // in place of the file's
    install_settings();
    vanacore::crash::install(g_boot.install_dir, dumps);
    vanacore::window::dpi_aware(g_boot.settings_ini); // before the game makes any window
    vanacore::window::hook_metrics();
    vanacore::frametime::install(g_boot.settings_ini); // [logging] frametimes
    vanacore::session::install(g_boot.attached != 0);

    if (g_boot.settings_ini[0] != L'\0') {
        vanacore::client::install(g_boot.settings_ini, g_boot.retail != 0); // [client] retail, --retail: before anything that serves what a server's setup stands in for
        if (!vanacore::client::retail())
            vanacore::game_files::install(g_boot.settings_ini, g_boot.install_dir); // before registry: adds install paths
        vanacore::pivot::install(g_boot.settings_ini, g_boot.install_dir); // before plugins: their file hooks go on top
        vanacore::registry::install(g_boot.settings_ini);
        vanacore::uiscale::install(g_boot.settings_ini); // [ffxi] resolution.ui as the game starts with it, live after
        vanacore::input::install(g_boot.settings_ini);
        vanacore::mousediag::install(g_boot.settings_ini); // [logging] mouse
        vanacore::binds::install(g_boot.settings_ini);
        vanacore::aliases::install(g_boot.settings_ini); // [input] aliases.silent
        vanacore::commands::install(g_boot.settings_ini); // [commands]: VanaCore's commands switched off (Client > Commands)
        vanacore::hideui::install(g_boot.settings_ini); // [ui] hide.key: F11 hides the game's interface
        vanacore::itemcount::install(g_boot.settings_ini); // [client] items.count: the bag count on a window's title bar
        vanacore::itemlist::install(g_boot.settings_ini); // [client] items.rows: more rows in the Items and Mog House lists
        vanacore::spelllist::install(g_boot.settings_ini); // [client] spells.autosort: the Magic lists in A-Z order
        vanacore::itemslot::install(g_boot.settings_ini); // [client] items.slot: the square behind an item's icon
        vanacore::itemrarity::install(g_boot.settings_ini); // [client] items.rarity: the +1..+4 box in an item's icon
        vanacore::netstat::install(g_boot.settings_ini); // [client] network.*: the Network window's arrows and numbers
        vanacore::chatfont::install(g_boot.settings_ini); // [client] chat.font: the chat's letters from a TrueType font
        vanacore::statuswindow::install(g_boot.settings_ini); // [client] status: the Status window's layout
        vanacore::profilewin::install(g_boot.settings_ini); // [client] profile.*: the Profile panel's title bars and layout
        vanacore::alliancelist::install(g_boot.settings_ini); // [client] alliance.macrobar: the alliance lists stay with the macro bar
        vanacore::tpbar::install(g_boot.settings_ini); // [client] party.tpbar: a TP bar for members with no spells (and partylist)
        vanacore::noblink::install(g_boot.settings_ini); // [client] blink.disable: your character doesn't blink on a gear change
        vanacore::fishing::install(g_boot.settings_ini); // [input] fishing.dpad: the D-pad fights a hooked fish
        vanacore::sound::install(g_boot.settings_ini); // [client] sound.follow: the game's sound moves with Windows' default output
        vanacore::soundslots::install(); // 100 sound slots, [ffxi] sound.max_sounds live; failed sounds' -1
        vanacore::autologin::install(g_boot.settings_ini); // [client] autologin: the title screen goes by on its own
        vanacore::helpbar::install(g_boot.settings_ini); // [client] helpbar.*: the help bar's frame, title, text, caption
        vanacore::targetwin::install(g_boot.settings_ini); // [client] target.move: the Target window at the top middle
        vanacore::chattabs::install(g_boot.settings_ini); // [client] chat.tabs.*: the chat log's tabs, their names in True Font
        vanacore::mapwin::install(g_boot.settings_ini); // [client] map.move: the in-game map in a box
        vanacore::castbar::install(g_boot.settings_ini); // [client] castbar.hide: the cast bar not drawn
        vanacore::buffwin::install(g_boot.settings_ini); // [client] buffs.move: the status icons where the sliders say
        vanacore::hudwin::install(g_boot.settings_ini); // [client] party.move, action.move, buffs.hide, macrobar.hide: the HUD Manager's windows
        vanacore::macrobar::install(g_boot.settings_ini); // [client] macrobar.mode: how the macro bar keys show the bar
        vanacore::menuhover::install(g_boot.settings_ini); // [client] menu.hovermove: the mouse picks a menu row only when it moves
        vanacore::configmenu::install(g_boot.settings_ini); // [client] configmenu: VanaCore in the game's Config list
        vanacore::gamesettings::install(g_boot.settings_ini); // [client] gamesettings.extended: Config > Gameplay's own rows
        vanacore::singlemenu::install(g_boot.settings_ini); // [client] singlemenu: the main menu's two pages as one list
        vanacore::equipfirst::install(g_boot.settings_ini); // [client] macrooptions.equipfirst: an equip set's options with Equip first
        vanacore::partymenu::install(g_boot.settings_ini); // [client] partyfinder.findmember: Party > Find Member opens Party Finder
        install_diagnostics();
        vanacore::framerate::install(g_boot.settings_ini);
        vanacore::chat::install_log(g_boot.install_dir, g_boot.settings_ini);
    }

    const std::wstring name = std::wstring(L"VanaCore v") + vanacore::kVersion;
    vanacore::fonts::install(g_boot.install_dir); // before anything draws text
    vanacore::gui::install(g_boot.install_dir, g_boot.settings_ini); // ImGui, before plugins and addons can use it ([client] imgui.font)
    vanacore::layout::install(g_boot.install_dir); // where their windows were moved to (config\layout.json)
    vanacore::graphics::install(g_boot.settings_ini, g_boot.install_dir); // before overlay: Direct3DCreate8
    vanacore::overlay::install(g_boot.settings_ini, name, L"SoulspireXI Client (based on " + name + L")",
                               std::wstring(g_boot.install_dir) + L"\\addons\\vanacore\\vana\\logo.png",
                               std::wstring(g_boot.install_dir) + L"\\VanaCore.exe");
    vanacore::addons::install(g_boot.install_dir, g_boot.settings_ini);
    vanacore::plugins::install(g_boot.install_dir, g_boot.settings_ini);
    vanacore::shutdown::install(); // ExitProcess: addons and plugins unloaded first
    vanacore::screenshot::install(g_boot.install_dir, g_boot.settings_ini);
    vanacore::game_config::install(g_boot.settings_ini);
    vanacore::camera::install(g_boot.settings_ini); // [camera] zoom: the zoom moves the camera
    vanacore::framepacing::install(g_boot.settings_ini); // [client] framepacing: 60 FPS timing kept to the clock
    vanacore::packetflow::install(g_boot.settings_ini); // [client] packetflow: the game sends sooner once the server answers
    vanacore::packetwatch::install(g_boot.settings_ini); // [client] packets.watch: the server's packets logged as they come
    vanacore::packets::configure(g_boot.settings_ini); // [client] packets.keepactions: an action an addon blocked still plays
    vanacore::mcp::install(g_boot.install_dir, g_boot.settings_ini); // [mcp] enabled: the MCP server
    vanacore::discord::install(g_boot.settings_ini); // [discord] enabled: the character and zone on Discord
    g_installed = true;

    // More hooks (window modes) are installed from here: see research/roadmap.md, M1.
    return 0;
}

// Called by VanaCore.exe as the game process of its own login (BootInfo::logged_in), in place of a
// bootloader: starts PlayOnline and FFXI in this process and returns when the game closes, with
// the exit code. Exported undecorated via VanaCore.Core.def.
extern "C" int WINAPI VanaCoreRunGame()
{
    if (!g_installed || g_boot.logged_in == 0) {
        vanacore::log::write(L"VanaCoreRunGame: VanaCore.exe didn't log in for this process");
        return vanacore::kExitGameStartFailed;
    }
    return vanacore::loader::run_game(g_boot);
}
