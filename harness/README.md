# Harness

The P1-R test harness: the laptop and badge halves that prove the risky parts first, then serve as the verifier and attack console. Spec: [docs/specs/P1-r-test-harness.md](../docs/specs/P1-r-test-harness.md). Status and change log: [docs/logs/raiana-p1-test-harness-changes.md](../docs/logs/raiana-p1-test-harness-changes.md).

Plan: validate everything on one badge first, then flash the same firmware to the other three and run the multi-badge items.

## Where things are

| Item | Laptop | Badge |
| --- | --- | --- |
| R0 environment | `requirements.txt`, `.venv/` | |
| R1 SE050 survey | `se050_survey.py` | (serial boot log) |
| R2 network probe | `r2_probe.py` | `apps/r2-probe/` |
| R3 signing | `../dashboard/scripts/r3-sign.mjs` (`npm run r3`), `r3_app.py` | `apps/r3-sign/` |
| R4 presence timing | `r4_timing.py` | `apps/r4-presence/` |
| R5 verifiers | `../dashboard/server/src/verify.js`, `vk_verify.py`, `vectors.json`, `make_vectors.py` | |
| R6 fuzz set | `r6_fuzz.mjs`, `fuzz_cases.mjs`, `fuzz_cases.test.mjs`, `fuzz-vectors.json`, `r6_app.py` | `apps/r6-fuzz/` |
| Shared | `badge_app.py` (staging and pushing apps) | `lib/vk.lua` from BadgeOS, added at push time |

Only two harness pieces live in `dashboard/`, because the spec puts them there: R5's `server/src/verify.js` ("used by U's backend") and R3's `scripts/r3-sign.mjs` (it reuses the server's RPC and config). R2's `GET /health` on the server's badge listener (00 §8.1) is U's. Everything else is here.

## Badge apps

The four apps are laid out like the BadgeOS apps in `os/apps/` (branch `badge-os`; [docs/os/apps/apps.md](https://github.com/ayushmk7/MHacks2026/blob/badge-os/docs/os/apps/apps.md)):

- `app.ini` lists `permissions=` and `min_api=2`.
- `config.lua` returns a table with every knob: the laptop's address, timeouts, run counts, and `header = "BADGEOS"`. `main.lua` has no such literals.
- `main.lua` uses `require("vk")` for JSON and the receipt-style screens (`vk.ui`, drawn through `vk.ui.frame` so the screen is redrawn only when something changed) and `badge.codec` for base64.
- Log lines go through `badge.log` with an app code: `[app] R2 ...`, `R3`, `R4`, `R6`.
- CANCEL always goes back or exits.

**Pushing.** Each helper (`r2_probe.py`, `r3_app.py`, `r4_timing.py`, `r6_app.py`) stages a temporary copy of the app, writes this laptop's address (and, for R3/R6, the badge's pubkey) into its `config.lua`, adds `lib/vk.lua` as `vk.lua`, and installs it with the Solana OS push tool. `vk.lua` comes from `os/lib/vk.lua` when the checkout has it, otherwise from `git show origin/badge-os:os/lib/vk.lua` (run `git fetch` once). `push --out <dir>` stages the folder and pushes nothing, so you can look at exactly what would go to the badge.

```
python3 harness/r2_probe.py push --host <badge address> --token <code>      # Settings > App push shows both
python3 harness/r2_probe.py push --ble badge-51A0 --token <code> --listener <url>
python3 harness/r3_app.py push --out /tmp/stage --badge 1                     # stage only
```

**Which firmware runs what.** `vk.lua` draws with `badge.gfx` when the firmware has no receipt kit, so the same apps run on the `temp_firmware` fork and on BadgeOS:

| App | Firmware on the badges today | `temp_firmware` build with the wallet stand-in | BadgeOS |
| --- | --- | --- | --- |
| R2 | runs; clock row amber (no SNTP) | runs; clock from SNTP | runs |
| R3 | stops at "needs A6" (no wallet) | runs | runs (dev profile: hold to sign) |
| R4 | nothing to do (no `sign_proof`) | runs | cannot run: BadgeOS has no `sign_proof` in Lua |
| R6 | every case `missing` ("NOT DONE") | runs | runs, but see [R6 on BadgeOS](#r6-on-badgeos) |

## R0 setup

Python 3.11 (`brew install python@3.11` on macOS).

```
python3.11 -m venv harness/.venv
harness/.venv/bin/pip install -r harness/requirements.txt
```

`requirements.txt` pins `pynacl`, `solders`, `solana`, `requests` (the spec's attack-script set) plus `pyserial` for serial capture. The Node scripts need `npm install` in `dashboard/` once (R6 imports the dashboard's config and R5's `verify.js`, whose dependencies resolve from `dashboard/node_modules`).

## R1 SE050 survey

```
harness/.venv/bin/python harness/se050_survey.py --port /dev/cu.usbserial-10 [--badges N]
```

The script asks how many badges you are surveying (or `--badges N`). Survey whatever you have; the spec's table covers all 4, so run it again later for the rest. Ctrl+C saves the badges already finished.

For each badge:

1. Power switch ON. If the buttons or the I2C bus are dead: unplug USB, switch off, switch on, replug ([BADGE-BUTTONS-I2C.md](../docs/logs/BADGE-BUTTONS-I2C.md)).
2. With `--port`, press Enter, then **unplug USB, switch off, switch on, plug USB back in**. The script waits for the port and captures 15 s of that cold boot. It never resets the badge itself: a warm reset can wedge the I2C bus. If the port name changes after replugging, Ctrl+C and rerun with the new one (`ls /dev/cu.usbserial-*`). Without `--port`, give the path of a log you captured, or Enter for none.
3. The script parses the log: key location, whether the SE050 probe passed, the failing step, and whether the I2C bus was up. A log taken with the bus down is flagged and marked incomplete; power-cycle and redo that badge.
4. On the badge open **Settings → Identity**. Type the full public key (two lines, 43–44 base58 characters, starting with the badge ID), then what "key lives in" says: `secure element`, `software` or `unknown`.

Never enter a private key or seed phrase.

Each run writes `harness/results/<UTC timestamp>/`: `results.csv` (one row per badge), `table.md` (the R1 table), `pubkeys-for-U.json` (`pubkey` + `keyLocation` in the vocabulary `badges.json` takes) and `badgeN-boot.log`.

**How the log is judged.** The firmware creates the identity only on a badge's first boot; later boots load the stored key and run no probe.

- `probe_passed`: the log shows the key being created on the SE050 and the identity ending up there.
- `failed` with the step (`se050_presence_or_atr`, `key_generation`, `public_key_read`, `eddsa_sign`, `probe_signature_verification`, `identity_selection`, `other_documented_step`) when an `[id]`/`[se050]` failure line is present.
- `unknown`: only a stored key was loaded. Normal for a provisioned badge; it says where the key is but not why.
- `unavailable` / `unreadable`: no log, or the path could not be read.

`--parse LOGFILE` assesses one saved log; `--check` runs the self-tests.

## R2 network probe

From the badge, the three things every payment needs. Press SELECT; each row shows the result, latency and a detail.

| Check | How | Green when |
| --- | --- | --- |
| Badge listener | `GET <listener>/health` | HTTP 200 with `{"ok":true}` |
| Devnet RPC | `POST <rpc_url>` `getHealth` (default `https://api.devnet.solana.com`) | `{"result":"ok"}` |
| Clock / SNTP | `wallet.time_ok()`, else `badge.system.time_ok()` | true. Amber on a firmware with neither |

`GET /health` is on the dashboard server's badge listener (`dashboard/server/src/http.js`). Without the server, `python3 harness/r2_probe.py serve` stands in for it on port 8788.

1. Laptop and badge on the same phone hotspot (00 §1; iPhone: **Maximize Compatibility**, the badge is 2.4 GHz only). The badge has no text entry, so send it the password once: on the badge **Settings → Wi-Fi → Start hotspot**, join the laptop to that network (password `solanabadge`), run `python3 temp_firmware/solana-os-settings-only/tools/badge-push.py --host 192.168.4.1 --token <code> join <hotspot> --password '<password>'`, then move the laptop to the phone hotspot. The badge saves the network.
2. `python3 harness/r2_probe.py serve` (or the dashboard server with `BADGE_LISTEN_HOST` set). Allow incoming connections if macOS asks.
3. `python3 harness/r2_probe.py push --host <badge address> --token <code>`.
4. Press SELECT on the badge.

The badge's own hotspot has no internet, so the RPC row fails there; use it only to deliver credentials.

**Run on a badge (2026-10-04, earlier app version):** badge-51A0 on an iPhone hotspot: listener green (220 ms), devnet green (900 ms), clock amber (no SNTP in that firmware). `results/20261004T040916Z-r2/`. The `temp_firmware` build starts SNTP on the first Wi-Fi join, so the clock row should turn green once it is flashed.

## R3 signing harness

Proves a badge can sign a HACK payment that devnet accepts.

**Laptop half:** `dashboard/scripts/r3-sign.mjs`, run as `npm run r3` in `dashboard/`.

1. **Build** an unsigned **legacy** `transferChecked` (plus an optional zero-account memo). The badge is payer, fee payer, token owner and only signer. Source and destination are the existing HACK token accounts (creating one would add an instruction the decoder refuses).
2. **Queue** it on its own listener (port 8789): `GET /badge/pending?badge=<pubkey>` returns `{id, claimed, messageBase64, txBase64, txVersion, txBytes}`, or 204 for any other badge. It rebuilds every 45 s so the blockhash never expires. It does not use the server's `/badge/pending`, which serves attack-demo rows.
3. **Verify** the badge's signature with `verifySolanaMessage` (`verify.js`), attach it (`[0x01, sig, message]`), `sendTransaction`, wait for confirmation, print the explorer link and append it to `harness/RESULTS.md`. `POST /badge/outcome` (a refusal) is logged and the job stays queued; a stale id gets 409.

Options: `--badge <id>` (default 1), `--to <id|pubkey>` (default 2), `--amount` (default 1), `--memo`, `--port`, `--host`, `--mint` / `--decimals`, `--no-send` (verify only), `--stub-key <keypair.json>` (sign on the laptop instead of a badge; such rows are marked `stub` and do not count for R3).

**Badge half:** `apps/r3-sign/`. SELECT fetches the job, passes the raw message to `wallet.begin_solana(msg, {})` (the firmware takes the screen), polls `wallet.poll()` from `on_update`, and POSTs `{id, sig}` (base64) to `/badge/signature`. A refusal reason (`cancelled`, `unverified`, ...) goes to `/badge/outcome`. If the pushed `badge` key differs from `wallet.address()`, the app refuses: the signature could never verify.

The app passes no record or REQ in `ctx`, so it needs a dev build: `VK_DEV_ALLOW_UNVERIFIED` on the `temp_firmware` stand-in (set `VK_HACK_MINT` in its `config.h` to `HACK_MINT` first, or the real transfer is refused as `undecodable`), or BadgeOS's dev profile (red UNVERIFIED RECIPIENT, hold to sign).

```
cd dashboard && npm run r3 -- --badge 1 --to 2 --amount 1 --memo r3           # terminal 1
python3 harness/r3_app.py push --host <badge address> --token <code> --badge 1 # terminal 2
```

Press SELECT on the badge and approve. A 409 (rebuilt during a slow approval) asks for SELECT again.

**Needs:** `dashboard/.env` with `HACK_MINT` and the payer funded with SOL and HACK (`npm run devnet:setup`, U), and a badge with `begin_solana`.

**Tested (no badge):** the laptop half builds against devnet; a stub signature passes `verifySolanaMessage`; devnet `simulateTransaction` with `sigVerify` accepts it (stopping only at `AccountNotFound`, the payer is unfunded) and rejects a one-bit-tampered copy. The badge app ran in the host rig (see [Testing without a badge](#testing-without-a-badge)): signed, cancelled, no-wallet and wrong-badge paths on the stand-in and on BadgeOS, with a fake laptop verifying the real Ed25519 signature. **Not yet run:** a badge-signed send on devnet.

## R4 presence timing

The presence round trip between two badges: CHAL (60 B, 00 §5) → the payee signs with `wallet.sign_proof` → PROOF (76 B). The payer times it with `badge.millis()` from creating the nonce to receiving the PROOF, which is what `wallet.check_proof` will time.

- **Payee:** every badge answers every CHAL it receives with a signed PROOF and tracks its own signing time.
- **Hello:** each badge broadcasts a harness-only frame about once a second (`R4H1` + pubkey + key location + name; not a VK frame), so the payer learns the peer's MAC, key and key location.
- **Payer:** SELECT runs `config.runs` (200) exchanges against the strongest peer, `gap_ms` (50) apart. No PROOF after `lost_ms` (1000) counts as **lost**; a later one is also counted **late**, at its real time. Every signature is verified after its time is recorded; failures count as **bad sig**.
- **Percentiles cover every exchange sent:** late PROOFs at their real time, missing ones as `lost`, so a slow tail cannot hide in the loss count.
- **Screen:** redrawn at most every `redraw_ms` (500) during a run, so the transfer to the panel stays out of the timings.
- **Saving:** each run is appended to `runs.jsonl` in the app's storage and logged as `R4 RUN ...`. With Wi-Fi up it is POSTed to the laptop; otherwise it waits for DOWN (or the next run) with Wi-Fi up. UP clears saved runs.

Needs two badges flashed with the `temp_firmware` build (it has the `sign_proof` stand-in). Flashing warm-resets the badge: power-cycle afterwards to free the I2C bus.

```
python3 harness/r4_timing.py serve                                          # terminal 1, leave running
python3 harness/r4_timing.py push --host <badge A address> --token <code>   # then the same for badge B
```

1. **Wi-Fi on**, both badges on the hotspot, in the actual room. SELECT on A, wait for `done 200/200`; then SELECT on B, so each key location signs once.
2. **Wi-Fi off:** **Settings → Wi-Fi → Disconnect** on both; check **Settings → ESP-NOW** shows the same channel on both; relaunch R4; SELECT on A, then B. Runs are saved on each badge.
3. **Upload:** reconnect each badge, open R4, press DOWN.
4. **Report:** stop `serve`, run `python3 harness/r4_timing.py report`. It writes `harness/results/<ts>-r4/table.md` and proposes `PRESENCE_DEADLINE_MS = p95 × 1.5` (rounded up to 10 ms) per payee key location, taking the worse Wi-Fi state. Over 5% lost and no deadline is proposed. A owns the constant: agree with A before writing it into 00 §4.

"Payee key" in the report is the **other** badge's key location; one SE050 badge and one NVS badge give both rows.

**Tested (no badges):** `r4_timing.py --check` passes. In the host rig, two badges ran both directions with real Ed25519 PROOFs and uploaded to the real `serve`; a Wi-Fi-off run was saved and not uploaded. **Not yet run:** on real badges; no real timings exist.

## R5 verifier library

Checks the four signed message types in [00-Interfaces.md](../docs/specs/00-Interfaces.md) §5–§7: REQ, PROOF (against its CHAL), the bank payload and the registry record: exact format, Ed25519 signature with the right prefix and signer, expiry and freshness.

| File | Role |
| --- | --- |
| `dashboard/server/src/verify.js` | JS verifier for U's backend (`verifyReq`, `verifyProof`, `verifyBank`, `verifyRegistry`, `verifyRegistryResponse`; also `sigOk` and `verifySolanaMessage` for R3/R6) |
| `vk_verify.py` | Python twin for the attack console, same functions and reasons |
| `vectors.json` | 58 known-good and tampered vectors, also for A's firmware tests |
| `make_vectors.py` | Regenerates `vectors.json` from fixed **test** keys; expectations come from the spec, not from either verifier |

```
harness/.venv/bin/python harness/vk_verify.py --check     # Python: all vectors
cd dashboard && node --test server/src/verify.test.js     # JS: all vectors (also part of npm run check)
harness/.venv/bin/python harness/make_vectors.py          # after a spec change
```

Reasons are identical in both languages: `ok`, `bad_length`, `bad_magic`, `bad_version`, `bad_type`, `bad_field`, `bad_format`, `mismatch`, `bad_signature`, `expired`, `stale`, `revoked`. Time checks run only when `now` is passed.

Interpretations the spec leaves open (change both files and the generator if the team disagrees): REQ amount non-zero, rail 1 uses `HACK` and rail 2 `USD\0`; names printable ASCII; `from_acct` and `attestation` 1–64 printable characters; `solana_ata` empty exactly when `solana_wallet` is; a bank `issued_at` too far in the future is `stale`.

## R6 fuzz set

Malformed inputs for the wallet's decoder and `sign_*` functions, served through the R3 path (`GET /badge/pending`, `POST /badge/outcome`). **Pass:** every fuzz input refused, nothing signed, no screen opened, no crash or watchdog reset, and each function's control accepted.

| File | Role |
| --- | --- |
| `fuzz_cases.mjs` | The 220 cases, plus reference decoders for the rules they target (P1-A §4.2 Solana message, 00 §5 REQ frame, 00 §4 `sign_proof` sizes, 00 §6 bank payload). Node built-ins only |
| `fuzz_cases.test.mjs` | Every fuzz case is rejected by its reference, every control accepted, and `fuzz-vectors.json` is current |
| `r6_fuzz.mjs` | Serves the cases, judges each outcome, writes `harness/results/<ts>-r6/table.md` + `results.json` after every result |
| `apps/r6-fuzz/` | Badge app: calls the wallet function under `pcall`, reports what came back |
| `r6_app.py` | Installs the app with this laptop's address and the badge's pubkey |
| `fuzz-vectors.json` | The same set with fixed **test** payer and mint, for A's firmware host tests |

| Function | Cases | Groups |
| --- | --- | --- |
| `begin_solana` | 154 + 2 controls | two-signer / wrong signer / wrong owner, header, versioned, wrong-prefix, wrong mint / decimals / program, instruction shape, index out of range, lying and non-canonical lengths, trailing bytes, oversized (to 8 KiB), truncation at every field boundary, seeded random bytes, single-bit flips |
| `sign_request` | 18 + 1 control | malformed REQ frame, trailing bytes, prefixed input, bad rail / currency / amount, payee not this badge, control bytes in the name, oversized |
| `sign_proof` | 23 + 1 control | each argument at 0, n−1, n+1 and 4n bytes, nil, number, table; no arguments |
| `begin_bank` | 20 + 1 control | trailing newline, extra / missing / unknown / reordered keys, CRLF, whitespace, bad values, invalid UTF-8, prefixed input, oversized |

Random and bit-flip cases are seeded, so the set is the same every run. Each case has a `basis`: `spec` (written in 00 / P1-A / P2-A) or `interp` (R's reading, to agree with A: every `sign_request` case and a few bank value checks). `interp` failures are reported separately and do not fail R6.

| Badge outcome | Fuzz case | Control |
| --- | --- | --- |
| `nil, reason` (`refused`) | pass; for `begin_*` a reason other than `undecodable` is noted | the function's results are **inconclusive** |
| Lua error (`raised`) | pass, noted for `begin_*` | inconclusive |
| `begin_*` returned true (`screen`) | **FAIL**: the decoder accepted it; press CANCEL | expected |
| 64-byte signature (`signed`) | **FAIL**, with whether it verifies against the badge key | expected; must verify |
| function absent (`missing`) | skipped | skipped |
| `crash` | **FAIL**: "badge reset" if the uptime went backwards, else "app died" | FAIL |

Crash detection: the app writes the case id to `storage.kv` (`inflight`) before each call and removes it after. A relaunch that finds it reports `crash` for that case first. Every request carries `badge.system.millis()`, so the laptop can tell a reset from an app crash. A case with no outcome after 60 s prints a hang warning. R6 reads **PASS** only when `begin_solana` ran and every function present passed on its spec rules; without `begin_solana` it says "NOT DONE".

```
node --env-file-if-exists=dashboard/.env harness/r6_fuzz.mjs --badge 1 --mint <mint pinned in the firmware>   # terminal 1
python3 harness/r6_app.py push --host <badge address> --token <code> --badge 1                             # terminal 2
```

With the `temp_firmware` stand-in the pinned mint is the TEST mint: `--mint 3JF3sEqM796hk5WFqA6EtmEwJQ9quALszsfJyvXNQKy3`, no devnet or `HACK_MINT` needed.

1. Press SELECT on the badge. The controls run first; each `begin_*` control opens the approval screen: press **CANCEL**, never SELECT.
2. The rest runs unattended. If the laptop prints `!!! ... REACHED THE APPROVAL SCREEN`, press CANCEL; the case is already recorded as a failure.
3. After a reset or app crash: power-cycle if the I2C bus is stuck, relaunch **R6 fuzz**, press SELECT. The crash is reported and the run continues.
4. `r6: done. R6: ...` means the report is in `harness/results/<ts>-r6/table.md`. Ctrl+C keeps a partial report.

Options: `--only <fn|group|id,...>` (that function's control always comes along), `--no-controls`, `--list`, `--port` (default 8790), `--pubkey` instead of `--badge`, `--decimals`, `--out <dir>`, `--export <file>` (regenerate `fuzz-vectors.json`: `node harness/r6_fuzz.mjs --export harness/fuzz-vectors.json`). The `--mint` must be the one the firmware pins, or the controls are refused and the run reports INCONCLUSIVE. Inputs are capped at 8 KiB.

```
node --test harness/fuzz_cases.test.mjs      # the set against its reference decoders
```

### R6 on BadgeOS

BadgeOS's `begin_solana` does not refuse bytes it cannot decode: it opens a red "cannot read this payment" approval and `poll()` then returns `undecodable` (docs/os/platform/lua-api.md, "begin reasons" / "poll reasons"). R6 counts any opened screen as a failure, so on BadgeOS 147 of the `begin_solana` cases read FAIL even though nothing can be signed, and each needs a CANCEL press. BadgeOS also has no `sign_request`, `sign_proof` or `begin_bank` in Lua (those cases are `missing`). Decide with A whether R6 should accept "red screen, then `undecodable`" as a refusal before running it there.

**Tested (no badge):** full runs of all 220 cases in the host rig against the real `r6_fuzz.mjs`: on the stand-in, 215 pass and 5 controls OK, R6 PASS; on the firmware on the badges today, every case skipped, NOT DONE. **Not yet run:** on a badge.

## Firmware wallet stand-in

`temp_firmware/solana-os-settings-only/src/lua_sdk/lib_wallet.cpp` stands in for A's wallet API (00 §4) so every harness app runs on the `temp_firmware` build before A's wallet is on the badges. Apps can tell it apart by `wallet.stub == true`.

| Function | Stand-in behaviour |
| --- | --- |
| `pubkey`, `address`, `key_location` | The real identity key |
| `time_ok` | True once SNTP has set the clock (started on the first Wi-Fi join) |
| `sign_proof` / `verify_proof` | `pay-proof:` domain only; arguments must be byte strings of exactly 8 / 16 / 32 B (a number is refused, not coerced) |
| `sign_request` | `pay-req:` + an unsigned REQ frame (00 §5) naming this badge as payee; anything else → `nil, "invalid_req"` |
| `begin_solana` / `begin_bank` | Real byte checks (P1-A §4.2, 00 §6) in `src/lua_sdk/wallet_decode.cpp`; refusals → `nil, "undecodable"`. Valid input opens a firmware-drawn approval screen with a **DEV BUILD - UNVERIFIED** banner: SELECT signs, CANCEL → `cancelled`, 45 s → `timeout`. Presses in the first 400 ms are ignored |
| `poll` | `"pending"`, then the 64-byte signature or `nil, reason` once; `nil, "idle"` when nothing is open; a second `begin_*` meanwhile → `busy` |

`src/lua_sdk/lib_codec.cpp` adds `badge.codec` with BadgeOS's names and rules (`b64enc` / `b64dec` strict, `b58enc` / `b58dec` up to 64 bytes, `hex` / `unhex`), so the apps use the same calls on both firmwares.

**Not done by the stand-in:** registry record, REQ and presence checks. `ctx` is ignored, nothing is verified, and it only signs with `VK_DEV_ALLOW_UNVERIFIED 1`. Never flash judge badges with it.

**Config** (`src/config.h`): `VK_HACK_MINT` (default: the R6 TEST mint; set it to `HACK_MINT` before an R3 run), `VK_HACK_DECIMALS` (2), `VK_DEV_ALLOW_UNVERIFIED` (1), `VK_APPROVAL_TIMEOUT_MS` (45 s).

**SE050 limit:** the SE050 APDU code signs at most 180 B (`MAX_SIGN_MESSAGE_BYTES`). A Solana message (about 251 B) or a bank payload is longer, so on a badge whose key lives in the SE050, SELECT ends in `sign_failed`. Software-key badges are not affected. A needs chained APDUs or a three-byte Lc.

## Testing without a badge

| Test | Command | Covers |
| --- | --- | --- |
| Decoders vs the fuzz set | `cd temp_firmware/solana-os-settings-only && c++ -std=c++17 -O1 -fsanitize=address,undefined -I src/lua_sdk tools/wallet_decode_test.cpp src/lua_sdk/wallet_decode.cpp -o /tmp/wdt && node tools/fuzz-vectors-flat.mjs ../../harness/fuzz-vectors.json \| /tmp/wdt` | all 196 `begin_solana` / `begin_bank` / `sign_request` cases |
| Fuzz set vs its references | `node --test harness/fuzz_cases.test.mjs` | the set itself |
| R5 | `harness/.venv/bin/python harness/vk_verify.py --check`, `node --test dashboard/server/src/verify.test.js` | all vectors |
| R4 statistics | `python3 harness/r4_timing.py --check` | percentiles, deadlines |
| R1 parser | `harness/.venv/bin/python harness/se050_survey.py --check` | log parsing, key validation |

The badge apps were also run in a host rig (2026-10-04, kept outside the repo): the firmware's own Lua 5.4 build and its real `badge.codec` and decoders, a `badge` table generated from the names each firmware registers (the `temp_firmware` fork as on the badges today, the fork with the stand-in, and BadgeOS with its permissions enforced), TweetNaCl for signatures, and real HTTP to the laptop halves. Each app was run before and after the BadgeOS-style rewrite through the same button presses, and the requests, wallet calls and results were the same in every scenario.
