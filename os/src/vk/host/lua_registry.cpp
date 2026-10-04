// vk::lua::open: builds the `badge` table for the app being launched (app-host.md, "How it is
// enforced", step 2; hook H7).
//   - A registered Lua function is installed if its permission is granted. Otherwise a stub with
//     the same name is installed that raises
//       permission '<name>' not granted (add it to permissions= in app.ini)
//   - Each upstream module table named by a permission that is not granted (badge.http and
//     badge.wifi for `net`, ...) is replaced by an empty table whose every read and write raises
//     the same message.
//   - The globals `loadfile` and `dofile` are removed (finding F13).
#include "lua_registry.h"

#include <string.h>

#include "permissions.h"

namespace vk::lua {

namespace {

// Upvalue 1 is the permission's name. Serves as the stub of a function and as the __index,
// __newindex and __call of a refused module table.
int raiseNotGranted(lua_State *L) {
  return luaL_error(L, "permission '%s' not granted (add it to permissions= in app.ini)",
                    lua_tostring(L, lua_upvalueindex(1)));
}

void pushNotGranted(lua_State *L, const char *permission) {
  lua_pushstring(L, permission);
  lua_pushcclosure(L, raiseNotGranted, 1);
}

// Replaces badge.<table> (the `badge` table is at badgeIdx) with a table that raises on any access.
void refuseTable(lua_State *L, int badgeIdx, const char *table, size_t length, const char *permission) {
  lua_pushlstring(L, table, length);     // the key
  lua_newtable(L);                       // the replacement: empty, so every read reaches __index
  lua_newtable(L);                       // its metatable
  pushNotGranted(L, permission);
  lua_setfield(L, -2, "__index");
  pushNotGranted(L, permission);
  lua_setfield(L, -2, "__newindex");
  pushNotGranted(L, permission);
  lua_setfield(L, -2, "__call");
  lua_pushboolean(L, 0);                 // getmetatable() answers false and setmetatable() refuses
  lua_setfield(L, -2, "__metatable");
  lua_setmetatable(L, -2);
  lua_rawset(L, badgeIdx);               // badge[<table>] = replacement
}

}  // namespace

void open(lua_State *L) {
  // The grant set that preLaunch() prepared becomes the active one before anything is installed.
  vk::host::promotePending();

  luaL_checkstack(L, 8, "vk::lua::open");
  const int badgeIdx = lua_gettop(L);   // the `badge` table; it stays on top when we return

  // 1. Registered functions. This runs before any module table is replaced: a refused table would
  //    raise on the lua_setfield below.
  for (LuaFunction *f = LuaFunction::first(); f; f = f->next()) {
    if (!f->module || !f->name || !f->fn) continue;
    // badge.<module>: an upstream module table if there is one, else created on first use.
    if (lua_getfield(L, badgeIdx, f->module) != LUA_TTABLE) {
      lua_pop(L, 1);
      lua_newtable(L);
      lua_pushvalue(L, -1);
      lua_setfield(L, badgeIdx, f->module);
    }
    if (f->permission == nullptr || vk::host::granted(f->permission)) {
      lua_pushcfunction(L, f->fn);
    } else {
      pushNotGranted(L, f->permission);
    }
    lua_setfield(L, -2, f->name);
    lua_pop(L, 1);                   // the module table
  }

  // 2. Upstream module tables behind a permission the app does not hold.
  for (vk::host::Permission *p = vk::host::Permission::first(); p; p = p->next()) {
    if (!p->name || !p->upstream_tables || vk::host::granted(p->name)) continue;
    const char *at = p->upstream_tables;
    for (;;) {
      const char *end = strchr(at, ',');
      const size_t length = end ? (size_t)(end - at) : strlen(at);
      if (length > 0) refuseTable(L, badgeIdx, at, length, p->name);
      if (end == nullptr) break;
      at = end + 1;
    }
  }

  // Upstream leaves these in; they open any path on the filesystem as a chunk (finding F13).
  lua_pushnil(L);
  lua_setglobal(L, "loadfile");
  lua_pushnil(L);
  lua_setglobal(L, "dofile");

  lua_settop(L, badgeIdx);
}

}  // namespace vk::lua
