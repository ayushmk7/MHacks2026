// Screen dim and sleep (ui.md, "Screen dim and sleep"). The state machine is pure C in power_core.c
// (host suite test_power); this file feeds it and applies its answer to the backlight.
//
// Backlight only. The panel is not put to sleep and the CPU does not light-sleep: in light sleep the
// loop stops, so ESP-NOW frames (payment requests, presence proofs) and the USB console would be lost,
// and the button expander's interrupt line is only a hint to a poll that would stop too. The
// backlight is by far the largest load this can remove; the canvas keeps being drawn and sent as
// usual, so the picture is current the moment the screen wakes.
//
// Who owns the backlight: the user's level is settings::brightness(); an app may set its own with
// badge.gfx.brightness; the approval engine lights the screen when it opens and puts back the level it
// found when it closes. This service changes the level only on a transition, remembers the level it
// replaced, and puts it back on waking only if nobody changed it meanwhile.
#include "screen_power.h"

#include <Arduino.h>

#include "../../badge_log.h"
#include "../../hal/buttons.h"
#include "../../hal/display.h"
#include "../../hal/leds.h"
#include "../../hal/power.h"
#include "../../lua_sdk/lua_runtime.h"
#include "../../net/broker_client.h"
#include "../core/config.h"
#include "../core/serial.h"
#include "../core/service.h"
#include "../host/home.h"
#include "../host/lifecycle.h"
#include "../host/notify.h"
#include "../vk.h"
#include "battery.h"
#include "leds.h"
#include "power_core.h"

namespace vk::ui::screen {

namespace {

VK_CONFIG_KEY(dim_s, "dim_s", vk::config::Type::U32, "30", vk::config::F_NONE, 0, 3600,
              "seconds with no activity before the backlight dims; 0 never");
VK_CONFIG_KEY(sleep_s, "sleep_s", vk::config::Type::U32, "120", vk::config::F_NONE, 0, 3600,
              "seconds with no activity before the backlight goes off; 0 never");
VK_CONFIG_KEY(dim_pct, "dim_pct", vk::config::Type::U32, "25", vk::config::F_NONE, 1, 100,
              "dimmed backlight, percent of the awake level");
VK_CONFIG_KEY(awake_usb, "awake_usb", vk::config::Type::U32, "0", vk::config::F_NONE, 0, 1,
              "1: never dim or sleep on external power");
VK_CONFIG_KEY(crit_sleep_s, "crit_sleep_s", vk::config::Type::U32, "30", vk::config::F_NONE, 0, 3600,
              "at critical battery, sleep after this many idle seconds if sooner; 0 off");

// The config is read this often, not on every pass: a key past the config cache's 32 slots is read
// from NVS on every call.
constexpr uint32_t CONFIG_EVERY_MS = 500;

vk_idle_t sIdle = {0, VK_SCREEN_AWAKE, 0};
uint8_t sApplied = VK_SCREEN_AWAKE;   // the state the backlight shows
uint8_t sAwakeLevel = 0;              // the level replaced when the screen dimmed or slept
uint8_t sSetLevel = 0;                // the level this service set
bool sRestorePending = false;         // woken under an approval: put the level back when it closes
bool sLedsQuieted = false;            // the idle LED animation was stopped for the sleep

bool sKeepAwake = false;

// Things other than keys that the wearer will want to see.
size_t sNotes = 0;
bool sAppRunning = false;
bool sOffer = false;

uint32_t sConfigAt = 0;
bool sConfigRead = false;
uint32_t sDimS = 0, sSleepS = 0, sDimPct = 100, sAwakeUsb = 0, sCritSleepS = 0;

const char *nameOf(uint8_t state) {
  switch (state) {
    case VK_SCREEN_DIM: return "dim";
    case VK_SCREEN_SLEEP: return "sleep";
    default: return "awake";
  }
}

void readConfig(uint32_t now) {
  if (sConfigRead && (uint32_t)(now - sConfigAt) < CONFIG_EVERY_MS) return;
  sConfigRead = true;
  sConfigAt = now;
  sDimS = vk::config::u32("dim_s");
  sSleepS = vk::config::u32("sleep_s");
  sDimPct = vk::config::u32("dim_pct");
  sAwakeUsb = vk::config::u32("awake_usb");
  sCritSleepS = vk::config::u32("crit_sleep_s");
}

// The backlight for a new state.
void apply(uint8_t state) {
  if (state == sApplied) return;
  const uint8_t from = sApplied;
  sApplied = state;
  badge_log::tagf("vk", "screen %s", nameOf(state));

  if (state == VK_SCREEN_AWAKE) {
    if (display::brightness() == sSetLevel) {
      display::setBrightness(sAwakeLevel);
    } else if (vk::modalActive()) {
      sRestorePending = true;   // the approval lit the screen and will put back our level when it closes
    }
    if (sLedsQuieted) {
      sLedsQuieted = false;
      if (vk::host::idle() && !vk::ui::leds::playing()) ::leds::playIdle();
    }
    return;
  }

  // Dim or sleep. The level to come back to is the one on screen when the screen was last awake,
  // or a level somebody set while it was dim (an app, an approval that closed).
  if (from == VK_SCREEN_AWAKE || display::brightness() != sSetLevel) sAwakeLevel = display::brightness();
  sRestorePending = false;
  sSetLevel = state == VK_SCREEN_DIM ? vk_dim_level(sAwakeLevel, sDimPct) : 0;
  display::setBrightness(sSetLevel);

  // Asleep in the shell: the idle LED animation (full colour) goes too. A registered pattern (the
  // notify breath, an approval's) is left alone: it means something. An app's LEDs are the app's.
  if (state == VK_SCREEN_SLEEP && vk::host::idle() && !vk::ui::leds::playing()) {
    ::leds::stopAnimation();
    ::leds::off();
    sLedsQuieted = true;
  }
}

void serviceBegin() {
  vk_idle_init(&sIdle, millis());
  sNotes = vk::host::notify::count();
  sAppRunning = !vk::host::idle();
}

void serviceUpdate() {
  const uint32_t now = millis();
  readConfig(now);

  // Activity that is not a key.
  const size_t notes = vk::host::notify::count();
  if (notes > sNotes) (void)vk_idle_activity(&sIdle, now);   // a new notification is shown, not slept through
  sNotes = notes;
  const bool appRunning = !vk::host::idle();
  if (appRunning != sAppRunning) (void)vk_idle_activity(&sIdle, now);   // an app started or stopped
  sAppRunning = appRunning;
  const bool offer = ::broker::hasOffer();
  if (offer && !sOffer) (void)vk_idle_activity(&sIdle, now);           // the app-store offer appears
  sOffer = offer;
#if VK_TEST_HOOKS
  // Injected buttons (VKBTN) are added after hook H25 and so are never swallowed; they still count.
  if (buttons::pressedMask() != 0) (void)vk_idle_activity(&sIdle, now);
#endif

  const bool modal = vk::modalActive();
  const bool hold = modal || sKeepAwake || (sAwakeUsb != 0 && ::power::charging());

  uint32_t sleepS = sSleepS;
  if (vk::ui::battery::critical() && sCritSleepS != 0 && (sleepS == 0 || sCritSleepS < sleepS)) {
    sleepS = sCritSleepS;
  }
  apply(vk_idle_step(&sIdle, now, sDimS, sleepS, hold ? 1 : 0));

  // Woken while an approval was open: when it closes it restores the level it found, which was ours.
  if (sRestorePending && !modal) {
    sRestorePending = false;
    if (display::brightness() == sSetLevel) display::setBrightness(sAwakeLevel);
  }
}

void onAppStop(const char *) { sKeepAwake = false; }

String infoDisplay() { return String(nameOf(sApplied)); }

}  // namespace

State state() { return (State)sApplied; }
const char *stateName() { return nameOf(sApplied); }

void keepAwake(bool on) {
  sKeepAwake = on;
  if (on) (void)vk_idle_activity(&sIdle, millis());
}

bool keepingAwake() { return sKeepAwake; }

void wake() { (void)vk_idle_activity(&sIdle, millis()); }

VK_SERVICE(screen_power, serviceBegin, serviceUpdate);
VK_ON_APP_STOP(screen_power, onAppStop);
VK_INFO_FIELD(display, "display", infoDisplay);

}  // namespace vk::ui::screen

extern "C" void vk_screen_filter_buttons(uint8_t down, uint8_t *pressed, uint8_t *released) {
  if (pressed == nullptr || released == nullptr) return;
  vk_idle_keys(&vk::ui::screen::sIdle, millis(), down, pressed, released, vk::modalActive() ? 1 : 0);
}
