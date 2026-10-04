// Screen `launcher`: the MENU screen (docs/os/ui/shell.md, "Launcher"). Every installed app, Lua
// and native, in a two-column grid; the inbox count; the balance row; the barcode of the badge key.
// It is the bottom of the screen stack and is never re-entered, so its cursor survives an app run.
#include <ctype.h>
#include <stdio.h>

#include "../../apps/app_store.h"
#include "../../hal/leds.h"
#include "../../lua_sdk/lua_runtime.h"
#include "../core/config.h"
#include "../host/native.h"
#include "../host/notify.h"
#include "../wallet/signer.h"
#include "page.h"

namespace vk::shell {

namespace {

constexpr int GRID_ROWS = 6;                 // rows at y = 48, 66, 84, 102, 120, 138
constexpr int GRID_COLS = 2;
constexpr int COL_X0[GRID_COLS] = {10, 170};
constexpr int COL_X1[GRID_COLS] = {150, 310};
constexpr int NAME_COLS = 16;                // "NN " + 16 leaves the cell room for the mark or the inbox count
constexpr uint32_t COLUMN_TAP_MS = 600;      // RIGHT released within this: move to the right column
constexpr uint32_t DELETE_HOLD_MS = 800;     // RIGHT held this long: the delete confirmation
const char MARK_SELECTED[] = "\xE2\x97\x82"; // U+25C2, the small left triangle; the kit draws it

int sCursor = 0;                             // index of the selected app
int sScrollRow = 0;                          // first grid row shown
size_t sDrawnApps = (size_t)-1;              // what the last draw showed: a change repaints
size_t sDrawnNotes = (size_t)-1;
bool sRightDown = false;                     // a RIGHT press that began on this screen is still down
bool sRightUsed = false;                     // ... and it already opened (or refused) the delete: ignore its release
uint32_t sRightAt = 0;

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

// One row up or down, staying in the column; wraps between the first and the last row and is
// clamped to the last app (an odd count leaves the last row's right cell empty).
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
    app_store::Info info;
    // A native app is part of the firmware and cannot be deleted: nothing happens.
    if (count > 0 && app_store::at((size_t)sCursor, info) && !vk::host::native::exists(info.id)) {
      sRightDown = false;
      appDeleteOpen(info.id, info.name.length() ? info.name : info.id);
      return true;
    }
  }
  return false;
}

void update() {
  const int count = (int)app_store::count();
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
    app_store::Info info;
    if (app_store::at((size_t)sCursor, info)) {
      ::leds::stopAnimation();
      runtime::requestLaunch(info.id);
    }
    return;
  }
  if (buttons::pressed(BTN_B)) {
    sRightDown = false;
    push(&kSettings);
    return;
  }

  // An install or a delete arriving over the network, or a new notification.
  if (app_store::count() != sDrawnApps || vk::host::notify::count() != sDrawnNotes) repaint();
}

void drawBalance() {
  using namespace vk::ui;
  if (!vk::config::provisioned()) {
    receipt::row(X0, X1, 166, "SETUP NEEDED", "provision over USB", false, theme::color(theme::STAMP_WARN));
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
  receipt::row(X0, X1, 166, "BALANCE", value);
}

void draw() {
  using namespace vk::ui;
  const int count = (int)app_store::count();
  const size_t notes = vk::host::notify::count();
  clampCursor(count);
  sDrawnApps = (size_t)count;
  sDrawnNotes = notes;

  char right[40];
  receipt::page();
  receipt::statusRight(right, sizeof right);
  receipt::header("BADGEOS", right);
  receipt::title("MENU", TITLE_Y);

  if (count == 0) {
    // Not reachable while the native apps are compiled in.
    textCentered(160, 84, "NO APPS INSTALLED");
    textCentered(160, 102, "push one: Settings > App push", theme::SUB);
  }

  app_store::Info info;
  for (int r = 0; r < GRID_ROWS; ++r) {
    for (int c = 0; c < GRID_COLS; ++c) {
      const int index = (sScrollRow + r) * GRID_COLS + c;
      if (index >= count || !app_store::at((size_t)index, info)) continue;
      const bool selected = index == sCursor;

      char label[8 + NAME_COLS];
      int n = snprintf(label, sizeof label, "%02d ", index + 1);
      const String &name = info.name.length() ? info.name : info.id;
      for (unsigned i = 0; i < name.length() && i < (unsigned)NAME_COLS && n < (int)sizeof label - 1; ++i) {
        label[n++] = (char)toupper((unsigned char)name[i]);
      }
      label[n] = '\0';

      // The inbox cell keeps its count when it is the selected one: "2 <" (the count, then the mark).
      char value[16] = "";
      const bool counted = info.id == "inbox" && notes > 0;
      if (selected && counted) {
        snprintf(value, sizeof value, "%u %s", (unsigned)(notes > 99 ? 99 : notes), MARK_SELECTED);
      } else if (selected) {
        snprintf(value, sizeof value, "%s", MARK_SELECTED);
      } else if (counted) {
        snprintf(value, sizeof value, "%u", (unsigned)notes);
      }
      receipt::row(COL_X0[c], COL_X1[c], LIST_Y + r * ROW_PITCH, label, value, selected);
    }
  }

  if (count > GRID_ROWS * GRID_COLS) {
    char position[16];
    snprintf(position, sizeof position, "%d/%d", sCursor + 1, count);
    textRight(X1, 28, position, theme::FAINT);
  }

  receipt::rule(156);
  drawBalance();
  const uint8_t *key = vk::wallet::publicKey();
  if (key != nullptr) receipt::barcode(X0, 184, 300, 22, key, 32);   // not drawn when the badge has no identity
  receipt::footer("SELECT open", "CANCEL settings");
}

}  // namespace

const Screen kLauncher = {"launcher", nullptr, update, draw, 0};

}  // namespace vk::shell
