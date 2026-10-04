// src/native_apps/selftest/ui.cpp
#include "ui.h"

#include <stdio.h>

#include "../../hal/display.h"
#include "../../vk/ui/receipt.h"
#include "../../vk/ui/theme.h"

namespace selftest::ui {

namespace rc = vk::ui::receipt;
namespace th = vk::ui::theme;            // ours; upstream's palette is ::theme

void frame(const char *title) {
  char right[32];
  rc::statusRight(right, sizeof right);
  rc::page();
  rc::header("BADGEOS", right);
  rc::title(title, TITLE_Y);
}

size_t windowStart(size_t cursor, size_t count) {
  if (count <= VISIBLE_ROWS) return 0;
  const size_t half = VISIBLE_ROWS / 2;
  size_t first = cursor > half ? cursor - half : 0;
  if (first > count - VISIBLE_ROWS) first = count - VISIBLE_ROWS;
  return first;
}

void scrollMark(size_t cursor, size_t count) {
  if (count <= VISIBLE_ROWS) return;
  char mark[12];
  snprintf(mark, sizeof mark, "%u/%u", (unsigned)(cursor + 1), (unsigned)count);
  display::textRight(mark, X1, 28, th::color(th::FAINT));
}

uint16_t stateColor(State state) {
  switch (state) {
    case State::Ok:   return th::color(th::STAMP_OK);
    case State::Fail: return th::color(th::STAMP_BAD);
    case State::Skip: return th::color(th::SUB);
    default:          return th::color(th::FAINT);
  }
}

uint16_t sub() { return th::color(th::SUB); }

void resultRow(int y, const char *label, const char *value, State state, bool selected) {
  char text[64];
  snprintf(text, sizeof text, "%s%s%s", value, value[0] ? "  " : "", stateText(state));
  // A selected row is inverted: its value stays in the paper colour, which is readable on ink.
  rc::row(X0, X1, y, label, text, selected, selected ? (uint16_t)0 : stateColor(state));
}

void countsText(char *out, size_t cap, const Counts &c) {
  snprintf(out, cap, "%u OK \xC2\xB7 %u FAIL \xC2\xB7 %u --", c.ok, c.fail, c.untested);
}

}  // namespace selftest::ui
