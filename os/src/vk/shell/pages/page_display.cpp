// Settings page `display` (shell.md, "Display"): the backlight level, applied live, and how long the
// screen stays lit with nothing happening (config key `sleep_s`, ui.md "Screen dim and sleep").

#include "../page.h"

#include "../../../settings.h"
#include "../../core/config.h"

namespace {
using namespace vk::shell;
namespace receipt = vk::ui::receipt;
namespace tk = vk::ui::theme;

constexpr int kStep = 8;
constexpr int kFloor = 8;   // a dark screen looks exactly like a crash

constexpr int ROWS = 2;     // Backlight, Sleep after
constexpr int ROW_BACKLIGHT = 0;
constexpr int ROW_SLEEP = 1;

// The choices LEFT/RIGHT step through, shortest first; 0 (never) is the longest. Any value of the key
// can be set over USB (VKSET sleep_s); a value between two choices steps to the neighbour.
constexpr uint32_t kSleepChoices[] = {30, 60, 120, 300, 600, 0};
constexpr int kSleepChoiceCount = sizeof kSleepChoices / sizeof kSleepChoices[0];

List sList;

// 0 sorts last.
uint32_t rank(uint32_t seconds) { return seconds == 0 ? UINT32_MAX : seconds; }

void durationText(uint32_t seconds, char *out, size_t cap) {
  if (seconds == 0) {
    snprintf(out, cap, "never");
  } else if (seconds % 60 == 0) {
    snprintf(out, cap, "%u min", (unsigned)(seconds / 60));
  } else {
    snprintf(out, cap, "%u s", (unsigned)seconds);
  }
}

void pageValue(char *out, size_t cap) { snprintf(out, cap, "%d%%", (settings::brightness() * 100) / 255); }

void pageEnter() { sList = List(); }

void adjustBacklight(int direction) {
  const int before = settings::brightness();
  const int after = constrain(before + direction * kStep, kFloor, 255);
  if (after == before) return;   // at a limit: nothing changes and nothing is written
  // Applied live, so the value being chosen is the value being seen.
  settings::setBrightness((uint8_t)after);
  display::setBrightness((uint8_t)after);
  repaint();
}

void adjustSleep(int direction) {
  const uint32_t now = vk::config::u32("sleep_s");
  int next = -1;
  if (direction > 0) {
    for (int i = 0; i < kSleepChoiceCount && next < 0; ++i) {
      if (rank(kSleepChoices[i]) > rank(now)) next = i;
    }
  } else {
    for (int i = kSleepChoiceCount - 1; i >= 0 && next < 0; --i) {
      if (rank(kSleepChoices[i]) < rank(now)) next = i;
    }
  }
  if (next < 0) return;          // at a limit
  char text[12];
  snprintf(text, sizeof text, "%u", (unsigned)kSleepChoices[next]);
  vk::config::set("sleep_s", text);   // not a secure key: written at once
  repaint();
}

void pageUpdate() {
  if (back()) return;  // CANCEL
  listMove(sList, ROWS, ROWS);
  int direction = 0;
  if (sList.cursor == ROW_BACKLIGHT) {
    if (buttons::repeated(BTN_LEFT)) direction -= 1;
    if (buttons::repeated(BTN_RIGHT)) direction += 1;
    if (direction != 0) adjustBacklight(direction);
  } else {
    if (buttons::pressed(BTN_LEFT)) direction -= 1;
    if (buttons::pressed(BTN_RIGHT)) direction += 1;
    if (direction != 0) adjustSleep(direction);
  }
}

void pageDraw() {
  frame("DISPLAY", "UP/DOWN choose  LEFT/RIGHT adjust");
  char percent[8];
  snprintf(percent, sizeof percent, "%d%%", (settings::brightness() * 100) / 255);
  const uint32_t sleepS = vk::config::u32("sleep_s");
  const uint32_t dimS = vk::config::u32("dim_s");
  char sleepText[12];
  durationText(sleepS, sleepText, sizeof sleepText);
  const ListRow rows[ROWS] = {
      {"Backlight", percent, 0},
      {"Sleep after", sleepText, 0},
  };
  listDraw(sList, rows, ROWS, LIST_Y, ROWS);
  bar(41, 90, 30, settings::brightness(), 255);

  // What the timers do, in words: the dim time is only a USB setting (VKSET dim_s).
  char line[60];
  const bool dims = dimS != 0 && (sleepS == 0 || dimS < sleepS);
  char dimText[12];
  durationText(dimS, dimText, sizeof dimText);
  if (dims && sleepS != 0) {
    snprintf(line, sizeof line, "Idle: dims after %s, off after %s.", dimText, sleepText);
  } else if (dims) {
    snprintf(line, sizeof line, "Idle: dims after %s, never turns off.", dimText);
  } else if (sleepS != 0) {
    snprintf(line, sizeof line, "Idle: turns off after %s.", sleepText);
  } else {
    snprintf(line, sizeof line, "Idle: stays on.");
  }
  text(X0, 112, line, tk::SUB);
  text(X0, 126, "Any key wakes it; that key does nothing else.", tk::SUB);
  if (vk::config::u32("awake_usb") != 0) text(X0, 140, "Stays on while on USB power.", tk::SUB);
}
}  // namespace

VK_SETTINGS_PAGE(display, "display", 80, "Display", pageValue, pageEnter, pageUpdate, pageDraw, 0);
