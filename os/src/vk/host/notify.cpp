// Notifications (app-host.md, "Notifications"): a small inbox for things that happen while the
// relevant app is not open. Eight notes, in RAM, newest first; the oldest is dropped when a ninth
// arrives and nothing is persisted. Firmware features post; apps cannot.
//
// Shown by the shell (the launcher's inbox cell and the Settings list's Inbox row show the count),
// by the LED pattern `notify` while a note waits and the badge is idle, and by the native app Inbox
// (src/native_apps/inbox/).
#include "notify.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "../../config.h"        // RGB_LED_COUNT
#include "../../hal/display.h"
#include "../../hal/leds.h"
#include "../core/service.h"
#include "../ui/leds.h"
#include "../ui/repaint.h"
#include "../ui/theme.h"         // vk::ui::theme: the LED colour of the active theme
#include "../vk.h"               // vk::modalActive
#include "home.h"                // vk::host::idle

namespace vk::host::notify {

namespace {

constexpr size_t MAX_NOTES = 8;
constexpr uint32_t DUPLICATE_MS = 10000;   // an identical title and body within this time is ignored

Note sNotes[MAX_NOTES];                    // [0] is the newest
size_t sCount = 0;

// The last post that was accepted. It outlives its note, so dismissing a note does not let the
// same text straight back in.
Note sLastPost = {};
bool sHaveLastPost = false;

bool sameText(const Note &note, const Note &other) {
  return strcmp(note.title, other.title) == 0 && strcmp(note.body, other.body) == 0;
}

bool isRepeat(const Note &note) {
  if (sHaveLastPost && sameText(sLastPost, note) && (uint32_t)(note.at_ms - sLastPost.at_ms) < DUPLICATE_MS) {
    return true;
  }
  for (size_t i = 0; i < sCount; ++i) {
    if (sameText(sNotes[i], note) && (uint32_t)(note.at_ms - sNotes[i].at_ms) < DUPLICATE_MS) return true;
  }
  return false;
}

// The shell shows the count (launcher, Settings list) and redraws only when asked.
void countChanged() { vk::ui::requestShellRepaint(); }

// ---- LED pattern `notify` (ui.md, "LED patterns"): dim breathe in the theme's LED colour, 3 s ----

constexpr uint32_t BREATHE_MS = 3000;
constexpr float BREATHE_FLOOR = 0.03f;     // never quite dark, so it reads as a breath and not a blink
constexpr float BREATHE_PEAK = 0.30f;      // dim: it waits on a lanyard for as long as the note does

// A note is waiting, the badge is idle and the approval (which has LED patterns of its own) is closed.
bool wanted() { return sCount > 0 && vk::host::idle() && !vk::modalActive(); }

// The pattern ends itself when the inbox is empty or an app starts. The LED service then turns the
// LEDs off and, if the badge is idle, gives them back to upstream's idle animation. Ending here,
// and not with leds::stop() from the service below, means a pattern that replaced this one (an
// approval's) is never stopped by mistake.
bool breathe(uint32_t t_ms) {
  if (!wanted()) return false;
  const float phase = (float)(t_ms % BREATHE_MS) / (float)BREATHE_MS;
  const float wave = 0.5f - 0.5f * cosf(phase * 6.2831853f);
  const float level = BREATHE_FLOOR + (BREATHE_PEAK - BREATHE_FLOOR) * wave;
  const uint16_t led = vk::ui::theme::color(vk::ui::theme::LED);   // RGB565, from the active theme
  const uint8_t r = (uint8_t)(((((led >> 11) & 0x1F) * 255) / 31) * level);
  const uint8_t g = (uint8_t)(((((led >> 5) & 0x3F) * 255) / 63) * level);
  const uint8_t b = (uint8_t)((((led & 0x1F) * 255) / 31) * level);
  for (uint8_t i = 0; i < RGB_LED_COUNT; ++i) ::leds::set(i, r, g, b);
  return true;
}

VK_LED_PATTERN(notify, "notify", breathe);

// Starts the pattern whenever it is wanted and the LEDs are free. While another pattern plays (the
// flashes after an approval) this waits for it to finish.
void serviceUpdate() {
  if (wanted() && !vk::ui::leds::playing()) vk::ui::leds::play("notify");
}

VK_SERVICE(notify, nullptr, serviceUpdate);

}  // namespace

void post(const char *title, const char *body, const char *app_id) {
  Note note = {};
  strlcpy(note.title, title != nullptr ? title : "", sizeof note.title);
  strlcpy(note.body, body != nullptr ? body : "", sizeof note.body);
  strlcpy(note.app_id, app_id != nullptr ? app_id : "", sizeof note.app_id);
  note.at_ms = (uint32_t)millis();
  if (note.title[0] == '\0' && note.body[0] == '\0') return;   // nothing to show
  if (isRepeat(note)) return;

  // Newest first. With eight stored, the oldest falls off the end.
  const size_t keep = sCount < MAX_NOTES ? sCount : MAX_NOTES - 1;
  for (size_t i = keep; i > 0; --i) sNotes[i] = sNotes[i - 1];
  sNotes[0] = note;
  const size_t before = sCount;
  sCount = keep + 1;

  sLastPost = note;
  sHaveLastPost = true;
  if (sCount != before) countChanged();
}

size_t count() { return sCount; }

const Note *at(size_t index) { return index < sCount ? &sNotes[index] : nullptr; }

void remove(size_t index) {
  if (index >= sCount) return;
  for (size_t i = index; i + 1 < sCount; ++i) sNotes[i] = sNotes[i + 1];
  --sCount;
  countChanged();
}

void clear() {
  if (sCount == 0) return;
  sCount = 0;
  countChanged();
}

}  // namespace vk::host::notify
