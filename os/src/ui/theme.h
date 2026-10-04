/*
  The compile-time palette, holding BadgeOS's Receipt-light values
  (docs/os/architecture/upstream-hooks.md, "Replaced upstream files"). Every
  constant name is upstream's, so code that uses them compiles unchanged;
  BadgeOS's own screens take colours from vk::ui::theme instead. Colours are RGB565 because that is what both the panel and
  every LovyanGFX call want - rgb565() does the conversion at compile time so
  the hex in the comments stays readable.
*/
#pragma once

#include <Arduino.h>

constexpr uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) {
  return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

namespace theme {

// -- Upstream's brand names, now ink and the status inks -----------------------
constexpr uint32_t BRAND_PURPLE_RGB = 0x1B1A17;
constexpr uint32_t BRAND_GREEN_RGB  = 0xFFE2AA;
constexpr uint32_t BRAND_TEAL_RGB   = 0xFFE2AA;
constexpr uint32_t BRAND_MAGENTA_RGB = 0x1B1A17;

constexpr uint16_t PURPLE  = rgb565(0x1B, 0x1A, 0x17);
constexpr uint16_t GREEN   = rgb565(0x17, 0x80, 0x4F);
constexpr uint16_t TEAL    = rgb565(0x17, 0x80, 0x4F);
constexpr uint16_t MAGENTA = rgb565(0xC8, 0x32, 0x1E);

// -- Surfaces ----------------------------------------------------------------
constexpr uint16_t BLACK   = rgb565(0x00, 0x00, 0x00);
constexpr uint16_t BG      = rgb565(0xF3, 0xEF, 0xE4);
constexpr uint16_t HEADER  = rgb565(0xF3, 0xEF, 0xE4);
constexpr uint16_t PANEL   = rgb565(0xF3, 0xEF, 0xE4);
constexpr uint16_t PANEL_2 = rgb565(0xE4, 0xDF, 0xD0);
constexpr uint16_t BORDER  = rgb565(0x8A, 0x84, 0x74);

// -- Text --------------------------------------------------------------------
constexpr uint16_t WHITE = rgb565(0xFF, 0xFF, 0xFF);
constexpr uint16_t TEXT  = rgb565(0x1B, 0x1A, 0x17);
constexpr uint16_t MUTED = rgb565(0x6D, 0x67, 0x59);

// -- Status ------------------------------------------------------------------
constexpr uint16_t OK      = GREEN;
constexpr uint16_t WARN    = rgb565(0xB5, 0x6A, 0x00);
constexpr uint16_t ERR     = rgb565(0xC8, 0x32, 0x1E);
constexpr uint16_t ACCENT  = PURPLE;

// Interpolates the ramp from BRAND_PURPLE_RGB (ink) at 0.0 to BRAND_GREEN_RGB
// (the LED colour) at 1.0. Lua's gfx.gradient and led.gradient use it.
void gradient(float t, uint8_t &r, uint8_t &g, uint8_t &b);
uint16_t gradient565(float t);

}  // namespace theme
