// LINK: src/vk/ui/code128.c
// Host test of the Code 128 encoder (src/vk/ui/code128.h). Run: test/host/run.sh test_code128
//
// What is checked:
//   1. the module count: vk_code128_width, and what vk_code128_encode returns;
//   2. the pattern table, all 107 values, bit for bit against REFERENCE below. REFERENCE is the
//      published table as bit patterns (the encoder holds it as element widths, typed separately),
//      and it is also checked for the properties every Code 128 pattern has;
//   3. the start-B, stop and check symbols of whole codes, written out here as literals;
//   4. the check symbol against a second implementation of its definition, and against the worked
//      example every reference gives ("Wikipedia" in subset B has check value 88);
//   5. refusals: a byte outside 32..126, a buffer one byte too small, null arguments; a refusal
//      writes nothing;
//   6. a decoder written here reads the modules of 100 random texts back into the same texts.
#include <stdio.h>
#include <string.h>

#include "../../src/vk/ui/code128.h"

static int failures = 0;

#define CHECK(cond, ...)                              \
  do {                                                \
    if (!(cond)) {                                    \
      ++failures;                                     \
      printf("FAIL %s:%d: ", __FILE__, __LINE__);     \
      printf(__VA_ARGS__);                            \
      printf("\n");                                   \
    }                                                 \
  } while (0)

// Values 0..105 as modules, '1' a bar and '0' a space; 106 is the stop pattern with its final bar.
static const char *const REFERENCE[107] = {
    "11011001100", "11001101100", "11001100110", "10010011000", "10010001100", "10001001100",   // 0
    "10011001000", "10011000100", "10001100100", "11001001000", "11001000100", "11000100100",   // 6
    "10110011100", "10011011100", "10011001110", "10111001100", "10011101100", "10011100110",   // 12
    "11001110010", "11001011100", "11001001110", "11011100100", "11001110100", "11101101110",   // 18
    "11101001100", "11100101100", "11100100110", "11101100100", "11100110100", "11100110010",   // 24
    "11011011000", "11011000110", "11000110110", "10100011000", "10001011000", "10001000110",   // 30
    "10110001000", "10001101000", "10001100010", "11010001000", "11000101000", "11000100010",   // 36
    "10110111000", "10110001110", "10001101110", "10111011000", "10111000110", "10001110110",   // 42
    "11101110110", "11010001110", "11000101110", "11011101000", "11011100010", "11011101110",   // 48
    "11101011000", "11101000110", "11100010110", "11101101000", "11101100010", "11100011010",   // 54
    "11101111010", "11001000010", "11110001010", "10100110000", "10100001100", "10010110000",   // 60
    "10010000110", "10000101100", "10000100110", "10110010000", "10110000100", "10011010000",   // 66
    "10011000010", "10000110100", "10000110010", "11000010010", "11001010000", "11110111010",   // 72
    "11000010100", "10001111010", "10100111100", "10010111100", "10010011110", "10111100100",   // 78
    "10011110100", "10011110010", "11110100100", "11110010100", "11110010010", "11011011110",   // 84
    "11011110110", "11110110110", "10101111000", "10100011110", "10001011110", "10111101000",   // 90
    "10111100010", "11110101000", "11110100010", "10111011110", "10111101110", "11101011110",   // 96
    "11110101110", "11010000100", "11010010000", "11010011100",                                 // 102
    "1100011101011",                                                                            // 106
};

enum { START_B = 104, STOP = 106, MAX_TEXT = 64, MAX_MODULES = 11 * (MAX_TEXT + 2) + 13 };

// ---- helpers ----------------------------------------------------------------------------------

// True when the `count` modules at `modules` are the pattern written as '0' and '1' in `bits`.
static int same(const uint8_t *modules, const char *bits, int count) {
  if ((int)strlen(bits) != count) return 0;
  for (int i = 0; i < count; ++i) {
    if (modules[i] != (uint8_t)(bits[i] - '0')) return 0;
  }
  return 1;
}

// The check value from the definition: (104 + sum of value * position) mod 103, positions from 1.
// Written apart from the encoder: no reduction inside the loop, 64-bit sum.
static int check_value(const char *text) {
  unsigned long long sum = START_B;
  for (size_t i = 0; text[i] != '\0'; ++i) {
    sum += (unsigned long long)((unsigned char)text[i] - 32) * (unsigned long long)(i + 1);
  }
  return (int)(sum % 103u);
}

// The value whose pattern is at `modules` (11 modules), or -1.
static int value_at(const uint8_t *modules) {
  for (int value = 0; value < STOP; ++value) {
    if (same(modules, REFERENCE[value], 11)) return value;
  }
  return -1;
}

// Reads a subset-B code back into text, as a scanner does: start B, symbols of 11 modules, the
// check symbol, the 13-module stop. Returns the text length, or a negative number naming what is
// wrong.
static int decode(const uint8_t *modules, int count, char *out, int cap) {
  if (count < 11 + 11 + 13 || (count - 13) % 11 != 0) return -1;
  for (int i = 0; i < count; ++i) {
    if (modules[i] > 1) return -2;
  }
  if (value_at(modules) != START_B) return -3;
  if (!same(modules + count - 13, REFERENCE[STOP], 13)) return -4;

  const int symbols = (count - 13) / 11 - 2;       // without the start and the check symbol
  if (symbols + 1 > cap) return -5;
  unsigned long long sum = START_B;
  for (int i = 0; i < symbols; ++i) {
    const int value = value_at(modules + 11 * (i + 1));
    if (value < 0 || value > 94) return -6;        // 95..102 are functions and shifts: not text
    out[i] = (char)(value + 32);
    sum += (unsigned long long)value * (unsigned long long)(i + 1);
  }
  out[symbols] = '\0';
  if (value_at(modules + 11 * (symbols + 1)) != (int)(sum % 103u)) return -7;
  return symbols;
}

// A small generator with a fixed seed: the same 100 texts on every run.
static unsigned long rng_state = 0x1D2C3B4Aul;
static unsigned rng(void) {
  rng_state = (rng_state * 1103515245ul + 12345ul) & 0x7FFFFFFFul;
  return (unsigned)(rng_state >> 8);
}

// ---- 1. module count --------------------------------------------------------------------------
static void test_width(void) {
  CHECK(vk_code128_width(0) == 35, "width(0) = %d", vk_code128_width(0));
  CHECK(vk_code128_width(1) == 46, "width(1) = %d", vk_code128_width(1));
  CHECK(vk_code128_width(8) == 123, "width(8) = %d", vk_code128_width(8));
  CHECK(vk_code128_width(9) == 134, "width(9) = %d", vk_code128_width(9));
  CHECK(vk_code128_width(-1) == -1, "width(-1) = %d", vk_code128_width(-1));
  for (int n = 0; n <= MAX_TEXT; ++n) {
    CHECK(vk_code128_width(n) == 11 * (n + 2) + 13, "width(%d) = %d", n, vk_code128_width(n));
  }

  // A badge ID: the first 8 characters of a base58 address. 123 modules, 143 with the quiet
  // zones, which is 286 px at 2 px per module: it fits the 320 px screen.
  uint8_t modules[MAX_MODULES];
  const int count = vk_code128_encode("7xKXtg2C", modules, (int)sizeof modules);
  CHECK(count == 123, "an 8 character text gave %d modules", count);
  CHECK((count + 2 * VK_CODE128_QUIET) * 2 <= 320, "8 characters at 2 px do not fit 320 px");
  CHECK(modules[0] == 1 && modules[count - 1] == 1, "a code must begin and end with a bar");
}

// ---- 2. the pattern table ---------------------------------------------------------------------
// The reference itself first: every pattern of 0..105 is 11 modules of three bars and three
// spaces, begins with a bar, ends with a space, and has an even number of bar modules; no two
// patterns are the same. A mistyped reference row would break one of these.
static void test_reference(void) {
  for (int value = 0; value < STOP; ++value) {
    const char *bits = REFERENCE[value];
    CHECK(strlen(bits) == 11, "reference %d is %d modules", value, (int)strlen(bits));
    if (strlen(bits) != 11) continue;
    int runs = 1, dark = 0;
    for (int i = 0; i < 11; ++i) {
      if (bits[i] == '1') ++dark;
      if (i > 0 && bits[i] != bits[i - 1]) ++runs;
    }
    CHECK(bits[0] == '1' && bits[10] == '0', "reference %d must begin with a bar and end with a space", value);
    CHECK(runs == 6, "reference %d has %d elements", value, runs);
    CHECK(dark % 2 == 0, "reference %d has %d bar modules", value, dark);
    for (int other = 0; other < value; ++other) {
      CHECK(strcmp(bits, REFERENCE[other]) != 0, "reference %d equals reference %d", value, other);
    }
  }
  CHECK(strcmp(REFERENCE[STOP], "1100011101011") == 0, "the reference stop pattern");
}

// Every value the encoder can emit, against the reference. A character c gives value c - 32
// (0..94); the check symbol reaches every value 0..102 with a suitable text; start B is 104 and
// the stop 106. (103 and 105, the starts of subsets A and C, are never emitted.)
static void test_table(void) {
  uint8_t modules[MAX_MODULES];

  // Characters: one text of all 95, and each character alone.
  char all[96];
  for (int c = 32; c <= 126; ++c) all[c - 32] = (char)c;
  all[95] = '\0';
  uint8_t wide[11 * (95 + 2) + 13];
  int count = vk_code128_encode(all, wide, (int)sizeof wide);
  CHECK(count == (int)sizeof wide, "all 95 characters gave %d modules", count);
  if (count == (int)sizeof wide) {
    for (int value = 0; value <= 94; ++value) {
      CHECK(same(wide + 11 * (value + 1), REFERENCE[value], 11), "pattern of value %d (character %d)", value, value + 32);
    }
    CHECK(same(wide, REFERENCE[START_B], 11), "start B of the long text");
    CHECK(same(wide + count - 13, REFERENCE[STOP], 13), "stop of the long text");
  }

  // Check symbols: for each value 0..102 find a two-character text whose check value it is, and
  // compare the check symbol's modules. This covers 95..102, which no character reaches.
  int seen[103] = {0};
  for (int a = 32; a <= 126; ++a) {
    for (int b = 32; b <= 126; ++b) {
      const char text[3] = {(char)a, (char)b, '\0'};
      const int want = check_value(text);
      if (seen[want]) continue;
      seen[want] = 1;
      count = vk_code128_encode(text, modules, (int)sizeof modules);
      CHECK(count == 57, "a 2 character text gave %d modules", count);
      CHECK(same(modules + 33, REFERENCE[want], 11), "check symbol %d of \"%s\"", want, text);
    }
  }
  for (int value = 0; value < 103; ++value) CHECK(seen[value], "no text found with check value %d", value);
}

// ---- 3. whole codes, written out --------------------------------------------------------------
static void test_known(void) {
  uint8_t modules[MAX_MODULES];

  // The empty text: start B (104), check 104 mod 103 = 1, stop.
  int count = vk_code128_encode("", modules, (int)sizeof modules);
  CHECK(count == 35, "the empty text gave %d modules", count);
  CHECK(same(modules, "11010010000" "11001101100" "1100011101011", 35), "the empty text's modules");

  // "A": value 33, check (104 + 33) mod 103 = 34.
  count = vk_code128_encode("A", modules, (int)sizeof modules);
  CHECK(count == 46, "\"A\" gave %d modules", count);
  CHECK(same(modules, "11010010000" "10100011000" "10001011000" "1100011101011", 46), "the modules of \"A\"");

  // " !": values 0 and 1, check (104 + 0 * 1 + 1 * 2) mod 103 = 3.
  count = vk_code128_encode(" !", modules, (int)sizeof modules);
  CHECK(count == 57, "\" !\" gave %d modules", count);
  CHECK(same(modules, "11010010000" "11011001100" "11001101100" "10010011000" "1100011101011", 57), "the modules of \" !\"");

  // "~": value 94, the last character; check (104 + 94) mod 103 = 95.
  count = vk_code128_encode("~", modules, (int)sizeof modules);
  CHECK(count == 46, "\"~\" gave %d modules", count);
  CHECK(same(modules, "11010010000" "10001011110" "10111101000" "1100011101011", 46), "the modules of \"~\"");
}

// ---- 4. the check symbol ----------------------------------------------------------------------
static void test_checksum(void) {
  uint8_t modules[MAX_MODULES];

  // The worked example: 104 + 55*1 + 73*2 + 75*3 + 73*4 + 80*5 + 69*6 + 68*7 + 73*8 + 65*9
  // = 3281 = 31 * 103 + 88.
  const int sum = 104 + 55 * 1 + 73 * 2 + 75 * 3 + 73 * 4 + 80 * 5 + 69 * 6 + 68 * 7 + 73 * 8 + 65 * 9;
  CHECK(sum == 3281 && sum % 103 == 88, "the worked example sums to %d, mod 103 = %d", sum, sum % 103);
  CHECK(check_value("Wikipedia") == 88, "check_value(\"Wikipedia\") = %d", check_value("Wikipedia"));
  int count = vk_code128_encode("Wikipedia", modules, (int)sizeof modules);
  CHECK(count == 134, "\"Wikipedia\" gave %d modules", count);
  CHECK(same(modules + 11 * 10, "11110010010", 11), "the check symbol of \"Wikipedia\" is not value 88");
  CHECK(same(modules + 11 * 1, "11101000110", 11), "the first symbol of \"Wikipedia\" is not W (value 55)");

  // A long text: positions past 103 and a sum past 16 bits.
  char text[MAX_TEXT + 1];
  memset(text, '~', MAX_TEXT);
  text[MAX_TEXT] = '\0';
  count = vk_code128_encode(text, modules, (int)sizeof modules);
  CHECK(count == MAX_MODULES, "%d characters gave %d modules", MAX_TEXT, count);
  CHECK(same(modules + 11 * (MAX_TEXT + 1), REFERENCE[check_value(text)], 11), "the check symbol of a long text");
}

// ---- 5. refusals ------------------------------------------------------------------------------
static int untouched(const uint8_t *modules, int count) {
  for (int i = 0; i < count; ++i) {
    if (modules[i] != 0xEE) return 0;
  }
  return 1;
}

static void test_refusals(void) {
  uint8_t modules[MAX_MODULES];
  static const char *const BAD[] = {
      "caf\xC3\xA9",      // UTF-8: two bytes above 126
      "tab\there",        // a control character in the middle
      "\x1F",             // one below the first character
      "\x7F",             // DEL, one above the last
      "ABC\x80",          // the first byte with the high bit set
      "line\n",
  };
  for (size_t i = 0; i < sizeof BAD / sizeof BAD[0]; ++i) {
    memset(modules, 0xEE, sizeof modules);
    CHECK(vk_code128_encode(BAD[i], modules, (int)sizeof modules) == -1, "bad text %d was encoded", (int)i);
    CHECK(untouched(modules, (int)sizeof modules), "bad text %d wrote modules", (int)i);
  }

  // The edges of the range are accepted.
  CHECK(vk_code128_encode(" ", modules, (int)sizeof modules) == 46, "a space was refused");
  CHECK(vk_code128_encode("~", modules, (int)sizeof modules) == 46, "a tilde was refused");

  // A buffer one byte too small, then exactly large enough.
  memset(modules, 0xEE, sizeof modules);
  CHECK(vk_code128_encode("7xKXtg2C", modules, 122) == -1, "8 characters fitted 122 bytes");
  CHECK(untouched(modules, (int)sizeof modules), "a refused encode wrote modules");
  CHECK(vk_code128_encode("7xKXtg2C", modules, 123) == 123, "8 characters did not fit 123 bytes");
  CHECK(modules[123] == 0xEE, "the encoder wrote past the code");
  CHECK(vk_code128_encode("", modules, 34) == -1, "the empty text fitted 34 bytes");
  CHECK(vk_code128_encode("A", modules, 0) == -1, "a text fitted 0 bytes");
  CHECK(vk_code128_encode("A", modules, -5) == -1, "a text fitted a negative size");

  CHECK(vk_code128_encode(NULL, modules, (int)sizeof modules) == -1, "a null text was encoded");
  CHECK(vk_code128_encode("A", NULL, 100) == -1, "a null buffer was accepted");
}

// ---- 6. round trip ----------------------------------------------------------------------------
static void test_round_trip(void) {
  uint8_t modules[MAX_MODULES + 1];
  char text[MAX_TEXT + 1], back[MAX_TEXT + 1];

  for (int round = 0; round < 100; ++round) {
    const int length = (int)(rng() % (MAX_TEXT + 1));       // 0..64
    for (int i = 0; i < length; ++i) text[i] = (char)(32 + rng() % 95);
    text[length] = '\0';

    modules[MAX_MODULES] = 0xEE;
    const int count = vk_code128_encode(text, modules, MAX_MODULES);
    CHECK(count == vk_code128_width(length), "round %d: %d modules for %d characters", round, count, length);
    if (count < 0) continue;
    CHECK(modules[MAX_MODULES] == 0xEE, "round %d: wrote past the buffer", round);

    const int got = decode(modules, count, back, (int)sizeof back);
    CHECK(got == length, "round %d: decode returned %d for \"%s\"", round, got, text);
    if (got == length) CHECK(strcmp(back, text) == 0, "round %d: \"%s\" came back as \"%s\"", round, text, back);

    // One flipped module must not decode to the same text.
    if (count > 0) {
      const int at = (int)(rng() % (unsigned)count);
      modules[at] ^= 1u;
      const int again = decode(modules, count, back, (int)sizeof back);
      CHECK(again < 0 || strcmp(back, text) != 0, "round %d: a flipped module at %d was not noticed", round, at);
    }
  }
}

int main(void) {
  test_width();
  test_reference();
  test_table();
  test_known();
  test_checksum();
  test_refusals();
  test_round_trip();

  if (failures != 0) {
    printf("%d code128 checks failed\n", failures);
    return 1;
  }
  printf("all code128 tests passed\n");
  return 0;
}
