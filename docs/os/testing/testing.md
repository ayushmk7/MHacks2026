# Testing

Three layers: host tests for the pure code (no badge), on-device tests driven over USB with dev hooks (no hands), and a short list of checks that need a person. Plus the numbers to measure.

A work package is done when its tests pass **on a badge**, not when its code is written.

## Host tests

`os/test/host/`. Plain C99/C++17 compiled with the laptop's compiler; no Arduino, no badge.

```bash
cd os && test/host/run.sh        # builds and runs every suite; non-zero exit on any failure
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

What the runner and the shim do, beyond the above (fixed in WP02; later suites rely on it):

- `run.sh` takes suite names as `sol`, `test_sol` or `test_sol.c`; with no argument it runs every suite. Besides the exit status it requires the suite's last output line to be `all <name> tests passed`. A suite names the firmware files it links in a `// LINK:` comment within its first five lines ([execution plan](../roadmap/execution-plan.md#51-host-test-infrastructure-owner-1c-never-edited-afterwards)).
- C suites include the pure headers by relative path (`"../../src/vk/wallet/pure/…"`). `src/identity/tweetnacl.c` alone is compiled with warnings off.
- The in-memory file layer is as strict as the badge: paths are absolute and at most 118 characters; `writeAll`, `renameFile` and `makeDir` fail when the parent directory was not created with `makeDir`; `writeAt` fails on a missing file. A store's test must `makeDir("/vk")` first, as the firmware must.
- The NVS shim follows the real limits: keys and namespaces are 1 to 15 characters; a read-only `begin` on a namespace never written returns false; `putString` of an empty string returns 0 even on success, as on the ESP32 core, so config code must not treat that as a failure.
- Failure injection: `vk_host_nvs_fail_writes` and `vk_host_fileio_fail_writes`. A positive N fails the next N writes and counts down; a negative value fails every write until it is set back to 0. For NVS only `put*` counts; for files `writeAll`, `writeAt` and `renameFile`.
- The shim has no `Serial`.

Vectors: `test/host/vectors.mjs` generates `vectors.json` and `vectors.h` using the dashboard's installed `@solana/kit` (run with `node` from `dashboard/`): transfer messages with and without a Memo, a test issuer key pair, a device key pair, a canonical registry record with its signature, a signed REQ, a PROOF. The record's `solana_ata` is the destination of the two transfer vectors, so those messages pay the record's holder; `tx.mint` is the mint in base58. Regenerating must reproduce the committed files byte for byte.

To make state machines host-testable, code under test never calls `millis()`, `buttons::` or `identity::` directly; it takes them as function pointers that the firmware fills with the real ones and the tests fill with fakes. With `-DVK_HOST_TEST` no Arduino header is included.

## Dev hooks

Dev profile only (`VK_TEST_HOOKS`), in `src/vk/features/devtools/`. They let a script see the screen and press buttons over USB, which is what makes unattended on-device testing possible. They also defeat the physical-press guarantee, which is why the release profile does not contain them and the pre-flash check refuses a dev build for a judge badge.

| Command | Reply | Does |
|---|---|---|
| `VKSTATE` | `OK {json}` | one-line snapshot (below) |
| `VKBTN <key> <action> [ms]` | `OK` | `key`: `up down left right a b`. `action`: `tap [ms]` (press, 80 ms unless `ms` is given, release), `press`, `release`, `hold <ms>`. Times are capped at 60000 ms. `ERR busy` when the queue of 16 events is full |
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
 "provisioned":true,"time":"sntp","notes":0,"poll":"pending","heap":182344,"screen":""}
```

Details the table leaves open, fixed in WP03:

- **Replies.** A dev command with bad arguments answers `ERR usage`. `VKSHOT` with no framebuffer answers `ERR no_canvas`.
- **`VKSHOT`** is the one command with two `OK` lines (`OK shot …` and `OK end …`); the tool treats it as a special case and retries up to three times on a CRC or decode failure. The CRC is the zlib CRC-32 of the decoded framebuffer (153,600 bytes), as 8 lower-case hex digits. Pixels are read with `canvas.readPixel()`, so they are true RGB565 whatever the sprite's byte order. Measured on the badge at 115200 baud, before the shell rewrite: 1.0 s for upstream's launcher, 0.6 s for upstream's `hello` sample; the main loop is blocked for that long. Its reply lines go through upstream's log, so one `VKSHOT` overwrites the 64-line log ring that Settings → Console and `/api/logs` show.
- **`VKSTATE` with no approval open:** `modal` false, `phase` `IDLE`, empty strings for `severity`, `select`, `title`, `headline`, `big` and `sub`, `lines` `[]`, `red_reason` `ok`.
- **`VKSTATE.select`** is the rule in force, not the request's raw field: a red request shows `disabled`, or `hold` when the dev override applies.
- **`VKSTATE.screen`** is the shell's current screen, from `::shell::screenName()` ([shell](../ui/shell.md#framework)). It is empty while an app runs (`app` is then not empty). The names: `launcher`, `app_delete`, `settings`, `wifi`, `bluetooth`, `espnow`, `push`, `store`, `identity`, `identity_new`, `display`, `leds`, `info`, `console`, `app_error`, `offer`, `installing`. An approval open over the shell leaves `screen` as it was and sets `modal`. The Settings rows Theme, Wallet and Inbox act in place or launch an app, so they never appear as a screen name. **Tests use `screen` to know where they are**, never a screenshot comparison: `app == "" and screen == "launcher"` is what "the badge is on the launcher" means in every test.
- **Stubs.** `VKTIME` and `VKNOTE` call `vk::clock::devSet` and `vk::host::notify::post`; `time` and `notes` become real when WP20 and WP32 land.

`poll` comes from `approval::peekResult()`, so reading the state never consumes a result. Tests assert on `VKSTATE` (exact strings) and use `VKSHOT` only to check that something is visibly drawn or to keep a picture for a person to look at.

## The serial tool

`os/scripts/vkdev.py` (Python 3, needs `pyserial`). One tool for everything done over USB.

```bash
vkdev.py --port P info                       # VKINFO, parsed
vkdev.py --port P cmd "VKGET tokens"         # any command
vkdev.py --port P wait-ready [--timeout 30]  # waits for "[os] ready" after a flash or reset
vkdev.py --port P provision ...              # see config.md
vkdev.py --port P push apps/pay              # push one app over serial (AUTH, BEGIN <id> <path>, DATA <base64>, END)
vkdev.py --port P run pay                    # RUN <id>
vkdev.py --port P stop                       # STOP the running app (authenticates first)
vkdev.py --port P reset                      # pulse the reset line, then wait for ready
vkdev.py --port P state                      # VKSTATE as JSON
vkdev.py --port P btn a tap
vkdev.py --port P shot out.png               # VKSHOT decoded to a PNG (stdlib zlib; no Pillow)
vkdev.py --port P test test/device/t_apr.py  # run a scripted device test
vkdev.py --port P monitor                    # tail the log
```

`--code <pairing code>` gives the push pairing code by hand; without it the tool reads it with `VKPAIR`, which only a dev build has. The tool refuses to send a line longer than 250 bytes.

How the tool holds the port, confirmed on the badge in WP01: it opens the port with DTR asserted and RTS released, then releases DTR, so the reset line is never pulsed and **opening the port does not reset the badge** (no boot output, state kept). `reset` asserts RTS for 0.1 s with DTR released, as esptool's hard reset does, and **does reboot it**: the boot banner follows and `[os] ready` comes about 8 s later. `wait-ready` drains the answers to its own `PING`s before it returns.

Serial push rules, from upstream's protocol: every command is answered by exactly one `OK` or `ERR` line, mixed in with `[tag]` log lines, so wait for it before sending the next; send at most 180 raw bytes per `DATA` line; `AUTH` again before each file and after any `RUN` (upstream drops the session whenever an app stops or a launch fails); a file is limited to 96 KB.

A scripted device test is a Python file with `def run(badge):` that uses `badge.cmd()`, `badge.state()`, `badge.btn()`, `badge.shot()`, `badge.wait_state(predicate, timeout)` and plain `assert`. Tests live in `os/test/device/`, one file per group below. The full `badge` API and the helpers in `test/device/common.py` are listed in the [execution plan](../roadmap/execution-plan.md#52-device-test-api-owner-1d). Helpers that changed with the BadgeOS shell: `common.to_launcher(badge)` closes an open approval, stops a running app, and then taps CANCEL until `screen == "launcher"` (at most six taps; CANCEL leaves `app_error`, `app_delete`, every settings page and Settings itself, and it declines an offer); `common.launch(badge, id)` is unchanged. Upstream's sample apps are deleted, so a test that needs "any app" uses `hello_native` or pushes a fixture from `fixtures.py`. `t_boot.py` checks navigation by state: DOWN on the launcher keeps `screen == "launcher"`, CANCEL gives `settings`, CANCEL again gives `launcher`.

The unattended loop for an agent: edit → `scripts/build.sh dev --upload <port>` → `vkdev.py wait-ready` → `vkdev.py test ...` → read the result and the log → repeat.

## Acceptance tests

"Auto" means scriptable with dev hooks on one badge. "2" needs two badges on USB. "Hands" needs a person.

### Boot and platform

| Id | Procedure | Pass | How |
|---|---|---|---|
| T-BOOT1 | flash, `wait-ready`, `PING` | `[os] ready` within 30 s; `OK pong`; the log has the `[vk] registries:` line with a non-zero count for every registry the build contains | auto |
| T-BOOT3 | `VKINFO` | answers; `selfcheck=1`; `key` is `se050` or `software` | auto |
| T-BOOT2 | unprovisioned badge: `VKSTATE` on the launcher | `screen` is `launcher`; `provisioned` false; the launcher's balance row reads `SETUP NEEDED` (kept as a screenshot for a person: `shot`) | auto |
| T-LED1 | watch the LEDs and the screen during boot | no splash image; the Receipt boot screen from the first frame; LEDs fill in order as stages complete; all lit at "Ready" | hands |
| T-HOOK1 | launch an app, exit, launch a second ESP-NOW app, send it a frame from another badge | the second app receives it (upstream finding F1 fixed) | 2 |

### Shell and names

Files `t_shell.py` (launcher, dialogs), `t_pages1.py` and `t_pages2.py` (settings pages). Each runs once per theme (`VKSET theme receipt-light`, then `receipt-dark`). The screens are specified in [shell](../ui/shell.md#tests).

| Id | Procedure | Pass | How |
|---|---|---|---|
| T-SHELL1 | `reset`, `wait-ready`, `VKSTATE` (no autostart app set) | `app` empty, `native` false, `screen` is `launcher`; the boot log has every stage and no `splash` line | auto |
| T-SHELL2 | from the launcher: CANCEL, then for each Settings row DOWN to it and SELECT; CANCEL back | `screen` is `settings`, then the page's name for every page (`wifi`, `bluetooth`, `espnow`, `push`, `store`, `identity` and from it `identity_new`, `display`, `leds`, `info`, `console`); Theme changes `VKGET theme` and stays on `settings`; Wallet and Inbox give `app` `wallet_settings` and `inbox`; one screenshot per screen and theme saved as `os/test/device/shots/shell_<screen>_<theme>.png`, none uniform | auto |
| T-SHELL3 | launch `hello_native` and tap CANCEL; push a fixture whose `main.lua` calls `error()` and run it; launch an app and hold CANCEL 1.7 s | `launcher`; `app_error` (then CANCEL gives `launcher`, and SELECT relaunches the fixture); `launcher` | auto |
| T-SHELL4 | push a fixture app, select it on the launcher, hold RIGHT 900 ms; CANCEL; hold RIGHT again and SELECT. Then hold RIGHT on a native app's row | `app_delete`; CANCEL keeps the app (`LIST` still has it); SELECT removes it and returns to `launcher`; on a native app the screen stays `launcher` | auto |
| T-SHELL5 | a short RIGHT tap, LEFT tap, DOWN and UP on the launcher | the selection moves by column and by row (screenshots differ) and `screen` stays `launcher` | auto |
| T-SHELL6 | `VKDEMOAPPROVE green` while a settings page is showing; CANCEL | `modal` true with `screen` unchanged; after it closes the page is drawn again (screenshot equals the one taken before) | auto |
| T-BRAND1 | the name grep of [upstream-hooks](../architecture/upstream-hooks.md#checking-the-hooks) (pre-flash check 7); the serial banner after a reset; `AUTH` then `INFO` | the grep prints nothing; the banner line reads `#  BadgeOS <version>`; `INFO` answers `OK BadgeOS …` | auto |
| T-BRAND2 | browse to the badge's web page; scan for its hotspot; read a second badge's radar | the page is titled BadgeOS in Receipt colours; nothing visible says Solana | hands |

The app-store offer and installing screens need a broker, which no deployment has: they are checked by reading the code against [shell](../ui/shell.md#app-store-offer) and stay untested on a badge.

### Config

| Id | Procedure | Pass | How |
|---|---|---|---|
| T-CFG1 | `provision`, reboot, `VKGET` each key | values survive; `provisioned=1` | auto |
| T-CFG2 | provisioned: `VKSET approval_tmo_s 30` | `OK pending`; `VKSTATE` shows a `Change setting` confirmation, amber, hold; `btn a hold 3200` writes it; `btn b tap` leaves it unchanged. `t_cfg.py` runs under the test provisioning (`hold_ms` 1000), so it sets 12 with a hold of `hold_ms` + 300 ms and then tries 14 with CANCEL; it puts `approval_tmo_s` back to 10 for `t_apr.py` | auto |
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

T-APR1 has two halves. The offline half (`t_sign.py`, one badge, no network) uses `checktest` with a transfer built on the laptop whose account 0 is the badge's own key: red `UNVERIFIED RECIPIENT` with the dev override, an injected hold, and the returned signature verified on the laptop against `VKINFO pubkey`; it also signs a transfer built on the badge by `wallet.build_transfer` with a memo. The on-chain half (`t_sign_net.py`, `NEEDS = "network"`) is `signtest` against the dashboard listener and devnet. T-APR5 asserts the specific headline per vector and the `[pay] undecodable: program|header|version|trailing` log line for the four decoder failures; the wrong-mint vector decodes, so it logs nothing. `t_sign.py` also covers review focus 2: an app stopped before it polls leaves the next `begin` not `busy`, and `DEL checktest` while the approval is open closes it.

### Checks

T-CHK2 to T-CHK9 run offline on one badge (`t_chk.py`): `checktest` passes a prepared `ctx`, and `test/device/fixtures.py` builds the message (account 0 is the badge's own `VKINFO` key), a fresh record signed by the test issuer of `vectors.json`, and the request, for each case. The vectors' own record and messages are used only in the fixtures' self-check (`python test/device/fixtures.py`), because that record is older than `record_ttl_s` and its messages have another payer. T-CHK1 needs a second badge. Against the real backend the same cases need its `/registry` route.

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

`t_chk.py` adds two cases with no id: the replay (valid record and valid request, no presence: amber `VERIFIED - NOT PRESENT`) and review focus 1 (after a `begin` with a record and a request the app still logs `CT tick`: the callback was not killed). The second half of T-CHK9 resets the badge so the clock has no source; if a saved Wi-Fi network syncs SNTP after that reset, CLOCK UNSYNCED cannot appear and the test accepts amber `VERIFIED - NOT PRESENT` with a printed note. Each case saves `test/device/shots/chk_<case>.png`.

### Requests and presence

| Id | Procedure | Pass | How |
|---|---|---|---|
| T-REQ1 | badge B: the `reqtest` app opens 10.00; badge A: `wallet.requests()` | A lists it within 2 s with the right amount, name claim and key | 2 |
| T-REQ2 | A: `challenge`; poll `presence` | `present` within `presence_ms` | 2 |
| T-REQ3 | a third badge (or A itself) replays B's REQ bytes; A challenges the replayer's MAC | `presence` stays `pending` then the approval is amber | 2 |
| T-REQ4 | send B nine CHALs for one request | at most `req_max_proofs` PROOFs come back | 2 |
| T-REQ5 | request seen while the launcher is showing | notification appears (`notes` ≥ 1, the launcher's `inbox` row shows `1` as its value); the Inbox entry names the app in `pay_app` | 2 |
| T-REQ6 | during an approval on badge A, a BLE central sends a line to an app that called `badge.ble.listen()`, and `RUN other` is sent over serial | the app's `on_ble` does not run and `other` starts only after the approval closes | auto |

### Apps and permissions

| Id | Procedure | Pass | How |
|---|---|---|---|
| T-APP1 | Lua app with no `sign` permission calls `wallet.begin_solana` | Lua error `permission 'sign' not granted` | auto |
| T-APP2 | app with `permissions=sign`, first launch | consent confirmation; hold approves and the app starts; second launch has no prompt | auto |
| T-APP3 | change that app's permissions line, push, launch | consent asked again | auto |
| T-APP4 | app with `min_api=99` | refused with `needs a newer BadgeOS`; `screen` is `app_error` | auto |
| T-APP5 | launch `hello_native`, CANCEL, launch again | draws, exits, fresh state | auto |
| T-APP6 | native app without `sign` calls `vk::wallet::begin` | `denied` | auto |
| T-APP7 | app with `permissions=` empty uses `badge.http.get` | Lua error naming `net` | auto |

### Stores, contacts, LEDs

| Id | Procedure | Pass | How |
|---|---|---|---|
| T-STO1 | sign once, reboot, `wallet.history(1)` | the entry is there with the right amount and outcome | auto |
| T-STO2 | Lua app tries `badge.storage.read("../../vk/history.bin")` and `"/vk/history.bin"` | both fail (upstream raises a Lua error for a path outside the app folder; `checktest` wraps the call in `pcall`, and an error or `nil` both count as failing) | auto |
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
| T-REL1 | every screen of every shipped app and every shell screen: press CANCEL repeatedly | always returns to the launcher (`app` empty, `screen` `launcher`); hold CANCEL 1.5 s force-quits an app | hands (scripted in Batch 5 with `VKSTATE`) |
| T-REL2 | release build: send each dev command | none is recognised | auto (`ERR` or silence) |
| T-REL3 | the demo script end to end on the four labelled badges (release app set, which includes `evilgame`) | honest payment green and confirmed; impostor caught; evil game exposed; revoked merchant red | hands |

## Measurements

Fill in on hardware; these numbers set config values and decide fallbacks.

| Id | What | How | Result | Decides |
|---|---|---|---|---|
| M1 | CHAL → PROOF round trip, 50 samples | log line `[req] proof <ms> ms` on the payer | p50 = , p95 = | `presence_ms` = p95 × 1.5 |
| M2 | one Ed25519 verify; one sign (software key; SE050 key) | log lines `[vk] verify <ms> ms`, `[vk] sign <domain> <n> bytes <ms> ms` | Batch 3 (2026-10-03, software key, TweetNaCl): **verify 419 ms** (median of 45 samples over three runs of `t_chk.py`, every sample 418 to 420). **Sign 211 ms** (`[vk] sign solana 214 bytes 211 ms`; `pay-req` 210 and 211 ms). SE050: not measurable on this badge | verify is above 400 ms: the Monocypher backend (`VK_ED25519_BACKEND 1`) is called for |
| M3 | `begin_solana` → approval visible | timestamp in the log at `begin` and at first draw | Batch 3, from the app's call (`CT call <ms>`) to `[vk] approval first draw at <ms>`: **12 ms** with no record, **434 ms** with a record (one verification), **854 ms** with a record and a request (two). Before the duplicate record check was removed ([checks](../wallet/checks.md#verdict-to-screen)) the last two were 853 and 1273 ms. Engine part alone (WP12): 11 ms | target under 2 s: met |
| M4 | image size; free heap and free PSRAM in the launcher and during an approval | compile output; `VKSTATE.heap`; upstream heartbeat line | WP01 dev build (2026-10-03): 1,882,539 bytes, 56 % of the slot (unmodified upstream: 1,871,707); globals 77,504 bytes. Launcher: heap 197 to 202 KB free, PSRAM 7,971 KB free. Batch 3 dev build: 1,961,263 bytes, 59 % of the slot; launcher heap 186 KB free, PSRAM 7,971 KB; during a payment approval opened by a Lua app `VKSTATE.heap` is 189,568 | slot is 3,342,336 bytes |
| M5 | one RPC request over the hotspot | `[bal] fetch <ms> ms` | | `balance_poll_s`, HTTP timeouts |
| M6 | loop-task stack high-water mark during a signature and during an HTTPS request | `uxTaskGetStackHighWaterMark(NULL)` logged once a minute in the dev profile | | if under 1 KB free, raise the loop stack with `SET_LOOP_TASK_STACK_SIZE(16 * 1024)` in `os.ino` (a new hook) |

The cost of one history write (22 to 320 ms, growing with the file) is measured in [stores](../wallet/stores.md#history).

## What cannot be tested without a person

Real button presses on a release build, LED colours, screen legibility at arm's length, and BOOT/RST recovery. Everything else above is scripted.
