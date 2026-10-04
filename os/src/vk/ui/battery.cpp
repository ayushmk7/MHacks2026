// Low battery (ui.md, "Low battery"). The thresholds and the hysteresis are pure C in power_core.c
// (host suite test_power); this file reads the measured figure and tells the wearer once per level.
//
// Not nagging: one notification and one LED blink when a level is entered, none while it lasts, and a
// level is left only hysteresis percent above its threshold, so a cell that sags under the radio's
// load does not warn again and again. The header marks the battery figure for as long as the level
// lasts (receipt::statusRight). What happens at critical level beyond the warning is in ui.md.
#include "battery.h"

#include <Arduino.h>

#include "../../badge_log.h"
#include "../../hal/power.h"
#include "../core/config.h"
#include "../core/serial.h"
#include "../core/service.h"
#include "../host/notify.h"
#include "../vk.h"
#include "../wallet/signer.h"
#include "leds.h"

namespace vk::ui::battery {

namespace {

VK_CONFIG_KEY(batt_low_pct, "batt_low_pct", vk::config::Type::U32, "20", vk::config::F_NONE, 0, 90,
              "battery percent at or below which the badge warns once; 0 off");
VK_CONFIG_KEY(batt_crit_pct, "batt_crit_pct", vk::config::Type::U32, "8", vk::config::F_NONE, 0, 50,
              "battery percent at or below which the battery is critical; 0 off");
VK_CONFIG_KEY(batt_hyst_pct, "batt_hyst_pct", vk::config::Type::U32, "3", vk::config::F_NONE, 0, 20,
              "a battery warning clears only this many percent above its threshold");

// The cell is read by upstream every 500 ms and filtered; once a second is plenty here.
constexpr uint32_t CHECK_EVERY_MS = 1000;

uint8_t sLevel = VK_BATT_OK;
uint32_t sLastCheckMs = 0;
bool sChecked = false;

const char *nameOf(uint8_t level) {
  switch (level) {
    case VK_BATT_LOW: return "low";
    case VK_BATT_CRITICAL: return "critical";
    default: return "ok";
  }
}

// The measured percentage, rounded as the header rounds it. False when there is no cell reading.
bool measured(int &percent) {
  if (::power::charging() || ::power::volts() <= 0.1f) return false;
  percent = (int)(::power::percent() + 0.5f);
  return true;
}

void serviceUpdate() {
  const uint32_t now = millis();
  if (sChecked && (uint32_t)(now - sLastCheckMs) < CHECK_EVERY_MS) return;
  sChecked = true;
  sLastCheckMs = now;

  int percent = 0;
  const bool have = measured(percent);
  const uint8_t next = vk_batt_level(sLevel, have ? 1 : 0, percent, vk::config::u32("batt_low_pct"),
                                     vk::config::u32("batt_crit_pct"), vk::config::u32("batt_hyst_pct"));
  if (next == sLevel) return;
  const uint8_t before = sLevel;
  sLevel = next;
  badge_log::tagf("vk", "battery %s (%d%%)", nameOf(next), have ? percent : -1);
  if (next < before) return;   // better: the header mark goes on its own; nothing to say

  char body[sizeof(vk::host::notify::Note::body)];
  if (next == VK_BATT_CRITICAL) {
    snprintf(body, sizeof body, "%d%% left: charge it now", percent);
    vk::host::notify::post("Battery critical", body, "");
  } else {
    snprintf(body, sizeof body, "%d%% left: charge it soon", percent);
    vk::host::notify::post("Battery low", body, "");
  }
  // One blink, unless the LEDs already mean something (an approval, its result).
  if (!vk::modalActive() && !vk::ui::leds::playing()) vk::ui::leds::play("low_battery");
}

String infoBattery() { return String(nameOf(sLevel)); }

}  // namespace

uint8_t level() { return sLevel; }
bool critical() { return sLevel == VK_BATT_CRITICAL; }
const char *levelName() { return nameOf(sLevel); }

void serviceBegin() { vk::wallet::batteryCritical = critical; }

VK_SERVICE(battery, serviceBegin, serviceUpdate);
VK_INFO_FIELD(battery, "battery", infoBattery);

}  // namespace vk::ui::battery
