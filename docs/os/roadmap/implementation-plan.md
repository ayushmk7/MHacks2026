# Badge OS Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan work package by work package. Steps use checkbox (`- [ ]`) syntax for tracking. Each work package is a sub-project: before coding, its executor reads the documents listed under **Read**, then writes the tests named under **Tests** first.

**Goal:** Turn Solana OS into Badge OS: a badge that signs payments only through a firmware approval screen, verifies who is being paid, and hosts Lua and native apps on a platform where adding or removing a feature is one file or one folder.

**Architecture:** A fork of Solana OS at `firmware/solana-os/`, changed only by marked one-line hooks. All new code is under `src/vk/`: core services, a wallet core with one signing path and a table of signing domains, a generic approval engine that pauses apps while the user decides, an app host (permissions, native apps, ESP-NOW router, notifications), and self-contained feature folders that register themselves at start-up.

**Tech stack:** Arduino-ESP32 3.x built with `arduino-cli`; C++17 for firmware glue; C99 for the host-tested pure code; Lua 5.4 apps; Python 3 + pyserial for the serial tool; Node (the dashboard's packages) for test vectors.

**Spec:** this folder, `docs/os/`. Start at [../README.md](../README.md). Where `docs/specs/` differs, `docs/os/` wins ([differences](../reference/differences-from-specs.md)).

## Global constraints

- Upstream base: `firmware/solana-os/` of <https://github.com/spacemandev-git/solana-defcon-badge-26> at commit `812b8c7aca5c366d18c0b040fafd2999f7204d84`.
- Every edit to an upstream file is a hook listed in [upstream-hooks.md](../architecture/upstream-hooks.md), tagged `// VK: H<n>`. All hooks (H1–H17, H19, H20) are applied in WP01; later work packages do not touch upstream files (exceptions are named in the package).
- All new firmware code lives under `src/vk/`, `src/native_apps/`, `apps/`, `lib/`, `scripts/`, `test/`.
- The only caller of `identity::sign` / `identity::signBase64` is `src/vk/wallet/signer.cpp`.
- `VK_SIGN_DOMAIN(` appears only in `src/vk/features/*/domain_*.cpp`.
- No key, mint, URL, SSID or limit is a literal in source. Deployment values are config keys ([config](../platform/config.md)); tunables have their default in their `VK_CONFIG_KEY` line and nowhere else.
- No code assumes the number of LEDs; use `RGB_LED_COUNT`.
- Amounts cross APIs as decimal strings or raw bytes, never as Lua numbers.
- Extensible lists are registries ([overview](../architecture/overview.md#6-self-registration)); no central table, no switch on a feature name.
- A feature includes headers only from `src/vk/core/`, `wallet/`, `host/`, `ui/`, never from another feature.
- Pure code in `src/vk/wallet/pure/` is C99 with no Arduino include and no heap; time, buttons, signing and verification reach testable code through function pointers.
- Nothing blocks the main loop for longer than one signature or one HTTP request.
- Reason strings, headline strings, config key names, frame layouts and Lua function names are exactly as written in the spec documents.
- `scripts/preflash-check.sh` and `test/host/run.sh` pass before every flash.
- Judge badges are flashed only with the `release` profile.
- Commits: one branch per work package, `wp<NN>-<slug>`; commit after each step that leaves the tree building; message prefix `WP<NN>:`.

## Review focus

Conditions the specification implies but that are easy to miss. Each has a test in the package that owns the code.

1. **Slow crypto inside a Lua callback.** `begin_solana` with a full `ctx` verifies a record and a request (about a second each with TweetNaCl) inside one 250 ms callback. Expected: the binding extends the deadline and the app survives. Test in WP21 (device: call with full ctx, app still running afterwards).
2. **App stopped while an approval is open.** Force-quit (hold CANCEL) or a pushed `STOP` during the approval. Expected: the approval closes, no result lingers, the next `begin` from another app is not `busy`. Test in WP12 (host: `appStopping`; device: `STOP` over serial while `modal` is true).
3. **Storage failure.** `/vk/` missing on first boot, filesystem full, or NVS write refused. Expected: directory is created; a failed history write never blocks or fails a signature; `VKSET` answers `ERR`. Tests in WP10 (config) and WP24 (history with a failing file layer on the host).
4. **No network.** Wi-Fi absent at boot or lost mid-flow. Expected: boot does not wait for SNTP; `vk.*` network helpers return `nil, message`; the balance service skips; the pay flow ends in `failed` with a reason, never hangs. Tests in WP20, WP33, WP35.
5. **Registration dropped by the linker.** A feature's static registrar is not linked, so the feature silently does nothing. Expected: `VKINFO`/`VKHELP` list what is registered and a boot log line prints the count of each registry. Test in WP01 (the count line is asserted by T-BOOT1 in every later package).

## Work packages at a glance

| WP | Name | Depends on | Needs a badge | Can run in parallel with |
|---|---|---|---|---|
| 00 | Toolchain; upstream on a badge | — | yes (hands once) | 02 |
| 01 | Fork, skeleton, all hooks | 00 | yes | 02 |
| 02 | Pure code and host tests | — | no | 00, 01 |
| 03 | Dev tools and serial tool | 01 | yes | 02 |
| 10 | Config store and provisioning | 01, 02, 03 | yes | — |
| 11 | Signer and domain table | 01, 02, 10 | yes | 20, 22 |
| 12 | Approval engine, screen, LED patterns | 10, 11 | yes | 20, 22 |
| 13 | Solana payments, Lua wallet, Sign test | 10, 12, 03 | yes | — |
| | **Gate 1: a badge-signed payment confirmed on devnet** | | | |
| 20 | Clock | 10 | yes | 11, 12, 22 |
| 21 | Record checks wired into the approval | 13, 20 | yes | 22, 24 |
| 22 | ESP-NOW router | 01 | yes (2) | 10–13, 20, 21, 24 |
| 23 | Requests and presence | 21, 22 | yes (2) | 24 |
| 24 | History | 12, 20 | yes | 21–23 |
| | **Gate 2: impostor red, replay amber, honest payee green** | | | |
| 30 | Permissions, consent, API version | 13 | yes | 31–35 |
| 31 | Native runtime and SDK | 13, 20, 30 | yes | 33–35 |
| 32 | Notifications, status items, Inbox | 23, 31 | yes | 33, 35 |
| 33 | Balance | 10 | yes | 30–32, 34, 35 |
| 34 | Contacts feature | 10, 11, 20, 22, 32 | yes (2) | 33, 35 |
| 35 | `lib/vk.lua` | 13, 23, 33 | yes | 30–32, 34 |
| 36 | Wallet settings app | 20, 24, 30, 31, 33 | yes | 4x |
| 37 | Receipt launcher, settings app, home service, boot screen | 12, 31, 33 | yes | 34–36 |
| | **Gate 3: unpermitted app refused; native app runs; request raises a notification** | | | |
| 40 | Home | 33, 35 | yes | 41–45 |
| 41 | Pay and Request | 23, 35 | yes (2) | 40, 42–45 |
| 42 | History app | 24, 30 | yes | 40–45 |
| 43 | Contacts app | 34, 35 | yes (2) | 40–45 |
| 44 | Game and Evil game | 35, 21 | yes | 40–45 |
| 45 | Duel | 41 | yes (2) | 40–44 |
| | **Gate 4: the demo script runs end to end** | | | |
| 50 | SE050 bring-up | 13 | yes (an SE050-keyed badge) | 2x–4x |
| 51 | Measurements and tuning | 23 | yes (2) | 3x, 4x |
| 52 | Release: four badges, release gate | all shipped | yes (4, hands) | — |
| 54 | Bank rail (optional) | 23 | yes (2) | — |

File ownership is disjoint between packages that may run in parallel: each package creates or fills only the files listed under **Files**. Stubs created in WP01 are filled in by the package that owns that file.

If time runs out, cut in this order: WP54, WP45, WP43 + WP34, WP36, WP32 inbox (keep the status item), WP33. Gates 1 and 2 are never cut.

---

## Phase 0: ground

### WP00: Toolchain; upstream on a badge

**Goal:** prove the laptop, cable and badge with unmodified upstream.
**Read:** [build guide](../guides/build-flash-provision.md) through "Build upstream first".
**Files:** none in the repository except corrections to the build guide and its version table.

- [ ] Install the toolchain with the commands in the guide. Record versions in the guide's table.
- [ ] Clone upstream to `/tmp/upstream`, check out `812b8c7`, compile. Expected: compile succeeds and prints an image size near 1.85 MB. If the newest 3.x core fails, install an older 3.x and record which.
- [ ] Upload to the badge on `/dev/cu.usbserial-*`. If it cannot connect, a person holds `BOOT1`, taps `RST1`.
- [ ] Open the monitor. Expected: banner, `[lcd] ready 320x240`, an `[id]` line, `[os] ready`. Send `PING`; expected `OK pong`.
- [ ] Record for each available badge: port, `[id]` line (secure element or software), public key (`curl` `/api/identity` or Settings → Identity).
- [ ] Correct anything in the guide that did not match.

**Done when:** one badge runs unmodified upstream, answers `PING`, and the guide's commands are known to work.

### WP01: Fork, skeleton, all hooks

**Goal:** `firmware/solana-os/` exists, contains the `src/vk/` skeleton and every hook, builds, and behaves exactly like upstream.
**Read:** [overview](../architecture/overview.md), [upstream-hooks](../architecture/upstream-hooks.md), [upstream-baseline](../architecture/upstream-baseline.md).
**Files:**
- Create: `firmware/solana-os/` (copy of upstream), `UPSTREAM-HOOKS.md`, `.gitignore` entry for `src/vk/vk_profile.h`
- Create: `src/vk/vk.h`, `vk.cpp`, `vk_build.h`, `core/registry.h`, `core/service.h`
- Create complete: `core/serial.{h,cpp}` (command registry, info-field registry, `handleLine`, `VKHELP`, `VKINFO` with fields `profile` and `api`), `host/lifecycle.{h,cpp}` (`VK_ON_APP_STOP`, `onAppStopping`; `luaPaused()` returns `vk::modalActive()`), `host/lua_registry.{h,cpp}` (everything except permission filtering, which WP30 adds; removes `loadfile`/`dofile`), `ui/statusbar.{h,cpp}` (registry, `draw`, `requestShellRepaint`, `consumeShellRepaint`), `ui/theme.{h,cpp}` (token enum, registry, the default theme `solana` with upstream's colours, `color()`)
- Create as stubs: full headers exactly as in the spec, with bodies that do nothing, to be filled by the named package: `core/config.{h,cpp}` (WP10; registry declared, accessors return defaults, `provisioned()` false), `core/clock.{h,cpp}` (WP20; source NONE), `host/router.{h,cpp}` (WP22), `host/permissions.{h,cpp}` (WP30; `granted()` true, `preLaunch()` true), `host/manifest.h`, `host/consent.h` (WP30; headers only), `host/native.{h,cpp}` (WP31), `host/notify.{h,cpp}` (WP32; `post()` drops, `count()` 0), `sdk/badge_sdk.hpp` (WP31; complete header, since it only declares), `ui/leds.{h,cpp}` (WP12), `wallet/signer.{h,cpp}`, `wallet/reason.h` (WP11), `wallet/approval.{h,cpp}` (WP12), `features/devtools/devtools.{h,cpp}` (WP03; a no-op `vk_dev_apply_injected_buttons` so hook H17 links)
- Create: `scripts/build.sh`, `scripts/preflash-check.sh` (checks 1–4 and 6; check 5 is added when `test/host/run.sh` lands)
- Modify: the upstream files named in hooks H1–H17, H19 and H20 (H18 only in WP50)

**Interfaces produced:** `vk::begin()`, `vk::update()`, `vk::modalActive()`, `vk::modalUpdate()`; `vk::Registered<T>`; `VK_SERVICE`; `vk::serial::handleLine`, `VK_SERIAL_COMMAND`; `VK_LUA_FUNCTION`, `vk::lua::open`; `VK_STATUS_ITEM`, `vk::ui::statusbar::draw`; the stub signatures exactly as in the spec so later packages only replace bodies.

Stub behaviour (must equal upstream behaviour): `modalActive()` false; `router::install()` installs `espnow_mgr::onReceive([](const uint8_t *m, const uint8_t *d, size_t n, int8_t r) { if (runtime::running()) runtime::dispatchEspnow(m, d, n, r); })`; `preLaunch` returns true; `native::*` report no apps; `leds::bootProgress` does nothing; `signStoreRegistration(message)` returns `identity::signBase64(message)` from inside `signer.cpp`; `onAppStopping` calls its (so far empty) listener list. `reason.h` needs `pure/vk_reason.h`: WP01 creates that one pure header and its `.c` (the enum and names from the signing document); WP02 owns the rest of `pure/`.

- [ ] Copy upstream into `firmware/solana-os/`; commit it unmodified first (one commit, so the hooks are a reviewable diff).
- [ ] Write `registry.h` and `service.h` exactly as in the overview. Write `vk.cpp`: `begin()` runs every service's `begin`, then logs `[vk] registries: services=<n> commands=<n> lua=<n> status=<n> domains=<n> routes=<n> permissions=<n> patterns=<n> native=<n> config=<n>`; `update()` runs every service's `update`.
- [ ] Write the stubs. Write `build.sh` and `preflash-check.sh`.
- [ ] Apply hooks H1–H17, H19 and H20 exactly as written. Copy the hook table to `UPSTREAM-HOOKS.md`.
- [ ] `scripts/build.sh dev`. Expected: compiles; the hook check passes.
- [ ] Add a temporary `VK_SERVICE` in a new file that logs once at boot; flash; confirm the line appears (this is the linker check of review focus 5). If it does not appear, add `src/vk/registry_anchor.cpp` per the overview's fallback and record the finding. Remove the temporary service.
- [ ] Flash. Tests: T-BOOT1, `VKHELP` lists `VKHELP` and `VKINFO`, `VKINFO` answers `OK profile=dev api=2`, launch an upstream sample app and exit, the launcher repaints after a pushed app install, T-HOOK1 if a second badge is available.

**Done when:** the fork boots and behaves as upstream; all hooks are present and checked; the registries line is logged.

### WP02: Pure code and host tests

**Goal:** every host-testable module exists and is green, with no badge.
**Read:** [solana-payments](../wallet/solana-payments.md), [checks](../wallet/checks.md), [protocol](../protocol/espnow.md) (Frames, Codec), [signing](../wallet/signing.md) (Reason codes), [testing](../testing/testing.md#host-tests).
**Files:**
- Create: `src/vk/wallet/pure/sol.h`, `sol_b58.c`, `sol_sha256.c`, `sol_tx.c` (copied from `docs/os/reference/code/`, then changed), `vk_record.{h,c}`, `vk_frames.{h,c}`, `vk_checks.{h,c}` (`vk_reason.{h,c}` already exists from WP01; if WP02 runs first it creates it and WP01 keeps it)
- Create: `test/host/run.sh`, `test/host/shim/` (the `Arduino.h`, file and NVS stand-ins described in the testing document), `test_sol.c`, `test_record.c`, `test_frames.c`, `test_checks.c`, `vectors.mjs`, `vectors-to-h.mjs`, `vectors.json`, `vectors.h`, `host_ed25519.c` (wraps upstream `tweetnacl.c` with a stub `randombytes`)

**Interfaces produced:** every declaration in the three spec documents' C blocks, unchanged.

- [ ] In `docs/os/reference/code/`, build and run the reference `test_sol` unchanged to confirm the starting point (`all sol tests passed`; command in the README). Then copy the four files and the test; delete the test's curve and address-derivation cases, which exercise files that are not copied.
- [ ] Extend `vectors.mjs` (Memo message, issuer and device key pairs, canonical record + signature, signed REQ, PROOF); regenerate; commit vectors.
- [ ] Decoder: write the new negative and Memo tests first (one per rule in the decoder table); see them fail; change `sol_tx.c` (legacy only, 1232, Memo, compact-u16, `SOL_TX_ERR_MEMO`, struct change); extend the builder with the memo argument; tests pass.
- [ ] `vk_reason`: enum and names; test that every code has a distinct lower-case name.
- [ ] `vk_record`: tests first (canonical record parses; each mutation refused; signature verify with the test issuer key; one flipped bit fails); implement.
- [ ] `vk_frames`: tests first (round trip per type; truncated and over-long refused; `signed_len`; signed-bytes helpers match the vectors); implement.
- [ ] `vk_checks`: tests first, one per row of both tables in the check chain plus cap and green; implement `vk_check_solana` and `vk_headline_text`.
- [ ] `test/host/run.sh` builds and runs all suites; wire it into `preflash-check.sh` as check 5.

**Done when:** `test/host/run.sh` prints one "all … tests passed" line per suite and exits 0.

### WP03: Dev tools and serial tool

**Goal:** a script can flash, wait for boot, read state, press buttons and take screenshots with no person present.
**Read:** [testing](../testing/testing.md) (Dev hooks, The serial tool), [config](../platform/config.md#serial-commands).
**Files:**
- Fill: `src/vk/features/devtools/devtools.{h,cpp}` (`VKSTATE`, `VKBTN`, `VKSHOT`, `VKTIME`, `VKPAIR`, `VKNOTE`; bodies compiled only when `VK_TEST_HOOKS`). Create: `scripts/vkdev.py`, `test/device/t_boot.py`

**Interfaces consumed:** `VK_SERIAL_COMMAND`; hook H17's `vk_dev_apply_injected_buttons`. `VKSTATE` reads the approval through `vk::wallet::approval::current()`, `phase()` and `peekResult()`, the clock through `vk::clock`, notifications through `vk::host::notify`; all are WP01 stubs until their packages land, so the command is written once and its values become real as packages merge. The same holds for `VKTIME` (calls a dev-only `vk::clock::devSet`) and `VKNOTE`.

- [ ] `vk_dev_apply_injected_buttons`: a queue of timed press/release events applied to the three masks; `VKBTN` fills it.
- [ ] `VKSHOT`: run-length encode `display::canvas()`'s buffer, base64, CRC32.
- [ ] `VKSTATE`: the JSON in the testing document, fields available so far.
- [ ] `vkdev.py`: `info`, `cmd`, `wait-ready`, `state`, `btn`, `shot` (PNG via `zlib`), `push` (upstream `AUTH`/`BEGIN`/`DATA`/`END` using `VKPAIR`), `run`, `monitor`, `test`.
- [ ] `t_boot.py`: T-BOOT1. Then a navigation test: `btn down tap`, `shot`, assert the two screenshots differ.
- [ ] Release build: confirm none of these commands answers (T-REL2).

**Done when:** `vkdev.py --port P test test/device/t_boot.py` passes after an unattended flash.

---

## Phase 1: sign

### WP10: Config store and provisioning

**Read:** [config](../platform/config.md).
**Files:** Fill `src/vk/core/config.{h,cpp}`; create `test/host/test_config.cpp`; add `provision` to `scripts/vkdev.py`; `test/device/t_cfg.py`. Core keys registered here: `listener_url`, `display_name`, `rpc_url`. Status item `setup`.
**Interfaces produced:** everything in `vk::config`; commands `VKINFO`, `VKKEYS`, `VKGET`, `VKSET`, `VKCOMMIT`, `VKRESET`, `VKWIFI`, `VKAUTOSTART`.

- [ ] Host tests first: token-table text parser (valid, bad mint, bad decimals, 5 entries, cap > max), `KEY32` base58, `U32` ranges, `STR` length. Implement the parsers as pure functions in `config.cpp` behind `VK_HOST_TEST`.
- [ ] NVS layer (`Preferences`, namespace `vkconf`); `set` returns `INVALID` if the NVS write fails (review focus 3).
- [ ] Commands, registered from `config.cpp`, and info fields `provisioned`, `wifi`. A secure change on a provisioned badge calls `confirmChange`; while that pointer is null (until WP12) it answers `ERR unavailable`. No edit to this file is needed when WP12 lands.
- [ ] `vkdev.py provision` as specified.
- [ ] Device: T-CFG1, T-CFG3; T-BOOT2.

**Done when:** a badge can be provisioned with one command and the values survive a reboot.

### WP11: Signer and domain table

**Read:** [signing](../wallet/signing.md).
**Files:** Fill `src/vk/wallet/signer.{h,cpp}`; create `signer_internal.h`, `crypto.{h,cpp}`, `features/store_reg/domain_store_reg.cpp`, `test/host/test_domains.cpp`.
**Interfaces produced:** `SignDomain`, `VK_SIGN_DOMAIN`, `findDomain`, `signAuto`, `begin` (returns `VK_UNSUPPORTED` for button domains until WP12 provides `approval::open`), `poll`, `keyLocation`, `maxSignBytes`, `publicKey`, `addressBase58`, `selfCheckOk`, `signStoreRegistration`, `signForApproval`, `presenceLookup` and `tokenInfoLookup` (both null), `vk::wallet::verify`, `vk::wallet::randomBytes`, `vk_verify_c`.

- [ ] Host test first: the self-check function (pure, takes an array of rows) accepts the shipped table and rejects each of the five rule violations and the reserved `registry:` prefix.
- [ ] `signRaw` with the timing log; `signAuto`; `store-reg` validator (copy upstream's `isSafeNonce` character set); `signStoreRegistration` now goes through `signAuto`.
- [ ] The self-check runs from a `VK_SERVICE` begin function in `signer.cpp` (no edit to `vk.cpp`) and logs the result; info fields `pubkey`, `key`, `selfcheck` are registered here.
- [ ] Device: T-BOOT3; boot log shows `selfcheck ok`; with the app store enabled and a broker URL set, registration still succeeds (or is unchanged if no broker is reachable); pre-flash check 2 passes.

**Done when:** one signing path exists, the self-check runs at boot, and nothing else in the tree can sign.

### WP12: Approval engine, screen, LED patterns

**Read:** [approval](../wallet/approval.md), [ui](../ui/ui.md) whole (LED patterns, tokens, fonts, the receipt kit).
**Files:** Fill `src/vk/wallet/approval.{h,cpp}`, `src/vk/ui/leds.{h,cpp}`; fill `src/vk/ui/theme.{h,cpp}` (tokens, `receipt-light`, `receipt-dark`, `blend`, config key `theme`); create `src/vk/ui/receipt.{h,cpp}`, `src/vk/ui/approval_screen.{h,cpp}`, `src/vk/features/devtools/demo_approve.cpp`, `test/host/test_approval.cpp`, `test/device/t_apr.py`. Config keys `approval_tmo_s`, `hold_ms`.
**Interfaces produced:** `ApprovalRequest`, `approval::open/confirm/active/update/phase/current/appStopping`, `VK_ON_APPROVAL`, `VK_LED_PATTERN`, `leds::play/stop/bootProgress`; `takeResult`, `peekResult`; `vk::modalActive()` and `modalUpdate()` now real (they call into the engine through the stub's functions, so `vk.cpp` is not edited); a `VK_ON_APP_STOP` listener calls `approval::appStopping`; `vk::config::confirmChange` is set at boot.

- [ ] Host tests first (fake clock, fake buttons): fresh-press rule; hold released early; hold completes; timeout; red closes with `red_reason`; dev override only when `dev_overridable`; `confirm` path; `appStopping` during each phase (review focus 2); result dropped after 60 s.
- [ ] Theme tokens and both themes; the receipt kit (confirm the font names against the installed LovyanGFX first; apply the stated fallback if one is missing). Engine. Screen drawing per the layout table, in both themes. Result screen.
- [ ] LED patterns and the boot bar; listeners play `signed`/`refused`.
- [ ] Keys-up-before-close, the backlight save and restore, and the shell repaint request, as specified.
- [ ] `features/devtools/demo_approve.cpp`: the dev-only command `VKDEMOAPPROVE <green|amber|red>`, which calls `approval::confirm` with a sample request, so the screen can be tested before any domain exists.
- [ ] Device (`t_apr.py`): open each severity, assert `VKSTATE`, screenshot each for the record; T-APR2, T-APR3; `STOP` and `RUN` over serial while modal (applied only after it closes; T-REQ6's serial half); the launcher repaints after a confirmation closes over it; config secure-change confirmation now works (T-CFG2).
- [ ] Hands: T-LED1, T-LED2.

**Done when:** any firmware code can raise an approval with a filled struct and get a yes/no, and the engine's rules are host-tested.

### WP13: Solana payments, Lua wallet, Sign test

**Read:** [solana-payments](../wallet/solana-payments.md), [checks](../wallet/checks.md) (Verdict to screen), [Lua API](../platform/lua-api.md), [apps](../apps/apps.md#sign-test), [backend](../integration/backend.md#existing-routes).
**Files:** Create `src/vk/features/solana_pay/domain_solana.cpp`, `lua_solana.cpp`; `src/vk/wallet/lua_wallet.cpp` (identity functions, `begin`, `poll`, `config`, `tokens`, `badge.codec`); `apps/signtest/{app.ini,main.lua,config.lua}`; `test/device/t_sign.py`. Config keys `issuer_key`, `tokens`, `record_ttl_s`.
**Interfaces consumed:** `vk_check_solana`, `approval::open`, `signer::begin/poll`, config.

- [ ] `decodeSolana`: fill `vk_check_input_t`, call the chain, map the verdict to an `ApprovalRequest` per the table. At this stage a record is rarely supplied; the path with one is exercised in WP21.
- [ ] `vk::wallet::begin` now opens the approval for button domains.
- [ ] Lua bindings; each calls `runtime::extendDeadline` as specified.
- [ ] Sign test app (it carries its own minimal JSON field extraction until WP35; replace with `vk` then).
- [ ] Device (`t_sign.py`), dashboard running with the listener open: T-APR1 (the signature verifies on the laptop with the badge's public key; the transaction confirms on devnet), T-APR4, T-APR5.

**Done when (Gate 1):** a transfer served by the laptop is shown with the correct amount on the firmware screen, signed after the hold, and confirmed on devnet; each refusal vector is refused.

---

## Phase 2: trust

### WP20: Clock

**Read:** [checks](../wallet/checks.md#clock).
**Files:** Fill `src/vk/core/clock.{h,cpp}`. Config key `ntp_server`. Info field `time`. Dev-only `vk::clock::devSet(unix)` for `VKTIME`.

- [ ] Service: start SNTP once when Wi-Fi is first connected; never wait for it (review focus 4). Sync callback, or the polling fallback.
- [ ] `raiseTo`. `VKINFO` reports `time=`.
- [ ] Device: with the hotspot up, `time=sntp` within 10 s of Wi-Fi connecting; with Wi-Fi off, boot reaches `[os] ready` in the usual time and `time=none`.

### WP21: Record checks wired into the approval

**Read:** [checks](../wallet/checks.md) whole.
**Files:** Modify `features/solana_pay/domain_solana.cpp`, `lua_solana.cpp` (`check_record`); create `apps/checktest/` (dev-only test app that calls `begin_solana` with prepared `ctx` variants fetched from the laptop), `test/device/t_chk.py`.
**Needs:** the backend's `GET /registry/<address>` ([backend](../integration/backend.md#needed-routes)). Until it exists, `t_chk.py` serves records itself: a 40-line Python HTTP server in the test that signs records with the test issuer key from `vectors.json`, with the badge provisioned to that issuer.

- [ ] Record verification, `clock::raiseTo`, and the full verdict mapping. A supplied request is verified here too (checks 11 and 12 do not need the `requests` feature); only presence is absent until WP23, so the best verdict in this package is amber.
- [ ] Device: T-CHK2 to T-CHK9; the app is still running after a `begin_solana` with a full `ctx` (review focus 1).

### WP22: ESP-NOW router

**Read:** [protocol](../protocol/espnow.md) (Frame header, Type registry, Router).
**Files:** Fill `src/vk/host/router.{h,cpp}`.

- [ ] `install`, route lookup, forwarding rules 1–3, `send`. `granted("espnow")` is consulted (always true until WP30).
- [ ] Device: T-HOOK1; an upstream ESP-NOW sample app (`whosnear`) still works between two badges.

### WP23: Requests and presence

**Read:** [protocol](../protocol/espnow.md) from "Payment requests" on; [Lua API](../platform/lua-api.md#badgewallet-requests).
**Files:** Create `src/vk/features/requests/domain_pay_req.cpp`, `domain_pay_proof.cpp`, `requests.{h,cpp}`, `presence.{h,cpp}`, `lua_requests.cpp`; `apps/reqtest/` (dev-only fixture: opens a request, logs RESULT frames); `test/device/t_req.py` (two ports). The request cache posts its notification through `vk::host::notify::post` (a stub until WP32). Config keys `presence_ms`, `req_ttl_s`, `req_period_ms`, `req_max_proofs`, `req_gap_ms`, `pay_app`. Permission `request`.
**Interfaces produced:** sets `vk::wallet::presenceLookup`; Lua `request_open/close/status`, `requests`, `challenge`, `presence`.

- [ ] Payee: active-request table, signing, rebroadcast service, CHAL route with rate limits, close on app stop (a `VK_ON_APP_STOP` listener).
- [ ] Payer: request cache route, presence slots, PROOF route with `lastRxMs`, log line `[req] proof <ms> ms`.
- [ ] Device: T-REQ1 to T-REQ4, T-CHK1.

**Done when (Gate 2):** on two badges, an honest request is green; a replayed request is amber; a transaction to the wrong account or for the wrong amount is red; an unregistered payee is red.

### WP24: History

**Read:** [stores](../wallet/stores.md).
**Files:** Create `src/vk/features/history/history.{h,cpp}`, `lua_history.cpp`; `test/host/test_stores.cpp`. Permission `history`.

- [ ] Host tests first: ring wrap at 128, bad magic recovery, a write that fails leaves the previous file intact and returns false.
- [ ] Listener writes one record per outcome; a failed write is logged and ignored (review focus 3). Creates `/vk/` if missing.
- [ ] Device: T-STO1, T-STO2.

---

## Phase 3: platform

### WP30: Permissions, consent, API version

**Read:** [app host](../platform/app-host.md) (Manifest, Permissions, Consent, API version, Lua function registry).
**Files:** Fill `src/vk/host/permissions.{h,cpp}`, `lua_registry.cpp` (filtering, stubs); fill `host/manifest.cpp`, `host/consent.cpp`; `test/host/test_manifest.cpp`, `test_consent.cpp`; `test/device/t_app.py`. Add `permissions=` lines to upstream's sample apps and to `signtest`, `checktest`.

- [ ] Host tests first: manifest parser; permission-list hash is order-independent; consent store round trip.
- [ ] `preLaunch`: unknown permission, id length, native-id collision, `min_api`, consent confirmation and relaunch; the pending and active grant slots exactly as specified (a `VK_ON_APP_STOP` listener clears the active slot).
- [ ] `vk::lua::open` filtering: stubs for ungranted functions; error tables for ungranted upstream modules.
- [ ] Device: T-APP1 to T-APP4, T-APP7.

### WP31: Native runtime and SDK

**Read:** [native apps](../platform/native-apps.md), [app host](../platform/app-host.md#native-runtime).
**Files:** Fill `src/vk/host/native.{h,cpp}`; create `src/native_apps/hello_native/hello_native.cpp`.

- [ ] Registry lookup, `infoAt`/`infoById`, start/stop with `new`/`delete`, dispatch, `badge::exit`, permission list for `granted()`.
- [ ] Device: T-APP5, T-APP6 (a second tiny native test app without `sign`, removed afterwards); the launcher lists it after the Lua apps; RIGHT (delete) on it shows upstream's failure message and removes nothing.

### WP32: Notifications, status items, Inbox

**Read:** [app host](../platform/app-host.md#notifications), [ui](../ui/ui.md#status-bar).
**Files:** Fill `src/vk/host/notify.{h,cpp}` (with status item `inbox`, LED pattern `notify`); create `src/vk/ui/status_dev.cpp` (status item `dev`), `src/native_apps/inbox/inbox.cpp`. No other package's file is edited: the requests feature already calls `notify::post`.

- [ ] Device: T-REQ5; `VKNOTE` posts; the bar shows `[1]`; Inbox opens the named app.

### WP33: Balance

**Read:** [ui](../ui/ui.md#balance).
**Files:** Create `src/vk/features/balance/balance.{h,cpp}`, `lua_balance.cpp`. Config key `balance_poll_s`. Status item `balance`.

- [ ] Service with the stated conditions; reply scanning; 3 s timeout; skip when Wi-Fi is down (review focus 4).
- [ ] Device: after `devnet:setup` funded the badge, the bar shows the balance within one poll; `wallet.token_account()` equals the account `devnet:setup` created; log `[bal] fetch <ms> ms`.

### WP34: Contacts feature

**Read:** [protocol](../protocol/espnow.md) (CONTACT frames), [stores](../wallet/stores.md#contacts), [Lua API](../platform/lua-api.md#badgewallet-contacts).
**Files:** Create `src/vk/features/contacts/domain_contact.cpp`, `contacts.{h,cpp}`, `lua_contacts.cpp`; `test/host/test_contacts.cpp`. Permission `contacts`.

- [ ] Host tests first: store; card signed bytes; accept refuses wrong nonce, wrong addressee, bad signature.
- [ ] Nonce lifetime and rotation; upsert; notification "Saved `<name>`".

### WP35: `lib/vk.lua`

**Read:** [Lua API](../platform/lua-api.md#libvklua).
**Files:** Create `lib/vk.lua`, `scripts/push-apps.sh`, `test/device/t_vk.py` with a test app `apps/vktest/`.

- [ ] JSON decode/encode with a test table run on the laptop under stock `lua5.4` if installed, and on the badge by `vktest`.
- [ ] `vk.ui` (the receipt look for Lua; needs `badge.theme` from WP37, falling back to the light colours when that module is absent). RPC helpers; `vk.record`; `vk.feed`; frame helpers including `vk.result_parse` and `vk.hello_parse`; `vk.pay` state machine. Every network helper returns `nil, message` on failure and `vk.pay` ends in `failed` (review focus 4): test with Wi-Fi off.
- [ ] Replace Sign test's private JSON code with `vk`.

### WP36: Wallet settings app

**Read:** [apps](../apps/apps.md#wallet-native).
**Files:** Create `src/native_apps/wallet_settings/wallet_settings.cpp`.

### WP37: Receipt launcher, settings app, home service, boot screen

**Read:** [ui](../ui/ui.md#theme) to the end; [apps](../apps/apps.md#launcher-native); the simulation in `docs/design/os-mockups/`.
**Files:** Create `src/native_apps/launcher/launcher.cpp`, `src/native_apps/settings/settings.cpp`, `src/vk/host/home.{h,cpp}` (config key `home_app`, `showShell()`), `src/vk/ui/boot_screen.cpp` (replaces the WP01 stub of `vk::ui::bootScreen`), `src/vk/ui/lua_theme.cpp` (`badge.theme.*`); restyle `inbox` and `wallet_settings` with the receipt kit if they predate it.

- [ ] Launcher grid, navigation, balance row, barcode. Settings list with the Theme toggle.
- [ ] Home service with the four conditions; `showShell()`; verify: an app that exits, errors or is force-quit returns to the launcher (after the error screen is dismissed); System settings reaches upstream's screens; launching from upstream's launcher returns to ours afterwards.
- [ ] Boot screen and LED bar: screenshots at each stage compared with the simulation; T-LED1 by a person.
- [ ] Screenshots of launcher, settings, approval (three severities), boot in both themes kept in `test/device/shots/`.

**Done when (Gate 3):** T-APP1–T-APP7, T-REQ5 pass; the Wallet app shows the provisioned values.

---

## Phase 4: apps

Each app is one folder under `apps/` with `app.ini`, `main.lua`, `config.lua`, written against [apps](../apps/apps.md) and the [Lua API](../platform/lua-api.md), and drawn only with `vk.ui` so it matches the Receipt design in both themes (compare with the simulation in `docs/design/os-mockups/`). Each package ends with a scripted device test in `test/device/` that drives the app with `VKBTN` and asserts on `VKSTATE` and screenshots, plus the two-badge checks named.

### WP40: Home
- [ ] Files `apps/home/`. Shows name, address, key location, balances; menu from `config.lua`. Provisioning sets it as the autostart app (`vkdev.py provision --autostart home`, using upstream's setting).

### WP41: Pay and Request
- [ ] Files `apps/pay/`, `apps/request/`. Two badges: request 10.00 → listed → paid → both show the result; green approval; RESULT confirmed on chain; a refusal is reported with `vk.report`; from the launcher, the Inbox entry for a request opens Pay.

### WP42: History app
- [ ] Files `apps/history/`.

### WP43: Contacts app
- [ ] Files `apps/contacts/`. Two badges: T-CON1, T-CON2.

### WP44: Game and Evil game
- [ ] Files `apps/game/`, `apps/evilgame/{app.ini,config.lua}`. Shop purchase is amber with the true amount; `evil = "amount"` shows 500.00 on the firmware screen while the game shows 5.00; `evil = "recipient"` is red.

### WP45: Duel
- [ ] Files `apps/duel/`. Two badges: invite, play, settle through the request flow; "unpaid" when the loser cancels.

**Done when (Gate 4):** the demo script runs on development badges: honest payment, impostor, replay, evil game, revoked merchant.

---

## Phase 5: release

### WP50: SE050 bring-up
**Read:** [signing](../wallet/signing.md#key), [build guide](../guides/build-flash-provision.md#se050-fallback).
- [ ] On a badge with `key=se050`: T-SE1, T-SE2. Record M2 for the SE050.
- [ ] If T-SE1 fails: capture the `se050` log (stage and status word), apply the fallback (H18, `VK_FORCE_SOFTWARE_KEY 1`) for that badge, and record it. One hour at most on diagnosis.

### WP51: Measurements and tuning
**Read:** [testing](../testing/testing.md#measurements).
- [ ] Record M1–M6. Set `presence_ms` from M1 on every badge. If M2's verify exceeds 400 ms, vendor Monocypher 4.0.2 into `src/vk/wallet/vendor/` and set `VK_ED25519_BACKEND 1`; rerun `test/host/run.sh` and M1. If M6 shows under 1 KB of stack free, add the loop-stack hook (new hook id) and rerun.

### WP52: Release
- [ ] `scripts/build.sh release --upload` on four badges; provision; fund; push the release app set; issue attestations; label.
- [ ] T-REL1, T-REL2, T-REL3, T-APR6. T-REL1 is the release gate.

### WP54: Bank rail (optional)
**Read:** [checks](../wallet/checks.md#bank-rail). Files `src/vk/features/bank/`. Needs the backend's `/balance` and `/bank/authorize`.

---

## How a package is executed

1. Read [../README.md](../README.md), [overview](../architecture/overview.md), and the documents under the package's **Read**.
2. Create the branch `wp<NN>-<slug>`.
3. Write the host tests named in the package; run them; see them fail.
4. Implement in the files the package owns. Do not edit another package's files; if an interface in the spec is wrong or missing, fix the spec document first in the same branch and say so in the commit.
5. `scripts/build.sh dev --upload <port>`, `vkdev.py wait-ready`, run the package's device tests.
6. Update the spec if hardware disagreed with it (remove the `[UNVERIFIED]` tag or apply the stated fallback and record which).
7. Record results in the tracking table below. Merge in dependency order.

## Tracking

| WP | Owner | Started | Tests passed | Notes |
|---|---|---|---|---|
| 00 | | | | |
| 01 | | | | |
| 02 | | | | |
| 03 | | | | |
| 10 | | | | |
| 11 | | | | |
| 12 | | | | |
| 13 | | | Gate 1 | |
| 20 | | | | |
| 21 | | | | |
| 22 | | | | |
| 23 | | | Gate 2 | |
| 24 | | | | |
| 30 | | | | |
| 31 | | | | |
| 32 | | | | |
| 33 | | | | |
| 34 | | | | |
| 35 | | | | |
| 36 | | | | |
| 37 | | | Gate 3 | |
| 40 | | | | |
| 41 | | | | |
| 42 | | | | |
| 43 | | | | |
| 44 | | | | |
| 45 | | | Gate 4 | |
| 50 | | | | |
| 51 | | | | |
| 52 | | | Release | |
| 54 | | | optional | |
