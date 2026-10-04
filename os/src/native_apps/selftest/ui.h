// src/native_apps/selftest/ui.h
// What every screen of the test suite shares: the list geometry of ui/shell.md ("Any list"), the
// receipt frame, and the colour of a result.
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "selftest.h"

namespace selftest::ui {

constexpr int X0 = 10, X1 = 310;         // the page margins
constexpr int TITLE_Y = 26;
constexpr int FIRST_ROW_Y = 46;          // a row at y owns y-5 .. y+12
constexpr int ROW_PITCH = 18;
constexpr size_t VISIBLE_ROWS = 9;       // the last at y = 190
constexpr int NOTE_Y = 112;              // a centred line of text under a short list

// page, header "BADGEOS" / time and battery, and the title.
void frame(const char *title);

// The first row of the window of VISIBLE_ROWS that holds `cursor`, centred where it can be.
size_t windowStart(size_t cursor, size_t count);

// "n/N" right of the title when a list scrolls (the shell's scroll mark).
void scrollMark(size_t cursor, size_t count);

// One result row: the value, then the state; inverted when selected.
void resultRow(int y, const char *label, const char *value, State state, bool selected);

uint16_t stateColor(State state);
uint16_t sub();                          // the SUB token: notes under a list

// "12 OK · 0 FAIL · 2 --"
void countsText(char *out, size_t cap, const Counts &c);

}  // namespace selftest::ui
