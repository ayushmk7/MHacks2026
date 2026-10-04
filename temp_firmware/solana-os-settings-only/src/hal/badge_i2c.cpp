#include "badge_i2c.h"

#include <Wire.h>

#include "../badge_log.h"
#include "../config.h"

namespace badge_i2c {
namespace {

uint8_t sLastError = 0;
uint8_t sErrorStreak = 0;

// Three failures in a row is the point where retrying the same transfer stops
// being useful and the bus itself is the suspect.
constexpr uint8_t RECOVERY_THRESHOLD = 3;

// Recovery is rate-limited, and the delay grows each time it fails to help.
//
// Without this, an absent or wedged device is catastrophic rather than merely
// broken: buttons::update() polls every 15ms, so every third poll would fire a
// full recovery - about 22 per second, each tearing down and re-initialising
// the Wire peripheral and printing a log line. That storm buries every other
// log message and keeps the bus permanently in reset, which is exactly the
// state it is trying to escape.
constexpr uint32_t RECOVERY_MIN_INTERVAL_MS = 250;
constexpr uint32_t RECOVERY_MAX_INTERVAL_MS = 8000;

// After this many recoveries in a row that did not bring the bus back, down()
// reports the bus as dead so callers can back off instead of hammering it.
// Recovery still runs at RECOVERY_MAX_INTERVAL_MS, and any success clears it.
constexpr uint8_t RECOVERY_GIVE_UP_AFTER = 5;

uint32_t sNextRecoveryAt = 0;
uint32_t sRecoveryBackoffMs = RECOVERY_MIN_INTERVAL_MS;
uint8_t sFailedRecoveries = 0;
bool sBusDown = false;
// Log-once latch, so a persistent fault produces a handful of lines rather than
// one per attempt.
bool sReportedDown = false;

}  // namespace

namespace {

// (Re)starts the Wire peripheral only. Used by begin() and by the recovery
// paths, which must never repeat the one-time touch-controller sequence.
void startWire() {
  Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL, I2C_HZ);
  Wire.setTimeOut(I2C_TIMEOUT_MS);
}

constexpr uint8_t GT911_ADDR = 0x5D;  // latched by INT LOW on the RST rising edge

// Holds the GT911 touch controller in reset. Safe on a live bus: in reset its
// pins are high-impedance, which is how the test kit runs its own bus-idle check.
void holdTouchInReset() {
  pinMode(PIN_TOUCH_RST, OUTPUT);
  digitalWrite(PIN_TOUCH_RST, LOW);
  pinMode(PIN_TOUCH_INT, INPUT_PULLDOWN);
}

// Goodix power-on timing T2..T5, copied from testkit.ino resetGt911(). INT is
// driven across the RST rising edge (LOW selects 0x5D) and held low for the
// ~50 ms the controller spends booting.
//
// Runs with Wire already up, in the test kit's order: the test kit holds the
// GT911 in reset until the bus is live and pulled up, then boots it. (This was
// once suspected of clamping SCL; it is not - see docs/logs/BADGE-BUTTONS-I2C.md.)
void bootTouchController() {
  pinMode(PIN_TOUCH_RST, OUTPUT);
  pinMode(PIN_TOUCH_INT, OUTPUT);

  digitalWrite(PIN_TOUCH_INT, LOW);
  digitalWrite(PIN_TOUCH_RST, LOW);
  delay(11);                              // T2 > 10 ms

  digitalWrite(PIN_TOUCH_INT, LOW);       // address select: LOW -> 0x5D
  delayMicroseconds(110);                 // T3 > 100 us

  digitalWrite(PIN_TOUCH_RST, HIGH);      // address latched on this edge
  delay(6);                               // T4 > 5 ms

  digitalWrite(PIN_TOUCH_INT, LOW);       // held low across the firmware boot
  delay(51);                              // T5 ~ 50 ms

  pinMode(PIN_TOUCH_INT, INPUT_PULLDOWN); // vendor GPIO_Mode_IPD; INT idles low
  delay(100);                             // settle before the first transfer
}

// Does what the test kit's initGt911() does once the part is up: read the
// product ID, then clear the buffer-status register (0x814E) so the controller
// is not left with a stale frame pending. Touch is unused; this only brings
// the part to the same quiet state the test kit leaves it in.
bool settleTouchController() {
  Wire.beginTransmission(GT911_ADDR);
  Wire.write(0x81);
  Wire.write(0x40);
  if (Wire.endTransmission(false) != 0) return false;
  char id[4] = {0};
  if (Wire.requestFrom((int)GT911_ADDR, 3, true) != 3) return false;
  for (uint8_t i = 0; i < 3 && Wire.available(); ++i) id[i] = (char)Wire.read();
  while (Wire.available()) Wire.read();

  Wire.beginTransmission(GT911_ADDR);
  Wire.write(0x81);
  Wire.write(0x4E);
  Wire.write(0x00);
  Wire.endTransmission(true);

  badge_log::tagf("i2c", "GT911 @0x%02X id=%s", GT911_ADDR, id);
  return true;
}

}  // namespace

// Bring-up order follows testkit.ino setup()/bootI2cCheck() exactly:
//   1. touch controller held in reset
//   2. Wire up, lines checked, bus clocked free if a slave is holding it
//   3. touch controller booted on the live bus, then addressed once
// Runs once at boot. Recovery re-inits Wire through startWire() only.
void begin() {
  holdTouchInReset();
  startWire();
  delay(10);

  if (!linesHigh()) {
    badge_log::tagf("i2c", "bus not idle before touch boot (SDA=%s SCL=%s) - recovering",
                    digitalRead(PIN_I2C_SDA) ? "H" : "L", digitalRead(PIN_I2C_SCL) ? "H" : "L");
    recover();
  }

  bootTouchController();
  const bool touchOk = settleTouchController();

  if (!linesHigh()) {
    // The GT911 came out of boot holding the bus. Touch is unused, and a GT911
    // in reset is high-impedance, so put it back: the buttons and the secure
    // element matter, the touch panel does not.
    badge_log::tagf("i2c", "bus held after GT911 boot (SDA=%s SCL=%s) - holding GT911 in reset",
                    digitalRead(PIN_I2C_SDA) ? "H" : "L", digitalRead(PIN_I2C_SCL) ? "H" : "L");
    holdTouchInReset();
    delay(5);
    recover();
    badge_log::tagf("i2c", "after GT911 reset: SDA=%s SCL=%s",
                    digitalRead(PIN_I2C_SDA) ? "H" : "L", digitalRead(PIN_I2C_SCL) ? "H" : "L");
  } else if (!touchOk) {
    badge_log::tagf("i2c", "GT911 not answering at 0x%02X (touch unused - ignoring)", GT911_ADDR);
  }
}

const char *errorText(uint8_t code) {
  switch (code) {
    case 0: return "ok";
    case 1: return "data too long";
    case 2: return "NACK on address";
    case 3: return "NACK on data";
    case 4: return "bus error";
    case 5: return "timeout";
    default: return "unknown";
  }
}

const char *knownDeviceName(uint8_t address) {
  switch (address) {
    case TCA9534_ADDR: return "TCA9534 buttons";
    case SE050_ADDR:   return "SE050 secure element";
    case 0x5D:
    case 0x14:         return "GT911 touch (unused)";
    default:           return "unknown";
  }
}

bool linesHigh() {
  return digitalRead(PIN_I2C_SDA) == HIGH && digitalRead(PIN_I2C_SCL) == HIGH;
}

bool idle() {
  Wire.end();
  pinMode(PIN_I2C_SDA, INPUT_PULLUP);
  pinMode(PIN_I2C_SCL, INPUT_PULLUP);
  delayMicroseconds(10);
  const bool high = digitalRead(PIN_I2C_SDA) == HIGH && digitalRead(PIN_I2C_SCL) == HIGH;
  startWire();
  return high;
}

bool recover() {
  Wire.end();

  pinMode(PIN_I2C_SCL, OUTPUT_OPEN_DRAIN);
  pinMode(PIN_I2C_SDA, INPUT_PULLUP);
  digitalWrite(PIN_I2C_SCL, HIGH);
  delayMicroseconds(10);

  // A slave mid-byte releases SDA after at most nine more clocks.
  for (uint8_t i = 0; i < 9 && digitalRead(PIN_I2C_SDA) == LOW; ++i) {
    digitalWrite(PIN_I2C_SCL, LOW);
    delayMicroseconds(5);
    digitalWrite(PIN_I2C_SCL, HIGH);
    delayMicroseconds(5);
  }

  // Manual STOP: SDA low->high while SCL is high.
  pinMode(PIN_I2C_SDA, OUTPUT_OPEN_DRAIN);
  digitalWrite(PIN_I2C_SDA, LOW);
  delayMicroseconds(5);
  digitalWrite(PIN_I2C_SCL, HIGH);
  delayMicroseconds(5);
  digitalWrite(PIN_I2C_SDA, HIGH);
  delayMicroseconds(5);

  // Read the lines while the peripheral is still detached - this is the only
  // honest check of whether anything is still holding the bus down. Doing it
  // through idle() instead would re-init Wire a second time on every recovery.
  pinMode(PIN_I2C_SDA, INPUT_PULLUP);
  pinMode(PIN_I2C_SCL, INPUT_PULLUP);
  delayMicroseconds(10);
  bool released =
      digitalRead(PIN_I2C_SDA) == HIGH && digitalRead(PIN_I2C_SCL) == HIGH;

  // Clocking only frees a slave holding SDA. A slave stretching SCL ignores it,
  // so as a last resort reset the SE050 through its enable pin (its keys are
  // non-volatile). Note this does NOT clear the stuck-bus fault seen on warm
  // resets: there SCL reads LOW even when driven push-pull HIGH, i.e. it is
  // clamped by an unpowered part, which only a full power cycle fixes - see
  // docs/logs/BADGE-BUTTONS-I2C.md.
  if (!released && digitalRead(PIN_I2C_SCL) == LOW) {
    digitalWrite(PIN_SE050_ISO_RST, SE050_ENABLE_ACTIVE_HIGH ? LOW : HIGH);
    delay(20);
    const bool freedByReset = digitalRead(PIN_I2C_SCL) == HIGH;
    digitalWrite(PIN_SE050_ISO_RST, SE050_ENABLE_ACTIVE_HIGH ? HIGH : LOW);
    delay(10);  // let the SE050 boot before the bus is used again
    released = digitalRead(PIN_I2C_SDA) == HIGH && digitalRead(PIN_I2C_SCL) == HIGH;
    badge_log::tagf("i2c", "SCL held low - SE050 reset %s",
                    freedByReset ? (released ? "freed it" : "freed it, but it came back")
                                 : "did not free it");
  }

  startWire();
  sErrorStreak = 0;
  return released;
}

void noteFailure(uint8_t errorCode) {
  sLastError = errorCode;

  // A NACK (2 = address, 3 = data) means "nothing answered at THIS address". It
  // says nothing about the bus, and while the lines are high there is by
  // definition nothing to recover. Letting a NACK drive recovery means one
  // absent device - an unpopulated SE050, a GT911 nobody addresses - tears down
  // and re-inits Wire underneath the devices that *are* working, which is
  // exactly what the expander poll cannot survive. The test kit has always
  // drawn this line (see noteI2cFailure there); this file did not.
  if ((errorCode == 2 || errorCode == 3) && linesHigh()) return;

  if (sErrorStreak < 255) ++sErrorStreak;
  if (sErrorStreak < RECOVERY_THRESHOLD) return;

  // No permanent give-up: retry() lives behind Settings > Device info, which
  // needs the buttons this bus carries. Once down, keep trying at the slowest
  // backoff (the window check below enforces it).

  const uint32_t now = millis();
  if (sNextRecoveryAt != 0 && (int32_t)(now - sNextRecoveryAt) < 0) {
    // Too soon. Reset the streak so the next attempt is judged on fresh
    // failures rather than firing the instant the window opens.
    sErrorStreak = 0;
    return;
  }

  // Both lines already high means nothing is holding the bus - the device is
  // simply not answering. Recovery clocks out a stuck slave and cannot help
  // with that, so doing it would be a Wire teardown/re-init every quarter
  // second, forever, for a part that is not on the board. Read the lines
  // directly rather than through idle(), which would itself re-init Wire.
  if (linesHigh()) {
    sErrorStreak = 0;
    sNextRecoveryAt = now + RECOVERY_MAX_INTERVAL_MS;
    return;
  }

  sErrorStreak = 0;
  const bool released = recover();

  if (released) {
    sFailedRecoveries = 0;
    sRecoveryBackoffMs = RECOVERY_MIN_INTERVAL_MS;
    sNextRecoveryAt = now + RECOVERY_MIN_INTERVAL_MS;
    if (sReportedDown) {
      badge_log::tagf("i2c", "bus recovered");
      sReportedDown = false;
    }
    return;
  }

  if (!sReportedDown) {
    badge_log::tagf("i2c", "bus is held low (error %u: %s) - recovering", sLastError,
                    errorText(sLastError));
    sReportedDown = true;
  }

  ++sFailedRecoveries;
  sRecoveryBackoffMs = sRecoveryBackoffMs * 2;
  if (sRecoveryBackoffMs > RECOVERY_MAX_INTERVAL_MS) {
    sRecoveryBackoffMs = RECOVERY_MAX_INTERVAL_MS;
  }
  sNextRecoveryAt = now + sRecoveryBackoffMs;

  if (sFailedRecoveries >= RECOVERY_GIVE_UP_AFTER && !sBusDown) {
    sBusDown = true;
    badge_log::tagf("i2c", "bus did not come back after %u attempts - retrying every %lus",
                    (unsigned)sFailedRecoveries,
                    (unsigned long)(RECOVERY_MAX_INTERVAL_MS / 1000));
  }
}

void noteSuccess() {
  sErrorStreak = 0;
  sLastError = 0;
  sFailedRecoveries = 0;
  sRecoveryBackoffMs = RECOVERY_MIN_INTERVAL_MS;
  sNextRecoveryAt = 0;
  if (sBusDown || sReportedDown) {
    badge_log::tagf("i2c", "bus is back");
    sBusDown = false;
    sReportedDown = false;
  }
}

bool down() { return sBusDown; }

void retry() {
  sBusDown = false;
  sReportedDown = false;
  sFailedRecoveries = 0;
  sErrorStreak = 0;
  sRecoveryBackoffMs = RECOVERY_MIN_INTERVAL_MS;
  sNextRecoveryAt = 0;
  recover();
}

uint8_t lastError() { return sLastError; }

bool writeReg(uint8_t address, uint8_t reg, uint8_t value) {
  Wire.beginTransmission(address);
  Wire.write(reg);
  Wire.write(value);
  const uint8_t result = Wire.endTransmission(true);
  if (result != 0) {
    noteFailure(result);
    return false;
  }
  noteSuccess();
  return true;
}

bool readReg(uint8_t address, uint8_t reg, uint8_t *buffer, size_t length) {
  Wire.beginTransmission(address);
  Wire.write(reg);
  const uint8_t result = Wire.endTransmission(false);  // repeated START
  if (result != 0) {
    noteFailure(result);
    return false;
  }

  const size_t received = Wire.requestFrom((int)address, (int)length, true);
  size_t index = 0;
  while (Wire.available() && index < length) buffer[index++] = (uint8_t)Wire.read();
  while (Wire.available()) Wire.read();

  if (received != length || index != length) {
    // A short read with nothing at all returned is a NACK; a partial one means
    // the slave gave up mid-transfer.
    noteFailure(index == 0 ? 2 : 4);
    return false;
  }
  noteSuccess();
  return true;
}

void scan() {
  // The line states matter as much as the device list: "nothing answered" and
  // "nothing could answer, a line is clamped" look identical in a bare count
  // and have completely different causes. digitalRead works here without
  // detaching Wire - it reads the GPIO input register, which follows the pin
  // whoever is driving it.
  badge_log::tagf("i2c", "scanning bus... SDA(GPIO%d)=%s SCL(GPIO%d)=%s @%luHz",
                  PIN_I2C_SDA, digitalRead(PIN_I2C_SDA) ? "HIGH" : "LOW",
                  PIN_I2C_SCL, digitalRead(PIN_I2C_SCL) ? "HIGH" : "LOW",
                  (unsigned long)I2C_HZ);

  uint8_t found = 0;
  for (uint8_t address = 0x08; address < 0x78; ++address) {
    Wire.beginTransmission(address);
    if (Wire.endTransmission(true) == 0) {
      badge_log::tagf("i2c", "  0x%02X  %s", address, knownDeviceName(address));
      ++found;
    }
    delay(1);
  }
  badge_log::tagf("i2c", "%u device(s)", found);

  if (found == 0) {
    // An empty bus on a board that is known to have two devices on it is worth
    // one round of questions rather than a shrug. The timeout sweep is the
    // interesting one: Wire's timeout is passed to the IDF driver in ticks, so
    // a value below one tick period can round to zero and fail every transfer
    // instantly, which looks exactly like an absent device.
    static const uint16_t TIMEOUTS[] = {I2C_TIMEOUT_MS, 50, 200};
    for (uint8_t i = 0; i < 3; ++i) {
      Wire.setTimeOut(TIMEOUTS[i]);
      for (uint8_t j = 0; j < 2; ++j) {
        const uint8_t address = j ? SE050_ADDR : TCA9534_ADDR;
        Wire.beginTransmission(address);
        const uint8_t error = Wire.endTransmission(true);
        badge_log::tagf("i2c", "  probe 0x%02X timeout=%ums -> %u (%s)", address,
                        (unsigned)TIMEOUTS[i], error, errorText(error));
      }
    }
    Wire.setTimeOut(I2C_TIMEOUT_MS);
  }
}

}  // namespace badge_i2c
