// badge.receipt (platform/lua-api.md, "badge.receipt"): the firmware's receipt kit for Lua apps, so
// that an app's screen is drawn by the same code, in the same fonts, as the shell's. No permission.
// lib/vk.lua's vk.ui calls these when the module exists and draws with badge.gfx when it does not.
//
//   receipt.page()
//   receipt.header(left [, right])           right nil: the time and the battery (receipt::statusRight)
//   receipt.title(text, y [, cx])            cx defaults to the middle of the screen
//   receipt.rule(y [, x0, x1])               defaults 10, 310
//   receipt.perforation(x, y0, y1)
//   receipt.row(x0, x1, y, label, value [, selected [, value_token]])
//                                            value_token: a badge.theme.color name, or an RGB565 number
//   receipt.subline(x0, x1, y, text [, selected])
//   receipt.amount(cx, y, label, value, unit)
//   receipt.barcode(x, y, w, h [, seed])     seed: a byte string; default this badge's public key
//   receipt.footer(left, right)
//   receipt.hold_bar(progress)               0..1
//   receipt.qr(x, y, size, text) -> boolean  false when nothing was drawn (empty or too long a text)
//
// Nothing an app passes can hurt the firmware: an argument of the wrong type raises a Lua error
// (luaL_check*), coordinates are clamped to a band around the screen, so no loop runs long, and a
// text is cut to what a screen can show before the kit sees it. A text may be nil (drawn as
// nothing) or a number.
#include <Arduino.h>

#include <math.h>
#include <string.h>

#include "../host/lua_registry.h"
#include "../wallet/signer.h"
#include "receipt.h"

namespace {

namespace receipt = vk::ui::receipt;

constexpr int SCREEN_W = 320, SCREEN_H = 240;
constexpr int X_MIN = -SCREEN_W, X_MAX = 2 * SCREEN_W;     // one screen of slack on each side
constexpr int Y_MIN = -SCREEN_H, Y_MAX = 2 * SCREEN_H;
constexpr size_t TEXT_CAP = 161;         // 53 columns; a mark the kit draws by hand is 3 bytes of UTF-8
constexpr size_t LINK_CAP = 161;         // receipt::qr takes at most 154 bytes

// A number (an integer or a float: `ui.W / 2` is a float), rounded down and clamped.
int clamped(lua_Number v, int lo, int hi) {
  if (!(v >= (lua_Number)lo)) return lo;                    // also NaN
  if (v > (lua_Number)hi) return hi;
  return (int)floor(v);
}
int xArg(lua_State *L, int index) { return clamped(luaL_checknumber(L, index), X_MIN, X_MAX); }
int yArg(lua_State *L, int index) { return clamped(luaL_checknumber(L, index), Y_MIN, Y_MAX); }
int xOpt(lua_State *L, int index, int fallback) { return lua_isnoneornil(L, index) ? fallback : xArg(L, index); }

// A text argument, cut to `cap` bytes; nil or absent is the empty text.
const char *textArg(lua_State *L, int index, char *out, size_t cap) {
  strlcpy(out, luaL_optstring(L, index, ""), cap);
  return out;
}

int l_page(lua_State *) {
  receipt::page();
  return 0;
}

int l_header(lua_State *L) {
  char left[TEXT_CAP], right[TEXT_CAP];
  textArg(L, 1, left, sizeof left);
  if (lua_isnoneornil(L, 2)) {
    receipt::statusRight(right, sizeof right);
  } else {
    textArg(L, 2, right, sizeof right);
  }
  receipt::header(left, right);
  return 0;
}

int l_footer(lua_State *L) {
  char left[TEXT_CAP], right[TEXT_CAP];
  receipt::footer(textArg(L, 1, left, sizeof left), textArg(L, 2, right, sizeof right));
  return 0;
}

int l_title(lua_State *L) {
  char text[TEXT_CAP];
  textArg(L, 1, text, sizeof text);
  const int y = yArg(L, 2);
  receipt::title(text, y, xOpt(L, 3, SCREEN_W / 2));
  return 0;
}

int l_rule(lua_State *L) {
  const int y = yArg(L, 1);
  receipt::rule(y, xOpt(L, 2, 10), xOpt(L, 3, SCREEN_W - 10));
  return 0;
}

int l_perforation(lua_State *L) {
  const int x = xArg(L, 1), y0 = yArg(L, 2), y1 = yArg(L, 3);
  receipt::perforation(x, y0, y1);
  return 0;
}

int l_row(lua_State *L) {
  char label[TEXT_CAP], value[TEXT_CAP];
  const int x0 = xArg(L, 1), x1 = xArg(L, 2), y = yArg(L, 3);
  textArg(L, 4, label, sizeof label);
  textArg(L, 5, value, sizeof value);
  const bool selected = lua_toboolean(L, 6);
  // The value's colour: a token name, mapped here; a number is taken as RGB565 (what
  // badge.theme.color returns); nil, or a name that is not a colour, is the row's own ink.
  uint16_t color = 0;
  if (lua_type(L, 7) == LUA_TNUMBER) {
    color = (uint16_t)clamped(lua_tonumber(L, 7), 0, 0xFFFF);
  } else if (!lua_isnoneornil(L, 7)) {
    vk::ui::theme::colorByName(luaL_checkstring(L, 7), color);
  }
  receipt::row(x0, x1, y, label, value, selected, color);
  return 0;
}

int l_subline(lua_State *L) {
  char text[TEXT_CAP];
  const int x0 = xArg(L, 1), x1 = xArg(L, 2), y = yArg(L, 3);
  receipt::subline(x0, x1, y, textArg(L, 4, text, sizeof text), lua_toboolean(L, 5));
  return 0;
}

int l_amount(lua_State *L) {
  char label[TEXT_CAP], value[TEXT_CAP], unit[TEXT_CAP];
  const int cx = xArg(L, 1), y = yArg(L, 2);
  textArg(L, 3, label, sizeof label);
  textArg(L, 4, value, sizeof value);
  textArg(L, 5, unit, sizeof unit);
  receipt::amount(cx, y, label, value, unit);
  return 0;
}

int l_barcode(lua_State *L) {
  const int x = xArg(L, 1), y = yArg(L, 2);
  const int w = clamped(luaL_checknumber(L, 3), 0, SCREEN_W), h = clamped(luaL_checknumber(L, 4), 0, SCREEN_H);
  size_t length = 0;
  const uint8_t *seed = (const uint8_t *)luaL_optlstring(L, 5, nullptr, &length);
  if (seed == nullptr || length == 0) {
    seed = vk::wallet::publicKey();                         // nullptr with no identity: the kit's plain bars
    length = seed ? 32 : 0;
  }
  receipt::barcode(x, y, w, h, seed, length);
  return 0;
}

int l_hold_bar(lua_State *L) {
  receipt::holdBar((float)luaL_checknumber(L, 1));          // the kit clamps to 0..1, NaN included
  return 0;
}

int l_qr(lua_State *L) {
  char text[LINK_CAP];
  const int x = xArg(L, 1), y = yArg(L, 2);
  const int size = clamped(luaL_checknumber(L, 3), 0, SCREEN_W);
  // A text longer than the buffer is cut here to more than the kit accepts, so it is refused.
  lua_pushboolean(L, receipt::qr(x, y, size, textArg(L, 4, text, sizeof text)));
  return 1;
}

VK_LUA_FUNCTION(receipt_page, "receipt", "page", nullptr, l_page);
VK_LUA_FUNCTION(receipt_header, "receipt", "header", nullptr, l_header);
VK_LUA_FUNCTION(receipt_footer, "receipt", "footer", nullptr, l_footer);
VK_LUA_FUNCTION(receipt_title, "receipt", "title", nullptr, l_title);
VK_LUA_FUNCTION(receipt_rule, "receipt", "rule", nullptr, l_rule);
VK_LUA_FUNCTION(receipt_perforation, "receipt", "perforation", nullptr, l_perforation);
VK_LUA_FUNCTION(receipt_row, "receipt", "row", nullptr, l_row);
VK_LUA_FUNCTION(receipt_subline, "receipt", "subline", nullptr, l_subline);
VK_LUA_FUNCTION(receipt_amount, "receipt", "amount", nullptr, l_amount);
VK_LUA_FUNCTION(receipt_barcode, "receipt", "barcode", nullptr, l_barcode);
VK_LUA_FUNCTION(receipt_hold_bar, "receipt", "hold_bar", nullptr, l_hold_bar);
VK_LUA_FUNCTION(receipt_qr, "receipt", "qr", nullptr, l_qr);

}  // namespace
