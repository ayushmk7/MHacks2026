// src/vk/ui/keyboard.h
// The on-screen keyboard (docs/os/ui/text-entry.md): one text-entry screen that any shell page or
// native app can open. A title, the text being typed (masked for a password), a 7 x 4 grid of
// character keys in four layers and an action row. The layout and the editing rules are in
// keyboard_core.h, which has no drawing in it and is host-tested.
//
// Buttons:
//   UP DOWN LEFT RIGHT   move the cursor, with key repeat; it wraps at every edge
//   SELECT               types the key (or switches layer, shows/hides, or DONE)
//   SELECT, held         the letter it just typed changes case: a -> A, and A -> a on the ABC layer
//   CANCEL               erases the last character; on an empty field it leaves
//   CANCEL, held         leaves at once, whatever has been typed (HOLD_EXIT_MS in keyboard.cpp)
// CANCEL therefore always leads out: one press per character, or one hold. The footer says so.
//
// One entry at a time: the state is this module's own. The text is wiped when the entry ends.
#pragma once

#include <Arduino.h>

#include "keyboard_core.h"

namespace vk::ui::keyboard {

constexpr size_t TEXT_MAX = VK_KB_TEXT_MAX;   // the longest text a field can hold (64)

struct Options {
  const char *title = "";          // drawn like a page title: upper case, at most 20 characters
  const char *hint = nullptr;      // one line under the text ("8 to 63 characters"); nullptr = none
  const char *initial = nullptr;   // the text to start with; nullptr = empty
  uint8_t minLen = 0;              // DONE is refused under this many characters
  uint8_t maxLen = TEXT_MAX;       // nothing is typed beyond this many
  bool secret = false;             // a password: masked, with a show key; no test hook reads it
  bool emptyOk = false;            // DONE also accepts an empty text, whatever minLen says
};

enum class State : uint8_t { CLOSED, EDITING, DONE, CANCELLED };

// ---- as a shell screen ----
// Pushes screen `keyboard`. When the user presses DONE or leaves, the screen pops itself and then
// calls done(accepted, text): `text` is what was typed when accepted, "" otherwise, and it is
// valid only during the call (copy it; the keyboard wipes its own copy). done may open the
// keyboard again or push another screen.
using Done = void (*)(bool accepted, const char *text);
void open(const Options &options, Done done);

// ---- as a part of someone else's screen (a native app) ----
// begin(), then every pass update(), and draw() whenever the whole screen must be painted: once
// after begin() and again after anything else drew on the canvas. update() paints by itself the
// parts its own input changed. When update() returns DONE, read text(), then call end().
void begin(const Options &options);
State update();                    // reads the buttons; returns the state after this pass
void draw();                       // paints the whole screen
State state();
const char *text();                // the text so far; "" when CLOSED
size_t length();
void end();                        // wipes the text; the state becomes CLOSED

}  // namespace vk::ui::keyboard
