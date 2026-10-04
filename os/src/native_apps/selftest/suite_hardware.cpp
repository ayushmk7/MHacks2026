// src/native_apps/selftest/suite_hardware.cpp
// HARDWARE: the first Self test's 14 checks, unchanged in behaviour and in their serial lines.
//
//   manual      display, buttons, leds        SELECT on the row; each can be stopped with CANCEL
//   automatic   battery, mic, i2c, storage, nvs, key, crypto, clock, wifi, espnow, memory
//               run when the app opens, one step per frame
//
// Keys of the manual tests:
//   display test  SELECT steps through the test screens and confirms after the last, CANCEL stops
//   LED test      SELECT confirms the LEDs were seen, CANCEL stops
//   button test   each of the six keys is asked for in turn (CANCEL last) and only that key ticks
//                 its row. A key not pressed within 20 s is marked FAIL and the test moves on; a
//                 short CANCEL while another key is asked for gives that key up at once. Six CANCEL
//                 presses or two minutes end the test; holding CANCEL for 1.5 s leaves the app
//                 from anywhere (the firmware's own force-quit).
//
// The secure element at I2C address 0x48 is quarantined (upstream-hooks.md, H21): this file never
// addresses it. The bus check probes four known addresses one at a time with badge_i2c::readReg and
// refuses 0x48 at compile time and at run time; badge_i2c::scan() and everything in se050* are not
// called.
//
// The test screens of the display check are the one place where colours are not theme tokens: a
// display test has to show pure red, green, blue, white and black.
#include "../../vk/sdk/badge_sdk.hpp"

#include <stdio.h>
#include <string.h>

#include "../../badge_log.h"
#include "../../hal/badge_i2c.h"
#include "../../hal/mic.h"
#include "../../hal/power.h"
#include "../../net/wifi_mgr.h"
#include "../../vk/ui/receipt.h"
#include "../../vk/ui/theme.h"
#include "../../vk/wallet/crypto.h"    // verify() only
#include "selftest.h"
#include "shared.h"
#include "suites.h"
#include "ui.h"

namespace selftest {
namespace {

namespace rc = vk::ui::receipt;
namespace th = vk::ui::theme;

// ---- limits of the automatic checks -----------------------------------------------------------
constexpr float BATTERY_MIN_V = 2.5f;    // below this the divider is reading nothing
constexpr float BATTERY_MAX_V = 5.6f;    // above USB's 5 V and its tolerance: not a real reading
constexpr uint32_t MIC_LISTEN_MS = 2000;
constexpr uint32_t MIC_SETTLE_MS = 300;  // the driver's offset filter needs a moment after power-up
constexpr float MIC_NO_SAMPLES_DB = -98.0f;   // mic.h reports -99 until a block of samples was read
constexpr uint32_t HEAP_MIN_BYTES = 16 * 1024;
constexpr uint32_t WIFI_SCAN_TIMEOUT_MS = 8000;
constexpr char CONFIG_KEY[] = "hold_ms";      // a key the wallet core always registers

// What the checks took and must give back when the app stops (stopHardware()).
bool sMicOurs = false;                   // this app turned the microphone on
bool sScanOurs = false;                  // this app started a scan that has not been cleared
bool sLedsTouched = false;

// ---- I2C addresses probed one at a time -------------------------------------------------------
// The button expander, the touch controller's two possible addresses, and 0x4A. Never the secure
// element: the list is checked at compile time, and probe() refuses the address again at run time.
constexpr uint8_t PROBES[] = {TCA9534_ADDR, 0x14, 0x5D, 0x4A};
constexpr size_t PROBE_COUNT = sizeof(PROBES) / sizeof(PROBES[0]);
constexpr bool probesAvoidQuarantine() {
  for (size_t i = 0; i < PROBE_COUNT; ++i) {
    if (PROBES[i] == SE050_ADDR) return false;
  }
  return true;
}
static_assert(probesAvoidQuarantine(), "the self test must never address the SE050 (H21)");

// One read of register 0 from `address`: true when a device answered. The lowest call upstream has
// that takes an address; an address that does not answer costs one NACK, not a bus recovery.
bool probe(uint8_t address) {
  if (address == SE050_ADDR) return false;      // H21
  uint8_t value = 0;
  return badge_i2c::readReg(address, 0x00, &value, 1);
}

// ---- automatic checks -------------------------------------------------------------------------

// Battery: the cell voltage, and the percentage upstream maps it to. On external power the ADC
// reads the charger, so the figure is "USB", as in the header.
void checkBattery(Ctx &c) {
  const float volts = power::volts();
  const State state = volts >= BATTERY_MIN_V && volts <= BATTERY_MAX_V ? State::Ok : State::Fail;
  if (power::charging()) c.finish(state, "%.2fV USB", (double)volts);
  else c.finish(state, "%.2fV %.0f%%", (double)volts, (double)power::percent());
}

// Microphone: on, two seconds of levels (the main loop's mic::update() reads the samples), the
// loudest block of each channel. It stays on for the log's summary line until the app stops.
struct MicScratch { bool seen; float left, right; };
void checkMic(Ctx &c) {
  MicScratch &s = c.scratch<MicScratch>();
  if (c.step == 0) {
    if (!mic::enabled()) {
      if (!mic::enable()) {
        c.finish(State::Fail, "did not start");
        return;
      }
      sMicOurs = true;
    }
    s.seen = false;
    s.left = s.right = MIC_NO_SAMPLES_DB;
    c.step = 1;
    c.stepAt = c.now;
    return;
  }
  const uint32_t elapsed = c.now - c.stepAt;
  if (elapsed >= MIC_SETTLE_MS) {
    const float left = mic::dbLeft(), right = mic::dbRight();
    if (left > MIC_NO_SAMPLES_DB || right > MIC_NO_SAMPLES_DB) s.seen = true;
    if (left > s.left) s.left = left;
    if (right > s.right) s.right = right;
  }
  if (elapsed < MIC_LISTEN_MS) return;
  if (!s.seen) c.finish(State::Fail, "no samples");
  else c.finish(State::Ok, "peak L%.0f R%.0f dB", (double)s.left, (double)s.right);
}

// I2C bus: both lines high, the button expander answering, and which of the known addresses
// answer. One probe per frame. With a line held low nothing is probed: a transfer could not work
// and would only feed upstream's bus recovery.
struct I2cScratch { bool linesHigh; uint8_t answered; };   // bit i: PROBES[i] answered
void checkI2c(Ctx &c) {
  I2cScratch &s = c.scratch<I2cScratch>();
  if (c.step == 0) {
    const bool sda = digitalRead(PIN_I2C_SDA) == HIGH, scl = digitalRead(PIN_I2C_SCL) == HIGH;
    badge_log::tagf(TAG, "i2c lines SDA=%s SCL=%s", sda ? "HIGH" : "LOW", scl ? "HIGH" : "LOW");
    s.linesHigh = sda && scl;
    s.answered = 0;
    c.step = 1;
    if (s.linesHigh) return;
    c.step = (uint8_t)(1 + PROBE_COUNT);       // straight to the result
  }
  const uint8_t index = (uint8_t)(c.step - 1);
  if (index < PROBE_COUNT) {
    const uint8_t address = PROBES[index];
    if (probe(address)) {
      s.answered |= (uint8_t)(1u << index);
      badge_log::tagf(TAG, "i2c 0x%02X answers (%s)", (unsigned)address, badge_i2c::knownDeviceName(address));
    } else {
      badge_log::tagf(TAG, "i2c 0x%02X no answer", (unsigned)address);
    }
    ++c.step;
    return;
  }

  badge_log::tagf(TAG, "i2c 0x%02X skipped (quarantined)", (unsigned)SE050_ADDR);
  if (!s.linesHigh) {
    c.finish(State::Fail, "a line is held low");
    return;
  }
  char list[24] = "";
  for (size_t i = 0; i < PROBE_COUNT; ++i) {
    if ((s.answered & (1u << i)) == 0) continue;
    char one[8];
    snprintf(one, sizeof one, "%s0x%02X", list[0] ? " " : "", (unsigned)PROBES[i]);
    strlcat(list, one, sizeof list);
  }
  if (buttons::present()) c.finish(State::Ok, "%s, 0x%02X skipped", list[0] ? list : "none", (unsigned)SE050_ADDR);
  else c.finish(State::Fail, "buttons 0x%02X no answer", (unsigned)TCA9534_ADDR);
}

// Storage: the filesystem is mounted, has room, and a small file can be written, read back and
// deleted through the one file layer.
void checkStorage(Ctx &c) {
  char value[VALUE_MAX];
  const State state = storageProbe(value, sizeof value);
  c.finish(state, "%s", value);
}

// Settings store: one value read through vk::config. (The header has no way to ask whether the
// NVS namespace opened; a store that did not open answers with the key's default.)
void checkNvs(Ctx &c) {
  if (vk::config::find(CONFIG_KEY) == nullptr) {
    c.finish(State::Fail, "no key %s", CONFIG_KEY);
    return;
  }
  const uint32_t value = vk::config::u32(CONFIG_KEY);
  const char *provisioned = vk::config::provisioned() ? "provisioned" : "setup needed";
  if (value == 0) c.finish(State::Fail, "%s unreadable", CONFIG_KEY);
  else c.finish(State::Ok, "%s %lu, %s", CONFIG_KEY, (unsigned long)value, provisioned);
}

// Key: where it is kept and that the badge has an identity. Nothing is signed.
void checkKey(Ctx &c) {
  const char *location = vk::wallet::keyLocation();          // "se050" | "software" | "none"
  if (vk::wallet::publicKey() == nullptr) {
    c.finish(State::Fail, "no identity (%s)", location);
    return;
  }
  const String address = vk::wallet::addressBase58();
  const size_t length = address.length();
  if (length >= 8) c.finish(State::Ok, "%s %.4s..%s", location, address.c_str(), address.c_str() + length - 4);
  else c.finish(State::Ok, "%s", location);
}

// Crypto: the published signature verifies, and neither a changed signature nor a changed message
// does. One verification per frame.
struct CryptoScratch { bool good, badAccepted; uint32_t ms; };
void checkCrypto(Ctx &c) {
  CryptoScratch &s = c.scratch<CryptoScratch>();
  const Ed25519Vector &v = RFC8032[RFC8032_HARDWARE];
  if (c.step == 0) {
    const uint32_t before = (uint32_t)micros();
    s.good = vk::wallet::verify(v.message, v.length, v.signature, v.key);
    s.ms = ((uint32_t)micros() - before + 500) / 1000;
    c.step = 1;
    return;
  }
  if (c.step == 1) {
    uint8_t signature[64];
    memcpy(signature, v.signature, sizeof signature);
    signature[0] ^= 0x01;
    s.badAccepted = vk::wallet::verify(v.message, v.length, signature, v.key);
    c.step = 2;
    return;
  }
  const uint8_t message[1] = {(uint8_t)(v.message[0] ^ 0x01)};
  if (vk::wallet::verify(message, sizeof message, v.signature, v.key)) s.badAccepted = true;

  if (!s.good) c.finish(State::Fail, "test vector refused");
  else if (s.badAccepted) c.finish(State::Fail, "bad signature accepted");
  else c.finish(State::Ok, "ed25519 verify %lu ms", (unsigned long)s.ms);
}

// Clock: where the time came from, and the time (UTC). No source is "--": it depends on the
// network, not on the badge.
void checkClock(Ctx &c) {
  const vk::clock::Source source = vk::clock::source();
  if (source == vk::clock::Source::NONE) {
    c.finish(State::Skip, "not set");
    return;
  }
  const uint32_t t = vk::clock::now();
  c.finish(State::Ok, "%s %02u:%02u:%02u UTC", source == vk::clock::Source::SNTP ? "sntp" : "floor",
           (unsigned)((t / 3600) % 24), (unsigned)((t / 60) % 60), (unsigned)(t % 60));
}

// Wi-Fi: the mode and whether it is joined; the signal when it is. Off or not joined is "--".
// A scan is started only on a joined station, where it comes back to the network's channel by
// itself; it is asynchronous, and the number of networks is added when it ends.
struct WifiScratch { int rssi; };
void checkWifi(Ctx &c) {
  WifiScratch &s = c.scratch<WifiScratch>();
  if (c.step == 0) {
    switch (wifi_mgr::mode()) {
      case wifi_mgr::Mode::Off:
        c.finish(State::Skip, "off");
        return;
      case wifi_mgr::Mode::AccessPoint:
        c.finish(State::Ok, "hotspot ch %u", (unsigned)wifi_mgr::channel());
        return;
      default:
        break;
    }
    if (!wifi_mgr::connected()) {
      c.finish(State::Skip, "station, %s", wifi_mgr::statusText());
      return;
    }
    s.rssi = wifi_mgr::rssi();
    if (!wifi_mgr::startScan()) {
      c.finish(State::Ok, "%.14s %ddBm", wifi_mgr::ssid().c_str(), s.rssi);
      return;
    }
    sScanOurs = true;
    c.step = 1;
    c.stepAt = c.now;
    return;
  }
  const bool timedOut = c.now - c.stepAt >= WIFI_SCAN_TIMEOUT_MS;
  if (wifi_mgr::scanning() && !timedOut) return;
  if (wifi_mgr::scanning()) {
    c.finish(State::Ok, "%.14s %ddBm", wifi_mgr::ssid().c_str(), s.rssi);
  } else {
    c.finish(State::Ok, "%.14s %ddBm, %d nets", wifi_mgr::ssid().c_str(), s.rssi, wifi_mgr::scanResultCount());
    wifi_mgr::clearScan();
    sScanOurs = false;
  }
}

// ESP-NOW: on or off, the channel, how many badges were heard. Off is "--".
void checkEspnow(Ctx &c) {
  if (!espnow_mgr::enabled()) {
    c.finish(State::Skip, "off");
    return;
  }
  c.finish(State::Ok, "ch %u, %u peers", (unsigned)espnow_mgr::channel(), (unsigned)espnow_mgr::peerCount());
}

// Memory: free internal heap and free PSRAM (the canvas lives in PSRAM: a badge without it has
// no screen).
void checkMemory(Ctx &c) {
  const uint32_t heap = ESP.getFreeHeap();
  const uint32_t psram = ESP.getFreePsram();
  if (ESP.getPsramSize() == 0) c.finish(State::Fail, "heap %lu, no PSRAM", (unsigned long)heap);
  else if (heap < HEAP_MIN_BYTES) c.finish(State::Fail, "heap %lu low, psram %lu", (unsigned long)heap, (unsigned long)psram);
  else c.finish(State::Ok, "heap %lu psram %lu", (unsigned long)heap, (unsigned long)psram);
}

// ---- display test -----------------------------------------------------------------------------
// Pure RGB565 colours. They are typed: the graphics library reads a 16-bit value as RGB565.
constexpr uint16_t K_BLACK = 0x0000, K_WHITE = 0xFFFF, K_RED = 0xF800, K_GREEN = 0x07E0, K_BLUE = 0x001F;
constexpr uint16_t K_YELLOW = 0xFFE0, K_CYAN = 0x07FF, K_MAGENTA = 0xF81F;

enum : uint8_t { SCREEN_BARS, SCREEN_RED, SCREEN_GREEN, SCREEN_BLUE, SCREEN_WHITE, SCREEN_BLACK, SCREEN_CHECKER, SCREEN_COUNT };
const char *const DISPLAY_NAMES[SCREEN_COUNT] = {"COLOUR BARS", "RED", "GREEN", "BLUE", "WHITE", "BLACK", "CHECKERBOARD"};
constexpr int BARS_H = 110;              // colour bars y 0..109
constexpr int RAMP_H = 25;               // four ramps y 110..209: red, green, blue, grey
constexpr int CAPTION_H = 30;            // caption strip y 210..239
constexpr int CHECKER_PX = 8;

// The screen shown is c.step.
void displayKey(Ctx &c, uint8_t key) {
  if (key == BTN_B) {
    c.finish(State::Skip, "stopped");
  } else if (key == BTN_A) {
    if (c.step + 1 < SCREEN_COUNT) ++c.step;
    else c.finish(State::Ok, "seen");
  }
}

void displayDraw(Ctx &c) {
  LGFX_Sprite &canvas = display::canvas();
  const int w = display::width(), h = display::height();
  uint16_t ink = K_WHITE;                  // the caption's colour
  bool strip = false;                      // a black strip under the caption

  switch (c.step) {
    case SCREEN_BARS: {
      static const uint16_t BARS[8] = {K_WHITE, K_YELLOW, K_CYAN, K_GREEN, K_MAGENTA, K_RED, K_BLUE, K_BLACK};
      const int barW = w / 8;
      for (int i = 0; i < 8; ++i) canvas.fillRect(i * barW, 0, i == 7 ? w - 7 * barW : barW, BARS_H, BARS[i]);
      // Four ramps from black to full: red, green, blue, grey. A stuck or swapped data line shows
      // as a step or a tint.
      for (int x = 0; x < w; ++x) {
        const uint16_t r = (uint16_t)(x * 31 / (w - 1)), g = (uint16_t)(x * 63 / (w - 1)), b = r;
        canvas.drawFastVLine(x, BARS_H, RAMP_H, (uint16_t)(r << 11));
        canvas.drawFastVLine(x, BARS_H + RAMP_H, RAMP_H, (uint16_t)(g << 5));
        canvas.drawFastVLine(x, BARS_H + 2 * RAMP_H, RAMP_H, b);
        canvas.drawFastVLine(x, BARS_H + 3 * RAMP_H, RAMP_H, (uint16_t)((r << 11) | (g << 5) | b));
      }
      strip = true;
      break;
    }
    case SCREEN_RED:   canvas.fillScreen(K_RED); break;
    case SCREEN_GREEN: canvas.fillScreen(K_GREEN); ink = K_BLACK; break;
    case SCREEN_BLUE:  canvas.fillScreen(K_BLUE); break;
    case SCREEN_WHITE: canvas.fillScreen(K_WHITE); ink = K_BLACK; break;
    case SCREEN_BLACK: canvas.fillScreen(K_BLACK); break;
    default: {                             // SCREEN_CHECKER: every other 8 px cell
      canvas.fillScreen(K_WHITE);
      for (int y = 0, row = 0; y < h; y += CHECKER_PX, ++row) {
        for (int x = (row % 2) * CHECKER_PX; x < w; x += 2 * CHECKER_PX) canvas.fillRect(x, y, CHECKER_PX, CHECKER_PX, K_BLACK);
      }
      strip = true;
      break;
    }
  }

  if (strip) canvas.fillRect(0, h - CAPTION_H, w, CAPTION_H, K_BLACK);
  char left[40];
  snprintf(left, sizeof left, "%u/%u %s", (unsigned)c.step + 1u, (unsigned)SCREEN_COUNT, DISPLAY_NAMES[c.step]);
  const int y = h - CAPTION_H + (CAPTION_H - 8) / 2;
  display::text(left, ui::X0, y, ink);
  display::textRight(c.step + 1 < SCREEN_COUNT ? "SELECT next  CANCEL stop" : "SELECT seen  CANCEL stop", ui::X1, y, ink);
  display::touch();
}

const ManualOps DISPLAY_TEST = {nullptr, nullptr, displayDraw, displayKey, true};

// ---- button test ------------------------------------------------------------------------------
constexpr uint8_t KEY_ORDER[BUTTON_COUNT] = {BTN_UP, BTN_DOWN, BTN_LEFT, BTN_RIGHT, BTN_A, BTN_B};   // CANCEL last
const char *const KEY_LABELS[BUTTON_COUNT] = {"UP", "DOWN", "LEFT", "RIGHT", "SELECT", "CANCEL"};
constexpr uint32_t KEY_TIMEOUT_MS = 20000;
enum class KeyState : uint8_t { Waiting, Pressed, TimedOut, GivenUp };

struct ButtonScratch {
  KeyState state[BUTTON_COUNT];
  uint8_t index;                         // into KEY_ORDER: the key being asked for
  uint32_t at;                           // since when
  uint8_t wrong;                         // another key pressed instead, else BUTTON_COUNT
};

// The silkscreen name of a key code, for "got DOWN".
const char *keyLabel(uint8_t key) {
  for (uint8_t i = 0; i < BUTTON_COUNT; ++i) {
    if (KEY_ORDER[i] == key) return KEY_LABELS[i];
  }
  return "?";
}

void buttonsStart(Ctx &c) {
  ButtonScratch &s = c.scratch<ButtonScratch>();
  for (uint8_t i = 0; i < BUTTON_COUNT; ++i) s.state[i] = KeyState::Waiting;
  s.index = 0;
  s.at = c.now;
  s.wrong = BUTTON_COUNT;
}

// The key asked for was decided (pressed, timed out or given up): on to the next, or the result.
void nextKey(Ctx &c) {
  ButtonScratch &s = c.scratch<ButtonScratch>();
  ++s.index;
  s.at = c.now;
  s.wrong = BUTTON_COUNT;
  c.redraw = true;
  if (s.index < BUTTON_COUNT) return;

  unsigned good = 0;
  char missing[32] = "";
  for (uint8_t i = 0; i < BUTTON_COUNT; ++i) {
    if (s.state[i] == KeyState::Pressed) { ++good; continue; }
    strlcat(missing, " ", sizeof missing);
    strlcat(missing, KEY_LABELS[i], sizeof missing);
  }
  if (good == BUTTON_COUNT) c.finish(State::Ok, "%u/%u keys", good, (unsigned)BUTTON_COUNT);
  else c.finish(State::Fail, "%u/%u no%s", good, (unsigned)BUTTON_COUNT, missing);
}

void buttonsKey(Ctx &c, uint8_t key) {
  ButtonScratch &s = c.scratch<ButtonScratch>();
  if (s.index >= BUTTON_COUNT) return;
  if (key == KEY_ORDER[s.index]) {
    s.state[s.index] = KeyState::Pressed;
    nextKey(c);
  } else if (key == BTN_B) {
    // A short CANCEL while another key is asked for: that key is given up, so a dead key never
    // costs its whole 20 s. (CANCEL's own turn is the branch above.)
    s.state[s.index] = KeyState::GivenUp;
    nextKey(c);
  } else {
    s.wrong = key;                       // shown on the row; the 20 s keep running
  }
}

void buttonsUpdate(Ctx &c) {
  ButtonScratch &s = c.scratch<ButtonScratch>();
  if (s.index < BUTTON_COUNT && c.now - s.at >= KEY_TIMEOUT_MS) {
    s.state[s.index] = KeyState::TimedOut;
    nextKey(c);
  }
}

void buttonsDraw(Ctx &c) {
  ButtonScratch &s = c.scratch<ButtonScratch>();
  ui::frame("BUTTONS");
  const uint16_t good = th::color(th::STAMP_OK), bad = th::color(th::STAMP_BAD);
  for (uint8_t i = 0; i < BUTTON_COUNT; ++i) {
    const int y = ui::FIRST_ROW_Y + (int)i * ui::ROW_PITCH;
    if (i == s.index) {
      const uint32_t waited = c.now - s.at;
      const unsigned left = waited >= KEY_TIMEOUT_MS ? 0u : (unsigned)((KEY_TIMEOUT_MS - waited + 999) / 1000);
      char value[40];
      if (s.wrong < BUTTON_COUNT) snprintf(value, sizeof value, "got %s  press now %us", keyLabel(s.wrong), left);
      else snprintf(value, sizeof value, "press now %us", left);
      rc::row(ui::X0, ui::X1, y, KEY_LABELS[i], value, true);
      continue;
    }
    switch (s.state[i]) {
      case KeyState::Pressed:  rc::row(ui::X0, ui::X1, y, KEY_LABELS[i], "OK", false, good); break;
      case KeyState::TimedOut: rc::row(ui::X0, ui::X1, y, KEY_LABELS[i], "no press  FAIL", false, bad); break;
      case KeyState::GivenUp:  rc::row(ui::X0, ui::X1, y, KEY_LABELS[i], "given up  FAIL", false, bad); break;
      default:                 rc::row(ui::X0, ui::X1, y, KEY_LABELS[i], ""); break;
    }
  }
  const bool cancelTurn = s.index < BUTTON_COUNT && KEY_ORDER[s.index] == BTN_B;
  rc::footer(cancelTurn ? "Press CANCEL" : "CANCEL gives this key up", "hold CANCEL exit");
}

const ManualOps BUTTON_TEST = {buttonsStart, buttonsUpdate, buttonsDraw, buttonsKey, false};

// ---- LED test ---------------------------------------------------------------------------------
constexpr uint32_t LED_STEP_MS = 500;
constexpr uint8_t LED_COLOURS = 4;
const char *const LED_COLOUR_NAMES[LED_COLOURS] = {"red", "green", "blue", "white"};
constexpr uint8_t LED_RGB[LED_COLOURS][3] = {{255, 0, 0}, {0, 255, 0}, {0, 0, 255}, {255, 255, 255}};
// Every LED through every colour, one at a time, then one step with all of them off.
constexpr uint32_t LED_STEPS = (uint32_t)RGB_LED_COUNT * LED_COLOURS + 1;

struct LedScratch { uint32_t at; uint32_t shown; };

void ledsStart(Ctx &c) {
  LedScratch &s = c.scratch<LedScratch>();
  s.at = c.now;
  s.shown = LED_STEPS;                   // none shown yet: the first update sets step 0
  sLedsTouched = true;
}

void ledsKey(Ctx &c, uint8_t key) {
  if (key != BTN_A && key != BTN_B) return;
  leds::off();
  if (key == BTN_A) c.finish(State::Ok, "seen");
  else c.finish(State::Skip, "stopped");
}

// Each LED in turn through red, green, blue and white, then all off, again and again until a key.
// The strip is written only when the step changes: leds::show() is a blocking transfer.
void ledsUpdate(Ctx &c) {
  LedScratch &s = c.scratch<LedScratch>();
  const uint32_t step = ((c.now - s.at) / LED_STEP_MS) % LED_STEPS;
  if (step == s.shown) return;
  s.shown = step;
  leds::stopAnimation();                 // upstream's own animations would draw over the test
  leds::setAll(0, 0, 0);
  if (step + 1 < LED_STEPS) {
    const uint8_t *rgb = LED_RGB[step % LED_COLOURS];
    leds::set((uint8_t)(step / LED_COLOURS), rgb[0], rgb[1], rgb[2]);
  }
  leds::show();
  c.redraw = true;
}

void ledsDraw(Ctx &c) {
  LedScratch &s = c.scratch<LedScratch>();
  ui::frame("LEDS");
  char value[24];
  const bool lit = s.shown + 1 < LED_STEPS;
  if (lit) snprintf(value, sizeof value, "%u of %u", (unsigned)(s.shown / LED_COLOURS) + 1u, (unsigned)RGB_LED_COUNT);
  else snprintf(value, sizeof value, "all of %u", (unsigned)RGB_LED_COUNT);
  rc::row(ui::X0, ui::X1, ui::FIRST_ROW_Y, "LED", value);
  rc::row(ui::X0, ui::X1, ui::FIRST_ROW_Y + ui::ROW_PITCH, "COLOUR", lit ? LED_COLOUR_NAMES[s.shown % LED_COLOURS] : "off");
  display::textCentered("Each LED shows red, green, blue and white in turn.", display::width() / 2, ui::NOTE_Y, ui::sub());
  rc::footer("SELECT seen", "CANCEL stop");
}

const ManualOps LED_TEST = {ledsStart, ledsUpdate, ledsDraw, ledsKey, false};

// ---- the table --------------------------------------------------------------------------------

const Check CHECKS[] = {
    {"display", "DISPLAY", Kind::Manual, Profile::Any, nullptr, &DISPLAY_TEST, 0},
    {"buttons", "BUTTONS", Kind::Manual, Profile::Any, nullptr, &BUTTON_TEST, 0},
    {"leds", "LEDS", Kind::Manual, Profile::Any, nullptr, &LED_TEST, 0},
    {"battery", "BATTERY", Kind::Auto, Profile::Any, checkBattery, nullptr, 0},
    {"mic", "MICROPHONE", Kind::Auto, Profile::Any, checkMic, nullptr, 0},
    {"i2c", "I2C BUS", Kind::Auto, Profile::Any, checkI2c, nullptr, 0},
    {"storage", "STORAGE", Kind::Auto, Profile::Any, checkStorage, nullptr, 0},
    {"nvs", "SETTINGS", Kind::Auto, Profile::Any, checkNvs, nullptr, 0},
    {"key", "KEY", Kind::Auto, Profile::Any, checkKey, nullptr, 0},
    {"crypto", "CRYPTO", Kind::Auto, Profile::Any, checkCrypto, nullptr, 0},
    {"clock", "CLOCK", Kind::Auto, Profile::Any, checkClock, nullptr, 0},
    {"wifi", "WI-FI", Kind::Auto, Profile::Any, checkWifi, nullptr, WIFI_SCAN_TIMEOUT_MS + 4000},
    {"espnow", "ESP-NOW", Kind::Auto, Profile::Any, checkEspnow, nullptr, 0},
    {"memory", "MEMORY", Kind::Auto, Profile::Any, checkMemory, nullptr, 0},
};

void stopHardware() {
  if (sMicOurs) mic::disable();
  if (sLedsTouched) leds::off();
  if (sScanOurs) wifi_mgr::clearScan();
  sMicOurs = sScanOurs = sLedsTouched = false;
}

}  // namespace

const Suite HARDWARE = {
    "hardware", "HARDWARE", Profile::Any, CHECKS, sizeof CHECKS / sizeof CHECKS[0],
    nullptr, nullptr, stopHardware, true,
};

}  // namespace selftest
