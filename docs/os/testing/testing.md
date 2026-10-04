# Testing

Three layers: host tests for the pure code (no badge), on-device tests driven over USB with dev hooks (no hands), and a short list of checks that need a person. Plus the numbers to measure.

A work package is done when its tests pass **on a badge**, not when its code is written.

## Host tests

`firmware/solana-os/test/host/`. Plain C99/C++17 compiled with the laptop's compiler; no Arduino, no badge.

```bash
cd firmware/solana-os && test/host/run.sh        # builds and runs every suite; non-zero exit on any failure
```

`run.sh` compiles each suite with `cc -std=c99 -Wall -Wextra -Wpedantic -O2 -DSOL_HOST_SHA256 -DVK_HOST_TEST` (C++ suites: `c++ -std=c++17`), plus upstream's `src/identity/tweetnacl.c` with a stub `randombytes` for signature checks. The C suites (`test_sol.c`, `test_record.c`, `test_frames.c`, `test_checks.c`) link only `src/vk/wallet/pure/`. The C++ suites (`test_domains.cpp`, `test_approval.cpp`, `test_config.cpp`, `test_manifest.cpp`, `test_consent.cpp`, `test_stores.cpp`, `test_contacts.cpp`) each link the one firmware `.cpp` under test, compiled with `-DVK_HOST_TEST -Itest/host/shim`. `test/host/shim/` holds a minimal `Arduino.h` (a `String` built on `std::string` with the handful of methods used, `millis()` from a settable fake clock, `strlcpy`) and in-memory stand-ins for the file and NVS calls. A firmware file that is host-tested keeps its hardware calls behind `#ifndef VK_HOST_TEST` or behind function pointers. Each suite prints `all <name> tests passed` and returns 0.

| Suite | Covers | Spec |
|---|---|---|
| `test_sol` | base58, decoder rules (one negative vector per rule), Memo, compact-u16, builder round trip, amount format/parse | [solana-payments](../wallet/solana-payments.md#tests) |
| `test_record` | registry record parser, strictness, signature verify | [checks](../wallet/checks.md#tests) |
| `test_frames` | every frame type: round trip, strict lengths, signed-bytes helpers | [protocol](../protocol/espnow.md#codec) |
| `test_checks` | the check chain: every failure row, every amber condition, cap, green | [checks](../wallet/checks.md#tests) |
| `test_domains` | the domain self-check accepts the shipped table and rejects each rule violation | [signing](../wallet/signing.md#self-check) |
| `test_approval` | approval state machine with fake clock and buttons | [approval](../wallet/approval.md#tests) |
| `test_config` | config text parsers (tokens, keys, ranges) | [config](../platform/config.md#tests) |
| `test_manifest`, `test_consent` | manifest parser, consent hash and store | [app host](../platform/app-host.md#tests) |
| `test_stores`, `test_contacts` | history ring, bad-magic recovery; contacts store and card acceptance | [stores](../wallet/stores.md#tests) |

Vectors: `test/host/vectors.mjs` generates `vectors.json` and `vectors.h` using the dashboard's installed `@solana/kit` (run with `node` from `dashboard/`): transfer messages with and without a Memo, a test issuer key pair, a device key pair, a canonical registry record with its signature, a signed REQ. Regenerating must reproduce the committed files byte for byte.

To make state machines host-testable, code under test never calls `millis()`, `buttons::` or `identity::` directly; it takes them as function pointers that the firmware fills with the real ones and the tests fill with fakes. With `-DVK_HOST_TEST` no Arduino header is included.

## Dev hooks

Dev profile only (`VK_TEST_HOOKS`), in `src/vk/features/devtools/`. They let a script see the screen and press buttons over USB, which is what makes unattended on-device testing possible. They also defeat the physical-press guarantee, which is why the release profile does not contain them and the pre-flash check refuses a dev build for a judge badge.

| Command | Reply | Does |
|---|---|---|
| `VKSTATE` | `OK {json}` | one-line snapshot (below) |
| `VKBTN <key> <action> [ms]` | `OK` | `key`: `up down left right a b`. `action`: `tap` (press, 80 ms, release), `press`, `release`, `hold <ms>` |
| `VKSHOT` | `OK shot 320 240`, then `+ <base64>` lines, then `OK end <crc32>` | the framebuffer, RGB565 little-endian, run-length encoded as (count u8, pixel u16) triples, base64 in 96-character lines |
| `VKTIME <unix>` | `OK` | sets the clock and marks the source SNTP (lets tests exercise expiry without a network) |
| `VKPAIR` | `OK <code>` | upstream's push pairing code, so apps can be pushed over serial without reading the screen |
| `VKNOTE <title>\|<body>\|<app>` | `OK` | posts a notification |
| `VKDEMOAPPROVE <green\|amber\|red>` | `OK` | raises a sample confirmation of that severity, to test the approval screen without a signing domain |

`VKSTATE` JSON:

```json
{"app":"pay","native":false,"modal":true,"phase":"ARMED","severity":"amber","select":"hold",
 "title":"Pay","headline":"VERIFIED - NOT PRESENT","big":"10.00 HACK","sub":"to MHacks Merch",
 "lines":[["Account","2awX..6wrr"],["Kind","merchant"]],"red_reason":"ok","dev_override":false,
 "provisioned":true,"time":"sntp","notes":0,"poll":"pending","heap":182344}
```

`poll` comes from `approval::peekResult()`, so reading the state never consumes a result. Tests assert on `VKSTATE` (exact strings) and use `VKSHOT` only to check that something is visibly drawn or to keep a picture for a person to look at.

## The serial tool

`firmware/solana-os/scripts/vkdev.py` (Python 3, needs `pyserial`). One tool for everything done over USB.

```bash
vkdev.py --port P info                       # VKINFO, parsed
vkdev.py --port P cmd "VKGET tokens"         # any command
vkdev.py --port P wait-ready [--timeout 30]  # waits for "[os] ready" after a flash or reset
vkdev.py --port P provision ...              # see config.md
vkdev.py --port P push apps/pay              # push one app over serial (AUTH, BEGIN <id> <path>, DATA <base64>, END)
vkdev.py --port P run pay                    # RUN <id>
vkdev.py --port P state                      # VKSTATE as JSON
vkdev.py --port P btn a tap
vkdev.py --port P shot out.png               # VKSHOT decoded to a PNG (stdlib zlib; no Pillow)
vkdev.py --port P test test/device/t_apr.py  # run a scripted device test
vkdev.py --port P monitor                    # tail the log
```

Serial push rules, from upstream's protocol: every command is answered by exactly one `OK` or `ERR` line, mixed in with `[tag]` log lines, so wait for it before sending the next; send at most 180 raw bytes per `DATA` line; `AUTH` again before each file and after any `RUN` (upstream drops the session whenever an app stops or a launch fails); a file is limited to 96 KB.

A scripted device test is a Python file with `def run(badge):` that uses `badge.cmd()`, `badge.state()`, `badge.btn()`, `badge.shot()`, `badge.wait_state(predicate, timeout)` and plain `assert`. Tests live in `firmware/solana-os/test/device/`, one file per group below.

The unattended loop for an agent: edit → `scripts/build.sh dev --upload <port>` → `vkdev.py wait-ready` → `vkdev.py test ...` → read the result and the log → repeat.

## Acceptance tests

"Auto" means scriptable with dev hooks on one badge. "2" needs two badges on USB. "Hands" needs a person.

### Boot and platform

| Id | Procedure | Pass | How |
|---|---|---|---|
| T-BOOT1 | flash, `wait-ready`, `PING` | `[os] ready` within 30 s; `OK pong`; the log has the `[vk] registries:` line with a non-zero count for every registry the build contains | auto |
| T-BOOT3 | `VKINFO` | answers; `selfcheck=1`; `key` is `se050` or `software` | auto |
| T-BOOT2 | unprovisioned badge: `VKSTATE` in the launcher | status item `SETUP` present (`shot` shows it); `provisioned` false | auto |
| T-LED1 | watch the LEDs during boot | LEDs fill in order as stages complete; all lit at "Ready" | hands |
| T-HOOK1 | launch an app, exit, launch a second ESP-NOW app, send it a frame from another badge | the second app receives it (upstream finding F1 fixed) | 2 |

### Config

| Id | Procedure | Pass | How |
|---|---|---|---|
| T-CFG1 | `provision`, reboot, `VKGET` each key | values survive; `provisioned=1` | auto |
| T-CFG2 | provisioned: `VKSET approval_tmo_s 30` | `OK pending`; `VKSTATE` shows a `Change setting` confirmation, amber, hold; `btn a hold 3200` writes it; `btn b tap` leaves it unchanged | auto |
| T-CFG3 | `VKSET tokens garbage` | `ERR invalid`; value unchanged | auto |
| T-CFG4 | send `VKINFO` over BLE or the HTTP push API | not recognised (commands are USB only) | hands |

### Approval

| Id | Procedure | Pass | How |
|---|---|---|---|
| T-APR1 | signtest: serve a valid transfer; `btn a hold` in the dev profile | signature returned; verifies on the laptop against the badge key; transaction confirms on devnet | auto |
| T-APR2 | hold `a` *before* the approval opens, keep holding | not approved; phase stays `WAIT_RELEASE` until released | auto |
| T-APR3 | open an approval; do nothing | closes after `approval_tmo_s`; `poll` gives `timeout` (or the red reason) | auto |
| T-APR4 | a Lua app that calls `badge.gfx.flush()` and draws every frame calls `begin_solana` | `shot` during the approval shows only the firmware screen; the app's `on_update` counter does not advance while `modal` is true | auto |
| T-APR5 | serve each refusal vector: second instruction, wrong mint, two signers, v0 message, trailing bytes | each: red `CANNOT READ PAYMENT` or `UNKNOWN TOKEN`; `select` disabled even in the dev profile; `poll` gives `undecodable` | auto |
| T-APR6 | release profile: any red approval; press and hold SELECT | never signs | hands (release build has no hooks) |

### Checks

Needs the backend's `/registry` route. Each is run by a test app that passes a prepared `ctx`.

| Id | Procedure | Pass | How |
|---|---|---|---|
| T-CHK1 | valid record, valid request, presence present, SNTP | green `VERIFIED - PRESENT`, press | 2 |
| T-CHK2 | valid record, no request | amber `VERIFIED - NOT PRESENT`, hold | auto |
| T-CHK3 | no record (release semantics checked via `VKSTATE`) | red `UNVERIFIED RECIPIENT`; `dev_override` true only in the dev profile | auto |
| T-CHK4 | record with one byte of the signature flipped | red `UNVERIFIED RECIPIENT`, not overridable | auto |
| T-CHK5 | revoked record | red `REVOKED` | auto |
| T-CHK6 | record for merchant, transfer to another account | red `WRONG RECIPIENT`, line `Expected` | auto |
| T-CHK7 | request for 10.00, transfer for 500.00 | red `WRONG AMOUNT`, line `Requested 10.00 HACK` | auto |
| T-CHK8 | amount above `cap`; above `max` | hold with line `Limit`; red `OVER LIMIT` | auto |
| T-CHK9 | `VKTIME` to past the record's expiry; then, on a badge that has not synced, a fresh record | red `EXPIRED`; amber `CLOCK UNSYNCED` | auto |

### Requests and presence

| Id | Procedure | Pass | How |
|---|---|---|---|
| T-REQ1 | badge B: the `reqtest` app opens 10.00; badge A: `wallet.requests()` | A lists it within 2 s with the right amount, name claim and key | 2 |
| T-REQ2 | A: `challenge`; poll `presence` | `present` within `presence_ms` | 2 |
| T-REQ3 | a third badge (or A itself) replays B's REQ bytes; A challenges the replayer's MAC | `presence` stays `pending` then the approval is amber | 2 |
| T-REQ4 | send B nine CHALs for one request | at most `req_max_proofs` PROOFs come back | 2 |
| T-REQ5 | request seen while the launcher is showing | notification appears (`notes` ≥ 1, status bar shows `[1]`); the Inbox entry names the app in `pay_app` | 2 |
| T-REQ6 | during an approval on badge A, a BLE central sends a line to an app that called `badge.ble.listen()`, and `RUN other` is sent over serial | the app's `on_ble` does not run and `other` starts only after the approval closes | auto |

### Apps and permissions

| Id | Procedure | Pass | How |
|---|---|---|---|
| T-APP1 | Lua app with no `sign` permission calls `wallet.begin_solana` | Lua error `permission 'sign' not granted` | auto |
| T-APP2 | app with `permissions=sign`, first launch | consent confirmation; hold approves and the app starts; second launch has no prompt | auto |
| T-APP3 | change that app's permissions line, push, launch | consent asked again | auto |
| T-APP4 | app with `min_api=99` | refused with `needs a newer Badge OS` | auto |
| T-APP5 | launch `hello_native`, CANCEL, launch again | draws, exits, fresh state | auto |
| T-APP6 | native app without `sign` calls `vk::wallet::begin` | `denied` | auto |
| T-APP7 | app with `permissions=` empty uses `badge.http.get` | Lua error naming `net` | auto |

### Stores, contacts, LEDs

| Id | Procedure | Pass | How |
|---|---|---|---|
| T-STO1 | sign once, reboot, `wallet.history(1)` | the entry is there with the right amount and outcome | auto |
| T-STO2 | Lua app tries `badge.storage.read("../../vk/history.bin")` and `"/vk/history.bin"` | both fail | auto |
| T-CON1 | two badges swap contacts | each lists the other's name and address | 2 |
| T-CON2 | replay a captured CARD after the swap | `expired` | 2 |
| T-LED2 | open green, amber and red approvals | LED colours match; red is solid | hands |

### Secure element

| Id | Procedure | Pass | How |
|---|---|---|---|
| T-SE1 | on a badge whose `VKINFO` says `key=se050`: T-APR1 with a 214-byte transfer | signature returned and verifies | auto |
| T-SE2 | same badge, a transfer with a Memo | refused with `too_long` before any screen | auto |

### Release gate

| Id | Procedure | Pass | How |
|---|---|---|---|
| T-REL1 | every screen of every shipped app: press CANCEL repeatedly | always returns to the launcher; hold CANCEL 1.5 s force-quits | hands |
| T-REL2 | release build: send each dev command | none is recognised | auto (`ERR` or silence) |
| T-REL3 | the demo script end to end on the four labelled badges (release app set, which includes `evilgame`) | honest payment green and confirmed; impostor caught; evil game exposed; revoked merchant red | hands |

## Measurements

Fill in on hardware; these numbers set config values and decide fallbacks.

| Id | What | How | Result | Decides |
|---|---|---|---|---|
| M1 | CHAL → PROOF round trip, 50 samples | log line `[req] proof <ms> ms` on the payer | p50 = , p95 = | `presence_ms` = p95 × 1.5 |
| M2 | one Ed25519 verify; one sign (software key; SE050 key) | log lines `[vk] verify <ms> ms`, `[vk] sign <domain> <n> bytes <ms> ms` | | if verify > 400 ms, switch `VK_ED25519_BACKEND` to 1 |
| M3 | `begin_solana` → approval visible | timestamp in the log at `begin` and at first draw | | target under 2 s |
| M4 | image size; free heap and free PSRAM in the launcher and during an approval | compile output; `VKSTATE.heap`; upstream heartbeat line | | slot is 3,342,336 bytes |
| M5 | one RPC request over the hotspot | `[bal] fetch <ms> ms` | | `balance_poll_s`, HTTP timeouts |
| M6 | loop-task stack high-water mark during a signature and during an HTTPS request | `uxTaskGetStackHighWaterMark(NULL)` logged once a minute in the dev profile | | if under 1 KB free, raise the loop stack with `SET_LOOP_TASK_STACK_SIZE(16 * 1024)` in `solana-os.ino` (a new hook) |

## What cannot be tested without a person

Real button presses on a release build, LED colours, screen legibility at arm's length, and BOOT/RST recovery. Everything else above is scripted.
