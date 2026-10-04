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
// For printing only: unix_s moved by the config key utc_offset_min (minutes, -720..840; anything
// else counts as 0). now() stays UTC for every check, record and SNTP; only displays call this.
uint32_t local(uint32_t unix_s);
#if VK_TEST_HOOKS
void devSet(uint32_t unix_s);   // dev profile only, for the VKTIME command: stores the time and sets the source to SNTP
#endif
}
