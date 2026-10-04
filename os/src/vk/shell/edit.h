// src/vk/shell/edit.h
// Changing a config key from the badge's buttons (docs/os/ui/shell.md, "Editing a config key"): the
// helpers the Badge, Setup and Advanced pages share. Every change goes through vk::config::set, so a
// key's own type, range and rule decide what is accepted; no range or default is copied here: they
// are read from the key's registration (vk::config::find).
//
// What can be changed here at all: a key that is neither secure nor required, of type U32 or STR.
// Secure keys (the issuer key, the token table, limits) and required keys (the RPC address) come
// from the laptop tool only, so that the wallet's root of trust is never typed with the buttons.
#pragma once

#include <Arduino.h>

#include "../core/config.h"
#include "../core/registry.h"

namespace vk::shell::edit {

// ---- choices ------------------------------------------------------------------------------------
// The values a key may take, when they are a list (an app id, a theme, an offset): the key is then
// changed with a picker instead of the keyboard. Registered by the code that knows the list, e.g.
//   VK_KEY_CHOICES(pay_app, "pay_app", appCount, appAt);
// at(i) writes choice i's value (what is stored) and its label (what is shown); false past the end.
using At = bool (*)(size_t index, char *value, size_t valueCap, char *label, size_t labelCap);
struct Choices : Registered<Choices> {
  const char *key;
  size_t (*count)();
  At at;
  Choices(const char *k, size_t (*c)(), At a) : key(k), count(c), at(a) {}
};
#define VK_KEY_CHOICES(ident, key, count_fn, at_fn) \
  static vk::shell::edit::Choices vk_choices_##ident{key, count_fn, at_fn}
const Choices *choicesFor(const char *key);

// ---- how a key is changed here ------------------------------------------------------------------
enum class How : uint8_t {
  STEP,     // U32: LEFT/RIGHT one step (vk_step_u32 within the key's range); SELECT types a number
  CHOOSE,   // a key with registered choices: LEFT/RIGHT the previous/next choice; SELECT the picker
  TYPE,     // STR: SELECT opens the keyboard with the key's length limits
  LAPTOP    // secure, required, another type, or a stored text too long for the keyboard: USB only
};
How how(const vk::config::ConfigKey &key);

// Each writes through vk::config::set and leaves a note (below). False when nothing changed.
bool step(const char *key, int direction);
bool cycle(const char *key, int direction);
void type(const char *key, const char *title);     // pushes screen `keyboard`; writes on DONE
void choose(const char *key, const char *title);   // pushes screen `pick`; writes on SELECT

// ---- the picker: screen `pick` --------------------------------------------------------------------
// A list of `count` choices read through `at` (only the rows on screen are read). It opens on the
// choice whose value is `current`, which is marked `current`. SELECT pops the picker, then calls
// picked(value) (copy it: it lives for the call); CANCEL pops it and calls nothing.
using Picked = void (*)(const char *value);
void pick(const char *title, size_t count, At at, const char *current, Picked picked);

// ---- going to another settings page ---------------------------------------------------------------
// The registered settings page with that id is pushed (an action row: its action runs). False when
// no page has that id, so a jump to a page that was removed does nothing.
bool openPage(const char *id);

// ---- the note -------------------------------------------------------------------------------------
// What the last write did, for 3 s: "saved", "refused: a number from 0 to 3600", "not saved:
// storage is full". nullptr when there is none. *bad (may be nullptr) is true for a refusal.
const char *note(bool *bad = nullptr);
void noteResult(const char *key, vk::config::SetResult result);   // the note for a write to `key`
void noteSay(const char *text, bool bad = false);                 // any other note ("saved")

}  // namespace vk::shell::edit
