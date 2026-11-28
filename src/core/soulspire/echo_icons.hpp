#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

// SoulspireXI — Shared Native Soul Icon Renderer for World Nameplates & Native Party HUD
// (QA-SOUL-COMPANIONS-001 / WS8-INFRA-020 / DEC-038).
//
// Zero new FFXiMain.dll hooks: reuses VanaCore's existing nameplates.cpp, partylist.cpp,
// and targetwin.cpp D3D8 overlay draw passes plus the single authoritative Soul icon
// assets in `addons/vanacore/soulspire_core/assets/crystals/<key>.png` mapped from
// `soul-companions.json`.
namespace vanacore::soulspire::echo_icons {

using DrawFn = std::uint32_t(__fastcall*)(void* renderer, void* edx, unsigned prim, unsigned count,
                                          const void* vertices, unsigned stride);

// Configure client install root for resolving shared Soulspire Core icon assets.
void set_install_dir(std::wstring install_dir);

// RAII scope active while nameplates.cpp draws one entity's world nameplate.
// O(1) no-op whenever no Soul Echoes are active or `entity` is not an active Soul Echo.
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
// If `text` matches an active Soul Echo, queues its shared Soul icon at `(x, y)`,
// shifts `out_x` right to leave clean icon room, and formats `<Name> [<ABBR>]` into `buf`.
// Returns `text` unchanged and leaves `out_x = x` for all non-Echo party members.
const char* format_party_echo_name(int x, int y, const char* text, char* buf,
                                   std::size_t buf_sz, int& out_x) noexcept;

// Called once per frame from targetwin.cpp::draw (D3D8 HUD overlay pass, alongside gmicons::draw_mark)
// to render queued native party HUD Soul icons using the exact same shared sprite cache.
void draw_party_icons(void* device, unsigned width, unsigned height) noexcept;

// Pure formatting helper (unit-tested in VanaCore.SettingsTest).
std::string build_party_echo_label(std::string_view name, std::string_view abbr, std::size_t max_chars = 16);

} // namespace vanacore::soulspire::echo_icons
