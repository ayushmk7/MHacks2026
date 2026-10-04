// Theme tokens and the theme registry (ui.md, "Tokens and modes").
// Inside namespace vk::ui, `theme::` is this namespace; upstream's palette is `::theme::`.
#pragma once

#include <Arduino.h>
#include <initializer_list>

#include "../core/registry.h"

namespace vk::ui::theme {
// STAMP_OK, STAMP_WARN and STAMP_BAD are the status inks: the colours of signed, warning and blocked text in lists.
// (The names are from the first design, which printed rubber stamps; no stamp is drawn any more.)
enum Token : uint8_t { PAPER, INK, FAINT, SUB, STAMP_OK, STAMP_WARN, STAMP_BAD, LED, TOKEN_COUNT };
struct Theme : Registered<Theme> {
  const char *name;
  uint16_t colors[TOKEN_COUNT];      // RGB565
  Theme(const char *n, std::initializer_list<uint32_t> rgb888);   // converts to RGB565
};
#define VK_THEME(ident, name, ...) static vk::ui::theme::Theme vk_theme_##ident(name, {__VA_ARGS__})
uint16_t color(Token token);         // from the active theme
uint16_t blend(Token a, Token b, uint8_t amount);   // a towards b, 0..255
const char *activeName();
void setActive(const char *name);    // writes config key `theme`; unknown name: no change
size_t count();  const Theme *at(size_t i);

// --- Added with the QR code and badge.receipt; nothing above changed. ---
const Theme *find(const char *name);                // the registered theme with that name, or nullptr
// The colour a Lua app names (lua_theme.cpp): a token ("paper", "ink", "faint", "sub", "stamp_ok",
// "stamp_warn", "stamp_bad", "led") from the active theme, or the fixed "green", "amber", "red".
// False for any other name; `out` is then untouched.
bool colorByName(const char *name, uint16_t &out);
}
