// The receipt drawing kit (ui.md, "The receipt kit"). Every Badge OS screen is drawn with these
// functions: monospace text, dotted leaders, dashed rules, a serif amount, a barcode.
//
// Pixel reference: docs/design/os-mockups/index.html (the ".t-rc" styles). Coordinates and layout
// constants: ui.md. Glyph sizes below were measured with the installed LovyanGFX 1.2.32.
#include "receipt.h"

#include <stdio.h>
#include <string.h>

#include "../../hal/display.h"
#include "../../hal/power.h"
#include "../core/clock.h"

namespace vk::ui::receipt {

namespace {

// ---- layout constants (ui.md) -----------------------------------------------------------------
constexpr int SCREEN_W = 320;
constexpr int SCREEN_H = 240;
constexpr int MARGIN = 10;             // left and right page margin
constexpr int CHAR_W = 6;              // Font0 column
constexpr int ROW_PITCH = 18;          // a row owns y-5 .. y+12
constexpr int ROW_ABOVE = 5;
constexpr int SUBLINE_H = 13;          // a subline owns y .. y+12
constexpr int SUBLINE_INDENT = 12;
constexpr int HEADER_TEXT_Y = 7;
constexpr int HEADER_RULE_Y = 19;
constexpr int FOOTER_RULE_Y = 216;
constexpr int FOOTER_TEXT_Y = 224;
constexpr int HOLD_X = 40, HOLD_Y = 204, HOLD_W = 240, HOLD_H = 6;   // y 204..209, x 40..279
constexpr int DASH_ON = 3, DASH_OFF = 2;
constexpr int LEADER_STEP = 3;         // one dot every 3 px
constexpr int LEADER_DY = 4;           // leader row, below the top of the capitals
constexpr int LEADER_GAP = 4;          // clear pixels between text and leader

// ---- fonts ------------------------------------------------------------------------------------
// Height of a capital letter or digit, in rows, with the baseline row counted.
constexpr int TITLE_CAP_H = 11;        // FreeMonoBold9pt7b
constexpr int TITLE_SPACING = 2;       // letter-spacing of a title

// The amount: label at y, the value inside the 34 rows from y+15, the unit at y+56.
constexpr int AMOUNT_MAX_W = 136;      // the 146 px stub less 5 px each side
constexpr int AMOUNT_BOX_DY = 15;
constexpr int AMOUNT_BOX_H = 34;       // digit height of FreeSerifBold24pt7b
constexpr int AMOUNT_UNIT_DY = 56;
constexpr int AMOUNT_LABEL_SPACING = 2;
constexpr int AMOUNT_UNIT_SPACING = 1;
struct AmountFont { const lgfx::IFont *font; int digitH; };
// ui.md names the first two. The other two are there so that a value too wide even for the 18 pt
// font is drawn smaller and whole: an amount is never cut and never crosses the perforation.
const AmountFont AMOUNT_FONTS[] = {
    {&fonts::FreeSerifBold24pt7b, 34},
    {&fonts::FreeSerifBold18pt7b, 24},
    {&fonts::FreeSerifBold9pt7b, 12},
};

// ---- text in Font0 ----------------------------------------------------------------------------
// One drawn column. Codes: 0x20..0x7E as themselves; below 0x20, a mark the kit draws by hand
// (Font0 has these shapes only at codes that LovyanGFX's string drawing skips or remaps).
constexpr uint8_t DOT = 0x01;          // middle dot
constexpr uint8_t TRI_LEFT = 0x02;     // small triangle pointing left: the launcher's selection mark
constexpr uint8_t TRI_RIGHT = 0x03;

uint8_t nextColumn(const uint8_t *&p) {
  const uint8_t b = p[0];
  if (b >= 0x20 && b <= 0x7E) { p += 1; return b; }
  if (b == 0xC2 && p[1] == 0xB7) { p += 2; return DOT; }                        // U+00B7
  if (b == 0xE2 && p[1] == 0x97 && (p[2] == 0x82 || p[2] == 0x80 || p[2] == 0x84)) { p += 3; return TRI_LEFT; }    // U+25C2, U+25C0, U+25C4
  if (b == 0xE2 && p[1] == 0x96 && (p[2] == 0xB8 || p[2] == 0xB6 || p[2] == 0xBA)) { p += 3; return TRI_RIGHT; }   // U+25B8, U+25B6, U+25BA
  p += 1;
  return '?';
}

int monoCols(const char *s) {
  int n = 0;
  for (const uint8_t *p = (const uint8_t *)s; *p;) { nextColumn(p); ++n; }
  return n;
}

int monoWidth(const char *s) { return monoCols(s) * CHAR_W; }

// Copies `s` into `out`, cut to `maxCols` columns; a cut text ends in "..".
void clip(char *out, size_t cap, const char *s, int maxCols) {
  if (cap == 0) return;
  if (s == nullptr) s = "";
  if (maxCols < 0) maxCols = 0;
  const int cols = monoCols(s);
  const bool cut = cols > maxCols;
  const int keep = !cut ? cols : (maxCols >= 2 ? maxCols - 2 : 0);
  size_t o = 0;
  const uint8_t *p = (const uint8_t *)s;
  for (int col = 0; col < keep && *p; ++col) {
    const uint8_t *start = p;
    nextColumn(p);
    const size_t bytes = (size_t)(p - start);
    if (o + bytes + 3 > cap) break;                 // keep room for ".." and the terminator
    memcpy(out + o, start, bytes);
    o += bytes;
  }
  if (cut && maxCols >= 2 && o + 3 <= cap) { out[o++] = '.'; out[o++] = '.'; }
  out[o] = '\0';
}

void useFont0(LGFX_Sprite &c, uint16_t color) {
  c.setFont(&fonts::Font0);
  c.setTextSize(1);
  c.setTextDatum(textdatum_t::top_left);
  c.setTextColor(color);
}

// What upstream's drawing assumes: it sets size, colour and datum, but never a font.
void restore(LGFX_Sprite &c) {
  c.setFont(&fonts::Font0);
  c.setTextSize(1);
  c.setTextDatum(textdatum_t::top_left);
}

// Draws `s` in Font0 with its top-left at (x, y). `spacing` is extra pixels between columns.
void mono(const char *s, int x, int y, uint16_t color, int spacing = 0) {
  if (s == nullptr) return;
  LGFX_Sprite &c = display::canvas();
  useFont0(c, color);
  char run[65];
  int n = 0, runX = x;
  auto flushRun = [&]() {
    if (n == 0) return;
    run[n] = '\0';
    c.drawString(run, runX, y);
    n = 0;
  };
  for (const uint8_t *p = (const uint8_t *)s; *p;) {
    const uint8_t code = nextColumn(p);
    if (code < 0x20) {
      flushRun();
      if (code == DOT) {
        c.fillRect(x + 1, y + 2, 2, 2, color);
      } else {
        // 4 columns wide, 7 rows tall, the point on the centre row.
        for (int i = 0; i < 4; ++i) {
          const int col = (code == TRI_LEFT) ? x + 1 + i : x + 4 - i;
          c.drawFastVLine(col, y + 3 - i, 2 * i + 1, color);
        }
      }
    } else {
      if (n == 0) runX = x;
      run[n++] = (char)code;
      if (spacing != 0 || n == (int)sizeof(run) - 1) flushRun();
    }
    x += CHAR_W + spacing;
  }
  flushRun();
}

// Font0 text centred on cx, with letter-spacing.
void monoCentered(const char *s, int cx, int y, uint16_t color, int spacing) {
  if (s == nullptr || s[0] == '\0') return;
  const int cols = monoCols(s);
  const int w = cols * CHAR_W + (cols - 1) * spacing - 1;   // the last column's blank pixel is not ink
  mono(s, cx - w / 2, y, color, spacing);
}

// ---- text in a proportional font, one character at a time (letter-spacing) ----------------------
char printable(char ch) { return (ch >= 0x20 && ch <= 0x7E) ? ch : '?'; }

int spacedWidth(LGFX_Sprite &g, const char *s, int spacing) {
  int w = 0;
  char one[2] = {0, 0};
  for (const char *p = s; *p; ++p) {
    one[0] = printable(*p);
    w += (int)g.textWidth(one) + (p[1] ? spacing : 0);
  }
  return w;
}

// The font and colour are already set on `g`. `baseline` is the row the capitals stand on.
void spaced(LGFX_Sprite &g, const char *s, int x, int baseline, int spacing) {
  g.setTextSize(1);
  g.setTextDatum(textdatum_t::baseline_left);
  char one[2] = {0, 0};
  for (const char *p = s; *p; ++p) {
    one[0] = printable(*p);
    g.drawString(one, x, baseline);
    x += (int)g.textWidth(one) + spacing;
  }
}

int clampInt(int v, int lo, int hi) { return hi < lo ? (lo + hi) / 2 : (v < lo ? lo : (v > hi ? hi : v)); }

}  // namespace

// ---- page, header, footer -----------------------------------------------------------------------

void page() {
  LGFX_Sprite &c = display::canvas();
  c.fillScreen(theme::color(theme::PAPER));
  restore(c);
  display::touch();
}

void headerText(const char *left, const char *right) {
  const uint16_t ink = theme::color(theme::INK);
  char r[80], l[80];
  clip(r, sizeof r, right, (SCREEN_W - 2 * MARGIN) / CHAR_W);
  const int rw = monoWidth(r);
  const int room = SCREEN_W - 2 * MARGIN - rw - (rw ? 2 * CHAR_W : 0);   // two columns clear of the right text
  clip(l, sizeof l, left, room / CHAR_W);
  mono(l, MARGIN, HEADER_TEXT_Y, ink);
  mono(r, SCREEN_W - MARGIN - rw, HEADER_TEXT_Y, ink);
  display::touch();
}

void header(const char *left, const char *right) {
  headerText(left, right);
  rule(HEADER_RULE_Y);
}

void statusRight(char *out, size_t cap) {
  if (out == nullptr || cap == 0) return;
  out[0] = '\0';
  // A part is added whole or not at all, so a short buffer never ends inside the separator.
  auto add = [&](const char *part) {
    static const char SEP[] = " \xC2\xB7 ";           // middle dot; the kit's text functions draw it
    const size_t have = strlen(out);
    const size_t need = (have ? sizeof(SEP) - 1 : 0) + strlen(part);
    if (have + need + 1 > cap) return;
    if (have) strlcat(out, SEP, cap);
    strlcat(out, part, cap);
  };
  char part[16];
  if (vk::clock::ok()) {
    const uint32_t t = vk::clock::now();              // unix seconds: the time shown is UTC
    snprintf(part, sizeof part, "%02u:%02u", (unsigned)((t / 3600) % 24), (unsigned)((t / 60) % 60));
    add(part);
  }
  // The badge has no fuel gauge: the only real measurement is the cell voltage. On external power the
  // ADC reads the charger, not the cell, so a percentage there would be invented; say USB instead.
  if (power::charging()) {
    add("USB");
  } else {
    int percent = (int)(power::percent() + 0.5f);
    percent = clampInt(percent, 0, 100);
    snprintf(part, sizeof part, "%d%%", percent);
    add(part);
  }
}

void footer(const char *left, const char *right) {
  LGFX_Sprite &c = display::canvas();
  const uint16_t ink = theme::color(theme::INK);
  // The footer covers whatever a long screen drew underneath it.
  c.fillRect(0, FOOTER_RULE_Y, SCREEN_W, SCREEN_H - FOOTER_RULE_Y, theme::color(theme::PAPER));
  rule(FOOTER_RULE_Y, 0, SCREEN_W - 1);              // edge to edge, as in the simulation
  char r[80], l[80];
  clip(r, sizeof r, right, (SCREEN_W - 2 * MARGIN) / CHAR_W);
  const int rw = monoWidth(r);
  const int room = SCREEN_W - 2 * MARGIN - rw - (rw ? 2 * CHAR_W : 0);
  clip(l, sizeof l, left, room / CHAR_W);
  mono(l, MARGIN, FOOTER_TEXT_Y, ink);
  mono(r, SCREEN_W - MARGIN - rw, FOOTER_TEXT_Y, ink);
  display::touch();
}

// ---- title, rules ---------------------------------------------------------------------------------

void title(const char *text, int y, int cx) {
  if (text == nullptr || text[0] == '\0') return;
  LGFX_Sprite &c = display::canvas();
  c.setFont(&fonts::FreeMonoBold9pt7b);
  c.setTextSize(1);
  c.setTextColor(theme::color(theme::INK));
  const int w = spacedWidth(c, text, TITLE_SPACING);
  spaced(c, text, cx - w / 2, y + TITLE_CAP_H - 1, TITLE_SPACING);
  restore(c);
  display::touch();
}

void title(const char *text, int y) { title(text, y, SCREEN_W / 2); }

void rule(int y, int x0, int x1) {
  LGFX_Sprite &c = display::canvas();
  const uint16_t ink = theme::color(theme::INK);
  for (int x = x0; x <= x1; x += DASH_ON + DASH_OFF) {
    const int w = (x1 - x + 1 < DASH_ON) ? (x1 - x + 1) : DASH_ON;
    c.drawFastHLine(x, y, w, ink);
  }
  display::touch();
}

void perforation(int x, int y0, int y1) {
  LGFX_Sprite &c = display::canvas();
  const uint16_t ink = theme::color(theme::INK);
  for (int y = y0; y <= y1; y += DASH_ON + DASH_OFF) {
    const int h = (y1 - y + 1 < DASH_ON) ? (y1 - y + 1) : DASH_ON;
    c.drawFastVLine(x, y, h, ink);
  }
  display::touch();
}

// ---- rows -----------------------------------------------------------------------------------------

void row(int x0, int x1, int y, const char *label, const char *value, bool selected, uint16_t valueColor) {
  LGFX_Sprite &c = display::canvas();
  const uint16_t text = theme::color(selected ? theme::PAPER : theme::INK);
  const uint16_t leader = selected ? text : theme::color(theme::FAINT);
  if (selected) c.fillRect(x0 - MARGIN, y - ROW_ABOVE, (x1 - x0) + 2 * MARGIN, ROW_PITCH, theme::color(theme::INK));

  // The label is kept; the value gets what is left, less two columns for the leader.
  const int cols = (x1 - x0) / CHAR_W;
  char l[64], v[64];
  clip(l, sizeof l, label, cols);
  const int lc = monoCols(l);
  clip(v, sizeof v, value, cols - lc - (lc ? 2 : 0));
  const int lw = lc * CHAR_W, vw = monoWidth(v);

  mono(l, x0, y, text);
  mono(v, x1 - vw, y, valueColor != 0 ? valueColor : text);

  // Dotted leader between them, on a 3 px grid so that the leaders of stacked rows line up.
  int from = x0 + (lw ? lw - 1 + LEADER_GAP : 0);
  const int to = x1 - (vw ? vw + LEADER_GAP : 0);
  from += (LEADER_STEP - from % LEADER_STEP) % LEADER_STEP;
  for (int x = from; x < to; x += LEADER_STEP) c.drawPixel(x, y + LEADER_DY, leader);
  display::touch();
}

void subline(int x0, int x1, int y, const char *text, bool selected) {
  LGFX_Sprite &c = display::canvas();
  if (selected) c.fillRect(x0 - MARGIN, y, (x1 - x0) + 2 * MARGIN, SUBLINE_H, theme::color(theme::INK));
  char t[80];
  clip(t, sizeof t, text, (x1 - x0 - SUBLINE_INDENT) / CHAR_W);
  mono(t, x0 + SUBLINE_INDENT, y, theme::color(selected ? theme::PAPER : theme::SUB));
  display::touch();
}

// ---- amount, barcode --------------------------------------------------------------------------------

void amount(int cx, int y, const char *label, const char *value, const char *unit) {
  LGFX_Sprite &c = display::canvas();
  const uint16_t ink = theme::color(theme::INK);
  monoCentered(label, cx, y, ink, AMOUNT_LABEL_SPACING);

  if (value != nullptr && value[0] != '\0') {
    char v[40];
    size_t n = 0;
    for (const char *p = value; *p && n + 1 < sizeof v; ++p) v[n++] = printable(*p);
    v[n] = '\0';
    c.setTextSize(1);
    c.setTextColor(ink);
    const AmountFont *pick = nullptr;
    for (const AmountFont &f : AMOUNT_FONTS) {
      c.setFont(f.font);
      if ((int)c.textWidth(v) <= AMOUNT_MAX_W) { pick = &f; break; }
    }
    if (pick != nullptr) {
      // Centred in the 34-row box, standing on its baseline.
      const int top = y + AMOUNT_BOX_DY + (AMOUNT_BOX_H - pick->digitH) / 2;
      c.setTextDatum(textdatum_t::baseline_center);
      c.drawString(v, cx, top + pick->digitH - 1);
    } else {
      monoCentered(v, cx, y + AMOUNT_BOX_DY + (AMOUNT_BOX_H - 7) / 2, ink, 0);
    }
    restore(c);
  }

  monoCentered(unit, cx, y + AMOUNT_UNIT_DY, ink, AMOUNT_UNIT_SPACING);
  display::touch();
}

void barcode(int x, int y, int w, int h, const uint8_t *seed, size_t seedLen) {
  if (w <= 0 || h <= 0) return;
  LGFX_Sprite &c = display::canvas();
  const uint16_t ink = theme::color(theme::INK);
  // The simulation's fixed pattern, for a caller with no key to print.
  static const uint8_t PLAIN[] = {0x15, 0x92, 0x6B, 0x04, 0xD3, 0x28, 0x7C, 0xA1};
  if (seed == nullptr || seedLen == 0) { seed = PLAIN; seedLen = sizeof PLAIN; }
  // Each half-byte is one bar and the gap after it: bar 1..3 px, gap 1..3 px.
  const size_t nibbles = seedLen * 2;
  const int end = x + w;
  size_t i = 0;
  for (int px = x; px < end; ++i) {
    const uint8_t b = seed[(i % nibbles) / 2];
    const uint8_t nib = (i % 2 == 0) ? (uint8_t)(b >> 4) : (uint8_t)(b & 0x0F);
    int bar = 1 + (nib & 3) % 3;
    const int gap = 1 + (nib >> 2) % 3;
    if (px + bar > end) bar = end - px;
    c.fillRect(px, y, bar, h, ink);
    px += bar + gap;
  }
  display::touch();
}

// ---- hold bar -----------------------------------------------------------------------------------------

void holdBar(float progress) {
  LGFX_Sprite &c = display::canvas();
  if (!(progress > 0.0f)) progress = 0.0f;           // also catches NaN
  if (progress > 1.0f) progress = 1.0f;
  const int filled = (int)(HOLD_W * progress + 0.5f);
  const uint16_t track = theme::color(theme::FAINT);
  c.fillRect(HOLD_X, HOLD_Y, HOLD_W, HOLD_H, theme::color(theme::PAPER));
  c.fillRect(HOLD_X, HOLD_Y, filled, HOLD_H, theme::color(theme::INK));
  // The track is a half-tone of FAINT, like the simulation's shaded blocks.
  for (int y = HOLD_Y; y < HOLD_Y + HOLD_H; ++y) {
    for (int x = HOLD_X + filled + ((HOLD_X + filled + y) & 1); x < HOLD_X + HOLD_W; x += 2) c.drawPixel(x, y, track);
  }
  display::touch();
}

}  // namespace vk::ui::receipt
