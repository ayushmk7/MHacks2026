// src/native_apps/wallet_settings/wallet_settings.cpp
// The Wallet app (apps.md, "Wallet (native)"): five pages showing what this badge is provisioned
// with, and the wallet reset. Read-only: the one action is the Reset page's SELECT, which raises the
// firmware's own confirmation (vk::config::requestReset).
//
// Keys: LEFT/RIGHT change page (the pages wrap), UP/DOWN scroll, SELECT acts on the Reset page only,
// CANCEL exits.
//
// Screen (ui.md, "Screens: any list"), drawn only with the receipt kit:
//   header   BADGEOS / time and battery
//   title    the page's name, capitals at y 26..36
//   lines    y 41..202: nine rows (pitch 18, the first at y = 46), or five rows with a subline each
//            (pitch 31); the same positions as the Inbox app's list
//   footer   "< WALLET n/5 >" and the key hint / CANCEL back
//
// Nothing is stored between frames. A page is a function that names its lines in order; the Painter
// skips the lines scrolled away, draws the ones that fit and notes whether any were left over. The
// values are read again on every repaint, which happens after a key, once a second (the clock, the
// balance or a VKSET over USB may have changed something), and when the app comes back from a pause
// (an approval was on the screen and has left its picture in the canvas).
#include "../../vk/sdk/badge_sdk.hpp"

#include <stdio.h>
#include <string.h>

#include "../../vk/host/consent.h"
#include "../../vk/ui/receipt.h"
#include "../../vk/ui/theme.h"

namespace {

namespace rc = vk::ui::receipt;
namespace th = vk::ui::theme;       // ours; upstream's palette is ::theme

// ---- layout (ui.md, "The receipt kit": margins 10, row pitch 18, subline 13) ----------------------
constexpr int MARGIN = 10;
constexpr int CHAR_W = 6;            // a Font0 column
constexpr int TITLE_Y = 26;
constexpr int LIST_TOP = 41;         // the first row owns y 41..58, so its text is at y = 46
constexpr int LIST_BOTTOM = 203;     // nine rows end at y 202; the footer's rule is at y = 216
constexpr int ROW_PITCH = 18;
constexpr int ROW_ABOVE = 5;         // a row at y owns y-5 .. y+12
constexpr int SUBLINE_H = 13;
constexpr int SUBLINE_INDENT = 12;

// The kit draws these two when they are written as UTF-8 (U+25C2, U+25B8).
#define WS_TRI_LEFT "\xE2\x97\x82"
#define WS_TRI_RIGHT "\xE2\x96\xB8"

// What the screen shows for a value that is not known. apps.md writes a long dash; the kit draws
// only ASCII, the middle dot and the two triangles, so this is the launcher's "--".
constexpr char UNKNOWN[] = "--";

enum Page : uint8_t { PAGE_STATUS, PAGE_TOKENS, PAGE_CONFIG, PAGE_APPS, PAGE_RESET, PAGE_COUNT };
const char *const PAGE_TITLES[PAGE_COUNT] = {"STATUS", "TOKENS", "CONFIG", "APPS", "RESET"};

constexpr float REFRESH_S = 1.0f;        // repaint period of a page that is not being touched
constexpr float REFRESH_APPS_S = 5.0f;   // the Apps page reads the consent store: less often
constexpr float PAUSE_GAP_S = 0.25f;     // a frame this late means the app was paused (an approval was up)

constexpr size_t MAX_SORTED = 64;        // config keys or native apps listed on one page

// ---- the list painter -----------------------------------------------------------------------------
class Painter {
 public:
  explicit Painter(int skipRows) : skip_(skipRows), x1_(display::width() - MARGIN) {}

  int rowCols() const { return (x1_ - MARGIN) / CHAR_W; }                        // 50
  int sublineCols() const { return (x1_ - MARGIN - SUBLINE_INDENT) / CHAR_W; }   // 48

  // One row: label left, value right, a dotted leader between. `valueColor` 0 = ink.
  void row(const char *label, const char *value, uint16_t valueColor = 0, bool selected = false) {
    ++rows_;
    if (rows_ <= skip_) return;
    if (full_ || top_ + ROW_PITCH > LIST_BOTTOM) { full_ = true; return; }
    rc::row(MARGIN, x1_, top_ + ROW_ABOVE, label, value, selected, valueColor);
    top_ += ROW_PITCH;
  }

  // A second line under the row named just before it; it scrolls with that row.
  void subline(const char *text, bool selected = false) {
    if (rows_ <= skip_) return;
    if (full_ || top_ + SUBLINE_H > LIST_BOTTOM) { full_ = true; return; }
    rc::subline(MARGIN, x1_, top_, text, selected);
    top_ += SUBLINE_H;
  }

  int rows() const { return rows_; }          // rows named so far, drawn or not
  bool moreBelow() const { return full_; }    // at least one line did not fit

 private:
  int skip_;
  int x1_;
  int top_ = LIST_TOP;
  int rows_ = 0;
  bool full_ = false;
};

// ---- small helpers ----------------------------------------------------------------------------------

// "9xQe..VFin" from 32 bytes: first 4 + ".." + last 4 of the base58 form.
void shortKey(const uint8_t key[32], char *out, size_t cap) {
  char full[SOL_B58_PUBKEY_MAX];
  const size_t n = sol_b58_encode(key, 32, full, sizeof full);
  if (n < 8) { snprintf(out, cap, "%s", n ? full : UNKNOWN); return; }
  snprintf(out, cap, "%.4s..%s", full, full + n - 4);
}

// A raw token amount in display units; `zero` instead when the amount is 0 and `zero` is given.
void amountText(uint64_t raw, uint8_t decimals, const char *zero, char *out, size_t cap) {
  if (raw == 0 && zero != nullptr) { snprintf(out, cap, "%s", zero); return; }
  if (sol_format_amount(raw, decimals, out, cap) == 0) snprintf(out, cap, "%s", UNKNOWN);
}

// Registration order is not defined (overview section 6): a page lists its entries by name.
template <class T, class NameOf>
void sortByName(const T **items, size_t count, NameOf nameOf) {
  for (size_t i = 1; i < count; ++i) {
    const T *item = items[i];
    size_t j = i;
    while (j > 0 && strcmp(nameOf(items[j - 1]), nameOf(item)) > 0) { items[j] = items[j - 1]; --j; }
    items[j] = item;
  }
}

const char *orEmpty(const char *text) { return text ? text : ""; }

// ---- page 1: Status ---------------------------------------------------------------------------------
// Provisioned or not, the public key in full (wrapped over two rows), key location, clock source,
// build profile, the result of the domain-table self-check.
void pageStatus(Painter &p) {
  const uint16_t good = th::color(th::STAMP_OK);
  const uint16_t warn = th::color(th::STAMP_WARN);
  const uint16_t bad = th::color(th::STAMP_BAD);

  const bool provisioned = vk::config::provisioned();
  p.row("STATUS", provisioned ? "provisioned" : "setup needed", provisioned ? good : warn);

  const String address = vk::wallet::addressBase58();
  const size_t length = address.length();
  if (length == 0) {
    p.row("PUBLIC KEY", "none", bad);
  } else {
    // 43 or 44 characters: two rows of half each, the second with no label.
    char first[48], second[48];
    size_t half = (length + 1) / 2;
    if (half > sizeof first - 1) half = sizeof first - 1;
    memcpy(first, address.c_str(), half);
    first[half] = '\0';
    snprintf(second, sizeof second, "%s", address.c_str() + half);
    p.row("PUBLIC KEY", first);
    p.row("", second);
  }

  const char *location = vk::wallet::keyLocation();                 // "se050" | "software" | "none"
  if (strcmp(location, "se050") == 0) p.row("KEY", "secure element");
  else if (strcmp(location, "none") == 0) p.row("KEY", "none", bad);
  else p.row("KEY", location);

  switch (vk::clock::source()) {
    case vk::clock::Source::SNTP:  p.row("CLOCK", "sntp, synced", good); break;
    case vk::clock::Source::FLOOR: p.row("CLOCK", "floor, unsynced", warn); break;
    default:                       p.row("CLOCK", "none", warn); break;
  }

#if VK_PROFILE_DEV
  p.row("BUILD", "dev", warn);
#else
  p.row("BUILD", "release");
#endif

  const bool selfCheck = vk::wallet::selfCheckOk();
  p.row("SELF-CHECK", selfCheck ? "ok" : "FAILED", selfCheck ? good : bad);
}

// ---- page 2: Tokens ---------------------------------------------------------------------------------
// Each token of the provisioned table: mint (short), cap, max, balance. The balance comes from
// vk::wallet::tokenInfoLookup, which is null without the balance feature and knows only the default
// token: every other balance is unknown.
void pageTokens(Painter &p) {
  vk_token_t tokens[VK_MAX_TOKENS];
  size_t count = vk::config::tokens(tokens);
  if (count > VK_MAX_TOKENS) count = VK_MAX_TOKENS;
  if (count == 0) {
    p.row("TOKENS", "none", th::color(th::STAMP_WARN));
    p.subline("the token table is set by provisioning");
    return;
  }
  for (size_t i = 0; i < count; ++i) {
    const vk_token_t &token = tokens[i];
    char label[24], value[40];

    snprintf(label, sizeof label, "%s MINT", token.symbol);
    shortKey(token.mint, value, sizeof value);
    p.row(label, value);

    snprintf(label, sizeof label, "%s CAP", token.symbol);
    amountText(token.cap, token.decimals, "none", value, sizeof value);
    p.row(label, value);

    snprintf(label, sizeof label, "%s MAX", token.symbol);
    amountText(token.max, token.decimals, "none", value, sizeof value);
    p.row(label, value);

    snprintf(label, sizeof label, "%s BALANCE", token.symbol);
    vk::wallet::TokenInfo info = {};
    if (vk::wallet::tokenInfoLookup != nullptr && vk::wallet::tokenInfoLookup(token.mint, info) && info.balance_known) {
      amountText(info.raw, token.decimals, nullptr, value, sizeof value);
    } else {
      snprintf(value, sizeof value, "%s", UNKNOWN);
    }
    p.row(label, value);
  }
}

// ---- page 3: Config ---------------------------------------------------------------------------------
// Every registered config key and its value (VKKEYS in screen form), by name. A value that does not
// fit beside its key is written in full on sublines under it.
void pageConfig(Painter &p) {
  const vk::config::ConfigKey *keys[MAX_SORTED];
  size_t count = 0;
  for (const vk::config::ConfigKey *key = vk::config::ConfigKey::first(); key != nullptr && count < MAX_SORTED; key = key->next()) {
    if (key->name != nullptr) keys[count++] = key;
  }
  sortByName(keys, count, [](const vk::config::ConfigKey *key) { return key->name; });
  if (count == 0) { p.row("CONFIG KEYS", "none"); return; }

  const size_t rowCols = (size_t)p.rowCols();
  const size_t sublineCols = (size_t)p.sublineCols();
  for (size_t i = 0; i < count; ++i) {
    const char *name = keys[i]->name;
    const String value = vk::config::text(name);
    const size_t length = value.length();
    if (length == 0) {
      p.row(name, "(none)");
    } else if (strlen(name) + 2 + length <= rowCols) {      // two columns of leader at least
      p.row(name, value.c_str());
    } else {
      p.row(name, "");
      char chunk[64];
      const size_t step = sublineCols < sizeof chunk - 1 ? sublineCols : sizeof chunk - 1;
      for (size_t offset = 0; offset < length && step > 0; offset += step) {
        const size_t n = (length - offset < step) ? length - offset : step;
        memcpy(chunk, value.c_str() + offset, n);
        chunk[n] = '\0';
        p.subline(chunk);
      }
    }
  }
}

// ---- page 4: Apps -----------------------------------------------------------------------------------
// Native apps with the permissions they declare in BADGE_APP, then the Lua apps the user has approved
// on the "Allow app" screen. The consent store keeps a hash of the approved list, not the list.
void pageApps(Painter &p) {
  const badge::NativeApp *apps[MAX_SORTED];
  size_t count = 0;
  for (const badge::NativeApp *app = badge::NativeApp::first(); app != nullptr && count < MAX_SORTED; app = app->next()) {
    if (app->id != nullptr) apps[count++] = app;
  }
  sortByName(apps, count, [](const badge::NativeApp *app) { return app->id; });

  char label[48], value[16];
  snprintf(value, sizeof value, "%u", (unsigned)count);
  p.row("NATIVE APPS", value);
  for (size_t i = 0; i < count; ++i) {
    const char *permissions = orEmpty(apps[i]->permissions);
    snprintf(label, sizeof label, "  %s", apps[i]->id);
    p.row(label, permissions[0] ? permissions : "no permissions");
  }

  const size_t consents = vk::host::consent::count();
  snprintf(value, sizeof value, "%u", (unsigned)consents);
  p.row("LUA APPS WITH CONSENT", value);
  for (size_t i = 0; i < consents; ++i) {
    String id;
    uint32_t hash = 0;
    if (!vk::host::consent::at(i, id, hash)) continue;
    snprintf(label, sizeof label, "  %s", id.c_str());
    p.row(label, "allowed");
  }
}

// ---- page 5: Reset ----------------------------------------------------------------------------------
// One action. The confirmation that follows is the firmware's (title "Change setting", headline
// "ERASE WALLET CONFIG", amber, hold); the two rows under the action repeat what it says.
void pageReset(Painter &p, bool unavailable) {
  p.row("RESET WALLET CONFIG", WS_TRI_RIGHT, 0, true);
  p.subline("press SELECT, then hold SELECT to confirm", true);
  p.row("ERASES", "all wallet settings");
  p.row("KEEPS", "key, history, contacts");
  if (unavailable) p.row("CONFIRMATION", "unavailable", th::color(th::STAMP_BAD));
}

// ---- the app ----------------------------------------------------------------------------------------
// Inside the unnamed namespace like the rest of the file, so that no name here can collide with one
// an upstream header puts at global scope.
class WalletSettings final : public badge::App {
 public:
  void on_update(float dt) override {
    sinceDraw_ += dt;
    // While an approval is up the app is not run at all, so the next frame comes late and finds the
    // approval's picture in the canvas.
    if (dt > PAUSE_GAP_S) dirty_ = true;
    if (sinceDraw_ >= (page_ == PAGE_APPS ? REFRESH_APPS_S : REFRESH_S)) dirty_ = true;
  }

  void on_draw() override {
    if (!dirty_) return;
    dirty_ = false;
    sinceDraw_ = 0.0f;

    char right[24];
    rc::statusRight(right, sizeof right);
    rc::page();
    rc::header("BADGEOS", right);
    rc::title(PAGE_TITLES[page_], TITLE_Y);

    Painter painter(scroll_);
    switch (page_) {
      case PAGE_STATUS: pageStatus(painter); break;
      case PAGE_TOKENS: pageTokens(painter); break;
      case PAGE_CONFIG: pageConfig(painter); break;
      case PAGE_APPS:   pageApps(painter); break;
      default:          pageReset(painter, resetUnavailable_); break;
    }
    moreBelow_ = painter.moreBelow();
    // The list became shorter than the scroll position (the config was erased): back to the top.
    if (scroll_ > 0 && painter.rows() <= scroll_) { scroll_ = 0; dirty_ = true; }

    const char *hint = "";
    if (page_ == PAGE_RESET) hint = "  SELECT reset";
    else if (scroll_ > 0 || moreBelow_) hint = "  UP/DOWN scroll";
    char left[64];
    snprintf(left, sizeof left, WS_TRI_LEFT " WALLET %u/%u " WS_TRI_RIGHT "%s", (unsigned)page_ + 1u, (unsigned)PAGE_COUNT, hint);
    rc::footer(left, "CANCEL back");
  }

  void on_button(uint8_t key, bool pressed) override {
    if (!pressed) return;
    switch (key) {
      case BTN_B:                                  // CANCEL always leads out
        badge::exit();
        return;
      case BTN_LEFT:
        showPage((uint8_t)((page_ + PAGE_COUNT - 1) % PAGE_COUNT));
        break;
      case BTN_RIGHT:
        showPage((uint8_t)((page_ + 1) % PAGE_COUNT));
        break;
      case BTN_UP:
        if (scroll_ > 0) --scroll_;
        break;
      case BTN_DOWN:
        if (moreBelow_) ++scroll_;
        break;
      case BTN_A:                                  // SELECT
        if (page_ != PAGE_RESET) return;
        // Raises the firmware confirmation; the erase happens only if the user holds SELECT there.
        // The call has no result: if no approval opened, the engine could not show one.
        vk::config::requestReset();
        resetUnavailable_ = !vk::wallet::approval::active();
        break;
      default:
        return;
    }
    dirty_ = true;
  }

 private:
  void showPage(uint8_t page) {
    page_ = page;
    scroll_ = 0;
    moreBelow_ = false;
    resetUnavailable_ = false;
  }

  uint8_t page_ = PAGE_STATUS;
  int scroll_ = 0;                  // rows scrolled off the top
  bool moreBelow_ = false;          // from the last repaint: lines were left over below the list
  bool dirty_ = true;
  bool resetUnavailable_ = false;
  float sinceDraw_ = 0.0f;
};

}  // namespace

BADGE_APP(WalletSettings, "wallet_settings", "Wallet", "1.0.0", "");
