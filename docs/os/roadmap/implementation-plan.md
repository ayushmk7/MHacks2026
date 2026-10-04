# BadgeOS Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan work package by work package. Steps use checkbox (`- [ ]`) syntax for tracking. Each work package is a sub-project: before coding, its executor reads the documents listed under **Read**, then writes the tests named under **Tests** first.

**Goal:** Turn Solana OS into BadgeOS: a badge that signs payments only through a firmware approval screen, verifies who is being paid, and hosts Lua and native apps on a platform where adding or removing a feature is one file or one folder.

**Architecture:** A fork of Solana OS at `os/`, changed only by marked one-line hooks. All new code is under `src/vk/`: core services, a wallet core with one signing path and a table of signing domains, a generic approval engine that pauses apps while the user decides, an app host (permissions, native apps, ESP-NOW router, notifications), and self-contained feature folders that register themselves at start-up.

**Tech stack:** Arduino-ESP32 3.x built with `arduino-cli`; C++17 for firmware glue; C99 for the host-tested pure code; Lua 5.4 apps; Python 3 + pyserial for the serial tool; Node (the dashboard's packages) for test vectors.

**Spec:** this folder, `docs/os/`. Start at [../README.md](../README.md). Where `docs/specs/` differs, `docs/os/` wins ([differences](../reference/differences-from-specs.md)).

## Global constraints

- Upstream base: `firmware/solana-os/` of <https://github.com/spacemandev-git/solana-defcon-badge-26> at commit `812b8c7aca5c366d18c0b040fafd2999f7204d84`.
- Every edit to an upstream file is a hook listed in [upstream-hooks.md](../architecture/upstream-hooks.md), tagged `// VK: H<n>`. All hooks (H1–H17, H19, H20, and the provisional H21) are applied in WP01 (H21 was moved from `os.ino` into three `src/hal/` functions in Batch 2); later work packages do not touch upstream files (exceptions are named in the package). **WP37 is the exception by design:** it replaces upstream's shell, boot splash and names; from then on an upstream file is untouched, carries tagged hook lines (H14, H15 and H20 are retired, H23 is new), or is listed under [Replaced upstream files](../architecture/upstream-hooks.md#replaced-upstream-files).
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

1. **Slow crypto inside a Lua callback.** `begin_solana` with a full `ctx` verifies a record and a request (measured: 419 ms each with TweetNaCl, 854 ms to the first draw) inside one 250 ms callback. Expected: the binding extends the deadline and the app survives. Test in WP21 (device: call with full ctx, app still running afterwards).
2. **App stopped while an approval is open.** Hook H4 skips `routeButtons()` and defers launch and stop requests while the approval is active, so a force-quit (hold CANCEL) cannot happen and a pushed `RUN` or `STOP` is applied on the first pass after the approval closes. The only stop during an approval is a pushed `DEL <running id>` (upstream calls `runtime::stop()` directly). Expected: the approval closes, no result lingers, the next `begin` from another app is not `busy`. Test in WP12 (host: `appStopping`; device: `DEL` of the running app while `modal` is true closes the approval; an app stopped with an un-polled result leaves the next `begin` not `busy`).
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
| 32 | Notifications, Inbox (the status items it first shipped are removed by WP37) | 23, 31 | yes | 33, 35 |
| 33 | Balance | 10 | yes | 30–32, 34, 35 |
| 34 | Contacts feature | 10, 11, 20, 22, 32 | yes (2) | 33, 35 |
| 35 | `lib/vk.lua` | 13, 23, 33 | yes | 30–32, 34 |
| 36 | Wallet settings app | 20, 24, 30, 31, 33 | yes | 4x |
| 37 | BadgeOS shell rewrite and rebrand: boot screen, launcher, settings pages, dialogs; upstream's shell, splash, sample apps and names removed | 12, 31, 32, 33, 36 | yes | 40–45 |
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

**Corrections to the dependency table** (from the [execution plan](execution-plan.md), section 1; where they differ from the table or from a package below, these win):

- WP01 needs WP02's headers: `core/config.h` uses `vk_token_t` and `VK_MAX_TOKENS`, and `signer.h` uses `vk_presence_t`, all from `pure/vk_checks.h`, which includes `sol.h`, `vk_record.h` and `vk_frames.h`. WP01 and WP02 are written in one batch and integrated together.
- WP10, WP11 and WP12 are listed as a chain. With the headers of the execution plan's section 4 frozen they are written in parallel and integrated together.
- WP13's step "`vk::wallet::begin` now opens the approval" edits `signer.cpp`, a WP11 file. It moves to WP11.
- WP23 lists WP21 and WP34 lists WP32 only for their gates and for `notify::post`, which is a stub from WP01. Neither is needed to compile.
- WP36 does not use WP24.
- "One branch per work package" is replaced by one branch (`badge-os`) and one commit per batch by the integrator.

File ownership is disjoint between packages that may run in parallel: each package creates or fills only the files listed under **Files**. Stubs created in WP01 are filled in by the package that owns that file.

If time runs out, cut in this order: WP54, WP45, WP43 + WP34, WP36, WP32 inbox (keep the notification count on the launcher), WP33. Inside WP37, cut settings pages before anything else (each is one file; the shell works with any subset), never the launcher, the boot screen or the rebrand. Gates 1 and 2 are never cut.

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

**Goal:** `os/` exists, contains the `src/vk/` skeleton and every hook, builds, and behaves exactly like upstream.
**Read:** [overview](../architecture/overview.md), [upstream-hooks](../architecture/upstream-hooks.md), [upstream-baseline](../architecture/upstream-baseline.md).
**Files:**
- Create: `os/` (copy of upstream), `UPSTREAM-HOOKS.md`, `.gitignore` entry for `src/vk/vk_profile.h`
- Create: every header in the table of the [execution plan](execution-plan.md), section 4 ("Stub contract"), with its `.cpp` where the table gives a stub body. That table replaces the list that stood here: it names each header, the spec block it is copied from, the declarations added because no document had them, and the stub body until the owning package fills it. Beyond the earlier list it includes `core/fileio.{h,cpp}`, `wallet/signer_internal.h`, `wallet/crypto.h`, `ui/receipt.h`, `ui/approval_screen.h`, `ui/boot_screen.cpp` (stub of `vk::ui::bootScreen`), `host/home.{h,cpp}`, `vk::clock::devSet` and the `InfoField` registry behind `VK_INFO_FIELD`. A later package that is listed as creating one of these headers fills it instead.
- Needs from WP02, written in the same batch: all of `src/vk/wallet/pure/` (`sol.h`, `vk_reason.h`, `vk_record.h`, `vk_frames.h`, `vk_checks.h`). `core/config.h` and `wallet/signer.h` include `pure/vk_checks.h`; `wallet/reason.h` includes `pure/vk_reason.h`.
- Create: `scripts/build.sh`, `scripts/preflash-check.sh` (checks 1–4 and 6; check 5 is added when `test/host/run.sh` lands)
- Modify: the upstream files named in hooks H1–H17, H19 and H20 (H18 only in WP50)

**Interfaces produced:** `vk::begin()`, `vk::update()`, `vk::modalActive()`, `vk::modalUpdate()`; `vk::Registered<T>`; `VK_SERVICE`; `vk::serial::handleLine`, `VK_SERIAL_COMMAND`; `VK_LUA_FUNCTION`, `vk::lua::open`; `VK_STATUS_ITEM`, `vk::ui::statusbar::draw`; the stub signatures exactly as in the spec so later packages only replace bodies.

Stub behaviour (must equal upstream behaviour) is the last column of that table. In short: `modalActive()` is false because `approval::active()` is; `router::install()` installs `espnow_mgr::onReceive([](const uint8_t *m, const uint8_t *d, size_t n, int8_t r) { if (runtime::running()) runtime::dispatchEspnow(m, d, n, r); })`; `preLaunch` returns true; `native::*` report no apps; `leds::bootProgress` does nothing; `bootScreen` returns false; `signStoreRegistration(message)` returns `identity::signBase64(message)` from inside `signer.cpp`; `onAppStopping` calls its (so far empty) listener list.

- [x] Copy upstream into `os/`; commit it unmodified first (one commit, so the hooks are a reviewable diff).
- [x] Write `registry.h` and `service.h` exactly as in the overview. Write `vk.cpp`: `begin()` runs every service's `begin`, then logs `[vk] registries: services=<n> commands=<n> lua=<n> status=<n> domains=<n> routes=<n> permissions=<n> patterns=<n> native=<n> config=<n>`; `update()` runs every service's `update`.
- [x] Write the stubs. Write `build.sh` and `preflash-check.sh`.
- [x] Apply hooks H1–H17, H19, H20 and H21 exactly as written. Copy the hook table to `UPSTREAM-HOOKS.md`.
- [x] `scripts/build.sh dev`. Expected: compiles; the hook check passes.
- [x] Add a temporary `VK_SERVICE` in a new file that logs once at boot; flash; confirm the line appears (this is the linker check of review focus 5). If it does not appear, add `src/vk/registry_anchor.cpp` per the overview's fallback and record the finding. Remove the temporary service.
- [x] Flash. Tests: T-BOOT1, `VKHELP` lists `VKHELP` and `VKINFO`, `VKINFO` answers `OK profile=dev api=2`, launch an upstream sample app and exit, the launcher repaints after a pushed app install, T-HOOK1 if a second badge is available. Done 2026-10-03 except T-HOOK1, which is deferred (needs a second badge).

**Done when:** the fork boots and behaves as upstream; all hooks are present and checked; the registries line is logged.

### WP02: Pure code and host tests

**Goal:** every host-testable module exists and is green, with no badge.
**Read:** [solana-payments](../wallet/solana-payments.md), [checks](../wallet/checks.md), [protocol](../protocol/espnow.md) (Frames, Codec), [signing](../wallet/signing.md) (Reason codes), [testing](../testing/testing.md#host-tests).
**Files:**
- Create: `src/vk/wallet/pure/sol.h`, `sol_b58.c`, `sol_sha256.c`, `sol_tx.c` (copied from `docs/os/reference/code/`, then changed), `vk_record.{h,c}`, `vk_frames.{h,c}`, `vk_reason.{h,c}`, `vk_checks.{h,c}` (WP02 owns all of `pure/`; WP01's headers include these, so the two packages are written in one batch)
- Create: `test/host/run.sh`, `test/host/shim/` (the `Arduino.h`, file and NVS stand-ins described in the testing document), `test_sol.c`, `test_record.c`, `test_frames.c`, `test_checks.c`, `vectors.mjs`, `vectors-to-h.mjs`, `vectors.json`, `vectors.h`, `host_ed25519.c` (wraps upstream `tweetnacl.c` with a stub `randombytes`)

**Interfaces produced:** every declaration in the three spec documents' C blocks, unchanged.

- [x] In `docs/os/reference/code/`, build and run the reference `test_sol` unchanged to confirm the starting point (`all sol tests passed`; command in the README). Then copy the four files and the test; delete the test's curve and address-derivation cases, which exercise files that are not copied.
- [x] Extend `vectors.mjs` (Memo message, issuer and device key pairs, canonical record + signature, signed REQ, PROOF); regenerate; commit vectors.
- [x] Decoder: write the new negative and Memo tests first (one per rule in the decoder table); see them fail; change `sol_tx.c` (legacy only, 1232, Memo, compact-u16, `SOL_TX_ERR_MEMO`, struct change); extend the builder with the memo argument; tests pass.
- [x] `vk_reason`: enum and names; test that every code has a distinct lower-case name.
- [x] `vk_record`: tests first (canonical record parses; each mutation refused; signature verify with the test issuer key; one flipped bit fails); implement.
- [x] `vk_frames`: tests first (round trip per type; truncated and over-long refused; `signed_len`; signed-bytes helpers match the vectors); implement.
- [x] `vk_checks`: tests first, one per row of both tables in the check chain plus cap and green; implement `vk_check_solana` and `vk_headline_text`.
- [x] `test/host/run.sh` builds and runs all suites; wire it into `preflash-check.sh` as check 5.

**Done when:** `test/host/run.sh` prints one "all … tests passed" line per suite and exits 0.

### WP03: Dev tools and serial tool

**Goal:** a script can flash, wait for boot, read state, press buttons and take screenshots with no person present.
**Read:** [testing](../testing/testing.md) (Dev hooks, The serial tool), [config](../platform/config.md#serial-commands).
**Files:**
- Fill: `src/vk/features/devtools/devtools.{h,cpp}` (`VKSTATE`, `VKBTN`, `VKSHOT`, `VKTIME`, `VKPAIR`, `VKNOTE`; bodies compiled only when `VK_TEST_HOOKS`). Create: `scripts/vkdev.py`, `test/device/t_boot.py`

**Interfaces consumed:** `VK_SERIAL_COMMAND`; hook H17's `vk_dev_apply_injected_buttons`. `VKSTATE` reads the approval through `vk::wallet::approval::current()`, `phase()` and `peekResult()`, the clock through `vk::clock`, notifications through `vk::host::notify`; all are WP01 stubs until their packages land, so the command is written once and its values become real as packages merge. The same holds for `VKTIME` (calls a dev-only `vk::clock::devSet`) and `VKNOTE`.

- [x] `vk_dev_apply_injected_buttons`: a queue of timed press/release events applied to the three masks; `VKBTN` fills it.
- [x] `VKSHOT`: run-length encode `display::canvas()`'s buffer, base64, CRC32.
- [x] `VKSTATE`: the JSON in the testing document, fields available so far.
- [x] `vkdev.py`: `info`, `cmd`, `wait-ready`, `state`, `btn`, `shot` (PNG via `zlib`), `push` (upstream `AUTH`/`BEGIN`/`DATA`/`END` using `VKPAIR`), `run`, `monitor`, `test`.
- [x] `t_boot.py`: T-BOOT1. Then a navigation test: `btn down tap`, `shot`, assert the two screenshots differ.
- [ ] Release build: confirm none of these commands answers (T-REL2). Deferred to Batch 6 (needs the release build).

**Done when:** `vkdev.py --port P test test/device/t_boot.py` passes after an unattended flash.

---

## Phase 1: sign

### WP10: Config store and provisioning

**Read:** [config](../platform/config.md).
**Files:** Fill `src/vk/core/config.{h,cpp}`; create `test/host/test_config.cpp`; add `provision` to `scripts/vkdev.py`; `test/device/t_cfg.py`. Core keys registered here: `listener_url`, `display_name`, `rpc_url`. Status item `setup`.
**Interfaces produced:** everything in `vk::config`; commands `VKINFO`, `VKKEYS`, `VKGET`, `VKSET`, `VKCOMMIT`, `VKRESET`, `VKWIFI`, `VKAUTOSTART`.

- [x] Host tests first: token-table text parser (valid, bad mint, bad decimals, 5 entries, cap > max), `KEY32` base58, `U32` ranges, `STR` length. Implement the parsers as pure functions in `config.cpp` behind `VK_HOST_TEST`.
- [x] NVS layer (`Preferences`, namespace `vkconf`); `set` returns `STORAGE` (`VKSET` answers `ERR nvs_full`) if the NVS write fails (review focus 3).
- [x] Commands, registered from `config.cpp`, and info fields `provisioned`, `wifi`. A secure change on a provisioned badge calls `confirmChange`; while that pointer is null (until WP12) it answers `ERR unavailable`. No edit to this file is needed when WP12 lands.
- [x] `vkdev.py provision` as specified (written in WP03; not yet run against a real `dashboard/.env`, whose `HACK_MINT` is empty).
- [x] Device: T-CFG1, T-CFG3; T-BOOT2. `tokens` and `issuer_key` are registered by `solana_pay` (WP13), so until then `VKSET tokens garbage` answers `ERR unknown_key`: at WP10, T-CFG3 runs against `rpc_url` (`VKSET rpc_url x` → `ERR invalid`); from WP13 on it runs against `tokens`. `t_cfg.py` reads `VKKEYS` and picks accordingly.

**Done when:** a badge can be provisioned with one command and the values survive a reboot.

### WP11: Signer and domain table

**Read:** [signing](../wallet/signing.md).
**Files:** Fill `src/vk/wallet/signer.{h,cpp}`; create `signer_internal.h`, `crypto.{h,cpp}`, `features/store_reg/domain_store_reg.cpp`, `test/host/test_domains.cpp`.
**Interfaces produced:** `SignDomain`, `VK_SIGN_DOMAIN`, `findDomain`, `signAuto`, `begin` (complete: it decodes and calls `approval::open`), `poll`, `keyLocation`, `maxSignBytes`, `publicKey`, `addressBase58`, `selfCheckOk`, `signStoreRegistration`, `signForApproval`, `presenceLookup` and `tokenInfoLookup` (both null), `vk::wallet::verify`, `vk::wallet::randomBytes`, `vk_verify_c`.

- [x] Host test first: the self-check function (pure, takes an array of rows) accepts the shipped table and rejects each of the five rule violations and the reserved `registry:` prefix.
- [x] `signRaw` with the timing log; `signAuto`; `store-reg` validator (copy upstream's `isSafeNonce` character set); `signStoreRegistration` now goes through `signAuto`.
- [x] The self-check runs from a `VK_SERVICE` begin function in `signer.cpp` (no edit to `vk.cpp`) and logs the result; info fields `pubkey`, `key`, `selfcheck` are registered here.
- [x] Device: T-BOOT3; boot log shows `selfcheck ok`; pre-flash check 2 passes. Deferred (network): with the app store enabled and a broker URL set, registration still succeeds.

**Done when:** one signing path exists, the self-check runs at boot, and nothing else in the tree can sign.

### WP12: Approval engine, screen, LED patterns

**Read:** [approval](../wallet/approval.md), [ui](../ui/ui.md) whole (LED patterns, tokens, fonts, the receipt kit).
**Files:** Fill `src/vk/wallet/approval.{h,cpp}`, `src/vk/ui/leds.{h,cpp}`; fill `src/vk/ui/theme.{h,cpp}` (tokens, `receipt-light`, `receipt-dark`, `blend`, config key `theme`); create `src/vk/ui/receipt.{h,cpp}`, `src/vk/ui/approval_screen.{h,cpp}`, `src/vk/features/devtools/demo_approve.cpp`, `test/host/test_approval.cpp`, `test/device/t_apr.py`. Config keys `approval_tmo_s`, `hold_ms`.
**Interfaces produced:** `ApprovalRequest`, `approval::open/confirm/active/update/phase/current/appStopping`, `VK_ON_APPROVAL`, `VK_LED_PATTERN`, `leds::play/stop/bootProgress`; `takeResult`, `peekResult`; `vk::modalActive()` and `modalUpdate()` now real (they call into the engine through the stub's functions, so `vk.cpp` is not edited); a `VK_ON_APP_STOP` listener calls `approval::appStopping`; `vk::config::confirmChange` is set at boot.

- [x] Host tests first (fake clock, fake buttons): fresh-press rule; hold released early; hold completes; timeout; red closes with `red_reason`; dev override only when `dev_overridable`; `confirm` path; `appStopping` during each phase (review focus 2); result dropped after 60 s.
- [x] Theme tokens and both themes; the receipt kit (confirm the font names against the installed LovyanGFX first; apply the stated fallback if one is missing). Engine. Screen drawing per the layout table, in both themes. Result screen.
- [x] LED patterns and the boot bar; listeners play `signed`/`refused`.
- [x] Keys-up-before-close, the backlight save and restore, and the shell repaint request, as specified.
- [x] `features/devtools/demo_approve.cpp`: the dev-only command `VKDEMOAPPROVE <green|amber|red>`, which calls `approval::confirm` with a sample request, so the screen can be tested before any domain exists.
- [x] Device (`t_apr.py`): open each severity, assert `VKSTATE`, screenshot each for the record; T-APR2, T-APR3; `STOP` and `RUN` over serial while modal (applied only after it closes; T-REQ6's serial half); the launcher repaints after a confirmation closes over it; config secure-change confirmation now works (T-CFG2).
- [ ] Hands: T-LED1, T-LED2.

**Done when:** any firmware code can raise an approval with a filled struct and get a yes/no, and the engine's rules are host-tested.

### WP13: Solana payments, Lua wallet, Sign test

**Read:** [solana-payments](../wallet/solana-payments.md), [checks](../wallet/checks.md) (Verdict to screen), [Lua API](../platform/lua-api.md), [apps](../apps/apps.md#sign-test), [backend](../integration/backend.md#existing-routes).
**Files:** Create `src/vk/features/solana_pay/domain_solana.cpp`, `lua_solana.cpp`; `src/vk/wallet/lua_wallet.cpp` (identity functions, `begin`, `poll`, `config`, `tokens`, `badge.codec`); `apps/signtest/{app.ini,main.lua,config.lua}`; `test/device/t_sign.py`. Config keys `issuer_key`, `tokens`, `record_ttl_s`.
**Interfaces consumed:** `vk_check_solana`, `approval::open`, `signer::begin/poll`, config.

- [x] `decodeSolana`: fill `vk_check_input_t`, call the chain, map the verdict to an `ApprovalRequest` per the table. At this stage a record is rarely supplied; the path with one is exercised in WP21.
- [x] `vk::wallet::begin` now opens the approval for button domains.
- [x] Lua bindings; each calls `runtime::extendDeadline` as specified.
- [x] Sign test app (it carries its own minimal JSON field extraction until WP35; replace with `vk` then). Written; its network path has not run.
- [ ] Device (`t_sign.py`), dashboard running with the listener open: T-APR1 (the signature verifies on the laptop with the badge's public key; the transaction confirms on devnet), T-APR4, T-APR5. Done offline with `checktest` (`t_sign.py`): T-APR1's signature half, T-APR4, T-APR5. Deferred (network: `t_sign_net.py`): `signtest` against the listener and the confirmation on devnet.

**Done when (Gate 1):** a transfer served by the laptop is shown with the correct amount on the firmware screen, signed after the hold, and confirmed on devnet; each refusal vector is refused.

---

## Phase 2: trust

### WP20: Clock

**Read:** [checks](../wallet/checks.md#clock).
**Files:** Fill `src/vk/core/clock.{h,cpp}`. Config key `ntp_server`. Info field `time`. Dev-only `vk::clock::devSet(unix)` for `VKTIME`.

- [x] Service: start SNTP once when Wi-Fi is first connected; never wait for it (review focus 4). Sync callback and the polling fallback, both compiled.
- [x] `raiseTo`. `VKINFO` reports `time=`.
- [ ] Device: with the hotspot up, `time=sntp` within 10 s of Wi-Fi connecting (deferred, network: `t_clock_net.py`); with Wi-Fi off, boot reaches `[os] ready` in the usual time and `time=none` (done: `t_clock.py`).

### WP21: Record checks wired into the approval

**Read:** [checks](../wallet/checks.md) whole.
**Files:** Modify `features/solana_pay/domain_solana.cpp`, `lua_solana.cpp` (`check_record`); create `apps/checktest/` (dev-only test app, permissions `sign,net,history,storage`, that calls `begin_solana` with the message and `ctx` of one case, loaded from `case.lua`, which the test pushes with the app), `test/device/t_chk.py`.
**Needs:** no network. `t_chk.py` builds each case on the laptop (message, record, request), signing with the test issuer and device keys from `vectors.json`, with the badge provisioned to that issuer, and pushes it as `case.lua`. The backend's `GET /registry/<address>` ([backend](../integration/backend.md#needed-routes)) is needed by the Pay app, not by this package.

- [x] Record verification, `clock::raiseTo`, and the full verdict mapping. A supplied request is verified here too (checks 11 and 12 do not need the `requests` feature); only presence is absent until WP23, so the best verdict in this package is amber.
- [x] Device: T-CHK2 to T-CHK9; the app is still running after a `begin_solana` with a full `ctx` (review focus 1).

### WP22: ESP-NOW router

**Read:** [protocol](../protocol/espnow.md) (Frame header, Type registry, Router).
**Files:** Fill `src/vk/host/router.{h,cpp}`.

- [x] `install`, route lookup, forwarding rules 1–3, `send`. `granted("espnow")` is consulted (always true until WP30).
- [ ] Device: T-HOOK1; an upstream ESP-NOW sample app (`whosnear`) still works between two badges (deferred, second badge: `t_hook.py`). On one badge, `hello` and `whosnear` still launch and stop with the router's handler installed.

### WP23: Requests and presence

**Read:** [protocol](../protocol/espnow.md) from "Payment requests" on; [Lua API](../platform/lua-api.md#badgewallet-requests).
**Files:** Create `src/vk/features/requests/domain_pay_req.cpp`, `domain_pay_proof.cpp`, `requests.{h,cpp}`, `presence.{h,cpp}`, `lua_requests.cpp`; `apps/reqtest/` (dev-only fixture: opens a request, logs RESULT frames); `test/device/t_req.py` (two ports). The request cache posts its notification through `vk::host::notify::post` (a stub until WP32). Config keys `presence_ms`, `req_ttl_s`, `req_period_ms`, `req_max_proofs`, `req_gap_ms`, `pay_app`. Permission `request`.
**Interfaces produced:** sets `vk::wallet::presenceLookup`; Lua `request_open/close/status`, `requests`, `challenge`, `presence`.

- [x] Payee: active-request table, signing, rebroadcast service, CHAL route with rate limits, close on app stop (a `VK_ON_APP_STOP` listener).
- [x] Payer: request cache route, presence slots, PROOF route with `lastRxMs`, log line `[req] proof <ms> ms`.
- [ ] Device: T-REQ1 to T-REQ4, T-CHK1 (deferred, second badge: `t_req.py`). On one badge (done: `t_req_single.py`): `no_time`, open, status, close on app stop, `busy`, `bad_arg`.

**Done when (Gate 2):** on two badges, an honest request is green; a replayed request is amber; a transaction to the wrong account or for the wrong amount is red; an unregistered payee is red.

### WP24: History

**Read:** [stores](../wallet/stores.md).
**Files:** Create `src/vk/features/history/history.{h,cpp}`, `lua_history.cpp`; `test/host/test_stores.cpp`. Permission `history`.

- [x] Host tests first: ring wrap at 128, bad magic recovery, a write that fails leaves the previous file intact and returns false.
- [x] Listener writes one record per outcome; a failed write is logged and ignored (review focus 3). Creates `/vk/` if missing.
- [x] Device: T-STO1, T-STO2.

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

> Design change (2026-10-03): the status-item registry is removed by WP37. The `inbox` and `dev` items this package creates are deleted there; the waiting count is shown by the launcher's `inbox` cell and the Settings list ([shell](../ui/shell.md#launcher)), and "the bar shows `[1]`" below becomes "the launcher's Inbox cell shows `1`". The `notify` LED pattern uses the theme's `LED` colour, not upstream's purple.

**Read:** [app host](../platform/app-host.md#notifications), [ui](../ui/ui.md#status-bar-removed).
**Files:** Fill `src/vk/host/notify.{h,cpp}` (with status item `inbox`, LED pattern `notify`); create `src/vk/ui/status_dev.cpp` (status item `dev`), `src/native_apps/inbox/inbox.cpp`. No other package's file is edited: the requests feature already calls `notify::post`.

- [ ] Device: T-REQ5; `VKNOTE` posts; the bar shows `[1]`; Inbox opens the named app.

### WP33: Balance

**Read:** [ui](../ui/ui.md#balance).
**Files:** Create `src/vk/features/balance/balance.{h,cpp}`, `lua_balance.cpp`. Config key `balance_poll_s`. Status item `balance`.

- [x] Service with the stated conditions; reply scanning; 3 s timeout; skip when Wi-Fi is down (review focus 4). Written and compiled; nothing of it has run against a network.
- [ ] Device: after `devnet:setup` funded the badge, the bar shows the balance within one poll; `wallet.token_account()` equals the account `devnet:setup` created; log `[bal] fetch <ms> ms` (deferred, network; no test file yet).

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

### WP37: BadgeOS shell rewrite and rebrand

The OS is named **BadgeOS** and its whole user interface is its own: upstream's shell is rewritten in the Receipt layout, the splash is removed, and nothing a user can see (and no network identifier) says "Solana" or "SKYRIZZ". This replaces the earlier WP37 (a native launcher app, a native settings app and a home service that relaunched the launcher), which is cancelled. Credit to upstream stays in the repository (`os/README.md`), not on the device.

**Read:** [shell](../ui/shell.md) (all); [ui](../ui/ui.md#theme) to the end; [upstream hooks](../architecture/upstream-hooks.md) (Table, Retired hooks, H23, Replaced upstream files); the simulation in `docs/design/os-mockups/`; upstream's `src/ui/shell.cpp` before it is deleted.
**Files:** Create `src/vk/shell/` (`screens.h`, `shell.cpp`, `page.h`, `page.cpp`, `launcher.cpp`, `settings_list.cpp`, `dialogs.cpp`, `pages/page_<id>.cpp` for the thirteen rows), `src/vk/ui/repaint.{h,cpp}`; fill `src/vk/ui/boot_screen.cpp`; rewrite `src/ui/boot.cpp`, `README.md`; delete `src/ui/shell.cpp`, `splash_images.h`, the six upstream sample apps, `src/vk/ui/statusbar.{h,cpp}`, `src/vk/ui/status_dev.cpp`; edit the files of hook H23 and of the replaced-files table; `src/vk/host/home.{h,cpp}` keeps only `idle()`; `VKSTATE` gains `screen`; `UPSTREAM-HOOKS.md`, `scripts/preflash-check.sh` (check 1 extended, check 7), `test/device/common.py`, `t_shell.py`, `t_pages1.py`, `t_pages2.py`. Agents and exact file lists: [execution plan](execution-plan.md), Batch 5 (5A shell, 5F and 5G pages, 5R rebrand).

- [ ] Shell framework: screen stack, the four `shell::` functions, `shell::screenName()`, repaint on request, the 500 ms header and theme check.
- [ ] Boot: no splash; the Receipt boot screen from the first frame; LED boot bar. T-LED1 by a person.
- [ ] Launcher: grid, navigation, hold RIGHT to delete a Lua app, balance row or `SETUP NEEDED`, barcode, the inbox count.
- [ ] Dialogs: delete confirmation, app error, app-store offer, installing.
- [ ] Settings list and the thirteen rows: Theme, Wi-Fi, Bluetooth, ESP-NOW, App push, App store, Identity (New identity behind a hold-SELECT confirmation), Display, LEDs, Wallet, Inbox, Device info, Console. Every upstream call of the old screens is kept ([shell](../ui/shell.md#settings-pages)).
- [ ] Rebrand: hook H23, the replaced files, the status-item registry removed, `DEFAULT_BROKER_URL` empty.
- [ ] Tests T-SHELL1 to T-SHELL6, T-BRAND1, T-BRAND2 ([testing](../testing/testing.md#acceptance-tests)); screenshots of every shell screen in both themes kept in `test/device/shots/` as `shell_<screen>_<theme>.png`.
- [ ] The Solana check prints nothing ([upstream hooks](../architecture/upstream-hooks.md#checking-the-hooks)).

**Done when (WP37):** the badge boots to the BadgeOS boot screen and the launcher; every settings page is reached by name and drawn in both themes; every app returns to the launcher (`VKSTATE` app empty, `screen` `launcher`); pre-flash checks 1 and 7 pass.

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
| 00 | orchestrator; agent 0 (Batch 0) | 2026-10-03 | upstream builds, flashes at 460800 baud, boots, `PING` ok; unmodified upstream compiles in place from `os/` | badge key is software (SE050 select failed). Public key `5vpmgLuCfbV7Lp2hTNFz7w75ibhVkc56G1mR6weQedvj`, read by eye from a screenshot of Settings → Identity (confirm against `VKINFO pubkey` when WP11 lands). Finding F17 (I²C clock held low) recorded |
| 01 | Batch 1: 1A (hooks, `vk.cpp`, `core/`, build scripts), 1B (`host/`, `ui/`, `wallet/`, `sdk/` stubs); integrator I1 | 2026-10-03 | dev build compiles; pre-flash checks 1 to 5 pass (hook ids H1–H17, H19, H20, H21); T-BOOT1 (`t_boot.py`); `VKHELP` lists `VKHELP` and `VKINFO`; `VKINFO` → `OK profile=dev api=2`; linker check (`[vk] linkcheck alive`, U2 settled, no anchor file); upstream sample `hello` pushed, run (`VKSTATE.app` = `hello`), stopped, launcher repaints (screenshot equal to the one before) | Boot line: `[vk] registries: services=0 commands=8 lua=0 status=0 domains=0 routes=0 permissions=0 patterns=0 native=0 config=1` (the one config key is `theme`; with the temporary link-check service it read `services=1`). `[id] 5vpmgLuC, software (1 ms)`. Image 1,882,539 bytes. The first build compiled with no cross-agent error. H21 applied (`VK_SE050_QUARANTINE 1`): no `[se050]` line and no bus scan at boot; the I²C bus was still held low at this flash (`[btn] TCA9534 init FAILED`), so hardware buttons need one power cycle. H21 did not cover every path to the SE050 at WP01; Batch 2 closed them (the guards now sit in `se050::test()`, `se050_t1::begin()` and `badge_i2c::scan()`), and after one power cycle the bus was healthy at the Batch 2 flash: `[btn] TCA9534 init ok`, heartbeat `btn=0` at 30 to 180 s, real key presses logged. Batch 2 boot line: `[vk] registries: services=4 commands=16 lua=0 status=1 domains=1 routes=0 permissions=0 patterns=5 native=0 config=7`. Image 1,928,819 bytes. Deferred: T-HOOK1 (second badge). Not yet exercised: `fileio` `renameFile` over an existing file on LittleFS (first used by WP10 and WP24). The `bootScreen` stub forwards the percentage to `leds::bootProgress`, so WP12's boot bar is driven as soon as that body exists |
| 02 | Batch 1: 1C; integrator I1 | 2026-10-03 | host suites `sol`, `record`, `frames`, `checks` (`test/host/run.sh`, also run as pre-flash check 5); the pure code compiles with the ESP32 toolchain and links into the image | vectors regenerate byte for byte (1C). Spec additions recorded in solana-payments.md, checks.md, espnow.md and testing.md. The shim has no suite of its own in the tree |
| 03 | Batch 1: 1D; integrator I1 | 2026-10-03 | `t_boot.py` (T-BOOT1, serial checks, navigation by injected key); `vkdev.py` `wait-ready`, `reset`, `state`, `cmd`, `btn`, `shot`, `push`, `run`, `stop`, `test` used on the badge | Opening the port does not reset the badge; `reset` reboots it (`[os] ready` after 8.3 s); `VKSHOT` takes 0.6 to 1.0 s; pushing `hello` (2 files) takes 1.5 s. Deferred: T-REL2 (release build, Batch 6); `provision` (needs WP10 and a filled `dashboard/.env`); `monitor` and `provision` not run on the badge |
| 10 | Batch 2: 2A; integrator I2 | 2026-10-03 | host suite `config`; on the badge `t_cfg.py`: T-BOOT2, T-CFG3 (against `rpc_url`, unprovisioned and again over a stored value), T-CFG1 (values survive a reset, `provisioned=1`), T-CFG2 (`Change setting` confirmation: hold writes, CANCEL leaves) | code complete. The first firmware build had no error in this package. `VKINFO pubkey` is `5vpmgLuCfbV7Lp2hTNFz7w75ibhVkc56G1mR6weQedvj`, equal to the key read by eye in WP00. The tests leave the badge provisioned with the test values. Deferred: T-CFG4 (hands); `vkdev.py provision --env` (needs `npm run devnet:setup`, U9). Spec additions recorded in config.md |
| 11 | Batch 2: 2B; integrator I2 | 2026-10-03 | host suite `domains`; pre-flash check 2 (nothing but `signer.cpp` signs); T-BOOT3 on the badge (`selfcheck=1`, `key=software`, boot log `[vk] selfcheck ok`) | code complete. One domain registered (`store-reg`). No signature has been made through `signRaw` on the badge yet: the first is WP13's (M2). Deferred: store registration against a broker (network). Spec additions recorded in signing.md |
| 12 | Batch 2: 2C (engine, LEDs, demo command), 2D (theme, receipt kit, screen); integrator I2 | 2026-10-03 | host suite `approval`; on the badge `t_apr.py` in `receipt-light` and in `receipt-dark`: the three demo severities with their `VKSTATE` and select rules, launcher repaint after close, T-APR2, pushed `STOP` and `RUN` applied only after the approval closes, T-APR3 | code complete. Screenshots in `os/test/device/shots/`: `approval_<green\|amber\|red>_<theme>.png` and `result_<approved\|cancelled\|blocked\|timed_out>_<theme>.png`; all read by the integrator, no clipped or overlapping text. Changed at integration, both by the design owner's decision: the footer in `RESULT` shows the result word and no key hints; **the rubber stamp was removed** from the approval screen and the kit (`receipt::stamp` deleted; the `STAMP_*` tokens stay as status inks), so the coloured band alone carries the verdict. Engine open to first draw: 11 ms (part of M3). Cosmetic, not changed: the screen is 5 px higher than the simulation between the band and the last row. Deferred: T-LED1, T-LED2 (hands); `DEL <running id>` while a signing approval is open (review focus 2) needs an app-owned approval, so it moves to Batch 3 with `signtest`; `SIGNED` and `SIGN FAILED` result screens (need a signing domain) |
| 13 | Batch 3: 3A (`domain_solana.cpp`), 3B (`lua_wallet.{h,cpp}`, `lua_solana.cpp`), 3C (`signtest`, `checktest`, `fixtures.py`, `t_sign.py`, `t_sign_net.py`); integrator I3 | 2026-10-03 | nine host suites (`sol record frames checks config domains approval stores requests`); `fixtures.py` self-check; on the badge `t_boot.py`, `t_cfg.py` (T-CFG3 now against `tokens`), `t_apr.py`, and `t_sign.py`: T-APR1 offline half (red UNVERIFIED RECIPIENT with the dev override, injected hold, signature verified on the laptop against `VKINFO pubkey`; also a transfer built on the badge with a memo), T-APR4, T-APR5 (five vectors), review focus 2 (stop before poll, then not `busy`; `DEL` while modal) | **Gate 1 is met only in its offline half**: the correct amount on the firmware screen, signed after the hold, each refusal vector refused. Missing half: `signtest` against the dashboard listener and a transaction confirmed on devnet (`t_sign_net.py`, never run; needs the hotspot, the listener and `devnet:setup`). First firmware build of the batch compiled with no error. Boot line: `[vk] registries: services=7 commands=16 lua=29 status=2 domains=4 routes=3 permissions=2 patterns=5 native=0 config=17`. Image 1,961,263 bytes (59 % of the slot). M2: sign 211 ms (`[vk] sign solana 214 bytes 211 ms`), **verify 419 ms, above the 400 ms threshold: Monocypher (WP51, agent 4F) is called for**. No loop-task stack overflow seen. Screenshots `pay_unverified_dev.png`, `apr4_approval.png`. The batch's sources first entered the history in the owner's snapshot commit `3586802` ("before utsav push"), taken while integration was running: it also holds a temporary dev-only file, `features/devtools/tmp_i3.cpp` (commands `VKHDR`, `VKHISTFILL`, `VKHISTRM`), which the batch commit removes |
| 20 | Batch 2: 2E; integrator I2 | 2026-10-03 | on the badge `t_clock.py`: `[os] ready` 7.8 s after reset with Wi-Fi absent, `time=none`, `VKTIME` gives `time=sntp` in `VKINFO` and `VKSTATE` | code complete; device verification of SNTP deferred (network: `t_clock_net.py`, U5, U6). Both SNTP paths are compiled (callback and status poll). No host suite (`clock.cpp` is host-compilable if one is wanted) |
| 21 | Batch 3: 3A (glue), 3C (`checktest`, `t_chk.py`); integrator I3 | 2026-10-03 | host suite `checks`; on the badge `t_chk.py`: T-CHK2 to T-CHK9 (severity, headline, select, the named line, the poll reason), the replay case (amber VERIFIED - NOT PRESENT), review focus 1 (the app survives a `begin` with a record and a request) | The decoder's verifier remembers its last answer, so the record is verified once, not twice (checks.md, "Verdict to screen"): M3 is 12 ms with no record, 434 ms with a record, 854 ms with a record and a request (853 and 1273 ms before). Screenshots `chk_<case>.png` for all 11 cases, read by the integrator: no clipped or overlapping text; also tried with a 32-character name, a 35-character memo and `999.99 HACK`: everything is shortened with `..` as the layout specifies. Deferred: T-CHK1 (second badge) |
| 22 | Batch 2: 2E; integrator I2 | 2026-10-03 | on the badge: `hello` launches, runs and stops with the router's handler installed (`t_clock.py`, `t_apr.py`); `whosnear` launched from the real keys comes up on channel 1 | code complete; device verification deferred (second badge: T-HOOK1 and `whosnear` between two badges, `t_hook.py`). `routes=0` until WP23 |
| 23 | Batch 3: 3D; integrator I3 | 2026-10-03 | host suite `requests`; on the badge `t_req_single.py`: `no_time` with the clock unset, two requests open after `VKTIME` (one `[vk] sign pay-req` each, 210 and 211 ms), status `open 0`, stopping the app closes them, a third open is `busy`, `10.001` and `0.00` are `bad_arg` | code complete; device verification of everything over the air deferred (second badge): T-REQ1 to T-REQ4 and T-CHK1 (`t_req.py`), M1, T-REQ5 (also needs the Inbox of WP32; `notify::post` is still the WP01 stub, so the "Payment request" note is dropped until then). **Gate 2 is met only in its offline half**: on one badge with prepared inputs, an unregistered payee is red (T-CHK3, T-CHK4), a request with no presence is amber (the replay case), a wrong account or amount is red (T-CHK6, T-CHK7). Missing half: an honest request green (T-CHK1) and the replay and wrong-payment cases between two real badges. Registries: services +2, routes +3, domains +2, lua +6, permissions +1, config +6 |
| 24 | Batch 3: 3E (history), 3C (`t_sto.py`); integrator I3 | 2026-10-03 | host suite `stores`; on the badge `t_sto.py`: T-STO1 (a refused then a signed payment; `signed 10.00 HACK` before and after a reset), T-STO2 (both paths fail); also run once with the ring full (128 records, wrap path) | One append costs 22 to 75 ms on a nearly empty file and 245 to 320 ms on a full ring, not "a few milliseconds" (table in stores.md); the dev profile logs `[vk] history write <ms> ms`. `fileio` `renameFile` over an existing file is still not exercised on the badge (only the bad-magic path uses it) |
| 30 | | | | |
| 31 | | | | |
| 32 | | | | |
| 33 | Batch 3: 3E; integrator I3 | 2026-10-03 | compiles and registers (service, status item `balance`, `balance_poll_s`, three Lua functions); the reply scanner is covered by host suite `stores` | code complete; device verification deferred (network): status item shows the balance within one poll, `wallet.token_account()` equals the funded account, `[bal] fetch` (M5), `refresh_balance` with Wi-Fi off returns `nil, "timeout"`. No test file exists for it yet (3E described the test; write `t_bal_net.py` with `NEEDS = "network"`). The device tests run with Wi-Fi off (`VKINFO wifi=0`), so the idle poll never fired and `balance_poll_s 0` was not needed in the test provisioning |
| 34 | | | | |
| 35 | | | | |
| 36 | | | | |
| 37 | Batch 5: 5A (shell framework, launcher, dialogs, boot), 5F and 5G (settings pages), 5R (rebrand, test tooling); integrator I5 | | Gate 3 | redefined 2026-10-03 as "BadgeOS shell rewrite and rebrand" (specification: `ui/shell.md`); the earlier launcher app, settings app and home service are cancelled; not started |
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
