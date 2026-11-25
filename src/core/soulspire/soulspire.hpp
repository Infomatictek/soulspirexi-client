#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

struct lua_State;

// Top-level coordinator for SoulspireXI's VanaCore-native client subsystem (CLIENT-MODERN-001).
// Modularly isolated under src/core/soulspire/ to minimize upstream VanaCore merge conflicts.
namespace vanacore::soulspire {

// Called from VanaCoreInstall (dllmain.cpp). Reads [soulspire] enabled=0|1 from settings_ini.
// Remains cleanly dormant when disabled (e.g. Modern — VanaCore Baseline).
void install(const wchar_t* install_dir, const wchar_t* settings_ini);

// Called once per frame from overlay::present() (game thread).
// Installs coordinated FFXiMain hooks once decrypted, refreshes local job/level,
// and applies authoritative UI input & cursor ownership.
void on_frame();

// Called from gui::begin_frame() / gui::release() so native textures stay bound to the active D3D8 device.
void on_gui_device(void* device);
void on_gui_release();

// Called from addons::remove() so unloading an addon immediately releases any UI input capture it held.
void on_addon_unloaded(std::string_view addon_name);

// True while a VanaCore-native Soulspire UI surface holds authoritative input ownership with cursor visible.
bool wants_visible_cursor();

// Called during shutdown::unload_everything(). Cleanly restores patched FFXiMain sites,
// releases textures, and restores normal gameplay input/cursor ownership.
void shutdown();

// Reloads authoritative soul-identities.json and soul-weapon-config.json from disk.
bool reload_registries();

// Packet lifecycle integration (called directly from vanacore::packets::process).
bool wants_packets() noexcept;
void on_packet_in(std::uint16_t id, const std::uint8_t* data, std::size_t length);
bool suppress_incoming_for_game(std::uint16_t id, const std::uint8_t* data, std::size_t length);

// Pushes `vana.soulspire` table onto the Lua stack (called from lua_api::open).
void push_lua_api(lua_State* L);

} // namespace vanacore::soulspire
