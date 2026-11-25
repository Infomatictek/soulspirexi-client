#pragma once

struct lua_State;

// Pushes the `vana.soulspire` native Lua API table onto the Lua stack.
namespace vanacore::soulspire::lua_bridge {

void push(lua_State* L);

} // namespace vanacore::soulspire::lua_bridge
