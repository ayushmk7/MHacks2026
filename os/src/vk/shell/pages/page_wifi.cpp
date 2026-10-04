// Settings page `wifi` and its sub-screens `wifi_scan`, `wifi_saved` and `wifi_join`
// (docs/os/ui/shell.md, "Wi-Fi").
//
// The whole way onto a network, on the badge: the status page -> the scan list -> the password, typed
// on the on-screen keyboard (ui/keyboard.h; skipped for an open or a saved network) -> CONNECTING,
// with a hard timeout -> the result, with what to do next.
//
// This file is the screens only. The saved networks, the join state machine and its timeout are
// src/vk/core/wifi_net; the radio is upstream's wifi_mgr. A password is held here only between the
// keyboard and the join (and for "type it again"), and is wiped when the flow ends. It is never
// logged. Deleting this file removes the page and its screens and nothing else.
#include "../page.h"

#include <WiFi.h>

#include "../../../badge_log.h"
#include "../../../net/push_server.h"
#include "../../../net/wifi_mgr.h"
#include "../../../settings.h"
#include "../../core/wifi_net.h"
#include "../../ui/keyboard.h"
#include "../../vk_build.h"

#if VK_TEST_HOOKS
#include "../../core/serial.h"
#endif

namespace {
using namespace vk::shell;
namespace receipt = vk::ui::receipt;
namespace keyboard = vk::ui::keyboard;
namespace tk = vk::ui::theme;
namespace net = vk::wifi;

// ---- tunables -------------------------------------------------------------------------------------
constexpr int MAX_LISTED = 24;                 // networks on the scan list, strongest first
constexpr uint32_t SCAN_TIMEOUT_MS = 12000;    // a scan that has not ended by then counts as failed
constexpr uint32_t REFRESH_MS = 250;           // the scan, the join and the link change without input
constexpr uint16_t PULSE_MS = 500;             // the LED pulse when a join ends
// Signal bars: one more for each of these levels (dBm) the signal reaches.
constexpr int BAR_LEVELS[] = {-85, -75, -65, -55};
constexpr int BARS = sizeof BAR_LEVELS / sizeof BAR_LEVELS[0];
const char OTHER_ROW[] = "OTHER...";           // the scan list's last row: a network that hides its name

// ---- layout ---------------------------------------------------------------------------------------
constexpr int STATUS_LIST_Y = 88;              // the status page's list: 7 rows under the status block
constexpr int STATUS_VISIBLE = 7;
constexpr int STATUS_ACTIONS = 3;              // Scan, Wi-Fi, Hotspot; the saved networks follow
constexpr int BAR_W = 3, BAR_GAP = 2;          // one signal bar, and the space to the next
constexpr int BARS_W = BARS * BAR_W + (BARS - 1) * BAR_GAP;   // 18 px
constexpr int LOCK_COLS = 2;                   // label columns kept free for the lock mark
const char BARS_PAD[] = "    ";                // value columns kept free for the bars (24 px)

// ---- the flow's state -------------------------------------------------------------------------------
enum class Kind : uint8_t { OPEN, PASSWORD, LOGIN };       // what a network asks for

struct Network {
  char ssid[net::SSID_LEN_MAX + 1];
  int rssi;
  Kind kind;
};

enum class View : uint8_t { JOIN, NEEDS_LOGIN, REFUSED };  // what `wifi_join` shows: the join, or a notice

Network sNetworks[MAX_LISTED];
int sNetworkCount = 0;
bool sScanStarted = false;                     // this screen's scan is running (or just ended)
bool sScanFailed = false;
uint32_t sScanAt = 0;
List sStatusList, sScanList, sSavedList;

char sSsid[net::SSID_LEN_MAX + 1] = "";        // the network being joined, or looked at on `wifi_saved`
char sPass[keyboard::TEXT_MAX + 1] = "";       // the password typed for it; "" for an open or a saved one
bool sTyped = false;                           // sPass came from the keyboard (kept for a retry)
bool sHidden = false;                          // the name was typed: the password may be empty
bool sFromScan = false;                        // `wifi_join` sits on `wifi_scan` (else on `wifi`)
bool sSavedIsLogin = false;                    // `wifi_saved` shows upstream's enterprise profile
bool sResume = false;                          // Wi-Fi was wanted before the flow: go back to it on leaving
View sView = View::JOIN;
net::Join sLastJoin = net::Join::IDLE;         // to pulse the LEDs once per result

void wipePassword() {
  volatile char *p = sPass;
  for (size_t i = 0; i < sizeof sPass; ++i) p[i] = 0;
  sTyped = false;
}

bool joinedTo(const char *ssid) {
  return wifi_mgr::mode() == wifi_mgr::Mode::Station && wifi_mgr::connected() && wifi_mgr::ssid() == ssid;
}

// The flow ended without a network: if Wi-Fi was on (or trying) before, it goes back to trying.
void resumeWifi() {
  if (sResume && wifi_mgr::mode() == wifi_mgr::Mode::Off) net::joinBest();
  sResume = false;
}

// ---- marks ------------------------------------------------------------------------------------------

int barsFor(int rssi) {
  int level = 0;
  for (int i = 0; i < BARS; ++i) level += rssi >= BAR_LEVELS[i] ? 1 : 0;
  return level;
}

// Rising bars in the 8 rows from y, BARS_W wide. Those the signal does not reach are drawn FAINT.
void drawBars(int x, int y, int rssi, bool selected) {
  LGFX_Sprite &c = display::canvas();
  const int level = barsFor(rssi);
  const uint16_t on = tk::color(selected ? tk::PAPER : tk::INK);
  const uint16_t off = tk::color(tk::FAINT);
  for (int i = 0; i < BARS; ++i) {
    const int h = 2 + 2 * i;
    c.fillRect(x + i * (BAR_W + BAR_GAP), y + 8 - h, BAR_W, h, i < level ? on : off);
  }
  display::touch();
}

// A closed padlock, 7 x 8 px: this network asks for a password or a login.
void drawLock(int x, int y, bool selected) {
  LGFX_Sprite &c = display::canvas();
  const uint16_t ink = tk::color(selected ? tk::PAPER : tk::INK);
  c.drawFastHLine(x + 2, y, 3, ink);
  c.drawFastVLine(x + 1, y + 1, 2, ink);
  c.drawFastVLine(x + 5, y + 1, 2, ink);
  c.fillRect(x, y + 3, 7, 5, ink);
  display::touch();
}

// "n/N" beside the title, as listDraw() prints it for a list longer than the screen.
void drawPosition(const List &list, int count, int visible) {
  if (count <= visible) return;
  char position[16];
  snprintf(position, sizeof position, "%d/%d", list.cursor + 1, count);
  textRight(X1, TITLE_Y + 2, position, tk::FAINT);
}

// The row count changes without input (a scan ends, a network is forgotten), so the cursor and the
// window are brought back inside it before either is used.
void clampList(List &list, int count, int visible) {
  if (list.cursor >= count) list.cursor = count - 1;
  if (list.cursor < 0) list.cursor = 0;
  if (list.scroll > count - visible) list.scroll = count - visible;
  if (list.scroll < 0) list.scroll = 0;
  if (list.cursor < list.scroll) list.scroll = list.cursor;
  if (list.cursor >= list.scroll + visible) list.scroll = list.cursor - visible + 1;
}

// =====================================================================================================
// wifi_join: CONNECTING, then the result
// =====================================================================================================

void joinUpdate();
void joinDraw();
const Screen kJoin = {"wifi_join", nullptr, joinUpdate, joinDraw, REFRESH_MS};

void passwordDone(bool accepted, const char *typed);

keyboard::Options passwordOptions(const char *initial) {
  static char hint[52];
  snprintf(hint, sizeof hint, sHidden ? "none if it is open, or %u to %u, for %s" : "%u to %u characters, for %s",
           (unsigned)net::PASS_LEN_MIN, (unsigned)net::PASS_LEN_MAX, sSsid);
  keyboard::Options options;
  options.title = "PASSWORD";
  options.hint = hint;
  options.initial = initial;
  options.minLen = (uint8_t)net::PASS_LEN_MIN;
  options.maxLen = (uint8_t)net::PASS_LEN_MAX;
  options.secret = true;
  options.emptyOk = sHidden;
  return options;
}

// Starts the join of sSsid and shows `wifi_join` over the screen that is on top.
void startJoin(bool saved) {
  sView = View::JOIN;
  sLastJoin = net::Join::CONNECTING;
  const bool started = saved ? net::joinSaved(sSsid) : net::join(sSsid, sPass);
  if (!started) sView = View::REFUSED;
  push(&kJoin);
}

void showNotice(View view) {
  sView = view;
  push(&kJoin);
}

// Leaves `wifi_join` for the screen under it, with the flow over.
void joinLeave() {
  net::joinDismiss();
  wipePassword();
  pop();
  resumeWifi();
}

void joinUpdate() {
  net::hold();
  const bool select = buttons::pressed(BTN_A);
  const bool cancel = buttons::pressed(BTN_B);

  if (sView != View::JOIN) {                       // a notice: any of the two keys goes back
    if (select || cancel) pop();
    return;
  }

  const net::Join state = net::joinState();
  if (state != sLastJoin) {                        // the result just arrived
    sLastJoin = state;
    if (state == net::Join::JOINED) {
      pulseLed(PULSE_MS);
    } else if (state != net::Join::IDLE && state != net::Join::CONNECTING) {
      pulseLedBad(PULSE_MS);
    }
    repaint();
  }

  switch (state) {
    case net::Join::CONNECTING:
      if (cancel) {
        net::joinCancel();
        wipePassword();
        pop();
        resumeWifi();
      }
      break;
    case net::Join::JOINED:
      if (select || cancel) {
        net::joinDismiss();
        wipePassword();
        sResume = false;
        pop();
        if (sFromScan) pop();                      // through the scan list, back to the status page
      }
      break;
    case net::Join::WRONG_PASSWORD:
      if (select) {                                // type it again, starting from what was typed
        net::joinDismiss();
        pop();
        char again[sizeof sPass];
        strlcpy(again, sTyped ? sPass : "", sizeof again);
        keyboard::open(passwordOptions(again), passwordDone);
        volatile char *p = again;
        for (size_t i = 0; i < sizeof again; ++i) p[i] = 0;
      } else if (cancel) {
        joinLeave();
      }
      break;
    case net::Join::NOT_FOUND:
    case net::Join::TIMEOUT:
      if (select) {                                // the same join again, with the same password
        net::joinDismiss();
        sLastJoin = net::Join::CONNECTING;
        const bool started = sTyped || !net::isSaved(sSsid) ? net::join(sSsid, sPass) : net::joinSaved(sSsid);
        if (!started) sView = View::REFUSED;
        repaint();
      } else if (cancel) {
        joinLeave();
      }
      break;
    default:                                       // IDLE: the join was dismissed elsewhere
      if (select || cancel) pop();
      break;
  }
}

// A result: title, the network, the verdict in its colour, up to three lines of what to do.
void drawResult(const char *title, const char *verdict, tk::Token token, const char *line1, const char *line2,
                const char *line3, const char *footLeft) {
  frame(title, footLeft);
  receipt::row(X0, X1, 48, "NETWORK", sSsid);
  receipt::row(X0, X1, 66, "RESULT", verdict, false, tk::color(token));
  receipt::rule(82);
  text(X0, 94, line1, tk::INK);
  text(X0, 108, line2, tk::SUB);
  text(X0, 122, line3, tk::SUB);
}

void joinDraw() {
  if (sView == View::NEEDS_LOGIN) {
    drawResult("NOT CONNECTED", "needs a login", tk::STAMP_WARN, "This network asks for a user name.",
               "The badge cannot type one in. Set the network up", "from the web page (Settings > App push).", "");
    return;
  }
  char line[64];
  if (sView == View::REFUSED) {
    snprintf(line, sizeof line, "A name is 1 to %u characters, a password %u to %u.", (unsigned)net::SSID_LEN_MAX,
             (unsigned)net::PASS_LEN_MIN, (unsigned)net::PASS_LEN_MAX);
    drawResult("NOT CONNECTED", "cannot join", tk::STAMP_BAD, "No network takes a name or a password of this", "length.",
               line, "");
    return;
  }

  switch (net::joinState()) {
    case net::Join::CONNECTING: {
      frame("CONNECTING", "", "CANCEL stop");
      receipt::row(X0, X1, 48, "NETWORK", sSsid);
      receipt::row(X0, X1, 66, "STATE", wifi_mgr::statusText());
      const uint32_t elapsed = net::joinElapsedMs(), limit = net::joinTimeoutMs();
      bar(41, 96, 30, elapsed, limit);
      snprintf(line, sizeof line, "%u s of %u s at most", (unsigned)(elapsed / 1000), (unsigned)(limit / 1000));
      textCentered(160, 114, line, tk::SUB);
      break;
    }
    case net::Join::JOINED: {
      frame("CONNECTED", "SELECT or CANCEL: done", "");
      receipt::row(X0, X1, 48, "NETWORK", sSsid);
      receipt::row(X0, X1, 66, "RESULT", "joined", false, tk::color(tk::STAMP_OK));
      receipt::row(X0, X1, 84, "ADDRESS", wifi_mgr::ip().toString().c_str());
      snprintf(line, sizeof line, "%d dBm%s", wifi_mgr::rssi(), BARS_PAD);
      receipt::row(X0, X1, 102, "SIGNAL", line);
      drawBars(X1 - BARS_W, 101, wifi_mgr::rssi(), false);
      if (net::isSaved(sSsid)) {
        snprintf(line, sizeof line, "yes (%u of %u)", (unsigned)net::savedCount(), (unsigned)net::MAX_SAVED);
        receipt::row(X0, X1, 120, "SAVED", line);
        text(X0, 142, "The badge joins it by itself from now on.", tk::SUB);
      } else {
        receipt::row(X0, X1, 120, "SAVED", "no: storage is full", false, tk::color(tk::STAMP_WARN));
      }
      break;
    }
    case net::Join::WRONG_PASSWORD:
      if (sPass[0] == '\0' && !net::isSaved(sSsid)) {
        drawResult("NOT CONNECTED", "needs a password", tk::STAMP_BAD, "This network is not open.",
                   "SELECT to type its password.", "", "SELECT type the password");
      } else {
        drawResult("NOT CONNECTED", "wrong password", tk::STAMP_BAD, "The network refused the password.",
                   "Capitals and small letters are different. The", "show key lets you read what you type.",
                   "SELECT type it again");
      }
      break;
    case net::Join::NOT_FOUND:
      drawResult("NOT CONNECTED", "not found", tk::STAMP_BAD, "No network with this name answered.",
                 "Move closer, or check the name: capitals and", "small letters are different.", "SELECT try again");
      break;
    case net::Join::TIMEOUT:
      snprintf(line, sizeof line, "The network did not let the badge in within %u s.", (unsigned)(net::joinTimeoutMs() / 1000));
      drawResult("NOT CONNECTED", "no answer", tk::STAMP_WARN, line, "It may be busy or far away. The password was",
                 "not refused.", "SELECT try again");
      break;
    default:
      drawResult("NOT CONNECTED", "stopped", tk::STAMP_WARN, "", "", "", "");
      break;
  }
}

// ---- the keyboard's answers ---------------------------------------------------------------------------
// Called after the keyboard has popped itself: the screen on top is the one it was opened from.

void passwordDone(bool accepted, const char *typed) {
  if (!accepted) {                                 // back on the list, nothing joined
    wipePassword();
    resumeWifi();
    return;
  }
  strlcpy(sPass, typed, sizeof sPass);
  sTyped = true;
  startJoin(false);
}

void nameDone(bool accepted, const char *typed) {
  if (!accepted) {
    resumeWifi();
    return;
  }
  strlcpy(sSsid, typed, sizeof sSsid);
  wipePassword();
  if (net::isSaved(sSsid)) {
    startJoin(true);
    return;
  }
  keyboard::open(passwordOptions(nullptr), passwordDone);
}

// =====================================================================================================
// wifi_scan: the networks in range
// =====================================================================================================

Kind kindOf(int index) {
  switch (WiFi.encryptionType((uint8_t)index)) {
    case WIFI_AUTH_OPEN:
      return Kind::OPEN;
    case WIFI_AUTH_ENTERPRISE:
    case WIFI_AUTH_WPA3_ENT_192:
    case WIFI_AUTH_WPA3_ENTERPRISE:
    case WIFI_AUTH_WPA2_WPA3_ENTERPRISE:
    case WIFI_AUTH_WPA_ENTERPRISE:
      return Kind::LOGIN;
    default:
      return Kind::PASSWORD;
  }
}

// The scan's results as the list shows them: one row per name (the strongest access point of
// each), no nameless network, strongest first, MAX_LISTED at most.
void collectNetworks() {
  sNetworkCount = 0;
  const int found = wifi_mgr::scanResultCount();
  for (int i = 0; i < found; ++i) {
    const String ssid = wifi_mgr::scanSsid(i);
    if (ssid.length() == 0 || ssid.length() > net::SSID_LEN_MAX) continue;
    const int rssi = wifi_mgr::scanRssi(i);
    int at = 0;
    while (at < sNetworkCount && ssid != sNetworks[at].ssid) ++at;
    if (at < sNetworkCount) {                      // a second access point of a network already listed
      if (rssi <= sNetworks[at].rssi) continue;
      for (; at + 1 < sNetworkCount; ++at) sNetworks[at] = sNetworks[at + 1];
      --sNetworkCount;
    }
    // Insert by signal, strongest first. A full list drops its weakest.
    int slot = sNetworkCount;
    while (slot > 0 && sNetworks[slot - 1].rssi < rssi) --slot;
    if (slot >= MAX_LISTED) continue;
    const int last = sNetworkCount < MAX_LISTED ? sNetworkCount : MAX_LISTED - 1;
    for (int j = last; j > slot; --j) sNetworks[j] = sNetworks[j - 1];
    strlcpy(sNetworks[slot].ssid, ssid.c_str(), sizeof sNetworks[slot].ssid);
    sNetworks[slot].rssi = rssi;
    sNetworks[slot].kind = kindOf(i);
    if (sNetworkCount < MAX_LISTED) ++sNetworkCount;
  }
}

void scanStart() {
  sNetworkCount = 0;
  sScanFailed = false;
  bool started = wifi_mgr::startScan();
  // A radio that is busy joining a network refuses to scan: the join is stopped first. A network
  // the badge is on, and the hotspot, are left alone.
  if (!started && wifi_mgr::mode() == wifi_mgr::Mode::Station && !wifi_mgr::connected()) {
    wifi_mgr::disconnect();
    started = wifi_mgr::startScan();
  }
  sScanStarted = started;
  sScanFailed = !started;
  sScanAt = millis();
  repaint();
}

void scanEnter() {
  sScanList = List();
  sResume = wifi_mgr::mode() == wifi_mgr::Mode::Station;
  sHidden = false;
  wipePassword();
  net::joinDismiss();
  scanStart();
}

int scanRows() { return sNetworkCount + 1; }       // the networks, then OTHER...

void scanSelect(int cursor) {
  sFromScan = true;
  wipePassword();
  if (cursor >= sNetworkCount) {                   // OTHER...: ask for the name
    sHidden = true;
    keyboard::Options options;
    options.title = "NETWORK NAME";
    options.hint = "the name of a network that hides it";
    options.minLen = 1;
    options.maxLen = (uint8_t)net::SSID_LEN_MAX;
    keyboard::open(options, nameDone);
    return;
  }
  const Network &network = sNetworks[cursor];
  sHidden = false;
  strlcpy(sSsid, network.ssid, sizeof sSsid);
  if (joinedTo(sSsid)) {                           // already on it: the status page says so
    pop();
  } else if (network.kind == Kind::LOGIN) {
    showNotice(View::NEEDS_LOGIN);
  } else if (net::isSaved(sSsid)) {
    startJoin(true);
  } else if (network.kind == Kind::OPEN) {
    startJoin(false);
  } else {
    keyboard::open(passwordOptions(nullptr), passwordDone);
  }
}

void scanUpdate() {
  net::hold();
  if (buttons::pressed(BTN_B)) {                   // CANCEL: the status page
    pop();
    resumeWifi();
    return;
  }
  if (sScanStarted) {
    if (!wifi_mgr::scanning()) {                   // it ended: build the list once
      sScanStarted = false;
      collectNetworks();
      repaint();
    } else if (millis() - sScanAt >= SCAN_TIMEOUT_MS) {
      sScanStarted = false;
      sScanFailed = true;
      repaint();
    }
    return;                                        // nothing to choose while it runs
  }
  if (buttons::pressed(BTN_RIGHT)) {
    scanStart();
    return;
  }
  const int count = scanRows();
  clampList(sScanList, count, LIST_ROWS);
  listMove(sScanList, count, LIST_ROWS);
  if (buttons::pressed(BTN_A)) scanSelect(sScanList.cursor);
}

void scanDraw() {
  if (sScanStarted) {
    frame("NETWORKS", "");
    textCentered(160, 104, "Looking for networks..", tk::INK);
    textCentered(160, 122, "a few seconds", tk::SUB);
    return;
  }

  const int count = scanRows();
  clampList(sScanList, count, LIST_ROWS);
  const bool onOther = sScanList.cursor >= sNetworkCount;
  const char *footLeft = "SELECT type the name  RIGHT rescan";
  if (!onOther) {
    const Network &under = sNetworks[sScanList.cursor];
    if (under.kind == Kind::LOGIN) {
      footLeft = "needs a login  RIGHT rescan";
    } else if (under.kind == Kind::OPEN || net::isSaved(under.ssid)) {
      footLeft = "SELECT join  RIGHT rescan";
    } else {
      footLeft = "SELECT password  RIGHT rescan";
    }
  }
  frame("NETWORKS", footLeft);
  drawPosition(sScanList, count, LIST_ROWS);

  for (int i = 0; i < LIST_ROWS && sScanList.scroll + i < count; ++i) {
    const int index = sScanList.scroll + i;
    const int y = LIST_Y + i * ROW_PITCH;
    const bool selected = index == sScanList.cursor;
    if (index >= sNetworkCount) {
      receipt::row(X0, X1, y, OTHER_ROW, "type a name", selected);
      continue;
    }
    const Network &network = sNetworks[index];
    char label[LOCK_COLS + sizeof network.ssid], value[16];
    snprintf(label, sizeof label, "%*s%s", LOCK_COLS, "", network.ssid);
    const char *note = joinedTo(network.ssid) ? "joined" : (net::isSaved(network.ssid) ? "saved" : (network.kind == Kind::LOGIN ? "login" : ""));
    snprintf(value, sizeof value, "%s%s", note, BARS_PAD);
    receipt::row(X0, X1, y, label, value, selected);
    if (network.kind != Kind::OPEN) drawLock(X0, y, selected);
    drawBars(X1 - BARS_W, y - 1, network.rssi, selected);
  }

  if (sScanFailed) {
    textCentered(160, 104, "The scan did not finish.", tk::STAMP_WARN);
    textCentered(160, 122, "RIGHT tries again", tk::SUB);
  } else if (sNetworkCount == 0) {
    textCentered(160, 104, "No network in range.", tk::SUB);
  }
}

const Screen kScan = {"wifi_scan", scanEnter, scanUpdate, scanDraw, REFRESH_MS};

// =====================================================================================================
// wifi_saved: one saved network: join it or forget it
// =====================================================================================================

void savedEnter() { sSavedList = List(); }

void savedUpdate() {
  net::hold();
  if (back()) return;                              // CANCEL
  listMove(sSavedList, 2, 2);
  if (!buttons::pressed(BTN_A)) return;

  if (sSavedList.cursor == 0) {                    // Join
    if (sSavedIsLogin) {
      // Upstream's enterprise profile: its CA comes out of the cert store, and it refuses to join
      // if that has gone missing.
      if (!wifi_mgr::connectEnterprise(sSsid, settings::enterpriseConfig(), false)) {
        badge_log::tagf("ui", "enterprise connect refused - see Console");
      }
      pop();
      return;
    }
    sFromScan = false;
    sHidden = false;
    sResume = wifi_mgr::mode() == wifi_mgr::Mode::Station;
    wipePassword();
    pop();                                         // `wifi_join` takes this screen's place
    startJoin(true);
    return;
  }

  // Forget. A network the badge is on is left as well (forgetting it and staying would be a lie),
  // and a radio that is still trying to reach it is stopped. What is left of the list is tried.
  const bool onIt = joinedTo(sSsid);
  const bool trying = wifi_mgr::mode() == wifi_mgr::Mode::Station && !wifi_mgr::connected();
  if (sSavedIsLogin) {
    settings::forgetWifi();
  } else {
    net::forget(sSsid);
  }
  if (onIt || trying) {
    wifi_mgr::disconnect();
    push_server::stop();
    net::joinBest();
  }
  pop();
}

void savedDraw() {
  frame("SAVED NETWORK", "SELECT choose");
  const bool joined = joinedTo(sSsid);
  receipt::row(X0, X1, 48, "NETWORK", sSsid);
  receipt::row(X0, X1, 66, "KEPT", sSavedIsLogin ? "a login, set on the web page" : (net::savedIsOpen(sSsid) ? "open: no password" : "its password"));
  receipt::row(X0, X1, 84, "STATE", joined ? "joined" : "not joined", false, onOffColor(joined));
  receipt::rule(100);
  const ListRow rows[] = {{"Join", "", 0}, {"Forget this network", "", 0}};
  listDraw(sSavedList, rows, 2, 110, 2);
}

const Screen kSaved = {"wifi_saved", savedEnter, savedUpdate, savedDraw, REFRESH_MS};

// =====================================================================================================
// wifi: status, the switches, the saved networks
// =====================================================================================================

// Upstream's one saved network when it is an enterprise profile: it is not in the list of
// wifi_net, which keeps networks with a password or none.
bool loginSaved() { return settings::wifiIsEnterprise() && settings::wifiSsid().length() > 0; }

int statusRows() { return STATUS_ACTIONS + (int)net::savedCount() + (loginSaved() ? 1 : 0); }

void wifiValue(char *out, size_t cap) { snprintf(out, cap, "%s", wifi_mgr::statusText()); }

void wifiEnter() {
  sStatusList = List();
  wipePassword();
}

void wifiSelect(int cursor) {
  const wifi_mgr::Mode mode = wifi_mgr::mode();
  if (cursor == 0) {
    push(&kScan);
  } else if (cursor == 1) {                        // Wi-Fi on / off
    if (mode == wifi_mgr::Mode::Station) {
      net::joinCancel();
      wifi_mgr::disconnect();
      push_server::stop();
    } else if (net::savedCount() > 0) {
      net::joinBest();                             // the strongest saved network in range
    } else if (loginSaved()) {
      if (!wifi_mgr::connectEnterprise(settings::wifiSsid(), settings::enterpriseConfig(), false)) {
        badge_log::tagf("ui", "enterprise connect refused - see Console");
      }
    } else {
      push(&kScan);                                // nothing saved: choose a network
    }
  } else if (cursor == 2) {                        // Hotspot on / off
    net::joinCancel();
    if (mode == wifi_mgr::Mode::AccessPoint) {
      wifi_mgr::disconnect();
      push_server::stop();
    } else {
      wifi_mgr::startAccessPoint();
      push_server::begin();
    }
  } else {
    const int saved = cursor - STATUS_ACTIONS;
    sSavedIsLogin = saved >= (int)net::savedCount();
    strlcpy(sSsid, sSavedIsLogin ? settings::wifiSsid().c_str() : net::savedSsid((size_t)saved), sizeof sSsid);
    push(&kSaved);
  }
}

void wifiUpdate() {
  if (back()) return;                              // CANCEL
  const int count = statusRows();
  clampList(sStatusList, count, STATUS_VISIBLE);
  listMove(sStatusList, count, STATUS_VISIBLE);
  if (buttons::pressed(BTN_A)) {
    wifiSelect(sStatusList.cursor);
    repaint();
  }
}

void wifiDraw() {
  const int count = statusRows();
  clampList(sStatusList, count, STATUS_VISIBLE);
  frame("WI-FI", sStatusList.cursor >= STATUS_ACTIONS ? "SELECT join or forget" : "SELECT choose");

  // Status block above the list: the state, then the network, the address and the signal.
  const wifi_mgr::Mode mode = wifi_mgr::mode();
  const bool station = mode == wifi_mgr::Mode::Station;
  const bool connected = wifi_mgr::connected();
  receipt::row(X0, X1, 48, "STATUS", wifi_mgr::statusText(), false, connected ? tk::color(tk::STAMP_OK) : (uint16_t)0);
  char line[96];
  if (connected && station) {
    snprintf(line, sizeof line, "%.16s  %s  %d dBm", wifi_mgr::ssid().c_str(), wifi_mgr::ip().toString().c_str(),
             wifi_mgr::rssi());
    drawBars(X1 - BARS_W, 60, wifi_mgr::rssi(), false);
  } else if (connected) {
    snprintf(line, sizeof line, "%.24s  %s", wifi_mgr::ssid().c_str(), wifi_mgr::ip().toString().c_str());
  } else if (station || wifi_mgr::scanning()) {    // joining, or the scan that picks a saved network
    snprintf(line, sizeof line, net::savedCount() > 0 || loginSaved() ? "looking for a saved network" : "not connected yet");
  } else {
    snprintf(line, sizeof line, "not connected");
  }
  receipt::subline(X0, X1 - BARS_W - 6, 61, line);
  receipt::rule(78);

  // A ListRow holds pointers: every string below outlives listDraw().
  const String login = loginSaved() ? settings::wifiSsid() : String();
  ListRow rows[STATUS_ACTIONS + net::MAX_SAVED + 1];
  rows[0] = {"Scan for networks", "", 0};
  rows[1] = {"Wi-Fi", onOff(station), onOffColor(station)};
  rows[2] = {"Hotspot", onOff(mode == wifi_mgr::Mode::AccessPoint), onOffColor(mode == wifi_mgr::Mode::AccessPoint)};
  int n = STATUS_ACTIONS;
  for (size_t i = 0; i < net::savedCount(); ++i) {
    const bool joined = joinedTo(net::savedSsid(i));
    rows[n++] = {net::savedSsid(i), joined ? "joined" : "saved", joined ? tk::color(tk::STAMP_OK) : tk::color(tk::SUB)};
  }
  if (login.length()) {
    const bool joined = joinedTo(login.c_str());
    rows[n++] = {login.c_str(), joined ? "joined" : "saved login", joined ? tk::color(tk::STAMP_OK) : tk::color(tk::SUB)};
  }
  listDraw(sStatusList, rows, n, STATUS_LIST_Y, STATUS_VISIBLE);
}

// ---- dev hook ---------------------------------------------------------------------------------------
// VKWIFIUI: where the Wi-Fi screens are, for device tests (testing.md, "Dev hooks"). Dev profile
// only. Names of networks and states; never a password.
#if VK_TEST_HOOKS
void jsonText(String &out, const char *s) {
  out += '"';
  for (; s != nullptr && *s; ++s) {
    if (*s == '"' || *s == '\\') out += '\\';
    out += (*s >= 0x20 && *s <= 0x7E) ? *s : '?';
  }
  out += '"';
}

void cmdWifiUi(const String &, const vk::serial::Reply &reply) {
  const char *view = sView == View::NEEDS_LOGIN ? "needs_login" : (sView == View::REFUSED ? "refused" : net::joinStateName());
  String out;
  out.reserve(256);
  out += "OK {\"scanning\":";
  out += sScanStarted ? "true" : "false";
  out += ",\"scan_failed\":";
  out += sScanFailed ? "true" : "false";
  out += ",\"networks\":" + String(sNetworkCount) + ",\"rows\":" + String(scanRows()) + ",\"cursor\":" + String(sScanList.cursor);
  out += ",\"row\":";
  jsonText(out, sScanList.cursor >= 0 && sScanList.cursor < sNetworkCount ? sNetworks[sScanList.cursor].ssid : OTHER_ROW);
  out += ",\"join\":";
  jsonText(out, view);
  out += ",\"ssid\":";
  jsonText(out, sSsid);
  out += ",\"saved\":" + String((unsigned)net::savedCount());
  out += ",\"mode\":";
  jsonText(out, wifi_mgr::statusText());
  out += '}';
  reply(out);
}

VK_SERIAL_COMMAND(vkwifiui, "VKWIFIUI", cmdWifiUi, "the Wi-Fi screens: scan list, cursor, join state (never a password)");
#endif  // VK_TEST_HOOKS

}  // namespace

VK_SETTINGS_PAGE(wifi, "wifi", 20, "Wi-Fi", wifiValue, wifiEnter, wifiUpdate, wifiDraw, REFRESH_MS);
