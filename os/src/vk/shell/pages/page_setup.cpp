// Settings page and screen `setup` (docs/os/ui/shell.md, "Setup"): a checklist of what this badge
// still needs, computed from its real state each time it is drawn, with what to do next for each row
// and, where it can be done on the badge, SELECT going straight there (Wi-Fi, the name, the time zone,
// the theme). The wallet's values are the root of trust and come from the laptop tool only: their
// row says how, names the config keys still missing (from the config registry), and turns done by
// itself when provisioning over USB completes.
//
// First boot: the screen opens once over the launcher when the badge is not provisioned, something
// required is left, no app starts at boot, and it was never dismissed (config key setup_done). CANCEL
// always leaves (to the launcher when it opened by itself) and sets setup_done, so it does not open
// by itself again; a wallet reset (VKRESET) erases setup_done with the rest of the config.
//
// The logic is src/vk/shell/setup_core.c (host-tested); this file reads the badge and draws.
// Deleting it removes the page, the first-boot screen, setup.h's functions and the key setup_done.

#include "../page.h"

#include "../../../apps/app_store.h"
#include "../../../badge_log.h"
#include "../../../lua_sdk/lua_runtime.h"
#include "../../../net/wifi_mgr.h"
#include "../../../settings.h"
#include "../../core/clock.h"
#include "../../core/config.h"
#include "../../core/service.h"
#include "../../core/wifi_net.h"
#include "../../vk.h"
#include "../../vk_build.h"
#include "../../wallet/signer.h"
#include "../edit.h"
#include "../setup.h"
#include "../setup_core.h"

#if VK_TEST_HOOKS
#include "../../core/serial.h"
#endif

namespace {
using namespace vk::shell;
namespace receipt = vk::ui::receipt;
namespace tk = vk::ui::theme;
namespace cfg = vk::config;
namespace edit = vk::shell::edit;

VK_CONFIG_KEY(setup_done, "setup_done", vk::config::Type::U32, "0", vk::config::F_NONE, 0, 1,
              "1: the setup screen does not open by itself at boot");

constexpr int ROWS = VK_SETUP_ITEMS;
constexpr int RULE_Y = 172;
constexpr int LINES_Y = 178;
constexpr int LINE_PITCH = 11;
constexpr uint32_t CHECK_MS = 500;    // how often the state is read again while the screen is up

// ---- the snapshot ---------------------------------------------------------------------------------

struct Snapshot {
  vk_setup_state_t state;
  char name[40];
  char theme[28];
};

// The required config keys with no value, by name, from the registry: no list of keys here.
void missingKeys(vk_setup_state_t &s) {
  s.missing_count = 0;
  for (const cfg::ConfigKey *key = cfg::ConfigKey::first(); key != nullptr; key = key->next()) {
    if (!(key->flags & cfg::F_REQUIRED) || cfg::text(key->name).length() > 0) continue;
    if (s.missing_count == VK_SETUP_MISSING_MAX) break;   // more than the row can name
    // Sorted by name, so the line does not depend on link order.
    unsigned at = s.missing_count;
    while (at > 0 && strcmp(key->name, s.missing[at - 1]) < 0) {
      s.missing[at] = s.missing[at - 1];
      --at;
    }
    s.missing[at] = key->name;
    ++s.missing_count;
  }
}

void take(Snapshot &snap) {
  memset(&snap, 0, sizeof snap);
  vk_setup_state_t &s = snap.state;
  s.identity = vk::wallet::publicKey() != nullptr;   // the wallet's view of the identity: no identity.h here
  s.wifi_saved = (unsigned)vk::wifi::savedCount();
  s.wifi_joined = wifi_mgr::mode() == wifi_mgr::Mode::Station && wifi_mgr::connected();
  s.hotspot = wifi_mgr::mode() == wifi_mgr::Mode::AccessPoint;
  switch (vk::clock::source()) {
    case vk::clock::Source::SNTP:  s.clock = VK_SETUP_CLOCK_SNTP; break;
    case vk::clock::Source::FLOOR: s.clock = VK_SETUP_CLOCK_FLOOR; break;
    case vk::clock::Source::NONE:  s.clock = VK_SETUP_CLOCK_NONE; break;
  }
  s.provisioned = cfg::provisioned();
  missingKeys(s);
  s.listener_set = cfg::text("listener_url").length() > 0;
  strlcpy(snap.name, cfg::text("display_name").c_str(), sizeof snap.name);
  s.name_set = snap.name[0] != '\0';
  s.name = snap.name;
  const char *theme = tk::activeName();
  if (theme == nullptr) theme = "";
  if (strncmp(theme, "receipt-", 8) == 0) theme += 8;
  strlcpy(snap.theme, theme, sizeof snap.theme);
  s.theme = snap.theme;
}

// ---- the screen -----------------------------------------------------------------------------------

List sList;
char sDrawn[400] = "";       // what the last draw showed (signature())
uint32_t sCheckedAt = 0;

void signature(const vk_setup_row_t rows[ROWS], char *out, size_t cap) {
  out[0] = '\0';
  for (int i = 0; i < ROWS; ++i) {
    char part[96];
    snprintf(part, sizeof part, "%s:%d:%s|", rows[i].value, (int)rows[i].status, rows[i].detail);
    strlcat(out, part, cap);
  }
  strlcat(out, edit::note() != nullptr ? "n" : "-", cap);
}

// The next registered theme, as Settings > Theme does it.
void nextTheme() {
  const size_t count = tk::count();
  if (count == 0) return;
  const char *active = tk::activeName();
  size_t next = 0;
  for (size_t i = 0; i < count; ++i) {
    const tk::Theme *theme = tk::at(i);
    if (theme != nullptr && active != nullptr && strcmp(theme->name, active) == 0) {
      next = (i + 1) % count;
      break;
    }
  }
  const tk::Theme *theme = tk::at(next);
  if (theme != nullptr) tk::setActive(theme->name);
  repaint();
}

void go(vk_setup_go_t where) {
  switch (where) {
    case VK_SETUP_GO_WIFI:  edit::openPage("wifi"); break;
    case VK_SETUP_GO_NAME:  edit::type("display_name", "NAME"); break;
    case VK_SETUP_GO_TIME:  edit::openPage("badge"); break;
    case VK_SETUP_GO_THEME: nextTheme(); break;
    case VK_SETUP_GO_LAPTOP:
    case VK_SETUP_GO_NONE:
      break;   // nothing to open on the badge: the lines under the list say what to do
  }
}

const char *footFor(vk_setup_go_t where) {
  switch (where) {
    case VK_SETUP_GO_WIFI:   return "SELECT open Wi-Fi";
    case VK_SETUP_GO_NAME:   return "SELECT type a name";
    case VK_SETUP_GO_TIME:   return "SELECT time zone";
    case VK_SETUP_GO_THEME:  return "SELECT switch theme";
    case VK_SETUP_GO_LAPTOP: return "needs a laptop: see below";
    case VK_SETUP_GO_NONE:   break;
  }
  return "";
}

uint16_t statusColor(vk_setup_status_t status) {
  switch (status) {
    case VK_SETUP_DONE:     return tk::color(tk::STAMP_OK);
    case VK_SETUP_TODO:     return tk::color(tk::STAMP_WARN);
    case VK_SETUP_WAIT:     return 0;   // ink: under way
    case VK_SETUP_OPTIONAL: return tk::color(tk::SUB);
  }
  return 0;
}

// Leaving it, by CANCEL, is the dismissal: it does not open by itself again.
void dismiss() {
  if (cfg::u32("setup_done") != 0) return;
  if (cfg::set("setup_done", "1") == cfg::SetResult::OK) badge_log::tagf("os", "setup dismissed");
}

void setupEnter() {
  sList = List();
  sDrawn[0] = '\0';
  sCheckedAt = millis();
}

void setupUpdate() {
  if (buttons::pressed(BTN_B)) {    // CANCEL always leaves: it never traps the person
    dismiss();
    pop();
    return;
  }
  listMove(sList, ROWS, ROWS);
  Snapshot snap;
  if (buttons::pressed(BTN_A) && sList.cursor >= 0 && sList.cursor < ROWS) {
    take(snap);
    vk_setup_row_t rows[ROWS];
    vk_setup_rows(&snap.state, rows);
    go(rows[sList.cursor].go);
    return;
  }
  // Provisioning over USB, a join, the network time: shown when they happen, drawn only then.
  if (millis() - sCheckedAt < CHECK_MS) return;
  sCheckedAt = millis();
  take(snap);
  vk_setup_row_t rows[ROWS];
  vk_setup_rows(&snap.state, rows);
  char now[sizeof sDrawn];
  signature(rows, now, sizeof now);
  if (strcmp(now, sDrawn) != 0) repaint();
}

void setupDraw() {
  Snapshot snap;
  take(snap);
  vk_setup_row_t rows[ROWS];
  vk_setup_rows(&snap.state, rows);
  signature(rows, sDrawn, sizeof sDrawn);
  const int cursor = sList.cursor >= 0 && sList.cursor < ROWS ? sList.cursor : 0;
  const vk_setup_row_t &here = rows[cursor];

  frame("SETUP", footFor(here.go), "CANCEL close");
  const unsigned left = vk_setup_left(&snap.state);
  char mark[16];
  if (left == 0) {
    snprintf(mark, sizeof mark, "ready");
  } else {
    snprintf(mark, sizeof mark, "%u left", left);
  }
  textRight(X1, TITLE_Y + 2, mark, left == 0 ? tk::STAMP_OK : tk::FAINT);

  ListRow list[ROWS];
  for (int i = 0; i < ROWS; ++i) list[i] = {rows[i].label, rows[i].value, statusColor(rows[i].status)};
  listDraw(sList, list, ROWS, LIST_Y, ROWS);
  receipt::rule(RULE_Y);

  // What to do next for the row under the cursor; the last line is the detail (the missing keys),
  // or for 3 s what the last change did.
  const char *lines[VK_SETUP_NEXT_LINES] = {nullptr, nullptr, nullptr};
  tk::Token tokens[VK_SETUP_NEXT_LINES] = {tk::SUB, tk::SUB, tk::SUB};
  int n = 0;
  for (int k = 0; k < VK_SETUP_NEXT_LINES && n < VK_SETUP_NEXT_LINES; ++k) {
    if (here.next[k] != nullptr) lines[n++] = here.next[k];
  }
  if (here.detail[0] != '\0' && n < VK_SETUP_NEXT_LINES) {
    tokens[n] = tk::STAMP_WARN;
    lines[n++] = here.detail;
  }
  bool bad = false;
  const char *note = edit::note(&bad);
  if (note != nullptr) {
    const int at = n < VK_SETUP_NEXT_LINES ? n : VK_SETUP_NEXT_LINES - 1;
    lines[at] = note;
    tokens[at] = bad ? tk::STAMP_WARN : tk::STAMP_OK;
    if (at == n) ++n;
  }
  for (int k = 0; k < n; ++k) text(X0, LINES_Y + k * LINE_PITCH, lines[k], tokens[k]);
}

const Screen kSetup = {"setup", setupEnter, setupUpdate, setupDraw, 0};

void setupValue(char *out, size_t cap) { vk::shell::setup::summary(out, cap); }

// ---- first boot -----------------------------------------------------------------------------------
// Decided once, on the first loop pass on which the launcher is up with nothing over it.

bool sDecided = false;

void firstBoot() {
  if (sDecided) return;
  if (vk::modalActive()) return;                       // an approval: wait until it closes
  sDecided = true;
  const String autostart = settings::autostartApp();
  if (autostart.length() && app_store::exists(autostart)) return;   // the person chose an app at boot
  if (runtime::running() || top() != &kLauncher) return;             // something else has the screen
  Snapshot snap;
  take(snap);
  if (!vk_setup_autoopen(&snap.state, cfg::u32("setup_done"))) return;
  badge_log::tagf("os", "setup: %u step(s) left; showing the checklist", vk_setup_left(&snap.state));
  showOver(&kSetup);
}

VK_SERVICE(setup_first_boot, nullptr, firstBoot);

// ---- dev hook -------------------------------------------------------------------------------------
// VKSETUP: the checklist as the screen computes it, for device tests (testing.md, "Dev hooks").
// Dev profile only. Names and states; no config value but the display name.
#if VK_TEST_HOOKS
const char *statusName(vk_setup_status_t status) {
  switch (status) {
    case VK_SETUP_DONE:     return "done";
    case VK_SETUP_TODO:     return "todo";
    case VK_SETUP_WAIT:     return "wait";
    case VK_SETUP_OPTIONAL: return "optional";
  }
  return "?";
}

void cmdSetup(const String &, const vk::serial::Reply &reply) {
  Snapshot snap;
  take(snap);
  vk_setup_row_t rows[ROWS];
  vk_setup_rows(&snap.state, rows);
  String out;
  out.reserve(320);
  out += "OK {\"left\":" + String(vk_setup_left(&snap.state));
  out += ",\"autoopen\":";
  out += vk_setup_autoopen(&snap.state, cfg::u32("setup_done")) ? "true" : "false";
  out += ",\"setup_done\":" + String((unsigned)cfg::u32("setup_done"));
  out += ",\"cursor\":" + String(sList.cursor);
  out += ",\"rows\":\"";
  for (int i = 0; i < ROWS; ++i) {
    if (i) out += ',';
    for (const char *p = rows[i].label; *p; ++p) out += (*p >= 'A' && *p <= 'Z') ? (char)(*p + 32) : *p;
    out += '=';
    out += statusName(rows[i].status);
  }
  out += "\"}";
  reply(out);
}

VK_SERIAL_COMMAND(vksetup, "VKSETUP", cmdSetup, "the setup checklist: rows and states, steps left, first-boot rule");
#endif  // VK_TEST_HOOKS

}  // namespace

// ---- setup.h --------------------------------------------------------------------------------------

namespace vk::shell::setup {

unsigned left() {
  Snapshot snap;
  take(snap);
  return vk_setup_left(&snap.state);
}

bool needed() { return left() > 0; }

void summary(char *out, size_t cap) {
  const unsigned n = left();
  if (n == 0) {
    snprintf(out, cap, "done");
  } else {
    snprintf(out, cap, "%u step%s left", n, n == 1 ? "" : "s");
  }
}

void open() { push(&kSetup); }

}  // namespace vk::shell::setup

VK_SETTINGS_PAGE(setup, "setup", 144, "Setup", setupValue, setupEnter, setupUpdate, setupDraw, 0);
