// src/vk/core/clock.h
// Where the time came from (wallet/checks.md, Clock). The raw system time is never trusted.
#pragma once

#include <stdint.h>

#include "../vk_build.h"   // VK_TEST_HOOKS

namespace vk::clock {
enum class Source : uint8_t { NONE, FLOOR, SNTP };
Source source();
uint32_t now();                 // unix seconds; meaningful only when source() != NONE
bool ok();                      // source() != NONE
void raiseTo(uint32_t unix_s);  // from a verified record's issued_at: if unix_s > now(), set the clock; NONE becomes FLOOR
// The venue's offset from UTC, in minutes (config key utc_offset, set on Settings > Badge), for
// DISPLAY only: the header and the settings pages print now() moved by it. Nothing else changes:
// now() stays UTC and source() is never touched by it, so the wallet checks cannot be moved by a
// person with the buttons (wallet/checks.md, "Clock"). Firmware only; 0 when unset.
int32_t utcOffsetMin();
#if VK_TEST_HOOKS
void devSet(uint32_t unix_s);   // dev profile only, for the VKTIME command: stores the time and sets the source to SNTP
#endif
}
