// badge.screen.keep_awake(on) (platform/lua-api.md, "badge.screen"; ui.md, "Screen dim and sleep").
// No permission: keeping the screen lit costs battery, not trust, and it ends when the app stops.
#include "../host/lua_registry.h"
#include "screen_power.h"

namespace {

// badge.screen.keep_awake(on): while on is true the screen neither dims nor sleeps. Returns nothing.
int l_keep_awake(lua_State *L) {
  luaL_checkany(L, 1);
  vk::ui::screen::keepAwake(lua_toboolean(L, 1) != 0);
  return 0;
}

}  // namespace

VK_LUA_FUNCTION(screen_keep_awake, "screen", "keep_awake", nullptr, l_keep_awake);
