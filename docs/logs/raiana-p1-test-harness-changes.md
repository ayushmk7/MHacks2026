# P1-R test harness: status and changes (Raiana)

State of the P1-R test harness as of 2026-10-04: what is committed, what is not, what has been tested and how, and what is left. Spec: [P1-r-test-harness.md](../specs/P1-r-test-harness.md). How to run each item: [harness/README.md](../../harness/README.md).

Everything below up to "Last commit" was committed on top of `main` at `a824373` (U's backend PR). The commit before that, `55535b3`, is described under [Last commit](#last-commit-55535b3).

## Status

| Item | Done when (spec §7) | Status |
| --- | --- | --- |
| R0 environment | Python 3.11 venv with the spec's packages | **Done** (committed) |
| R1 SE050 survey | Table for all 4 badges; pubkeys to U | Tool done and validated on hardware. **1 of 4 surveyed** (`5mG1mVHD`). `ChPPKh4s`, now slot 1 in `badges.json` and R3's payer, is not surveyed yet |
| R2 network probe | Listener, devnet and clock green from a badge on the hotspot | Run on badge-51A0: listener and devnet green, clock amber. The SNTP that turns the clock green is in the unflashed firmware |
| R3 signing | Badge-signed HACK transfer on devnet; link in `harness/RESULTS.md` | Both halves built and tested without a badge. Needs U's devnet setup and a badge flashed with a wallet (stand-in or A's) |
| R4 presence timing | p50/p95/max and loss for both Wi-Fi states; deadline in 00 §4 | Built and tested in the host rig. Needs two badges flashed with the `temp_firmware` build |
| R5 verifiers | JS and Python verify all vectors and reject tampered copies | **Done**: 58/58 vectors in both. Wiring into backend routes is U's |
| R6 fuzz set | Every fuzz input refused without a crash | Built. Stand-in decoders pass all 196 decoder cases on the host; a full 220-case rig run reads PASS. Needs a flashed badge |
| R7 multi-hop FRAG, R8 routing vectors | Added to the spec in `047c362` | Not started |

Nothing has been flashed since the I2C hardening: the badges still run the firmware from `55535b3`.

## Last commit: `55535b3`

"Harness R0/R1, badge 5mG1mVHD in badges.json, I2C debug docs", 2026-10-03 21:03 (146 files, +45,839 / −2).

| Path | What |
| --- | --- |
| `temp_firmware/solana-os-settings-only/` (138 files) | A stripped Solana OS fork that boots into Settings, with the I2C hardening in `src/hal/badge_i2c.cpp` ([below](#firmware)), `tools/badge-push.py`, `tools/fetch-broker-ca.ts`, and its own `.gitignore` for `build/` |
| `harness/requirements.txt`, `harness/.gitignore` | R0 environment; ignores `.venv/`, `__pycache__/`, `results/` |
| `harness/se050_survey.py`, `harness/README.md` | R1 survey script and its docs |
| `dashboard/server/config/badges.json` | Slot 1 = `5mG1mVHD…` (replaced the stand-in `Gn2GQYmc…`) |
| `docs/logs/2026-10-03-session-changes.md`, `docs/logs/BADGE-BUTTONS-I2C.md`, `docs/README.md` | Session log (since replaced by this file), the I2C investigation, a docs index row |

## Uncommitted work

### Harness (`harness/`)

| Path | What |
| --- | --- |
| `apps/r2-probe/`, `apps/r3-sign/`, `apps/r4-presence/`, `apps/r6-fuzz/` | The four badge apps, each `app.ini` + `config.lua` + `main.lua` in the BadgeOS app style (below) |
| `badge_app.py` | Shared by the push helpers: stages an app, writes the laptop's values into `config.lua`, adds `lib/vk.lua`, pushes. `--out DIR` stages only |
| `r2_probe.py` | R2: `serve` (stand-in `/health`) and `push` |
| `r3_app.py` | R3: `push` with the listener and the paying badge's key |
| `r4_timing.py` | R4: `serve` (collector), `push`, `report` (tables and proposed deadlines), `--check` |
| `r6_app.py` | R6: `push` with the listener and the badge's key |
| `r6_fuzz.mjs`, `fuzz_cases.mjs`, `fuzz_cases.test.mjs`, `fuzz-vectors.json` | R6 laptop half, the 220 cases with their reference decoders, their test, and the exported set for A's firmware tests |
| `vk_verify.py`, `vectors.json`, `make_vectors.py` | R5 Python verifier, the 58 shared vectors, their generator |
| `README.md` | Rewritten for the current layout |
| `certs/*.pem` (3) | UMich MWireless CA chain (InCommon / USERTrust). Not used by any harness script; they appear to be for joining a badge to WPA2-Enterprise Wi-Fi, which goes with the enterprise fixes in `wifi_mgr` |

**Badge apps in the BadgeOS style.** The apps were rewritten to match the apps in `os/apps/` on the `badge-os` branch: `app.ini` with `permissions=` and `min_api=2`; every knob (laptop address, timeouts, run counts, `header = "BADGEOS"`) in `config.lua`; `require("vk")` for JSON and the receipt-style screens through `vk.ui.frame`; `badge.codec` for base64 instead of each app's own copy; `badge.log` lines with an app code (`[app] R2 ...` and so on). The push helpers copy `lib/vk.lua` into the app folder, as `os/scripts/push-apps.sh` does; it is read from `os/lib/vk.lua`, or from `origin/badge-os` with `git show` while `main` does not have it. What the apps do is unchanged (see [Testing](#testing-without-a-badge)).

**R6 moved out of `dashboard/`.** The R6 laptop half was in `dashboard/server/src/fuzz.js`, `fuzz.test.js` and `dashboard/scripts/r6-fuzz.mjs` with an `npm run r6` script. The spec gives R6 no location and nothing in the backend uses it, so it now lives in `harness/` and runs as `node --env-file-if-exists=dashboard/.env harness/r6_fuzz.mjs`. Its `@solana/kit` use (32-byte address encoding) was replaced with a small base58 helper, so it needs only Node built-ins plus the dashboard's config and `verify.js`, imported by path. Checked before and after the move: the exported set is identical case for case, `--list` output is byte-identical, and the tests pass.

### Dashboard (`dashboard/`)

What stays there is what the spec puts there:

| File | Change | Spec |
| --- | --- | --- |
| `server/src/verify.js`, `verify.test.js` (new) | R5 JS verifier and its test | R5: "`server/src/verify.js` (used by U's backend)" |
| `scripts/r3-sign.mjs` (new) | R3 laptop half; reuses the server's RPC and config | R3: "JS (in `server/`/`scripts/`)" |
| `package.json` | `npm run r3` | R3 |
| `server/config/badges.json` | Slot 1 pubkey `5mG1mVHD…` → `ChPPKh4s47BMK3RGVCRc53fsUuw2xgSXw8jefRGhzA39` (software key, `standIn: false`) | R1: hand pubkeys to U for `badges.json` |

`package.json` and `badges.json` are U's files. `GET /health` on the badge listener, which R2 needs (§6, 00 §8.1), came with U's backend PR (`afd7992`), so `http.js` is not changed here. After `badges.json` is committed, U restarts the server and runs `npm run devnet:setup` to fund the new key.

### Firmware (`temp_firmware/solana-os-settings-only/`)

| File | Change |
| --- | --- |
| `src/net/wifi_mgr.cpp`, `.h` | SNTP started on the first Wi-Fi join (`timeSynced()`); join-failure reasons logged and shown in Settings (`lastReason()`); EAP state always cleared before a PSK join; enterprise join through `STA.connect()`, so one failed login no longer breaks every retry |
| `src/lua_sdk/lib_system.cpp` | `badge.system.time_ok()` |
| `src/net/espnow_mgr.cpp` | Does not force the ESP-NOW channel while the station is still joining |
| `src/net/push_protocol.cpp`, `tools/badge-push.py` | `WIFI` reply includes `reason=`; new `LOGS [n]` command, so the log can be read over BLE when Wi-Fi is down (opening the serial port resets the badge) |
| `src/lua_sdk/lib_wallet.cpp` (new), `wallet_decode.cpp`/`.h` (new), `wallet_approval.h` (new) | Dev stand-in for A's wallet API: `pubkey`, `address`, `key_location`, `time_ok`, `sign_proof`, `verify_proof`, `sign_request`, `begin_solana`, `begin_bank`, `poll`. Real byte checks (P1-A §4.2, 00 §5, 00 §6); a firmware-drawn approval screen with a DEV BUILD banner (SELECT signs, CANCEL refuses, 45 s timeout) |
| `src/lua_sdk/lua_runtime.cpp` | While the approval screen is up it is drawn instead of the app's `on_draw` and takes the buttons; reset when the app stops |
| `src/lua_sdk/lib_codec.cpp` (new) | `badge.codec` with BadgeOS's names and rules, so the apps make the same calls on both firmwares |
| `src/lua_sdk/lua_bindings.cpp`, `.h` | Register `badge.wallet` and `badge.codec` |
| `src/config.h` | `VK_HACK_MINT` (default: the R6 TEST mint), `VK_HACK_DECIMALS`, `VK_DEV_ALLOW_UNVERIFIED` (1), `VK_APPROVAL_TIMEOUT_MS` |
| `tools/wallet_decode_test.cpp`, `tools/fuzz-vectors-flat.mjs` (new) | Host test of the decoders against `fuzz-vectors.json` |

The stand-in checks no registry record, REQ or presence, so every approval is unverified; it must never go on a judge badge. The `badge-os` branch has A's real wallet, which replaces the stand-in once the team moves to BadgeOS.

The build compiles (1.89 MB, Arduino-ESP32 3.3.12, FQBN `esp32:esp32:esp32s3:PSRAM=opi,FlashSize=16M,PartitionScheme=custom,CDCOnBoot=cdc`). Flash, from the folder, app partition only, then power-cycle (flashing is a warm reset):

```
esptool --chip esp32s3 -p /dev/cu.usbserial-10 -b 115200 write-flash 0x10000 build/solana-os-settings-only.ino.bin
```

Never `erase-flash`: it wipes NVS, including the identity key. 921600 baud is unreliable.

### Docs

`docs/logs/2026-10-03-session-changes.md` is deleted (replaced by this file); `docs/logs/BADGE-BUTTONS-I2C.md` links here.

## Firmware

Committed in `55535b3`, flashed on `5mG1mVHD`. All in `src/hal/badge_i2c.cpp`:

1. **I2C bring-up order matches the vendor test kit:** GT911 touch held in reset, Wire started (and `recover()` if a line is low), GT911 booted on the live bus with the Goodix timing and addressed once, put back in reset if the bus is held afterwards.
2. **`recover()` pulses the SE050 enable pin when SCL is held low.** In the stuck state it logs "did not free it", which points at a collapsed rail rather than the SE050 stretching the clock.
3. **No permanent give-up:** recovery keeps running every 8 s instead of stopping after 5 failures, because the buttons that would retry it sit on the same bus.

None of this fixes the stuck bus after a warm reset, which looks like hardware ([BADGE-BUTTONS-I2C.md](BADGE-BUTTONS-I2C.md)). Rule for every measurement: cold-boot (unplug USB, switch off, switch on, replug); avoid RST1 and anything that warm-resets.

## Testing without a badge

| What | How | Result |
| --- | --- | --- |
| Stand-in decoders | `tools/wallet_decode_test.cpp` against `fuzz-vectors.json`, ASan/UBSan | 196/196 cases (every fuzz input refused, every control accepted) |
| `badge.codec` | Built with the firmware's own Lua; compared with Python on 305 random inputs plus edge cases, ASan/UBSan | 1,848 checks pass |
| Firmware build | `arduino-cli compile` | Compiles, no warnings from the new files |
| R5 | `vk_verify.py --check`; `node --test dashboard/server/src/*.test.js` | 58/58; 66/66 |
| R6 set | `node --test harness/fuzz_cases.test.mjs` | 4/4 |
| R4 statistics | `r4_timing.py --check` | pass |
| R3 laptop half | Built against devnet; `simulateTransaction` with `sigVerify` | Accepts the stub-signed transaction (stops at `AccountNotFound`, the payer is unfunded); rejects a one-bit-tampered copy |
| Badge apps, old vs new | Host rig, below | Identical in every scenario |

**Host rig.** The apps were run before and after the BadgeOS-style rewrite, in a rig kept outside the repo. It uses the firmware's own Lua 5.4 build (with its curated standard library) and the firmware's real `badge.codec` and decoders. It generates the `badge` table from the function names each firmware registers, so a call to anything a firmware lacks fails. It has three profiles: the fork as flashed on the badges today, the fork with the stand-in, and BadgeOS with its permissions enforced. Signatures use the firmware's TweetNaCl (checked against Node), and HTTP goes to real laptop halves. Each scenario ran both versions through the same button presses and compared every request, wallet call and result:

| App | Scenarios | Result |
| --- | --- | --- |
| R2 | 7: normal, listener down, RPC error, Wi-Fi off, no clock, on the flashed fork and the stand-in | identical |
| R3 | 6: sign, cancel, no wallet, wrong badge key, BadgeOS hold-to-sign and cancel. A fake laptop with r3-sign's reply shapes verified the real Ed25519 signature | identical |
| R4 | two badges over a simulated ESP-NOW bus, 200 exchanges each way into the real `r4_timing.py serve`; Wi-Fi off; flashed fork; BadgeOS | identical apart from random run ids; same `report` tables |
| R6 | all 220 cases against the real `r6_fuzz.mjs` on each profile | identical case for case. Stand-in: 215 pass + 5 controls OK, **PASS**. Flashed fork: all skipped, NOT DONE. BadgeOS: see findings |

Nothing has run on a badge since the R2 run on badge-51A0.

## Findings for the team

1. **SE050 badges cannot sign payments yet (A).** `se050_apdu::signEd25519` refuses messages over 180 bytes (`MAX_SIGN_MESSAGE_BYTES`). A Solana transfer message is about 251 B and a bank payload is longer, so any badge whose key is in the SE050 fails at signing. Software-key badges are fine. Needs chained APDUs or a three-byte Lc.
2. **R6 on BadgeOS needs a decision (A + R).** BadgeOS's `begin_solana` does not refuse undecodable bytes; it opens a red "cannot read this payment" approval and `poll()` returns `undecodable` afterwards. R6 counts any opened screen as a failure, so on BadgeOS 147 `begin_solana` cases read FAIL (nothing can be signed, but each needs a CANCEL). Either R6 accepts "red screen then `undecodable`" as a refusal, or BadgeOS refuses at `begin`, as P1-A §4.4 describes.
3. **R4 cannot run on BadgeOS.** It has no `sign_proof` in Lua (presence runs inside the firmware via `wallet.challenge` / `wallet.presence`). R4's numbers have to come from the `temp_firmware` stand-in, or R4 is rebuilt around `challenge`/`presence` timing.
4. **BadgeOS has no `begin_bank`, `sign_request` or `sign_proof` in Lua.** R6 reports those cases as `missing`.
5. **BadgeOS `signtest` passes its timeout as the content type** (`http.get(url, cfg.http_timeout_ms)`; the binding is `get(url, [content_type], [timeout])`), so it always runs with the 5 s default. Harmless; worth a one-line fix by A.
6. **`sign_proof` coerced numbers to byte strings** in the stand-in (the number `12345678` was accepted as an 8-byte `req_id` and signed). Fixed; the R6 set has that case.

## R1 survey results

| Run | Badge | Pubkey | Key location | Log | Result |
| --- | --- | --- | --- | --- | --- |
| `harness/results/20261004T004215Z/` | 5mG1mVHD | `5mG1mVHDd9JHAvUzBfiVry7SEeXT5SYRaYmYJQSnaHY4` | software (screen and log agree) | stored key loaded, cold boot, I2C bus ok | fallback to NVS |
| `harness/results/20261004T005257Z/` | 5mG1mVHD | same | software | stored key loaded, `rst:0x1 (POWERON)`, buttons and SE050 ATR ok | fallback to NVS |

`harness/results/` is git-ignored, so this table is the record. The failing step for `5mG1mVHD` stays unknown: the key location was decided on its first boot, whose log no longer exists. It may be on the software key only because the I2C bus was down then; regenerating the identity would re-run the SE050 attempt but changes the badge's key and ID.

## Next

1. **Survey `ChPPKh4s`** (R1, no reflash): `harness/.venv/bin/python harness/se050_survey.py --port /dev/cu.usbserial-10 --badges 1`. If it is on the SE050, update its `keyLocation` in `badges.json`, and note finding 1 for R3.
2. **U:** review the `dashboard/` changes, commit `badges.json`, restart the server, `npm run devnet:setup`.
3. **Decide the firmware** for the remaining runs: the `temp_firmware` stand-in (everything here is ready for it) or BadgeOS from `badge-os` (A's real wallet; R4 not possible, R6 needs finding 2 settled).
4. **Flash** that firmware (app partition only, then power-cycle), then rerun R2 for the clock row, run R6, and run R3 once U's funding is in.
5. **R4** with two flashed badges, both Wi-Fi states, in the room; agree the deadline with A.
6. **R7 and R8** (new in the spec).
7. Survey the other badges at rollout.
8. Decide whether `temp_firmware/` keeps that name.

## Badge Wi-Fi notes

- **Settings → Wi-Fi → Start hotspot** makes the badge an access point: SSID = device name, IP `192.168.4.1`, password `solanabadge` (`DEFAULT_AP_PASSWORD`). No internet; use it to deliver the phone hotspot's credentials with `badge-push.py ... join` (steps in the [harness README](../../harness/README.md#r2-network-probe)).
- For real runs, badges and laptop join **one phone hotspot** on 2.4 GHz (iPhone: Maximize Compatibility), per 00 §1.
- Prefer reading logs over Wi-Fi (`badge-push.py logs`) or BLE (`LOGS`): opening the USB serial port warm-resets the badge and can wedge the I2C bus.
