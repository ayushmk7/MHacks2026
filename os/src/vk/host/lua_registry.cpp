// vk::lua::open: adds every registered Lua function to the `badge` table of the app being launched.
// Complete except permission filtering, which arrives with the permissions module (WP30): until then
// every registered function is installed, whatever its `permission` says.
#include "lua_registry.h"

#include "permissions.h"

namespace vk::lua {

void open(lua_State *L) {
  // The grant set that preLaunch() prepared becomes the active one before anything is installed.
  vk::host::promotePending();

  luaL_checkstack(L, 4, "vk::lua::open");
  const int badgeIdx = lua_gettop(L);   // the `badge` table; it stays on top when we return

  for (LuaFunction *f = LuaFunction::first(); f; f = f->next()) {
    if (!f->module || !f->name || !f->fn) continue;
    // badge.<module>: an upstream module table if there is one, else created on first use.
    if (lua_getfield(L, badgeIdx, f->module) != LUA_TTABLE) {
      lua_pop(L, 1);
      lua_newtable(L);
      lua_pushvalue(L, -1);
      lua_setfield(L, badgeIdx, f->module);
    }
    lua_pushcfunction(L, f->fn);
    lua_setfield(L, -2, f->name);
    lua_pop(L, 1);                   // the module table
  }

  // Upstream leaves these in; they open any path on the filesystem as a chunk (finding F13).
  lua_pushnil(L);
  lua_setglobal(L, "loadfile");
  lua_pushnil(L);
  lua_setglobal(L, "dofile");

  lua_settop(L, badgeIdx);
}

}  // namespace vk::lua
