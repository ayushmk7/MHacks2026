// LED patterns and the boot bar (ui.md, "LED patterns"), and the boot screen entry (hook H15).
// Inside namespace vk::ui, `leds::` is this namespace; upstream's LED driver is `::leds::`.
#pragma once

#include <Arduino.h>

#include "../core/registry.h"

namespace vk::ui::leds {
struct LedPattern : Registered<LedPattern> {
  const char *name;
  bool (*frame)(uint32_t t_ms);     // t_ms since the pattern started. Sets LEDs with ::leds::set(). Return false when finished
  LedPattern(const char *n, bool (*f)(uint32_t)) : name(n), frame(f) {}
};
#define VK_LED_PATTERN(ident, name, frame_fn) static vk::ui::leds::LedPattern vk_led_##ident(name, frame_fn)

void play(const char *name);        // replaces whatever is playing; unknown name: logs and does nothing
void stop();
bool playing();
void bootProgress(uint8_t percent); // hook H15
}

namespace vk::ui {
// Hook H15, defined in ui/boot_screen.cpp. True when the Receipt boot screen was drawn and flushed,
// so upstream's own progress drawing is skipped.
bool bootScreen(const char *step, const char *detail, uint8_t percent);
}
