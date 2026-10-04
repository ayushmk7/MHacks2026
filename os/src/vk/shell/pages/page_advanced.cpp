// Settings page `advanced` (docs/os/ui/shell.md, "Advanced"): every config key that is not secure,
// read from the config registry, with its value, and changed in place: a number with LEFT/RIGHT (or
// typed), a key with registered choices with a picker, a text with the keyboard. No key is named in
// this file except `theme`, whose choices (the registered themes) it registers: a key added to the
// firmware tomorrow appears here with no edit.
//
// Secure keys are not listed (they change only over USB, with a hold on the badge). Required keys
// (the RPC address) are listed but read-only: they come from the provision command, with the rest of
// the wallet's root of trust. Ranges, lengths and rules are the key's own (vk::config::set).

#include "../page.h"

#include "../../core/config.h"
#include "../edit.h"

namespace {
using namespace vk::shell;
namespace tk = vk::ui::theme;
namespace cfg = vk::config;
namespace edit = vk::shell::edit;

constexpr int MAX_KEYS = 64;
constexpr int VISIBLE = 8;          // rows 48 .. 174; the key's help under them
constexpr int HELP_Y = 194;
constexpr uint32_t CHECK_MS = 500;

const cfg::ConfigKey *sKeys[MAX_KEYS];
int sCount = 0;
List sList;
uint32_t sDrawn = 0;                // hash of what the last draw showed
uint32_t sCheckedAt = 0;

// ---- the theme's choices: the registered themes ----------------------------------------------------

size_t themeCount() { return tk::count(); }

bool themeAt(size_t index, char *value, size_t valueCap, char *label, size_t labelCap) {
  const tk::Theme *theme = tk::at(index);
  if (theme == nullptr) return false;
  strlcpy(value, theme->name, valueCap);
  strlcpy(label, theme->name, labelCap);
  return true;
}

VK_KEY_CHOICES(theme, "theme", themeCount, themeAt);

// ---- the list -------------------------------------------------------------------------------------

void collect() {
  sCount = 0;
  for (const cfg::ConfigKey *key = cfg::ConfigKey::first(); key != nullptr; key = key->next()) {
    if (key->flags & cfg::F_SECURE) continue;
    if (sCount == MAX_KEYS) break;
    int at = sCount;
    while (at > 0 && strcmp(key->name, sKeys[at - 1]->name) < 0) {
      sKeys[at] = sKeys[at - 1];
      --at;
    }
    sKeys[at] = key;
    ++sCount;
  }
}

const cfg::ConfigKey *current() {
  return sList.cursor >= 0 && sList.cursor < sCount ? sKeys[sList.cursor] : nullptr;
}

uint32_t hashAll() {
  uint32_t h = 2166136261u;   // FNV-1a over every value, the cursor's key and the note
  auto mix = [&h](const char *s) {
    for (; *s; ++s) h = (h ^ (uint8_t)*s) * 16777619u;
    h = (h ^ 0xFF) * 16777619u;
  };
  for (int i = 0; i < sCount; ++i) mix(cfg::text(sKeys[i]->name).c_str());
  mix(edit::note() != nullptr ? "n" : "-");
  return h;
}

void pageValue(char *out, size_t cap) { snprintf(out, cap, "all settings"); }

void pageEnter() {
  collect();
  sList = List();
  sDrawn = 0;
  sCheckedAt = millis();
}

void pageUpdate() {
  if (back()) return;  // CANCEL
  listMove(sList, sCount, VISIBLE);
  const cfg::ConfigKey *key = current();
  if (key != nullptr) {
    const edit::How how = edit::how(*key);
    int direction = 0;
    if (buttons::pressed(BTN_LEFT)) direction = -1;
    if (buttons::pressed(BTN_RIGHT)) direction = 1;
    const bool select = buttons::pressed(BTN_A);
    switch (how) {
      case edit::How::STEP:
        if (direction != 0) edit::step(key->name, direction);
        if (select) edit::type(key->name, key->name);
        break;
      case edit::How::CHOOSE:
        if (direction != 0) edit::cycle(key->name, direction);
        if (select) edit::choose(key->name, key->name);
        break;
      case edit::How::TYPE:
        if (select) edit::type(key->name, key->name);
        break;
      case edit::How::LAPTOP:
        break;
    }
  }
  // A VKSET while the page is open, a note running out: drawn only when something changed.
  if (millis() - sCheckedAt < CHECK_MS) return;
  sCheckedAt = millis();
  if (hashAll() != sDrawn) repaint();
}

void pageDraw() {
  sDrawn = hashAll();
  const cfg::ConfigKey *key = current();
  const edit::How how = key != nullptr ? edit::how(*key) : edit::How::LAPTOP;
  const char *foot = "";
  switch (how) {
    case edit::How::STEP:   foot = "LEFT/RIGHT change  SELECT type"; break;
    case edit::How::CHOOSE: foot = "LEFT/RIGHT change  SELECT list"; break;
    case edit::How::TYPE:   foot = "SELECT type"; break;
    case edit::How::LAPTOP: foot = key != nullptr ? "set from a laptop (USB)" : ""; break;
  }
  frame("ADVANCED", foot);
  if (sCount == 0) {
    textCentered(160, 104, "No settings.", tk::SUB);
    return;
  }

  // Only the rows on screen are read; the strings outlive the listDraw call.
  static char values[VISIBLE][40];
  ListRow rows[MAX_KEYS];
  const uint16_t sub = tk::color(tk::SUB);
  for (int i = 0; i < sCount; ++i) rows[i] = {sKeys[i]->name, "", 0};
  for (int i = 0; i < VISIBLE && sList.scroll + i < sCount; ++i) {
    const cfg::ConfigKey *k = sKeys[sList.scroll + i];
    const String text = cfg::text(k->name);
    strlcpy(values[i], text.length() ? text.c_str() : "(empty)", sizeof values[i]);
    const bool readOnly = edit::how(*k) == edit::How::LAPTOP;
    rows[sList.scroll + i] = {k->name, values[i], (readOnly || text.length() == 0) ? sub : (uint16_t)0};
  }
  listDraw(sList, rows, sCount, LIST_Y, VISIBLE);

  bool bad = false;
  const char *note = edit::note(&bad);
  if (note != nullptr) {
    text(X0, HELP_Y, note, bad ? tk::STAMP_WARN : tk::STAMP_OK);
  } else if (key != nullptr && key->help != nullptr) {
    text(X0, HELP_Y, key->help, tk::SUB);
  }
}
}  // namespace

VK_SETTINGS_PAGE(advanced, "advanced", 146, "Advanced", pageValue, pageEnter, pageUpdate, pageDraw, 0);
