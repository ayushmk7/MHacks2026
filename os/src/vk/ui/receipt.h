// The receipt drawing kit every Badge OS screen uses (ui.md, "The receipt kit").
//
// Every function draws into display::canvas() with colours from the active theme and calls
// display::touch(). Each one leaves the canvas text state as upstream expects it (Font0, size 1,
// top-left datum), because upstream's own drawing never sets a font.
//
// Text. A `y` is the top of the capital letters. Body text is Font0 (6 px per column). Three
// characters outside ASCII are drawn, each one column wide, when written as UTF-8: the middle dot
// (U+00B7) and the small left and right triangles (U+25C2, U+25B8). Any other byte outside
// 0x20..0x7E is drawn as '?'. Text that does not fit its space is cut and ends in "..".
//
// Rows. A row at `y` owns the 18 px from y-5 to y+12 (the row pitch): that is what a selected row
// fills, from x0-10 to x1+10. A subline belongs at y+13 and owns the 13 px below that, so a row with
// a subline has a pitch of 31.
#pragma once

#include <Arduino.h>

#include "theme.h"

namespace vk::ui::receipt {
void page();                                              // fill PAPER
void header(const char *left, const char *right);         // y 0..19: text at y=7, then a dashed rule at y=19
void statusRight(char *out, size_t cap);                  // "14:32 . 87% . [2]" plus SETUP / DEV when they apply
void title(const char *text, int y);                      // centred, letter-spaced, FreeMonoBold9pt7b
void rule(int y, int x0 = 10, int x1 = 310);              // dashed: 3 px on, 2 px off
void perforation(int x, int y0, int y1);                  // dashed vertical line
void row(int x0, int x1, int y, const char *label, const char *value, bool selected = false, uint16_t valueColor = 0);
                                                          // label left, value right, dotted leader (one dot every 3 px) between
void subline(int x0, int x1, int y, const char *text, bool selected = false);   // SUB colour, indented 12 px
void amount(int cx, int y, const char *label, const char *value, const char *unit);   // label, big serif value, unit; centred on cx
void barcode(int x, int y, int w, int h, const uint8_t *seed, size_t seedLen);       // bars derived from the bytes
void holdBar(float progress);                             // y 204..209, x 40..280
void footer(const char *left, const char *right);         // dashed rule at y=216, text at y=224

// --- Added with receipt.cpp (WP12); not in the ui.md block. ---
void headerText(const char *left, const char *right);     // header() without its rule: the approval's band sits directly under it
void title(const char *text, int y, int cx);              // title() centred on cx instead of the screen (the body column of a two-column screen is centred on 233)
}
