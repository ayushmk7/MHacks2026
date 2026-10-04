// src/vk/core/clock.cpp
// WP01 stub of the clock service (wallet/checks.md, Clock). The source stays NONE: no SNTP and no
// floor yet. Only the dev command VKTIME can set the time, through devSet(). WP20 fills this file.
#include "clock.h"

#include <Arduino.h>

namespace vk::clock {
namespace {

Source sSource = Source::NONE;
uint32_t sBaseUnix = 0;   // the time given to devSet()
uint32_t sBaseMs = 0;     // millis() when it was given

}  // namespace

Source source() { return sSource; }

uint32_t now() {
  if (sSource == Source::NONE) return 0;
  return sBaseUnix + (uint32_t)((millis() - sBaseMs) / 1000);
}

bool ok() { return sSource != Source::NONE; }

void raiseTo(uint32_t unix_s) { (void)unix_s; }

#if VK_TEST_HOOKS
void devSet(uint32_t unix_s) {
  sBaseUnix = unix_s;
  sBaseMs = millis();
  sSource = Source::SNTP;
}
#endif

}  // namespace vk::clock
