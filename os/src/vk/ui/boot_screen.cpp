// Boot screen (hook H15), WP01 stub: returns false, so upstream draws its own progress screen.
// The Receipt boot screen arrives in WP37.
#include "leds.h"

namespace vk::ui {

bool bootScreen(const char *step, const char *detail, uint8_t percent) {
  (void)step; (void)detail;
  leds::bootProgress(percent);   // the LED boot bar's input; a no-op until leds.cpp is filled
  return false;
}

}  // namespace vk::ui
