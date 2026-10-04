// src/vk/ui/keyboard_core.c
// The on-screen keyboard's logic (keyboard_core.h; docs/os/ui/text-entry.md). Pure C99.
#include "keyboard_core.h"

#include <stddef.h>

// One string per row, VK_KB_COLS characters each. The two lower rows of `123` and `#+=` are the
// same, so a common symbol is in one place on both.
static const char *const LAYERS[VK_KB_LAYER_COUNT][VK_KB_CHAR_ROWS] = {
    {"abcdefg", "hijklmn", "opqrstu", "vwxyz.-"},
    {"ABCDEFG", "HIJKLMN", "OPQRSTU", "VWXYZ.-"},
    {"1234567", "890.-_@", "!#$%&*+", "=/:;?,'"},
    {"\"()<>[]", "{}\\^`|~", "!#$%&*+", "=/:;?,'"},
};

static const char *const LAYER_NAMES[VK_KB_LAYER_COUNT] = {"abc", "ABC", "123", "#+="};

// The action row's columns.
enum { COL_SPACE = VK_KB_LAYER_COUNT, COL_SHOW, COL_DONE };

static int printable(char ch) { return ch >= 0x20 && ch <= 0x7E; }

static int wrap(int value, int count) {
  value %= count;
  return value < 0 ? value + count : value;
}

static int is_lower(char ch) { return ch >= 'a' && ch <= 'z'; }
static int is_upper(char ch) { return ch >= 'A' && ch <= 'Z'; }

void vk_kb_init(vk_kb_t *kb, const char *initial, uint8_t min_len, uint8_t max_len, uint8_t flags) {
  if (kb == NULL) return;
  vk_kb_wipe(kb);
  if (max_len == 0 || max_len > VK_KB_TEXT_MAX) max_len = VK_KB_TEXT_MAX;
  if (min_len > max_len) min_len = max_len;
  kb->min_len = min_len;
  kb->max_len = max_len;
  kb->flags = flags;
  kb->layer = VK_KB_LOWER;
  kb->row = 0;
  kb->col = 0;
  kb->shown = (flags & VK_KB_SECRET) ? 0 : 1;
  kb->can_swap = 0;
  if (initial != NULL) {
    while (kb->len < max_len && printable(initial[kb->len])) {
      kb->text[kb->len] = initial[kb->len];
      ++kb->len;
    }
    kb->text[kb->len] = '\0';
  }
}

vk_kb_key_t vk_kb_key(const vk_kb_t *kb, int row, int col) {
  vk_kb_key_t key = {VK_KB_KEY_CHAR, 0, 0};
  if (kb == NULL || row < 0 || row >= VK_KB_ROWS || col < 0 || col >= VK_KB_COLS) return key;
  if (row < VK_KB_CHAR_ROWS) {
    key.ch = LAYERS[kb->layer < VK_KB_LAYER_COUNT ? kb->layer : 0][row][col];
    return key;
  }
  if (col < VK_KB_LAYER_COUNT) {
    key.kind = VK_KB_KEY_LAYER;
    key.layer = (uint8_t)col;
  } else if (col == COL_SPACE || (col == COL_SHOW && !(kb->flags & VK_KB_SECRET))) {
    key.kind = VK_KB_KEY_SPACE;
    key.ch = ' ';
  } else if (col == COL_SHOW) {
    key.kind = VK_KB_KEY_SHOW;
  } else {
    key.kind = VK_KB_KEY_DONE;
  }
  return key;
}

vk_kb_key_t vk_kb_current(const vk_kb_t *kb) {
  if (kb == NULL) return vk_kb_key(NULL, 0, 0);
  return vk_kb_key(kb, kb->row, kb->col);
}

void vk_kb_action_span(const vk_kb_t *kb, int col, int *first, int *last) {
  int a = col, b = col;
  if (kb != NULL && !(kb->flags & VK_KB_SECRET) && (col == COL_SPACE || col == COL_SHOW)) {
    a = COL_SPACE;
    b = COL_SHOW;
  }
  if (first != NULL) *first = a;
  if (last != NULL) *last = b;
}

void vk_kb_move(vk_kb_t *kb, int dx, int dy) {
  if (kb == NULL) return;
  if (dy != 0) kb->row = (uint8_t)wrap(kb->row + (dy > 0 ? 1 : -1), VK_KB_ROWS);
  if (dx != 0) {
    int first = kb->col, last = kb->col;
    if (kb->row == VK_KB_ACTION_ROW) vk_kb_action_span(kb, kb->col, &first, &last);
    kb->col = (uint8_t)wrap(dx > 0 ? last + 1 : first - 1, VK_KB_COLS);
  }
}

int vk_kb_type(vk_kb_t *kb, char ch) {
  if (kb == NULL || !printable(ch)) return VK_KB_NONE;
  kb->can_swap = 0;
  if (kb->len >= kb->max_len) return VK_KB_FULL;
  kb->text[kb->len++] = ch;
  kb->text[kb->len] = '\0';
  return VK_KB_TYPED;
}

int vk_kb_acceptable(const vk_kb_t *kb) {
  if (kb == NULL) return 0;
  if (kb->len == 0 && (kb->flags & VK_KB_EMPTY_OK)) return 1;
  return kb->len >= kb->min_len;
}

int vk_kb_select(vk_kb_t *kb) {
  if (kb == NULL) return VK_KB_NONE;
  const vk_kb_key_t key = vk_kb_current(kb);
  int result;
  switch (key.kind) {
    case VK_KB_KEY_CHAR:
    case VK_KB_KEY_SPACE:
      result = vk_kb_type(kb, key.ch);
      if (result == VK_KB_TYPED && (is_lower(key.ch) || is_upper(key.ch))) kb->can_swap = 1;
      return result;
    case VK_KB_KEY_LAYER:
      kb->can_swap = 0;
      if (kb->layer == key.layer) return VK_KB_NONE;
      kb->layer = key.layer;
      return VK_KB_LAYER_CHANGED;
    case VK_KB_KEY_SHOW:
      kb->can_swap = 0;
      kb->shown = kb->shown ? 0 : 1;
      return VK_KB_SHOW_CHANGED;
    default:
      kb->can_swap = 0;
      return vk_kb_acceptable(kb) ? VK_KB_ACCEPT : VK_KB_TOO_SHORT;
  }
}

int vk_kb_hold(vk_kb_t *kb) {
  if (kb == NULL || !kb->can_swap || kb->len == 0) return VK_KB_NONE;
  kb->can_swap = 0;
  char *last = &kb->text[kb->len - 1];
  if (is_lower(*last)) {
    *last = (char)(*last - 'a' + 'A');
  } else if (is_upper(*last)) {
    *last = (char)(*last - 'A' + 'a');
  } else {
    return VK_KB_NONE;
  }
  return VK_KB_TYPED;
}

int vk_kb_backspace(vk_kb_t *kb) {
  if (kb == NULL) return VK_KB_EMPTY;
  kb->can_swap = 0;
  if (kb->len == 0) return VK_KB_EMPTY;
  kb->text[--kb->len] = '\0';
  return VK_KB_ERASED;
}

int vk_kb_find(char ch, int *layer, int *row, int *col) {
  if (!printable(ch)) return 0;
  if (ch == ' ') {
    if (layer != NULL) *layer = VK_KB_LOWER;
    if (row != NULL) *row = VK_KB_ACTION_ROW;
    if (col != NULL) *col = COL_SPACE;
    return 1;
  }
  for (int l = 0; l < VK_KB_LAYER_COUNT; ++l) {
    for (int r = 0; r < VK_KB_CHAR_ROWS; ++r) {
      for (int c = 0; c < VK_KB_COLS; ++c) {
        if (LAYERS[l][r][c] != ch) continue;
        if (layer != NULL) *layer = l;
        if (row != NULL) *row = r;
        if (col != NULL) *col = c;
        return 1;
      }
    }
  }
  return 0;
}

const char *vk_kb_layer_name(int layer) {
  return (layer >= 0 && layer < VK_KB_LAYER_COUNT) ? LAYER_NAMES[layer] : "";
}

void vk_kb_wipe(vk_kb_t *kb) {
  if (kb == NULL) return;
  // Through a volatile pointer, so the compiler cannot drop the writes as dead stores.
  volatile char *p = kb->text;
  for (size_t i = 0; i < sizeof kb->text; ++i) p[i] = 0;
  kb->len = 0;
  kb->can_swap = 0;
}
