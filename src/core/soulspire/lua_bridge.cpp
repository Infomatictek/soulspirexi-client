#include "lua_bridge.hpp"

#include <windows.h>
#include <lua.hpp>

#include <cstdint>
#include <string>
#include <vector>

#include "../addons.hpp"
#include "../gui.hpp"
#include "../window.hpp"
#include "input_ownership.hpp"
#include "soulspire.hpp"
#include "state.hpp"
#include "textures.hpp"

namespace vanacore::soulspire::lua_bridge {
namespace {

void set_fn(lua_State* L, const char* name, lua_CFunction fn)
{
    lua_pushcfunction(L, fn);
    lua_setfield(L, -2, name);
}

int l_enabled(lua_State* L)
{
    lua_pushboolean(L, engine().enabled() ? 1 : 0);
    return 1;
}

int l_version(lua_State* L)
{
    lua_pushinteger(L, static_cast<lua_Integer>(kNativeApiVersion));
    return 1;
}

int l_capabilities(lua_State* L)
{
    lua_pushnumber(L, static_cast<lua_Number>(engine().capabilities()));
    return 1;
}

int l_identity(lua_State* L)
{
    const ActiveSoul s = engine().active_soul();
    lua_newtable(L);
    lua_pushnumber(L, static_cast<lua_Number>(s.character_id));
    lua_setfield(L, -2, "character_id");
    lua_pushinteger(L, s.job);
    lua_setfield(L, -2, "job");
    lua_pushinteger(L, s.branch);
    lua_setfield(L, -2, "branch");
    lua_pushinteger(L, s.effective_level);
    lua_setfield(L, -2, "effective_level");
    lua_pushboolean(L, s.known ? 1 : 0);
    lua_setfield(L, -2, "known");
    lua_pushlstring(L, s.source.data(), s.source.size());
    lua_setfield(L, -2, "source");

    std::string key, abbr, full;
    if (s.known && s.job >= 1 && s.branch >= 1) {
        if (engine().pair_to_soul_key(s.job, s.branch, key)) {
            lua_pushlstring(L, key.data(), key.size());
            lua_setfield(L, -2, "soul_key");
        }
        if (engine().resolve_label(s.job, s.branch, false, abbr)) {
            lua_pushlstring(L, abbr.data(), abbr.size());
            lua_setfield(L, -2, "short_name");
        }
        if (engine().resolve_label(s.job, s.branch, true, full)) {
            lua_pushlstring(L, full.data(), full.size());
            lua_setfield(L, -2, "full_name");
        }
    }
    return 1;
}

int l_set_identity(lua_State* L)
{
    std::uint8_t job = 0, branch = 0;
    std::uint32_t char_id = 0;
    std::uint8_t eff_level = 0;

    if (lua_type(L, 1) == LUA_TSTRING) {
        const char* key = lua_tostring(L, 1);
        if (key == nullptr || !engine().soul_key_to_pair(key, job, branch)) {
            lua_pushboolean(L, 0);
            return 1;
        }
        char_id = static_cast<std::uint32_t>(luaL_optnumber(L, 2, 0));
        eff_level = static_cast<std::uint8_t>(luaL_optinteger(L, 3, 0));
    } else {
        job = static_cast<std::uint8_t>(luaL_checkinteger(L, 1));
        branch = static_cast<std::uint8_t>(luaL_checkinteger(L, 2));
        char_id = static_cast<std::uint32_t>(luaL_optnumber(L, 3, 0));
        eff_level = static_cast<std::uint8_t>(luaL_optinteger(L, 4, 0));
    }

    if (job < 1 || job > 22 || branch > 3) {
        lua_pushboolean(L, 0);
        return 1;
    }
    engine().set_active_soul(char_id, job, branch, eff_level, branch > 0, "lua_bridge", GetTickCount64());
    lua_pushboolean(L, 1);
    return 1;
}

int l_clear_identity(lua_State* L)
{
    engine().clear_active_soul("lua_clear");
    lua_pushboolean(L, 1);
    return 1;
}

bool read_action_from_table(lua_State* L, int idx, ActionRecord& out)
{
    if (!lua_istable(L, idx))
        return false;
    out = {};
    lua_getfield(L, idx, "job");
    out.job = static_cast<std::uint8_t>(lua_tointeger(L, -1));
    lua_pop(L, 1);

    lua_getfield(L, idx, "branch");
    out.branch = static_cast<std::uint8_t>(lua_tointeger(L, -1));
    lua_pop(L, 1);

    lua_getfield(L, idx, "kind");
    const int k = static_cast<int>(lua_tointeger(L, -1));
    lua_pop(L, 1);
    if (k < 1 || k > 3)
        return false;
    out.kind = static_cast<ActionKind>(k);

    lua_getfield(L, idx, "id");
    out.action_id = static_cast<std::uint16_t>(lua_tointeger(L, -1));
    lua_pop(L, 1);

    lua_getfield(L, idx, "icon_id");
    out.icon_id = lua_isnumber(L, -1) ? static_cast<std::uint16_t>(lua_tointeger(L, -1)) : 0xFFFFu;
    lua_pop(L, 1);

    lua_getfield(L, idx, "icon_id_v2");
    out.icon_id_v2 = lua_isnumber(L, -1) ? static_cast<std::uint16_t>(lua_tointeger(L, -1)) : 0xFFFFu;
    lua_pop(L, 1);

    lua_getfield(L, idx, "name");
    if (const char* s = lua_tostring(L, -1))
        out.name = s;
    lua_pop(L, 1);

    lua_getfield(L, idx, "description");
    if (const char* s = lua_tostring(L, -1))
        out.description = s;
    lua_pop(L, 1);

    return out.job >= 1 && out.job <= 22 && out.branch >= 1 && out.branch <= 3;
}

int l_replace_actions(lua_State* L)
{
    luaL_checktype(L, 1, LUA_TTABLE);
    std::vector<ActionRecord> recs;
    const std::size_t n = lua_objlen(L, 1);
    recs.reserve(n);
    for (std::size_t i = 1; i <= n; ++i) {
        lua_rawgeti(L, 1, static_cast<int>(i));
        ActionRecord rec{};
        if (read_action_from_table(L, -1, rec))
            recs.push_back(std::move(rec));
        lua_pop(L, 1);
    }
    const std::uint32_t count = engine().replace_actions(std::move(recs));
    lua_pushinteger(L, static_cast<lua_Integer>(count));
    return 1;
}

int l_clear_actions(lua_State* L)
{
    engine().clear_actions();
    lua_pushboolean(L, 1);
    return 1;
}

int l_set_row_limit(lua_State* L)
{
    const auto lim = static_cast<std::uint32_t>(luaL_checkinteger(L, 1));
    lua_pushboolean(L, engine().set_action_row_limit(lim) ? 1 : 0);
    return 1;
}

int l_replace_visibility(lua_State* L)
{
    luaL_checktype(L, 1, LUA_TTABLE);
    std::vector<VisibilityRecord> recs;
    const std::size_t n = lua_objlen(L, 1);
    recs.reserve(n);
    for (std::size_t i = 1; i <= n; ++i) {
        lua_rawgeti(L, 1, static_cast<int>(i));
        if (lua_istable(L, -1)) {
            VisibilityRecord rec{};
            lua_getfield(L, -1, "job"); rec.job = static_cast<std::uint8_t>(lua_tointeger(L, -1)); lua_pop(L, 1);
            lua_getfield(L, -1, "branch"); rec.branch = static_cast<std::uint8_t>(lua_tointeger(L, -1)); lua_pop(L, 1);
            lua_getfield(L, -1, "kind"); const int k = static_cast<int>(lua_tointeger(L, -1)); lua_pop(L, 1);
            lua_getfield(L, -1, "id"); rec.action_id = static_cast<std::uint16_t>(lua_tointeger(L, -1)); lua_pop(L, 1);
            lua_getfield(L, -1, "min_level"); rec.min_effective_level = static_cast<std::uint8_t>(lua_tointeger(L, -1)); lua_pop(L, 1);
            lua_getfield(L, -1, "rule"); const int r = static_cast<int>(lua_tointeger(L, -1)); lua_pop(L, 1);
            if (rec.job >= 1 && rec.job <= 22 && rec.branch >= 1 && rec.branch <= 3 && k >= 1 && k <= 3) {
                rec.kind = static_cast<ActionKind>(k);
                rec.action = (r == 0) ? VisibilityRuleAction::Hide : VisibilityRuleAction::Show;
                recs.push_back(rec);
            }
        }
        lua_pop(L, 1);
    }
    const std::uint32_t count = engine().replace_visibility(std::move(recs));
    lua_pushinteger(L, static_cast<lua_Integer>(count));
    return 1;
}

int l_clear_visibility(lua_State* L)
{
    engine().clear_visibility();
    lua_pushboolean(L, 1);
    return 1;
}

int l_hud(lua_State* L)
{
    const HudSnapshot h = engine().hud_snapshot();
    lua_newtable(L);
    lua_pushlstring(L, h.soul_key.data(), h.soul_key.size());
    lua_setfield(L, -2, "soul_key");
    lua_pushnumber(L, static_cast<lua_Number>(h.updated_ms));
    lua_setfield(L, -2, "updated_ms");
    lua_pushinteger(L, static_cast<lua_Integer>(h.packet_count));
    lua_setfield(L, -2, "packet_count");
    lua_newtable(L);
    for (const auto& [k, v] : h.kv) {
        lua_pushlstring(L, v.data(), v.size());
        lua_setfield(L, -2, k.c_str());
    }
    lua_setfield(L, -2, "kv");
    return 1;
}

int l_mob_status(lua_State* L)
{
    const auto server_id = static_cast<std::uint32_t>(luaL_checknumber(L, 1));
    MobStatusSnapshot snap{};
    if (!engine().mob_status(server_id, snap)) {
        lua_pushnil(L);
        return 1;
    }
    lua_newtable(L);
    lua_pushnumber(L, static_cast<lua_Number>(snap.server_id));
    lua_setfield(L, -2, "server_id");
    lua_pushlstring(L, snap.payload.data(), snap.payload.size());
    lua_setfield(L, -2, "payload");
    lua_pushnumber(L, static_cast<lua_Number>(snap.updated_ms));
    lua_setfield(L, -2, "updated_ms");
    lua_pushinteger(L, static_cast<lua_Integer>(snap.packet_count));
    lua_setfield(L, -2, "packet_count");
    return 1;
}

int l_all_mob_statuses(lua_State* L)
{
    const auto list = engine().all_mob_statuses();
    lua_newtable(L);
    for (std::size_t i = 0; i < list.size(); ++i) {
        const auto& snap = list[i];
        lua_newtable(L);
        lua_pushnumber(L, static_cast<lua_Number>(snap.server_id));
        lua_setfield(L, -2, "server_id");
        lua_pushlstring(L, snap.payload.data(), snap.payload.size());
        lua_setfield(L, -2, "payload");
        lua_pushnumber(L, static_cast<lua_Number>(snap.updated_ms));
        lua_setfield(L, -2, "updated_ms");
        lua_pushinteger(L, static_cast<lua_Integer>(snap.packet_count));
        lua_setfield(L, -2, "packet_count");
        lua_rawseti(L, -2, static_cast<int>(i + 1));
    }
    return 1;
}

int l_lookup_action(lua_State* L)
{
    const int k = static_cast<int>(luaL_checkinteger(L, 1));
    const auto id = static_cast<std::uint16_t>(luaL_checkinteger(L, 2));
    if (k < 1 || k > 3) {
        lua_pushnil(L);
        return 1;
    }
    ActionRecord rec{};
    if (!engine().lookup_action(static_cast<ActionKind>(k), id, rec)) {
        lua_pushnil(L);
        return 1;
    }
    lua_newtable(L);
    lua_pushinteger(L, rec.job); lua_setfield(L, -2, "job");
    lua_pushinteger(L, rec.branch); lua_setfield(L, -2, "branch");
    lua_pushinteger(L, static_cast<int>(rec.kind)); lua_setfield(L, -2, "kind");
    lua_pushinteger(L, rec.action_id); lua_setfield(L, -2, "id");
    lua_pushlstring(L, rec.name.data(), rec.name.size()); lua_setfield(L, -2, "name");
    lua_pushlstring(L, rec.description.data(), rec.description.size()); lua_setfield(L, -2, "description");
    return 1;
}

int l_search_actions(lua_State* L)
{
    const char* q = luaL_optstring(L, 1, "");
    const auto limit = static_cast<std::uint32_t>(luaL_optinteger(L, 2, 64));
    const auto rows = engine().search_actions(q != nullptr ? q : "", limit);
    lua_newtable(L);
    for (std::size_t i = 0; i < rows.size(); ++i) {
        const auto& rec = rows[i];
        lua_newtable(L);
        lua_pushinteger(L, static_cast<int>(rec.kind)); lua_setfield(L, -2, "kind");
        lua_pushinteger(L, rec.action_id); lua_setfield(L, -2, "id");
        lua_pushlstring(L, rec.name.data(), rec.name.size()); lua_setfield(L, -2, "name");
        lua_pushlstring(L, rec.description.data(), rec.description.size()); lua_setfield(L, -2, "description");
        lua_rawseti(L, -2, static_cast<int>(i + 1));
    }
    return 1;
}

int l_decide_visibility(lua_State* L)
{
    const int k = static_cast<int>(luaL_checkinteger(L, 1));
    const auto id = static_cast<std::uint16_t>(luaL_checkinteger(L, 2));
    const auto lvl = static_cast<std::uint8_t>(luaL_optinteger(L, 3, 0));
    if (k < 1 || k > 3) {
        lua_pushstring(L, "unmanaged");
        return 1;
    }
    const VisibilityDecision d = engine().decide_visibility(static_cast<ActionKind>(k), id, lvl);
    if (d == VisibilityDecision::Show)
        lua_pushstring(L, "show");
    else if (d == VisibilityDecision::Hide)
        lua_pushstring(L, "hide");
    else
        lua_pushstring(L, "unmanaged");
    return 1;
}

int l_request_open_core(lua_State* L)
{
    engine().request_open_core();
    lua_pushboolean(L, 1);
    return 1;
}

int l_consume_open_core(lua_State* L)
{
    lua_pushinteger(L, static_cast<lua_Integer>(engine().consume_open_core()));
    return 1;
}

int l_reload(lua_State* L)
{
    lua_pushboolean(L, reload_registries() ? 1 : 0);
    return 1;
}

int l_load_texture(lua_State* L)
{
    const char* path = luaL_checkstring(L, 1);
    textures::TextureInfo info{};
    std::string err;
    if (!textures::load(path != nullptr ? path : "", info, err)) {
        lua_pushnil(L);
        lua_pushlstring(L, err.data(), err.size());
        return 2;
    }
    lua_pushnumber(L, static_cast<lua_Number>(info.id));
    lua_pushinteger(L, info.width);
    lua_pushinteger(L, info.height);
    return 3;
}

int l_free_texture(lua_State* L)
{
    const auto id = static_cast<std::uint32_t>(luaL_checknumber(L, 1));
    lua_pushboolean(L, textures::free_by_id(id) ? 1 : 0);
    return 1;
}

int l_release_textures(lua_State* L)
{
    lua_pushinteger(L, static_cast<lua_Integer>(textures::release_all_count()));
    return 1;
}

int l_texture_stats(lua_State* L)
{
    const textures::TextureStats s = textures::stats();
    lua_newtable(L);
    lua_pushinteger(L, static_cast<lua_Integer>(s.loaded)); lua_setfield(L, -2, "loaded");
    lua_pushnumber(L, static_cast<lua_Number>(s.bytes)); lua_setfield(L, -2, "bytes");
    lua_pushinteger(L, static_cast<lua_Integer>(s.hits)); lua_setfield(L, -2, "hits");
    lua_pushinteger(L, static_cast<lua_Integer>(s.misses)); lua_setfield(L, -2, "misses");
    lua_pushinteger(L, static_cast<lua_Integer>(s.failures)); lua_setfield(L, -2, "failures");
    lua_pushboolean(L, s.device_ready ? 1 : 0); lua_setfield(L, -2, "device_ready");
    return 1;
}

int l_acquire_ui_input(lua_State* L)
{
    const char* owner = luaL_optstring(L, 1, "soulspire_core");
    UiInputOptions opts{};
    if (lua_istable(L, 2)) {
        lua_getfield(L, 2, "block_mouse");
        if (!lua_isnil(L, -1)) opts.block_mouse = lua_toboolean(L, -1) != 0;
        lua_pop(L, 1);
        lua_getfield(L, 2, "capture_gamepad");
        if (!lua_isnil(L, -1)) opts.capture_gamepad = lua_toboolean(L, -1) != 0;
        lua_pop(L, 1);
        lua_getfield(L, 2, "show_cursor");
        if (!lua_isnil(L, -1)) opts.show_cursor = lua_toboolean(L, -1) != 0;
        lua_pop(L, 1);
        lua_getfield(L, 2, "lease_ms");
        if (lua_isnumber(L, -1)) opts.lease_ms = static_cast<std::uint32_t>(lua_tointeger(L, -1));
        lua_pop(L, 1);
    }
    const UiInputTransition tr = input_ownership().acquire(owner != nullptr ? owner : "soulspire_core", opts, GetTickCount64());
    if (tr.active) {
        if (tr.block_mouse) window::block_mouse(true, 400);
        if (tr.capture_gamepad) gui::want_gamepad(true);
        gui::set_cursor_override(tr.show_cursor);
    }
    lua_pushboolean(L, tr.active ? 1 : 0);
    return 1;
}

int l_release_ui_input(lua_State* L)
{
    const char* owner = luaL_optstring(L, 1, "soulspire_core");
    const char* reason = luaL_optstring(L, 2, "closed");
    const UiInputTransition tr = input_ownership().release(
        owner != nullptr ? owner : "soulspire_core",
        reason != nullptr ? reason : "closed",
        GetTickCount64());
    if (tr.became_inactive) {
        window::block_mouse(false);
        gui::want_gamepad(false);
        gui::set_cursor_override(false);
        gui::restore_cursor();
    }
    lua_pushboolean(L, !tr.active ? 1 : 0);
    return 1;
}

int l_ui_input_state(lua_State* L)
{
    const UiInputSnapshot s = input_ownership().snapshot();
    lua_newtable(L);
    lua_pushboolean(L, s.active ? 1 : 0); lua_setfield(L, -2, "active");
    lua_pushboolean(L, s.block_mouse ? 1 : 0); lua_setfield(L, -2, "block_mouse");
    lua_pushboolean(L, s.capture_gamepad ? 1 : 0); lua_setfield(L, -2, "capture_gamepad");
    lua_pushboolean(L, s.show_cursor ? 1 : 0); lua_setfield(L, -2, "show_cursor");
    lua_pushinteger(L, static_cast<lua_Integer>(s.owner_count)); lua_setfield(L, -2, "owner_count");
    lua_pushinteger(L, static_cast<lua_Integer>(s.acquire_count)); lua_setfield(L, -2, "acquire_count");
    lua_pushinteger(L, static_cast<lua_Integer>(s.release_count)); lua_setfield(L, -2, "release_count");
    lua_pushlstring(L, s.last_release_reason.data(), s.last_release_reason.size()); lua_setfield(L, -2, "last_release_reason");
    lua_newtable(L);
    for (std::size_t i = 0; i < s.owners.size(); ++i) {
        lua_pushlstring(L, s.owners[i].data(), s.owners[i].size());
        lua_rawseti(L, -2, static_cast<int>(i + 1));
    }
    lua_setfield(L, -2, "owners");
    return 1;
}

int l_ashita_compat_enabled(lua_State* L)
{
    lua_pushboolean(L, addons::ashita_enabled() ? 1 : 0);
    return 1;
}

int l_diagnostics(lua_State* L)
{
    const DiagnosticsSnapshot d = engine().diagnostics();
    lua_newtable(L);
    lua_pushboolean(L, d.enabled ? 1 : 0); lua_setfield(L, -2, "enabled");
    lua_pushinteger(L, static_cast<lua_Integer>(d.api_version)); lua_setfield(L, -2, "api_version");
    lua_pushnumber(L, static_cast<lua_Number>(d.capabilities)); lua_setfield(L, -2, "capabilities");
    lua_pushinteger(L, static_cast<lua_Integer>(d.hooks_installed)); lua_setfield(L, -2, "hooks_installed");
    lua_pushinteger(L, static_cast<lua_Integer>(d.hooks_total)); lua_setfield(L, -2, "hooks_total");
    lua_pushinteger(L, static_cast<lua_Integer>(d.registry_soul_count)); lua_setfield(L, -2, "registry_soul_count");
    lua_pushlstring(L, d.registry_hash.data(), d.registry_hash.size()); lua_setfield(L, -2, "registry_hash");
    lua_pushlstring(L, d.registry_source.data(), d.registry_source.size()); lua_setfield(L, -2, "registry_source");
    lua_pushlstring(L, d.weapon_config_hash.data(), d.weapon_config_hash.size()); lua_setfield(L, -2, "weapon_config_hash");
    lua_pushinteger(L, static_cast<lua_Integer>(d.action_count)); lua_setfield(L, -2, "action_count");
    lua_pushinteger(L, static_cast<lua_Integer>(d.action_generation)); lua_setfield(L, -2, "action_generation");
    lua_pushinteger(L, static_cast<lua_Integer>(d.action_row_limit)); lua_setfield(L, -2, "action_row_limit");
    lua_pushinteger(L, static_cast<lua_Integer>(d.visibility_count)); lua_setfield(L, -2, "visibility_count");
    lua_pushinteger(L, static_cast<lua_Integer>(d.weapon_rule_count)); lua_setfield(L, -2, "weapon_rule_count");
    lua_pushinteger(L, static_cast<lua_Integer>(d.packets_1ff)); lua_setfield(L, -2, "packets_1ff");
    lua_pushinteger(L, static_cast<lua_Integer>(d.packets_1fc)); lua_setfield(L, -2, "packets_1fc");
    lua_pushinteger(L, static_cast<lua_Integer>(d.packets_1fd)); lua_setfield(L, -2, "packets_1fd");
    lua_pushinteger(L, static_cast<lua_Integer>(d.packets_1fe)); lua_setfield(L, -2, "packets_1fe");
    lua_pushinteger(L, static_cast<lua_Integer>(d.packets_oxhud)); lua_setfield(L, -2, "packets_oxhud");
    lua_pushinteger(L, static_cast<lua_Integer>(d.packets_oxmob)); lua_setfield(L, -2, "packets_oxmob");
    lua_pushinteger(L, static_cast<lua_Integer>(d.packets_oxtal)); lua_setfield(L, -2, "packets_oxtal");
    lua_pushinteger(L, static_cast<lua_Integer>(d.suppressed_chat_frames)); lua_setfield(L, -2, "suppressed_chat_frames");

    const textures::TextureStats ts = textures::stats();
    const UiInputSnapshot uis = input_ownership().snapshot();
    lua_pushinteger(L, static_cast<lua_Integer>(ts.loaded)); lua_setfield(L, -2, "textures_loaded");
    lua_pushboolean(L, ts.device_ready ? 1 : 0); lua_setfield(L, -2, "texture_device_ready");
    lua_pushboolean(L, uis.active ? 1 : 0); lua_setfield(L, -2, "ui_input_active");
    lua_pushinteger(L, static_cast<lua_Integer>(uis.owner_count)); lua_setfield(L, -2, "ui_input_owner_count");
    lua_pushboolean(L, addons::ashita_enabled() ? 1 : 0); lua_setfield(L, -2, "ashita_compat_enabled");

    lua_newtable(L);
    for (std::size_t i = 0; i < d.hooks.size(); ++i) {
        const auto& h = d.hooks[i];
        lua_newtable(L);
        lua_pushlstring(L, h.id.data(), h.id.size()); lua_setfield(L, -2, "id");
        lua_pushlstring(L, h.group.data(), h.group.size()); lua_setfield(L, -2, "group");
        lua_pushlstring(L, h.owner.data(), h.owner.size()); lua_setfield(L, -2, "owner");
        lua_pushlstring(L, h.disposition.data(), h.disposition.size()); lua_setfield(L, -2, "disposition");
        lua_pushnumber(L, static_cast<lua_Number>(h.rva)); lua_setfield(L, -2, "rva");
        lua_pushboolean(L, h.installed ? 1 : 0); lua_setfield(L, -2, "installed");
        lua_pushinteger(L, static_cast<lua_Integer>(h.hit_count)); lua_setfield(L, -2, "hit_count");
        lua_pushlstring(L, h.detail.data(), h.detail.size()); lua_setfield(L, -2, "detail");
        lua_rawseti(L, -2, static_cast<int>(i + 1));
    }
    lua_setfield(L, -2, "hooks");
    return 1;
}

} // namespace

void push(lua_State* L)
{
    lua_newtable(L);
    set_fn(L, "enabled",           &l_enabled);
    set_fn(L, "version",           &l_version);
    set_fn(L, "capabilities",      &l_capabilities);
    set_fn(L, "identity",          &l_identity);
    set_fn(L, "set_identity",      &l_set_identity);
    set_fn(L, "clear_identity",    &l_clear_identity);
    set_fn(L, "replace_actions",   &l_replace_actions);
    set_fn(L, "clear_actions",     &l_clear_actions);
    set_fn(L, "lookup_action",     &l_lookup_action);
    set_fn(L, "search_actions",    &l_search_actions);
    set_fn(L, "set_row_limit",     &l_set_row_limit);
    set_fn(L, "replace_visibility",&l_replace_visibility);
    set_fn(L, "clear_visibility",  &l_clear_visibility);
    set_fn(L, "decide_visibility", &l_decide_visibility);
    set_fn(L, "hud",               &l_hud);
    set_fn(L, "mob_status",        &l_mob_status);
    set_fn(L, "all_mob_statuses",  &l_all_mob_statuses);
    set_fn(L, "request_open_core",     &l_request_open_core);
    set_fn(L, "consume_open_core",     &l_consume_open_core);
    set_fn(L, "reload",                &l_reload);
    set_fn(L, "load_texture",          &l_load_texture);
    set_fn(L, "free_texture",          &l_free_texture);
    set_fn(L, "release_textures",      &l_release_textures);
    set_fn(L, "texture_stats",         &l_texture_stats);
    set_fn(L, "acquire_ui_input",      &l_acquire_ui_input);
    set_fn(L, "release_ui_input",      &l_release_ui_input);
    set_fn(L, "ui_input_state",        &l_ui_input_state);
    set_fn(L, "ashita_compat_enabled", &l_ashita_compat_enabled);
    set_fn(L, "diagnostics",           &l_diagnostics);
}

} // namespace vanacore::soulspire::lua_bridge
