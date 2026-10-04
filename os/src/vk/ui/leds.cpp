// LED patterns and the boot bar (ui.md, "LED patterns" and "Boot bar").
//
// One registered pattern plays at a time. A service calls its frame function and then
// ::leds::show(). Upstream's own leds::update() also runs every loop and redraws whenever one of
// its animations is set, so every frame drawn here first calls ::leds::stopAnimation(). When a
// pattern ends or is stopped the LEDs go dark and, if the badge is idle, upstream's idle breath
// comes back. No loop assumes a number of LEDs: all of them run to RGB_LED_COUNT.
//
// Inside namespace vk::ui::leds, upstream's LED driver is ::leds::.
#include "leds.h"

#include <math.h>
#include <string.h>

#include "../../badge_log.h"
#include "../../config.h"       // RGB_LED_COUNT
#include "../../hal/leds.h"
#include "../core/service.h"
#include "../host/home.h"
#include "../wallet/approval.h"
#include "theme.h"

namespace vk::ui::leds {
namespace {

struct Rgb {
  uint8_t r, g, b;
};

// The approval's fixed severity colours (approval.md, "Screen"). They are not theme tokens.
constexpr Rgb SEVERITY_GREEN = {0x1F, 0xBF, 0x75};
constexpr Rgb SEVERITY_AMBER = {0xFF, 0xB0, 0x20};
constexpr Rgb SEVERITY_RED = {0xFF, 0x45, 0x45};

// A frame every 16 ms at most, as upstream's own animations do: each ::leds::show() is a blocking
// RMT transfer, and the loop is far faster than that when nothing is on screen.
constexpr uint32_t FRAME_MS = 16;
constexpr uint32_t BOOT_BLINK_MS = 170;   // boot bar: on 170 ms, off 170 ms
constexpr float PULSE_FLOOR = 0.12f;      // a pulse dims to this, never to black

const LedPattern *sCurrent = nullptr;
uint32_t sStartedAt = 0;
uint32_t sLastFrameAt = 0;
bool sFrameDue = false;                   // draw the first frame of a pattern at once
uint8_t sBootPercent = 0;

void fill(Rgb colour, float level) {
  if (level < 0.0f) level = 0.0f;
  if (level > 1.0f) level = 1.0f;
  for (uint8_t i = 0; i < RGB_LED_COUNT; ++i) {
    ::leds::set(i, (uint8_t)(colour.r * level), (uint8_t)(colour.g * level), (uint8_t)(colour.b * level));
  }
}

// PULSE_FLOOR -> 1 -> PULSE_FLOOR once per period, starting dim.
float pulse(uint32_t t_ms, uint32_t period_ms) {
  const float phase = (float)(t_ms % period_ms) / (float)period_ms;
  const float wave = 0.5f - 0.5f * cosf(phase * 6.2831853f);
  return PULSE_FLOOR + (1.0f - PULSE_FLOOR) * wave;
}

// ---- the five approval patterns and the battery blink (ui.md, table under "LED patterns") ------

// Slow green pulse, 1.5 s period. Ends when the approval closes (the engine calls stop()).
bool approveGreen(uint32_t t_ms) {
  fill(SEVERITY_GREEN, pulse(t_ms, 1500));
  return true;
}

// Amber pulse, 1 s period. Ends when the approval closes.
bool approveAmber(uint32_t t_ms) {
  fill(SEVERITY_AMBER, pulse(t_ms, 1000));
  return true;
}

// Solid red. Ends when the approval closes.
bool approveRed(uint32_t t_ms) {
  (void)t_ms;
  fill(SEVERITY_RED, 1.0f);
  return true;
}

// Three quick green flashes in 600 ms: 100 ms on, 100 ms off.
bool signedFlashes(uint32_t t_ms) {
  fill(SEVERITY_GREEN, (t_ms % 200) < 100 ? 1.0f : 0.0f);
  return t_ms < 600;
}

// One red blink: on for 250 ms, done at 400 ms.
bool refusedBlink(uint32_t t_ms) {
  fill(SEVERITY_RED, t_ms < 250 ? 1.0f : 0.0f);
  return t_ms < 400;
}

// Battery low or critical (battery.cpp), once when the level is entered: two short red blinks at half
// strength, 150 ms on, 150 ms off. Unlike `refused` (one long blink) it is not an answer to a key.
bool lowBattery(uint32_t t_ms) {
  fill(SEVERITY_RED, (t_ms < 450 && (t_ms % 300) < 150) ? 0.5f : 0.0f);
  return t_ms < 600;
}

VK_LED_PATTERN(low_battery, "low_battery", lowBattery);
VK_LED_PATTERN(approve_green, "approve_green", approveGreen);
VK_LED_PATTERN(approve_amber, "approve_amber", approveAmber);
VK_LED_PATTERN(approve_red, "approve_red", approveRed);
VK_LED_PATTERN(signed, "signed", signedFlashes);
VK_LED_PATTERN(refused, "refused", refusedBlink);

// ---- playback ---------------------------------------------------------------------------------

const LedPattern *find(const char *name) {
  if (name == nullptr) return nullptr;
  for (const LedPattern *pattern = Registered<LedPattern>::first(); pattern;
       pattern = pattern->Registered<LedPattern>::next()) {
    if (pattern->name != nullptr && strcmp(pattern->name, name) == 0) return pattern;
  }
  return nullptr;
}

// A pattern is over: LEDs off, and the idle animation back if the badge is idle. While an app
// runs and no pattern is playing, the LEDs belong to the app.
void release() {
  ::leds::stopAnimation();
  ::leds::off();
  if (vk::host::idle()) ::leds::playIdle();
}

void serviceUpdate() {
  if (sCurrent == nullptr) return;
  const uint32_t now = millis();
  if (!sFrameDue && (uint32_t)(now - sLastFrameAt) < FRAME_MS) return;
  sFrameDue = false;
  sLastFrameAt = now;

  const LedPattern *const pattern = sCurrent;
  ::leds::stopAnimation();
  const bool more = pattern->frame != nullptr && pattern->frame((uint32_t)(now - sStartedAt));
  if (sCurrent != pattern) return;   // the frame function itself started or stopped a pattern
  if (more) {
    ::leds::show();
    return;
  }
  sCurrent = nullptr;
  release();
}

// The boot bar (ui.md, "Boot bar"): each LED owns an equal share of the boot; it is off before its
// share, flashing during it and solid after it.
void drawBootBar() {
  ::leds::stopAnimation();

  const uint16_t accent = vk::ui::theme::color(vk::ui::theme::LED);   // RGB565
  const uint8_t r = (uint8_t)((((accent >> 11) & 0x1F) * 255) / 31);
  const uint8_t g = (uint8_t)((((accent >> 5) & 0x3F) * 255) / 63);
  const uint8_t b = (uint8_t)(((accent & 0x1F) * 255) / 31);

  const uint32_t share = 100 / RGB_LED_COUNT;
  const bool blink = (millis() / BOOT_BLINK_MS) % 2 == 0;
  for (uint8_t i = 0; i < RGB_LED_COUNT; ++i) {
    bool on;
    if (sBootPercent >= 100 || sBootPercent >= (i + 1) * share) {
      on = true;
    } else if (sBootPercent >= i * share) {
      on = blink;
    } else {
      on = false;
    }
    if (on) {
      ::leds::set(i, r, g, b);
    } else {
      ::leds::set(i, 0, 0, 0);
    }
  }
  ::leds::show();
}

// The result of every approval: `signed` when approved (with or without a signature), `refused`
// for cancelled, timed out, blocked and failed.
void onApproval(const vk::wallet::ApprovalOutcome &outcome) {
  play(outcome.approved ? "signed" : "refused");
}

}  // namespace

void play(const char *name) {
  const LedPattern *pattern = find(name);
  if (pattern == nullptr) {
    badge_log::tagf("vk", "led pattern '%s' is not registered", name ? name : "(null)");
    return;
  }
  sCurrent = pattern;
  sStartedAt = millis();
  sFrameDue = true;
}

void stop() {
  if (sCurrent == nullptr) return;   // nothing of ours is playing: the LEDs are not ours to clear
  sCurrent = nullptr;
  release();
}

bool playing() { return sCurrent != nullptr; }

// Hook H15. Boot stages block and no service runs before the main loop, so the bar is drawn here,
// once per stage: a long stage holds one flash state. After the last stage upstream starts its
// idle animation, which takes the LEDs over.
void bootProgress(uint8_t percent) {
  sBootPercent = percent;
  drawBootBar();
}

VK_SERVICE(leds, nullptr, serviceUpdate);
VK_ON_APPROVAL(leds, onApproval);

void pulseTheme(uint16_t ms) {
  const uint16_t c = vk::ui::theme::color(vk::ui::theme::LED);   // RGB565, from the active theme
  const uint8_t r = (uint8_t)((((c >> 11) & 0x1F) * 255) / 31);
  const uint8_t g = (uint8_t)((((c >> 5) & 0x3F) * 255) / 63);
  const uint8_t b = (uint8_t)(((c & 0x1F) * 255) / 31);
  ::leds::pulse(r, g, b, ms);
}

}  // namespace vk::ui::leds
