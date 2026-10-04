// src/native_apps/selftest/selftest.cpp
// TESTS: the badge's one test suite app (docs/os/apps/apps.md, "Self test"). A menu of suites,
// each a checklist of checks that end OK, FAIL or -- (not tested: it needs a person, or the thing
// is off or absent), and RUN ALL. The screens are Receipt lists (ui/shell.md, "Any list"); the same
// results go to the serial log, so a person can read them and a script can (t_selftest.py).
//
// Files of this app:
//   selftest.cpp        the app: menu, checklists, RUN ALL, keys, drawing, the 5 s summary line
//   selftest.h, core.cpp  the table types and the runner (plain C++; the host simulation runs it)
//   suites.h, suites.cpp  the list of suites, in menu order
//   suite_<name>.cpp    one suite each: its table of checks and their functions
//   checks_cases.*, checks_vectors.h  the CHECKS suite's vectors (plain C++ as well)
//   shared.*, ui.*      what several suites share: the storage probe, RFC 8032 vectors, list drawing
//
// Screens and keys:
//   menu        UP/DOWN move, SELECT opens a suite (its automatic checks run the first time) or
//               starts RUN ALL, CANCEL exits the app
//   checklist   UP/DOWN move, SELECT on a manual row starts that test, SELECT on an automatic row
//               runs the suite's automatic checks again, CANCEL back to the menu
//   RUN ALL     every automatic check of every suite, in menu order, then the totals; SELECT on a
//               suite opens its checklist, CANCEL back to the menu
//   a manual test  its own keys (see its suite); CANCEL always ends it
// Holding CANCEL for 1.5 s leaves the app from anywhere (the firmware's own force-quit). Every
// automatic check has a hard timeout in the runner, every manual test its own.
//
// HARDWARE runs by itself when the app opens, as the first Self test did.
//
// Serial log, each line "[selftest] ...":
//   start                                    once, when the app opens
//   <name>=<OK|FAIL|--> <value>              HARDWARE, one per check (a manual check is reported as
//   done ok=<n> fail=<n> manual=<n>          "--" when the suite is first run, and again when a
//                                            person has done it); done after the automatic run and
//                                            again after each manual test
//   <suite>.<name>=<OK|FAIL|--> <value>      every other suite, the same way
//   suite <suite> done ok=<n> fail=<n> manual=<n>
//   all done ok=<n> fail=<n> manual=<n>      at the end of RUN ALL, over every suite
//   batt=4.85V mic=-57dB btn=0x00 i2c=ok heap=182344       every 5 s while the app is open
// "manual" counts every row that is not OK or FAIL. Nothing secret is ever logged: no private key,
// no Wi-Fi password.
#include "../../vk/sdk/badge_sdk.hpp"

#include <stdio.h>
#include <stdlib.h>

#include "../../badge_log.h"
#include "../../hal/badge_i2c.h"
#include "../../hal/mic.h"
#include "../../hal/power.h"
#include "../../vk/ui/receipt.h"
#include "../../vk/ui/screen_power.h"
#include "../../vk/ui/theme.h"
#include "../../vk/vk_build.h"          // VK_PROFILE_DEV
#include "selftest.h"
#include "ui.h"

namespace {

using namespace selftest;
namespace rc = vk::ui::receipt;
namespace th = vk::ui::theme;           // ours; upstream's palette is ::theme

constexpr float PAUSE_GAP_S = 0.25f;     // a frame this late means an approval was up over the app
constexpr float REFRESH_S = 1.0f;        // the header's clock, countdowns, running rows
constexpr uint32_t BEAT_MS = 5000;       // the one-line summary in the log
constexpr size_t HARDWARE_SUITE = 0;     // the first suite of SUITES: run when the app opens

void logLine(const char *line) { badge_log::tagf(TAG, "%s", line); }

// The rows live in PSRAM when there is some (a few kilobytes; native-apps.md, rule 5).
void *allocRows(size_t bytes) {
  void *memory = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM);
  return memory ? memory : malloc(bytes);
}
void freeRows(void *memory) { free(memory); }

class SelfTest final : public badge::App {
 public:
  void on_start() override {
    badge_log::tagf(TAG, "start");
    vk::ui::screen::keepAwake(true);        // a person is watching the checks; cleared when the app stops
    if (!runner_.begin(SUITES, SUITE_COUNT, VK_PROFILE_DEV != 0, logLine, allocRows, freeRows)) {
      badge_log::tagf(TAG, "no memory for the rows");
      badge::exit();
      return;
    }
    runner_.queue(HARDWARE_SUITE);
    beatAt_ = (uint32_t)millis();
  }

  void on_stop() override { runner_.end(); }

  void on_update(float dt) override {
    const uint32_t now = (uint32_t)millis();
    sinceDraw_ += dt;
    // While an approval is up the app is not run at all: the next frame comes late and finds the
    // approval's picture in the canvas.
    if (dt > PAUSE_GAP_S) dirty_ = true;

    runner_.step(now);
    runner_.manualUpdate(now);
    afterManual();
    if (runner_.takeDirty()) dirty_ = true;

    if ((uint32_t)(now - beatAt_) >= BEAT_MS) {
      beatAt_ = now;
      heartbeat();
    }
    // The clock, countdowns and running rows move by themselves. A display test screen does not.
    const Check *manual = runner_.manualCheck();
    const bool still = manual != nullptr && manual->manual->fullScreen;
    if (!still && sinceDraw_ >= REFRESH_S) dirty_ = true;
  }

  void on_draw() override {
    if (!dirty_) return;
    dirty_ = false;
    sinceDraw_ = 0.0f;
    if (runner_.manualActive()) {
      runner_.manualDraw();
      return;
    }
    switch (screen_) {
      case Screen::Menu:      drawMenu(); break;
      case Screen::Checklist: drawChecklist(); break;
      case Screen::RunAll:    drawRunAll(); break;
    }
  }

  void on_button(uint8_t key, bool pressed) override {
    if (!pressed) return;
    dirty_ = true;
    const uint32_t now = (uint32_t)millis();
    if (runner_.manualActive()) {
      runner_.manualKey(key, now);
      afterManual();
      return;
    }
    switch (screen_) {
      case Screen::Menu:      menuKey(key); break;
      case Screen::Checklist: checklistKey(key, now); break;
      case Screen::RunAll:    runAllKey(key); break;
    }
  }

 private:
  enum class Screen : uint8_t { Menu, Checklist, RunAll };

  // A manual test ended: back to its checklist, and on to the next manual test if the row below
  // is one (as the first Self test did after each of its three).
  void afterManual() {
    size_t s, r;
    State state;
    if (!runner_.takeManualEnd(s, r, state)) return;
    screen_ = Screen::Checklist;
    suite_ = s;
    dirty_ = true;
    if (state != State::Skip && cursor_ == r && r + 1 < runner_.rowCount(s) &&
        runner_.row(s, r + 1).check->kind == Kind::Manual) {
      cursor_ = r + 1;
    }
  }

  void heartbeat() const {
    char level[12];
    if (mic::enabled()) {
      const float left = mic::dbLeft(), right = mic::dbRight();
      snprintf(level, sizeof level, "%.0fdB", (double)(left > right ? left : right));
    } else {
      snprintf(level, sizeof level, "--");
    }
    badge_log::tagf(TAG, "batt=%.2fV mic=%s btn=0x%02X i2c=%s heap=%lu", (double)power::volts(), level,
                    (unsigned)buttons::downMask(), badge_i2c::linesHigh() ? "ok" : "LOW",
                    (unsigned long)ESP.getFreeHeap());
  }

  // ---- what a suite's row on the menu says ------------------------------------------------------
  void suiteSummary(size_t s, char *value, size_t cap, uint16_t &color) const {
    color = th::color(th::SUB);
    if (runner_.running(s)) {
      snprintf(value, cap, "running");
      return;
    }
    if (runner_.queued(s)) {
      snprintf(value, cap, "waiting");
      return;
    }
    if (!runner_.ran(s)) {
      snprintf(value, cap, "not run");
      return;
    }
    const Counts c = runner_.counts(s);
    ui::countsText(value, cap, c);
    color = c.fail ? th::color(th::STAMP_BAD) : c.ok ? th::color(th::STAMP_OK) : th::color(th::SUB);
  }

  bool allRan() const {
    for (size_t s = 0; s < runner_.suiteCount(); ++s) {
      if (!runner_.ran(s)) return false;
    }
    return true;
  }

  // ---- menu ---------------------------------------------------------------------------------------
  size_t menuRows() const { return runner_.suiteCount() + 1; }    // the suites, then RUN ALL

  void drawMenu() const {
    ui::frame("TESTS");
    const size_t count = menuRows();
    const size_t first = ui::windowStart(menu_, count);
    for (size_t slot = 0; slot < ui::VISIBLE_ROWS && first + slot < count; ++slot) {
      const size_t index = first + slot;
      const bool selected = index == menu_;
      const int y = ui::FIRST_ROW_Y + (int)slot * ui::ROW_PITCH;
      char value[48];
      uint16_t color;
      const char *label;
      if (index < runner_.suiteCount()) {
        label = runner_.suite(index).title;
        suiteSummary(index, value, sizeof value, color);
      } else {
        label = "RUN ALL";
        color = th::color(th::SUB);
        if (runner_.runAllActive()) {
          snprintf(value, sizeof value, "running");
        } else if (allRan()) {
          const Counts c = runner_.countsAll();
          ui::countsText(value, sizeof value, c);
          color = c.fail ? th::color(th::STAMP_BAD) : th::color(th::STAMP_OK);
        } else {
          value[0] = '\0';
        }
      }
      rc::row(ui::X0, ui::X1, y, label, value, selected, selected ? (uint16_t)0 : color);
    }
    ui::scrollMark(menu_, count);
    rc::footer("SELECT open", "CANCEL exit");
  }

  void menuKey(uint8_t key) {
    switch (key) {
      case BTN_B:                                  // CANCEL always leads out
        badge::exit();
        break;
      case BTN_UP:
        if (menu_ > 0) --menu_;
        break;
      case BTN_DOWN:
        if (menu_ + 1 < menuRows()) ++menu_;
        break;
      case BTN_A:
        if (menu_ < runner_.suiteCount()) {
          openSuite(menu_);
        } else {
          if (!runner_.runAllActive()) runner_.queueAll();
          screen_ = Screen::RunAll;
          runAllCursor_ = 0;
        }
        break;
      default:
        break;
    }
  }

  void openSuite(size_t s) {
    if (!runner_.ran(s) && !runner_.queued(s) && !runner_.running(s)) runner_.queue(s);
    if (s != suite_) cursor_ = 0;
    suite_ = s;
    screen_ = Screen::Checklist;
  }

  // ---- a suite's checklist ----------------------------------------------------------------------
  void drawChecklist() const {
    const size_t s = suite_, count = runner_.rowCount(s);
    ui::frame(runner_.suite(s).title);
    const size_t first = ui::windowStart(cursor_, count);
    for (size_t slot = 0; slot < ui::VISIBLE_ROWS && first + slot < count; ++slot) {
      const size_t index = first + slot;
      const Row &row = runner_.row(s, index);
      ui::resultRow(ui::FIRST_ROW_Y + (int)slot * ui::ROW_PITCH, row.label, row.value, row.state, index == cursor_);
    }
    ui::scrollMark(cursor_, count);
    if (count == 0) display::textCentered("Nothing to check.", display::width() / 2, ui::NOTE_Y, ui::sub());

    char left[64];
    ui::countsText(left, sizeof left, runner_.counts(s));
    if (cursor_ < count) {
      const bool manual = runner_.row(s, cursor_).check->kind == Kind::Manual;
      strlcat(left, manual ? "  SELECT test" : runner_.running(s) || runner_.queued(s) ? "" : "  SELECT rerun", sizeof left);
    }
    rc::footer(left, "CANCEL back");
  }

  void checklistKey(uint8_t key, uint32_t now) {
    const size_t count = runner_.rowCount(suite_);
    switch (key) {
      case BTN_B:
        screen_ = Screen::Menu;
        break;
      case BTN_UP:
        if (cursor_ > 0) --cursor_;
        break;
      case BTN_DOWN:
        if (cursor_ + 1 < count) ++cursor_;
        break;
      case BTN_A:
        if (cursor_ >= count) break;
        if (runner_.row(suite_, cursor_).check->kind == Kind::Manual) {
          runner_.manualStart(suite_, cursor_, now);
          afterManual();                           // a test with nothing to do ends at once
        } else {
          runner_.queue(suite_);                   // ignored while it is already waiting or running
        }
        break;
      default:
        break;
    }
  }

  // ---- RUN ALL ------------------------------------------------------------------------------------
  void drawRunAll() const {
    ui::frame("RUN ALL");
    const size_t count = runner_.suiteCount();
    for (size_t s = 0; s < count && s < ui::VISIBLE_ROWS - 1; ++s) {
      char value[48];
      uint16_t color;
      suiteSummary(s, value, sizeof value, color);
      const bool selected = s == runAllCursor_;
      rc::row(ui::X0, ui::X1, ui::FIRST_ROW_Y + (int)s * ui::ROW_PITCH, runner_.suite(s).title, value, selected,
              selected ? (uint16_t)0 : color);
    }
    char total[48];
    ui::countsText(total, sizeof total, runner_.countsAll());
    rc::footer(runner_.runAllActive() ? "running..." : total, "CANCEL back");
  }

  void runAllKey(uint8_t key) {
    const size_t count = runner_.suiteCount();
    switch (key) {
      case BTN_B:
        screen_ = Screen::Menu;
        break;
      case BTN_UP:
        if (runAllCursor_ > 0) --runAllCursor_;
        break;
      case BTN_DOWN:
        if (runAllCursor_ + 1 < count) ++runAllCursor_;
        break;
      case BTN_A:
        if (runAllCursor_ < count) openSuite(runAllCursor_);
        break;
      default:
        break;
    }
  }

  // ---- state ------------------------------------------------------------------------------------
  Runner runner_;
  Screen screen_ = Screen::Menu;
  size_t menu_ = 0;                          // the menu's cursor
  size_t suite_ = 0;                         // the checklist shown
  size_t cursor_ = 0;                        // the checklist's cursor
  size_t runAllCursor_ = 0;
  bool dirty_ = true;
  float sinceDraw_ = 0.0f;
  uint32_t beatAt_ = 0;
};

}  // namespace

BADGE_APP(SelfTest, "selftest", "Self test", "1.0.0", "");
