// The receipt drawing kit every Badge OS screen uses (ui.md, "The receipt kit").
// Header only until receipt.cpp is written (WP12).
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
void stamp(int cx, int cy, const char *text, theme::Token ink);                      // rotated -13 degrees, double border, faded
void holdBar(float progress);                             // y 204..209, x 40..280
void footer(const char *left, const char *right);         // dashed rule at y=216, text at y=224
}
