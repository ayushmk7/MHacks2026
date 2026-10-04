// src/vk/ui/code128.h
// A Code 128 barcode encoder, subset B (printable ASCII, 32..126): the bars a barcode scanner reads
// back as the text. Pure C99: no Arduino, no heap, no drawing. The caller draws the modules.
//
// A Code 128 symbol is a row of modules, each one unit wide, dark (a bar) or light (a space):
//
//   quiet zone | start B | one symbol per character | check symbol | stop | quiet zone
//       10     |   11    |        11 each           |      11      |  13  |    10      modules
//
// The encoder writes everything between the two quiet zones. The quiet zones are light modules the
// caller leaves empty on both sides: VK_CODE128_QUIET of them at least, or a scanner may not find
// the code. A scanner reads dark on light only, so draw the bars in a dark colour on a light patch
// in every theme.
//
// Width on a 320 px screen: 8 characters are 11 * 10 + 13 = 123 modules, 143 with the quiet zones,
// so a module can be 2 px wide (286 px). 13 characters are the most that fit at 1 px per module.
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Light modules to leave empty on each side of the code.
#define VK_CODE128_QUIET 10

// The number of modules vk_code128_encode writes for a text of `text_len` characters, quiet zones
// not counted: 11 * (text_len + 2) + 13. -1 when text_len is negative.
int vk_code128_width(int text_len);

// Writes the code for `text` into `modules`, one byte per module: 1 is a bar, 0 is a space. The
// first module and the last are bars. Returns the number of modules written, which is
// vk_code128_width(strlen(text)). Returns -1, with nothing written, when `text` or `modules` is
// null, when `text` holds a character outside 32..126, or when the code does not fit `cap` bytes.
// An empty text is valid: start, check symbol and stop, 35 modules.
int vk_code128_encode(const char *text, uint8_t *modules, int cap);

#ifdef __cplusplus
}
#endif
