// src/vk/ui/keyboard.cpp
// The on-screen keyboard: drawing, buttons and timers (docs/os/ui/text-entry.md). The layout, the
// cursor and the text are keyboard_core.c's; this file never decides what a key does.
//
// Drawing. draw() paints the whole screen. update() paints only what its own input changed: the
// two cells a cursor step touches, the text line when a character comes or goes, the grid when
// the layer changes. Both go to the canvas; the main loop sends it to the panel.
//
// Secrets. The text of a field marked secret is never logged, never sent over serial and never
// handed to the dev hook below (which reports its length only). It is wiped when the entry ends.
#include "keyboard.h"

#include <stdio.h>
#include <string.h>

#include "../../config.h"
#include "../../hal/buttons.h"
#include "../../hal/display.h"
#include "../shell/page.h"         // text(), textRight(), textCentered(), X0, X1, TITLE_Y; push(), pop()
#include "../vk_build.h"
#include "receipt.h"

#if VK_TEST_HOOKS
#include "../core/serial.h"
#endif

namespace vk::ui::keyboard {

namespace {

namespace th = vk::ui::theme;
using vk::shell::X0;
using vk::shell::X1;

// ---- timing ---------------------------------------------------------------------------------------
constexpr uint32_t HOLD_ALT_MS = 450;     // SELECT held this long: the letter it typed changes case
constexpr uint32_t HOLD_EXIT_MS = 700;    // CANCEL held this long: leave, whatever has been typed
constexpr uint32_t REVEAL_MS = 1200;      // a masked field shows the character just typed this long
constexpr uint32_t NOTE_MS = 2500;        // how long a refusal ("needs 8 or more") replaces the hint
// While an app runs, upstream force-quits it when CANCEL is held APP_ESCAPE_HOLD_MS: the hold that
// only leaves the keyboard must come first.
static_assert(HOLD_EXIT_MS < APP_ESCAPE_HOLD_MS, "the keyboard's CANCEL hold must be shorter than the force-quit hold");

// ---- layout, in pixels ------------------------------------------------------------------------------
// The typed text and the character keys are FreeMonoBold9pt7b: 11 px per character, capitals 11
// rows tall, glyphs between 12 rows above the baseline and 4 below it (LovyanGFX 1.2.32).
constexpr int GLYPH_W = 11;
constexpr int FIELD_TOP = 42;             // the text line owns y 42..62
constexpr int FIELD_H = 21;
constexpr int FIELD_BASE = 57;            // baseline of the typed text
constexpr int FIELD_SLOTS = 24;           // characters across, the caret included (x 10..273)
constexpr int FIELD_RULE_Y = 64;
constexpr int COUNT_Y = 49;               // "12/63", Font0, right-aligned at X1
constexpr int HINT_Y = 68;                // one Font0 line: the hint, or why a key was refused
constexpr int GRID_X = 13;                // 7 cells of 42 px: x 13..306
constexpr int GRID_Y = 80;                // 4 rows of 24 px: y 80..175
constexpr int KEY_W = 42;
constexpr int KEY_H = 24;
constexpr int KEY_BASE = 16;              // baseline of a key's character, from the top of its cell
constexpr int ACTION_RULE_Y = 178;
constexpr int ACTION_Y = 181;             // the action row: y 181..204
constexpr int LABEL_DY = 8;               // top of an action key's Font0 label, from the top of its cell
constexpr int MARK_DY = 19;               // the bar under the label of the layer that is showing:
constexpr int MARK_W = 18;                // as wide as its three characters, 2 px thick
constexpr int SCREEN_W = 320;

// ---- state ----------------------------------------------------------------------------------------
vk_kb_t sKb;
State sState = State::CLOSED;
char sTitle[24] = "";
char sHint[52] = "";
char sNote[52] = "";                      // shown instead of the hint until sNoteUntil
uint32_t sNoteUntil = 0;                  // 0 = no note
uint32_t sRevealUntil = 0;                // 0 = the last character is masked like the others
bool sSelectArmed = false;                // this SELECT press typed a letter: a hold changes its case
bool sCancelArmed = false;                // this CANCEL press erased a character here: a hold leaves
bool sAcceptDrawn = false;                // how the DONE key was last drawn
Done sDone = nullptr;

bool secret() { return (sKb.flags & VK_KB_SECRET) != 0; }
bool due(uint32_t now, uint32_t at) { return at != 0 && (int32_t)(now - at) >= 0; }
uint32_t later(uint32_t now, uint32_t ms) { const uint32_t at = now + ms; return at != 0 ? at : 1; }

// ---- drawing ----------------------------------------------------------------------------------------

// The state every kit function and upstream's own drawing expect to find.
void restoreFont(LGFX_Sprite &c) {
  c.setFont(&fonts::Font0);
  c.setTextSize(1);
  c.setTextDatum(textdatum_t::top_left);
}

// One character of the key font, its 11 px box starting at x, standing on `baseline`.
void glyph(LGFX_Sprite &c, char ch, int x, int baseline, uint16_t color) {
  const char one[2] = {ch, '\0'};
  c.setFont(&fonts::FreeMonoBold9pt7b);
  c.setTextSize(1);
  c.setTextDatum(textdatum_t::baseline_left);
  c.setTextColor(color);
  c.drawString(one, x, baseline);
}

// A typed space, when the text is shown: a low bracket, so that it can be seen and counted.
void spaceMark(LGFX_Sprite &c, int x, int baseline, uint16_t color) {
  c.drawFastHLine(x + 1, baseline, GLYPH_W - 3, color);
  c.drawFastVLine(x + 1, baseline - 3, 3, color);
  c.drawFastVLine(x + GLYPH_W - 3, baseline - 3, 3, color);
}

void paintField() {
  LGFX_Sprite &c = display::canvas();
  const uint16_t ink = th::color(th::INK);
  c.fillRect(0, FIELD_TOP, SCREEN_W, FIELD_H, th::color(th::PAPER));

  // The end of the text is what is being edited: when it is longer than the line, the start is
  // replaced by a mark.
  const int len = sKb.len;
  const bool cut = len + 1 > FIELD_SLOTS;
  const int first = cut ? len - (FIELD_SLOTS - 2) : 0;
  int x = X0;
  if (cut) {
    for (int i = 0; i < 4; ++i) c.drawFastVLine(x + 3 + i, FIELD_BASE - 5 - i, 2 * i + 1, th::color(th::FAINT));
    x += GLYPH_W;
  }
  const bool clear = sKb.shown != 0;
  for (int i = first; i < len; ++i, x += GLYPH_W) {
    const char ch = sKb.text[i];
    if (!clear && !(i == len - 1 && sRevealUntil != 0)) {
      c.fillCircle(x + GLYPH_W / 2, FIELD_BASE - 5, 2, ink);
    } else if (ch == ' ') {
      spaceMark(c, x, FIELD_BASE, th::color(th::FAINT));
    } else {
      glyph(c, ch, x, FIELD_BASE, ink);
    }
  }
  c.fillRect(x + 1, FIELD_BASE + 1, GLYPH_W - 2, 2, ink);        // the caret
  restoreFont(c);

  char count[12];
  snprintf(count, sizeof count, "%u/%u", (unsigned)sKb.len, (unsigned)sKb.max_len);
  vk::shell::textRight(X1, COUNT_Y, count, sKb.len >= sKb.max_len ? th::STAMP_WARN : th::FAINT);
  display::touch();
}

void paintHint() {
  display::canvas().fillRect(0, HINT_Y - 1, SCREEN_W, 10, th::color(th::PAPER));
  if (sNoteUntil != 0) {
    vk::shell::text(X0, HINT_Y, sNote, th::STAMP_WARN);
  } else {
    vk::shell::text(X0, HINT_Y, sHint, th::SUB);
  }
  display::touch();
}

// The key at (row, col), with the cursor's highlight when it is there. An action key is painted
// whole, whichever of its columns is asked for.
void paintKey(int row, int col) {
  LGFX_Sprite &c = display::canvas();
  const vk_kb_key_t key = vk_kb_key(&sKb, row, col);
  int first = col, last = col;
  if (row == VK_KB_ACTION_ROW) vk_kb_action_span(&sKb, col, &first, &last);
  const bool selected = sKb.row == row && sKb.col >= first && sKb.col <= last;
  const int x = GRID_X + first * KEY_W;
  const int w = (last - first + 1) * KEY_W;
  const int y = row == VK_KB_ACTION_ROW ? ACTION_Y : GRID_Y + row * KEY_H;
  const uint16_t ground = th::color(selected ? th::INK : th::PAPER);
  const uint16_t ink = th::color(selected ? th::PAPER : th::INK);

  c.fillRect(x, y, w, KEY_H, th::color(th::PAPER));
  if (selected) c.fillRect(x + 1, y + 1, w - 2, KEY_H - 2, ground);

  if (key.kind == VK_KB_KEY_CHAR) {
    glyph(c, key.ch, x + (KEY_W - GLYPH_W) / 2, y + KEY_BASE, ink);
    restoreFont(c);
    display::touch();
    return;
  }

  const char *label = "";
  th::Token token = selected ? th::PAPER : th::INK;
  switch (key.kind) {
    case VK_KB_KEY_LAYER: label = vk_kb_layer_name(key.layer); break;
    case VK_KB_KEY_SPACE: label = "space"; break;
    case VK_KB_KEY_SHOW:  label = sKb.shown ? "hide" : "show"; break;
    default:
      label = "DONE";
      sAcceptDrawn = vk_kb_acceptable(&sKb) != 0;
      if (!sAcceptDrawn && !selected) token = th::FAINT;      // it will be refused: too short
      break;
  }
  vk::shell::textCentered(x + w / 2, y + LABEL_DY, label, token);
  if (key.kind == VK_KB_KEY_LAYER && key.layer == sKb.layer) {
    c.fillRect(x + (KEY_W - MARK_W) / 2, y + MARK_DY, MARK_W, 2, ink);
  }
  display::touch();
}

void paintGrid() {
  for (int row = 0; row < VK_KB_CHAR_ROWS; ++row) {
    for (int col = 0; col < VK_KB_COLS; ++col) paintKey(row, col);
  }
}

void paintActions() {
  for (int col = 0; col < VK_KB_COLS; ++col) {
    int first = col, last = col;
    vk_kb_action_span(&sKb, col, &first, &last);
    if (col == first) paintKey(VK_KB_ACTION_ROW, col);
  }
}

void paintFooter() {
  const char *left = "SELECT type";
  if (sKb.layer == VK_KB_LOWER) left = "SELECT type  hold: CAPS";
  if (sKb.layer == VK_KB_UPPER) left = "SELECT type  hold: small";
  receipt::footer(left, sKb.len == 0 ? "CANCEL back" : "CANCEL erase  hold: exit");
}

void note(uint32_t now, const char *format, unsigned value) {
  snprintf(sNote, sizeof sNote, format, value);
  sNoteUntil = later(now, NOTE_MS);
  paintHint();
}

// The text changed: its line, and the two things that depend on its length.
void textChanged(int lengthBefore) {
  paintField();
  if ((lengthBefore == 0) != (sKb.len == 0)) paintFooter();
  if ((vk_kb_acceptable(&sKb) != 0) != sAcceptDrawn) paintKey(VK_KB_ACTION_ROW, VK_KB_COLS - 1);
}

void step(int dx, int dy) {
  const int row = sKb.row, col = sKb.col;
  vk_kb_move(&sKb, dx, dy);
  if (sKb.row == row && sKb.col == col) return;
  paintKey(row, col);
  paintKey(sKb.row, sKb.col);
}

void pressSelect(uint32_t now) {
  const int before = sKb.len;
  switch (vk_kb_select(&sKb)) {
    case VK_KB_TYPED:
      sSelectArmed = sKb.can_swap != 0;
      sRevealUntil = (secret() && !sKb.shown) ? later(now, REVEAL_MS) : 0;
      textChanged(before);
      break;
    case VK_KB_FULL:
      note(now, "%u characters is the most", sKb.max_len);
      break;
    case VK_KB_LAYER_CHANGED:
      paintGrid();
      paintActions();
      paintFooter();
      break;
    case VK_KB_SHOW_CHANGED:
      sRevealUntil = 0;
      paintField();
      paintKey(sKb.row, sKb.col);
      break;
    case VK_KB_ACCEPT:
      sState = State::DONE;
      break;
    case VK_KB_TOO_SHORT:
      note(now, (sKb.flags & VK_KB_EMPTY_OK) ? "needs %u or more characters, or none" : "needs %u or more characters",
           sKb.min_len);
      break;
    default:
      break;
  }
}

// ---- as a shell screen ------------------------------------------------------------------------------

void screenUpdate() {
  const State now = update();
  if (now != State::DONE && now != State::CANCELLED) return;
  // The callback may open the keyboard again, so this entry is over before it runs: the text is
  // copied out, the module's own copy wiped, the screen popped.
  char copy[TEXT_MAX + 1];
  const bool accepted = now == State::DONE;
  strlcpy(copy, accepted ? text() : "", sizeof copy);
  const Done done = sDone;
  sDone = nullptr;
  end();
  vk::shell::pop();
  if (done != nullptr) done(accepted, copy);
  volatile char *p = copy;
  for (size_t i = 0; i < sizeof copy; ++i) p[i] = 0;
}

const vk::shell::Screen kScreen = {"keyboard", nullptr, screenUpdate, draw, 0};

}  // namespace

// ---- the entry --------------------------------------------------------------------------------------

void begin(const Options &options) {
  const uint8_t flags = (uint8_t)((options.secret ? VK_KB_SECRET : 0) | (options.emptyOk ? VK_KB_EMPTY_OK : 0));
  vk_kb_init(&sKb, options.initial, options.minLen, options.maxLen, flags);
  strlcpy(sTitle, options.title != nullptr ? options.title : "", sizeof sTitle);
  strlcpy(sHint, options.hint != nullptr ? options.hint : "", sizeof sHint);
  sNote[0] = '\0';
  sNoteUntil = 0;
  sRevealUntil = 0;
  sSelectArmed = false;
  sCancelArmed = false;
  sAcceptDrawn = false;
  sState = State::EDITING;
}

void end() {
  vk_kb_wipe(&sKb);
  sTitle[0] = '\0';
  sHint[0] = '\0';
  sState = State::CLOSED;
}

State state() { return sState; }

const char *text() { return sState == State::CLOSED ? "" : sKb.text; }

size_t length() { return sState == State::CLOSED ? 0 : sKb.len; }

State update() {
  if (sState != State::EDITING) return sState;
  const uint32_t now = millis();

  if (buttons::repeated(BTN_UP)) step(0, -1);
  if (buttons::repeated(BTN_DOWN)) step(0, 1);
  if (buttons::repeated(BTN_LEFT)) step(-1, 0);
  if (buttons::repeated(BTN_RIGHT)) step(1, 0);

  // SELECT types on the press, so a tap is on the screen at once. Still down HOLD_ALT_MS later, the
  // letter that press typed changes case.
  if (buttons::pressed(BTN_A)) {
    sSelectArmed = false;
    pressSelect(now);
    if (sState != State::EDITING) return sState;
  } else if (!buttons::down(BTN_A)) {
    sSelectArmed = false;
  } else if (sSelectArmed && buttons::heldMs(BTN_A) >= HOLD_ALT_MS) {
    sSelectArmed = false;
    if (vk_kb_hold(&sKb) == VK_KB_TYPED) {
      sRevealUntil = (secret() && !sKb.shown) ? later(now, REVEAL_MS) : 0;
      paintField();
    }
  }

  // CANCEL erases on the press; on an empty field it leaves. Still down HOLD_EXIT_MS later, it
  // leaves whatever is typed. Only a press that began here counts, so the CANCEL that closed
  // another screen cannot run on into this one.
  if (buttons::pressed(BTN_B)) {
    const int before = sKb.len;
    if (vk_kb_backspace(&sKb) == VK_KB_EMPTY) {
      sState = State::CANCELLED;
      return sState;
    }
    sCancelArmed = true;
    sRevealUntil = 0;
    textChanged(before);
  } else if (!buttons::down(BTN_B)) {
    sCancelArmed = false;
  } else if (sCancelArmed && buttons::heldMs(BTN_B) >= HOLD_EXIT_MS) {
    sState = State::CANCELLED;
    return sState;
  }

  if (due(now, sRevealUntil)) {
    sRevealUntil = 0;
    paintField();
  }
  if (due(now, sNoteUntil)) {
    sNoteUntil = 0;
    paintHint();
  }
  return sState;
}

void draw() {
  char right[40];
  receipt::page();
  receipt::statusRight(right, sizeof right);
  receipt::header("BADGEOS", right);
  receipt::title(sTitle, vk::shell::TITLE_Y);
  paintField();
  receipt::rule(FIELD_RULE_Y);
  paintHint();
  paintGrid();
  receipt::rule(ACTION_RULE_Y);
  paintActions();
  paintFooter();
}

void open(const Options &options, Done done) {
  begin(options);
  sDone = done;
  vk::shell::push(&kScreen);
}

// ---- dev hook ---------------------------------------------------------------------------------------
// VKKBD: the keyboard's state, for device tests (testing.md, "Dev hooks"). Dev profile only. The
// typed text is in the reply only when the field is not secret; a secret field gives its length.
#if VK_TEST_HOOKS
namespace {

void jsonText(String &out, const char *s) {
  out += '"';
  for (; s != nullptr && *s; ++s) {
    if (*s == '"' || *s == '\\') out += '\\';
    out += (*s >= 0x20 && *s <= 0x7E) ? *s : '?';
  }
  out += '"';
}

void cmdKeyboard(const String &, const vk::serial::Reply &reply) {
  String out;
  out.reserve(320);
  out += "OK {\"open\":";
  out += sState == State::EDITING ? "true" : "false";
  if (sState == State::EDITING) {
    const vk_kb_key_t key = vk_kb_current(&sKb);
    char cursor[8] = "";
    switch (key.kind) {
      case VK_KB_KEY_CHAR:  cursor[0] = key.ch; cursor[1] = '\0'; break;
      case VK_KB_KEY_LAYER: strlcpy(cursor, vk_kb_layer_name(key.layer), sizeof cursor); break;
      case VK_KB_KEY_SPACE: strlcpy(cursor, "space", sizeof cursor); break;
      case VK_KB_KEY_SHOW:  strlcpy(cursor, "show", sizeof cursor); break;
      default:              strlcpy(cursor, "DONE", sizeof cursor); break;
    }
    out += ",\"title\":";
    jsonText(out, sTitle);
    out += ",\"layer\":";
    jsonText(out, vk_kb_layer_name(sKb.layer));
    out += ",\"row\":" + String((unsigned)sKb.row) + ",\"col\":" + String((unsigned)sKb.col) + ",\"key\":";
    jsonText(out, cursor);
    out += ",\"len\":" + String((unsigned)sKb.len) + ",\"min\":" + String((unsigned)sKb.min_len) +
           ",\"max\":" + String((unsigned)sKb.max_len);
    out += ",\"secret\":";
    out += secret() ? "true" : "false";
    out += ",\"shown\":";
    out += sKb.shown ? "true" : "false";
    // The character rows of the layer that is showing, so a test finds a key without its own copy
    // of the layout.
    out += ",\"rows\":[";
    for (int row = 0; row < VK_KB_CHAR_ROWS; ++row) {
      char line[VK_KB_COLS + 1];
      for (int col = 0; col < VK_KB_COLS; ++col) line[col] = vk_kb_key(&sKb, row, col).ch;
      line[VK_KB_COLS] = '\0';
      if (row) out += ',';
      jsonText(out, line);
    }
    out += ']';
    if (!secret()) {
      out += ",\"text\":";
      jsonText(out, sKb.text);
    }
  }
  out += '}';
  reply(out);
}

VK_SERIAL_COMMAND(vkkbd, "VKKBD", cmdKeyboard, "the on-screen keyboard: layer, cursor, length (the text only if not secret)");

}  // namespace
#endif  // VK_TEST_HOOKS

}  // namespace vk::ui::keyboard
