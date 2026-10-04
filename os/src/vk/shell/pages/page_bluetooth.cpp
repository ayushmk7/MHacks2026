// Settings page `bluetooth` (docs/os/ui/shell.md, "Bluetooth"). Deleting this file removes the
// page and nothing else.
#include "../page.h"

#include "../../../net/ble_mgr.h"
#include "../../../settings.h"

namespace {
using namespace vk::shell;
namespace receipt = vk::ui::receipt;
namespace tk = vk::ui::theme;

constexpr int ROWS = 3;

List sList;

void bluetoothValue(char *out, size_t cap) { snprintf(out, cap, "%s", onOff(ble_mgr::enabled())); }

void bluetoothEnter() { sList = List(); }

void bluetoothUpdate() {
  if (back()) return;                                  // CANCEL
  listMove(sList, ROWS);
  if (buttons::pressed(BTN_A)) {
    if (sList.cursor == 0) {
      if (ble_mgr::enabled()) {
        ble_mgr::end();
      } else {
        ble_mgr::begin(settings::deviceName());
      }
    } else if (sList.cursor == 2) {
      settings::setBleEnabledAtBoot(!settings::bleEnabledAtBoot());
    }
    repaint();
  }
}

void bluetoothDraw() {
  frame("BLUETOOTH", "SELECT toggle");

  const bool enabled = ble_mgr::enabled();
  const bool connected = ble_mgr::connected();
  const bool atBoot = settings::bleEnabledAtBoot();
  const ListRow rows[ROWS] = {
      {"Bluetooth LE", onOff(enabled), onOffColor(enabled)},
      {"Connected", onOff(connected), onOffColor(connected)},
      {"Enable at boot", onOff(atBoot), onOffColor(atBoot)},
  };
  listDraw(sList, rows, ROWS);

  receipt::rule(106);
  const String address = ble_mgr::address();
  text(10, 116, "Nordic UART service", tk::SUB);
  text(10, 130, address.length() ? address.c_str() : "6e400001-b5a3-f393-e0a9-e50e24dcca9e", tk::SUB);
}
}  // namespace

VK_SETTINGS_PAGE(bluetooth, "bluetooth", 30, "Bluetooth", bluetoothValue, bluetoothEnter,
                 bluetoothUpdate, bluetoothDraw, 1000);
