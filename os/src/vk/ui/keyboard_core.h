// src/vk/ui/keyboard_core.h
// The on-screen keyboard's logic (docs/os/ui/text-entry.md): the key layout, the cursor, the four
// layers and the text being typed. Pure C99: no Arduino, no heap, no drawing, no clock, no buttons.
// src/vk/ui/keyboard.cpp draws it and feeds it the buttons; test/host/test_keyboard.c tests it.
//
// The keys are a grid of VK_KB_COLS x VK_KB_ROWS cells. Rows 0..3 hold the characters of the layer
// that is showing; row 4 is the action row, the same on every layer, one key per column:
//
//      col   0     1     2     3     4      5      6
//   row 4  [abc] [ABC] [123] [#+=] [space][show] [DONE]
//
// On a field that is not secret there is nothing to show or hide, so the space key covers
// columns 4 and 5.
//
// The grid is a torus: the cursor wraps at every edge, so the action row is one step above the
// first character row as well as one step below the last.
//
// The four layers (each 4 rows of 7):
//
//   abc  a b c d e f g     ABC  A B C D E F G     123  1 2 3 4 5 6 7     #+=  " ( ) < > [ ]
//        h i j k l m n          H I J K L M N          8 9 0 . - _ @          { } \ ^ ` | ~
//        o p q r s t u          O P Q R S T U          ! # $ % & * +          ! # $ % & * +
//        v w x y z . -          V W X Y Z . -          = / : ; ? , '          = / : ; ? , '
//
// Together with the space key they hold every printable ASCII character, 32..126. Why this shape
// and this order: text-entry.md, "Layout".
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define VK_KB_COLS 7
#define VK_KB_CHAR_ROWS 4
#define VK_KB_ROWS 5                 // the character rows and the action row
#define VK_KB_ACTION_ROW 4
#define VK_KB_TEXT_MAX 64            // the longest text a field can hold

// Layers, in the order of their keys on the action row.
enum { VK_KB_LOWER = 0, VK_KB_UPPER, VK_KB_DIGITS, VK_KB_SYMBOLS, VK_KB_LAYER_COUNT };

// What a key is.
enum { VK_KB_KEY_CHAR = 0, VK_KB_KEY_LAYER, VK_KB_KEY_SPACE, VK_KB_KEY_SHOW, VK_KB_KEY_DONE };

// Field flags.
#define VK_KB_SECRET 0x01            // a password: drawn masked, with a show key
#define VK_KB_EMPTY_OK 0x02          // DONE also accepts an empty text, whatever min_len says

// What a call changed.
enum {
  VK_KB_NONE = 0,                    // nothing
  VK_KB_TYPED,                       // the text got a character, or its last character changed case
  VK_KB_ERASED,                      // the text lost its last character
  VK_KB_EMPTY,                       // backspace on an empty text
  VK_KB_FULL,                        // the text is max_len long: the character was not added
  VK_KB_LAYER_CHANGED,
  VK_KB_SHOW_CHANGED,
  VK_KB_ACCEPT,                      // DONE, and the length is allowed
  VK_KB_TOO_SHORT                    // DONE, and the text is shorter than min_len
};

typedef struct {
  uint8_t kind;                      // VK_KB_KEY_*
  char ch;                           // CHAR: the character. SPACE: ' '. Otherwise 0
  uint8_t layer;                     // LAYER: the layer it switches to. Otherwise 0
} vk_kb_key_t;

typedef struct {
  char text[VK_KB_TEXT_MAX + 1];     // always NUL-terminated
  uint8_t len;
  uint8_t min_len, max_len;          // max_len is 1..VK_KB_TEXT_MAX; min_len <= max_len
  uint8_t flags;                     // VK_KB_SECRET | VK_KB_EMPTY_OK
  uint8_t layer;                     // the layer that is showing
  uint8_t row, col;                  // the cursor
  uint8_t shown;                     // a secret text is drawn in clear
  uint8_t can_swap;                  // the last character is a letter typed by the last select
} vk_kb_t;

// Starts a field. `initial` may be NULL; it is copied up to max_len characters and up to its first
// byte outside 32..126. The cursor starts on the first key of the lower-case layer.
void vk_kb_init(vk_kb_t *kb, const char *initial, uint8_t min_len, uint8_t max_len, uint8_t flags);

// The key at (row, col) of the layer that is showing, and the key under the cursor.
vk_kb_key_t vk_kb_key(const vk_kb_t *kb, int row, int col);
vk_kb_key_t vk_kb_current(const vk_kb_t *kb);

// The columns the action-row key at `col` covers (one, or two for the wide space key).
void vk_kb_action_span(const vk_kb_t *kb, int col, int *first, int *last);

// One step: dx and dy are -1, 0 or 1. Wraps at every edge. On the action row a horizontal step
// leaves the whole key, so the wide space key is one stop.
void vk_kb_move(vk_kb_t *kb, int dx, int dy);

// SELECT on the key under the cursor. Returns VK_KB_TYPED, VK_KB_FULL, VK_KB_LAYER_CHANGED,
// VK_KB_SHOW_CHANGED, VK_KB_ACCEPT, VK_KB_TOO_SHORT or VK_KB_NONE (the layer already showing).
int vk_kb_select(vk_kb_t *kb);

// SELECT is still held after it typed a letter: that letter changes case (a -> A, A -> a), once.
// VK_KB_TYPED when it did, VK_KB_NONE otherwise.
int vk_kb_hold(vk_kb_t *kb);

// Removes the last character: VK_KB_ERASED, or VK_KB_EMPTY when there was none.
int vk_kb_backspace(vk_kb_t *kb);

// Appends `ch` wherever the cursor is: VK_KB_TYPED, VK_KB_FULL, or VK_KB_NONE for a byte outside
// 32..126.
int vk_kb_type(vk_kb_t *kb, char ch);

// 1 when DONE would be accepted now.
int vk_kb_acceptable(const vk_kb_t *kb);

// Where `ch` is: the first layer that has it, and its cell. The space is on the action row of
// every layer (*layer is then VK_KB_LOWER). Returns 0, writing nothing, for a byte outside 32..126.
int vk_kb_find(char ch, int *layer, int *row, int *col);

// "abc", "ABC", "123", "#+=": the label of the layer's key. "" for an unknown layer.
const char *vk_kb_layer_name(int layer);

// Overwrites the text with zeros and sets the length to 0 (a password must not stay in memory).
void vk_kb_wipe(vk_kb_t *kb);

#ifdef __cplusplus
}
#endif
