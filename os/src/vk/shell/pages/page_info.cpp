// Settings page `info` (shell.md, "Device info"): the twelve fields upstream shows, and the one
// place a wedged I2C bus can be retried by hand.

#include "../page.h"

#include "../../../apps/app_store.h"
#include "../../../hal/badge_i2c.h"
#include "../../../hal/power.h"
#include "../../../hal/se050.h"
#include "../../../net/wifi_mgr.h"
#include "../../../settings.h"

namespace {
using namespace vk::shell;
namespace th = vk::ui::theme;

constexpr int FIELDS = 12;

int sScroll = 0;   // first visible row: the rows cannot be selected, so UP/DOWN scroll by one

void pageValue(char *out, size_t cap) { snprintf(out, cap, "%s", SOLANA_OS_VERSION); }

// app_store::usedBytes() walks the filesystem (about 290 ms on the badge), so the STORAGE row is
// read when the page opens and on SELECT, not on every repaint.
char sStorage[32] = "";

void readStorage() {
  snprintf(sStorage, sizeof sStorage, "%u / %u KB", (unsigned)(app_store::usedBytes() / 1024),
           (unsigned)(app_store::totalBytes() / 1024));
}

void pageEnter() {
  sScroll = 0;
  readStorage();
}

void pageUpdate() {
  if (back()) return;  // CANCEL
  const int maxScroll = FIELDS > LIST_ROWS ? FIELDS - LIST_ROWS : 0;
  if (buttons::repeated(BTN_UP) && sScroll > 0) {
    --sScroll;
    repaint();
  }
  if (buttons::repeated(BTN_DOWN) && sScroll < maxScroll) {
    ++sScroll;
    repaint();
  }
  if (buttons::pressed(BTN_A)) {
    // With hook H21 the last two do nothing.
    badge_i2c::retry();
    buttons::retry();
    se050::test();
    badge_i2c::scan();
    readStorage();
    repaint();
  }
}

void pageDraw() {
  frame("DEVICE INFO", "SELECT re-scan I2C");

  const uint16_t ok = th::color(th::STAMP_OK);
  const uint16_t warn = th::color(th::STAMP_WARN);
  const uint16_t bad = th::color(th::STAMP_BAD);

  // A ListRow holds pointers: every string below outlives the listDraw call.
  const String device = settings::deviceName();
  const String mac = wifi_mgr::macAddress();

  char chip[40];
  snprintf(chip, sizeof chip, "%s rev%u @%uMHz", ESP.getChipModel(), (unsigned)ESP.getChipRevision(),
           (unsigned)ESP.getCpuFreqMHz());
  char heap[24];
  snprintf(heap, sizeof heap, "%u KB free", (unsigned)(ESP.getFreeHeap() / 1024));
  char psram[32];
  snprintf(psram, sizeof psram, "%u / %u KB", (unsigned)(ESP.getFreePsram() / 1024),
           (unsigned)(ESP.getPsramSize() / 1024));
  const char *storage = sStorage;
  char battery[32];
  snprintf(battery, sizeof battery, "%.2fV  %d%%%s", power::volts(), (int)power::percent(),
           power::charging() ? " chg" : "");
  char uptime[16];
  snprintf(uptime, sizeof uptime, "%lus", (unsigned long)(millis() / 1000));

  const uint16_t batteryColor = power::percent() < 15 ? bad : (uint16_t)0;
  const bool busDown = badge_i2c::down();
  const bool keys = buttons::present();
  const bool se = se050::present();

  const ListRow rows[FIELDS] = {
      {"FIRMWARE", SOLANA_OS_NAME " " SOLANA_OS_VERSION, 0},
      {"DEVICE", device.c_str(), 0},
      {"CHIP", chip, 0},
      {"HEAP", heap, 0},
      {"PSRAM FREE", psram, 0},
      {"STORAGE", storage, 0},
      {"BATTERY", battery, batteryColor},
      {"I2C BUS", busDown ? "held low" : "ok", busDown ? bad : ok},
      {"BUTTONS", keys ? "TCA9534 ok" : "not found", keys ? ok : bad},
      {"SE050", se ? "ATR ok" : "no answer", se ? ok : warn},
      {"WIFI MAC", mac.c_str(), 0},
      {"UPTIME", uptime, 0},
  };

  List list;
  list.cursor = -1;      // nothing is selectable
  list.scroll = sScroll;
  listDraw(list, rows, FIELDS);
}
}  // namespace

VK_SETTINGS_PAGE(info, "info", 120, "Device info", pageValue, pageEnter, pageUpdate, pageDraw, 1000);
