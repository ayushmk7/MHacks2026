// The settings-page registry and the helpers every shell screen draws with
// (docs/os/ui/shell.md, "page.h"). The only shell header a page file includes.
#pragma once

#include <Arduino.h>

#include "../../config.h"          // BTN_UP, BTN_DOWN, BTN_LEFT, BTN_RIGHT, BTN_A (SELECT), BTN_B (CANCEL)
#include "../../hal/buttons.h"     // buttons::pressed, repeated, released, down
#include "../../hal/display.h"     // display::canvas, width, height
#include "../core/registry.h"
#include "../ui/receipt.h"         // vk::ui::receipt::*, vk::ui::theme::*
#include "screens.h"

namespace vk::shell {

// ---- layout, in pixels (ui.md, "The receipt kit") ----
constexpr int X0 = 10;             // left edge of a full-width row
constexpr int X1 = 310;            // right edge
constexpr int TITLE_Y = 26;        // page title
constexpr int LIST_Y = 48;         // first row: a row at y owns y-5 .. y+12
constexpr int ROW_PITCH = 18;
constexpr int LIST_ROWS = 9;       // rows at 48 .. 192; the last one ends at y = 204
constexpr int TEXT_COLS = 50;      // Font0 characters between X0 and X1

// ---- the registry ----
struct SettingsPage : Registered<SettingsPage> {
  const char *id;                          // the page's screen name ("wifi"): [a-z_], unique
  int order;                               // position in the Settings list, ascending
  const char *title;                       // the row's label in the Settings list ("Wi-Fi")
  void (*value)(char *out, size_t cap);    // the row's right-hand text; nullptr = none. Called on every repaint of the list
  void (*action)();                        // an action row: SELECT calls this and the list stays. nullptr for a page
  void (*enter)();                         // a page: called each time it opens; may be nullptr
  void (*update)();                        // a page: every loop pass while on top
  void (*draw)();                          // a page: full repaint
  uint32_t refresh_ms;                     // a page: 0, or the repaint period
  SettingsPage(const char *i, int o, const char *t, void (*v)(char *, size_t), void (*a)(),
               void (*e)(), void (*u)(), void (*d)(), uint32_t r)
      : id(i), order(o), title(t), value(v), action(a), enter(e), update(u), draw(d), refresh_ms(r) {}
};
// A row that opens a page.
#define VK_SETTINGS_PAGE(ident, id, order, title, value_fn, enter_fn, update_fn, draw_fn, refresh_ms) \
  static vk::shell::SettingsPage vk_page_##ident(id, order, title, value_fn, nullptr, enter_fn, update_fn, draw_fn, refresh_ms)
// A row that acts in place (cycle a value, launch an app).
#define VK_SETTINGS_ACTION(ident, id, order, title, value_fn, action_fn) \
  static vk::shell::SettingsPage vk_page_##ident(id, order, title, value_fn, action_fn, nullptr, nullptr, nullptr, 0)

// ---- input ----
bool back();                       // CANCEL pressed on this pass: pop() and return true

// ---- drawing ----
// page(); header("BADGEOS", statusRight); title(title, TITLE_Y); footer(footLeft, footRight).
void frame(const char *title, const char *footLeft, const char *footRight = "CANCEL back");
// Font0 ASCII text in a theme colour, cut to maxCols with "..". y is the top of the capitals.
void text(int x, int y, const char *s, vk::ui::theme::Token token = vk::ui::theme::INK, int maxCols = TEXT_COLS);
void textCentered(int cx, int y, const char *s, vk::ui::theme::Token token = vk::ui::theme::INK);
// A block bar: `cells` cells of 6x8 px, 2 px apart, from x. round(value * cells / max) cells are filled INK; the rest are outlined FAINT.
void bar(int x, int y, int cells, uint32_t value, uint32_t max);
void pulseLed(uint16_t ms);        // ::leds::pulse in the active theme's LED colour
void pulseLedBad(uint16_t ms);     // ::leds::pulse(0xFF, 0x45, 0x45, ms): the fixed severity red
const char *onOff(bool on);        // "on" / "off"
uint16_t onOffColor(bool on);      // theme STAMP_OK when on, SUB when off

// ---- lists ----
struct ListRow { const char *label; const char *value; uint16_t valueColor; };   // valueColor 0 = ink
struct List { int cursor = 0; int scroll = 0; };
// UP/DOWN with key repeat (buttons::repeated), wrapping at both ends, keeping the cursor inside the
// `visible` window. Returns true when the cursor moved (it has already called repaint()).
bool listMove(List &list, int count, int visible = LIST_ROWS);
// Draws rows scroll .. scroll+visible-1 with receipt::row(X0, X1, y0 + i * ROW_PITCH, ...); the row under
// the cursor is selected (inverted). When count > visible it also draws "n/N" (selected row / rows)
// right-aligned at (X1, TITLE_Y + 2) in FAINT. Pass a List with cursor -1 for rows that cannot be
// selected: such a list has no mark.
void listDraw(const List &list, const ListRow *rows, int count, int y0 = LIST_Y, int visible = LIST_ROWS);

// ---- additions (the shell's owner may add declarations below this line; nothing above changes) ----

// Font0 text whose right edge is at xRight (the "n/N" scroll mark of a list or of the launcher).
void textRight(int xRight, int y, const char *s, vk::ui::theme::Token token = vk::ui::theme::INK);
// Swaps the top screen for `screen` and calls its enter() (offer -> installing). On the launcher it pushes.
void replaceTop(const Screen *screen);
// The stack becomes [launcher, screen]; calls enter(). The launcher is not re-entered: it keeps its cursor.
void showOver(const Screen *screen);

}  // namespace vk::shell
