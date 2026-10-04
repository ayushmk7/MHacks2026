// LINK: src/vk/ui/keyboard_core.c
// Host test of the on-screen keyboard's logic (src/vk/ui/keyboard_core.h; docs/os/ui/text-entry.md).
// Run: test/host/run.sh test_keyboard
//
// What is checked:
//   1. the layout: every printable ASCII character, 32..126, is on a key; vk_kb_find and vk_kb_key
//      agree; no layer holds a character twice; the two letter layers differ only in case; the
//      action row is the same on every layer;
//   2. the cursor: one step in each direction, the wrap at all four edges, the action row one step
//      above the first row and one step below the last, the wide space key as one stop;
//   3. the layers: each layer key switches to its layer and leaves the cursor where it is;
//   4. the text: typing, the space, backspace down to empty, the case change of a held SELECT
//      (once, letters only, not after another edit), bytes outside 32..126 refused;
//   5. the limits: nothing is added at max_len, DONE is refused under min_len, VK_KB_EMPTY_OK, the
//      values vk_kb_init corrects, an initial text cut at max_len and at its first bad byte;
//   6. show/hide on a secret field; the wipe leaves no byte of the text;
//   7. a string that uses all four layers, typed with nothing but steps and SELECT, the way the
//      device test types it;
//   8. the figure the layout is chosen for: the mean number of steps between two character keys.
#include <stdio.h>
#include <string.h>

#include "../../src/vk/ui/keyboard_core.h"

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

// ---- helpers ------------------------------------------------------------------------------------

static int torus(int a, int b, int count) {
  const int d = a > b ? a - b : b - a;
  return d < count - d ? d : count - d;
}

// Moves the cursor to (row, col) by single steps, the short way round. Returns the steps taken.
static int walk_to(vk_kb_t *kb, int row, int col) {
  int steps = 0;
  while (kb->row != row) {
    const int down = (row - kb->row + VK_KB_ROWS) % VK_KB_ROWS;
    vk_kb_move(kb, 0, down <= VK_KB_ROWS - down ? 1 : -1);
    if (++steps > 64) return -1;
  }
  while (kb->col != col) {
    int first, last;
    vk_kb_action_span(kb, kb->col, &first, &last);
    if (kb->row == VK_KB_ACTION_ROW && col >= first && col <= last) break;   // already on that key
    const int right = (col - kb->col + VK_KB_COLS) % VK_KB_COLS;
    vk_kb_move(kb, right <= VK_KB_COLS - right ? 1 : -1, 0);
    if (++steps > 64) return -1;
  }
  return steps;
}

// Types `ch` as a person would: to the layer key if the character is on another layer, SELECT, to
// the character, SELECT. Returns what the last SELECT returned.
static int type_char(vk_kb_t *kb, char ch) {
  int layer, row, col;
  if (!vk_kb_find(ch, &layer, &row, &col)) return -1;
  // A character on the layer that is showing needs no switch (the common symbols are on two).
  int on_this_layer = row == VK_KB_ACTION_ROW;
  for (int r = 0; r < VK_KB_CHAR_ROWS && !on_this_layer; ++r) {
    for (int c = 0; c < VK_KB_COLS && !on_this_layer; ++c) {
      if (vk_kb_key(kb, r, c).ch == ch) { on_this_layer = 1; row = r; col = c; }
    }
  }
  if (!on_this_layer) {
    if (walk_to(kb, VK_KB_ACTION_ROW, layer) < 0) return -1;
    if (vk_kb_select(kb) != VK_KB_LAYER_CHANGED) return -1;
  }
  if (walk_to(kb, row, col) < 0) return -1;
  return vk_kb_select(kb);
}

// ---- 1: the layout ------------------------------------------------------------------------------

static void test_layout(void) {
  vk_kb_t kb;
  vk_kb_init(&kb, NULL, 0, 63, VK_KB_SECRET);

  for (int ch = 32; ch <= 126; ++ch) {
    int layer = -1, row = -1, col = -1;
    CHECK(vk_kb_find((char)ch, &layer, &row, &col), "character %d ('%c') is on no key", ch, ch);
    if (layer < 0) continue;
    kb.layer = (uint8_t)layer;
    const vk_kb_key_t key = vk_kb_key(&kb, row, col);
    CHECK(key.ch == (char)ch, "find says '%c' is at layer %d (%d,%d); the key there is '%c'", ch, layer, row, col, key.ch);
    CHECK(key.kind == (ch == ' ' ? VK_KB_KEY_SPACE : VK_KB_KEY_CHAR), "'%c' has kind %d", ch, key.kind);
  }
  CHECK(!vk_kb_find(31, NULL, NULL, NULL), "31 was found");
  CHECK(!vk_kb_find(127, NULL, NULL, NULL), "127 was found");
  CHECK(!vk_kb_find('\0', NULL, NULL, NULL), "NUL was found");
  CHECK(!vk_kb_find((char)0xE9, NULL, NULL, NULL), "0xE9 was found");
  CHECK(vk_kb_find('a', NULL, NULL, NULL), "find with null outputs");

  for (int layer = 0; layer < VK_KB_LAYER_COUNT; ++layer) {
    kb.layer = (uint8_t)layer;
    int seen[128] = {0};
    for (int row = 0; row < VK_KB_CHAR_ROWS; ++row) {
      for (int col = 0; col < VK_KB_COLS; ++col) {
        const vk_kb_key_t key = vk_kb_key(&kb, row, col);
        CHECK(key.kind == VK_KB_KEY_CHAR, "layer %d (%d,%d) is not a character key", layer, row, col);
        CHECK(key.ch > 32 && key.ch <= 126, "layer %d (%d,%d) holds byte %d", layer, row, col, key.ch);
        if (key.ch > 32 && key.ch <= 126) CHECK(seen[(int)key.ch]++ == 0, "layer %d holds '%c' twice", layer, key.ch);
      }
    }
    // The action row does not depend on the layer.
    for (int col = 0; col < VK_KB_LAYER_COUNT; ++col) {
      const vk_kb_key_t key = vk_kb_key(&kb, VK_KB_ACTION_ROW, col);
      CHECK(key.kind == VK_KB_KEY_LAYER && key.layer == col, "layer %d: action key %d is kind %d layer %d", layer, col, key.kind, key.layer);
    }
    CHECK(vk_kb_key(&kb, VK_KB_ACTION_ROW, 4).kind == VK_KB_KEY_SPACE, "layer %d: no space key", layer);
    CHECK(vk_kb_key(&kb, VK_KB_ACTION_ROW, 5).kind == VK_KB_KEY_SHOW, "layer %d: no show key", layer);
    CHECK(vk_kb_key(&kb, VK_KB_ACTION_ROW, 6).kind == VK_KB_KEY_DONE, "layer %d: no DONE key", layer);
  }

  // Lower and upper case are the same layout.
  for (int row = 0; row < VK_KB_CHAR_ROWS; ++row) {
    for (int col = 0; col < VK_KB_COLS; ++col) {
      kb.layer = VK_KB_LOWER;
      const char lower = vk_kb_key(&kb, row, col).ch;
      kb.layer = VK_KB_UPPER;
      const char upper = vk_kb_key(&kb, row, col).ch;
      const char expect = (lower >= 'a' && lower <= 'z') ? (char)(lower - 'a' + 'A') : lower;
      CHECK(upper == expect, "(%d,%d): '%c' on abc, '%c' on ABC", row, col, lower, upper);
    }
  }
  // The alphabet in order: nobody has to learn where a letter is.
  kb.layer = VK_KB_LOWER;
  for (int i = 0; i < 26; ++i) {
    CHECK(vk_kb_key(&kb, i / VK_KB_COLS, i % VK_KB_COLS).ch == (char)('a' + i), "letter %d is out of order", i);
  }
  // The two lower rows of the digit layer and the symbol layer are the same.
  for (int row = 2; row < VK_KB_CHAR_ROWS; ++row) {
    for (int col = 0; col < VK_KB_COLS; ++col) {
      kb.layer = VK_KB_DIGITS;
      const char a = vk_kb_key(&kb, row, col).ch;
      kb.layer = VK_KB_SYMBOLS;
      CHECK(vk_kb_key(&kb, row, col).ch == a, "(%d,%d) differs between 123 and #+=", row, col);
    }
  }

  CHECK(strcmp(vk_kb_layer_name(VK_KB_LOWER), "abc") == 0, "name of the lower layer");
  CHECK(strcmp(vk_kb_layer_name(VK_KB_UPPER), "ABC") == 0, "name of the upper layer");
  CHECK(strcmp(vk_kb_layer_name(VK_KB_DIGITS), "123") == 0, "name of the digit layer");
  CHECK(strcmp(vk_kb_layer_name(VK_KB_SYMBOLS), "#+=") == 0, "name of the symbol layer");
  CHECK(strcmp(vk_kb_layer_name(-1), "") == 0 && strcmp(vk_kb_layer_name(VK_KB_LAYER_COUNT), "") == 0, "name of no layer");

  // Out of range: a key with no character, never a read outside the tables.
  CHECK(vk_kb_key(&kb, -1, 0).ch == 0 && vk_kb_key(&kb, VK_KB_ROWS, 0).ch == 0, "row out of range");
  CHECK(vk_kb_key(&kb, 0, -1).ch == 0 && vk_kb_key(&kb, 0, VK_KB_COLS).ch == 0, "column out of range");
}

// ---- 2: the cursor ------------------------------------------------------------------------------

static void test_cursor(void) {
  vk_kb_t kb;
  vk_kb_init(&kb, NULL, 0, 63, VK_KB_SECRET);
  CHECK(kb.layer == VK_KB_LOWER && kb.row == 0 && kb.col == 0, "start at layer %d (%d,%d)", kb.layer, kb.row, kb.col);
  CHECK(vk_kb_current(&kb).ch == 'a', "the first key is '%c'", vk_kb_current(&kb).ch);

  vk_kb_move(&kb, 1, 0);
  CHECK(kb.row == 0 && kb.col == 1 && vk_kb_current(&kb).ch == 'b', "RIGHT");
  vk_kb_move(&kb, 0, 1);
  CHECK(kb.row == 1 && kb.col == 1 && vk_kb_current(&kb).ch == 'i', "DOWN");
  vk_kb_move(&kb, -1, 0);
  CHECK(kb.row == 1 && kb.col == 0 && vk_kb_current(&kb).ch == 'h', "LEFT");
  vk_kb_move(&kb, 0, -1);
  CHECK(kb.row == 0 && kb.col == 0, "UP");
  vk_kb_move(&kb, 0, 0);
  CHECK(kb.row == 0 && kb.col == 0, "no step");

  // Wrap, horizontally: LEFT from the first column is the last column; a full turn comes back.
  vk_kb_move(&kb, -1, 0);
  CHECK(kb.row == 0 && kb.col == VK_KB_COLS - 1 && vk_kb_current(&kb).ch == 'g', "LEFT from column 0");
  vk_kb_move(&kb, 1, 0);
  CHECK(kb.col == 0, "RIGHT from the last column");
  for (int i = 0; i < VK_KB_COLS; ++i) vk_kb_move(&kb, 1, 0);
  CHECK(kb.row == 0 && kb.col == 0, "seven steps RIGHT do not come back");

  // Wrap, vertically: UP from the first row is the action row; DOWN from it is the first row.
  vk_kb_move(&kb, 0, -1);
  CHECK(kb.row == VK_KB_ACTION_ROW && kb.col == 0, "UP from row 0");
  CHECK(vk_kb_current(&kb).kind == VK_KB_KEY_LAYER && vk_kb_current(&kb).layer == VK_KB_LOWER, "the key above 'a'");
  vk_kb_move(&kb, 0, 1);
  CHECK(kb.row == 0 && kb.col == 0, "DOWN from the action row");
  for (int i = 0; i < VK_KB_ROWS; ++i) vk_kb_move(&kb, 0, 1);
  CHECK(kb.row == 0 && kb.col == 0, "five steps DOWN do not come back");
  for (int i = 0; i < VK_KB_CHAR_ROWS; ++i) vk_kb_move(&kb, 0, 1);
  CHECK(kb.row == VK_KB_ACTION_ROW, "four steps DOWN from row 0");

  // A column keeps its place through the action row.
  vk_kb_init(&kb, NULL, 0, 63, VK_KB_SECRET);
  walk_to(&kb, 3, 6);
  CHECK(vk_kb_current(&kb).ch == '-', "(3,6) is '%c'", vk_kb_current(&kb).ch);
  vk_kb_move(&kb, 0, 1);
  CHECK(kb.row == VK_KB_ACTION_ROW && kb.col == 6 && vk_kb_current(&kb).kind == VK_KB_KEY_DONE, "DOWN from (3,6)");
  vk_kb_move(&kb, 0, 1);
  CHECK(kb.row == 0 && kb.col == 6 && vk_kb_current(&kb).ch == 'g', "DOWN from DONE");

  // A secret field: seven one-column keys on the action row.
  vk_kb_init(&kb, NULL, 0, 63, VK_KB_SECRET);
  vk_kb_move(&kb, 0, -1);
  {
    const int kinds[VK_KB_COLS] = {VK_KB_KEY_LAYER, VK_KB_KEY_LAYER, VK_KB_KEY_LAYER, VK_KB_KEY_LAYER,
                                   VK_KB_KEY_SPACE, VK_KB_KEY_SHOW, VK_KB_KEY_DONE};
    for (int col = 0; col < VK_KB_COLS; ++col) {
      int first = -1, last = -1;
      vk_kb_action_span(&kb, col, &first, &last);
      CHECK(first == col && last == col, "secret: key %d spans %d..%d", col, first, last);
      CHECK(kb.col == col && vk_kb_current(&kb).kind == kinds[col], "secret: action key %d is kind %d", col, vk_kb_current(&kb).kind);
      vk_kb_move(&kb, 1, 0);
    }
    CHECK(kb.col == 0, "secret: the action row does not wrap");
  }

  // A plain field: the space key covers columns 4 and 5 and is one stop in both directions.
  vk_kb_init(&kb, NULL, 0, 32, 0);
  {
    int first = -1, last = -1;
    vk_kb_action_span(&kb, 4, &first, &last);
    CHECK(first == 4 && last == 5, "plain: the space key spans %d..%d from column 4", first, last);
    vk_kb_action_span(&kb, 5, &first, &last);
    CHECK(first == 4 && last == 5, "plain: the space key spans %d..%d from column 5", first, last);
    vk_kb_action_span(&kb, 6, &first, &last);
    CHECK(first == 6 && last == 6, "plain: DONE spans %d..%d", first, last);
    vk_kb_action_span(&kb, 3, NULL, NULL);
  }
  CHECK(vk_kb_key(&kb, VK_KB_ACTION_ROW, 5).kind == VK_KB_KEY_SPACE, "plain: column 5 is not the space key");
  walk_to(&kb, VK_KB_ACTION_ROW, 3);
  vk_kb_move(&kb, 1, 0);
  CHECK(kb.col == 4 && vk_kb_current(&kb).kind == VK_KB_KEY_SPACE, "plain: RIGHT from #+=");
  vk_kb_move(&kb, 1, 0);
  CHECK(kb.col == 6 && vk_kb_current(&kb).kind == VK_KB_KEY_DONE, "plain: RIGHT from space lands on column %d", kb.col);
  vk_kb_move(&kb, -1, 0);
  CHECK(kb.col == 5 && vk_kb_current(&kb).kind == VK_KB_KEY_SPACE, "plain: LEFT from DONE lands on column %d", kb.col);
  vk_kb_move(&kb, -1, 0);
  CHECK(kb.col == 3 && vk_kb_current(&kb).kind == VK_KB_KEY_LAYER, "plain: LEFT from space lands on column %d", kb.col);
  // Coming down column 5 lands on the space key, and going back up keeps column 5.
  walk_to(&kb, 3, 5);
  vk_kb_move(&kb, 0, 1);
  CHECK(kb.row == VK_KB_ACTION_ROW && kb.col == 5 && vk_kb_current(&kb).kind == VK_KB_KEY_SPACE, "plain: DOWN from (3,5)");
  vk_kb_move(&kb, 0, -1);
  CHECK(kb.row == 3 && kb.col == 5, "plain: UP from the space key");

  // Null is ignored everywhere.
  vk_kb_move(NULL, 1, 1);
  CHECK(vk_kb_select(NULL) == VK_KB_NONE, "select on null");
  CHECK(vk_kb_hold(NULL) == VK_KB_NONE, "hold on null");
  CHECK(vk_kb_backspace(NULL) == VK_KB_EMPTY, "backspace on null");
  CHECK(vk_kb_type(NULL, 'a') == VK_KB_NONE, "type on null");
  CHECK(vk_kb_acceptable(NULL) == 0, "acceptable on null");
  vk_kb_init(NULL, "x", 0, 8, 0);
  vk_kb_wipe(NULL);
}

// ---- 3: the layers ------------------------------------------------------------------------------

static void test_layers(void) {
  vk_kb_t kb;
  vk_kb_init(&kb, NULL, 0, 63, VK_KB_SECRET);
  const char first_key[VK_KB_LAYER_COUNT] = {'a', 'A', '1', '"'};

  for (int layer = VK_KB_LAYER_COUNT - 1; layer >= 0; --layer) {
    walk_to(&kb, VK_KB_ACTION_ROW, layer);
    const int expect = layer == kb.layer ? VK_KB_NONE : VK_KB_LAYER_CHANGED;
    CHECK(vk_kb_select(&kb) == expect, "select on the key of layer %d", layer);
    CHECK(kb.layer == layer, "the layer is %d, not %d", kb.layer, layer);
    CHECK(kb.row == VK_KB_ACTION_ROW && kb.col == layer, "the cursor left the layer key: (%d,%d)", kb.row, kb.col);
    CHECK(kb.len == 0, "a layer key typed something");
    CHECK(vk_kb_key(&kb, 0, 0).ch == first_key[layer], "layer %d starts with '%c'", layer, vk_kb_key(&kb, 0, 0).ch);
    CHECK(vk_kb_select(&kb) == VK_KB_NONE, "the key of the layer that is showing");
  }
  // One step DOWN from a layer key is the first character row: the digits are next to their key.
  walk_to(&kb, VK_KB_ACTION_ROW, VK_KB_DIGITS);
  vk_kb_select(&kb);
  vk_kb_move(&kb, 0, 1);
  CHECK(vk_kb_current(&kb).ch == '3', "DOWN from the 123 key is '%c'", vk_kb_current(&kb).ch);
}

// ---- 4: the text --------------------------------------------------------------------------------

static void test_text(void) {
  vk_kb_t kb;
  vk_kb_init(&kb, NULL, 0, 63, 0);
  CHECK(kb.len == 0 && kb.text[0] == '\0', "a new field is not empty");
  CHECK(vk_kb_backspace(&kb) == VK_KB_EMPTY, "backspace on an empty field");

  CHECK(vk_kb_select(&kb) == VK_KB_TYPED && strcmp(kb.text, "a") == 0, "select on 'a' gave \"%s\"", kb.text);
  // SELECT held: the letter changes case, once.
  CHECK(vk_kb_hold(&kb) == VK_KB_TYPED && strcmp(kb.text, "A") == 0, "hold gave \"%s\"", kb.text);
  CHECK(vk_kb_hold(&kb) == VK_KB_NONE && strcmp(kb.text, "A") == 0, "a second hold gave \"%s\"", kb.text);
  // The same from the upper layer, the other way.
  kb.layer = VK_KB_UPPER;
  vk_kb_move(&kb, 1, 0);
  CHECK(vk_kb_select(&kb) == VK_KB_TYPED && strcmp(kb.text, "AB") == 0, "select on 'B' gave \"%s\"", kb.text);
  CHECK(vk_kb_hold(&kb) == VK_KB_TYPED && strcmp(kb.text, "Ab") == 0, "hold on 'B' gave \"%s\"", kb.text);
  // Not after a move away and another edit, and never for a character that has no case.
  CHECK(vk_kb_select(&kb) == VK_KB_TYPED, "select");
  CHECK(vk_kb_backspace(&kb) == VK_KB_ERASED && strcmp(kb.text, "Ab") == 0, "backspace gave \"%s\"", kb.text);
  CHECK(vk_kb_hold(&kb) == VK_KB_NONE && strcmp(kb.text, "Ab") == 0, "hold after backspace gave \"%s\"", kb.text);
  kb.layer = VK_KB_DIGITS;
  CHECK(vk_kb_select(&kb) == VK_KB_TYPED && strcmp(kb.text, "Ab2") == 0, "select on '2' gave \"%s\"", kb.text);
  CHECK(vk_kb_hold(&kb) == VK_KB_NONE && strcmp(kb.text, "Ab2") == 0, "hold on a digit gave \"%s\"", kb.text);
  // A letter typed, then a layer key: the hold belongs to the layer key and changes nothing.
  kb.layer = VK_KB_LOWER;
  CHECK(vk_kb_select(&kb) == VK_KB_TYPED && strcmp(kb.text, "Ab2b") == 0, "select gave \"%s\"", kb.text);
  walk_to(&kb, VK_KB_ACTION_ROW, VK_KB_LOWER);
  CHECK(vk_kb_select(&kb) == VK_KB_NONE, "the abc key on the abc layer");
  CHECK(vk_kb_hold(&kb) == VK_KB_NONE && strcmp(kb.text, "Ab2b") == 0, "hold on a layer key gave \"%s\"", kb.text);

  // The space key, then backspace down to nothing.
  walk_to(&kb, VK_KB_ACTION_ROW, 4);
  CHECK(vk_kb_select(&kb) == VK_KB_TYPED && strcmp(kb.text, "Ab2b ") == 0, "the space key gave \"%s\"", kb.text);
  CHECK(vk_kb_hold(&kb) == VK_KB_NONE, "hold on the space key");
  for (int left = 4; left >= 0; --left) {
    CHECK(vk_kb_backspace(&kb) == VK_KB_ERASED && kb.len == left && kb.text[left] == '\0', "backspace to %d", left);
  }
  CHECK(vk_kb_backspace(&kb) == VK_KB_EMPTY && kb.len == 0, "backspace past the start");

  // vk_kb_type: any printable character, wherever the cursor is; nothing else.
  CHECK(vk_kb_type(&kb, '~') == VK_KB_TYPED && vk_kb_type(&kb, ' ') == VK_KB_TYPED, "type");
  CHECK(vk_kb_type(&kb, '\n') == VK_KB_NONE && vk_kb_type(&kb, 127) == VK_KB_NONE, "a control byte was typed");
  CHECK(vk_kb_type(&kb, (char)0xC3) == VK_KB_NONE && vk_kb_type(&kb, '\0') == VK_KB_NONE, "a byte outside ASCII was typed");
  CHECK(strcmp(kb.text, "~ ") == 0 && kb.len == 2, "type gave \"%s\"", kb.text);
  CHECK(vk_kb_hold(&kb) == VK_KB_NONE, "hold after vk_kb_type");
}

// ---- 5: the limits ------------------------------------------------------------------------------

static void test_limits(void) {
  vk_kb_t kb;

  // WPA2: 8 to 63.
  vk_kb_init(&kb, NULL, 8, 63, VK_KB_SECRET);
  CHECK(!vk_kb_acceptable(&kb), "an empty password is acceptable");
  walk_to(&kb, VK_KB_ACTION_ROW, 6);
  for (int i = 0; i < 7; ++i) {
    CHECK(vk_kb_select(&kb) == VK_KB_TOO_SHORT, "DONE with %d characters", i);
    CHECK(vk_kb_type(&kb, 'x') == VK_KB_TYPED, "type");
  }
  CHECK(vk_kb_select(&kb) == VK_KB_TOO_SHORT && kb.len == 7, "DONE with 7 characters");
  vk_kb_type(&kb, 'x');
  CHECK(vk_kb_acceptable(&kb) && vk_kb_select(&kb) == VK_KB_ACCEPT, "DONE with 8 characters");
  CHECK(kb.len == 8, "DONE changed the text");
  for (int i = 0; i < 200 && kb.len < 63; ++i) vk_kb_type(&kb, 'y');
  CHECK(vk_kb_type(&kb, 'z') == VK_KB_FULL && kb.len == 63 && kb.text[62] == 'y' && kb.text[63] == '\0', "a 64th character");
  walk_to(&kb, 0, 0);
  CHECK(vk_kb_select(&kb) == VK_KB_FULL && kb.len == 63, "select on a full field");
  CHECK(vk_kb_hold(&kb) == VK_KB_NONE && kb.text[62] == 'y', "hold after a refused character changed the text");
  walk_to(&kb, VK_KB_ACTION_ROW, 6);
  CHECK(vk_kb_select(&kb) == VK_KB_ACCEPT, "DONE with 63 characters");

  // A hidden network's password: empty (an open network) or 8 and more.
  vk_kb_init(&kb, NULL, 8, 63, VK_KB_SECRET | VK_KB_EMPTY_OK);
  walk_to(&kb, VK_KB_ACTION_ROW, 6);
  CHECK(vk_kb_acceptable(&kb) && vk_kb_select(&kb) == VK_KB_ACCEPT, "empty with VK_KB_EMPTY_OK");
  vk_kb_type(&kb, 'x');
  CHECK(!vk_kb_acceptable(&kb) && vk_kb_select(&kb) == VK_KB_TOO_SHORT, "one character with VK_KB_EMPTY_OK");

  // No limits: the open-network case.
  vk_kb_init(&kb, NULL, 0, 32, 0);
  walk_to(&kb, VK_KB_ACTION_ROW, 6);
  CHECK(vk_kb_select(&kb) == VK_KB_ACCEPT, "DONE with no minimum");

  // What init corrects.
  vk_kb_init(&kb, NULL, 0, 0, 0);
  CHECK(kb.max_len == VK_KB_TEXT_MAX, "max_len 0 became %d", kb.max_len);
  vk_kb_init(&kb, NULL, 0, 200, 0);
  CHECK(kb.max_len == VK_KB_TEXT_MAX, "max_len 200 became %d", kb.max_len);
  vk_kb_init(&kb, NULL, 40, 32, 0);
  CHECK(kb.min_len == 32 && kb.max_len == 32, "min_len 40 with max_len 32 became %d", kb.min_len);
  for (int i = 0; i < 200 && vk_kb_type(&kb, 'q') == VK_KB_TYPED; ++i) {}   // bounded: a broken limit must fail, not hang
  CHECK(kb.len == 32 && kb.text[32] == '\0', "the field took %d characters", kb.len);
  vk_kb_init(&kb, NULL, 0, VK_KB_TEXT_MAX, 0);
  for (int i = 0; i < 200 && vk_kb_type(&kb, 'q') == VK_KB_TYPED; ++i) {}
  CHECK(kb.len == VK_KB_TEXT_MAX && kb.text[VK_KB_TEXT_MAX] == '\0', "the longest field took %d characters", kb.len);

  // The initial text.
  vk_kb_init(&kb, "My Hotspot", 1, 32, 0);
  CHECK(strcmp(kb.text, "My Hotspot") == 0 && kb.len == 10, "initial text \"%s\"", kb.text);
  CHECK(vk_kb_backspace(&kb) == VK_KB_ERASED && strcmp(kb.text, "My Hotspo") == 0, "backspace on the initial text");
  vk_kb_init(&kb, "abcdefgh", 0, 5, 0);
  CHECK(strcmp(kb.text, "abcde") == 0 && kb.len == 5, "an initial text longer than max_len gave \"%s\"", kb.text);
  vk_kb_init(&kb, "ab\tcd", 0, 32, 0);
  CHECK(strcmp(kb.text, "ab") == 0 && kb.len == 2, "an initial text with a tab gave \"%s\"", kb.text);
  vk_kb_init(&kb, "", 0, 32, 0);
  CHECK(kb.len == 0, "an empty initial text");
  // A second init leaves nothing of the first text.
  vk_kb_init(&kb, "secret-secret", 0, 32, VK_KB_SECRET);
  vk_kb_init(&kb, "x", 0, 32, 0);
  CHECK(strcmp(kb.text, "x") == 0 && kb.text[2] == '\0' && kb.text[12] == '\0', "init kept bytes of the text before");
}

// ---- 6: show, wipe ------------------------------------------------------------------------------

static void test_show_wipe(void) {
  vk_kb_t kb;
  vk_kb_init(&kb, NULL, 8, 63, VK_KB_SECRET);
  CHECK(kb.shown == 0, "a secret field starts shown");
  walk_to(&kb, VK_KB_ACTION_ROW, 5);
  CHECK(vk_kb_select(&kb) == VK_KB_SHOW_CHANGED && kb.shown == 1, "show");
  CHECK(vk_kb_select(&kb) == VK_KB_SHOW_CHANGED && kb.shown == 0, "hide");
  CHECK(kb.len == 0, "the show key typed something");

  vk_kb_init(&kb, NULL, 1, 32, 0);
  CHECK(kb.shown == 1, "a plain field starts hidden");

  vk_kb_init(&kb, "correct horse battery staple", 8, 63, VK_KB_SECRET);
  vk_kb_wipe(&kb);
  int dirty = 0;
  for (size_t i = 0; i < sizeof kb.text; ++i) dirty += kb.text[i] != 0;
  CHECK(dirty == 0 && kb.len == 0, "the wipe left %d bytes", dirty);
  CHECK(kb.min_len == 8 && kb.max_len == 63, "the wipe changed the limits");
}

// ---- 7: a string over all four layers -------------------------------------------------------------

static void test_typing(void) {
  vk_kb_t kb;
  // Lower, upper, digit, a symbol of the digit layer, a symbol only the symbol layer has, a space.
  const char *want = "hi Zed-42 {ok}~\\";
  vk_kb_init(&kb, NULL, 8, 63, VK_KB_SECRET);
  for (const char *p = want; *p; ++p) {
    CHECK(type_char(&kb, *p) == VK_KB_TYPED, "typing '%c'", *p);
  }
  CHECK(strcmp(kb.text, want) == 0, "typed \"%s\", wanted \"%s\"", kb.text, want);
  walk_to(&kb, VK_KB_ACTION_ROW, 6);
  CHECK(vk_kb_select(&kb) == VK_KB_ACCEPT, "DONE");

  // Every printable character, in order, then every one backwards.
  vk_kb_init(&kb, NULL, 0, VK_KB_TEXT_MAX, 0);
  char all[VK_KB_TEXT_MAX + 1];
  int n = 0;
  for (int ch = 32; ch <= 126 && n < VK_KB_TEXT_MAX; ch += 3) all[n++] = (char)ch;
  for (int ch = 126; ch >= 32 && n < VK_KB_TEXT_MAX; ch -= 3) all[n++] = (char)ch;
  all[n] = '\0';
  for (int i = 0; i < n; ++i) CHECK(type_char(&kb, all[i]) == VK_KB_TYPED, "typing byte %d", all[i]);
  CHECK(strcmp(kb.text, all) == 0, "typed \"%s\"", kb.text);
}

// ---- 8: travel ----------------------------------------------------------------------------------

static void test_travel(void) {
  // The mean number of steps between two different character keys on the torus of 7 columns and
  // 5 rows, and on a 10-column keyboard of three letter rows over one action row (QWERTY's shape).
  double ours = 0, wide = 0;
  int pairs = 0, wide_pairs = 0;
  for (int a = 0; a < 26; ++a) {
    for (int b = 0; b < 26; ++b) {
      if (a == b) continue;
      ours += torus(a / 7, b / 7, VK_KB_ROWS) + torus(a % 7, b % 7, VK_KB_COLS);
      ++pairs;
    }
  }
  const int row_len[3] = {10, 9, 7};
  int wr[26], wc[26], n = 0;
  for (int r = 0; r < 3; ++r) {
    for (int c = 0; c < row_len[r]; ++c) { wr[n] = r; wc[n] = c; ++n; }
  }
  for (int a = 0; a < 26; ++a) {
    for (int b = 0; b < 26; ++b) {
      if (a == b) continue;
      wide += torus(wr[a], wr[b], 4) + torus(wc[a], wc[b], 10);
      ++wide_pairs;
    }
  }
  ours /= pairs;
  wide /= wide_pairs;
  CHECK(ours > 2.90 && ours < 2.93, "mean steps between two letters: %.3f (text-entry.md says 2.92)", ours);
  CHECK(wide > 3.45 && wide < 3.47, "mean steps on a 10-column keyboard: %.3f (text-entry.md says 3.46)", wide);
  CHECK(ours < wide, "the 7-column grid is not shorter");

  // walk_to takes the short way round: no key is further than 3 + 2 steps from any other.
  vk_kb_t kb;
  vk_kb_init(&kb, NULL, 0, 63, VK_KB_SECRET);
  int worst = 0;
  for (int from = 0; from < VK_KB_ROWS * VK_KB_COLS; ++from) {
    for (int to = 0; to < VK_KB_ROWS * VK_KB_COLS; ++to) {
      kb.row = (uint8_t)(from / VK_KB_COLS);
      kb.col = (uint8_t)(from % VK_KB_COLS);
      const int steps = walk_to(&kb, to / VK_KB_COLS, to % VK_KB_COLS);
      CHECK(steps >= 0 && kb.row == to / VK_KB_COLS && kb.col == to % VK_KB_COLS, "walk from %d to %d", from, to);
      if (steps > worst) worst = steps;
    }
  }
  CHECK(worst == 5, "the furthest key is %d steps away", worst);
}

int main(void) {
  test_layout();
  test_cursor();
  test_layers();
  test_text();
  test_limits();
  test_show_wipe();
  test_typing();
  test_travel();
  if (failures != 0) {
    printf("%d keyboard checks failed\n", failures);
    return 1;
  }
  printf("all keyboard tests passed\n");
  return 0;
}
