// Settings page `push` (docs/os/ui/shell.md, "App push"): where the web page is, the pairing code
// a pusher must present, and the three switches. Deleting this file removes the page and nothing
// else.
#include "../page.h"

#include <esp_wifi.h>
#include <string.h>

#include "../../../config.h"               // DEFAULT_AP_PASSWORD: the hotspot Settings > Wi-Fi starts
#include "../../../net/ble_mgr.h"
#include "../../../net/push_server.h"
#include "../../../net/wifi_mgr.h"
#include "../../../settings.h"

namespace {
using namespace vk::shell;
namespace receipt = vk::ui::receipt;
namespace tk = vk::ui::theme;

constexpr int ROWS = 3;
constexpr int ROWS_Y = 158;

List sList;

// As upstream's Settings row: the server also runs on the badge's own hotspot.
void pushValue(char *out, size_t cap) {
  snprintf(out, cap, "%s", push_server::running() ? "ready" : "needs wi-fi");
}

void pushEnter() { sList = List(); }

// The badge's own hotspot, as the radio has it (a Lua app may have started it with another password).
bool hotspot(char *ssid, size_t ssidCap, char *pass, size_t passCap) {
  if (wifi_mgr::mode() != wifi_mgr::Mode::AccessPoint) return false;
  wifi_config_t conf;
  if (esp_wifi_get_config(WIFI_IF_AP, &conf) != ESP_OK) return false;
  const size_t ssidLen = strnlen((const char *)conf.ap.ssid, sizeof conf.ap.ssid);
  snprintf(ssid, ssidCap, "%.*s", (int)ssidLen, (const char *)conf.ap.ssid);
  const size_t passLen = strnlen((const char *)conf.ap.password, sizeof conf.ap.password);
  snprintf(pass, passCap, "%.*s", (int)passLen, (const char *)conf.ap.password);
  return true;
}

void pushUpdate() {
  if (back()) return;                                  // CANCEL
  listMove(sList, ROWS, ROWS);
  if (buttons::pressed(BTN_A)) {
    if (sList.cursor == 0) {
      settings::setPushRequiresPairing(!settings::pushRequiresPairing());
    } else if (sList.cursor == 1) {
      settings::regeneratePairingCode();
      pulseLed(400);
    } else if (sList.cursor == 2) {
      if (ble_mgr::enabled()) {
        ble_mgr::end();
      } else {
        ble_mgr::begin(settings::deviceName());
      }
    }
    repaint();
  }
}

void pushDraw() {
  frame("APP PUSH", "SELECT choose");

  const bool up = push_server::running() && wifi_mgr::connected();
  receipt::row(X0, X1, 48, "WEB UI", up ? "ready" : "wi-fi is off", false, onOffColor(up));
  char ssid[33];
  char pass[65];
  if (up) {
    const String byAddress = "http://" + wifi_mgr::ip().toString() + "/";
    receipt::subline(X0, X1, 61, byAddress.c_str());
    if (hotspot(ssid, sizeof ssid, pass, sizeof pass)) {
      // On the badge's own hotspot a phone must join it first: say what to join.
      char line[112];
      snprintf(line, sizeof line, "hotspot %s  password %s", ssid, pass[0] ? pass : "(none)");
      text(22, 76, line, tk::SUB, 48);
    } else {
      const String byName = "http://" + settings::deviceName() + ".local/";
      text(22, 76, byName.c_str(), tk::SUB, 48);
    }
  } else {
    receipt::subline(X0, X1, 61, "Settings > Wi-Fi to connect");
    char line[64];
    snprintf(line, sizeof line, "or start the hotspot: password %s", DEFAULT_AP_PASSWORD);
    text(22, 76, line, tk::SUB, 48);
  }

  // The pairing code, big enough to read across a table.
  const bool pairing = settings::pushRequiresPairing();
  const String code = settings::pairingCode();
  receipt::amount(160, 92, pairing ? "PAIRING CODE" : "PAIRING CODE (NOT REQUIRED)", code.c_str(), "");
  receipt::rule(148);

  const bool ble = ble_mgr::enabled();
  const ListRow rows[ROWS] = {
      {"Require pairing code", onOff(pairing), onOffColor(pairing)},
      {"New pairing code", "", 0},
      {"BLE push", ble ? "advertising" : "off", onOffColor(ble)},
  };
  listDraw(sList, rows, ROWS, ROWS_Y, ROWS);
}
}  // namespace

VK_SETTINGS_PAGE(push, "push", 50, "App push", pushValue, pushEnter, pushUpdate, pushDraw, 1000);
