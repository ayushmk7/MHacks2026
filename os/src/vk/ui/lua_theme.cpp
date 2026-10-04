// badge.theme (platform/lua-api.md, "badge.theme"): lets a Lua app draw in the active theme's
// colours. No permission. lib/vk.lua's vk.ui draws with these.
//   theme.name()         "receipt-light", "receipt-dark" or another registered theme
//   theme.color(token)   RGB565 for "paper", "ink", "faint", "sub", "stamp_ok", "stamp_warn",
//                        "stamp_bad", "led" (from the active theme) and for the fixed "green",
//                        "amber", "red"; nil for any other name
#include <Arduino.h>

#include <string.h>

#include "../host/lua_registry.h"
#include "theme.h"

namespace {

constexpr uint16_t toRgb565(uint32_t rgb) {
  return (uint16_t)((((rgb >> 16) & 0xF8) << 8) | (((rgb >> 8) & 0xFC) << 3) | ((rgb & 0xFF) >> 3));
}

struct TokenName {
  const char *name;
  vk::ui::theme::Token token;
};

// The Lua names of the theme tokens (ui.md, "Tokens and modes").
const TokenName kTokens[] = {
    {"paper", vk::ui::theme::PAPER},
    {"ink", vk::ui::theme::INK},
    {"faint", vk::ui::theme::FAINT},
    {"sub", vk::ui::theme::SUB},
    {"stamp_ok", vk::ui::theme::STAMP_OK},
    {"stamp_warn", vk::ui::theme::STAMP_WARN},
    {"stamp_bad", vk::ui::theme::STAMP_BAD},
    {"led", vk::ui::theme::LED},
};

struct FixedColor {
  const char *name;
  uint16_t color;
};

// The approval's severity colours: not tokens, the same in every theme (approval.md, "Screen").
const FixedColor kFixedColors[] = {
    {"green", toRgb565(0x1FBF75)},
    {"amber", toRgb565(0xFFB020)},
    {"red", toRgb565(0xFF4545)},
};

// theme.name() -> the active theme's name
int l_name(lua_State *L) {
  lua_pushstring(L, vk::ui::theme::activeName());
  return 1;
}

// theme.color(token) -> RGB565 | nil
int l_color(lua_State *L) {
  const char *name = luaL_checkstring(L, 1);
  for (const TokenName &t : kTokens) {
    if (strcmp(t.name, name) == 0) {
      lua_pushinteger(L, (lua_Integer)vk::ui::theme::color(t.token));
      return 1;
    }
  }
  for (const FixedColor &f : kFixedColors) {
    if (strcmp(f.name, name) == 0) {
      lua_pushinteger(L, (lua_Integer)f.color);
      return 1;
    }
  }
  lua_pushnil(L);
  return 1;
}

VK_LUA_FUNCTION(theme_name, "theme", "name", nullptr, l_name);
VK_LUA_FUNCTION(theme_color, "theme", "color", nullptr, l_color);

}  // namespace
