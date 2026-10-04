// src/vk/ui/code128.c
// Code 128, subset B (code128.h). Tested on the laptop: test/host/test_code128.c.
#include "code128.h"

#include <stddef.h>

// The 107 patterns of Code 128, by symbol value. Each is written as the widths of its elements in
// modules, one decimal digit per element, starting with a bar and alternating: 212222 is a bar of
// 2, a space of 2, a bar of 1, a space of 2, a bar of 2, a space of 2. Values 0..105 have six
// elements and are 11 modules wide; the stop pattern (106) has seven and is 13 wide, ending in the
// final 2-module bar. In subset B, value v stands for the character with code v + 32 (0..94);
// 103, 104 and 105 are the start symbols of subsets A, B and C.
//
// This is the form in which the standard (ISO/IEC 15417) and every reference print the table, so
// it can be compared line by line. The host test holds the same table a second time, as bit
// patterns from another source, and compares all 107.
static const uint32_t WIDTHS[107] = {
    212222, 222122, 222221, 121223, 121322, 131222, 122213, 122312, 132212, 221213,   //   0..9
    221312, 231212, 112232, 122132, 122231, 113222, 123122, 123221, 223211, 221132,   //  10..19
    221231, 213212, 223112, 312131, 311222, 321122, 321221, 312212, 322112, 322211,   //  20..29
    212123, 212321, 232121, 111323, 131123, 131321, 112313, 132113, 132311, 211313,   //  30..39
    231113, 231311, 112133, 112331, 132131, 113123, 113321, 133121, 313121, 211331,   //  40..49
    231131, 213113, 213311, 213131, 311123, 311321, 331121, 312113, 312311, 332111,   //  50..59
    314111, 221411, 431111, 111224, 111422, 121124, 121421, 141122, 141221, 112214,   //  60..69
    112412, 122114, 122411, 142112, 142211, 241211, 221114, 413111, 241112, 134111,   //  70..79
    111242, 121142, 121241, 114212, 124112, 124211, 411212, 421112, 421211, 212141,   //  80..89
    214121, 412121, 111143, 111341, 131141, 114113, 114311, 411113, 411311, 113141,   //  90..99
    114131, 311141, 411131, 211412, 211214, 211232,                                   // 100..105
    2331112,                                                                          // 106: stop
};

enum {
  SYMBOL_MODULES = 11,
  STOP_MODULES = 13,
  START_B = 104,
  STOP = 106,
  CHECK_MODULUS = 103,
  FIRST_CHAR = 32,     // value 0 of subset B
  LAST_CHAR = 126,     // value 94
};

// Writes the pattern of `value` at modules[at] and returns the index after it.
static int put(uint8_t *modules, int at, int value) {
  const uint32_t widths = WIDTHS[value];
  uint32_t place = widths >= 1000000u ? 1000000u : 100000u;   // seven elements or six
  uint8_t bar = 1;
  for (; place > 0; place /= 10) {
    int run = (int)((widths / place) % 10);
    while (run-- > 0) modules[at++] = bar;
    bar ^= 1u;
  }
  return at;
}

int vk_code128_width(int text_len) {
  if (text_len < 0) return -1;
  return SYMBOL_MODULES * (text_len + 2) + STOP_MODULES;
}

int vk_code128_encode(const char *text, uint8_t *modules, int cap) {
  if (text == NULL || modules == NULL) return -1;

  // Everything is checked before the first module is written.
  int length = 0;
  for (const unsigned char *p = (const unsigned char *)text; *p != '\0'; ++p) {
    if (*p < FIRST_CHAR || *p > LAST_CHAR) return -1;
    // Stop counting long before the width can overflow an int: no such text fits a real buffer.
    if (++length > (1 << 20)) return -1;
  }
  const int width = vk_code128_width(length);
  if (width > cap) return -1;

  // The check symbol: the start value, plus each character's value times its position (from 1),
  // modulo 103. Reduced at every step, so no text is long enough to overflow the sum.
  uint32_t sum = START_B;
  int at = put(modules, 0, START_B);
  for (int i = 0; i < length; ++i) {
    const uint32_t value = (uint32_t)((unsigned char)text[i] - FIRST_CHAR);
    sum = (sum + value * (((uint32_t)i + 1u) % CHECK_MODULUS)) % CHECK_MODULUS;
    at = put(modules, at, (int)value);
  }
  at = put(modules, at, (int)(sum % CHECK_MODULUS));   // the empty text: 104 mod 103 = 1
  at = put(modules, at, STOP);
  return at;      // == width
}
