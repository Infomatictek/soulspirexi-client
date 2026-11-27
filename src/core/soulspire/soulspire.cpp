#include "soulspire.hpp"

#include <windows.h>

#include <cstdio>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <vector>

#include "../game.hpp"
#include "../gui.hpp"
#include "../log.hpp"
#include "../window.hpp"
#include "echoes_rebrand.hpp"
#include "hooks.hpp"
#include "input_ownership.hpp"
#include "lua_bridge.hpp"
#include "packets.hpp"
#include "state.hpp"
#include "textures.hpp"

namespace vanacore::soulspire {
namespace {

std::wstring g_install_dir;
ULONGLONG g_last_job_poll = 0;

bool read_file_utf8(const std::wstring& path, std::string& out)
{
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"rb") != 0 || f == nullptr)
        return false;
    std::fseek(f, 0, SEEK_END);
    const long sz = std::ftell(f);
    if (sz <= 0 || sz > 512 * 1024) {
        std::fclose(f);
        return false;
    }
    std::rewind(f);
    out.resize(static_cast<std::size_t>(sz));
    const std::size_t got = std::fread(out.data(), 1, out.size(), f);
    std::fclose(f);
    out.resize(got);
    return got > 0;
}

bool load_first_existing(const std::vector<std::wstring>& candidates, std::string& content, std::wstring& used)
{
    for (const auto& p : candidates) {
        if (read_file_utf8(p, content)) {
            used = p;
            return true;
        }
    }
    return false;
}

} // namespace

bool reload_registries()
{
    if (g_install_dir.empty())
        return false;

    bool any_loaded = false;
    std::string json;
    std::wstring used;

    const std::vector<std::wstring> ident_paths = {
        g_install_dir + L"\\addons\\soulspire_core\\data\\soul-identities.json",
        g_install_dir + L"\\addons\\vanacore\\soulspire_core\\data\\soul-identities.json",
        g_install_dir + L"\\plugins\\ashita\\data\\soul-identities.json",
        g_install_dir + L"\\plugins\\data\\soul-identities.json",
        g_install_dir + L"\\data\\soul-identities.json",
    };
    if (load_first_existing(ident_paths, json, used)) {
        if (engine().load_identities_json(json, "runtime_json")) {
            const auto d = engine().diagnostics();
            log::write(L"Soulspire: loaded %u Soul identities from %ls (hash=%hs)",
                       d.registry_soul_count, used.c_str(), d.registry_hash.c_str());
            any_loaded = true;
        } else {
            log::write(L"Soulspire: failed to parse %ls", used.c_str());
        }
    }

    const std::vector<std::wstring> weapon_paths = {
        g_install_dir + L"\\addons\\soulspire_core\\data\\soul-weapon-config.json",
        g_install_dir + L"\\addons\\vanacore\\soulspire_core\\data\\soul-weapon-config.json",
        g_install_dir + L"\\plugins\\ashita\\data\\soul-weapon-config.json",
        g_install_dir + L"\\plugins\\data\\soul-weapon-config.json",
        g_install_dir + L"\\data\\soul-weapon-config.json",
    };
    json.clear();
    used.clear();
    if (load_first_existing(weapon_paths, json, used)) {
        if (engine().load_weapon_config_json(json)) {
            const auto d = engine().diagnostics();
            log::write(L"Soulspire: loaded %u weapon eligibility rules from %ls (hash=%hs)",
                       d.weapon_rule_count, used.c_str(), d.weapon_config_hash.c_str());
            any_loaded = true;
        } else {
            log::write(L"Soulspire: failed to parse %ls", used.c_str());
        }
    }

    const std::vector<std::wstring> companion_paths = {
        g_install_dir + L"\\addons\\soulspire_core\\data\\soul-companions.json",
        g_install_dir + L"\\addons\\vanacore\\soulspire_core\\data\\soul-companions.json",
        g_install_dir + L"\\data\\soul-companions.json",
    };
    json.clear();
    used.clear();
    if (load_first_existing(companion_paths, json, used)) {
        if (engine().load_companions_json(json)) {
            log::write(L"Soulspire: loaded %u Soul Echoes spells from %ls",
                       engine().companion_spell_count(), used.c_str());
            any_loaded = true;
        } else {
            log::write(L"Soulspire: failed to parse %ls", used.c_str());
        }
    }

    const std::vector<std::wstring> dsb_paths = {
        g_install_dir + L"\\addons\\soulspire_core\\data\\soul-dynamic-spellbooks.json",
        g_install_dir + L"\\addons\\vanacore\\soulspire_core\\data\\soul-dynamic-spellbooks.json",
        g_install_dir + L"\\data\\soul-dynamic-spellbooks.json",
    };
    json.clear();
    used.clear();
    if (load_first_existing(dsb_paths, json, used)) {
        if (engine().load_dynamic_spellbooks_json(json)) {
            log::write(L"Soulspire: loaded %u dynamic spellbook entries from %ls",
                       engine().dynamic_spell_count(), used.c_str());
            any_loaded = true;
        } else {
            log::write(L"Soulspire: failed to parse %ls", used.c_str());
        }
    }
    return any_loaded;
}

void install(const wchar_t* install_dir, const wchar_t* settings_ini)
{
    g_install_dir = install_dir != nullptr ? install_dir : L"";
    while (!g_install_dir.empty() && (g_install_dir.back() == L'\\' || g_install_dir.back() == L'/'))
        g_install_dir.pop_back();

    const bool enabled = settings_ini != nullptr && settings_ini[0] != L'\0' &&
                         GetPrivateProfileIntW(L"soulspire", L"enabled", 0, settings_ini) != 0;
    engine().set_enabled(enabled);
    if (!enabled) {
        log::write(L"Soulspire: VanaCore-native subsystem disabled ([soulspire] enabled=0)");
        return;
    }

    log::write(L"Soulspire: VanaCore-native subsystem enabled (API v%u)", kNativeApiVersion);
    reload_registries();
}

void apply_input_transition(const UiInputTransition& tr)
{
    if (tr.active) {
        if (tr.block_mouse)
            window::block_mouse(true, 400);
        if (tr.capture_gamepad)
            gui::want_gamepad(true);
        gui::set_cursor_override(tr.show_cursor);
    } else if (tr.became_inactive) {
        window::block_mouse(false);
        gui::want_gamepad(false);
        gui::set_cursor_override(false);
        gui::restore_cursor();
    }
}

void on_frame()
{
    if (!engine().enabled())
        return;

    hooks::install_if_ready();

    // Apply authoritative UI input & cursor ownership (handles focus loss & lease expiry).
    const ULONGLONG now = GetTickCount64();
    const UiInputTransition tr = input_ownership().tick(now, window::in_background());
    apply_input_transition(tr);

    // Refresh local player job & effective level every 250ms from VanaCore's game memory reader.
    if (now - g_last_job_poll >= 250) {
        g_last_job_poll = now;
        game::PartyMember members[18]{};
        game::Alliance alliance{};
        if (game::party(members, alliance) && members[0].active && members[0].main_job >= 1) {
            engine().update_live_job_level(members[0].main_job, members[0].main_job_level);
        }
    }
}

void on_gui_device(void* device)
{
    textures::set_device(device);
}

void on_gui_release()
{
    textures::release_all();
}

void on_addon_unloaded(std::string_view addon_name)
{
    if (addon_name.empty())
        return;
    const UiInputTransition tr = input_ownership().release(std::string(addon_name), "addon_unload", GetTickCount64());
    apply_input_transition(tr);
}

bool wants_visible_cursor()
{
    return engine().enabled() && input_ownership().wants_visible_cursor();
}

void shutdown()
{
    const UiInputTransition tr = input_ownership().release_all("shutdown", GetTickCount64());
    apply_input_transition(tr);
    textures::release_all();
    hooks::uninstall();
    engine().set_enabled(false);
}

bool wants_packets() noexcept
{
    return engine().enabled();
}

void on_packet_in(std::uint16_t id, const std::uint8_t* data, std::size_t length)
{
    packets::on_incoming(engine(), id, data, length, GetTickCount64());
}

bool suppress_incoming_for_game(std::uint16_t id, const std::uint8_t* data, std::size_t length)
{
    return packets::should_suppress_for_game(engine(), id, data, length);
}

void push_lua_api(lua_State* L)
{
    lua_bridge::push(L);
}

namespace {

constexpr std::size_t kEchoesPassthroughCap = 256u;
struct EchoesShapeCacheState {
    std::mutex mutex;
    std::unique_ptr<EchoesMenuSpriteBank> banks[kEchoesMaxSpriteBanks]{};
    const void* passthrough[kEchoesPassthroughCap]{};
    std::size_t passthrough_next = 0;
};

EchoesShapeCacheState& echoes_shape_cache()
{
    static EchoesShapeCacheState state;
    return state;
}

bool seh_read_shape_header(const void* shape, std::uint8_t (&hdr)[16]) noexcept
{
    __try {
        std::memcpy(hdr, shape, sizeof(hdr));
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool seh_decode_button(const void* shape, DecodedButtonSprite& out) noexcept
{
    __try {
        return decode_button_sprite_label(static_cast<const std::uint8_t*>(shape), out);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool seh_synthesize_button(
    EchoesMenuSpriteBank& bank,
    const void* shape,
    const DecodedButtonSprite& decoded,
    const char* label,
    std::size_t label_len) noexcept
{
    __try {
        return synthesize_echoes_button_sprite(
            bank, static_cast<const std::uint8_t*>(shape), decoded, std::string_view(label, label_len));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

} // namespace

void* rebrand_menu_shape(void* shape) noexcept
{
    if (shape == nullptr || !engine().enabled())
        return shape;

    auto& cache = echoes_shape_cache();
    std::uint8_t hdr[16]{};
    if (!seh_read_shape_header(shape, hdr))
        return shape;

    std::lock_guard lock(cache.mutex);

    // Fast path: check already-synthesized banks (also avoids re-processing our own bank sprites).
    for (std::size_t i = 0; i < kEchoesMaxSpriteBanks; ++i) {
        if (cache.banks[i] == nullptr || !cache.banks[i]->active)
            continue;
        if (shape == cache.banks[i]->sprite)
            return shape;
        if (cache.banks[i]->orig_shape == shape &&
            std::memcmp(cache.banks[i]->orig_hdr, hdr, sizeof(hdr)) == 0) {
            return cache.banks[i]->sprite;
        }
    }

    // Fast path: non-Trust shape already inspected.
    for (std::size_t i = 0; i < kEchoesPassthroughCap; ++i) {
        if (cache.passthrough[i] == shape)
            return shape;
    }

    DecodedButtonSprite decoded{};
    if (!seh_decode_button(shape, decoded)) {
        cache.passthrough[cache.passthrough_next] = shape;
        cache.passthrough_next = (cache.passthrough_next + 1u) % kEchoesPassthroughCap;
        return shape;
    }

    std::string target_label;
    const int pill_w = decoded.pill_right - decoded.pill_left;
    if (!choose_fitted_echoes_button_label(decoded.label_view(), pill_w, target_label)) {
        cache.passthrough[cache.passthrough_next] = shape;
        cache.passthrough_next = (cache.passthrough_next + 1u) % kEchoesPassthroughCap;
        return shape;
    }

    for (std::size_t i = 0; i < kEchoesMaxSpriteBanks; ++i) {
        if (cache.banks[i] == nullptr) {
            auto fresh = std::unique_ptr<EchoesMenuSpriteBank>(new (std::nothrow) EchoesMenuSpriteBank{});
            if (fresh == nullptr)
                break;
            if (!seh_synthesize_button(*fresh, shape, decoded, target_label.data(), target_label.size()))
                break;
            void* replaced = fresh->sprite;
            cache.banks[i] = std::move(fresh);
            log::write(L"Soulspire: rebranded native menu button \"%hs\" -> \"%hs\" (pill=%dpx)",
                       decoded.label, target_label.c_str(), pill_w);
            return replaced;
        }
    }

    return shape;
}

} // namespace vanacore::soulspire
