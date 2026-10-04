// Theme registry and colour lookup. WP01 ships one theme, "solana", carrying upstream's colours;
// WP12 replaces it with receipt-light and receipt-dark.
#include "theme.h"

#include <string.h>

#include "../core/config.h"

namespace vk::ui::theme {

VK_CONFIG_KEY(theme, "theme", vk::config::Type::STR, "", vk::config::F_NONE, 0, 24, "active theme name; empty = the default theme");

// Upstream's palette (src/ui/theme.h), token by token:
//                        PAPER=BG  INK=TEXT  FAINT=BORDER SUB=MUTED STAMP_OK=GREEN STAMP_WARN=WARN STAMP_BAD=ERR LED=PURPLE
VK_THEME(solana, "solana", 0x0B0B12, 0xE8E8F0, 0x3A3A4E, 0x9393A8, 0x14F195, 0xFFB020, 0xFF4545, 0x9945FF);

Theme::Theme(const char *n, std::initializer_list<uint32_t> rgb888) : name(n), colors{} {
  size_t i = 0;
  for (uint32_t c : rgb888) {
    if (i >= TOKEN_COUNT) break;
    const uint8_t r = (uint8_t)(c >> 16), g = (uint8_t)(c >> 8), b = (uint8_t)c;
    colors[i++] = (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
  }
}

namespace {

const Theme *sActive = nullptr;

const Theme *find(const char *name) {
  if (name == nullptr || name[0] == '\0') return nullptr;
  for (const Theme *t = Theme::first(); t; t = t->next()) {
    if (strcmp(t->name, name) == 0) return t;
  }
  return nullptr;
}

// Resolved on first use, not in a begin(): color() is called during boot, before vk::begin().
// An empty or unknown stored name means the default theme.
const Theme *active() {
  if (sActive == nullptr) {
    sActive = find(vk::config::text("theme").c_str());
    if (sActive == nullptr) sActive = Theme::first();
  }
  return sActive;
}

}  // namespace

uint16_t color(Token token) {
  const Theme *t = active();
  if (t == nullptr || token >= TOKEN_COUNT) return 0;
  return t->colors[token];
}

uint16_t blend(Token a, Token b, uint8_t amount) {
  const uint16_t ca = color(a), cb = color(b);
  const int ra = (ca >> 11) & 0x1F, ga = (ca >> 5) & 0x3F, ba = ca & 0x1F;
  const int rb = (cb >> 11) & 0x1F, gb = (cb >> 5) & 0x3F, bb = cb & 0x1F;
  const int r = ra + (rb - ra) * amount / 255;
  const int g = ga + (gb - ga) * amount / 255;
  const int bl = ba + (bb - ba) * amount / 255;
  return (uint16_t)((r << 11) | (g << 5) | bl);
}

const char *activeName() {
  const Theme *t = active();
  return t ? t->name : "";
}

void setActive(const char *name) {
  const Theme *t = find(name);
  if (t == nullptr) return;                 // unknown name: no change
  sActive = t;
  vk::config::set("theme", t->name);        // persisted once the config store exists (WP10)
}

size_t count() {
  size_t n = 0;
  for (const Theme *t = Theme::first(); t; t = t->next()) ++n;
  return n;
}

const Theme *at(size_t i) {
  for (const Theme *t = Theme::first(); t; t = t->next()) {
    if (i-- == 0) return t;
  }
  return nullptr;
}

}  // namespace vk::ui::theme
