# Text entry: the on-screen keyboard

One screen on which a person types a short text with the six buttons: a Wi-Fi password, a network name. It is a part of the kit ([ui](ui.md#the-receipt-kit)): any shell page and any native app can open it. Files: `src/vk/ui/keyboard.{h,cpp}` (the screen), `src/vk/ui/keyboard_core.{h,c}` (the layout, the cursor and the text: pure C99, no drawing, host-tested).

The first user is Settings → Wi-Fi ([shell](shell.md#wi-fi)), which asks for a password, and for the name of a network that hides it. The others are the badge's own settings: the display name on Settings → Badge and Setup, and any text or number key on Settings → Advanced, through `vk::shell::edit::type`, which takes the field's limits from the config key ([shell](shell.md#editing-a-config-key)).

**Status:** built and host-tested (`test_keyboard`). **Not yet seen on a badge**: the picture and the timings below were not checked on the glass; `t_wifi_setup.py` is written and has not been run ([what to check](#tests)).

## The screen

```
 BADGEOS                                     14:32 · 87%      header
 - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
                     P A S S W O R D                          title, y = 26
  ● ● ● ● ● ● ● e _                              8/63         the text, baseline y = 57; count at y = 49
 - - - - - - - - - - - - - - - - - - - - - - - - - - - - -    rule, y = 64
  8 to 63 characters, for MHacks-Guest                        hint, or why a key was refused, y = 68

      a      b      c      d      e      f      g             character keys:
      h      i      j     [k]     l      m      n             4 rows of 24 px from y = 80,
      o      p      q      r      s      t      u             7 cells of 42 px from x = 13
      v      w      x      y      z      .      -
 - - - - - - - - - - - - - - - - - - - - - - - - - - - - -    rule, y = 178
     abc    ABC    123    #+=   space   show   DONE           action row, y = 181..204
     ‾‾‾
 ---------------------------------------------------------
  SELECT type  hold: CAPS        CANCEL erase  hold: exit     footer
```

| Element | Drawn |
|---|---|
| frame | `receipt::page()`, `receipt::header("BADGEOS", statusRight)`, `receipt::title(title, 26)` |
| the text | `FreeMonoBold9pt7b`, 11 px a character, from x = 10, on the baseline y = 57, in `INK`; then the caret, a 9 × 2 px bar under the next position. 24 positions fit, the caret included. A longer text shows its **end** (that is where the typing happens) behind a `FAINT` triangle in the first position |
| a masked character | a filled circle of radius 2 in `INK`. In a secret field every character is one, except the one just typed, which is readable for 1.2 s (`REVEAL_MS`) |
| a typed space, when readable | a low `FAINT` bracket, so that it can be seen and counted |
| count | `<length>/<maxLen>`, Font0, right-aligned at x = 310, y = 49, `FAINT`; `STAMP_WARN` when the field is full |
| rule | `receipt::rule(64)` |
| hint | the caller's hint, Font0 at (10, 68), `SUB`. For 2.5 s after a refused key (`NOTE_MS`) the reason takes its place in `STAMP_WARN`: `needs 8 or more characters` (with `, or none` when an empty text is allowed), `63 characters is the most` |
| a character key | its character in `FreeMonoBold9pt7b`, centred in a 42 × 24 px cell, baseline 16 px under the cell's top |
| the cursor | the cell filled `INK` (inset 1 px), its character or label in `PAPER`: the same inversion as a selected row |
| rule | `receipt::rule(178)` |
| an action key | its label in Font0, centred in its cell, 8 px under the cell's top. The key of the layer that is showing has an 18 × 2 px bar under its label. `DONE` is `FAINT` while it would be refused. The show key reads `show` or `hide` |
| footer | left: `SELECT type  hold: CAPS` on the `abc` layer, `SELECT type  hold: small` on `ABC`, `SELECT type` on the other two. Right: `CANCEL back` while the field is empty, otherwise `CANCEL erase  hold: exit` |

Colours are theme tokens only, so both themes work without a line of their own.

## Layout

The keys are a grid of 7 columns and 5 rows: four rows of characters, which change with the layer, and the action row, which never does. The grid is a **torus**: the cursor wraps at every edge, so the action row is one step above the first character row as well as one step below the last, and the last column is one step left of the first.

```
 abc   a b c d e f g      ABC   A B C D E F G      123   1 2 3 4 5 6 7      #+=   " ( ) < > [ ]
       h i j k l m n            H I J K L M N            8 9 0 . - _ @            { } \ ^ ` | ~
       o p q r s t u            O P Q R S T U            ! # $ % & * +            ! # $ % & * +
       v w x y z . -            V W X Y Z . -            = / : ; ? , '            = / : ; ? , '

 action row, every layer:   [abc]  [ABC]  [123]  [#+=]  [space]  [show]  [DONE]
 column                       0      1      2      3       4       5       6
```

The four layers and the space key hold every printable ASCII character, 32 to 126: a Wi-Fi passphrase may use any of them. On a field that is not secret there is nothing to show or hide, and the space key covers columns 4 and 5 (one stop for the cursor).

Why this shape and this order, and not QWERTY:

- **Travel.** With a direction pad the cost of a character is the number of steps to it. For text that is not a word (most passwords) that number depends only on the shape of the grid, and a torus close to square is the shortest: between two letters it is **2.92 steps** on average here, against 3.46 on a keyboard ten keys wide with three letter rows over an action row (QWERTY's shape, with the same wrap-around), and 4.03 on that keyboard without wrap-around. No key is more than 5 steps from any other. For English words (letters weighted by their frequency) the figures are about 2.7 and 3.2. `test_keyboard` computes the first two.
- **Finding the key.** QWERTY cannot be kept on seven columns, and its only merit, that fingers know it, does not apply to a thumb on a direction pad. The alphabet in reading order needs no learning: a person types a network password once, at a venue, and never practises. A layout sorted by letter frequency would save part of a step on English words and cost a visual search for every key.
- **Fewer layer switches.** A layer switch costs a trip to the action row and back, so the layout avoids them: a capital is a held SELECT (below), not a trip to `ABC`; `.` and `-` are on both letter layers; the `123` layer holds the digits **and** the eighteen commonest symbols, so a password such as `sunny-day-2024!` needs one switch. Only the fourteen rare symbols need `#+=`, whose two lower rows repeat those of `123` so that a symbol is in the same place on both.
- **Fast switching all the same.** Each layer has its own key (no cycling through layers), the keys are on the action row, and because of the wrap the action row touches both the first and the last character row. The cursor stays on the layer key after a switch: one step DOWN from `123` is the digit row.

## Buttons

| Button | Does |
|---|---|
| UP, DOWN, LEFT, RIGHT | move the cursor one cell, with key repeat (`buttons::repeated`: after 420 ms, then every 110 ms); wraps at every edge |
| SELECT on a character key or `space` | types it, on the press. At `maxLen` nothing is typed and the hint line says so |
| SELECT, kept down 450 ms (`HOLD_ALT_MS`) | the letter that press typed changes case: `a` becomes `A` (and on the `ABC` layer `A` becomes `a`). Once per press; nothing happens for a character without a case |
| SELECT on a layer key | shows that layer; the cursor stays on the key |
| SELECT on `show` / `hide` | secret fields only: the whole text readable, or masked again |
| SELECT on `DONE` | ends the entry with the text, if its length is allowed (`minLen` or more; or empty, when the caller allows that). Otherwise the hint line says what is needed and nothing else happens |
| CANCEL, field not empty | erases the last character, on the press |
| CANCEL, field empty | leaves: the entry ends without a text |
| CANCEL, kept down 700 ms (`HOLD_EXIT_MS`) | leaves at once, whatever is typed (the press itself has already erased one character; the text is thrown away anyway) |

**CANCEL.** The house rule is that CANCEL always goes back and nobody gets stuck. Here CANCEL goes back one character at a time, and out when there is nothing left; the hold is the short cut. The footer says both whenever there is text (`CANCEL erase  hold: exit`) and `CANCEL back` when there is none. The reasons for not making CANCEL leave at once:

- erasing is the commonest thing a person does while typing a masked password after typing itself, so it gets a button of its own instead of a key that costs a trip across the grid;
- one slip of the thumb must not throw away sixteen typed characters. With this rule a slip costs one character.

Consequences for other code: a script or a person that taps CANCEL repeatedly still gets out (one tap per character, then one more); a test that must leave at once holds CANCEL for a second. The hold is shorter than upstream's force-quit of an app (CANCEL held 1500 ms, `APP_ESCAPE_HOLD_MS`; a `static_assert` keeps it so), so inside a native app the hold leaves the keyboard and does not kill the app unless it is kept down twice as long.

A key press that opened the keyboard, or that is still down from the screen before, does nothing here: SELECT and CANCEL act on a press that began on this screen.

## Limits

`minLen` and `maxLen` belong to the field, not to the keyboard. For a WPA2 passphrase: 8 and 63. For a network name: 1 and 32. For an open network no password is asked. A field with `emptyOk` accepts an empty text besides one of `minLen` or more (the password of a network whose name was typed: empty means open). `maxLen` is at most 64 (`TEXT_MAX`). An initial text is cut at `maxLen` and at its first byte outside 32..126.

## Secrets

- A field marked `secret` is masked from the first frame, and its text is never logged, never sent over serial and never handed to the dev hook, which reports its **length** only. The keyboard itself writes no log line at all.
- The text lives in one static buffer. It is overwritten with zeros when the entry ends (DONE, leaving, or `end()`), and the copy the shell wrapper hands to the caller's callback is wiped when the callback returns. The caller owns what it copied.
- `VKSTATE` knows nothing about the keyboard except the screen's name.
- One thing the keyboard cannot fix: upstream's button driver logs every **physical** key press (`[btn] P4 SELECT down`), on USB and in the log ring. That says which button was pressed, not which key was under the cursor, but someone reading the log of a whole entry could replay the cursor. It was there before this screen existed and it affects every screen; removing it is an upstream edit (a hook), not done here.

## Drawing only what changed

`draw()` paints the whole screen. `update()` paints, straight on the canvas, only the parts its own input changed, and never asks for a full repaint:

| Input | Painted |
|---|---|
| a cursor step | the cell left and the cell entered |
| a character typed, erased, or changed by the hold | the text line; the footer when the field became empty or stopped being empty; `DONE` when it became acceptable or stopped being so |
| a layer key | the 28 character cells, the action row, the footer |
| `show` / `hide` | the text line and that key |
| a refused key | the hint line (and again when the note expires) |
| the reveal time runs out | the text line |

The transfer of the canvas to the panel is the same 34 ms for any change ([measurements](../testing/testing.md#responsiveness)); what this saves is the drawing in front of it. An idle keyboard draws nothing.

## API

`src/vk/ui/keyboard.h`, complete:

```cpp
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
```

There is one keyboard: its state is the module's own, and `begin()` or `open()` while an entry is open starts a new one. The title, the hint and the initial text are copied, so the caller's strings need not outlive the call. The title is cut at 23 characters and the hint at 51 (50 fit the screen).

The logic is `src/vk/ui/keyboard_core.h` (C99; the header has the layout as a comment): `vk_kb_init`, `vk_kb_key`, `vk_kb_current`, `vk_kb_action_span`, `vk_kb_move`, `vk_kb_select`, `vk_kb_hold`, `vk_kb_backspace`, `vk_kb_type`, `vk_kb_acceptable`, `vk_kb_find`, `vk_kb_layer_name`, `vk_kb_wipe`, on a `vk_kb_t` that holds the text, the limits, the layer and the cursor. It knows no button, no clock and no pixel. `vk_kb_type(kb, ch)` appends any printable character wherever the cursor is: a touch driver, if the badge ever has one, calls it with the key a tap landed on.

### From a shell page

```cpp
#include "../../ui/keyboard.h"

namespace keyboard = vk::ui::keyboard;

void nameDone(bool accepted, const char *text) {     // the keyboard has popped itself: this page is on top
  if (!accepted) return;                             // the user left
  strlcpy(sName, text, sizeof sName);                // copy: `text` is wiped after this call
  repaint();
}

void pageUpdate() {
  if (back()) return;
  if (buttons::pressed(BTN_A)) {
    keyboard::Options options;
    options.title = "DISPLAY NAME";
    options.hint = "what other badges see";
    options.initial = sName;
    options.minLen = 1;
    options.maxLen = 32;
    keyboard::open(options, nameDone);               // pushes screen `keyboard`
  }
}
```

The screen's name is `keyboard` (`shell::screenName()`, `VKSTATE.screen`). It needs one slot of the shell's stack of six.

### From a native app

```cpp
#include "../../vk/sdk/badge_sdk.hpp"
#include "../../vk/ui/keyboard.h"

namespace keyboard = vk::ui::keyboard;

class Rename final : public badge::App {
 public:
  void on_start() override {
    keyboard::Options options;
    options.title = "NAME";
    options.maxLen = 16;
    keyboard::begin(options);
    dirty_ = true;
  }
  void on_update(float) override {
    switch (keyboard::update()) {                    // reads the buttons itself: no on_button needed
      case keyboard::State::DONE:      save(keyboard::text()); keyboard::end(); badge::exit(); break;
      case keyboard::State::CANCELLED: keyboard::end(); badge::exit(); break;
      default: break;
    }
  }
  void on_draw() override {
    if (!dirty_) return;                             // on_draw runs every pass: paint the whole screen once
    dirty_ = false;
    keyboard::draw();
  }
  void on_stop() override { keyboard::end(); }
 private:
  bool dirty_ = true;
};
```

An app must call `draw()` again after something else drew on the canvas (an approval that closed). A native app is firmware and is reviewed as such ([native apps](../platform/native-apps.md#trust)); a Lua app has no keyboard binding.

## Timing

Named constants at the top of `keyboard.cpp`:

| Constant | Value | Meaning |
|---|---|---|
| `HOLD_ALT_MS` | 450 | SELECT held this long changes the case of the letter it typed |
| `HOLD_EXIT_MS` | 700 | CANCEL held this long leaves; must stay under `APP_ESCAPE_HOLD_MS` (1500) |
| `REVEAL_MS` | 1200 | a masked field shows the character just typed this long |
| `NOTE_MS` | 2500 | a refusal stays in the hint line this long |

Key repeat is upstream's (`BUTTON_REPEAT_DELAY_MS` 420, `BUTTON_REPEAT_PERIOD_MS` 110).

## Touch

None. The board has a footprint for a touch controller, but the tree has no touch driver: upstream only brings the GT911 (I²C `0x14` or `0x5D`) out of reset so that it does not clamp the bus, and never addresses it (`src/hal/badge_i2c.cpp`, `PIN_TOUCH_RST`); the one controller seen on the bus (a TSC2007 at `0x4A`, finding F17) has no driver either. No driver was added for this screen. If one arrives, a tap is `vk_kb_type()` with the key under the finger; the cells are 42 × 24 px.

## Dev hook

`VKKBD`, dev profile only (registered in `keyboard.cpp` under `VK_TEST_HOOKS`; a release image does not contain it):

```json
{"open":true,"title":"NETWORK NAME","layer":"abc","row":0,"col":0,"key":"a","len":0,"min":1,"max":32,
 "secret":false,"shown":true,"rows":["abcdefg","hijklmn","opqrstu","vwxyz.-"],"text":""}
```

`{"open":false}` when no entry is open. `key` is the key under the cursor: a character, a layer's name, `space`, `show` or `DONE`. `rows` are the character rows of the layer that is showing, so a test finds a key without a copy of the layout. **`text` is in the reply only when the field is not secret**; a secret field gives `len` and nothing more.

## Tests

- Host: `test_keyboard` (`cd os && test/host/run.sh test_keyboard`), on `keyboard_core.c` alone: every character 32..126 is on a key; the cursor's steps and its wrap at all four edges; the wide space key as one stop; each layer key; typing, the held SELECT, backspace to empty; `maxLen`, `minLen`, `emptyOk`, the values `vk_kb_init` corrects; show and hide; the wipe; a string over all four layers typed with steps and SELECT only; the travel figures above.
- Device: `t_wifi_setup.py` ([shell](shell.md#wi-fi)) opens the keyboard twice (a plain field and a secret one), types through all four layers, checks CANCEL on an empty field, CANCEL as backspace and the CANCEL hold, DONE refused under `minLen`, show and hide, and that a secret field's text is in no reply. Screenshots in both themes: `shots/wifi_setup_name_<theme>.png`, `wifi_setup_password_<theme>.png`.
- To look at on the glass, once (nothing here was seen on a badge): that the characters sit in the middle of their cells and that `g`, `j`, `|` and `_` are not cut by the cell or the rule; that `l`, `1`, `I`, `O` and `0` can be told apart in the text line; that a held direction key repeats at a speed a thumb can stop; that 450 ms is neither too short for a deliberate tap nor too long for a capital.
