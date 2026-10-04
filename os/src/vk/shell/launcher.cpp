// Screen `launcher`: the MENU screen (docs/os/ui/shell.md, "Launcher"). Every listed app, Lua and
// native, in a two-column grid, with one folder row per category; the inbox count; the balance
// row; a QR code of the project's repository (config key `repo_url`). It is the bottom of the
// screen stack and is never re-entered, so its cursor and its open folder survive an app run.
//
// The rows come from vk::host::catalog (the apps and their categories, hidden apps left out). No
// category is named here: a folder exists because an app's manifest names it.
#include <ctype.h>
#include <stdio.h>

#include "../../apps/app_store.h"
#include "../../hal/leds.h"
#include "../../lua_sdk/lua_runtime.h"
#include "../core/config.h"
#include "../host/catalog.h"
#include "../host/native.h"
#include "../host/notify.h"
#include "../wallet/signer.h"
#include "page.h"

namespace vk::shell {

namespace {

constexpr int GRID_ROWS = 5;                 // rows at y = 48, 66, 84, 102, 120
constexpr int STRIP_RULE_Y = 138;            // the rule over the balance and the QR code
constexpr int QR_SIZE = 74;                  // 2 px modules and a 4-module quiet zone up to 53 characters (29 modules)
constexpr int QR_X = X1 - QR_SIZE, QR_Y = 140; // ends at 213, above the footer's rule at 216
constexpr int BALANCE_X1 = QR_X - 8;         // the balance row stops short of the code
constexpr int BALANCE_Y = 174;               // centred in the strip 139..215
constexpr int GRID_COLS = 2;
constexpr int COL_X0[GRID_COLS] = {10, 170};
constexpr int COL_X1[GRID_COLS] = {150, 310};
constexpr int NAME_COLS = 16;                // "NN " + 16 leaves the cell room for the mark or a count
constexpr int MAX_ROWS = 48;                 // catalog::count() is at most 48
constexpr uint32_t COLUMN_TAP_MS = 600;      // RIGHT released within this: move to the right column
constexpr uint32_t DELETE_HOLD_MS = 800;     // RIGHT held this long: the delete confirmation
const char MARK_SELECTED[] = "\xE2\x97\x82"; // U+25C2, the small left triangle; the kit draws it
const char FOLDER_KEY[] = "folder:";         // how VKSTATE names a folder row

// One cell of the grid: an app (an index into the catalog) or a folder (a category and its size).
struct Row {
  int app;                                   // catalog index, or -1 for a folder
  String category;                           // the folder's category ("" for an app)
  int size;                                  // a folder: how many apps it holds
};

Row sRows[MAX_ROWS];
int sRowCount = 0;
String sFolder;                              // "" = the top level; else the open folder's category
int sCursor = 0;                             // index of the selected row
int sScrollRow = 0;                          // first grid row shown
int sTopCursor = 0;                          // the top level's cursor while a folder is open
uint32_t sBuiltGeneration = 0;               // catalog::generation() the rows were built from
bool sBuilt = false;
size_t sDrawnNotes = (size_t)-1;             // what the last draw showed: a change repaints
bool sRightDown = false;                     // a RIGHT press that began on this screen is still down
bool sRightUsed = false;                     // ... and it already opened (or refused) the delete: ignore its release
uint32_t sRightAt = 0;

// Rebuilds sRows for the current level. The top level is every app with no category, in the
// catalog's order, then one row per category in alphabetical order. A folder is its apps, in the
// catalog's order.
void build() {
  using namespace vk::host;
  sRowCount = 0;
  const size_t count = catalog::count();
  for (size_t i = 0; i < count && sRowCount < MAX_ROWS; ++i) {
    const catalog::Entry *entry = catalog::at(i);
    if (entry == nullptr || entry->category != sFolder) continue;
    sRows[sRowCount++] = Row{(int)i, String(), 0};
  }
  if (sFolder.length() == 0) {
    // The categories, each once, sorted: an insertion sort into the rows after the apps.
    const int firstFolder = sRowCount;
    for (size_t i = 0; i < count; ++i) {
      const catalog::Entry *entry = catalog::at(i);
      if (entry == nullptr || entry->category.length() == 0) continue;
      int at = firstFolder;
      while (at < sRowCount && sRows[at].category < entry->category) ++at;
      if (at < sRowCount && sRows[at].category == entry->category) {
        ++sRows[at].size;
        continue;
      }
      if (sRowCount >= MAX_ROWS) continue;
      for (int j = sRowCount; j > at; --j) sRows[j] = sRows[j - 1];
      sRows[at] = Row{-1, entry->category, 1};
      ++sRowCount;
    }
  }
  sBuiltGeneration = catalog::generation();
  sBuilt = true;
}

// The rows, rebuilt when the catalog changed (an install, a delete, a new app.ini). A folder whose
// last app went away closes. Returns true when the rows were rebuilt.
bool ensureRows() {
  if (sBuilt && vk::host::catalog::generation() == sBuiltGeneration) return false;
  build();
  if (sFolder.length() && sRowCount == 0) {
    sFolder = "";
    sCursor = sTopCursor;
    build();
  }
  return true;
}

void clampCursor(int count) {
  if (count <= 0) {
    sCursor = 0;
    sScrollRow = 0;
    return;
  }
  if (sCursor >= count) sCursor = count - 1;
  if (sCursor < 0) sCursor = 0;
  const int row = sCursor / GRID_COLS;
  if (row < sScrollRow) sScrollRow = row;
  if (row >= sScrollRow + GRID_ROWS) sScrollRow = row - GRID_ROWS + 1;
  if (sScrollRow < 0) sScrollRow = 0;
}

// The app of a row, or nullptr for a folder.
const vk::host::catalog::Entry *appOf(int row) {
  if (row < 0 || row >= sRowCount || sRows[row].app < 0) return nullptr;
  return vk::host::catalog::at((size_t)sRows[row].app);
}

void openFolder(const String &category) {
  sTopCursor = sCursor;
  sFolder = category;
  sCursor = 0;
  sScrollRow = 0;
  build();
  repaint();
}

void closeFolder() {
  sFolder = "";
  build();
  sCursor = sTopCursor;
  sScrollRow = 0;
  clampCursor(sRowCount);
  repaint();
}

void launch(const String &id) {
  ::leds::stopAnimation();
  appFromSettings(false);
  runtime::requestLaunch(id);
}

// One row up or down, staying in the column; wraps between the first and the last row and is
// clamped to the last cell (an odd count leaves the last row's right cell empty).
void moveRow(int count, int delta) {
  const int rows = (count + GRID_COLS - 1) / GRID_COLS;
  int row = sCursor / GRID_COLS + delta;
  const int col = sCursor % GRID_COLS;
  if (row < 0) row = rows - 1;
  if (row >= rows) row = 0;
  int next = row * GRID_COLS + col;
  if (next >= count) next = count - 1;
  if (next != sCursor) {
    sCursor = next;
    repaint();
  }
}

// RIGHT does two things: a short press moves to the right column, a hold opens the delete
// confirmation. Returns true when the screen was left.
bool updateRight(int count) {
  if (buttons::pressed(BTN_RIGHT)) {
    sRightDown = true;
    sRightUsed = false;
    sRightAt = millis();
  }
  if (!sRightDown) return false;

  if (buttons::released(BTN_RIGHT) || !buttons::down(BTN_RIGHT)) {
    const bool tap = !sRightUsed && (millis() - sRightAt) <= COLUMN_TAP_MS;
    sRightDown = false;
    if (tap && sCursor % GRID_COLS == 0 && sCursor + 1 < count) {
      ++sCursor;
      repaint();
    }
    return false;
  }

  if (!sRightUsed && (millis() - sRightAt) >= DELETE_HOLD_MS) {
    sRightUsed = true;                       // the release that follows is ignored
    // A folder is not an app, and a native app is part of the firmware: neither can be deleted.
    const vk::host::catalog::Entry *entry = appOf(sCursor);
    if (entry != nullptr && !vk::host::native::exists(entry->id)) {
      sRightDown = false;
      appDeleteOpen(entry->id, entry->name);
      return true;
    }
  }
  return false;
}

void update() {
  if (ensureRows()) repaint();
  const int count = sRowCount;
  clampCursor(count);

  if (count > 0) {
    if (buttons::repeated(BTN_UP)) moveRow(count, -1);
    else if (buttons::repeated(BTN_DOWN)) moveRow(count, +1);

    if (buttons::pressed(BTN_LEFT) && sCursor % GRID_COLS == 1) {
      --sCursor;
      repaint();
    }
  }
  if (updateRight(count)) return;

  if (buttons::pressed(BTN_A) && count > 0) {
    const Row &row = sRows[sCursor];
    if (row.app >= 0) {
      const vk::host::catalog::Entry *entry = appOf(sCursor);
      if (entry != nullptr) launch(entry->id);
    } else if (row.size == 1) {
      // A folder of one app is that app: no screen with a single cell in between.
      const size_t total = vk::host::catalog::count();
      for (size_t i = 0; i < total; ++i) {
        const vk::host::catalog::Entry *entry = vk::host::catalog::at(i);
        if (entry != nullptr && entry->category == row.category) {
          launch(entry->id);
          break;
        }
      }
    } else {
      openFolder(row.category);
    }
    return;
  }
  if (buttons::pressed(BTN_B)) {
    sRightDown = false;
    if (sFolder.length()) {
      closeFolder();
    } else {
      push(&kSettings);
    }
    return;
  }

  // A new notification changes the inbox cell.
  if (vk::host::notify::count() != sDrawnNotes) repaint();
}

void drawBalance() {
  using namespace vk::ui;
  if (!vk::config::provisioned()) {
    receipt::row(X0, BALANCE_X1, BALANCE_Y, "SETUP NEEDED", "provision over USB", false, theme::color(theme::STAMP_WARN));
    return;
  }
  // The default token is the first entry of the token table.
  vk_token_t tokens[VK_MAX_TOKENS];
  const size_t have = vk::config::tokens(tokens);
  char value[40] = "--";
  if (have > 0) {
    vk::wallet::TokenInfo token = {};
    char amount[24];
    // A null pointer means the balance feature is not compiled in.
    if (vk::wallet::tokenInfoLookup != nullptr && vk::wallet::tokenInfoLookup(tokens[0].mint, token) &&
        token.balance_known && sol_format_amount(token.raw, tokens[0].decimals, amount, sizeof amount) > 0) {
      snprintf(value, sizeof value, "%s %s", amount, tokens[0].symbol);
    } else {
      snprintf(value, sizeof value, "-- %s", tokens[0].symbol);
    }
  }
  receipt::row(X0, BALANCE_X1, BALANCE_Y, "BALANCE", value);
}

// `name` in capitals after `prefix`, cut to NAME_COLS characters.
void upperName(char *out, size_t cap, const char *prefix, const String &name) {
  int n = snprintf(out, cap, "%s", prefix);
  for (unsigned i = 0; i < name.length() && i < (unsigned)NAME_COLS && n < (int)cap - 1; ++i) {
    out[n++] = (char)toupper((unsigned char)name[i]);
  }
  out[n] = '\0';
}

void draw() {
  using namespace vk::ui;
  ensureRows();
  const int count = sRowCount;
  const size_t notes = vk::host::notify::count();
  clampCursor(count);
  sDrawnNotes = notes;

  char right[40];
  receipt::page();
  receipt::statusRight(right, sizeof right);
  receipt::header("BADGEOS", right);
  if (sFolder.length()) {
    char title[8 + NAME_COLS];
    upperName(title, sizeof title, "", sFolder);    // an open folder's title is its name
    receipt::title(title, TITLE_Y);
  } else {
    receipt::title("MENU", TITLE_Y);
  }

  if (count == 0) {
    // Not reachable while the native apps are compiled in.
    textCentered(160, 84, "NO APPS INSTALLED");
    textCentered(160, 102, "push one: Settings > App push", theme::SUB);
  }

  for (int r = 0; r < GRID_ROWS; ++r) {
    for (int c = 0; c < GRID_COLS; ++c) {
      const int index = (sScrollRow + r) * GRID_COLS + c;
      if (index >= count) continue;
      const Row &row = sRows[index];
      const vk::host::catalog::Entry *entry = appOf(index);
      if (row.app >= 0 && entry == nullptr) continue;
      const bool selected = index == sCursor;

      // "NN NAME": the cell's number with two digits, then the app's name or the folder's.
      char number[8];
      snprintf(number, sizeof number, "%02d ", index + 1);
      char label[8 + NAME_COLS];
      upperName(label, sizeof label, number, entry != nullptr ? entry->name : row.category);

      // A count stays when its cell is the selected one: "2 <" (the count, then the mark). A
      // folder's count is its number of apps; an app with count=notes in its manifest (the Inbox)
      // shows the number of waiting notifications.
      unsigned shown = 0;
      if (entry == nullptr) shown = (unsigned)row.size;
      else if (entry->countNotes && notes > 0) shown = (unsigned)(notes > 99 ? 99 : notes);
      char value[16] = "";
      if (selected && shown) {
        snprintf(value, sizeof value, "%u %s", shown, MARK_SELECTED);
      } else if (selected) {
        snprintf(value, sizeof value, "%s", MARK_SELECTED);
      } else if (shown) {
        snprintf(value, sizeof value, "%u", shown);
      }
      receipt::row(COL_X0[c], COL_X1[c], LIST_Y + r * ROW_PITCH, label, value, selected);
    }
  }

  if (count > GRID_ROWS * GRID_COLS) {
    char position[16];
    snprintf(position, sizeof position, "%d/%d", sCursor + 1, count);
    textRight(X1, 28, position, theme::FAINT);
  }

  receipt::rule(STRIP_RULE_Y);
  drawBalance();
  // Nothing is drawn when the link is not set or too long for the square.
  receipt::qr(QR_X, QR_Y, QR_SIZE, vk::config::text("repo_url").c_str());
  receipt::footer("SELECT open", sFolder.length() ? "CANCEL back" : "CANCEL settings");
}

}  // namespace

const Screen kLauncher = {"launcher", nullptr, update, draw, 0};

const char *launcherFolder() { return sFolder.c_str(); }

int launcherCursor() {
  ensureRows();
  return sCursor;
}

int launcherRowCount() {
  ensureRows();
  return sRowCount;
}

String launcherRowKey(int index) {
  ensureRows();
  if (index < 0 || index >= sRowCount) return String();
  const vk::host::catalog::Entry *entry = appOf(index);
  if (entry != nullptr) return entry->id;
  return String(FOLDER_KEY) + sRows[index].category;
}

}  // namespace vk::shell
