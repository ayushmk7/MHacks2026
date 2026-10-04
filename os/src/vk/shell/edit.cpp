// Changing a config key from the buttons, the picker screen `pick`, and the jump to another settings
// page (docs/os/ui/shell.md, "Editing a config key"). See edit.h.
#include "edit.h"

#include <stdio.h>
#include <string.h>

#include "../../badge_log.h"
#include "../ui/keyboard.h"
#include "page.h"
#include "setup_core.h"

namespace vk::shell::edit {

namespace {

namespace cfg = vk::config;
namespace kb = vk::ui::keyboard;
namespace tk = vk::ui::theme;

constexpr uint32_t NOTE_MS = 3000;
constexpr size_t VALUE_CAP = kb::TEXT_MAX + 1;   // the longest value a picker row or the keyboard holds
constexpr size_t LABEL_CAP = 40;
constexpr int JUMP_SLOTS = 4;                    // pages opened by a jump that can be on the stack at once

char sNote[56] = "";
uint32_t sNoteAt = 0;
bool sNoteBad = false;

void setNote(bool bad, const char *text) {
  strlcpy(sNote, text != nullptr ? text : "", sizeof sNote);
  sNoteAt = millis();
  sNoteBad = bad;
  repaint();
}

}  // namespace

// ---- choices and how ----------------------------------------------------------------------------

const Choices *choicesFor(const char *key) {
  if (key == nullptr) return nullptr;
  for (const Choices *c = Choices::first(); c != nullptr; c = c->next()) {
    if (c->key != nullptr && strcmp(c->key, key) == 0) return c;
  }
  return nullptr;
}

How how(const cfg::ConfigKey &key) {
  // The root of trust and anything that could weaken an approval: from the laptop only.
  if (key.flags & (cfg::F_SECURE | cfg::F_REQUIRED)) return How::LAPTOP;
  if (key.type != cfg::Type::U32 && key.type != cfg::Type::STR) return How::LAPTOP;
  if (choicesFor(key.name) != nullptr) return How::CHOOSE;
  if (key.type == cfg::Type::U32) return How::STEP;
  // A stored text the keyboard cannot hold whole would be cut by editing it.
  if (cfg::text(key.name).length() > kb::TEXT_MAX) return How::LAPTOP;
  return How::TYPE;
}

// ---- the note -----------------------------------------------------------------------------------

const char *note(bool *bad) {
  if (sNote[0] == '\0') return nullptr;
  if (millis() - sNoteAt >= NOTE_MS) {
    sNote[0] = '\0';
    return nullptr;
  }
  if (bad != nullptr) *bad = sNoteBad;
  return sNote;
}

void noteSay(const char *text, bool bad) { setNote(bad, text); }

void noteResult(const char *key, cfg::SetResult result) {
  const cfg::ConfigKey *k = cfg::find(key);
  switch (result) {
    case cfg::SetResult::OK:
      badge_log::tagf("os", "setting %s changed on the badge", key);   // the name only: values may be personal
      setNote(false, "saved");
      return;
    case cfg::SetResult::PENDING:
      setNote(false, "confirm on the badge");
      return;
    case cfg::SetResult::INVALID: {
      if (k == nullptr) break;
      char text[sizeof sNote];
      if (k->type == cfg::Type::U32) {
        snprintf(text, sizeof text, "refused: a number from %u to %u", (unsigned)k->min, (unsigned)k->max);
      } else if (cfg::ruleFor(k->name) != nullptr) {
        snprintf(text, sizeof text, "refused: not a valid value (see its help)");
      } else if (k->min == 0) {
        snprintf(text, sizeof text, "refused: up to %u printable characters", (unsigned)k->max);
      } else {
        snprintf(text, sizeof text, "refused: %u to %u printable characters", (unsigned)k->min, (unsigned)k->max);
      }
      setNote(true, text);
      return;
    }
    case cfg::SetResult::UNAVAILABLE:
      setNote(true, "not saved: it needs a confirmation");
      return;
    case cfg::SetResult::STORAGE:
      setNote(true, "not saved: storage is full");
      return;
    case cfg::SetResult::UNKNOWN_KEY:
      break;
  }
  setNote(true, "not saved: unknown setting");
}

// ---- changes ------------------------------------------------------------------------------------

namespace {

cfg::SetResult write(const char *key, const char *value) {
  const cfg::ConfigKey *k = cfg::find(key);
  // Never past this file's own rule, whatever a caller asks: a secure or required key is not
  // written from the buttons (set() would raise the hold confirmation for a secure key on a
  // provisioned badge, and write it without one on an unprovisioned badge).
  if (k == nullptr) return cfg::SetResult::UNKNOWN_KEY;
  if (how(*k) == How::LAPTOP) return cfg::SetResult::UNAVAILABLE;
  return cfg::set(key, value);
}

char sTypeKey[16] = "";

void typedDone(bool accepted, const char *text) {
  if (!accepted || sTypeKey[0] == '\0') return;
  noteResult(sTypeKey, write(sTypeKey, text));
}

char sChooseKey[16] = "";

void chosen(const char *value) {
  if (sChooseKey[0] == '\0') return;
  noteResult(sChooseKey, write(sChooseKey, value));
}

}  // namespace

bool step(const char *key, int direction) {
  const cfg::ConfigKey *k = cfg::find(key);
  if (k == nullptr || k->type != cfg::Type::U32 || how(*k) != How::STEP) return false;
  const uint32_t now = cfg::u32(key);
  const uint32_t next = vk_step_u32(now, k->min, k->max, direction);
  if (next == now) return false;   // at a limit: nothing changes and nothing is written
  char text[12];
  snprintf(text, sizeof text, "%u", (unsigned)next);
  const cfg::SetResult result = write(key, text);
  if (result != cfg::SetResult::OK) noteResult(key, result);
  repaint();
  return result == cfg::SetResult::OK;
}

bool cycle(const char *key, int direction) {
  const Choices *c = choicesFor(key);
  if (c == nullptr || direction == 0) return false;
  const size_t n = c->count();
  if (n == 0) return false;
  const String current = cfg::text(key);
  char value[VALUE_CAP], label[LABEL_CAP];
  size_t at = n;   // not found
  for (size_t i = 0; i < n; ++i) {
    if (c->at(i, value, sizeof value, label, sizeof label) && current == value) {
      at = i;
      break;
    }
  }
  size_t next;
  if (at == n) {
    next = direction > 0 ? 0 : n - 1;
  } else {
    next = direction > 0 ? (at + 1) % n : (at + n - 1) % n;
  }
  if (next == at || !c->at(next, value, sizeof value, label, sizeof label)) return false;
  const cfg::SetResult result = write(key, value);
  if (result != cfg::SetResult::OK) noteResult(key, result);
  repaint();
  return result == cfg::SetResult::OK;
}

void type(const char *key, const char *title) {
  const cfg::ConfigKey *k = cfg::find(key);
  if (k == nullptr) return;
  const How h = how(*k);
  if (h != How::TYPE && h != How::STEP) return;
  strlcpy(sTypeKey, key, sizeof sTypeKey);

  // The limits are the key's own; the keyboard holds at most TEXT_MAX characters.
  char hint[52];
  kb::Options options;
  options.title = title;
  if (k->type == cfg::Type::U32) {
    snprintf(hint, sizeof hint, "a number from %u to %u", (unsigned)k->min, (unsigned)k->max);
    options.minLen = 1;
    options.maxLen = 10;
  } else {
    const uint32_t most = k->max < kb::TEXT_MAX ? k->max : kb::TEXT_MAX;
    options.minLen = (uint8_t)(k->min < most ? k->min : most);
    options.maxLen = (uint8_t)most;
    options.emptyOk = k->min == 0;
    if (k->min == 0) {
      snprintf(hint, sizeof hint, "up to %u characters; empty: the default", (unsigned)most);
    } else {
      snprintf(hint, sizeof hint, "%u to %u characters", (unsigned)k->min, (unsigned)most);
    }
  }
  options.hint = hint;   // the keyboard copies the title, the hint and the initial text
  const String current = cfg::text(key);
  options.initial = current.c_str();
  kb::open(options, typedDone);
}

void choose(const char *key, const char *title) {
  const Choices *c = choicesFor(key);
  const cfg::ConfigKey *k = cfg::find(key);
  if (c == nullptr || k == nullptr || how(*k) != How::CHOOSE) return;
  strlcpy(sChooseKey, key, sizeof sChooseKey);
  const String current = cfg::text(key);
  pick(title, c->count(), c->at, current.c_str(), chosen);
}

// ---- the picker ---------------------------------------------------------------------------------

namespace {

char sPickTitle[24] = "";
size_t sPickCount = 0;
At sPickAt = nullptr;
Picked sPicked = nullptr;
char sPickCurrent[VALUE_CAP] = "";
List sPickList;

void pickUpdate() {
  if (back()) return;   // CANCEL: nothing chosen
  listMove(sPickList, (int)sPickCount);
  if (!buttons::pressed(BTN_A) || sPickCount == 0 || sPickAt == nullptr) return;
  char value[VALUE_CAP], label[LABEL_CAP];
  if (!sPickAt((size_t)sPickList.cursor, value, sizeof value, label, sizeof label)) return;
  const Picked picked = sPicked;
  pop();
  if (picked != nullptr) picked(value);
}

void pickDraw() {
  frame(sPickTitle, "SELECT choose");
  if (sPickCount == 0 || sPickAt == nullptr) {
    textCentered(160, 104, "Nothing to choose from.", tk::SUB);
    return;
  }
  // Only the rows on screen are read: an offset list has 105 choices.
  static char values[LIST_ROWS][VALUE_CAP];
  static char labels[LIST_ROWS][LABEL_CAP];
  ListRow rows[LIST_ROWS];
  int shown = 0;
  for (int i = 0; i < LIST_ROWS && (size_t)(sPickList.scroll + i) < sPickCount; ++i) {
    values[i][0] = labels[i][0] = '\0';
    sPickAt((size_t)(sPickList.scroll + i), values[i], VALUE_CAP, labels[i], LABEL_CAP);
    const bool current = strcmp(values[i], sPickCurrent) == 0;
    rows[i] = {labels[i][0] ? labels[i] : values[i], current ? "current" : "",
               current ? tk::color(tk::STAMP_OK) : (uint16_t)0};
    ++shown;
  }
  List window;
  window.cursor = sPickList.cursor - sPickList.scroll;
  listDraw(window, rows, shown, LIST_Y, LIST_ROWS);
  if (sPickCount > (size_t)LIST_ROWS) {
    char position[16];
    snprintf(position, sizeof position, "%d/%u", sPickList.cursor + 1, (unsigned)sPickCount);
    textRight(X1, TITLE_Y + 2, position, tk::FAINT);
  }
}

const Screen kPick = {"pick", nullptr, pickUpdate, pickDraw, 0};

Screen sJumps[JUMP_SLOTS];
int sJumpNext = 0;

}  // namespace

void pick(const char *title, size_t count, At at, const char *current, Picked picked) {
  strlcpy(sPickTitle, title != nullptr ? title : "", sizeof sPickTitle);
  sPickCount = count;
  sPickAt = at;
  sPicked = picked;
  strlcpy(sPickCurrent, current != nullptr ? current : "", sizeof sPickCurrent);
  // Open on the current choice, a few rows from the top when the list scrolls.
  sPickList = List();
  char value[VALUE_CAP], label[LABEL_CAP];
  for (size_t i = 0; at != nullptr && i < count; ++i) {
    if (at(i, value, sizeof value, label, sizeof label) && strcmp(value, sPickCurrent) == 0) {
      sPickList.cursor = (int)i;
      break;
    }
  }
  int scroll = sPickList.cursor - LIST_ROWS / 2;
  const int last = (int)count > LIST_ROWS ? (int)count - LIST_ROWS : 0;
  if (scroll > last) scroll = last;
  if (scroll < 0) scroll = 0;
  sPickList.scroll = scroll;
  push(&kPick);
}

// ---- jumps --------------------------------------------------------------------------------------

bool openPage(const char *id) {
  if (id == nullptr) return false;
  for (const SettingsPage *page = Registered<SettingsPage>::first(); page != nullptr; page = page->next()) {
    if (page->id == nullptr || strcmp(page->id, id) != 0) continue;
    if (page->action != nullptr) {
      page->action();
      repaint();
      return true;
    }
    if (page->draw == nullptr) return false;
    // A slot of its own: the screen that jumped stays on the stack under this one, and may itself
    // have been opened by a jump.
    Screen &screen = sJumps[sJumpNext];
    sJumpNext = (sJumpNext + 1) % JUMP_SLOTS;
    screen = {page->id, page->enter, page->update, page->draw, page->refresh_ms};
    push(&screen);
    return true;
  }
  return false;
}

}  // namespace vk::shell::edit
