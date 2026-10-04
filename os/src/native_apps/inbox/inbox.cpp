// src/native_apps/inbox/inbox.cpp
// The Inbox (apps.md, "Inbox"): the notes of vk::host::notify, newest first, as a Receipt list
// screen. SELECT launches the note's app and removes the note; RIGHT dismisses; CANCEL exits.
#include <stdio.h>
#include <string.h>

#include "../../vk/sdk/badge_sdk.hpp"

#include "../../apps/app_store.h"
#include "../../lua_sdk/lua_runtime.h"
#include "../../vk/ui/receipt.h"

namespace {

namespace notify = vk::host::notify;
namespace receipt = vk::ui::receipt;

// The list layout of ui.md ("Screens", any list): header, title, five rows with sublines, footer.
constexpr int X0 = 10, X1 = 310;         // the page margins
constexpr int TITLE_Y = 26;
constexpr int FIRST_ROW_Y = 46;          // a row at y owns y-5 .. y+12
constexpr int SUBLINE_DY = 13;           // its subline owns the 13 px below that
constexpr int ROW_PITCH = 31;            // row + subline
constexpr size_t VISIBLE_ROWS = 5;
constexpr int EMPTY_Y = 112;
constexpr uint32_t REDRAW_MS = 250;      // the ages and the clock in the header move on their own

// "now", then minutes, hours, days.
void ageText(uint32_t age_ms, char *out, size_t cap) {
  const uint32_t seconds = age_ms / 1000;
  if (seconds < 60) snprintf(out, cap, "now");
  else if (seconds < 3600) snprintf(out, cap, "%um", (unsigned)(seconds / 60));
  else if (seconds < 86400) snprintf(out, cap, "%uh", (unsigned)(seconds / 3600));
  else snprintf(out, cap, "%ud", (unsigned)(seconds / 86400));
}

void upperCase(char *text) {
  for (; *text != '\0'; ++text) {
    if (*text >= 'a' && *text <= 'z') *text = (char)(*text - 'a' + 'A');
  }
}

class Inbox final : public badge::App {
 public:
  void on_start() override { dirty_ = true; }

  void on_draw() override {
    // A note can arrive or be removed at any time, the approval can have drawn over this screen,
    // and the theme can change: redraw on every change made here, and four times a second besides.
    const uint32_t now = (uint32_t)millis();
    if (!dirty_ && (uint32_t)(now - drawnAt_) < REDRAW_MS) return;
    dirty_ = false;
    drawnAt_ = now;
    draw(now);
  }

  void on_button(uint8_t key, bool pressed) override {
    if (!pressed) return;
    dirty_ = true;
    const size_t count = notify::count();
    clampSelection(count);
    switch (key) {
      case BTN_UP:
        if (selected_ > 0) --selected_;
        break;
      case BTN_DOWN:
        if (selected_ + 1 < count) ++selected_;
        break;
      case BTN_A:
        open();
        break;
      case BTN_RIGHT:
        if (count > 0) notify::remove(selected_);
        clampSelection(notify::count());
        break;
      case BTN_B:
        badge::exit();     // CANCEL
        break;
      default:
        break;
    }
  }

 private:
  void clampSelection(size_t count) {
    if (count == 0) selected_ = 0;
    else if (selected_ >= count) selected_ = count - 1;
  }

  // SELECT: the note goes, and its app (if it names one that is installed) is launched.
  void open() {
    const notify::Note *note = notify::at(selected_);
    if (note == nullptr) return;
    char appId[sizeof note->app_id];
    strlcpy(appId, note->app_id, sizeof appId);
    notify::remove(selected_);
    clampSelection(notify::count());
    if (appId[0] != '\0' && app_store::exists(appId)) runtime::requestLaunch(appId);
  }

  void draw(uint32_t now) {
    char right[32];
    receipt::statusRight(right, sizeof right);
    receipt::page();
    receipt::header("BADGE OS", right);
    receipt::title("INBOX", TITLE_Y);

    const size_t count = notify::count();
    clampSelection(count);
    if (count == 0) {
      display::textCentered("Nothing new", display::width() / 2, EMPTY_Y,
                            vk::ui::theme::color(vk::ui::theme::SUB));
      receipt::footer("", "CANCEL back");
      return;
    }

    // The window of rows that holds the selection, as in the simulation: centred where it can be.
    size_t first = 0;
    if (count > VISIBLE_ROWS) {
      const size_t half = VISIBLE_ROWS / 2;
      first = selected_ > half ? selected_ - half : 0;
      if (first > count - VISIBLE_ROWS) first = count - VISIBLE_ROWS;
    }

    for (size_t slot = 0; slot < VISIBLE_ROWS && first + slot < count; ++slot) {
      const size_t index = first + slot;
      const notify::Note *note = notify::at(index);
      if (note == nullptr) break;
      char label[sizeof note->title];
      strlcpy(label, note->title, sizeof label);
      upperCase(label);
      char age[12];
      ageText((uint32_t)(now - note->at_ms), age, sizeof age);
      const int y = FIRST_ROW_Y + (int)slot * ROW_PITCH;
      const bool selected = index == selected_;
      receipt::row(X0, X1, y, label, age, selected);
      receipt::subline(X0, X1, y + SUBLINE_DY, note->body, selected);
    }

    receipt::footer("SELECT open \xC2\xB7 RIGHT dismiss", "CANCEL back");
  }

  size_t selected_ = 0;
  bool dirty_ = true;
  uint32_t drawnAt_ = 0;
};

}  // namespace

BADGE_APP(Inbox, "inbox", "Inbox", "1.0.0", "");
