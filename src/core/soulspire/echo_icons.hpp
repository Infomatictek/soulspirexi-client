#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

// SoulspireXI — Shared Native Soul Icon Renderer for World Nameplates & Native Party HUD
// (ECHO-PARTY / QA-SOUL-COMPANIONS-001 / WS8-INFRA-020 / DEC-038).
//
// Zero new FFXiMain.dll hooks: reuses VanaCore's existing nameplates.cpp, partylist.cpp,
// and targetwin.cpp D3D8 overlay draw passes plus the single authoritative 66-Soul icon
// assets in `addons/vanacore/soulspire_core/assets/crystals/<key>.png` mapped from
// `soul-companions.json`, with a neutral Echo fallback (`echo_neutral.png` + procedural backup).
namespace vanacore::soulspire::echo_icons {

using DrawFn = std::uint32_t(__fastcall*)(void* renderer, void* edx, unsigned prim, unsigned count,
                                          const void* vertices, unsigned stride);

// Configure client install root for resolving shared Soulspire Core icon assets.
void set_install_dir(std::wstring install_dir);

// Release cached D3D8 icon textures (called on device reset / shutdown).
void release_device_resources() noexcept;

// RAII scope active while nameplates.cpp draws one entity's world nameplate.
// O(1) no-op whenever `entity` is not an active Soul Echo.
class NameScope {
public:
    explicit NameScope(const void* entity) noexcept;
    ~NameScope() noexcept;
    NameScope(const NameScope&) = delete;
    NameScope& operator=(const NameScope&) = delete;
};

// Called from nameplates.cpp::draw_letter when drawing the entity's name quads.
// Draws the Echo's Soul icon once, immediately to the left of the first name letter quad,
// inheriting exact screen coordinates, z/rhw occlusion, scaling, and distance-fade alpha.
void draw_nameplate_icon(void* renderer, DrawFn draw, unsigned prim, unsigned count,
                         const void* vertices, unsigned stride) noexcept;

// Called from partylist.cpp::party_name for each row of the native party HUD.
// Resolves Echo identity strictly by authoritative server entity ID (never by display name)
// and queues its unique Soul icon (or neutral Echo fallback) in the native left-of-name
// emblem position without shifting or altering the character name.
void queue_party_echo_icon(std::uint32_t member_id, int x, int y) noexcept;

// Called once per frame from targetwin.cpp::draw (D3D8 HUD overlay pass, alongside gmicons::draw_mark)
// to render queued native party HUD Soul icons using the exact same shared sprite cache.
void draw_party_icons(void* device, unsigned width, unsigned height) noexcept;

} // namespace vanacore::soulspire::echo_icons
