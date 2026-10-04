// Theme registry and colour lookup (ui.md, "Tokens and modes"): the Receipt design in a light and a
// dark mode. A theme changes paper and ink only; the approval's severity colours are constants in
// approval_screen.cpp and are not tokens.
#include "theme.h"

#include <string.h>

#include "../core/config.h"

namespace vk::ui::theme {

VK_CONFIG_KEY(theme, "theme", vk::config::Type::STR, "", vk::config::F_NONE, 0, 24,
              "active theme: receipt-light (also when empty) or receipt-dark");

// STAMP_OK, STAMP_WARN, STAMP_BAD: the status inks (signed, warning, blocked text in lists).
//                                     PAPER     INK       FAINT     SUB       STAMP_OK  STAMP_WARN STAMP_BAD LED
VK_THEME(receipt_light, "receipt-light", 0xF3EFE4, 0x1B1A17, 0x8A8474, 0x6D6759, 0x17804F, 0xB56A00, 0xC8321E, 0xFFE2AA);
VK_THEME(receipt_dark,  "receipt-dark",  0x15140F, 0xECE6D6, 0x7D7868, 0xA39C8A, 0x4FD69A, 0xFFC35A, 0xFF7B6E, 0xFFC478);

Theme::Theme(const char *n, std::initializer_list<uint32_t> rgb888) : name(n), colors{} {
  size_t i = 0;
  for (uint32_t c : rgb888) {
    if (i >= TOKEN_COUNT) break;
    const uint8_t r = (uint8_t)(c >> 16), g = (uint8_t)(c >> 8), b = (uint8_t)c;
    colors[i++] = (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
  }
}

const Theme *find(const char *name) {
  if (name == nullptr || name[0] == '\0') return nullptr;
  for (const Theme *t = Theme::first(); t; t = t->next()) {
    if (strcmp(t->name, name) == 0) return t;
  }
  return nullptr;
}

namespace {

constexpr char DEFAULT_THEME[] = "receipt-light";
constexpr uint32_t RESOLVE_EVERY_MS = 500;

const Theme *sActive = nullptr;
uint32_t sResolvedAt = 0;

// An empty or unknown stored name means receipt-light. If that theme's line was deleted, any
// registered theme serves.
const Theme *resolve() {
  const Theme *t = find(vk::config::text("theme").c_str());
  if (t == nullptr) t = find(DEFAULT_THEME);
  if (t == nullptr) t = Theme::first();
  return t;
}

// Resolved on first use, not in a begin(): color() is called by the boot screen, before vk::begin()
// (the config accessors initialise lazily). The config key is the truth, so it is read again twice
// a second: a `VKSET theme <name>` over serial then shows without a reboot, and a colour lookup
// stays a table read.
const Theme *active() {
  const uint32_t now = millis();
  if (sActive == nullptr || (uint32_t)(now - sResolvedAt) >= RESOLVE_EVERY_MS) {
    sActive = resolve();
    sResolvedAt = now;
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
  vk::config::set("theme", t->name);
  // Shown at once. If the write was refused the stored name wins again at the next resolve.
  sActive = t;
  sResolvedAt = millis();
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
