#include "lua_bridge.hpp"

#include <windows.h>
#include <lua.hpp>

#include <cstdint>
#include <string>
#include <vector>

#include "soulspire.hpp"
#include "state.hpp"

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

    lua_newtable(L);
    for (std::size_t i = 0; i < d.hooks.size(); ++i) {
        const auto& h = d.hooks[i];
        lua_newtable(L);
        lua_pushlstring(L, h.id.data(), h.id.size()); lua_setfield(L, -2, "id");
        lua_pushlstring(L, h.group.data(), h.group.size()); lua_setfield(L, -2, "group");
        lua_pushlstring(L, h.owner.data(), h.owner.size()); lua_setfield(L, -2, "owner");
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
    set_fn(L, "set_row_limit",     &l_set_row_limit);
    set_fn(L, "replace_visibility",&l_replace_visibility);
    set_fn(L, "clear_visibility",  &l_clear_visibility);
    set_fn(L, "hud",               &l_hud);
    set_fn(L, "mob_status",        &l_mob_status);
    set_fn(L, "request_open_core", &l_request_open_core);
    set_fn(L, "consume_open_core", &l_consume_open_core);
    set_fn(L, "reload",            &l_reload);
    set_fn(L, "diagnostics",       &l_diagnostics);
}

} // namespace vanacore::soulspire::lua_bridge
