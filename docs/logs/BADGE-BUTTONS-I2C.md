# Badge buttons dead: I2C clock line (SCL) clamped low after a warm reset

> **Updated 2026-10-03 (later the same session).** The first version of this log blamed the power switch alone ("USB plugged in, switch off"). New evidence shows the more common trigger is a **warm reset** of an already-powered badge. The original explanation is kept, marked as superseded, under [History](#history-superseded-power-switch-only-explanation). The root cause is **still not proven**; see [Working hypothesis](#working-hypothesis-unverified).

## Buttons dead? Checklist

1. **Full power cycle.** Unplug USB, set the power switch to off, wait a few seconds, switch on, then plug USB back in. Nothing short of removing all power clears it: not RST1, not a serial-tool reset, not toggling the switch with USB still plugged in.
2. **Avoid warm resets when you need the buttons.** Pressing RST1, or anything that resets the ESP32 over serial (esptool, `se050_survey.py --port`, the Arduino serial monitor toggling RTS), can put the badge back into the stuck state. The first boot after a full power cycle is the one to trust.
3. **Check serial** (115200 baud, see below). You want `SCL(GPIO9)=HIGH` in the bus scan, `[btn] TCA9534 @0x20 ready, input=0x3F`, and a `[se050] ATR ok` line.
4. **If SCL is still LOW with the vendor test kit too,** it is the board, not our firmware. Firmware cannot fix it; power-cycle.

## Symptom

On Solana OS, and on our fork at `temp_firmware/solana-os-settings-only/` (renamed from `firmware/` this session), none of the six buttons (UP/DOWN/LEFT/RIGHT/SELECT/CANCEL) did anything. The screen worked normally. The vendor test kit sometimes read the buttons fine, which made it look like a firmware bug.

Serial in the stuck state:

```
[btn] TCA9534 init ok                         <- sometimes; the bus was fine at this point
[i2c] scanning bus... SDA(GPIO10)=HIGH SCL(GPIO9)=LOW @100000Hz
[i2c] 0 device(s)
[i2c]   probe 0x20 timeout=8ms -> 5 (timeout)
[os] up ... btn=-- int=L                      <- heartbeat; btn=-- means the expander isn't answering
```

Note what is missing: no `[se050] ATR ok` line.

## Hardware

| Part | Bus address | Pins |
|---|---|---|
| Shared I2C bus | - | SDA=GPIO10, SCL=GPIO9, 100 kHz, 8 ms Wire timeout |
| TCA9534 expander (all 6 buttons) | 0x20 | button INT=GPIO4 |
| GT911 touch (not used by the OS) | firmware selects 0x5D; scan sees it at 0x14 | RST=GPIO6, INT=GPIO5 |
| SE050 secure element | 0x48 | enable=GPIO8 (active high) |
| Unidentified device | 0x4A | - |

Pin and address constants: `temp_firmware/solana-os-settings-only/src/config.h` lines 33-43 and 65-68.

Every button goes through the expander, so if the bus is down, no button can be read.

Test kit source: `/private/tmp/solana-defcon-Pf8K2z/firmware/testkit/testkit.ino` (temporary checkout; the binary is also at `app/static/firmware/testkit.bin` in that repo).

## Root cause (current understanding)

### What we observed

1. **First boot after a full power cycle is healthy.**

   ```
   [btn] TCA9534 @0x20 ready, input=0x3F      <- 0x3F = no key held
   [se050] ATR ok (35 bytes)
   [i2c] SCL(GPIO9)=HIGH ... 0x14 GT911, 0x20 TCA9534, 0x48 SE050, 0x4A unknown -> 4 device(s)
   ```

2. **A later warm reset breaks it** (ESP32 reset via RTS or RST1, power still applied). The log shows `[btn] TCA9534 init ok`, then `SCL(GPIO9)=LOW` at the bus scan, and **no `[se050] ATR ok`**. Reproduced twice.

3. **The bus dies between two specific log lines.** In `setup()` (`temp_firmware/solana-os-settings-only/solana-os-settings-only.ino`, "Peripherals" boot stage, lines 190-199) the only bus traffic between `TCA9534 init ok` (line 195) and `badge_i2c::scan()` (line 199) is:
   - `power::begin()` (line 197)
   - `se050::test()` (line 198): the SE050 T=1 soft reset `5A CF 00 37 7F`, a 15 ms wait, then a 35-byte ATR read (`src/hal/se050.cpp` lines 27-64).

4. **Once stuck, SCL is clamped hard to ground.** It survives all of these:
   - ESP32 resets
   - SE050 enable (GPIO8) held low for 500 ms
   - GT911 reset (GPIO6) held low for 500 ms
   - 64 SCL clock pulses
   - **SCL driven push-pull HIGH by the ESP32 still reads LOW.** This is the decisive one. A chip clock-stretching uses an open-drain pull-down that a push-pull driver would at least partly fight; reading solid LOW against a push-pull HIGH means something is sinking a lot of current.

5. **Only full power removal clears it.** Unplug USB, switch off, switch on, replug.

6. **The vendor test kit fails identically in the stuck state** (`I2C BUS LOW ... SCL(GPIO9)=LOW`, `TCA9534 btns: FAIL`, `batt=4.75V`) and works after a power cycle. So this is not specific to our firmware.

### Working hypothesis (unverified)

A peripheral power rail has collapsed or latched off, and an **unpowered I2C chip is clamping SCL to ground through its ESD protection diode**. This fits "push-pull HIGH still reads LOW" and "only full power removal clears it" far better than any chip holding the line through its logic.

The suspected trigger is the SE050 activity in `se050::test()` on a warm boot, possibly combined with how the badge is powered (USB only vs. battery), causing a brownout or tripping a load switch / LDO with latch-off on the rail that feeds the I2C chips.

### Next steps

- [ ] Measure the I2C devices' 3V3 rail with a multimeter while stuck. Near 0 V confirms the rail theory.
- [ ] Check the schematic for a load switch or LDO with latch-off / overcurrent protection on that rail.
- [ ] Test skipping `se050::test()` on warm boot (e.g. guard on `esp_reset_reason()`) and see whether the bus survives a warm reset.
- [ ] Compare running on battery vs. USB only.
- [ ] Try other badges to see if it is board-specific.

### Workaround

Full power cycle. Avoid warm resets (RST1, serial-tool resets) when you need the buttons.

## Ruled out

Each of these was checked with a code audit (four parallel audits) or an experiment:

- **Button driver** (`src/hal/buttons.cpp`) and the shell/Lua input consumers: correct.
- **Stale build:** no.
- **Pin conflicts on GPIO 4/9/10:** none.
- **`bootloader_random_enable()`:** does not touch GPIO9 (checked by disassembling it).
- **Our HAL changes:** the badge HAL code was byte-identical to upstream Solana OS.
- **GT911 holding SCL:** SCL stayed LOW with the GT911 held in reset (500 ms on GPIO6).
- **SE050 holding SCL by clock-stretching:** resetting it through its enable pin (GPIO8, 20 ms in firmware, 500 ms by hand) did not release SCL.

## Firmware changes made (hardening)

All in `temp_firmware/solana-os-settings-only/src/hal/badge_i2c.cpp`. Untracked (not committed), built and flashed on badge `5mG1mVHD`. They do **not** fix the stuck state, which looks like hardware; they make the firmware behave better on wedged-bus cases it *can* fix and keep retrying instead of giving up. Full detail: [raiana-p1-test-harness-changes.md](raiana-p1-test-harness-changes.md).

1. **Bring-up order matches the test kit** (`begin()`, lines 120-153): GT911 held in reset, Wire up, lines checked and `recover()` if not idle, GT911 booted on the live bus and addressed once (product ID read, 0x814E cleared), GT911 put back in reset if the bus is held afterwards. Recovery paths call `startWire()` (lines 48-51) only.
2. **`recover()` tries the SE050 enable when SCL is held** (lines 191-246, SE050 branch 231-241): pulses GPIO8 low for 20 ms and logs `SCL held low - SE050 reset freed it / freed it, but it came back / did not free it`. In the stuck state it logs **did not free it**, consistent with the rail hypothesis.
3. **No permanent give-up** (`noteFailure()`, lines 248-319): keeps retrying at the 8 s max backoff; the "did not come back after N attempts" line is logged once (lines 313-318).

Known stale comments in that file (code unchanged, comments only):
- Lines 29-32: `RECOVERY_GIVE_UP_AFTER` comment still describes the old give-up behaviour; it now only gates a one-time log line.
- Lines 225-230: claims the SE050 clock-stretches SCL after a cut-off transfer. The experiments above contradict that.
- Lines 67-72: attributes the stuck SCL to the GT911 booting before Wire was up. That was the pre-investigation theory; the GT911 was later ruled out.

## Diagnostics how-to

**Serial:** UART at 115200 on `/dev/cu.usbserial-*` (CH340C; set S1 to UART). Opening the port can reset the ESP32 (RTS), which is a warm reset; see the checklist.

| Log line | Meaning |
|---|---|
| `[btn] TCA9534 init ok` / `FAILED` | Expander found at boot, or not. `ok` here does **not** mean the bus stays up |
| `[btn] TCA9534 @0x20 ready, input=0x3F` | Healthy, no key held |
| `[se050] ATR ok (35 bytes)` | SE050 answered. **Missing on a boot where SCL ends up LOW** |
| `[i2c] scanning bus... SDA(GPIO10)=.. SCL(GPIO9)=..` | Line levels. **SCL=LOW = stuck state, power-cycle** |
| `[i2c] N device(s)` | Expect 4: 0x14, 0x20, 0x48, 0x4A |
| `[i2c] SCL held low - SE050 reset did not free it` | Firmware tried the SE050 enable; no effect (expected in the stuck state) |
| `[os] up ... btn=XX int=H/L` (every 30 s) | `btn=--` means the expander isn't answering |
| `[btn] P0 UP down (raw=0x..)` | Fires on each press, so the input path works |

**Flash the app only** (keeps settings), from `temp_firmware/solana-os-settings-only/`:

```
esptool --chip esp32s3 -p /dev/cu.usbserial-10 -b 115200 \
  write-flash 0x10000 build/solana-os-settings-only.ino.bin
```

Use 115200 baud. 921600 was unreliable on this cable. Remember to power-cycle after flashing (esptool resets the chip).

**Don't run `erase-flash`.** It wipes NVS, which holds the settings and the identity key.

## Note on badge identity (R1 survey)

Badge `5mG1mVHD`: the key is in NVS (`software`), and the log state is unknown ("stored software identity loaded"). Caveat: the identity may have been created during a boot when the bus was down. With the SE050 unreachable, the firmware falls back to the software/NVS key, and that choice sticks. If a badge should have an SE050-backed key but reports `software`, suspect this, and do a full power cycle before drawing conclusions or re-provisioning.

## History (superseded): "power switch only" explanation

The first version of this log (earlier on 2026-10-03) said:

> If the badge runs from USB with the power switch off, or the board gets into a bad power state, the ESP32 still boots and drives the display. The I2C peripherals are unpowered, though, and SCL (GPIO9) is held LOW.

**Why it is superseded:** it was partly right (a power problem, cleared only by a full power cycle, and the test kit fails the same way), but it pinned the trigger on the switch position. Later tests showed a badge with the switch on and a healthy first boot going into the stuck state after a **warm reset**, with the failure appearing during `se050::test()`. The power-switch case may still be one way in; it is not the only one. That version also listed "SE050 reset did not release SCL" and the mid-boot SCL drop as side observations; they are now central evidence.
