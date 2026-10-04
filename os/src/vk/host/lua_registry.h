// The Lua function registry (app-host.md, "Lua function registry").
#pragma once

#include "../core/registry.h"

extern "C" {
#include "../../lua/lua.h"
#include "../../lua/lauxlib.h"
}

namespace vk::lua {
struct LuaFunction : Registered<LuaFunction> {
  const char *module;        // "wallet" -> badge.wallet
  const char *name;          // "begin_solana"
  const char *permission;    // nullptr = always available
  lua_CFunction fn;
  LuaFunction(const char *m, const char *n, const char *p, lua_CFunction f) : module(m), name(n), permission(p), fn(f) {}
};
#define VK_LUA_FUNCTION(ident, module, name, permission, fn) \
  static vk::lua::LuaFunction vk_lua_##ident(module, name, permission, fn)

void open(lua_State *L);       // hook H7; `badge` is on top of the stack and stays there
}
