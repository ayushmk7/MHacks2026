// BadgeOS boot sequence: no splash. Each stage of setup() draws the Receipt boot screen
// (docs/os/ui/shell.md, "Boot"). This file replaces upstream's boot.cpp
// (docs/os/architecture/upstream-hooks.md, "Replaced upstream files").
#include "boot.h"

#include "../hal/display.h"
#include "../settings.h"
#include "../vk/ui/leds.h"

namespace boot {

void progress(const char *step, const char *detail, uint8_t percent) {
  vk::ui::bootScreen(step, detail, percent);   // draws the screen, flushes it, advances the LED boot bar
}

void run() {
  display::setBrightness(settings::brightness());   // upstream's splash used to fade the backlight in
  progress("", "power on", 0);
}

}  // namespace boot
