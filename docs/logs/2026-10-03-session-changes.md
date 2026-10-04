# Session changes: 2026-10-03

Everything changed in this work session (2026-10-03 to 2026-10-04). **Nothing is committed yet.** `harness/`, `temp_firmware/` and `docs/logs/` are untracked; `docs/README.md` and `dashboard/server/config/badges.json` are modified. Check `git status` before you pull or switch branches.

Related: [BADGE-BUTTONS-I2C.md](BADGE-BUTTONS-I2C.md) (the button / I2C investigation behind the firmware changes).

## Status at a glance

| Area | Path | Status |
|---|---|---|
| Firmware I2C hardening | `temp_firmware/solana-os-settings-only/src/hal/badge_i2c.cpp` | Untracked, built, flashed on `5mG1mVHD`. Does not fix the stuck bus |
| Harness R0 (env) | `harness/requirements.txt`, `harness/.venv/` | Done, imports verified |
| Harness R1 (survey script) | `harness/se050_survey.py`, `harness/README.md` | Script done, self-tested and validated on hardware (two cold-boot runs); data 1 of 4 badges |
| R1 handoff | `dashboard/server/config/badges.json` | Slot 1 "Merchant (team)" = badge `5mG1mVHD`, software key, `standIn: false`. Uncommitted; U must restart the server and run `npm run devnet:setup` |
| Debug docs | `docs/logs/` | New, untracked |
| Docs index | `docs/README.md` | Edited (row for `docs/logs/`), uncommitted |
| Ignore rules | `harness/.gitignore`; stash `stash@{0}` | `harness/.gitignore` ignores `.venv/`, `__pycache__/`, `results/`. The old root `.gitignore` edit in `stash@{0}` is obsolete and can be dropped |

## Firmware

**Folder:** `temp_firmware/solana-os-settings-only/` (untracked). A stripped fork of Solana OS that boots straight into Settings. The user renamed it from `firmware/` to `temp_firmware/` this session; older notes may still say `firmware/`.

All code changes are in **`src/hal/badge_i2c.cpp`**. Temporary diagnostics added while debugging have been removed.

### 1. I2C bring-up order matches the vendor test kit

- **Where:** `begin()`, lines 120-153; helpers `startWire()` 48-51, `holdTouchInReset()` 57-61, `bootTouchController()` 73-92, `settleTouchController()` 98-116.
- **What:** Boot sequence is now:
  1. GT911 touch held in reset (pins high-impedance).
  2. Wire started; if SDA/SCL aren't both high, `recover()`.
  3. GT911 booted on the live bus with the Goodix timing (INT low across the RST edge selects 0x5D), then addressed once: product ID read from 0x8140, buffer status 0x814E cleared.
  4. If the bus is held after the GT911 boots, the GT911 goes back into reset and `recover()` runs.
  Recovery paths re-init the bus through `startWire()` only and never repeat the GT911 sequence.
- **Why:** The test kit reads the buttons and our firmware didn't, so we made the bring-up identical to remove that variable.
- **Status:** Flashed, verified booting healthy after a full power cycle. It does not prevent the warm-reset stuck state.

### 2. `recover()` tries the SE050 enable pin when SCL is held low

- **Where:** `recover()`, lines 191-246; new branch lines 231-241.
- **What:** After the usual 9 clocks + manual STOP, if SCL (not just SDA) is still low, GPIO8 (SE050 enable) is pulsed low for 20 ms, then re-enabled with 10 ms to boot. Logs `[i2c] SCL held low - SE050 reset freed it` / `freed it, but it came back` / `did not free it`.
- **Why:** Clocking frees a slave holding SDA but not one holding SCL; an SE050 stuck mid-transfer was a suspect.
- **Status:** Flashed. In the stuck state it logs **did not free it**, which supports the "collapsed rail" hypothesis over "SE050 stretching the clock".

### 3. No permanent give-up on the bus

- **Where:** `noteFailure()`, lines 248-319 (comment 263-265; log-once 313-318).
- **What:** Previously, after 5 failed recoveries (`RECOVERY_GIVE_UP_AFTER`) the bus was abandoned until Settings > Device info > retry, which needs the buttons. Now recovery keeps running at the 8 s max backoff (`RECOVERY_MAX_INTERVAL_MS`). `down()` still reports true after 5 failures, and the "did not come back after N attempts - retrying every 8s" line is printed only once.
- **Why:** A badge with a dead bus could never get back without the buttons that bus carries.
- **Status:** Flashed.

### Stale comments left in `badge_i2c.cpp` (no code impact)

- Lines 29-32: describe the old give-up behaviour.
- Lines 67-72: blame the stuck SCL on the GT911 booting before Wire; GT911 was later ruled out.
- Lines 225-230: claim the SE050 clock-stretches SCL; experiments contradict this.

### Build and flash

- **Board:** badge `5mG1mVHD`, app partition only, current clean build.
- **FQBN:** `esp32:esp32:esp32s3:PSRAM=opi,FlashSize=16M,PartitionScheme=custom,CDCOnBoot=cdc`, Arduino-ESP32 core 3.3.12.
- **Flash** (from `temp_firmware/solana-os-settings-only/`):

  ```
  esptool --chip esp32s3 -p /dev/cu.usbserial-10 -b 115200 write-flash 0x10000 build/solana-os-settings-only.ino.bin
  ```

  921600 baud is unreliable. **Never `erase-flash`**: it wipes NVS (settings and the identity key). Flashing resets the chip, which is a warm reset; power-cycle afterwards if you need the buttons.

## Harness (R0 + R1)

Spec: [`docs/specs/P1-r-test-harness.md`](../specs/P1-r-test-harness.md). Usage: [`harness/README.md`](../../harness/README.md).

### R0: Python environment (done)

- Homebrew `python@3.11` installed.
- `harness/.venv/` created with Python 3.11.17.
- `harness/requirements.txt` pins: `pynacl==1.6.2`, `solders==0.29.0`, `solana==0.41.0`, `requests==2.34.2` (the spec's set) plus `pyserial==3.5` (for `--port` capture).
- All imports verified in the venv.

Setup for a teammate:

```
brew install python@3.11
python3.11 -m venv harness/.venv
harness/.venv/bin/pip install -r harness/requirements.txt
```

### R1: SE050 survey script (rewritten)

**File:** `harness/se050_survey.py` (370 lines). **Docs:** `harness/README.md` (51 lines).

| Feature | Where (line) | Notes |
|---|---|---|
| Full pubkey collected and validated | `valid_pubkey()` 145; prompt 257-263 | 43-44 chars, base58 decoding to exactly 32 bytes. Badge ID = first 8 chars (spec 4.2); cross-checked against the badge ID in the log |
| Screen key location in the server's vocabulary | `SCREEN_LOCATIONS` 30 | `secure element` / `software` / `unknown` → `se050` / `software` / `unknown`, as `dashboard/server/config/badges.json` expects |
| Script parses the serial log | `assess_log()` 64-126 | Maps firmware `[id]` / `[se050]` lines to the spec's failure steps (`STAGE_TO_STEP` 55-61) |
| Honest `probe_passed` | 117-119 | Only when the key was **created on the SE050 this boot**. A stored-key load is `unknown` (later boots run no probe) |
| I2C-down logs flagged | 73-76, 253-255, 271-272 | Result becomes `unknown/incomplete (I2C bus down during log)`; redo after a power cycle |
| Live capture | `capture_boot_log()`, `--port` | Waits for the user to power-cycle the badge (unplug USB, switch off/on, replug), opens the port as it reappears, and captures 15 s of the **cold** boot. Never resets the badge: a warm reset can wedge the I2C bus |
| `--parse LOGFILE` | 344-347 | Assess one saved log |
| `--check` | `self_check()` 309-336 | Parser and key-validation self-tests |
| Badge count | `main()`, `--badges N` | Asks how many badges are being surveyed (or `--badges N`); no longer assumes 4. Ctrl+C saves finished badges. The spec's R1 table still needs all 4, across runs |
| Partial runs saved | 357-366 | `try/finally` writes whatever was finished |
| Outputs per run | `write_results()` 289-306 | `harness/results/<UTC>/results.csv`, `table.md`, `pubkeys-for-U.json` (+ `badgeN-boot.log` with `--port`) |

**Verified:**
- `--check`: all self-tests pass.
- `--parse` on a real boot log of `5mG1mVHD`: key location `software`, log state `unknown` ("stored software identity loaded; original fallback reason not in this log").
- `--port` capture: the original warm-reset version was verified to capture and parse; it was then changed to cold-boot capture (not yet run on hardware) because the warm reset itself triggers the stuck bus.

**R1 data so far:** 1 of 4 badges. `harness/results/20261003T210247Z/results.csv` has `5mG1mVHD` → `nvs` / `unknown` / "fallback to NVS". That run used the **previous** script version: old column set, `nvs` instead of `software`, and **no pubkey**. It needs redoing with the new script.

**Caveat:** if a badge's identity was first created while the I2C bus was down, the SE050 was unreachable and the badge was forced onto the software key permanently. A `software` result is not proof the SE050 is broken.

### P1-R "Done when" status

Spec text in `docs/specs/P1-r-test-harness.md` is unchanged. Status against its section 3 (R0) and section 7 "Done when" list:

| Item | Spec requirement | Status |
|---|---|---|
| R0 | `harness/` with Python 3.11 venv (`pynacl`, `solders`, `solana`, `requests`) | **Done.** Pinned in `requirements.txt`, imports verified |
| R1 | R1 table filled for all 4 badges; pubkeys handed to U | **In progress.** Script done and tested. 1 of 4 badges surveyed (old script, no pubkey). No pubkeys handed to U for `dashboard/server/config/badges.json` yet |
| R2 | Green for badge listener, devnet and clock from a badge on the hotspot | Not started |
| R3 | Badge-signed HACK transfer on devnet; explorer link in `harness/RESULTS.md` | Not started |
| R4 | p50/p95/max and packet loss for both Wi-Fi states; deadline agreed | Not started |
| R5 | JS and Python verifiers pass all vectors and reject tampered copies | Not started |
| R6 | Every fuzz input refused without crash or watchdog reset | Not started |

## Repo housekeeping

- **Pulled 4 upstream commits** (up to `f2f60a3 Organize project documentation`): `dashboard/` restructure, `docs/os/`, `docs/specs/`.
- **`.gitignore`:** the local edit adding `harness/results/` conflicted with upstream moving `.gitignore` into `dashboard/`. It is parked in **`stash@{0}`** ("local .gitignore: ignore harness/results"). There is **no root `.gitignore`** now (only `dashboard/.gitignore`). Don't `git stash pop` blindly: it would recreate a root `.gitignore` with the old dashboard entries.
- **Untracked:** `harness/`, `temp_firmware/`, `docs/logs/`. **Modified:** `docs/README.md`, `dashboard/server/config/badges.json`. Nothing committed. `temp_firmware/solana-os-settings-only/build/` (136 MB) is ignored by that folder's own `.gitignore`.
- **Docs:** `docs/logs/BADGE-BUTTONS-I2C.md` updated with the corrected root cause; this file added; `docs/README.md` gained a row for `docs/logs/`.

## Open issues

1. **Stuck I2C bus after warm reset (unresolved, likely hardware).** Hypothesis: a peripheral rail collapses or latches off during `se050::test()` on a warm boot, and an unpowered chip clamps SCL. Next steps (multimeter on the 3V3 rail, schematic, skip `se050::test()` on warm boot, battery vs USB, other badges) are in [BADGE-BUTTONS-I2C.md](BADGE-BUTTONS-I2C.md#next-steps).
2. **Fixed: `se050_survey.py --port` no longer warm-resets the badge.** It waits for a user power cycle and captures the cold boot. Verified on hardware: runs `20261004T004215Z` and `20261004T005257Z` both captured `rst:0x1 (POWERON)` boots with the I2C bus up.
3. **R1 incomplete:** 3 badges left (rollout phase). `5mG1mVHD` is re-surveyed and in `badges.json`; U still has to restart the server and fund it. Note `harness/results/` is git-ignored, so the R1 table lives in this file (below), not in the run folders.
4. **`5mG1mVHD` may be on the software key only because of the bus fault.** Decide whether to re-provision once the bus issue is understood (re-provisioning means wiping the identity; never via `erase-flash` casually).
5. **Fixed: harness ignore rules.** `harness/.gitignore` now ignores `.venv/`, `__pycache__/` and `results/`, so `stash@{0}` is no longer needed and can be dropped.
6. **Fixed: stale comments** in `badge_i2c.cpp` and `harness/se050_survey.py` now point to the warm-reset / power-rail explanation.
7. **`temp_firmware/` is untracked and named "temp".** Decide whether it gets committed, moved back to `firmware/`, or replaced by upstream.
8. **R2-R6 not started.**

## R1 survey results so far

| Run | Badge | Pubkey | Key location | Log | Result |
| --- | --- | --- | --- | --- | --- |
| `harness/results/20261004T004215Z/` | 5mG1mVHD | `5mG1mVHDd9JHAvUzBfiVry7SEeXT5SYRaYmYJQSnaHY4` | software (screen and log agree) | unknown: stored key loaded, cold boot, I2C bus ok | fallback to NVS |
| `harness/results/20261004T005257Z/` | 5mG1mVHD | same | software (screen and log agree) | unknown: stored key loaded, `rst:0x1 (POWERON)`, buttons + SE050 ATR ok, I2C bus ok | fallback to NVS |

The two runs agree, so the R1 tool is validated on one badge. The latest run is the reference.

**Handoff done for 5mG1mVHD:** `dashboard/server/config/badges.json` slot 1 "Merchant (team)" now holds `5mG1mVHDd9JHAvUzBfiVry7SEeXT5SYRaYmYJQSnaHY4`, `keyLocation: "software"`, `standIn: false` (replacing stand-in `Gn2GQYmc…`). U still has to restart the server and run `npm run devnet:setup` to fund it. Run `20261003T210247Z` was made with the earlier script (no pubkey) and is superseded.

The failing step for 5mG1mVHD stays `unknown`: the key location was decided on the badge's first boot and that log no longer exists. Later boots only load the stored key. Regenerating the identity (Settings > Identity) would re-run the SE050 attempt and log the step, but it changes the badge's public key and ID. Deferred; decide before U uses the key.

## Plan: validate on one badge, then roll out

Only one badge (5mG1mVHD) is in hand. The harness is developed and proven end-to-end on it first; the other three badges are brought in only once it works.

1. **Single-badge phase (now).** Prove each harness item that can run on one badge against 5mG1mVHD, with the firmware in `temp_firmware/solana-os-settings-only/`.
2. **Rollout phase (later).** Flash the same validated firmware to the other three badges (app-only at `0x10000`, never `erase-flash`, power-cycle after flashing). Then run the R1 survey on each and the multi-badge items.

Rules for both phases: cold-boot (unplug USB, switch off, switch on, replug) before any measurement, and avoid RST1 / warm resets (see [BADGE-BUTTONS-I2C.md](BADGE-BUTTONS-I2C.md)). A badge whose identity is created during rollout should be first booted on a healthy bus, and its survey log will then show `probe_passed` or the exact failing step.

| Item | One badge? | Status / blocker |
| --- | --- | --- |
| R0 environment | yes | Done |
| R1 survey | yes (tool) / no (4-badge table) | Tool validated on 5mG1mVHD; its pubkey is in `badges.json` slot 1; remaining 3 badges in rollout |
| R2 network probe (Lua) | yes | Not started. Needs U's badge listener `GET /health` (not in `dashboard/server/src/` yet) and the hotspot |
| R3 signing harness | yes | Not started. Laptop side (legacy `transferChecked`, queue on `/badge/pending`, verify, send) can be built now; the final run needs A's `wallet.begin_solana` / `wallet.poll` and U's funded badge ATA, which needs this badge's pubkey in `badges.json` |
| R4 presence timing | **no, needs 2 badges** | Not started. Lua app can be written and self-tested, but no measurement until rollout |
| R5 verifier library | yes (laptop only) | Not started. No badge needed |
| R6 fuzz set | yes | Not started. Served through the R3 path; useful once A's decoder exists |

