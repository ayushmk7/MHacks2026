// Settings page `push` (docs/os/ui/shell.md, "App push"): where the web page is, the pairing code
// a pusher must present, and the three switches. Deleting this file removes the page and nothing
// else.
#include "../page.h"

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
  if (up) {
    const String byAddress = "http://" + wifi_mgr::ip().toString() + "/";
    const String byName = "http://" + settings::deviceName() + ".local/";
    receipt::subline(X0, X1, 61, byAddress.c_str());
    text(22, 76, byName.c_str(), tk::SUB, 48);
  } else {
    receipt::subline(X0, X1, 61, "Settings > Wi-Fi to connect");
    text(22, 76, "or start the hotspot", tk::SUB, 48);
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
