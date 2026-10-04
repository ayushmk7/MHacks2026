// Settings page `wifi` (docs/os/ui/shell.md, "Wi-Fi").
//
// The badge has no keyboard: an open network is joined from here; a secured one is set up from a
// phone through the hotspot and the web page, then re-joined from here ("Connect saved network").
// Deleting this file removes the page and nothing else.
#include "../page.h"

#include "../../../badge_log.h"
#include "../../../net/push_server.h"
#include "../../../net/wifi_mgr.h"
#include "../../../settings.h"

namespace {
using namespace vk::shell;
namespace receipt = vk::ui::receipt;
namespace tk = vk::ui::theme;

// Fixed rows above the scan results. Shared by update and draw so the two cannot disagree about
// where the scan list starts.
constexpr int ACTIONS = 5;
constexpr int VISIBLE = 7;            // list rows on screen, from LIST_TOP
constexpr int LIST_TOP = 88;
constexpr int MAX_NETWORKS = 59;      // scan results listed; with the actions, 64 rows
constexpr int MAX_ROWS = ACTIONS + MAX_NETWORKS;

List sList;

int networkCount() {
  if (wifi_mgr::scanning()) return 0;
  const int n = wifi_mgr::scanResultCount();
  return n < 0 ? 0 : (n > MAX_NETWORKS ? MAX_NETWORKS : n);
}

// The row count changes without input (a scan starts or ends), so the cursor and the window are
// brought back inside it before either is used.
void clampList(int count) {
  if (sList.cursor >= count) sList.cursor = count - 1;
  if (sList.cursor < 0) sList.cursor = 0;
  if (sList.scroll > count - VISIBLE) sList.scroll = count - VISIBLE;
  if (sList.scroll < 0) sList.scroll = 0;
  if (sList.cursor < sList.scroll) sList.scroll = sList.cursor;
  if (sList.cursor >= sList.scroll + VISIBLE) sList.scroll = sList.cursor - VISIBLE + 1;
}

void wifiValue(char *out, size_t cap) { snprintf(out, cap, "%s", wifi_mgr::statusText()); }

void wifiEnter() { sList = List(); }

void wifiSelect(int cursor) {
  if (cursor == 0) {
    wifi_mgr::startScan();
  } else if (cursor == 1) {
    // Re-join whatever is saved, personal or enterprise. The enterprise path reads its CA out of
    // the cert store and refuses if it has gone missing.
    const String saved = settings::wifiSsid();
    if (saved.length() == 0) {
      badge_log::tagf("ui", "no saved network - set one up from the web UI");
    } else if (settings::wifiIsEnterprise()) {
      if (!wifi_mgr::connectEnterprise(saved, settings::enterpriseConfig(), false)) {
        badge_log::tagf("ui", "enterprise connect refused - see Console");
      }
    } else {
      wifi_mgr::connect(saved, settings::wifiPassword(), false);
    }
  } else if (cursor == 2) {
    wifi_mgr::startAccessPoint();
    push_server::begin();
  } else if (cursor == 3) {
    wifi_mgr::disconnect();
    push_server::stop();
  } else if (cursor == 4) {
    settings::forgetWifi();
  } else {
    const int network = cursor - ACTIONS;
    if (!wifi_mgr::scanEncrypted(network)) {
      wifi_mgr::connect(wifi_mgr::scanSsid(network), "", true);
    } else {
      badge_log::tagf("ui", "'%s' is secured - set it up from the web UI",
                      wifi_mgr::scanSsid(network).c_str());
    }
  }
}

void wifiUpdate() {
  if (back()) return;                                  // CANCEL
  const int count = ACTIONS + networkCount();
  clampList(count);
  listMove(sList, count, VISIBLE);
  if (buttons::pressed(BTN_A)) {
    wifiSelect(sList.cursor);
    repaint();
  }
}

void wifiDraw() {
  const int networks = networkCount();
  const int count = ACTIONS + networks;
  clampList(count);

  const bool onSecured = sList.cursor >= ACTIONS && wifi_mgr::scanEncrypted(sList.cursor - ACTIONS);
  frame("WI-FI", onSecured ? "* needs a password: use the hotspot" : "SELECT choose");

  // Status block above the list.
  const bool connected = wifi_mgr::connected();
  receipt::row(X0, X1, 48, "STATUS", wifi_mgr::statusText(), false,
               connected ? tk::color(tk::STAMP_OK) : (uint16_t)0);
  char line[96];
  if (connected) {
    snprintf(line, sizeof(line), "%s  %s  %d dBm", wifi_mgr::ssid().c_str(),
             wifi_mgr::ip().toString().c_str(), wifi_mgr::rssi());
  } else {
    snprintf(line, sizeof(line), "not connected");
  }
  receipt::subline(X0, X1, 61, line);
  receipt::rule(78);

  // A ListRow holds pointers: every string below is a named local that outlives listDraw().
  const String saved = settings::wifiSsid();
  const String savedLabel =
      saved.length() ? (settings::wifiIsEnterprise() ? saved + " (EAP)" : saved) : String("-");
  const String forgetLabel = saved.length() ? saved : String("-");
  const bool hotspot = wifi_mgr::mode() == wifi_mgr::Mode::AccessPoint;

  static ListRow rows[MAX_ROWS];
  rows[0] = {"Scan for networks", wifi_mgr::scanning() ? "scanning.." : "", 0};
  rows[1] = {"Connect saved network", savedLabel.c_str(), 0};
  rows[2] = {"Start hotspot", hotspot ? "on" : "", onOffColor(true)};
  rows[3] = {"Disconnect", "", 0};
  rows[4] = {"Forget saved network", forgetLabel.c_str(), 0};

  // Scan results. Only the rows inside the window are read from the radio driver and given text;
  // listDraw() draws no other.
  String labels[VISIBLE];
  char values[VISIBLE][16];
  for (int i = ACTIONS; i < count; ++i) {
    const int slot = i - sList.scroll;
    if (slot < 0 || slot >= VISIBLE) {
      rows[i] = {"", "", 0};
      continue;
    }
    const int network = i - ACTIONS;
    labels[slot] = wifi_mgr::scanSsid(network);
    if (wifi_mgr::scanEncrypted(network)) labels[slot] = "* " + labels[slot];   // cannot be joined from here
    snprintf(values[slot], sizeof(values[slot]), "%d dBm", wifi_mgr::scanRssi(network));
    rows[i] = {labels[slot].c_str(), values[slot], 0};
  }

  listDraw(sList, rows, count, LIST_TOP, VISIBLE);
}
}  // namespace

VK_SETTINGS_PAGE(wifi, "wifi", 20, "Wi-Fi", wifiValue, wifiEnter, wifiUpdate, wifiDraw, 250);
