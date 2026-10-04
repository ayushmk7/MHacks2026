// LED patterns, WP01 stub: nothing plays and the boot bar does nothing, so upstream's LED
// animations run untouched. The pattern service and the boot bar arrive in WP12.
#include "leds.h"

namespace vk::ui::leds {

void play(const char *name) { (void)name; }
void stop() {}
bool playing() { return false; }
void bootProgress(uint8_t percent) { (void)percent; }

}  // namespace vk::ui::leds
