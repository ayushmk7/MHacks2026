// Screen `settings`: the registered settings pages, sorted by order (docs/os/ui/shell.md,
// "Settings list" and "Settings page registry"). No page is named here: a page is one file under
// pages/ that registers itself with VK_SETTINGS_PAGE or VK_SETTINGS_ACTION.
#include <string.h>

#include "page.h"

namespace vk::shell {

namespace {

constexpr int MAX_PAGES = 24;
constexpr size_t VALUE_CAP = 28;

const SettingsPage *sPages[MAX_PAGES];
int sCount = 0;
List sList;
Screen sPageScreen = {"", nullptr, nullptr, nullptr, 0};   // the one open page: only one is open at a time

bool before(const SettingsPage *a, const SettingsPage *b) {
  if (a->order != b->order) return a->order < b->order;
  return strcmp(a->id ? a->id : "", b->id ? b->id : "") < 0;
}

// The registry is a linked list in link order; the screen shows it sorted by order, ties by id.
void collect() {
  sCount = 0;
  for (const SettingsPage *page = Registered<SettingsPage>::first(); page != nullptr; page = page->next()) {
    if (sCount == MAX_PAGES) break;
    int at = sCount;
    while (at > 0 && before(page, sPages[at - 1])) {
      sPages[at] = sPages[at - 1];
      --at;
    }
    sPages[at] = page;
    ++sCount;
  }
}

bool sKeepCursor = false;                   // re-entered after an app that a row of this list started

void enter() {
  collect();
  if (!sKeepCursor) sList = List();
  sKeepCursor = false;
}

void update() {
  if (back()) {                              // CANCEL: the launcher
    appFromSettings(false);
    sKeepCursor = false;
    return;
  }
  listMove(sList, sCount);
  if (!buttons::pressed(BTN_A) || sCount == 0 || sList.cursor < 0 || sList.cursor >= sCount) return;

  const SettingsPage *page = sPages[sList.cursor];
  if (page->action != nullptr) {             // an action row: act in place, the list stays
    appFromSettings(true);                   // if it starts an app, that app comes back here
    sKeepCursor = true;
    page->action();
    repaint();
    return;
  }
  if (page->draw == nullptr) return;
  sPageScreen.name = page->id;
  sPageScreen.enter = page->enter;
  sPageScreen.update = page->update;
  sPageScreen.draw = page->draw;
  sPageScreen.refresh_ms = page->refresh_ms;
  push(&sPageScreen);
}

void draw() {
  frame("SETTINGS", "SELECT open");
  // The rows hold pointers into `values`, which outlives the listDraw call.
  static char values[MAX_PAGES][VALUE_CAP];
  ListRow rows[MAX_PAGES];
  for (int i = 0; i < sCount; ++i) {
    values[i][0] = '\0';
    if (sPages[i]->value != nullptr) sPages[i]->value(values[i], VALUE_CAP);
    values[i][VALUE_CAP - 1] = '\0';
    rows[i] = {sPages[i]->title ? sPages[i]->title : sPages[i]->id, values[i], 0};
  }
  listDraw(sList, rows, sCount);
}

}  // namespace

// Row values are live (Wi-Fi state, peers, notifications): repaint every second.
const Screen kSettings = {"settings", enter, update, draw, 1000};

}  // namespace vk::shell
