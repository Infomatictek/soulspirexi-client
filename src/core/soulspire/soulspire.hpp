#pragma once

#include <cstddef>
#include <cstdint>

struct lua_State;

// Top-level coordinator for SoulspireXI's VanaCore-native client subsystem (CLIENT-MODERN-001).
// Modularly isolated under src/core/soulspire/ to minimize upstream VanaCore merge conflicts.
namespace vanacore::soulspire {

// Called from VanaCoreInstall (dllmain.cpp). Reads [soulspire] enabled=0|1 from settings_ini.
// Remains cleanly dormant when disabled (e.g. Modern — VanaCore Baseline).
void install(const wchar_t* install_dir, const wchar_t* settings_ini);

// Called once per frame from overlay::present() (game thread).
// Installs coordinated FFXiMain hooks once decrypted and refreshes local job/level.
void on_frame();

// Called during shutdown::unload_everything(). Cleanly restores patched FFXiMain sites.
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
