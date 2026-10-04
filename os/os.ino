/*
  Solana OS - application firmware for the Solana Badge (ESP32-S3, v1 board).

  A Lua runtime with an SDK over every badge peripheral, a launcher and settings
  UI, and app delivery over Wi-Fi or BLE. For the hardware bring-up and factory
  test image, see ../testkit/.

  The full reference - Lua API, app format, push protocol, build steps - is in
  README.md next to this file.

  Board settings: ESP32S3 Dev Module, 16MB flash, 8MB OPI PSRAM,
  partition scheme from partitions.csv. See README.md#building.
*/

#include <Arduino.h>
#include <Wire.h>
#include <bootloader_random.h>  // bootloader_random_enable/disable - TRNG seeding

#include "src/apps/app_store.h"
#include "src/badge_log.h"
#include "src/config.h"
#include "src/hal/badge_i2c.h"
#include "src/hal/buttons.h"
#include "src/hal/display.h"
#include "src/hal/leds.h"
#include "src/hal/mic.h"
#include "src/hal/power.h"
#include "src/hal/se050.h"
#include "src/identity/identity.h"
#include "src/lua_sdk/lua_runtime.h"
#include "src/net/ble_bridge.h"
#include "src/net/ble_mgr.h"
#include "src/net/broker_client.h"
#include "src/net/cert_store.h"
#include "src/net/espnow_mgr.h"
#include "src/net/push_protocol.h"
#include "src/net/push_server.h"
#include "src/net/wifi_mgr.h"
#include "src/settings.h"
#include "src/ui/boot.h"
#include "src/ui/shell.h"
#include "src/vk/vk.h"  // VK: H1

namespace {

// True while an app was running at the top of this tick. Compared against the
// state at the bottom to notice an app that exited or died mid-frame.
bool sAppWasRunning = false;

// Serial console line being accumulated. The same push protocol the BLE link
// speaks is available over USB, which is how an app gets onto a badge that has
// no radio configured yet.
String sSerialLine;

uint32_t sLastHeartbeatAt = 0;

void logBanner() {
  badge_log::println("");
  badge_log::println("###################################################");
  badge_log::printf("#  %s %s\n", SOLANA_OS_NAME, SOLANA_OS_VERSION);
  badge_log::printf("#  built %s %s\n", __DATE__, __TIME__);
  badge_log::printf("#  chip %s rev%u  %u MHz  flash %luMB  psram %luKB\n", ESP.getChipModel(),
                    ESP.getChipRevision(), (unsigned)ESP.getCpuFreqMHz(),
                    (unsigned long)(ESP.getFlashChipSize() / (1024 * 1024)),
                    (unsigned long)(ESP.getPsramSize() / 1024));
  badge_log::println("###################################################");
}

// Buttons go to the running app, or to the shell when there is none. The one
// exception is a long hold on B, which always force-quits back to the launcher
// - an app that traps every button must never be able to strand the user.
void routeButtons() {
  if (!runtime::running()) return;

  if (buttons::heldMs(BTN_B) >= APP_ESCAPE_HOLD_MS) {
    badge_log::tagf("os", "force-quit '%s' (B held)", runtime::currentApp().c_str());
    runtime::requestStop();
    return;
  }

  for (uint8_t key = 0; key < BUTTON_COUNT; ++key) {
    if (buttons::pressed(key)) runtime::dispatchButton(key, true);
    if (buttons::released(key)) runtime::dispatchButton(key, false);
  }
}

// The push protocol over USB serial, one line at a time.
void pumpSerialConsole() {
  int value;
  while ((value = badge_log::readCommand()) >= 0) {
    const char c = (char)value;
    if (c == '\n') {
      if (sSerialLine.length()) {
        const push_protocol::Reply reply = [](const String &text) { badge_log::println(text.c_str()); };  // VK: H6
        if (!vk::serial::handleLine(sSerialLine, reply)) push_protocol::handleLine(sSerialLine, reply);   // VK: H6
      }
      sSerialLine = "";
    } else if (c != '\r') {
      if (sSerialLine.length() < 8192) sSerialLine += c;
    }
  }
}

void heartbeat() {
  if (millis() - sLastHeartbeatAt < 30000) return;
  sLastHeartbeatAt = millis();
  // The button fields are here because "the keys do nothing" is the one fault
  // that cannot be reported from the UI - the UI needs the keys. btn=-- means
  // the expander is not answering; btn=00 with int=H and a key held means it
  // answers but the press is not reaching P0..P5.
  badge_log::tagf("os", "up %lus  heap %uKB  psram %uKB  batt %d%%  btn=%s int=%c  %s",
                  (unsigned long)(millis() / 1000), (unsigned)(ESP.getFreeHeap() / 1024),
                  (unsigned)(ESP.getFreePsram() / 1024), (int)power::percent(),
                  buttons::present() ? String(buttons::downMask(), HEX).c_str() : "--",
                  digitalRead(PIN_BUTTON_INT) ? 'H' : 'L',
                  runtime::running() ? runtime::currentApp().c_str() : "launcher");
}

// Radios are brought up after the UI so a badge with a broken radio still
// reaches a usable launcher.
void startRadios() {
  wifi_mgr::begin();

  if (settings::espnowEnabledAtBoot()) {
    espnow_mgr::begin(settings::espnowChannel());
  }
  if (settings::bleEnabledAtBoot()) {
    ble_mgr::begin(settings::deviceName());
  }

  // ESP-NOW frames reach the running app; with no app up they are simply
  // dropped, since the beacons that drive the radar are handled inside
  // espnow_mgr and never come through here.
  vk::host::router::install();  // VK: H3

  // The app-store client is a radio consumer rather than a radio: begin() only
  // reads settings and picks up any stored token, and the first request waits
  // for wifi_mgr::connected() inside update(). Starting it here keeps all the
  // network-facing state in one place.
  broker::begin();
}

}  // namespace

void setup() {
  badge_log::begin(115200);
  badge_log::waitForUsb(1200);
  delay(80);
  logBanner();

  // Turn on a true hardware entropy source BEFORE anything draws random bytes.
  // esp_fill_random()/esp_random() are only a real TRNG while an RF subsystem is
  // running; the radios do not come up until startRadios(), so without this the
  // Ed25519 identity seed (identity::begin) and the 6-digit pairing code
  // (settings::begin) would both be drawn from a boot-seeded PRNG. This stays on
  // through both of those and is handed back to the RF driver in startRadios().
  bootloader_random_enable();

  // The serial push console appends a byte at a time up to 8 KB per line; one
  // reserve here turns that from O(n^2) reallocation churn into O(n) appends.
  sSerialLine.reserve(8192);

  settings::begin();

  leds::begin();
  leds::setBrightness(settings::ledBrightness());

  // ENA before the bus comes up, matching the test kit: the secure element gets
  // its whole boot time to settle before anything addresses it, instead of
  // being released into a bus that is already live.
  se050::begin();
  badge_i2c::begin();

  if (!display::begin()) {
    // Without a framebuffer there is no UI at all; keep logging so the failure
    // is at least diagnosable over serial.
    badge_log::println("display init failed - running headless");
  }

  // Splash + LED animation. Everything after this can report progress.
  boot::run();

  boot::progress("Storage", "mounting filesystem", 20);
  app_store::begin();
  cert_store::begin();  // must follow app_store::begin(); it mounts LittleFS

  boot::progress("Peripherals", "buttons, battery, secure element", 45);
  // Logged either way: a silent failure here is a badge with no buttons and
  // nothing in the log to say so, which is exactly how this went unnoticed. A
  // failure is no longer terminal - buttons::update() keeps probing - but it is
  // still the first line to look for when the keys do nothing.
  badge_log::tagf("btn", "TCA9534 init %s",
                  buttons::begin() ? "ok" : "FAILED (will keep re-probing)");
  power::begin();
  se050::test();
  badge_i2c::scan();

  // Deliberately its own stage rather than part of "Peripherals": it must run
  // after se050::test(), because whether the secure element answered decides
  // which of the two key paths identity takes, and on a first boot it spends a
  // second or two generating a keypair - long enough that a stage with no
  // progress line of its own would look like a hang.
  boot::progress("Identity", "ed25519 keypair", 55);
  identity::begin();

  boot::progress("Runtime", "starting Lua", 65);
  runtime::begin();
  boot::progress("Wallet", "config, keys, stores", 75);  // VK: H2
  vk::begin();                                           // VK: H2

  boot::progress("Radios", "wi-fi, esp-now, bluetooth", 85);
  // Hand the RNG back before the radios start: once Wi-Fi/BT are up the RF
  // driver owns and continuously reseeds the hardware entropy pool, and the
  // bootloader entropy source must be disabled before that transition. All key
  // and pairing-code generation has already happened above under it.
  bootloader_random_disable();
  startRadios();

  boot::progress("Ready", settings::deviceName().c_str(), 100);
  delay(300);

  shell::begin();
  leds::playIdle();

  // An autostart app takes over immediately; the launcher is still one long
  // press of B away.
  const String autostart = settings::autostartApp();
  if (autostart.length() && app_store::exists(autostart)) {
    badge_log::tagf("os", "autostarting '%s'", autostart.c_str());
    runtime::requestLaunch(autostart);
  }

  badge_log::tagf("os", "ready");
}

void loop() {
  // Captured before anything else: a push over HTTP, BLE or serial can stop the
  // running app part-way through this tick, and the check at the bottom has to
  // see that it was running when the tick began.
  sAppWasRunning = runtime::running();

  // --- Input and sensors --------------------------------------------------
  buttons::update();
  power::update();
  mic::update();
  leds::update();

  // --- Radios -------------------------------------------------------------
  wifi_mgr::update();
  espnow_mgr::update();
  ble_mgr::update();
  // ble_mgr::update() already fed the bridge every frame it had; this is where a
  // disconnect noticed on the BLE stack task actually tears the bridge down, on
  // the main loop, where freeing the response buffer is safe.
  ble_bridge::update();

  // The HTTP server only makes sense once there is an address to reach it on,
  // and starting it before that leaves a socket bound to nothing.
  if (wifi_mgr::connected() && !push_server::running()) {
    push_server::begin();
  } else if (!wifi_mgr::connected() && push_server::running()) {
    push_server::stop();
  }
  push_server::update();

  // Outbound half of the same job: push_server waits for someone to reach the
  // badge, broker::update() reaches out. It costs one HTTP request every
  // BROKER_POLL_MS and writes at most one script per tick, so it sits in the
  // loop next to the server rather than in a task of its own - see the note at
  // the top of broker_client.h.
  broker::update();
  vk::update();  // VK: H5

  pumpSerialConsole();

  // --- App / shell --------------------------------------------------------
  if (vk::modalActive()) {     // VK: H4
    vk::modalUpdate();         // VK: H4
  } else {                     // VK: H4
    routeButtons();
    if (runtime::running()) {
      runtime::update();
    } else {
      shell::update();
    }
  }                            // VK: H4

  // Launch and stop requests are applied here, between frames, never inside a
  // Lua callback - see the note in lua_runtime.h.
  //
  // An app whose on_start() errors, or whose script does not even load, dies
  // inside this call and never shows up in running() - so sAppWasRunning, which
  // was sampled before the launch happened, is false and the branch below would
  // skip it. The badge would drop back to the launcher with no explanation and
  // the error would sit in runtime::lastError() until some unrelated app exited
  // and wore the blame for it.
  const bool lifecycleRan = vk::modalActive() ? false : runtime::processRequests();  // VK: H4

  if ((sAppWasRunning || lifecycleRan) && !runtime::running()) {
    // The app exited or errored. Peripheral cleanup already happened inside
    // runtime::stop(); what is left is releasing the BLE link back to the push
    // protocol and telling the shell to come back up.
    push_protocol::reset();
    ble_bridge::reset();
    shell::onAppStopped();
  }

  display::flush();
  heartbeat();

  // Yield to the Wi-Fi and BLE tasks. Without this the badge still works but
  // the radios get starved and throughput collapses.
  delay(1);
}
