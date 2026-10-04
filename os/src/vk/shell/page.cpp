// The helpers every shell screen and settings page draws with (docs/os/ui/shell.md, "page.h").
// They use only the receipt kit, theme colours and the canvas, so the boot screen may call the
// drawing ones before vk::begin().
#include "page.h"

#include <stdio.h>
#include <string.h>

#include "../../hal/leds.h"
#include "../ui/leds.h"

namespace vk::shell {

namespace {

constexpr int CHAR_W = 6;                    // one Font0 column
constexpr int CELL_W = 6, CELL_H = 8, CELL_GAP = 2;

// Copies `s` as printable ASCII (anything else becomes '?'), cut to maxCols with "..".
// Returns the number of characters written.
int ascii(char *out, size_t cap, const char *s, int maxCols) {
  if (cap == 0) return 0;
  if (s == nullptr) s = "";
  if (maxCols > (int)cap - 1) maxCols = (int)cap - 1;
  if (maxCols < 0) maxCols = 0;
  const int length = (int)strlen(s);
  const bool cut = length > maxCols;
  const int keep = !cut ? length : (maxCols >= 2 ? maxCols - 2 : 0);
  int n = 0;
  for (; n < keep; ++n) {
    const char ch = s[n];
    out[n] = (ch >= 0x20 && ch <= 0x7E) ? ch : '?';
  }
  if (cut && maxCols >= 2) { out[n++] = '.'; out[n++] = '.'; }
  out[n] = '\0';
  return n;
}

// Font0, size 1, top-left: also the state the kit and upstream's drawing expect to find afterwards.
void drawAscii(const char *s, int x, int y, uint16_t color) {
  LGFX_Sprite &c = display::canvas();
  c.setFont(&fonts::Font0);
  c.setTextSize(1);
  c.setTextDatum(textdatum_t::top_left);
  c.setTextColor(color);
  c.drawString(s, x, y);
  display::touch();
}

void clampScroll(int cursor, int &scroll, int count, int visible) {
  if (visible < 1) visible = 1;
  if (cursor >= 0) {
    if (cursor < scroll) scroll = cursor;
    if (cursor >= scroll + visible) scroll = cursor - visible + 1;
  }
  const int last = count > visible ? count - visible : 0;
  if (scroll > last) scroll = last;
  if (scroll < 0) scroll = 0;
}

}  // namespace

// ---- input ----------------------------------------------------------------------------------------

bool back() {
  if (!buttons::pressed(BTN_B)) return false;
  pop();
  return true;
}

// ---- drawing ----------------------------------------------------------------------------------------

void frame(const char *title, const char *footLeft, const char *footRight) {
  char right[40];
  vk::ui::receipt::page();
  vk::ui::receipt::statusRight(right, sizeof right);
  vk::ui::receipt::header("BADGEOS", right);
  vk::ui::receipt::title(title, TITLE_Y);
  vk::ui::receipt::footer(footLeft ? footLeft : "", footRight ? footRight : "");
}

void text(int x, int y, const char *s, vk::ui::theme::Token token, int maxCols) {
  char line[64];
  if (ascii(line, sizeof line, s, maxCols) == 0) return;
  drawAscii(line, x, y, vk::ui::theme::color(token));
}

void textCentered(int cx, int y, const char *s, vk::ui::theme::Token token) {
  char line[64];
  const int n = ascii(line, sizeof line, s, TEXT_COLS);
  if (n == 0) return;
  const int w = n * CHAR_W - 1;              // the last column's blank pixel is not ink
  drawAscii(line, cx - w / 2, y, vk::ui::theme::color(token));
}

void textRight(int xRight, int y, const char *s, vk::ui::theme::Token token) {
  char line[64];
  const int n = ascii(line, sizeof line, s, TEXT_COLS);
  if (n == 0) return;
  drawAscii(line, xRight - n * CHAR_W, y, vk::ui::theme::color(token));
}

void bar(int x, int y, int cells, uint32_t value, uint32_t max) {
  if (cells <= 0) return;
  LGFX_Sprite &c = display::canvas();
  if (max == 0) max = 1;
  if (value > max) value = max;
  const int filled = (int)(((uint64_t)value * (uint32_t)cells + max / 2) / max);
  const uint16_t ink = vk::ui::theme::color(vk::ui::theme::INK);
  const uint16_t faint = vk::ui::theme::color(vk::ui::theme::FAINT);
  for (int i = 0; i < cells; ++i) {
    const int cx = x + i * (CELL_W + CELL_GAP);
    if (i < filled) {
      c.fillRect(cx, y, CELL_W, CELL_H, ink);
    } else {
      c.drawRect(cx, y, CELL_W, CELL_H, faint);
    }
  }
  display::touch();
}

void pulseLed(uint16_t ms) { vk::ui::leds::pulseTheme(ms); }

void pulseLedBad(uint16_t ms) { ::leds::pulse(0xFF, 0x45, 0x45, ms); }

const char *onOff(bool on) { return on ? "on" : "off"; }

uint16_t onOffColor(bool on) {
  return vk::ui::theme::color(on ? vk::ui::theme::STAMP_OK : vk::ui::theme::SUB);
}

// ---- lists ------------------------------------------------------------------------------------------

bool listMove(List &list, int count, int visible) {
  if (count <= 0) {
    list.cursor = 0;
    list.scroll = 0;
    return false;
  }
  int cursor = list.cursor;
  bool moved = false;
  if (buttons::repeated(BTN_UP)) {
    cursor = cursor <= 0 ? count - 1 : cursor - 1;       // wrap up
    moved = true;
  } else if (buttons::repeated(BTN_DOWN)) {
    cursor = cursor >= count - 1 ? 0 : cursor + 1;       // wrap down
    moved = true;
  } else if (cursor >= count) {
    cursor = count - 1;                                  // the list got shorter under the cursor
    moved = true;
  }
  if (!moved) return false;
  list.cursor = cursor;
  clampScroll(list.cursor, list.scroll, count, visible);
  repaint();
  return true;
}

void listDraw(const List &list, const ListRow *rows, int count, int y0, int visible) {
  if (rows == nullptr || count <= 0) return;
  int scroll = list.scroll;
  clampScroll(list.cursor < count ? list.cursor : count - 1, scroll, count, visible);
  for (int i = 0; i < visible && scroll + i < count; ++i) {
    const int index = scroll + i;
    const ListRow &row = rows[index];
    const bool selected = index == list.cursor;
    // A selected row is inverted: its value is drawn in paper like its label, whatever its colour.
    vk::ui::receipt::row(X0, X1, y0 + i * ROW_PITCH, row.label ? row.label : "", row.value ? row.value : "",
                         selected, selected ? 0 : row.valueColor);
  }
  if (count > visible) {
    char position[16];
    const int at = list.cursor >= 0 ? list.cursor + 1 : (scroll + visible < count ? scroll + visible : count);
    snprintf(position, sizeof position, "%d/%d", at, count);
    textRight(X1, TITLE_Y + 2, position, vk::ui::theme::FAINT);
  }
}

}  // namespace vk::shell
