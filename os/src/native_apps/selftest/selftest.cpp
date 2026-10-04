// src/native_apps/selftest/selftest.cpp
// Self test: one app that checks every part of the badge. The checklist is on the screen in the
// Receipt layout, and the same results go to the serial log, so that a person can read them and a
// script can (test/device/t_selftest.py).
//
// Checks. Each ends as OK, FAIL or -- (not tested: it needs a person, or the thing is switched off).
//   manual      display, buttons, leds        entered from the checklist with SELECT; can be skipped
//   automatic   battery, mic, i2c, storage, nvs, key, crypto, clock, wifi, espnow, memory
//               run by themselves when the app opens, one step per frame, so no frame is held long
//
// Keys.
//   checklist     UP/DOWN move, SELECT starts the manual test under the cursor, CANCEL exits the app
//   display test  SELECT steps through the test screens and confirms after the last, CANCEL stops
//   LED test      SELECT confirms the LEDs were seen, CANCEL stops
//   button test   each of the six keys is asked for in turn (CANCEL last) and only that key ticks
//                 its row. A key not pressed within 20 s is marked FAIL and the test moves on; a
//                 short CANCEL while another key is asked for gives that key up at once. Nobody can
//                 be stuck here: six CANCEL presses or two minutes end the test, and holding
//                 CANCEL for 1.5 s leaves the app from anywhere (the firmware's own force-quit).
//
// Serial log, each line "[selftest] ...":
//   start                                     once, when the app opens
//   <name>=<OK|FAIL|--> <value>               one per check, when it finishes (a manual check is
//                                             reported as "--" at the start, and again when a
//                                             person has done it)
//   i2c 0x20 answers (TCA9534 buttons)        detail lines of the I2C check
//   i2c 0x48 skipped (quarantined)
//   done ok=<n> fail=<n> manual=<n>           after the last automatic check, and again after each
//                                             manual test; manual counts every check that is "--"
//   batt=4.85V mic=-57dB btn=0x00 i2c=ok heap=182344       every 5 s while the app is open
//
// The secure element at I2C address 0x48 is quarantined (upstream-hooks.md, H21): this file never
// addresses it. The bus check probes four known addresses one at a time with badge_i2c::readReg
// and refuses 0x48 at compile time and at run time; badge_i2c::scan() and everything in se050* are
// not called.
//
// Nothing here signs. The crypto check only verifies a published test signature.
//
// The test screens of the display check are the one place where colours are not theme tokens: a
// display test has to show pure red, green, blue, white and black.
#include "../../vk/sdk/badge_sdk.hpp"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "../../apps/app_store.h"      // mounted(), usedBytes(), totalBytes()
#include "../../badge_log.h"
#include "../../hal/badge_i2c.h"
#include "../../hal/mic.h"
#include "../../hal/power.h"
#include "../../net/wifi_mgr.h"
#include "../../vk/core/fileio.h"
#include "../../vk/ui/receipt.h"
#include "../../vk/ui/theme.h"
#include "../../vk/wallet/crypto.h"    // verify() only

namespace {

namespace rc = vk::ui::receipt;
namespace th = vk::ui::theme;          // ours; upstream's palette is ::theme

constexpr char TAG[] = "selftest";

// ---- the checks -------------------------------------------------------------------------------------

enum Check : uint8_t {
  CHECK_DISPLAY, CHECK_BUTTONS, CHECK_LEDS,                             // manual
  CHECK_BATTERY, CHECK_MIC, CHECK_I2C, CHECK_STORAGE, CHECK_NVS, CHECK_KEY,
  CHECK_CRYPTO, CHECK_CLOCK, CHECK_WIFI, CHECK_ESPNOW, CHECK_MEMORY,    // automatic
  CHECK_COUNT
};

enum class State : uint8_t { Waiting, Running, Ok, Fail, Skip };   // Skip is shown as "--"

struct Spec {
  const char *name;      // in the serial log
  const char *label;     // on the screen
  bool manual;
};

constexpr Spec SPECS[CHECK_COUNT] = {
    {"display", "DISPLAY", true},   {"buttons", "BUTTONS", true},  {"leds", "LEDS", true},
    {"battery", "BATTERY", false},  {"mic", "MICROPHONE", false},  {"i2c", "I2C BUS", false},
    {"storage", "STORAGE", false},  {"nvs", "SETTINGS", false},    {"key", "KEY", false},
    {"crypto", "CRYPTO", false},    {"clock", "CLOCK", false},     {"wifi", "WI-FI", false},
    {"espnow", "ESP-NOW", false},   {"memory", "MEMORY", false},
};

struct Result {
  State state = State::Waiting;
  char value[40] = "";
};

const char *stateText(State state) {
  switch (state) {
    case State::Ok:   return "OK";
    case State::Fail: return "FAIL";
    case State::Skip: return "--";
    default:          return "..";
  }
}

// ---- layout (ui.md, "Screens": any list) ------------------------------------------------------------
constexpr int X0 = 10, X1 = 310;         // the page margins
constexpr int TITLE_Y = 26;
constexpr int FIRST_ROW_Y = 46;          // a row at y owns y-5 .. y+12
constexpr int ROW_PITCH = 18;
constexpr size_t VISIBLE_ROWS = 9;
constexpr int NOTE_Y = 112;              // a centred line of text under a short list

constexpr float PAUSE_GAP_S = 0.25f;     // a frame this late means an approval was up over the app
constexpr float REFRESH_S = 1.0f;        // the header's clock, a countdown
constexpr uint32_t BEAT_MS = 5000;       // the one-line summary in the log

// ---- limits of the automatic checks -----------------------------------------------------------------
constexpr float BATTERY_MIN_V = 2.5f;    // below this the divider is reading nothing
constexpr float BATTERY_MAX_V = 5.6f;    // above USB's 5 V and its tolerance: not a real reading
constexpr uint32_t MIC_LISTEN_MS = 2000;
constexpr uint32_t MIC_SETTLE_MS = 300;  // the driver's offset filter needs a moment after power-up
constexpr float MIC_NO_SAMPLES_DB = -98.0f;   // mic.h reports -99 until a block of samples was read
constexpr uint32_t HEAP_MIN_BYTES = 16 * 1024;
constexpr uint32_t WIFI_SCAN_TIMEOUT_MS = 8000;
constexpr char TEMP_FILE[] = "/vk/selftest.tmp";
constexpr char CONFIG_KEY[] = "hold_ms";      // a key the wallet core always registers

// ---- I2C addresses probed one at a time -------------------------------------------------------------
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

// ---- the Ed25519 test vector: RFC 8032, section 7.1, TEST 2 (a one-byte message) --------------------
const uint8_t VECTOR_KEY[32] = {
    0x3d, 0x40, 0x17, 0xc3, 0xe8, 0x43, 0x89, 0x5a, 0x92, 0xb7, 0x0a, 0xa7, 0x4d, 0x1b, 0x7e, 0xbc,
    0x9c, 0x98, 0x2c, 0xcf, 0x2e, 0xc4, 0x96, 0x8c, 0xc0, 0xcd, 0x55, 0xf1, 0x2a, 0xf4, 0x66, 0x0c,
};
const uint8_t VECTOR_MESSAGE[1] = {0x72};
const uint8_t VECTOR_SIGNATURE[64] = {
    0x92, 0xa0, 0x09, 0xa9, 0xf0, 0xd4, 0xca, 0xb8, 0x72, 0x0e, 0x82, 0x0b, 0x5f, 0x64, 0x25, 0x40,
    0xa2, 0xb2, 0x7b, 0x54, 0x16, 0x50, 0x3f, 0x8f, 0xb3, 0x76, 0x22, 0x23, 0xeb, 0xdb, 0x69, 0xda,
    0x08, 0x5a, 0xc1, 0xe4, 0x3e, 0x15, 0x99, 0x6e, 0x45, 0x8f, 0x36, 0x13, 0xd0, 0xf1, 0x1d, 0x8c,
    0x38, 0x7b, 0x2e, 0xae, 0xb4, 0x30, 0x2a, 0xee, 0xb0, 0x0d, 0x29, 0x16, 0x12, 0xbb, 0x0c, 0x00,
};

// ---- display test -----------------------------------------------------------------------------------
// Pure RGB565 colours. They are typed: the graphics library reads a 16-bit value as RGB565.
constexpr uint16_t K_BLACK = 0x0000, K_WHITE = 0xFFFF, K_RED = 0xF800, K_GREEN = 0x07E0, K_BLUE = 0x001F;
constexpr uint16_t K_YELLOW = 0xFFE0, K_CYAN = 0x07FF, K_MAGENTA = 0xF81F;

enum : uint8_t { SCREEN_BARS, SCREEN_RED, SCREEN_GREEN, SCREEN_BLUE, SCREEN_WHITE, SCREEN_BLACK, SCREEN_CHECKER, SCREEN_COUNT };
const char *const DISPLAY_NAMES[SCREEN_COUNT] = {"COLOUR BARS", "RED", "GREEN", "BLUE", "WHITE", "BLACK", "CHECKERBOARD"};
constexpr int BARS_H = 110;              // colour bars y 0..109
constexpr int RAMP_H = 25;               // four ramps y 110..209: red, green, blue, grey
constexpr int CAPTION_H = 30;            // caption strip y 210..239
constexpr int CHECKER_PX = 8;

// ---- button test ------------------------------------------------------------------------------------
constexpr uint8_t KEY_ORDER[BUTTON_COUNT] = {BTN_UP, BTN_DOWN, BTN_LEFT, BTN_RIGHT, BTN_A, BTN_B};   // CANCEL last
const char *const KEY_LABELS[BUTTON_COUNT] = {"UP", "DOWN", "LEFT", "RIGHT", "SELECT", "CANCEL"};
constexpr uint32_t KEY_TIMEOUT_MS = 20000;
enum class KeyState : uint8_t { Waiting, Pressed, TimedOut, GivenUp };

// The silkscreen name of a key code, for "got DOWN".
const char *keyLabel(uint8_t key) {
  for (uint8_t i = 0; i < BUTTON_COUNT; ++i) {
    if (KEY_ORDER[i] == key) return KEY_LABELS[i];
  }
  return "?";
}

// ---- LED test ---------------------------------------------------------------------------------------
constexpr uint32_t LED_STEP_MS = 500;
constexpr uint8_t LED_COLOURS = 4;
const char *const LED_COLOUR_NAMES[LED_COLOURS] = {"red", "green", "blue", "white"};
constexpr uint8_t LED_RGB[LED_COLOURS][3] = {{255, 0, 0}, {0, 255, 0}, {0, 0, 255}, {255, 255, 255}};
// Every LED through every colour, one at a time, then one step with all of them off.
constexpr uint32_t LED_STEPS = (uint32_t)RGB_LED_COUNT * LED_COLOURS + 1;

// ---- the app ----------------------------------------------------------------------------------------
class SelfTest final : public badge::App {
 public:
  void on_start() override {
    badge_log::tagf(TAG, "start");
    for (uint8_t check = 0; check < CHECK_COUNT; ++check) {
      if (SPECS[check].manual) finish((Check)check, State::Skip, "manual");
    }
    auto_ = firstAutomatic(0);
    beatAt_ = (uint32_t)millis();
  }

  void on_stop() override {
    if (micOurs_) mic::disable();
    if (ledsTouched_) leds::off();
    if (scanOurs_) wifi_mgr::clearScan();
  }

  void on_update(float dt) override {
    const uint32_t now = (uint32_t)millis();
    sinceDraw_ += dt;
    // While an approval is up the app is not run at all: the next frame comes late and finds the
    // approval's picture in the canvas.
    if (dt > PAUSE_GAP_S) dirty_ = true;

    runAutomatic(now);
    if (mode_ == Mode::ButtonTest) updateButtons(now);
    if (mode_ == Mode::LedTest) updateLeds(now);

    if ((uint32_t)(now - beatAt_) >= BEAT_MS) {
      beatAt_ = now;
      heartbeat();
    }
    // The header's clock and the button test's countdown move by themselves. A display test screen
    // has neither.
    if (mode_ != Mode::DisplayTest && sinceDraw_ >= REFRESH_S) dirty_ = true;
  }

  void on_draw() override {
    if (!dirty_) return;
    dirty_ = false;
    sinceDraw_ = 0.0f;
    switch (mode_) {
      case Mode::DisplayTest: drawDisplay(); break;
      case Mode::ButtonTest:  drawButtons((uint32_t)millis()); break;
      case Mode::LedTest:     drawLeds(); break;
      default:                drawList(); break;
    }
  }

  void on_button(uint8_t key, bool pressed) override {
    if (!pressed) return;
    dirty_ = true;
    const uint32_t now = (uint32_t)millis();
    switch (mode_) {
      case Mode::Checklist:   listKey(key, now); break;
      case Mode::DisplayTest: displayKey(key); break;
      case Mode::ButtonTest:  buttonsKey(key, now); break;
      case Mode::LedTest:     ledsKey(key); break;
    }
  }

 private:
  enum class Mode : uint8_t { Checklist, DisplayTest, ButtonTest, LedTest };

  // ---- results ----------------------------------------------------------------------------------

  // Stores the result, logs its line and asks for a repaint.
  // (`this` is argument 1 of a member function: the format is argument 4.)
  __attribute__((format(printf, 4, 5))) void finish(Check check, State state, const char *format, ...) {
    Result &result = results_[check];
    result.state = state;
    va_list args;
    va_start(args, format);
    vsnprintf(result.value, sizeof result.value, format, args);
    va_end(args);
    badge_log::tagf(TAG, "%s=%s %s", SPECS[check].name, stateText(state), result.value);
    dirty_ = true;
  }

  void count(unsigned &ok, unsigned &fail, unsigned &untested) const {
    ok = fail = untested = 0;
    for (uint8_t check = 0; check < CHECK_COUNT; ++check) {
      switch (results_[check].state) {
        case State::Ok:   ++ok; break;
        case State::Fail: ++fail; break;
        default:          ++untested; break;
      }
    }
  }

  void logDone() const {
    unsigned ok, fail, untested;
    count(ok, fail, untested);
    badge_log::tagf(TAG, "done ok=%u fail=%u manual=%u", ok, fail, untested);
  }

  // A manual test ended: its line is logged by finish(); the totals follow once the automatic run
  // has logged its own.
  void finishManual(Check check, State state, const char *value) {
    finish(check, state, "%s", value);
    if (auto_ >= CHECK_COUNT) logDone();
    mode_ = Mode::Checklist;
    // On to the next manual test, if the one below is one.
    if (state != State::Skip && selected_ == check && check + 1 < CHECK_COUNT && SPECS[check + 1].manual) {
      selected_ = (uint8_t)(check + 1);
    }
  }

  void heartbeat() const {
    char level[12];
    if (mic::enabled()) {
      const float left = mic::dbLeft(), right = mic::dbRight();
      snprintf(level, sizeof level, "%.0fdB", (double)(left > right ? left : right));
    } else {
      snprintf(level, sizeof level, "--");
    }
    badge_log::tagf(TAG, "batt=%.2fV mic=%s btn=0x%02X i2c=%s heap=%lu", (double)power::volts(), level,
                    (unsigned)buttons::downMask(), badge_i2c::linesHigh() ? "ok" : "LOW",
                    (unsigned long)ESP.getFreeHeap());
  }

  // ---- the automatic run --------------------------------------------------------------------------
  // One step of one check per frame. A check function returns true when it has called finish().

  static uint8_t firstAutomatic(uint8_t from) {
    while (from < CHECK_COUNT && SPECS[from].manual) ++from;
    return from;
  }

  void runAutomatic(uint32_t now) {
    if (auto_ >= CHECK_COUNT) return;
    if (results_[auto_].state == State::Waiting) {
      results_[auto_].state = State::Running;
      step_ = 0;
      stepAt_ = now;
    }
    bool finished = true;
    switch (auto_) {
      case CHECK_BATTERY: checkBattery(); break;
      case CHECK_MIC:     finished = checkMic(now); break;
      case CHECK_I2C:     finished = checkI2c(); break;
      case CHECK_STORAGE: checkStorage(); break;
      case CHECK_NVS:     checkNvs(); break;
      case CHECK_KEY:     checkKey(); break;
      case CHECK_CRYPTO:  finished = checkCrypto(); break;
      case CHECK_CLOCK:   checkClock(); break;
      case CHECK_WIFI:    finished = checkWifi(now); break;
      case CHECK_ESPNOW:  checkEspnow(); break;
      case CHECK_MEMORY:  checkMemory(); break;
      default:            finish((Check)auto_, State::Skip, "no check"); break;
    }
    if (!finished) return;
    auto_ = firstAutomatic((uint8_t)(auto_ + 1));
    if (auto_ >= CHECK_COUNT) logDone();
  }

  // Battery: the cell voltage, and the percentage upstream maps it to. On external power the ADC
  // reads the charger, so the figure is "USB", as in the header.
  void checkBattery() {
    const float volts = power::volts();
    const bool plausible = volts >= BATTERY_MIN_V && volts <= BATTERY_MAX_V;
    if (power::charging()) {
      finish(CHECK_BATTERY, plausible ? State::Ok : State::Fail, "%.2fV USB", (double)volts);
    } else {
      finish(CHECK_BATTERY, plausible ? State::Ok : State::Fail, "%.2fV %.0f%%", (double)volts, (double)power::percent());
    }
  }

  // Microphone: on, two seconds of levels (the main loop's mic::update() reads the samples), the
  // loudest block of each channel. It stays on for the log's summary line until the app stops.
  bool checkMic(uint32_t now) {
    if (step_ == 0) {
      if (!mic::enabled()) {
        if (!mic::enable()) {
          finish(CHECK_MIC, State::Fail, "did not start");
          return true;
        }
        micOurs_ = true;
      }
      micSeen_ = false;
      micPeakLeft_ = micPeakRight_ = MIC_NO_SAMPLES_DB;
      step_ = 1;
      stepAt_ = now;
      return false;
    }
    const uint32_t elapsed = (uint32_t)(now - stepAt_);
    if (elapsed >= MIC_SETTLE_MS) {
      const float left = mic::dbLeft(), right = mic::dbRight();
      if (left > MIC_NO_SAMPLES_DB || right > MIC_NO_SAMPLES_DB) micSeen_ = true;
      if (left > micPeakLeft_) micPeakLeft_ = left;
      if (right > micPeakRight_) micPeakRight_ = right;
    }
    if (elapsed < MIC_LISTEN_MS) return false;
    if (!micSeen_) {
      finish(CHECK_MIC, State::Fail, "no samples");
    } else {
      finish(CHECK_MIC, State::Ok, "peak L%.0f R%.0f dB", (double)micPeakLeft_, (double)micPeakRight_);
    }
    return true;
  }

  // I2C bus: both lines high, the button expander answering, and which of the known addresses
  // answer. One probe per frame. With a line held low nothing is probed: a transfer could not work
  // and would only feed upstream's bus recovery.
  bool checkI2c() {
    if (step_ == 0) {
      const bool sda = digitalRead(PIN_I2C_SDA) == HIGH, scl = digitalRead(PIN_I2C_SCL) == HIGH;
      badge_log::tagf(TAG, "i2c lines SDA=%s SCL=%s", sda ? "HIGH" : "LOW", scl ? "HIGH" : "LOW");
      i2cLinesHigh_ = sda && scl;
      i2cAnswered_ = 0;
      step_ = 1;
      if (i2cLinesHigh_) return false;
      step_ = (uint8_t)(1 + PROBE_COUNT);     // straight to the result
    }
    const uint8_t index = (uint8_t)(step_ - 1);
    if (index < PROBE_COUNT) {
      const uint8_t address = PROBES[index];
      if (probe(address)) {
        i2cAnswered_ |= (uint8_t)(1u << index);
        badge_log::tagf(TAG, "i2c 0x%02X answers (%s)", (unsigned)address, badge_i2c::knownDeviceName(address));
      } else {
        badge_log::tagf(TAG, "i2c 0x%02X no answer", (unsigned)address);
      }
      ++step_;
      return false;
    }

    badge_log::tagf(TAG, "i2c 0x%02X skipped (quarantined)", (unsigned)SE050_ADDR);
    if (!i2cLinesHigh_) {
      finish(CHECK_I2C, State::Fail, "a line is held low");
      return true;
    }
    char list[24] = "";
    for (size_t i = 0; i < PROBE_COUNT; ++i) {
      if ((i2cAnswered_ & (1u << i)) == 0) continue;
      char one[8];
      snprintf(one, sizeof one, "%s0x%02X", list[0] ? " " : "", (unsigned)PROBES[i]);
      strlcat(list, one, sizeof list);
    }
    if (buttons::present()) {
      finish(CHECK_I2C, State::Ok, "%s, 0x%02X skipped", list[0] ? list : "none", (unsigned)SE050_ADDR);
    } else {
      finish(CHECK_I2C, State::Fail, "buttons 0x%02X no answer", (unsigned)TCA9534_ADDR);
    }
    return true;
  }

  // Storage: the filesystem is mounted, has room, and a small file can be written, read back and
  // deleted through the one file layer.
  void checkStorage() {
    if (!app_store::mounted()) {
      finish(CHECK_STORAGE, State::Fail, "not mounted");
      return;
    }
    const size_t total = app_store::totalBytes(), used = app_store::usedBytes();
    const size_t freeKb = (total > used ? total - used : 0) / 1024;
    const vk::fileio::Ops *io = vk::fileio::ops;
    if (io == nullptr) {
      finish(CHECK_STORAGE, State::Fail, "no file layer");
      return;
    }

    uint8_t written[32], read[32];
    for (size_t i = 0; i < sizeof written; ++i) written[i] = (uint8_t)(0xA5u ^ (i * 7u));
    memset(read, 0, sizeof read);
    io->makeDir("/vk");                       // succeeds if it is already there
    const char *failed = nullptr;
    if (!io->writeAll(TEMP_FILE, written, sizeof written)) failed = "write failed";
    else if (io->size(TEMP_FILE) != (long)sizeof written) failed = "wrong size";
    else if (!io->read(TEMP_FILE, 0, read, sizeof read) || memcmp(read, written, sizeof written) != 0) failed = "read differs";
    // The file goes whether or not the steps before worked.
    const bool removed = io->removeFile(TEMP_FILE) && !io->exists(TEMP_FILE);
    if (failed == nullptr && !removed) failed = "delete failed";

    if (failed != nullptr) {
      finish(CHECK_STORAGE, State::Fail, "%s", failed);
    } else {
      finish(CHECK_STORAGE, State::Ok, "%luK free of %luK", (unsigned long)freeKb, (unsigned long)(total / 1024));
    }
  }

  // Settings store: one value read through vk::config. (The header has no way to ask whether the
  // NVS namespace opened; a store that did not open answers with the key's default.)
  void checkNvs() {
    if (vk::config::find(CONFIG_KEY) == nullptr) {
      finish(CHECK_NVS, State::Fail, "no key %s", CONFIG_KEY);
      return;
    }
    const uint32_t value = vk::config::u32(CONFIG_KEY);
    const char *provisioned = vk::config::provisioned() ? "provisioned" : "setup needed";
    if (value == 0) {
      finish(CHECK_NVS, State::Fail, "%s unreadable", CONFIG_KEY);
    } else {
      finish(CHECK_NVS, State::Ok, "%s %lu, %s", CONFIG_KEY, (unsigned long)value, provisioned);
    }
  }

  // Key: where it is kept and that the badge has an identity. Nothing is signed.
  void checkKey() {
    const char *location = vk::wallet::keyLocation();          // "se050" | "software" | "none"
    if (vk::wallet::publicKey() == nullptr) {
      finish(CHECK_KEY, State::Fail, "no identity (%s)", location);
      return;
    }
    const String address = vk::wallet::addressBase58();
    const size_t length = address.length();
    if (length >= 8) {
      finish(CHECK_KEY, State::Ok, "%s %.4s..%s", location, address.c_str(), address.c_str() + length - 4);
    } else {
      finish(CHECK_KEY, State::Ok, "%s", location);
    }
  }

  // Crypto: the published signature verifies, and neither a changed signature nor a changed message
  // does. One verification per frame.
  bool checkCrypto() {
    if (step_ == 0) {
      const uint32_t before = (uint32_t)micros();
      cryptoGood_ = vk::wallet::verify(VECTOR_MESSAGE, sizeof VECTOR_MESSAGE, VECTOR_SIGNATURE, VECTOR_KEY);
      cryptoMs_ = ((uint32_t)micros() - before + 500) / 1000;
      step_ = 1;
      return false;
    }
    if (step_ == 1) {
      uint8_t signature[64];
      memcpy(signature, VECTOR_SIGNATURE, sizeof signature);
      signature[0] ^= 0x01;
      cryptoBadAccepted_ = vk::wallet::verify(VECTOR_MESSAGE, sizeof VECTOR_MESSAGE, signature, VECTOR_KEY);
      step_ = 2;
      return false;
    }
    const uint8_t message[1] = {(uint8_t)(VECTOR_MESSAGE[0] ^ 0x01)};
    if (vk::wallet::verify(message, sizeof message, VECTOR_SIGNATURE, VECTOR_KEY)) cryptoBadAccepted_ = true;

    if (!cryptoGood_) finish(CHECK_CRYPTO, State::Fail, "test vector refused");
    else if (cryptoBadAccepted_) finish(CHECK_CRYPTO, State::Fail, "bad signature accepted");
    else finish(CHECK_CRYPTO, State::Ok, "ed25519 verify %lu ms", (unsigned long)cryptoMs_);
    return true;
  }

  // Clock: where the time came from, and the time (UTC). No source is "--": it depends on the
  // network, not on the badge.
  void checkClock() {
    const vk::clock::Source source = vk::clock::source();
    if (source == vk::clock::Source::NONE) {
      finish(CHECK_CLOCK, State::Skip, "not set");
      return;
    }
    const uint32_t t = vk::clock::now();
    finish(CHECK_CLOCK, State::Ok, "%s %02u:%02u:%02u UTC", source == vk::clock::Source::SNTP ? "sntp" : "floor",
           (unsigned)((t / 3600) % 24), (unsigned)((t / 60) % 60), (unsigned)(t % 60));
  }

  // Wi-Fi: the mode and whether it is joined; the signal when it is. Off or not joined is "--".
  // A scan is started only on a joined station, where it comes back to the network's channel by
  // itself; it is asynchronous, and the number of networks is added when it ends.
  bool checkWifi(uint32_t now) {
    if (step_ == 0) {
      switch (wifi_mgr::mode()) {
        case wifi_mgr::Mode::Off:
          finish(CHECK_WIFI, State::Skip, "off");
          return true;
        case wifi_mgr::Mode::AccessPoint:
          finish(CHECK_WIFI, State::Ok, "hotspot ch %u", (unsigned)wifi_mgr::channel());
          return true;
        default:
          break;
      }
      if (!wifi_mgr::connected()) {
        finish(CHECK_WIFI, State::Skip, "station, %s", wifi_mgr::statusText());
        return true;
      }
      wifiRssi_ = wifi_mgr::rssi();
      if (!wifi_mgr::startScan()) {
        finish(CHECK_WIFI, State::Ok, "%.14s %ddBm", wifi_mgr::ssid().c_str(), wifiRssi_);
        return true;
      }
      scanOurs_ = true;
      step_ = 1;
      stepAt_ = now;
      return false;
    }
    const bool timedOut = (uint32_t)(now - stepAt_) >= WIFI_SCAN_TIMEOUT_MS;
    if (wifi_mgr::scanning() && !timedOut) return false;
    if (wifi_mgr::scanning()) {
      finish(CHECK_WIFI, State::Ok, "%.14s %ddBm", wifi_mgr::ssid().c_str(), wifiRssi_);
    } else {
      finish(CHECK_WIFI, State::Ok, "%.14s %ddBm, %d nets", wifi_mgr::ssid().c_str(), wifiRssi_, wifi_mgr::scanResultCount());
      wifi_mgr::clearScan();
      scanOurs_ = false;
    }
    return true;
  }

  // ESP-NOW: on or off, the channel, how many badges were heard. Off is "--".
  void checkEspnow() {
    if (!espnow_mgr::enabled()) {
      finish(CHECK_ESPNOW, State::Skip, "off");
      return;
    }
    finish(CHECK_ESPNOW, State::Ok, "ch %u, %u peers", (unsigned)espnow_mgr::channel(), (unsigned)espnow_mgr::peerCount());
  }

  // Memory: free internal heap and free PSRAM (the canvas lives in PSRAM: a badge without it has
  // no screen).
  void checkMemory() {
    const uint32_t heap = ESP.getFreeHeap();
    const uint32_t psram = ESP.getFreePsram();
    if (ESP.getPsramSize() == 0) {
      finish(CHECK_MEMORY, State::Fail, "heap %lu, no PSRAM", (unsigned long)heap);
    } else if (heap < HEAP_MIN_BYTES) {
      finish(CHECK_MEMORY, State::Fail, "heap %lu low, psram %lu", (unsigned long)heap, (unsigned long)psram);
    } else {
      finish(CHECK_MEMORY, State::Ok, "heap %lu psram %lu", (unsigned long)heap, (unsigned long)psram);
    }
  }

  // ---- the checklist ------------------------------------------------------------------------------

  void listKey(uint8_t key, uint32_t now) {
    switch (key) {
      case BTN_B:                                  // CANCEL always leads out
        badge::exit();
        break;
      case BTN_UP:
        if (selected_ > 0) --selected_;
        break;
      case BTN_DOWN:
        if (selected_ + 1 < CHECK_COUNT) ++selected_;
        break;
      case BTN_A:
        if (selected_ == CHECK_DISPLAY) startDisplay();
        else if (selected_ == CHECK_BUTTONS) startButtons(now);
        else if (selected_ == CHECK_LEDS) startLeds(now);
        break;
      default:
        break;
    }
  }

  uint16_t stateColor(State state) const {
    switch (state) {
      case State::Ok:   return th::color(th::STAMP_OK);
      case State::Fail: return th::color(th::STAMP_BAD);
      case State::Skip: return th::color(th::SUB);
      default:          return th::color(th::FAINT);
    }
  }

  void drawHead(const char *title) const {
    char right[32];
    rc::statusRight(right, sizeof right);
    rc::page();
    rc::header("BADGEOS", right);
    rc::title(title, TITLE_Y);
  }

  void drawList() const {
    drawHead("SELF TEST");

    // The window of rows that holds the cursor, centred where it can be (as the Inbox does).
    size_t first = 0;
    if (CHECK_COUNT > VISIBLE_ROWS) {
      const size_t half = VISIBLE_ROWS / 2;
      first = selected_ > half ? selected_ - half : 0;
      if (first > CHECK_COUNT - VISIBLE_ROWS) first = CHECK_COUNT - VISIBLE_ROWS;
    }
    for (size_t slot = 0; slot < VISIBLE_ROWS && first + slot < CHECK_COUNT; ++slot) {
      const size_t index = first + slot;
      const Result &result = results_[index];
      char value[64];
      snprintf(value, sizeof value, "%s%s%s", result.value, result.value[0] ? "  " : "", stateText(result.state));
      const bool selected = index == selected_;
      // A selected row is inverted: its value stays in the paper colour, which is readable on ink.
      rc::row(X0, X1, FIRST_ROW_Y + (int)slot * ROW_PITCH, SPECS[index].label, value, selected,
              selected ? (uint16_t)0 : stateColor(result.state));
    }

    unsigned ok, fail, untested;
    count(ok, fail, untested);
    char left[64];
    snprintf(left, sizeof left, "%u OK \xC2\xB7 %u FAIL \xC2\xB7 %u --%s", ok, fail, untested,
             SPECS[selected_].manual ? "  SELECT test" : "");
    rc::footer(left, "CANCEL exit");
  }

  // ---- display test -------------------------------------------------------------------------------

  void startDisplay() {
    mode_ = Mode::DisplayTest;
    displayStep_ = 0;
  }

  void displayKey(uint8_t key) {
    if (key == BTN_B) {
      finishManual(CHECK_DISPLAY, State::Skip, "stopped");
    } else if (key == BTN_A) {
      if (displayStep_ + 1 < SCREEN_COUNT) ++displayStep_;
      else finishManual(CHECK_DISPLAY, State::Ok, "seen");
    }
  }

  void drawDisplay() const {
    LGFX_Sprite &c = display::canvas();
    const int w = display::width(), h = display::height();
    uint16_t ink = K_WHITE;                  // the caption's colour
    bool strip = false;                      // a black strip under the caption

    switch (displayStep_) {
      case SCREEN_BARS: {
        static const uint16_t BARS[8] = {K_WHITE, K_YELLOW, K_CYAN, K_GREEN, K_MAGENTA, K_RED, K_BLUE, K_BLACK};
        const int barW = w / 8;
        for (int i = 0; i < 8; ++i) c.fillRect(i * barW, 0, i == 7 ? w - 7 * barW : barW, BARS_H, BARS[i]);
        // Four ramps from black to full: red, green, blue, grey. A stuck or swapped data line
        // shows as a step or a tint.
        for (int x = 0; x < w; ++x) {
          const uint16_t r = (uint16_t)(x * 31 / (w - 1)), g = (uint16_t)(x * 63 / (w - 1)), b = r;
          c.drawFastVLine(x, BARS_H, RAMP_H, (uint16_t)(r << 11));
          c.drawFastVLine(x, BARS_H + RAMP_H, RAMP_H, (uint16_t)(g << 5));
          c.drawFastVLine(x, BARS_H + 2 * RAMP_H, RAMP_H, b);
          c.drawFastVLine(x, BARS_H + 3 * RAMP_H, RAMP_H, (uint16_t)((r << 11) | (g << 5) | b));
        }
        strip = true;
        break;
      }
      case SCREEN_RED:   c.fillScreen(K_RED); break;
      case SCREEN_GREEN: c.fillScreen(K_GREEN); ink = K_BLACK; break;
      case SCREEN_BLUE:  c.fillScreen(K_BLUE); break;
      case SCREEN_WHITE: c.fillScreen(K_WHITE); ink = K_BLACK; break;
      case SCREEN_BLACK: c.fillScreen(K_BLACK); break;
      default: {                             // SCREEN_CHECKER: every other 8 px cell
        c.fillScreen(K_WHITE);
        for (int y = 0, row = 0; y < h; y += CHECKER_PX, ++row) {
          for (int x = (row % 2) * CHECKER_PX; x < w; x += 2 * CHECKER_PX) c.fillRect(x, y, CHECKER_PX, CHECKER_PX, K_BLACK);
        }
        strip = true;
        break;
      }
    }

    if (strip) c.fillRect(0, h - CAPTION_H, w, CAPTION_H, K_BLACK);
    char left[40];
    snprintf(left, sizeof left, "%u/%u %s", (unsigned)displayStep_ + 1u, (unsigned)SCREEN_COUNT, DISPLAY_NAMES[displayStep_]);
    const int y = h - CAPTION_H + (CAPTION_H - 8) / 2;
    display::text(left, X0, y, ink);
    display::textRight(displayStep_ + 1 < SCREEN_COUNT ? "SELECT next  CANCEL stop" : "SELECT seen  CANCEL stop", X1, y, ink);
    display::touch();
  }

  // ---- button test --------------------------------------------------------------------------------

  void startButtons(uint32_t now) {
    mode_ = Mode::ButtonTest;
    for (uint8_t i = 0; i < BUTTON_COUNT; ++i) keyState_[i] = KeyState::Waiting;
    keyIndex_ = 0;
    keyAt_ = now;
    wrongKey_ = BUTTON_COUNT;                // none
  }

  // The key asked for was decided (pressed, timed out or given up): on to the next, or the result.
  void nextKey(uint32_t now) {
    ++keyIndex_;
    keyAt_ = now;
    wrongKey_ = BUTTON_COUNT;
    dirty_ = true;
    if (keyIndex_ < BUTTON_COUNT) return;

    unsigned good = 0;
    char missing[32] = "";
    for (uint8_t i = 0; i < BUTTON_COUNT; ++i) {
      if (keyState_[i] == KeyState::Pressed) { ++good; continue; }
      strlcat(missing, " ", sizeof missing);
      strlcat(missing, KEY_LABELS[i], sizeof missing);
    }
    char value[40];
    if (good == BUTTON_COUNT) {
      snprintf(value, sizeof value, "%u/%u keys", good, (unsigned)BUTTON_COUNT);
      finishManual(CHECK_BUTTONS, State::Ok, value);
    } else {
      snprintf(value, sizeof value, "%u/%u no%s", good, (unsigned)BUTTON_COUNT, missing);
      finishManual(CHECK_BUTTONS, State::Fail, value);
    }
  }

  void buttonsKey(uint8_t key, uint32_t now) {
    if (keyIndex_ >= BUTTON_COUNT) return;
    if (key == KEY_ORDER[keyIndex_]) {
      keyState_[keyIndex_] = KeyState::Pressed;
      nextKey(now);
    } else if (key == BTN_B) {
      // A short CANCEL while another key is asked for: that key is given up, so a dead key never
      // costs its whole 20 s. (CANCEL's own turn is the branch above.)
      keyState_[keyIndex_] = KeyState::GivenUp;
      nextKey(now);
    } else {
      wrongKey_ = key;                        // shown on the row; the 20 s keep running
    }
  }

  void updateButtons(uint32_t now) {
    if (keyIndex_ < BUTTON_COUNT && (uint32_t)(now - keyAt_) >= KEY_TIMEOUT_MS) {
      keyState_[keyIndex_] = KeyState::TimedOut;
      nextKey(now);
    }
  }

  void drawButtons(uint32_t now) const {
    drawHead("BUTTONS");
    const uint16_t good = th::color(th::STAMP_OK), bad = th::color(th::STAMP_BAD);
    for (uint8_t i = 0; i < BUTTON_COUNT; ++i) {
      const int y = FIRST_ROW_Y + (int)i * ROW_PITCH;
      if (i == keyIndex_) {
        const uint32_t waited = (uint32_t)(now - keyAt_);
        const unsigned left = waited >= KEY_TIMEOUT_MS ? 0u : (unsigned)((KEY_TIMEOUT_MS - waited + 999) / 1000);
        char value[40];
        if (wrongKey_ < BUTTON_COUNT) snprintf(value, sizeof value, "got %s  press now %us", keyLabel(wrongKey_), left);
        else snprintf(value, sizeof value, "press now %us", left);
        rc::row(X0, X1, y, KEY_LABELS[i], value, true);
        continue;
      }
      switch (keyState_[i]) {
        case KeyState::Pressed:  rc::row(X0, X1, y, KEY_LABELS[i], "OK", false, good); break;
        case KeyState::TimedOut: rc::row(X0, X1, y, KEY_LABELS[i], "no press  FAIL", false, bad); break;
        case KeyState::GivenUp:  rc::row(X0, X1, y, KEY_LABELS[i], "given up  FAIL", false, bad); break;
        default:                 rc::row(X0, X1, y, KEY_LABELS[i], ""); break;
      }
    }
    const bool cancelTurn = keyIndex_ < BUTTON_COUNT && KEY_ORDER[keyIndex_] == BTN_B;
    rc::footer(cancelTurn ? "Press CANCEL" : "CANCEL gives this key up", "hold CANCEL exit");
  }

  // ---- LED test -----------------------------------------------------------------------------------

  void startLeds(uint32_t now) {
    mode_ = Mode::LedTest;
    ledAt_ = now;
    ledStep_ = LED_STEPS;                    // none shown yet: the first update sets step 0
    ledsTouched_ = true;
  }

  void ledsKey(uint8_t key) {
    if (key != BTN_A && key != BTN_B) return;
    leds::off();
    if (key == BTN_A) finishManual(CHECK_LEDS, State::Ok, "seen");
    else finishManual(CHECK_LEDS, State::Skip, "stopped");
  }

  // Each LED in turn through red, green, blue and white, then all off, again and again until a key.
  // The strip is written only when the step changes: leds::show() is a blocking transfer.
  void updateLeds(uint32_t now) {
    const uint32_t step = ((uint32_t)(now - ledAt_) / LED_STEP_MS) % LED_STEPS;
    if (step == ledStep_) return;
    ledStep_ = step;
    leds::stopAnimation();                   // upstream's own animations would draw over the test
    leds::setAll(0, 0, 0);
    if (step + 1 < LED_STEPS) {
      const uint8_t *rgb = LED_RGB[step % LED_COLOURS];
      leds::set((uint8_t)(step / LED_COLOURS), rgb[0], rgb[1], rgb[2]);
    }
    leds::show();
    dirty_ = true;
  }

  void drawLeds() const {
    drawHead("LEDS");
    char value[24];
    const bool lit = ledStep_ + 1 < LED_STEPS;
    if (lit) snprintf(value, sizeof value, "%u of %u", (unsigned)(ledStep_ / LED_COLOURS) + 1u, (unsigned)RGB_LED_COUNT);
    else snprintf(value, sizeof value, "all of %u", (unsigned)RGB_LED_COUNT);
    rc::row(X0, X1, FIRST_ROW_Y, "LED", value);
    rc::row(X0, X1, FIRST_ROW_Y + ROW_PITCH, "COLOUR", lit ? LED_COLOUR_NAMES[ledStep_ % LED_COLOURS] : "off");
    display::textCentered("Each LED shows red, green, blue and white in turn.", display::width() / 2, NOTE_Y,
                          th::color(th::SUB));
    rc::footer("SELECT seen", "CANCEL stop");
  }

  // ---- state ------------------------------------------------------------------------------------
  Result results_[CHECK_COUNT];
  Mode mode_ = Mode::Checklist;
  uint8_t selected_ = 0;                     // the checklist's cursor
  bool dirty_ = true;
  float sinceDraw_ = 0.0f;
  uint32_t beatAt_ = 0;

  // the automatic run
  uint8_t auto_ = CHECK_COUNT;                   // the check that is running; CHECK_COUNT when all are done
  uint8_t step_ = 0;                         // its step
  uint32_t stepAt_ = 0;                      // when that step began
  bool micOurs_ = false;                     // this app turned the microphone on
  bool micSeen_ = false;
  float micPeakLeft_ = MIC_NO_SAMPLES_DB, micPeakRight_ = MIC_NO_SAMPLES_DB;
  bool i2cLinesHigh_ = false;
  uint8_t i2cAnswered_ = 0;                  // bit i: PROBES[i] answered
  bool cryptoGood_ = false, cryptoBadAccepted_ = false;
  uint32_t cryptoMs_ = 0;
  int wifiRssi_ = 0;
  bool scanOurs_ = false;                    // this app started a scan that has not been cleared

  // manual tests
  uint8_t displayStep_ = 0;
  KeyState keyState_[BUTTON_COUNT] = {};
  uint8_t keyIndex_ = 0;                     // into KEY_ORDER: the key being asked for
  uint32_t keyAt_ = 0;                       // since when
  uint8_t wrongKey_ = BUTTON_COUNT;          // another key that was pressed instead, else BUTTON_COUNT
  uint32_t ledAt_ = 0;
  uint32_t ledStep_ = LED_STEPS;
  bool ledsTouched_ = false;
};

}  // namespace

BADGE_APP(SelfTest, "selftest", "Self test", "1.0.0", "", "category=tests");
