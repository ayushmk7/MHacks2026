// Low battery (ui.md, "Low battery"): the measured cell percentage against two thresholds, with
// hysteresis. Entering a level posts one notification and blinks the LEDs once; the header marks the
// battery figure while the level lasts. Nothing is predicted: the level comes from the same measured
// figure the header shows, and on external power (no cell reading) there is no level.
#pragma once

#include <stdint.h>

#include "power_core.h"

namespace vk::ui::battery {

uint8_t level();            // VK_BATT_OK, VK_BATT_LOW or VK_BATT_CRITICAL (power_core.h)
bool critical();            // level() == VK_BATT_CRITICAL
const char *levelName();    // "ok", "low", "critical"

}  // namespace vk::ui::battery
