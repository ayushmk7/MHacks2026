# BadgeOS execution plan

> **Orchestrator notes (2026-10-03), which override the text below where they differ:**
> - The firmware folder is `os/` at the repository root and its main file is `os.ino` (upstream's `solana-os.ino` renamed). It already exists and is committed unmodified (commit `f7a05d7`), so step 1 of Brief 0 is done.
> - Upload needs 460800 baud: use the FQBN with `,UploadSpeed=460800` for `arduino-cli upload`. Auto-reset works. Opening the serial port does not reset the badge.
> - The connected badge's key is a **software key** (its SE050 refuses the applet select).
> - Hook id **H21 is reserved for the button/I2C fix** being developed separately; the loop-stack fallback is H22.
> - The upstream clone for reference and diffing is at the `UPSTREAM` path below (pristine).
> - **Batch 5 was redefined by a design change (2026-10-03).** The OS is named BadgeOS; upstream's shell is deleted and rewritten as BadgeOS's own (`src/vk/shell/`, [ui/shell.md](../ui/shell.md)); the native `launcher` and `settings` apps, the home service, the status-item registry, the splash images and hooks H14, H15 and H20 are gone; hook H23 carries the renames. Sections 2 to 9 below are updated; the briefs of Batches 0 to 4 are kept as they were run and are history where they mention those things.


Save this file as `docs/os/roadmap/execution-plan.md` **before dispatching anything**: every brief tells its agent to read sections of it from disk.

- `REPO` = `/Users/ayush/Downloads/My Projects/Hackathons/MHacks2026` (the path contains a space; always quote it)
- `FW` = `$REPO/os` (the firmware folder at the repository root; its main sketch file is `os.ino`)
- `UPSTREAM` = `/private/tmp/claude-502/-Users-ayush-Downloads-My-Projects-Hackathons-MHacks2026/41bb931f-51cc-40a5-9e9e-f24006cd3c71/scratchpad/upstream/firmware/solana-os` (pristine, commit 812b8c7)
- `PY` = `$REPO/.venv/bin/python`; `PORT` = `/dev/cu.usbserial-10`
- `FQBN` = `esp32:esp32:esp32s3:PSRAM=opi,FlashSize=16M,PartitionScheme=custom,CDCOnBoot=cdc`

Checked while planning:
- Every hook site in `upstream-hooks.md` exists in upstream as described.
- The five fonts the spec names exist in the installed LovyanGFX 1.2.32 (`lgfx_fonts.hpp` lines 366–407).
- The venv has `cryptography` 50, so Python device tests can sign and verify Ed25519.
- `lua` on the laptop is 5.5.1; the badge runs 5.4 with 32-bit integers.
- `dashboard/.env` has an empty `HACK_MINT` (`devnet:setup` has not been run), so `vkdev.py provision --env` cannot be verified now. Device tests provision from `test/host/vectors.json` instead.
- Upstream has only been compiled from the scratchpad path, which has no space. Compiling from `REPO` is unverified; Batch 0 settles it.

## 1. Spec fixes needed before starting

Agent 0 applies these to `docs/os/` before any code is written. None changes a requirement; each closes a contradiction or a missing declaration that would make parallel agents write incompatible code.

| # | File(s) | Conflict | Resolution |
|---|---|---|---|
| F1 | `ui/ui.md` (Balance; LED patterns), `platform/app-host.md` (Notifications) | Balance polls "only while no app is running"; `notify` plays "while … no app is running"; a finished pattern resumes idle "if no app is running". But `ui.md` "Launcher and settings" keeps the native `launcher` running at all times, and hook H8c makes `runtime::running()` true for it. As written the balance is never fetched. | Add `bool vk::host::idle()` to `host/home.h`: true when no app is running or the running app is config `home_app`. Replace "no app is running" with "the badge is idle (`vk::host::idle()`)" in those three places. *(Superseded by the Batch 5 design change: there is no launcher app and no `home_app`; `idle()` stays and is simply `!runtime::running()`.)* |
| F2 | `wallet/approval.md` (Screen) | The signature is `drawApproval(const ApprovalRequest &, Phase, float holdProgress)`. The same section needs the outcome to draw the RESULT band (SIGNED / APPROVED / CANCELLED / TIMED OUT / BLOCKED / SIGN FAILED) and "footer blinks once" for DISABLED. Neither is in the arguments. | Signature becomes `void drawApproval(const ApprovalRequest &, approval::Phase, float holdProgress, const ApprovalOutcome *outcome, bool footerBlink)`, declared in `src/vk/ui/approval_screen.h`. `outcome` is non-null only in RESULT. |
| F3 | `platform/app-host.md` (Permissions) | Step 1 says "`vk::lua::open` (or `native::start`) promotes pending to active", but the `permissions.h` block declares only `granted` and `preLaunch`. | Add `void promotePending();` to `namespace vk::host` in that block. |
| F4 | `platform/app-host.md` (Consent), `roadmap/implementation-plan.md` (WP01) | WP01 must create `host/consent.h` "exactly as in the spec"; no document declares its functions. The Wallet app (apps.md page 4) needs to enumerate stored consent. | Add the `vk::host::consent` block from section 4 of this plan to `app-host.md`. |
| F5 | `roadmap/implementation-plan.md` (WP01, at-a-glance), `platform/config.md`, `wallet/signing.md` | WP01 "creates that one pure header [`vk_reason.h`]; WP02 owns the rest" and may run in parallel with WP02. But `core/config.h` uses `vk_token_t` / `VK_MAX_TOKENS` and `signer.h` uses `vk_presence_t`, both from `pure/vk_checks.h`, which includes `sol.h`, `vk_record.h`, `vk_frames.h`. The WP01 list also omits `signer_internal.h`, `crypto.h`, `ui/receipt.h`, `ui/approval_screen.h`, `ui/boot_screen.cpp` (stub of `bootScreen`), `host/home.{h,cpp}`, `vk::clock::devSet`, and the `InfoField` struct behind `VK_INFO_FIELD`. | WP01 depends on WP02's headers; they are written in one batch and integrated together. Replace WP01's stub list with section 4 of this plan. Add the `InfoField` block to `config.md` (Serial commands). |
| F6 | `roadmap/implementation-plan.md` (Review focus 2) vs `architecture/upstream-hooks.md` (H4) | Review focus 2: "Force-quit (hold CANCEL) or a pushed STOP during the approval. Expected: the approval closes". H4: "Requests queued during the approval (a pushed RUN or STOP…) are applied on the first pass after it closes", and `routeButtons()` is skipped, so force-quit cannot happen. | H4 wins. Reword focus 2: the only stop during an approval is `DEL <running id>` (upstream calls `runtime::stop()` directly). Tests: `DEL` while modal closes the approval; an app stopped with an un-polled result leaves the next `begin` not `busy`. |
| F7 | `roadmap/implementation-plan.md` (WP10 "Device: T-CFG1, T-CFG3") vs WP13 and `config.md` Keys | T-CFG3 is `VKSET tokens garbage → ERR invalid`, but `tokens` and `issuer_key` are registered by `solana_pay` (WP13). At WP10 the reply is `ERR unknown_key`. | At WP10, T-CFG3 runs against `rpc_url` (`VKSET rpc_url x` → `ERR invalid`); from WP13 on it runs against `tokens`. `t_cfg.py` reads `VKKEYS` and picks accordingly. |
| F8 | `wallet/stores.md` (History outcome 6 "approved") vs `platform/lua-api.md` (`wallet.history` lists five outcomes) | A confirmation that is not a signature has no Lua outcome string. | Add `"approved"` to the list in `lua-api.md`. |
| F9 | `ui/ui.md` (Status bar: `dev` "Registered by `ui/statusbar`") vs `implementation-plan.md` WP32 (`src/vk/ui/status_dev.cpp`) | Two homes for one item. | `ui/status_dev.cpp`; fix the table cell. |
| F10 | `guides/build-flash-provision.md` (Build profiles: "hold-to-sign on unverified and no-clock screens") vs `wallet/approval.md` (Dev builds: only "no record supplied") | The guide names an override that does not exist. | Delete "and no-clock" in the guide. |
| F11 | `testing/testing.md` (host tests use "in-memory stand-ins for the file … calls"), `wallet/stores.md` (three stores in three owners that may not include each other) | No shared file layer is named, so three agents would each invent one and the shim could not serve them. | Add `src/vk/core/fileio.{h,cpp}` (section 4) to the source tree in `overview.md` §4 and to "Common rules" in `stores.md`. |
| F12 | `architecture/overview.md` §4, `platform/lua-api.md` | `wallet.begin` (in `wallet/lua_wallet.cpp`) and `wallet.begin_solana` / `begin_bank` (in feature folders) need the same ctx parsing; features may not include each other. | Add `src/vk/wallet/lua_wallet.h` declaring `int vk::wallet::luaBegin(lua_State *L, const char *domain, int bytesIndex, int ctxIndex);` to the tree. |
| F13 | `apps/apps.md` (Check test row), `implementation-plan.md` WP21 | `checktest` "fetches ctx variants from the laptop", which needs a network that does not exist now; T-STO1 and T-STO2 need a test app with `history` and `storage`. | `checktest` loads its case from `case.lua`, pushed with the app by the test. Its permissions become `sign,net,history,storage`. Update the row and WP21. |

Corrections to the dependency table in `implementation-plan.md` (agent 0 records them in that file under the table):

- WP01 needs WP02's headers (F5).
- WP10, WP11 and WP12 are listed as a chain. With the section 4 headers frozen they are written in parallel and integrated together.
- WP13's step "`vk::wallet::begin` now opens the approval" edits `signer.cpp`, a WP11 file. It moves to WP11.
- WP23 lists WP21 and WP34 lists WP32 only for their gates and for `notify::post`, which is a stub from WP01. Neither is needed to compile.
- WP36 does not use WP24.
- "One branch per work package" is replaced here by one branch (`badge-os`) and one commit per batch by the integrator.

Choices made where the spec leaves one open:

- **Offline device tests.** `checktest` plus `test/device/fixtures.py` build messages, records and requests on the laptop with the test issuer and device keys from `vectors.json`, so T-APR1 (signature half), T-APR4, T-APR5, T-CHK2–9 and T-STO1–2 run on one badge with no network.
- **Test provisioning.** `issuer_key` = vectors' test issuer, `tokens` = `<vectors tx.mint>:2:HACK:100.00:1000.00`, `rpc_url` = `http://127.0.0.1:8899`, `approval_tmo_s` 10, `hold_ms` 1000, `record_ttl_s` 3600.
- **Host suites added under README rule 7** (state machines): `test_requests` (presence slots, request cache, rate limits).
- **Temporary test files** (linker check, T-APP6 native app) are added by the integrator, built, tested, deleted, and the tree recompiled before the commit.
- **Monocypher** is dispatched (agent 4F) only if integrator I3 measures verify > 400 ms.
- **WP54 (bank)** is written and compiled in Batch 6 only if Batches 1–5 closed with no open failure.

## 2. Batch table

| Batch | Goal | WPs | Parallel agents | Integrator | Gate before the next batch |
|---|---|---|---|---|---|
| 0 | Fork exists, unmodified upstream committed, spec fixed, in-place build proven | 00 (rest), 01 (copy) | **0** fork agent (alone; may compile and commit) | — | two commits exist; `arduino-cli compile` of the unmodified fork succeeds from `FW`; build-directory note written in the build guide |
| 1 | Skeleton with every hook and frozen headers; pure code green; unattended test loop | 01, 02, 03 | **1A** hooks, `vk.cpp`, `core/`, build scripts · **1B** `host/`, `ui/`, `wallet/`, `sdk/` stubs · **1C** `pure/` and host-test infrastructure · **1D** devtools, `vkdev.py`, `t_boot.py` | **I1** compile, linker check, flash, T-BOOT1 | host suites `sol record frames checks` pass; dev build compiles with hook check; badge boots like upstream; `t_boot.py` passes (or upload failure recorded and compile-only) |
| 2 | One signing path, config, approval engine and screen, clock, router | 10, 11, 12, 20, 22 | **2A** config · **2B** signer, crypto, `store_reg` · **2C** approval engine, LEDs, demo command · **2D** theme, receipt kit, approval screen · **2E** clock, router | **I2** | + suites `config domains approval`; T-BOOT2, T-BOOT3, T-CFG1–3, T-APR2, T-APR3, demo approvals in three severities and both themes |
| 3 | Payments decoded, checked, shown, signed; requests; history; balance | 13, 21, 23, 24, 33 | **3A** `domain_solana.cpp` · **3B** Lua wallet bindings · **3C** `signtest`, `checktest`, fixtures, device tests · **3D** requests and presence · **3E** history, balance | **I3** | + suites `stores requests`; T-APR1 (offline half), T-APR4, T-APR5, T-CHK2–9, T-STO1; M2 recorded. Gates 1 and 2 are met only in their offline halves. |
| 4 | App platform: permissions, native apps, notifications, contacts, `vk.lua` | 30, 31, 32, 34, 35, 36 (+51 Monocypher if needed) | **4A** permissions, consent, manifest · **4B** native runtime, notify, Inbox · **4C** contacts feature · **4D** `vk.lua`, `badge.theme`, push script · **4E** Wallet app · **4F** (conditional) Monocypher | **I4** | + suites `manifest consent contacts vk`; T-APP1–7, T-STO2, `VKNOTE` → Inbox; Batch 3 tests still pass with consent |
| 5 | BadgeOS shell (launcher, settings, dialogs, boot) replacing upstream's, the rebrand, and every shipped app | 37, 40–45 | **5A** shell framework, launcher, dialogs, boot · **5F** settings pages 1 · **5G** settings pages 2 · **5R** rebrand and test tooling · **5B** Home, History · **5C** Pay, Request · **5D** Contacts, Game, Evil game · **5E** Duel | **I5** | the badge boots with no splash into the shell's launcher; every settings page is reached and screenshotted in both themes; every app launches, navigates and returns to the launcher on CANCEL (`VKSTATE` app empty and `screen == "launcher"`); the "Solana" grep of brief I5 prints nothing; the pre-flash check passes with the new hook list |
| 6 | Measurements, SE050 check, release build, optional bank rail | 50, 51, 52 (single-badge part), 54 | **6A** (optional) bank rail | **I6** | M2–M4, M6 recorded; release build compiles, passes pre-flash check 6 and T-REL2; full single-badge regression on the dev build; deferred list final |

## 3. Common rules for implementers

Every implementer brief points here.

1. **Read first**: `docs/os/README.md`, `docs/os/architecture/overview.md`, then your brief's Read list. The spec is the source of truth. Use function names, struct fields, config keys, reason strings, headline strings, frame layouts and Lua names exactly as written. Take signatures from the doc section your brief names; do not paraphrase them.
2. **Files.** Create or fill only the files in your brief. Do not touch upstream files, other agents' files, `docs/`, `test/host/run.sh`, `test/host/shim/`, or `scripts/vkdev.py` unless your brief lists them. If you own a `.cpp`, you may *add* declarations to its paired `.h`; never change or remove one that section 4 or the spec defines.
3. **Forbidden.** No `arduino-cli`, no `scripts/build.sh`, no serial port (`vkdev.py`, `esptool`, `screen`, `arduino-cli monitor`), no `git add/commit/stash/checkout/reset`. `git status` and `git diff` are fine.
4. **Missing or wrong interface.** Write against section 4 and the spec anyway. Do not fix someone else's file. Report it.
5. **Host tests first.** Run only your own suites: `cd "$FW" && test/host/run.sh <suite> [<suite>…]`. A bare `run.sh` also builds neighbours' half-written suites.
6. **Host-test seam.** A firmware `.cpp` with a host suite must compile with `-DVK_HOST_TEST -Itest/host/shim` and nothing else from the firmware except the files in the suite's `// LINK:` line. Put every upstream or hardware include and call behind `#ifndef VK_HOST_TEST` or behind a function pointer. Time, buttons, signing and verification are always function pointers.
7. **You cannot compile the firmware.** Before finishing, re-read every `#include` path and every call against the headers on disk (`src/vk/**.h`, upstream `src/**.h`). Where a file is host-compilable, compile it.
8. **Name clashes.**
   - Inside `namespace vk::ui`, upstream's palette is `::theme::…` and upstream's LEDs are `::leds::…`.
   - Lua headers are included as `extern "C" { #include "<rel>/lua/lua.h" #include "<rel>/lua/lauxlib.h" }`.
   - No exceptions, no RTTI. No heap in `pure/`.
   - Never include anything from `src/identity/` outside `src/vk/wallet/signer.cpp` and `crypto.cpp`. `crypto.cpp` may include `identity/ed25519.h` for `verify` only. (The shell's Identity page is the one other reader of `identity.h`, for display and for New identity; it never signs. See [ui/shell.md](../ui/shell.md).)
   - The shell's internals are `vk::shell::…` (`src/vk/shell/`). Upstream's four functions (`begin`, `update`, `onAppStopped`, `showError`) and `screenName()` are in the global `shell` namespace, so from code inside `namespace vk` write `::shell::screenName()`: a bare `shell::` there means `vk::shell`.
9. **Registries.** Section 5 lists who registers each config key, Lua function, permission, command, settings page, LED pattern, domain and route. Register only yours.
10. **No deployment literals** (keys, mints, URLs, names, limits): use config keys. Never assume the LED count: use `RGB_LED_COUNT`. Amounts are decimal strings or raw bytes, never Lua numbers.
11. **Lua bindings.** Use `luaL_check*` for types and return `nil, "<reason>"` for refusals. Call `runtime::extendDeadline(2500)` before each signature or verification, and with the timeout before a network call.
12. **Device tests** you write go in `test/device/`, use only the section 5 API, and declare `NEEDS` when they need more than one badge. You cannot run them; the integrator will.
13. **Final report**, exactly these headings:
    - `FILES` — every file created or changed, with line counts.
    - `HOST TESTS` — each command run and its last output line.
    - `DEVICE TESTS WRITTEN` — file, T-ids, `NEEDS`.
    - `SPEC ISSUES` — document, what was ambiguous or wrong, how you resolved it.
    - `INTERFACE NOTES` — anything in section 4 or another agent's file that did not fit.
    - `NOT DONE` — anything in the brief left undone, and why.

## 4. Stub contract (frozen after Batch 1)

After Batch 1 every header below exists under `FW/src/vk/` with its full declarations. Later agents replace bodies only. "Spec" means copy the block from that document verbatim, adding only the includes and the constructor it calls for. "ADDED" declarations are defined here because no document has them; agent 0 copies them into the named document.

| Header | Owner | Declarations | Stub body until filled |
|---|---|---|---|
| `vk.h`, `vk.cpp` | 1A | `vk::begin()`, `update()`, `modalActive()`, `modalUpdate()` (overview §4, §7). `vk.h` includes `core/serial.h` and `host/router.h` (hooks H3, H6). | complete: `modalActive()` returns `wallet::approval::active()`, `modalUpdate()` calls `wallet::approval::update()` |
| `vk_build.h` | 1A | overview §10. Includes `vk_profile.h` unless `VK_HOST_TEST` (then `VK_PROFILE_DEV 1`). Preprocessor only. | complete |
| `core/registry.h`, `core/service.h` | 1A | overview §6, §7, verbatim | complete |
| `core/serial.h` | 1A | config.md "Serial commands" block, plus ADDED: `struct InfoField : Registered<InfoField> { const char *name; String (*fn)(); InfoField(const char *n, String (*f)()) : name(n), fn(f) {} };` and `#define VK_INFO_FIELD(ident, name, fn) static vk::serial::InfoField vk_info_##ident(name, fn)` | complete (WP01) |
| `core/config.h` | 1A | config.md "Config store" block. Includes `../wallet/pure/vk_checks.h`. | registry declared; accessors return the default; `provisioned()` false; `set()` returns `UNAVAILABLE`; `confirmChange` null (filled by 2A) |
| `core/clock.h` | 1A | checks.md "Clock" block, plus ADDED inside the namespace: `#if VK_TEST_HOOKS` `void devSet(uint32_t unix_s);` `#endif` | source NONE; `devSet` stores the time and sets SNTP (filled by 2E) |
| `core/fileio.h`, `.cpp` | 1A | ADDED: `namespace vk::fileio { struct Ops { bool (*exists)(const char *path); long (*size)(const char *path); bool (*read)(const char *path, size_t offset, uint8_t *out, size_t len); bool (*writeAll)(const char *path, const uint8_t *data, size_t len); bool (*writeAt)(const char *path, size_t offset, const uint8_t *data, size_t len); bool (*renameFile)(const char *from, const char *to); bool (*removeFile)(const char *path); bool (*makeDir)(const char *path); }; extern const Ops *ops; }`. Paths are LittleFS-relative (`"/vk/history.bin"`). | complete: the firmware implementation prepends `FS_ROOT` and uses POSIX stdio |
| `wallet/pure/sol.h`, `vk_reason.h`, `vk_record.h`, `vk_frames.h`, `vk_checks.h` | 1C | solana-payments.md (header changes, builder, token table), signing.md "Reason codes", checks.md (record, presence, check chain), espnow.md "Codec". All with `extern "C"` guards. | complete (WP02) |
| `wallet/reason.h` | 1B | signing.md | complete |
| `wallet/signer.h` | 1B | signing.md "Domain table" and "Cross-feature interfaces" blocks | `signStoreRegistration` returns `identity::signBase64(message)`; `begin`/`signAuto` return `VK_UNSUPPORTED`; `poll` IDLE; pointers null (filled by 2B) |
| `wallet/signer_internal.h` | 1B | signing.md "Who owns what": `Reason signForApproval(const SignDomain *, const uint8_t *, size_t, uint8_t sig[64]);` | returns `VK_SIGN_FAILED` |
| `wallet/crypto.h` | 1B | signing.md "Crypto backend" block | header only; `crypto.cpp` is created by 2B |
| `wallet/approval.h` | 1B | approval.md "The request" block, complete | `active()` false; `open`/`confirm` false; `phase()` IDLE; `current()` null; `peekResult()` IDLE (filled by 2C) |
| `host/lifecycle.h` | 1B | app-host.md "Lifecycle events" | complete; `luaPaused()` returns `vk::modalActive()` |
| `host/lua_registry.h` | 1B | app-host.md "Lua function registry" | complete except permission filtering (4A); removes `loadfile`/`dofile`; calls `promotePending()` |
| `host/router.h` | 1B | espnow.md "Router" block | `install()` installs the forwarding lambda from implementation-plan WP01; `send` forwards to `espnow_mgr` (filled by 2E) |
| `host/permissions.h` | 1B | app-host.md "Permissions" block, plus ADDED `void promotePending();` | `granted()` true; `preLaunch()` true; `promotePending()` no-op (filled by 4A) |
| `host/manifest.h` | 1B | app-host.md "Manifest" | header only (4A) |
| `host/consent.h` | 1B | ADDED: `namespace vk::host::consent { uint32_t hashPermissions(const String &permissions); bool has(const String &appId, uint32_t hash); bool save(const String &appId, uint32_t hash); void eraseAll(); size_t count(); bool at(size_t index, String &appIdOut, uint32_t &hashOut); }` | header only (4A) |
| `host/native.h` | 1B | app-host.md "Native runtime" | reports no apps; `active()` false (4B) |
| `host/notify.h` | 1B | app-host.md "Notifications" | `post` drops; `count()` 0; `at()` null (4B) |
| `host/home.h`, `.cpp` | 1B | ADDED: `namespace vk::host { void showShell(); bool idle(); }` | `showShell()` no-op; `idle()` returns `!runtime::running()` (5A) |
| `ui/statusbar.h` | 1B | ui.md "Status bar" block, plus `namespace vk::ui { void requestShellRepaint(); bool consumeShellRepaint(); }` | complete |
| `ui/theme.h` | 1B | ui.md "Tokens and modes" block | complete registry and `color()`; one theme `solana` with upstream colours (2D replaces it) |
| `ui/leds.h` | 1B | ui.md "LED patterns" block, plus `namespace vk::ui { bool bootScreen(const char *step, const char *detail, uint8_t percent); }` | `play`/`stop`/`bootProgress` no-ops (2C); `ui/boot_screen.cpp` defines `bootScreen` returning false (5A) |
| `ui/receipt.h` | 1B | ui.md "The receipt kit" block | header only; `receipt.cpp` by 2D |
| `ui/approval_screen.h` | 1B | F2 signature | header only; `.cpp` by 2D |
| `sdk/badge_sdk.hpp` | 1B | native-apps.md "The SDK header", verbatim | complete; `badge::exit()` is defined in `host/native.cpp` |
| `features/devtools/devtools.h` | 1D | upstream-hooks.md H17: `extern "C" void vk_dev_apply_injected_buttons(uint8_t *down, uint8_t *pressed, uint8_t *released);` | complete |

Created later, by one owner, before any consumer exists: `wallet/lua_wallet.h` (3B, F12), `features/*/…h` (private to each feature).

Include trace: every `#include` a later package needs is one of the rows above, an upstream header listed in `upstream-baseline.md`, or a header private to its own feature.

**Changed by the Batch 5 design change.** The table above is what Batch 1 froze; four things in it change in Batch 5, and nothing else does:

- `host/home.h` loses `showShell()`. It keeps `bool vk::host::idle()`, which is `!runtime::running()` (owner 5R). There is no home service and no config key `home_app`.
- `ui/statusbar.h` and `statusbar.cpp` are deleted with the status-item registry (`VK_STATUS_ITEM`). The repaint pair moves, unchanged, to `ui/repaint.h`: `namespace vk::ui { void requestShellRepaint(); bool consumeShellRepaint(); }` (owner 5R; hooks H14 and H20 are retired, and the new shell consumes the request itself).
- `ui/leds.h`: `vk::ui::bootScreen(step, detail, percent)` keeps its signature, now always returns true, and is called from the rewritten `src/ui/boot.cpp` (owner 5A; hook H15 is retired: `boot.cpp` is a replaced upstream file).
- New headers `shell/screens.h` (the screen stack; `::shell::screenName()`) and `shell/page.h` (the `SettingsPage` registry, `VK_SETTINGS_PAGE`, `VK_SETTINGS_ACTION`, and the helpers pages draw with), owner 5A. Their content is in [ui/shell.md](../ui/shell.md) ("Framework", "page.h"); the block in "page.h" is the contract 5F, 5G and 5R code against before the file exists.

## 5. Shared contracts

### 5.1 Host-test infrastructure (owner 1C; never edited afterwards)

- `test/host/run.sh [suite…]`.
  - With no arguments it runs every `test/host/test_*.c`, `test_*.cpp` and (if a `lua` binary exists) `test_*.lua`. With arguments it runs only those.
  - A suite named `test_x` prints `all x tests passed` and exits 0.
  - Build output goes to `test/host/build/<suite>/`.
- A suite names its extra firmware sources in a comment within its first five lines: `// LINK: src/vk/core/config.cpp` (paths relative to `FW`, space-separated). Every suite is linked with all `src/vk/wallet/pure/*.c`, `test/host/host_ed25519.c`, `src/identity/tweetnacl.c` and, for C++, `test/host/shim/*.cpp`.
- Flags:
  - C: `cc -std=c99 -Wall -Wextra -Wpedantic -O2 -DSOL_HOST_SHA256 -DVK_HOST_TEST`.
  - C++: `c++ -std=c++17 -Wall -Wextra -O2 -DSOL_HOST_SHA256 -DVK_HOST_TEST -Itest/host/shim -Itest/host`.
- `test/host/host_ed25519.h`:
  - `void host_ed25519_keypair(const uint8_t seed[32], uint8_t pub[32]);`
  - `void host_ed25519_sign(const uint8_t seed[32], const uint8_t *msg, size_t len, uint8_t sig[64]);`
  - `int host_ed25519_verify(const uint8_t *msg, size_t len, const uint8_t *sig, const uint8_t *pub);` (1 = valid; same type as the pure code's `verify` pointer).
- Shim:
  - `Arduino.h`: `String`, `millis()`, `void vk_host_set_millis(uint32_t)`, `strlcpy`, fixed-width types.
  - `Preferences.h`: in-memory NVS; `void vk_host_nvs_reset(); extern int vk_host_nvs_fail_writes;`.
  - `vk_host_fileio.h`: `void vk_host_fileio_install(); void vk_host_fileio_reset(); extern int vk_host_fileio_fail_writes;` (the next N write or rename calls fail).
- `vectors.json` gains an object `vk`:
  - `issuer{seed_hex,pubkey_hex,pubkey_b58}`
  - `device{seed_hex,pubkey_hex,pubkey_b58}`
  - `record{text,hex,sig_hex}`
  - `req{frame_hex,signed_len}`
  - `proof{req_id_hex,nonce_hex,payer_hex,sig_hex}`
  - `memo_tx{messageHex,memo}`
- `vectors.h` exposes the same as `V_ISSUER_SEED`, `V_ISSUER_PUB`, `V_DEVICE_SEED`, `V_DEVICE_PUB`, `V_RECORD`, `V_RECORD_SIG`, `V_REQ`, `V_PROOF_NONCE`, `V_PROOF_PAYER`, `V_PROOF_SIG`, `V_MEMO_LEGACY`.

### 5.2 Device-test API (owner 1D)

A test is `test/device/t_<name>.py` with `def run(badge):` (or `def run(badge, badge2):`) and an optional module constant `NEEDS = "two-badges" | "network" | "hands"`.

`vkdev.py --port P [--port2 P2] test <file>…` prints `PASS <file>`, `FAIL <file>: <message>` or `SKIP <file> (needs …)` and exits non-zero on any FAIL. Tests with `NEEDS` are skipped unless `--include-deferred`.

`badge` methods:

| Method | Does |
|---|---|
| `cmd(line, timeout=5) -> list[str]` | sends one line; returns the `+ …` lines and the final `OK…`/`ERR…` line |
| `ok(line, timeout=5) -> str` | `cmd`, asserts OK, returns the text after `OK ` |
| `info() -> dict` | `VKINFO` parsed by name |
| `state() -> dict` | `VKSTATE` JSON |
| `wait_state(pred, timeout=10) -> dict` | polls `state()` until `pred` is true |
| `btn(key, action="tap", ms=None)` | `VKBTN` |
| `shot(path=None) -> bytes` | 153,600 bytes RGB565 little-endian; writes a PNG if `path` |
| `push(folder, app_id=None, extra=None)` | `VKPAIR`, then `AUTH` + `BEGIN`/`DATA`/`END` per file; `extra` is `{relative_path: bytes}` |
| `run(app_id)`, `stop()` | `AUTH` + `RUN` / `STOP` |
| `reset()` | pulses the reset line, then `wait_ready()` |
| `wait_ready(timeout=30)` | `[os] ready`, or `OK pong` to a `PING` sent every 2 s |
| `log() -> list[str]`, `clear_log()`, `wait_log(regex, timeout=10)` | every non-reply line since the last clear |

`test/device/common.py` (1D): `b58enc`, `b58dec`, `load_vectors()`, `provision_test(badge)`, `to_launcher(badge)`, `launch(badge, app_id)`.

- `provision_test`: if provisioned, `VKRESET` and an injected hold; then `VKSET` every key the firmware lists in `VKKEYS` with the values in section 1, then `VKCOMMIT`.
- `to_launcher`: cancels an open approval, stops the running app, then taps `b` until `state()["screen"] == "launcher"` (at most 6 taps). Until Batch 5 it was `stop`, `b`, `stop`, because `VKSTATE` could not tell upstream's launcher from its error screen; the `screen` field removes the guess.
- `goto_screen(badge, name)` (Batch 5): `to_launcher`, then through Settings to the shell screen called `name` ([ui/shell.md](../ui/shell.md), "Framework").
- `launch`: `run`, and if a modal with title `Allow app` appears, an injected hold of `VKGET hold_ms` + 300 ms (1300 under test provisioning).

As built in Batch 1 (integrator I1, confirmed on the badge):

- `btn()` blocks for the tap or hold time plus 150 ms, so a `shot()` sent straight after sees the key; `btn(..., wait=False)` returns at once.
- `shot()` takes about 1 s; `reset()` returns about 8 s after the pulse; opening the port does not reset the badge.
- `common.py` also has `test_config()` and `hold_ms(badge)`. `vectors.json` has `tx.mint` (base58), which the test provisioning in section 1 uses.
- Routes are iterated with `router::firstRoute()` or `Registered<EspnowRoute>::first()`, never `EspnowRoute::first()` (the struct's field `first` hides it).
- `approval.h` removes the Arduino core's `DISABLED` macro; a file that names `SelectRule::DISABLED` includes `approval.h` itself.
- Hook H21 is applied (`VK_SE050_QUARANTINE 1`); the expected hook list in every later check includes H21.

As built in Batch 2 (integrator I2, confirmed on the badge):

- Boot line: `[vk] registries: services=4 commands=16 lua=0 status=1 domains=1 routes=0 permissions=0 patterns=5 native=0 config=7`. A later batch's counts only grow from these.
- The device tests leave the badge **provisioned with the test values** (`approval_tmo_s` 10, `hold_ms` 1000). `provision_test` resets and provisions again, so a test that needs a known state calls it.
- `RESULT` stays on screen while any key is down. A test that wants a screenshot of the result presses a key without releasing it (`btn(key, "press")`), takes the shot, then releases. For a timeout, hold `up`, which the engine ignores.
- `VKSET theme receipt-dark` takes effect within 500 ms, with no reboot.
- Enumerator names to avoid: `CHANGE`, `RISING`, `FALLING`, `DISABLED` (Arduino macros) and, in host suites, `DOMAIN` (a macOS `math.h` macro).
- Kit functions leave the canvas on `Font0`, size 1, top-left datum; code that sets a font puts it back. Text with `·`, `◂` or `▸` must be drawn by the kit, not by `display::text`.
- **No stamps.** The product owner removed the rubber stamp from the design during Batch 2: `receipt::stamp` and `vk.ui.stamp` do not exist, and the approval screen draws none. The tokens `STAMP_OK`, `STAMP_WARN`, `STAMP_BAD` stay, as the status inks for signed, warning and blocked text in lists. In `RESULT` the footer's left text is the result word and its right text is empty.
- Hook H21 now sits in `src/hal/se050.cpp`, `se050_t1.cpp` and `badge_i2c.cpp`, not in `os.ino`. The I²C bus is healthy and the real keys work; a person may press them during a test run.

`test/device/fixtures.py` (3C): `build_transfer_msg`, `make_record`, `make_req`, `case_lua(dict)`.

As built in Batch 3 (integrator I3, confirmed on the badge):

- Boot line: `[vk] registries: services=7 commands=16 lua=29 status=2 domains=4 routes=3 permissions=2 patterns=5 native=0 config=17`.
- `fixtures.py` also has the helpers the tests share for driving `checktest`: `Keys`, `install_checktest`, `push_case`, `start_case`, `open_case`, `cancel`, `poll_result`, `ct_wait`, `ct_ticks`, `log_ms`, `lines_of`, `set_time`, `badge_pubkey`, `short_address`, `verify`. `vkdev.py` has no delete and no single-file push: a test sends `AUTH` + `DEL <id>` through `cmd`, and `push_case` pushes a temporary folder holding only `case.lua` with `app_id="checktest"`.
- `common.launch()` returns only when the app is running with no approval open, so a `checktest` case waits `delay_ms` (1500) before it calls `begin_solana`. `begin_solana` with a record and a request blocks the loop for about 0.85 s; the helpers wait for the `CT begin` log line and send nothing meanwhile.
- One verification is 419 ms and one signature 211 ms (TweetNaCl, software key). A `begin` verifies each supplied signature once.
- The receipt header's right side (`receipt::statusRight`) is the time and the battery only, and `USB` on external power; confirmed on the badge as `03:00 · USB` and, with no clock source, `USB` (`shots/header_clock_usb.png`, `header_noclock_usb.png`). No shipped screen calls it yet (the launcher of WP37 is the first). `t_cfg.py`'s SETUP check looks at upstream's bar (status item `setup`, hook H14), which is unchanged; it will need rewriting when the receipt launcher replaces upstream's.
- The device tests leave about 55 history records per full run; a history append then costs about 145 ms (stores.md).
- `checktest`, `reqtest` and `hello` stay installed on the badge after the tests.

As built in Batch 4 (integrator I4, confirmed on the badge):

- Boot line: `[vk] registries: services=8 commands=16 lua=37 status=4 domains=5 routes=3 permissions=9 patterns=6 native=3 config=17`. The three native apps are `hello_native`, `inbox` and `wallet_settings`.
- `VK_ED25519_BACKEND` is 1 (Monocypher). One verification is 18 ms (it was 419 ms); one signature is still 211 ms. Image 1,998,531 bytes.
- Permissions are enforced. A test launches an app with `common.launch`, which answers the consent prompt; `badge.run` is for launches that are expected to be refused or that end at once. Consent is erased by `provision_test` (through `VKRESET`).
- A refused launch logs `[vk] launch of '<id>' refused: <error>`; a test reads that line, not the error screen.
- A pushed folder with a native app's id is ignored and the native app launches (`[vk] ignoring pushed app '<id>': the id belongs to a built-in app`). While a native app object exists, `granted()` answers from its `BADGE_APP` line, `on_stop` included.
- `wallet.time()` exists (unix seconds or nil); `vk.ui.status()` uses it.
- An app that requires `vk` is pushed with `extra={"vk.lua": <lib/vk.lua>}` (`t_vk.py`, `t_sign_net.py`) or with `scripts/push-apps.sh`.
- `test/device/fixtures/` (app folders pushed by `t_app.py`) sits beside `fixtures.py`. `from fixtures import …` resolves to the module as long as nobody adds an `__init__.py` to the folder.
- `t_notify.py` resets the badge first, so the clock is unset afterwards. `t_con_single.py` and `t_con.py` push their own helper app `contest`, which stays installed, as do `vktest` and the four fixtures of `t_app.py` (`noperm`, `needsign`, `minapi99`, `nonet`).
- `rename` over an existing file works on the badge's LittleFS (`t_con_single.py`).
- The Batch 4 integration ran a reduced device-test set by the product owner's decision: `t_sto.py`, `t_req_single.py` and `t_clock.py` were not rerun against this build and are left to the final regression (I6).

As changed by the Batch 5 design change (specified; integrator I5 confirms on the badge):

- `VKSTATE` has a field `screen`: the shell's current screen (`launcher`, `app_delete`, `settings`, `wifi`, `bluetooth`, `espnow`, `push`, `store`, `identity`, `identity_new`, `display`, `leds`, `info`, `console`, `app_error`, `offer`, `installing`), or `""` while an app runs. Tests use it instead of comparing screenshots to know where they are. "The launcher" is `app == ""` and `screen == "launcher"`; there is no launcher app.
- The boot line has no `status=` count and has `pages=13` (settings pages).
- Upstream's sample apps (`hello`, `dice`, `gallery`, `radar`, `vumeter`, `whosnear`) are deleted. The smallest app to launch in a test is the native `hello_native` or a pushed fixture.
- Nothing reads upstream's status bar any more (it is never drawn): `t_cfg.py`'s SETUP check uses `VKSTATE` `provisioned`. The header shows only the time and the battery.
- Screenshots of shell screens are saved as `test/device/shots/shell_<screen>_<theme>.png`.
- Never send SELECT on screen `identity_new`: it replaces the badge's key.

### 5.3 Who registers what

| Kind | Name → owner file |
|---|---|
| Config keys | `listener_url`, `display_name`, `rpc_url` → `core/config.cpp` · `ntp_server` → `core/clock.cpp` · `approval_tmo_s`, `hold_ms` → `wallet/approval.cpp` · `theme` → `ui/theme.cpp` · `issuer_key`, `tokens`, `record_ttl_s` → `features/solana_pay/domain_solana.cpp` · `presence_ms`, `req_ttl_s`, `req_period_ms`, `req_max_proofs`, `req_gap_ms`, `pay_app` → `features/requests/` · `balance_poll_s` → `features/balance/` |
| Serial commands | `VKHELP`, `VKINFO` → `core/serial.cpp` · `VKKEYS`, `VKGET`, `VKSET`, `VKCOMMIT`, `VKRESET`, `VKWIFI`, `VKAUTOSTART` → `core/config.cpp` · `VKSTATE`, `VKBTN`, `VKSHOT`, `VKTIME`, `VKPAIR`, `VKNOTE` → `features/devtools/devtools.cpp` · `VKDEMOAPPROVE` → `features/devtools/demo_approve.cpp` |
| Info fields | `profile`, `api` → `serial.cpp` · `provisioned`, `wifi` → `config.cpp` · `pubkey`, `key`, `selfcheck` → `signer.cpp` · `time` → `clock.cpp` |
| Signing domains | `store-reg` → `features/store_reg/domain_store_reg.cpp` · `solana` → `features/solana_pay/domain_solana.cpp` · `pay-req`, `pay-proof` → `features/requests/domain_pay_*.cpp` · `contact` → `features/contacts/domain_contact.cpp` · `bank` → `features/bank/domain_bank.cpp` |
| Lua functions | `wallet.pubkey/address/key_location/provisioned/time_ok/time/tokens/config/begin/poll`, `codec.*` → `wallet/lua_wallet.cpp` · `wallet.check_record/build_transfer/begin_solana/wire_tx` → `features/solana_pay/lua_solana.cpp` · `wallet.request_open/request_close/request_status/requests/challenge/presence` → `features/requests/lua_requests.cpp` · `wallet.history` → `features/history/lua_history.cpp` · `wallet.balance/token_account/refresh_balance` → `features/balance/lua_balance.cpp` · `wallet.contact_hello/contact_card/contact_accept/contacts/contact_remove` → `features/contacts/lua_contacts.cpp` · `theme.name/color` → `ui/lua_theme.cpp` · `wallet.begin_bank` → `features/bank/` |
| Permissions | `sign`, `net`, `espnow`, `ble`, `mic`, `storage` → `host/permissions.cpp` · `request` → `features/requests/` · `history` → `features/history/` · `contacts` → `features/contacts/` |
| Settings pages (id, order → file under `shell/pages/`) | 5F: `theme` 10 → `page_theme.cpp` · `wifi` 20 → `page_wifi.cpp` · `bluetooth` 30 → `page_bluetooth.cpp` · `espnow` 40 → `page_espnow.cpp` · `push` 50 → `page_push.cpp` · 5G: `store` 60 → `page_store.cpp` · `identity` 70 → `page_identity.cpp` (also the screen `identity_new`) · `display` 80 → `page_display.cpp` · `leds` 90 → `page_leds.cpp` · `wallet` 100 → `page_wallet.cpp` · `inbox` 110 → `page_inbox.cpp` · `info` 120 → `page_info.cpp` · `console` 130 → `page_console.cpp`. `theme`, `wallet` and `inbox` are action rows (`VK_SETTINGS_ACTION`) and never become a screen |
| Shell screens that are not pages | `launcher` → `shell/launcher.cpp` · `settings` → `shell/settings_list.cpp` · `app_delete`, `app_error`, `offer`, `installing` → `shell/dialogs.cpp` (all 5A) |
| LED patterns | `approve_green`, `approve_amber`, `approve_red`, `signed`, `refused`, boot bar → `ui/leds.cpp` · `notify` → `host/notify.cpp` |
| Routes | types 1, 2, 3 → `features/requests/` |
| Listeners | `VK_ON_APP_STOP`: approval, permissions, requests · `VK_ON_APPROVAL`: LEDs (in `leds.cpp`), history · `VK_ON_RESET`: consent |

## 6. Integrator procedure

Referenced by every integrator brief. Integrators are the only agents that run `arduino-cli`, use the serial port, edit `docs/`, and commit.

1. **Collect.** Read the batch's implementer reports (the orchestrator pastes them into your prompt). Run `git status --short` in `REPO`. Every changed file must belong to an agent of this batch; investigate anything else before continuing.
2. **Host tests.** `cd "$FW" && test/host/run.sh`. Fix failures in the owning file.
3. **Build.** `cd "$FW" && scripts/build.sh dev`, with a 10-minute timeout. The first build of a batch is a full compile; later ones are incremental.
   - Fix each compile or link error with the smallest change in the file that owns the mistake.
   - If two files disagree, the spec decides; if the spec is silent, section 4 decides.
   - If a spec interface is itself wrong, fix the document in the same change and list it in your report.
   - Never fix by editing an upstream file outside a listed hook.
   - If the pre-flash check fails, fix the cause, not the check.
4. **Flash.** `scripts/build.sh dev --upload /dev/cu.usbserial-10`. If the upload cannot connect, retry once. If it fails again, write "upload failed: needs BOOT1/RST1 by hand" in the tracking notes, treat every device test of this batch as deferred (hands), and continue with step 8. Do not loop.
5. **Wait.** `"$PY" scripts/vkdev.py --port /dev/cu.usbserial-10 wait-ready --timeout 40`.
6. **Device tests.** Run the tests your brief lists, one `vkdev.py … test` invocation per file, in the order given. Always run `t_boot.py` first and copy the `[vk] registries:` line into the tracking notes; a count that did not grow as the brief expects means a registration was dropped (see Risk 2).
7. **Failures.** For each failing test, decide whether the test or the firmware is wrong; the spec decides. Fix, rebuild, reflash, rerun.
   - At most **3 build-flash cycles per failing test.** After that, record the failure (test id, observed, expected, your best diagnosis) in the tracking notes and move on.
   - A crash log containing `Stack canary` or `stack overflow` in `loopTask` is Risk 5: apply its fallback at once.
8. **Temporary files.** Delete any temporary test file your brief had you add, then run `scripts/build.sh dev` once more (compile only).
9. **Docs.** In `docs/os/`:
   - Apply each `SPEC ISSUES` item the implementers reported, and every interface you changed.
   - Remove `[UNVERIFIED]` tags that hardware settled, or apply the stated fallback and say which.
   - In `roadmap/implementation-plan.md`, fill the tracking row of every WP in the batch:
     - Owner: batch and agent ids.
     - Started: date.
     - Tests passed: the host suites and T-ids that passed on the badge today.
     - Notes: deferred T-ids with what they need, fallbacks applied, image size from the build output, any recorded failure.
   - A WP whose remaining tests are all deferred is marked "code complete; device verification deferred".
10. **Commit.** `cd "$REPO" && git status --short` (check nothing unexpected, no `vk_profile.h`, no `build/`), then `git add -A && git commit -m "<message from your brief>"`. Branch `badge-os`. Never push. Never amend an earlier batch's commit.
11. **Report**, exactly these headings:
    - `BUILD` — result and image size.
    - `FIXES` — file, what, why.
    - `DEVICE TESTS` — per file: PASS/FAIL/SKIP, with the output line.
    - `MEASUREMENTS`
    - `SPEC CHANGES`
    - `RECORDED FAILURES`
    - `DEFERRED`
    - `COMMIT` — hash.
    - `GATE` — met or not, and why.

## 7. Briefs

Each block is pasted verbatim as the agent's prompt. Integrator prompts additionally get the batch's implementer reports appended by the orchestrator.

### Batch 0

```text
BRIEF 0 — Fork agent (runs alone; may compile and commit; must not use the serial port)

You are preparing the repository for a team of coding agents that will build "BadgeOS", firmware for an ESP32-S3 badge, as a fork of the upstream "Solana OS" firmware. REPO=/Users/ayush/Downloads/My Projects/Hackathons/MHacks2026 (the path contains a space: quote it everywhere). FW=$REPO/os. UPSTREAM=/private/tmp/claude-502/-Users-ayush-Downloads-My-Projects-Hackathons-MHacks2026/41bb931f-51cc-40a5-9e9e-f24006cd3c71/scratchpad/upstream/firmware/solana-os. Git branch badge-os is checked out; never push.

Read first: $REPO/docs/os/roadmap/execution-plan.md sections 1, 4 and 6; $REPO/docs/os/README.md; $REPO/docs/os/guides/build-flash-provision.md.

Do, in this order:
1. mkdir -p "$REPO/firmware" and cp -R "$UPSTREAM" "$FW" (including dotfiles; keep src/lua/). Verify with diff -r that the copy equals UPSTREAM. Commit only that: git add os && git commit -m "WP01: upstream Solana OS 812b8c7, unmodified".
2. Prove the in-place build. FQBN="esp32:esp32:esp32s3:PSRAM=opi,FlashSize=16M,PartitionScheme=custom,CDCOnBoot=cdc". Run arduino-cli compile --fqbn "$FQBN" --build-path "$FW/build/dev" "$FW" (allow 10 minutes). If arduino-cli refuses a build path inside the sketch, use "$HOME/Library/Caches/badge-os/dev" instead. If the compile fails because of the space in the path, create a symlink directory without spaces (/tmp/badge-os/solana-os -> $FW) and compile through it. Record exactly which sketch path and build path worked, the compile time and the image size.
3. In docs/os/guides/build-flash-provision.md: fill the toolchain version table (arduino-cli 1.5.1, esp32:esp32 3.3.12, LovyanGFX 1.2.32, today's date) and add a short subsection "Build directory" under "Build profiles" stating the sketch path and --build-path that scripts/build.sh must use (one directory per profile), from step 2. Remove the [UNVERIFIED] note about the core version; mark U1 settled in docs/os/README.md.
4. Apply every row F1 to F13 of section 1 of the execution plan to the named documents, and add the "Corrections to the dependency table" paragraph under the at-a-glance table in docs/os/roadmap/implementation-plan.md. For F2, F3, F4, F5 and F11 copy the ADDED declarations from section 4 of the plan into the named document so the documents and the plan agree word for word. Change nothing else in the spec.
5. Commit: git add -A && git commit -m "WP00: spec fixes from the execution plan; toolchain versions; build directory" (this also commits docs/os/roadmap/execution-plan.md). Check git status first: no build/ directory and no vk_profile.h may be staged.

Do not: edit any file under os after step 1, flash, open the serial port, or push.

Final report: the two commit hashes; the diff -r result; the exact compile command that worked, its time and the image size; the list of documents changed with one line each; anything in section 1 you could not apply and why.
```

### Batch 1

```text
BRIEF 1A — Hooks, vk core, build scripts (WP01, part 1)

You are one of four coding agents working at the same time on BadgeOS, firmware for an ESP32-S3 badge built as a fork of "Solana OS". REPO=/Users/ayush/Downloads/My Projects/Hackathons/MHacks2026 (contains a space: quote it). FW=$REPO/os, which already holds unmodified upstream. First read $REPO/docs/os/roadmap/execution-plan.md sections 3, 4 and 5, then docs/os/README.md.

Goal: apply every upstream hook exactly as specified and write the core of src/vk/ so that, together with agent 1B's stubs and agent 1C's pure headers, the firmware compiles and behaves exactly like upstream, and logs one line counting every registry.

Read: docs/os/architecture/overview.md (all), architecture/upstream-hooks.md (all; this is your main input), architecture/upstream-baseline.md, platform/config.md (Config store, Serial commands), wallet/checks.md (Clock), guides/build-flash-provision.md (Build profiles, Build directory, Pre-flash checks), roadmap/implementation-plan.md (WP01).

Files you own (paths relative to FW; touch nothing else):
- Upstream edits, hooks H1 to H17, H19, H20 only (not H18), each line tagged "// VK: H<n>" exactly as in upstream-hooks.md: os.ino, src/lua_sdk/lua_bindings.cpp, src/lua_sdk/lua_runtime.cpp, src/net/broker_client.cpp, src/apps/app_store.cpp, src/hal/se050_apdu.h, src/net/espnow_mgr.cpp, src/net/espnow_mgr.h, src/hal/display.cpp, src/ui/boot.cpp, src/config.h, src/hal/buttons.cpp, src/ui/shell.cpp.
- UPSTREAM-HOOKS.md (copy of the hook table).
- src/vk/vk.h, src/vk/vk.cpp, src/vk/vk_build.h, src/vk/core/registry.h, core/service.h, core/serial.h, core/serial.cpp, core/config.h, core/config.cpp (stub), core/clock.h, core/clock.cpp (stub), core/fileio.h, core/fileio.cpp.
- scripts/build.sh, scripts/preflash-check.sh.

Implement:
- registry.h and service.h verbatim from overview.md sections 6 and 7.
- vk_build.h, serial.h, config.h, clock.h, fileio.h exactly as section 4 of the execution plan says (spec blocks plus the ADDED declarations there). ConfigKey needs a constructor taking its seven fields in declaration order.
- serial.cpp complete: handleLine (first word, case-insensitive, matched against registered commands; returns false for anything else so upstream's protocol gets the line), VKHELP, VKINFO, info fields "profile" and "api" (config.md, Serial commands).
- vk.cpp: begin() calls vk::config::begin(), then every Service::begin, then logs with badge_log::tagf("vk", ...) exactly: "registries: services=<n> commands=<n> lua=<n> status=<n> domains=<n> routes=<n> permissions=<n> patterns=<n> native=<n> config=<n>". update() runs every Service::update. modalActive() and modalUpdate() forward to vk::wallet::approval::active() and update(). Include the headers of every registry you count (1B writes them at the same paths as in section 4).
- config.cpp and clock.cpp as stubs with the behaviour in section 4. config.cpp defines confirmChange as null.
- fileio.cpp: the firmware Ops (POSIX stdio on FS_ROOT + path; writeAt opens "r+b"; makeDir succeeds if the directory already exists).
- build.sh <dev|release> [--upload <port>]: writes src/vk/vk_profile.h (VK_PROFILE_DEV 1 or 0), runs scripts/preflash-check.sh <profile>, runs arduino-cli compile with the FQBN, sketch path and --build-path recorded in the build guide's "Build directory" subsection (one build directory per profile, so builds are incremental), prints the image size, and with --upload runs arduino-cli upload for that port from the same build directory. It must work when called from any directory and with the space in REPO.
- preflash-check.sh <profile>: checks 1 to 6 of the build guide. Check 5 runs test/host/run.sh only if that file exists. Check 1 uses the grep in upstream-hooks.md ("Checking the hooks") and compares with the ids in UPSTREAM-HOOKS.md.

Consumes: headers 1B writes (src/vk/host/*.h, ui/*.h, wallet/*.h, sdk/badge_sdk.hpp) and 1C writes (src/vk/wallet/pure/*.h). They do not exist yet; write against section 4.

Verification you can do: bash -n on both scripts; run scripts/preflash-check.sh dev and confirm check 1 passes and prints the expected id list from upstream-hooks.md; diff every upstream file against UPSTREAM (path in section "paths" of the execution plan) and confirm every changed line carries a VK tag. There is no host suite for your files.

Do not: run arduino-cli or scripts/build.sh, open the serial port, apply H18, edit any file outside your list (including docs/), or run git add/commit.

Final report: the format in section 3, rule 13, plus the output of the hook-id grep.
```

```text
BRIEF 1B — Host, UI, wallet and SDK stubs (WP01, part 2)

You are one of four coding agents working at the same time on BadgeOS, firmware for an ESP32-S3 badge built as a fork of "Solana OS". REPO=/Users/ayush/Downloads/My Projects/Hackathons/MHacks2026 (contains a space: quote it). FW=$REPO/os. First read $REPO/docs/os/roadmap/execution-plan.md sections 3, 4 and 5, then docs/os/README.md.

Goal: create every header under src/vk/host, ui, wallet and sdk with its full, final declarations, and the stub or complete bodies listed in section 4, so that every later agent compiles against stable interfaces and the firmware still behaves exactly like upstream.

Read: docs/os/architecture/overview.md, architecture/upstream-hooks.md (rule 5 and hooks H3, H7, H8, H10, H11, H14, H15, H19, H20: they call your functions), architecture/upstream-baseline.md, wallet/signing.md, wallet/approval.md (The request), protocol/espnow.md (Router), platform/app-host.md (all code blocks), platform/native-apps.md (The SDK header), ui/ui.md (LED patterns, Status bar, Tokens and modes, The receipt kit), roadmap/implementation-plan.md (WP01: "Stub behaviour").

Files you own (relative to FW/src/vk; touch nothing else):
host/lifecycle.h, lifecycle.cpp (complete) · host/lua_registry.h, lua_registry.cpp (complete except permission filtering) · host/router.h, router.cpp (stub) · host/permissions.h, permissions.cpp (stub) · host/manifest.h · host/consent.h · host/native.h, native.cpp (stub) · host/notify.h, notify.cpp (stub) · host/home.h, home.cpp (stub) · ui/statusbar.h, statusbar.cpp (complete) · ui/theme.h, theme.cpp (complete registry; one theme "solana" with upstream's colours) · ui/leds.h, leds.cpp (stub) · ui/boot_screen.cpp (stub: bootScreen returns false) · ui/receipt.h · ui/approval_screen.h · sdk/badge_sdk.hpp · wallet/reason.h · wallet/signer.h, signer.cpp (stub) · wallet/signer_internal.h · wallet/crypto.h · wallet/approval.h, approval.cpp (stub).

Implement: each header exactly as section 4 of the execution plan names it (spec block verbatim, plus the ADDED declarations given there). Stub bodies exactly as section 4's last column and implementation-plan WP01 "Stub behaviour" say. Points that are easy to get wrong:
- router::install() must install the forwarding lambda quoted in WP01; signStoreRegistration must return identity::signBase64(message), and signer.cpp is the only file in your set that includes src/identity/identity.h.
- lua_registry.cpp: open(L) runs with the badge table on top of the stack and leaves it there; for each registered LuaFunction it creates badge.<module> on first use and sets the function; it sets the globals loadfile and dofile to nil; it calls vk::host::promotePending().
- statusbar.cpp: draw(rightEdgeX) sorts items by order (lower is further right), draws right to left 8 px apart at y=7, stops before x=110; requestShellRepaint/consumeShellRepaint are a flag.
- native.cpp stub: count 0, exists false, active false; infoAt/infoById return false. Also define badge::exit() here (calls runtime::requestStop()).
- SignDomain, LedPattern, StatusItem and the other registered structs need the constructors the spec calls for.
- theme.cpp: color() must be callable before vk::begin() (it is used during boot later).

Consumes: src/vk/core/*.h and vk.h, vk_build.h from agent 1A; src/vk/wallet/pure/*.h from agent 1C (signer.h needs vk_presence_t from pure/vk_checks.h; reason.h includes pure/vk_reason.h). They are being written now; use the paths and names in section 4.

Verification you can do: there is no host suite. Check every header for self-sufficiency by reading its includes; check each hook call in upstream-hooks.md against your signatures character by character.

Do not: run arduino-cli or scripts/build.sh, open the serial port, edit any file outside your list (upstream, core/, pure/, docs/), or run git add/commit.

Final report: the format in section 3, rule 13.
```

```text
BRIEF 1C — Pure code and host-test infrastructure (WP02)

You are one of four coding agents working at the same time on BadgeOS, firmware for an ESP32-S3 badge. REPO=/Users/ayush/Downloads/My Projects/Hackathons/MHacks2026 (contains a space: quote it). FW=$REPO/os. First read $REPO/docs/os/roadmap/execution-plan.md sections 3, 4 and 5 (5.1 is yours to build), then docs/os/README.md.

Goal: every host-testable C99 module of the wallet exists under src/vk/wallet/pure/ and passes its suite on this laptop, and the host-test runner and shim that every later agent will use exist and are complete.

Read: docs/os/wallet/solana-payments.md (all), wallet/checks.md (all), protocol/espnow.md (Frame header, Frames, Codec), wallet/signing.md (Reason codes), reference/reasons.md, testing/testing.md (Host tests), roadmap/implementation-plan.md (WP02). Starting code: $REPO/docs/os/reference/code/ (first build and run its test unchanged with the command in docs/os/README.md and confirm "all sol tests passed").

Files you own (relative to FW; touch nothing else):
- src/vk/wallet/pure/sol.h, sol_b58.c, sol_sha256.c, sol_tx.c (copied from the reference, then changed as solana-payments.md says; remove the curve, PDA, ATA and SAS declarations and constants that are no longer defined) · vk_reason.h, vk_reason.c · vk_record.h, vk_record.c · vk_frames.h, vk_frames.c · vk_checks.h, vk_checks.c.
- test/host/run.sh · test/host/shim/Arduino.h, Preferences.h, vk_host_fileio.h and their .cpp files · test/host/host_ed25519.h, host_ed25519.c · test/host/test_sol.c, test_record.c, test_frames.c, test_checks.c · test/host/vectors.mjs, vectors-to-h.mjs, vectors.json, vectors.h.

Implement: every declaration in the C blocks of solana-payments.md ("Changes to the reference sol.h", "Builder", "Token table"), checks.md ("Registry record", "Presence lookup", "The check chain") and espnow.md ("Codec"), unchanged, in headers with extern "C" guards and no Arduino include. Decoder rules 1 to 12, check-chain rows and both verdict tables exactly as tabulated. vk_reason_name returns the lower-case names in reasons.md order. Section 5.1 of the execution plan fixes run.sh's interface, the // LINK: convention, the flags, host_ed25519.h, the shim's test hooks and the names in vectors.json and vectors.h: implement exactly those.
Shim details: String must offer construction from const char*, std::string and integers; length, c_str, operator+ and +=, ==, !=, [], substring, indexOf (char and String), lastIndexOf, startsWith, endsWith, trim, toUpperCase, toLowerCase, toInt, equalsIgnoreCase, concat, reserve. The in-memory fileio must implement every member of vk::fileio::Ops in section 4 (declare the struct by including FW/src/vk/core/fileio.h, which agent 1A writes; until it exists use the declaration in section 4). The NVS shim must implement the Preferences methods begin, end, getString, putString, isKey, remove, clear, with vk_host_nvs_fail_writes making writes return 0.
Vectors: extend vectors.mjs (run from $REPO/dashboard so it resolves that folder's node_modules; run npm install there if @solana/kit is missing) with the Memo message, a fixed test issuer key pair and device key pair, a canonical record with its signature over "registry:"+record, a signed REQ, and a PROOF. Regenerating must reproduce the committed files byte for byte.

Host tests (write each before the code it tests): test_sol (solana-payments.md, Tests: every bullet, one negative vector per decoder rule, 100 random builder round trips with and without memo), test_record and test_checks (checks.md, Tests: one test per row of both tables, the cap, green), test_frames (round trip per type, each truncated and over-long copy refused, signed_len, signed-bytes helpers match the vectors), and inside test_checks or test_sol a check that every vk_reason_t has a distinct lower-case name.
Command: cd "$FW" && test/host/run.sh test_sol test_record test_frames test_checks   (expect four "all … tests passed" lines, exit 0). Also run test/host/run.sh with no arguments and confirm it runs exactly these four.

Do not: run arduino-cli or scripts/build.sh, open the serial port, edit any file outside your list (including docs/os/reference/ and docs/), or run git add/commit.

Final report: the format in section 3, rule 13.
```

```text
BRIEF 1D — Dev tools and the serial tool (WP03)

You are one of four coding agents working at the same time on BadgeOS, firmware for an ESP32-S3 badge built as a fork of "Solana OS". REPO=/Users/ayush/Downloads/My Projects/Hackathons/MHacks2026 (contains a space: quote it). FW=$REPO/os. First read $REPO/docs/os/roadmap/execution-plan.md sections 3, 4 and 5 (5.2 is yours to build), then docs/os/README.md.

Goal: a script can flash-wait, read the badge's state, press buttons, take screenshots, push apps and provision over USB serial with no person present.

Read: docs/os/testing/testing.md (Dev hooks, The serial tool, Acceptance tests: Boot), platform/config.md (Provisioning, Serial commands), architecture/upstream-hooks.md (H6, H17), architecture/upstream-baseline.md, roadmap/implementation-plan.md (WP03). Upstream files to read: FW/src/net/push_protocol.h and .cpp (AUTH, BEGIN, DATA, END, RUN, STOP), FW/src/hal/buttons.cpp (update), FW/src/hal/display.h, FW/src/badge_log.cpp.

Files you own (relative to FW; touch nothing else): src/vk/features/devtools/devtools.h, devtools.cpp · scripts/vkdev.py · test/device/common.py · test/device/t_boot.py.

Implement:
- devtools.h: the extern "C" declaration in hook H17. devtools.cpp: everything inside #if VK_TEST_HOOKS (include ../../vk_build.h); in the release profile the file must compile to nothing.
  - vk_dev_apply_injected_buttons: a queue of timed press and release events applied to the three masks exactly as H17's last paragraph says (force the bit on in *down, clear it in *released, set *pressed only on the pass the press begins; on release set *released for one pass).
  - Commands VKSTATE, VKBTN, VKSHOT, VKTIME, VKPAIR, VKNOTE with VK_SERIAL_COMMAND, replies and formats exactly as the Dev hooks table and the VKSTATE JSON in testing.md. VKSTATE reads vk::wallet::approval::current(), phase(), peekResult(), vk::clock::source(), vk::host::notify::count(), vk::config::provisioned(), vk::host::native::active(), runtime::currentApp(), ESP.getFreeHeap(). VKTIME calls vk::clock::devSet. VKSHOT must output true RGB565 little-endian: LovyanGFX sprites store 16-bit pixels byte-swapped, so read pixels with canvas.readPixel(x, y) (or swap the buffer bytes) and say which you did.
- vkdev.py: Python 3 with pyserial only. Subcommands info, cmd, wait-ready, reset, state, btn, shot, push, run, monitor, test, provision, and the Badge class, exactly as section 5.2 and testing.md "The serial tool" (serial push rules included). Open the port without toggling the reset line (create serial.Serial() unopened, set dtr and rts False, then open); reset() pulses RTS the way esptool's hard reset does. provision as config.md "With the tool" (issuer key = last 32 bytes of the 64-byte keypair file, or --issuer).
- common.py: the helpers named in section 5.2, with test-provisioning values from section 1 of the plan ("Choices made").
- t_boot.py: T-BOOT1 (reset; "[os] ready" within 30 s; PING answers "OK pong"; the log holds a "[vk] registries:" line and commands>=2), VKHELP lists VKHELP and VKINFO, VKINFO has profile=dev and api=2, VKSTATE parses as JSON, then navigation: stop any app, shot, btn down tap, shot, assert the two screenshots differ.

Consumes (being written now by agents 1A and 1B; use the names in section 4): src/vk/core/serial.h, core/clock.h, core/config.h, wallet/approval.h, host/notify.h, host/native.h, vk_build.h.

Verification you can do: python3 -m py_compile on the three Python files; a self-test in vkdev.py (--selftest) that round-trips the VKSHOT run-length decoding and PNG writing on synthetic data and the base58 helpers, run with "$REPO/.venv/bin/python" scripts/vkdev.py --selftest.

Do not: run arduino-cli or scripts/build.sh, open the serial port (not even to list it), edit any file outside your list, or run git add/commit.

Final report: the format in section 3, rule 13.
```

```text
BRIEF I1 — Integrator, Batch 1

You integrate the first batch of BadgeOS, firmware for an ESP32-S3 badge built as a fork of "Solana OS". REPO=/Users/ayush/Downloads/My Projects/Hackathons/MHacks2026 (contains a space: quote it). FW=$REPO/os. PY=$REPO/.venv/bin/python. One badge is on /dev/cu.usbserial-10; you are the only agent allowed to compile, flash, use the port, edit docs/ and commit. Read $REPO/docs/os/roadmap/execution-plan.md sections 1, 4, 5 and 6 (section 6 is your procedure; follow it step by step), docs/os/README.md, architecture/overview.md, architecture/upstream-hooks.md, roadmap/implementation-plan.md (WP01 to WP03). The four implementer reports follow this brief.

Batch-specific steps, in the order of section 6:
- Before the first build, add the temporary file FW/src/vk/zz_linkcheck.cpp containing: #include "core/service.h" / #include "../badge_log.h" / static void lcBegin() { badge_log::tagf("vk", "linkcheck alive"); } / VK_SERVICE(zz_linkcheck, lcBegin, nullptr);
- Build. Expect cross-agent mismatches between 1A, 1B, 1C and 1D; fix them in the owning file, keeping section 4 as the reference.
- Confirm every header in section 4 exists with the declarations listed. Add anything missing now: later batches depend on it.
- Flash, wait-ready, then run: "$PY" scripts/vkdev.py --port /dev/cu.usbserial-10 test test/device/t_boot.py
- Linker check: the boot log must contain "[vk] linkcheck alive". If it does not, apply the fallback in overview.md section 6 (src/vk/registry_anchor.cpp referencing one symbol per registering file), record it under U2 in docs/os/README.md, and tell the orchestrator in your report that every later brief must add its file to the anchor.
- Upstream behaviour: push and run an upstream sample ("$PY" scripts/vkdev.py --port … push apps/hello, then run hello), check VKSTATE app is "hello", stop it, confirm the launcher repaints (two screenshots differ before and after), and read the "[id]" boot line. Record the key location (secure element or software) and the public key.
- Delete zz_linkcheck.cpp; compile once more.
- Docs: README Status paragraph (the firmware now runs on a badge), open items U2 and U10, the tracking rows for WP00 to WP03, the image size in testing.md M4.
- Commit message: "WP01: batch 1, fork skeleton with all hooks, pure code, dev tools (WP01 WP02 WP03)".

Deferred in this batch: T-HOOK1 (needs a second badge), T-REL2 (release build; Batch 6).
Gate: host suites sol, record, frames, checks pass; dev build compiles and the hook check passes; t_boot.py passes, or the upload failure is recorded and the build is compile-verified.

Final report: the format in section 6, step 11, plus the "[vk] registries:" line, the "[id]" line, and whether opening the port resets the badge.
```

### Batch 2

```text
BRIEF 2A — Config store and provisioning (WP10)

You are one of five coding agents working at the same time on BadgeOS, firmware for an ESP32-S3 badge built as a fork of "Solana OS". REPO=/Users/ayush/Downloads/My Projects/Hackathons/MHacks2026 (contains a space: quote it). FW=$REPO/os. First read $REPO/docs/os/roadmap/execution-plan.md sections 3, 4 and 5, then docs/os/README.md and architecture/overview.md.

Goal: the NVS-backed config store, its serial commands and provisioning work, so a badge can be provisioned with one command and the values survive a reboot.

Read: docs/os/platform/config.md (all), ui/ui.md (Status bar), roadmap/implementation-plan.md (WP10, Review focus 3), testing/testing.md (Config tests).

Files you own (relative to FW; touch nothing else): src/vk/core/config.cpp (fill the stub; you may add declarations to core/config.h, never change existing ones) · test/host/test_config.cpp · test/device/t_cfg.py.

Implement: everything in namespace vk::config as declared in config.md "Config store"; config keys listener_url, display_name, rpc_url; commands VKKEYS, VKGET, VKSET, VKCOMMIT, VKRESET, VKWIFI, VKAUTOSTART with the exact replies in the Serial commands table; info fields provisioned and wifi; status item "setup" (order 10, "SETUP" in amber while unprovisioned; call vk::ui::requestShellRepaint() when provisioning changes). Rules: NVS namespace "vkconf" through Preferences; every write's result is checked (a failed write makes set() return STORAGE and VKSET answer "ERR nvs_full"); on a provisioned badge a secure key calls confirmChange and returns PENDING, and while confirmChange is null returns UNAVAILABLE; requestReset raises the confirmation described under Provisioning through vk::wallet::approval::confirm is NOT allowed here (core never includes a wallet header): use confirmChange for both, passing the key name "(reset)" for a reset, and document that in a comment. Accessors must work before begin() is called (they call begin() lazily; begin() is idempotent): the boot screen reads the theme before vk::begin(). The text parsers (TOKENS form, KEY32 base58, U32 range, STR length and printable ASCII) are pure functions compiled under VK_HOST_TEST.

Already on disk: core/config.h, core/registry.h, core/serial.h, ui/statusbar.h, wallet/pure/vk_checks.h (vk_token_t, VK_MAX_TOKENS), wallet/pure/sol.h (sol_b58_decode, sol_parse_amount), test/host/shim/Preferences.h.

Host tests first (test_config.cpp, first line "// LINK: src/vk/core/config.cpp"): token-table parser (valid one and three entries, bad mint, bad decimals, bad symbol, 5 entries refused, cap > max refused), KEY32, U32 ranges, STR length, set/get round trip on the in-memory NVS, set returns STORAGE when vk_host_nvs_fail_writes is set, commit reports the missing required key.
Command: cd "$FW" && test/host/run.sh test_config   (expect "all config tests passed").

Device test to write (t_cfg.py; you cannot run it): T-BOOT2 (after ensuring unprovisioned: VKSTATE provisioned false and a screenshot taken), T-CFG3 (reads VKKEYS; uses "tokens garbage" if the key tokens is listed, else "rpc_url x"; expects "ERR invalid" and the value unchanged), T-CFG1 (common.provision_test, badge.reset(), VKGET each key set, VKINFO provisioned=1), T-CFG2 (VKSET approval_tmo_s 12 answers "OK pending", VKSTATE shows title "Change setting", severity amber, select hold; btn a hold 1300 writes it; repeat with 14 and btn b tap leaves 12). T-CFG4 needs hands: leave a comment.

Do not: run arduino-cli or scripts/build.sh, open the serial port, edit any file outside your list (upstream, other agents', docs/), or run git add/commit.

Final report: the format in section 3, rule 13.
```

```text
BRIEF 2B — Signer, domain table, crypto, store registration (WP11)

You are one of five coding agents working at the same time on BadgeOS, firmware for an ESP32-S3 badge built as a fork of "Solana OS". REPO=/Users/ayush/Downloads/My Projects/Hackathons/MHacks2026 (contains a space: quote it). FW=$REPO/os. First read $REPO/docs/os/roadmap/execution-plan.md sections 3, 4 and 5, then docs/os/README.md and architecture/overview.md.

Goal: one signing path exists (signRaw in signer.cpp is the only caller of identity::sign), the domain self-check runs at boot, and upstream's store registration signs through the auto domain "store-reg".

Read: docs/os/wallet/signing.md (all), wallet/approval.md (The request: open, takeResult), platform/app-host.md (Permissions: granted), roadmap/implementation-plan.md (WP11 and the WP13 step "vk::wallet::begin now opens the approval": that step is yours). Upstream: FW/src/identity/identity.h, FW/src/identity/ed25519.h, FW/src/net/broker_client.cpp (isSafeNonce, submitRegistration).

Files you own (relative to FW; touch nothing else): src/vk/wallet/signer.cpp (fill; may add to signer.h and signer_internal.h) · src/vk/wallet/crypto.cpp (create; crypto.h exists) · src/vk/features/store_reg/domain_store_reg.cpp · test/host/test_domains.cpp.

Implement: every function declared in signing.md's "Domain table" block, "Who owns what" and "Cross-feature interfaces", following "The signing path" line by line, including begin() in full: it calls vk::config::provisioned(), vk::host::granted(domain->permission), checks busy through approval::active() and approval::peekResult(), the two length rules, domain->decode, then approval::open(request, domain, bytes, len, app_id); poll() is approval::takeResult(). signRaw logs "[vk] sign <domain> <n> bytes <ms> ms". The self-check ("Self-check", six rules) is a pure function over an array of rows, run from a VK_SERVICE begin function in signer.cpp, logging "selfcheck ok" or "[vk] DOMAIN TABLE INVALID: <rule> <name>". Info fields pubkey, key, selfcheck. keyLocation maps identity::source() to "se050", "software", "none"; maxSignBytes is 242 or 1248. crypto.cpp: verify (backend 0 calls ed25519::verify and logs "[vk] verify <ms> ms"; backend 1, under #if VK_ED25519_BACKEND == 1, calls crypto_ed25519_check from vendor/monocypher-ed25519.h, which does not exist yet and must not be included otherwise), randomBytes (esp_fill_random), vk_verify_c. domain_store_reg.cpp: one VK_SIGN_DOMAIN line and a validator requiring exactly "solana-badge-register:<own pubkey base58>:<nonce>" with upstream's isSafeNonce character set (copy that function). signStoreRegistration goes through signAuto and returns base64 (identity::base64Encode) or "".

Already on disk: wallet/signer.h, signer_internal.h, crypto.h, approval.h, reason.h, core/config.h, core/serial.h, core/service.h, host/permissions.h.

Host tests first (test_domains.cpp, first line "// LINK: src/vk/wallet/signer.cpp"): the self-check accepts the six shipped rows of signing.md's table and rejects, one case each, a duplicate name, a malformed prefix, a prefix that is a prefix of another, two button domains with an empty prefix, an auto domain without validate, a button domain without decode, and the reserved prefix "registry:". With VK_HOST_TEST, signer.cpp must compile without identity, config, approval or Arduino hardware headers.
Command: cd "$FW" && test/host/run.sh test_domains   (expect "all domains tests passed").

Do not: run arduino-cli or scripts/build.sh, open the serial port, edit any file outside your list (approval.cpp belongs to agent 2C), or run git add/commit.

Final report: the format in section 3, rule 13.
```

```text
BRIEF 2C — Approval engine, LED patterns, demo command (WP12, engine)

You are one of five coding agents working at the same time on BadgeOS, firmware for an ESP32-S3 badge built as a fork of "Solana OS". REPO=/Users/ayush/Downloads/My Projects/Hackathons/MHacks2026 (contains a space: quote it). FW=$REPO/os. First read $REPO/docs/os/roadmap/execution-plan.md sections 1 (F2, F6), 3, 4 and 5, then docs/os/README.md and architecture/overview.md (section 5).

Goal: the approval state machine: any firmware code can raise an approval with a filled struct and get a yes or no, the rules (fresh press, hold, timeout, red is closed, result ownership, keys up before closing) hold and are host-tested, and the LEDs follow.

Read: docs/os/wallet/approval.md (all), wallet/signing.md (Who owns what), ui/ui.md (LED patterns, Boot bar), platform/config.md (Provisioning: the Change setting and reset confirmations; confirmChange), architecture/upstream-hooks.md (H4, H15, H19, H20), roadmap/implementation-plan.md (WP12, Review focus 2 as corrected by F6), testing/testing.md (Approval tests).

Files you own (relative to FW; touch nothing else): src/vk/wallet/approval.cpp (fill; may add to approval.h) · src/vk/ui/leds.cpp (fill; may add to leds.h) · src/vk/features/devtools/demo_approve.cpp · test/host/test_approval.cpp · test/device/t_apr.py.

Implement:
- approval.cpp: everything in approval.md "The request" block with the "State machine" and every bullet under "Rules" and "Dev builds". The engine reads time, button state, config values, the signer (signForApproval from signer_internal.h), the drawing function and LED playback through function pointers that the firmware fills from a VK_SERVICE begin function and the host test fills with fakes. Each pass in update() it calls vk::ui::drawApproval(request, phase, holdProgress, outcome, footerBlink) (signature in section 4; agent 2D implements it). Before signing it draws "Signing..." and calls display::flush(). It registers config keys approval_tmo_s and hold_ms, a VK_ON_APP_STOP listener calling appStopping (ignore an empty app id), and at boot sets vk::config::confirmChange to a function that raises the "Change setting" confirmation (or "ERASE WALLET CONFIG" when the key is "(reset)") exactly as config.md "Provisioning" describes. On open it saves the backlight and sets settings::brightness(); on close it restores it and calls vk::ui::requestShellRepaint(). Strings are sanitised to printable ASCII. Results: stored until taken, dropped after 60 s or when the owning app stops.
- leds.cpp: play, stop, playing, bootProgress and the service described under "LED patterns" (call ::leds::stopAnimation() on every frame drawn; when a pattern ends turn the LEDs off and, if vk::host::idle(), call ::leds::playIdle()); the five patterns approve_green, approve_amber, approve_red, signed, refused with the looks and durations in the table; the boot bar exactly as the pseudocode (colour = vk::ui::theme::color(LED)); a VK_ON_APPROVAL listener that plays signed or refused. Every loop over LEDs uses RGB_LED_COUNT.
- demo_approve.cpp (inside #if VK_TEST_HOOKS): VKDEMOAPPROVE <green|amber|red> calls approval::confirm with a sample request (title "Pay", big "10.00 HACK", sub "to Demo Merchant", two lines; green press, amber hold, red with red_reason VK_UNVERIFIED).

Already on disk: wallet/approval.h, signer.h, signer_internal.h, ui/approval_screen.h, ui/leds.h, ui/theme.h, ui/statusbar.h, host/lifecycle.h, host/home.h, core/config.h, core/service.h.

Host tests first (test_approval.cpp, first line "// LINK: src/vk/wallet/approval.cpp", fake clock and fake buttons): fresh-press rule (a key held at open never approves); PRESS; HOLD released early returns to ARMED; HOLD completes after hold_ms; DISABLED does not sign; timeout; RED closes with red_reason whether by CANCEL or timeout; dev override only when dev_overridable; confirm path calls done(true) and done(false); listeners are called once per outcome; RESULT waits 800 ms and for all keys up; appStopping during WAIT_RELEASE, ARMED, HOLDING and with an un-taken result; result dropped after 60 s; open returns false while active; takeResult hands SIGNED and FAILED out once.
Command: cd "$FW" && test/host/run.sh test_approval   (expect "all approval tests passed").

Device test to write (t_apr.py; you cannot run it; it may assume common.provision_test was run, so approval_tmo_s is 10 and hold_ms 1000): for each of green, amber, red: VKDEMOAPPROVE, assert VKSTATE modal, phase, severity, select, title, headline, big, sub, lines; save a screenshot to test/device/shots/approval_<sev>_<theme>.png; close. T-APR2 (btn a press, then VKDEMOAPPROVE green, phase stays WAIT_RELEASE for 2 s, btn a release, phase ARMED). T-APR3 (open amber, wait 12 s, modal false). While modal: send STOP and RUN hello and assert the app changes only after the approval closes. After a demo approval closes over the launcher, two screenshots show the launcher repainted. T-LED1 and T-LED2 need hands: comments only.

Do not: run arduino-cli or scripts/build.sh, open the serial port, edit any file outside your list (theme, receipt and approval_screen belong to agent 2D; signer.cpp to 2B; config.cpp to 2A), or run git add/commit.

Final report: the format in section 3, rule 13.
```

```text
BRIEF 2D — Theme, receipt kit, approval screen (WP12, drawing)

You are one of five coding agents working at the same time on BadgeOS, firmware for an ESP32-S3 badge built as a fork of "Solana OS". REPO=/Users/ayush/Downloads/My Projects/Hackathons/MHacks2026 (contains a space: quote it). FW=$REPO/os. First read $REPO/docs/os/roadmap/execution-plan.md sections 1 (F2), 3, 4 and 5, then docs/os/README.md and architecture/overview.md.

Goal: the Receipt look exists in firmware: two themes, the drawing kit every screen uses, and the fixed approval screen, in light and dark.

Read: docs/os/ui/ui.md (Theme to the end), wallet/approval.md (Screen, Dev builds), and the running simulation $REPO/docs/design/os-mockups/index.html (read the CSS classes under ".t-rc" and the approval template; when the document and the simulation disagree about a pixel the simulation wins). Upstream: FW/src/hal/display.h. LovyanGFX: the font names in ui.md "Fonts" were checked and exist in the installed version 1.2.32 (lgfx_fonts.hpp).

Files you own (relative to FW; touch nothing else): src/vk/ui/theme.cpp (fill; may add to theme.h) · src/vk/ui/receipt.cpp (create; receipt.h exists) · src/vk/ui/approval_screen.cpp (create; approval_screen.h exists).

Implement:
- theme.cpp: remove the "solana" theme; register receipt-light and receipt-dark with the eight colours in the token table; color, blend, activeName, setActive (writes config key "theme" with vk::config::set; unknown name: no change), count, at; config key "theme" (STR, default empty, 0 to 24). Empty or unknown stored name means receipt-light. color() must work before vk::begin() (config accessors initialise lazily).
- receipt.cpp: every function in "The receipt kit" block with the layout constants given there, drawing into display::canvas() and calling display::touch(). statusRight builds "HH:MM . NN% . [n]" from vk::clock (omit the time when !clock::ok()), power::percent() and vk::host::notify::count() (omit "[n]" when 0), plus "SETUP" when !vk::config::provisioned() and "DEV" when VK_PROFILE_DEV. amount picks FreeSerifBold24pt7b, or FreeSerifBold18pt7b when the text is wider than 136 px. barcode: bars derived from the seed bytes.
- approval_screen.cpp: vk::ui::drawApproval with the signature in section 4, laid out exactly by the table in approval.md "Screen" (header, verdict band, perforation, left stub, body rows, hold bar, footer) and the "Result" paragraph (outcome non-null: approved with a signature → SIGNED, approved without → APPROVED, VK_CANCELLED → CANCELLED, VK_TIMEOUT → TIMED OUT, VK_SIGN_FAILED → SIGN FAILED, any other reason → BLOCKED). The severity colours are constants in this file, not tokens. "KEY SE" or "KEY SW" from vk::wallet::keyLocation(); "DEV BUILD · " before it when VK_PROFILE_DEV. A request whose domain is "confirm" omits "asked by". footerBlink inverts the footer text for that frame.

Already on disk: ui/theme.h, receipt.h, approval_screen.h, wallet/approval.h, wallet/signer.h, core/config.h, core/clock.h, host/notify.h.

Host tests: none are specified for drawing code. Verify by reading: every coordinate against the two tables, every font name against ui.md.

Do not: run arduino-cli or scripts/build.sh, open the serial port, edit any file outside your list (approval.cpp and leds.cpp belong to agent 2C), or run git add/commit.

Final report: the format in section 3, rule 13, plus a table of every screen region with the coordinates you used.
```

```text
BRIEF 2E — Clock and ESP-NOW router (WP20, WP22)

You are one of five coding agents working at the same time on BadgeOS, firmware for an ESP32-S3 badge built as a fork of "Solana OS". REPO=/Users/ayush/Downloads/My Projects/Hackathons/MHacks2026 (contains a space: quote it). FW=$REPO/os. First read $REPO/docs/os/roadmap/execution-plan.md sections 3, 4 and 5, then docs/os/README.md and architecture/overview.md.

Goal: a clock that knows where its time came from and never blocks boot, and the router that owns the one ESP-NOW receive handler.

Read: docs/os/wallet/checks.md (Clock), protocol/espnow.md (Frame header, Type registry, Router), architecture/upstream-hooks.md (H3, H9, H13), architecture/upstream-baseline.md (ESP-NOW framing, finding F3), roadmap/implementation-plan.md (WP20, WP22, Review focus 4). Upstream: FW/src/net/espnow_mgr.h, wifi_mgr.h, lua_sdk/lua_runtime.h.

Files you own (relative to FW; touch nothing else): src/vk/core/clock.cpp (fill; may add to clock.h) · src/vk/host/router.cpp (fill; may add to router.h) · test/device/t_clock.py.

Implement:
- clock.cpp: source, now, ok, raiseTo exactly as checks.md "Clock"; a VK_SERVICE that, the first time wifi_mgr::mode() is Station and connected, calls configTime(0, 0, <ntp_server>) once and never waits; SNTP marked from sntp_set_time_sync_notification_cb, and also by polling sntp_get_sync_status() once a second (keep both: the poll is the stated fallback and is harmless). The service keeps its own time base (unix seconds at a millis() reference); it never trusts the raw system time and never goes backwards. Config key ntp_server. Info field "time" (none, floor, sntp). devSet under #if VK_TEST_HOOKS.
- router.cpp: install() sets the one upstream handler; forwarding rules 1 to 3 exactly as espnow.md "Router" ("Forward to the app" needs: an app is running, !vk::modalActive(), and vk::host::granted("espnow")); routes get rx_ms = espnow_mgr::lastRxMs(); send(mac, frame, len) with mac nullptr meaning broadcast. Use vk_frame_type from wallet/pure/vk_frames.h.

Already on disk: core/clock.h, host/router.h, host/permissions.h, core/config.h, core/service.h, core/serial.h, wallet/pure/vk_frames.h, vk.h.

Host tests: none specified (both files are hardware glue). 
Device test to write (t_clock.py; you cannot run it): after badge.reset(), "[os] ready" arrives within 30 s with Wi-Fi absent and VKINFO time=none; VKTIME <now> gives time=sntp and VKSTATE time "sntp"; upstream sample app "hello" still launches and stops (regression for the handler change). The SNTP check (time=sntp within 10 s of Wi-Fi connecting) goes in a second file t_clock_net.py with NEEDS = "network"; T-HOOK1 and the whosnear check go in t_hook.py with NEEDS = "two-badges".

Do not: run arduino-cli or scripts/build.sh, open the serial port, edit any file outside your list, or run git add/commit.

Final report: the format in section 3, rule 13.
```

```text
BRIEF I2 — Integrator, Batch 2

You integrate the second batch of BadgeOS (config, signer, approval engine and screen, clock, router). REPO=/Users/ayush/Downloads/My Projects/Hackathons/MHacks2026 (contains a space: quote it). FW=$REPO/os. PY=$REPO/.venv/bin/python. Port /dev/cu.usbserial-10. Read $REPO/docs/os/roadmap/execution-plan.md sections 1, 4, 5 and 6 (follow section 6 step by step), docs/os/wallet/signing.md, wallet/approval.md, platform/config.md, ui/ui.md, roadmap/implementation-plan.md (WP10, WP11, WP12, WP20, WP22). The five implementer reports follow this brief.

Batch-specific steps:
- Host: run.sh must now print seven "all … tests passed" lines (sol, record, frames, checks, config, domains, approval).
- Build: the likeliest mismatches are approval.cpp ↔ approval_screen.cpp (drawApproval arguments), approval.cpp ↔ signer.cpp (signForApproval, open), approval.cpp ↔ config.cpp (confirmChange and the "(reset)" key). Section 4 and the spec decide.
- Device tests, in this order: t_boot.py (the registries line must now show domains>=1, config>=7, patterns>=5, status>=1, services>=3; VKINFO must show selfcheck=1 and key=se050 or software: that is T-BOOT3) · t_cfg.py · t_apr.py · t_clock.py.
- Run t_apr.py once per theme: VKSET theme receipt-dark, run, VKSET theme receipt-light, run. Look at the six screenshots in test/device/shots/ yourself (Read the PNG files) and compare with docs/design/os-mockups/index.html; fix layout faults that make text overlap or fall off the screen; note cosmetic differences without fixing them.
- Pre-flash check 2 must pass (nothing but signer.cpp signs).

Deferred: T-CFG4, T-LED1, T-LED2 (hands); T-HOOK1 and whosnear (second badge); SNTP (network); store registration against a broker (network).
Docs: tracking rows WP10, WP11, WP12, WP20, WP22; README open item U5 (SNTP callback: record that both paths are compiled; verification deferred).
Commit message: "WP10: batch 2, config, signer, approval engine and screen, clock, router (WP10 WP11 WP12 WP20 WP22)".
Gate: seven host suites pass; t_boot, t_cfg, t_apr (both themes), t_clock pass on the badge.

Final report: the format in section 6, step 11.
```

### Batch 3

```text
BRIEF 3A — The solana signing domain (WP13 and WP21, decoder glue)

You are one of five coding agents working at the same time on BadgeOS, firmware for an ESP32-S3 badge built as a fork of "Solana OS". REPO=/Users/ayush/Downloads/My Projects/Hackathons/MHacks2026 (contains a space: quote it). FW=$REPO/os. First read $REPO/docs/os/roadmap/execution-plan.md sections 3, 4 and 5, then docs/os/README.md and architecture/overview.md.

Goal: the "solana" domain: bytes handed to the wallet are decoded, checked by the host-tested chain, and turned into the approval the user sees.

Read: docs/os/wallet/checks.md (all; "Verdict to screen" is your specification), wallet/solana-payments.md (Token table, The feature folder), wallet/approval.md (The request, Dev builds), wallet/signing.md (Domain table), reference/reasons.md, platform/config.md (Keys), roadmap/implementation-plan.md (WP13, WP21, Review focus 1).

Files you own (relative to FW; touch nothing else): src/vk/features/solana_pay/domain_solana.cpp.

Implement: VK_SIGN_DOMAIN(solana, "solana", "", true, "sign", 1232, decodeSolana, nullptr); config keys issuer_key (KEY32, secure, required), tokens (TOKENS, secure, required), record_ttl_s (U32, 30, secure, 5 to 3600); decodeSolana doing steps 1 to 4 of "Verdict to screen" in order: verify the record with vk_record_verify and vk_verify_c and call vk::clock::raiseTo(issued_at); fill vk_check_input_t from vk::config (issuer key, token table, record_ttl_s), vk::clock (now, source mapped to vk_time_source_t), vk_verify_c, vk::wallet::presenceLookup (may be null), vk::wallet::publicKey(); call vk_check_solana; build the ApprovalRequest field by field from the table (headline from vk_headline_text; big "<amount> <symbol>" with sol_format_amount; sub; lines with the stated conditions and the priority order Requested, Expected, Limit, Account, Kind, Memo, at most four; severity, select, red_reason, dev_overridable from the verdict; recipient, recipient_name, amount, decimals, symbol). Base58 with sol_b58_encode, shortened to first 4 + ".." + last 4. Log "[pay] undecodable: <sol_tx_err_name>" when the decoder fails. Always return VK_OK (a red request still opens). If issuer_key is not set, pass a zero key so every record fails verification.

Already on disk: wallet/signer.h, approval.h, crypto.h, reason.h, wallet/pure/*.h, core/config.h, core/clock.h.

Host tests: none specified for this glue (the chain itself is covered by test_checks; the mapping is covered on the badge by t_chk.py, written by agent 3C). Verify by reading each table row against your code.

Do not: run arduino-cli or scripts/build.sh, open the serial port, edit any file outside your list (lua_solana.cpp belongs to agent 3B), or run git add/commit.

Final report: the format in section 3, rule 13, plus a table mapping each "Verdict to screen" row to the line numbers that implement it.
```

```text
BRIEF 3B — Lua wallet bindings (WP13, Lua side)

You are one of five coding agents working at the same time on BadgeOS, firmware for an ESP32-S3 badge built as a fork of "Solana OS". REPO=/Users/ayush/Downloads/My Projects/Hackathons/MHacks2026 (contains a space: quote it). FW=$REPO/os. First read $REPO/docs/os/roadmap/execution-plan.md sections 1 (F12), 3, 4 and 5, then docs/os/README.md and architecture/overview.md.

Goal: Lua apps can read the badge's identity, build a transfer, open the firmware approval and poll for the signature.

Read: docs/os/platform/lua-api.md (Conventions; badge.wallet identity and payments; badge.codec), platform/app-host.md (Lua function registry), wallet/signing.md (The signing path), wallet/solana-payments.md (Builder, Token table), wallet/checks.md (Registry record), reference/reasons.md. Upstream: FW/src/lua_sdk/lua_runtime.h (extendDeadline, currentApp), FW/src/lua_sdk/lib_net.cpp (as a model of a blocking binding).

Files you own (relative to FW; touch nothing else): src/vk/wallet/lua_wallet.h (create) · src/vk/wallet/lua_wallet.cpp · src/vk/features/solana_pay/lua_solana.cpp.

Implement, each with one VK_LUA_FUNCTION line (names and permissions in section 5.3 and lua-api.md):
- lua_wallet.h: int vk::wallet::luaBegin(lua_State *L, const char *domain, int bytesIndex, int ctxIndex); it reads the optional ctx table (record, record_sig of exactly 64 bytes, req), calls runtime::extendDeadline(2500) once per signed item present plus once more, calls vk::wallet::begin(domain, bytes, len, ctx, runtime::currentApp().c_str()), and returns true or nil, reason.
- lua_wallet.cpp: pubkey, address, key_location, provisioned, time_ok, tokens (array of {symbol, mint, decimals, cap, max} with mint base58 and cap, max as display-unit strings), config (permission none) · begin(domain, bytes, [ctx]) and poll() (permission "sign"; poll returns "pending", the 64-byte signature, or nil, reason, and nil, "idle" with nothing begun) · codec.b64enc, b64dec, b58enc, b58dec, hex, unhex (permission none; decoders return nil on bad input).
- lua_solana.cpp (permission "sign"): begin_solana(msg, [ctx]) = luaBegin(L, "solana", 1, 2) · build_transfer{destination=, amount=, blockhash=, [symbol=], [source=], [memo=]} using sol_parse_amount with the token's decimals and sol_tx_build_transfer; source defaults through vk::wallet::tokenInfoLookup and fails with "unsupported" when that is null or the account is unknown; other bad input fails with "bad_arg" · wire_tx(sig, msg) = base64 of 0x01 ‖ sig ‖ msg · check_record(record, sig) returning the table in lua-api.md (verify with vk_record_verify and vk_verify_c after runtime::extendDeadline(2500); ok is true only if the signature is valid, status active and, when the clock is ok, not expired; reason from reasons.md).

Already on disk: host/lua_registry.h, wallet/signer.h, approval.h, crypto.h, wallet/pure/*.h, core/config.h, core/clock.h.

Host tests: none specified (bindings need the Lua VM; they are exercised on the badge by agent 3C's tests). Verify by reading each lua-api.md row against your code, including every return shape.

Do not: run arduino-cli or scripts/build.sh, open the serial port, edit any file outside your list (domain_solana.cpp belongs to agent 3A), or run git add/commit.

Final report: the format in section 3, rule 13.
```

```text
BRIEF 3C — Sign test, check test, fixtures and device tests (WP13, WP21, WP24 tests)

You are one of five coding agents working at the same time on BadgeOS, firmware for an ESP32-S3 badge built as a fork of "Solana OS". REPO=/Users/ayush/Downloads/My Projects/Hackathons/MHacks2026 (contains a space: quote it). FW=$REPO/os. First read $REPO/docs/os/roadmap/execution-plan.md sections 1 (F13 and "Choices made"), 3, 4 and 5, then docs/os/README.md and architecture/overview.md.

Goal: the two dev-only Lua test apps and the scripted tests that prove, on one badge with no network, that the firmware shows the right verdict for every kind of payment and signs only after the button.

Read: docs/os/apps/apps.md (Rules for every Lua app, Sign test, the Check test row as changed by F13), platform/lua-api.md (identity, payments, codec), wallet/checks.md (all), wallet/solana-payments.md (Message format, Decoder rules), protocol/espnow.md (REQ), integration/backend.md (Existing routes; /registry exact behaviour), testing/testing.md (Dev hooks, Approval, Checks, Stores tests), FW/README.md (upstream's Lua API). Also FW/scripts/vkdev.py and FW/test/device/common.py (the API you must use) and FW/test/host/vectors.json (object "vk": test issuer and device keys).

Files you own (relative to FW; touch nothing else): apps/signtest/app.ini, main.lua, config.lua · apps/checktest/app.ini, main.lua, config.lua · test/device/fixtures.py · test/device/t_sign.py, t_chk.py, t_sto.py · test/device/t_sign_net.py.

Implement:
- signtest (permissions=sign,net; min_api=2) exactly as apps.md "Sign test", steps 1 to 4. It carries its own minimal JSON field extraction and RPC call for now (lib/vk.lua does not exist yet; agent 4D replaces them). Every network call returning nil shows a status line and retries; nothing hangs.
- checktest (permissions=sign,net,history,storage; min_api=2): on start it requires "case" (a file case.lua the test pushes into the app folder; without it the app logs "CT nocase" and idles). Case fields: msg_hex or transfer (a table passed to wallet.build_transfer), record_hex, sig_hex, req_hex, delay_ms, flush_draw (draw and call badge.gfx.flush() every frame), history (log the newest wallet.history entry), storage_probe (try badge.storage.read on "../../vk/history.bin" and "/vk/history.bin"). It logs with badge.log, one line each: "CT tick <n>" once a second from on_update, "CT begin ok" or "CT begin err <reason>", "CT poll sig <hex>" or "CT poll err <reason>", "CT hist <outcome> <amount> <symbol>", "CT storage <a> <b>". CANCEL exits.
- fixtures.py: build_transfer_msg(payer, source, dest, mint, blockhash, amount, decimals, memo=None) mirroring sol_tx_build_transfer's layout; raw-byte mutators for the five refusal vectors of T-APR5; make_record(fields, issuer_seed) producing the canonical record text of checks.md and its signature over "registry:"+record with cryptography's Ed25519; make_req(device_seed, amount, currency, req_id, expiry, name, rail=1) producing a REQ frame signed over "pay-req:"+signed bytes; case_lua(dict) rendering a case.lua.
- t_sign.py (all runnable on one badge): provision_test; VKTIME now. T-APR1 offline half: case with a valid transfer and no ctx → VKSTATE red, headline "UNVERIFIED RECIPIENT", dev_override true, big "10.00 HACK"; btn a hold 1300; "CT poll sig"; verify the signature over the message with the badge's VKINFO pubkey; record the "[vk] sign … ms" line. T-APR4: flush_draw case; while modal the screenshot equals a second screenshot taken 500 ms later and "CT tick" does not advance. T-APR5: each of the five vectors → red, headline "CANNOT READ PAYMENT" or "UNKNOWN TOKEN", select disabled, dev_override false, and after btn b tap "CT poll err undecodable". Review focus 2: begin, approve, stop the app before it polls, relaunch, begin again is not "busy"; and DEL checktest while modal closes the approval.
- t_chk.py: T-CHK2 to T-CHK9 exactly as the table in testing.md, each asserting severity, headline, select, the named line, and the poll reason after closing; T-CHK7 uses make_req; T-CHK9's second half does badge.reset() (clock back to none) and expects amber "CLOCK UNSYNCED". Add the replay case: valid record + valid request, no presence → amber "VERIFIED - NOT PRESENT". Review focus 1: after a begin with record and request the app still logs "CT tick". Record the "[vk] verify … ms" lines.
- t_sto.py: T-STO1 (sign once, badge.reset(), history case → "CT hist signed 10.00 HACK"), T-STO2 (storage_probe → both fail).
- t_sign_net.py: NEEDS = "network": the full T-APR1 with signtest, the dashboard listener and devnet.

Consumes (being written now by 3A, 3B, 3E; use the documents): the Lua functions of lua-api.md; history from stores.md.

Verification you can do: "$REPO/.venv/bin/python" -m py_compile on each Python file; a self-check in fixtures.py (python fixtures.py) that rebuilds vectors.json's record signature and REQ from the same inputs and compares byte for byte, and that build_transfer_msg reproduces vectors tx.legacy_1000.messageHex.

Do not: run arduino-cli or scripts/build.sh, open the serial port, edit any file outside your list (vkdev.py and common.py belong to an earlier agent: report what is missing), or run git add/commit.

Final report: the format in section 3, rule 13.
```

```text
BRIEF 3D — Payment requests and presence (WP23)

You are one of five coding agents working at the same time on BadgeOS, firmware for an ESP32-S3 badge built as a fork of "Solana OS". REPO=/Users/ayush/Downloads/My Projects/Hackathons/MHacks2026 (contains a space: quote it). FW=$REPO/os. First read $REPO/docs/os/roadmap/execution-plan.md sections 3, 4 and 5, then docs/os/README.md and architecture/overview.md.

Goal: a badge can ask to be paid (signed REQ, rebroadcast, answering presence challenges in firmware) and a payer badge can cache requests, challenge the payee and judge the proof. No second badge is available, so correctness rests on your host suite.

Read: docs/os/protocol/espnow.md (all, especially "Payment requests", "Presence", "Request cache"), platform/lua-api.md (badge.wallet requests; the requests, challenge, presence rows of payments), wallet/signing.md (Domain table rows pay-req and pay-proof; Cross-feature interfaces), wallet/checks.md (Presence lookup), platform/app-host.md (Lifecycle events, Notifications, Permissions), platform/config.md (Keys), roadmap/implementation-plan.md (WP23).

Files you own (relative to FW; touch nothing else): src/vk/features/requests/domain_pay_req.cpp, domain_pay_proof.cpp, requests.h, requests.cpp, presence.h, presence.cpp, lua_requests.cpp · apps/reqtest/app.ini, main.lua, config.lua · test/host/test_requests.cpp · test/device/t_req.py, t_req_single.py.

Implement:
- Domains pay-req (auto, prefix "pay-req:", max 94, validator: the bytes parse as a REQ header-to-name whose payee_pubkey is this badge's key) and pay-proof (auto, prefix "pay-proof:", max 56, validator: exactly 56 bytes whose req_id matches an active request). Bytes are always built by this feature, never taken from an app.
- Payee (requests.cpp): the active-request table (2 entries), request_open building the REQ with vk_req_build, expiry = clock::now() + ttl, signing once with signAuto("pay-req"), a service broadcasting each active request every req_period_ms through vk::host::router::send(nullptr, …), the CHAL route (type 2) with the two rate limits and the pay-proof signature answered unicast, close on request_close, at expiry, and from a VK_ON_APP_STOP listener.
- Payer (presence.cpp and requests.cpp): the request cache route (type 1; 8 entries; dropped at expiry or 30 s after last heard; returns false so the app also gets the frame; posts vk::host::notify::post("Payment request", "<name> <amount> <currency>", <pay_app>) when a new req_id appears and the running app is not pay_app), the 4-slot presence table, challenge(), the PROOF route (type 3) judging with rx_ms and presence_ms exactly as "Payer: challenging and judging", the log line "[req] proof <ms> ms", and at boot vk::wallet::presenceLookup set to the slot lookup.
- Config keys presence_ms, req_ttl_s, req_period_ms, req_max_proofs, req_gap_ms, pay_app with the defaults, flags and ranges in config.md. Permission "request" (label "ask others to pay this badge", consent yes).
- Lua (lua_requests.cpp): request_open, request_close, request_status (permission "request"); requests, challenge, presence (permission "sign"); returns and reasons exactly as lua-api.md. request_open calls runtime::extendDeadline(2500) and refuses with not_provisioned, no_time, busy, bad_arg, sign_failed.
- The table logic (active requests, rate limits, cache, presence slots and judging) takes time, random bytes, sign and verify through function pointers so it compiles with VK_HOST_TEST.
- reqtest (permissions=request,espnow; min_api=2): opens a request for config.amount on start, logs "RT open <req_id>" or "RT err <reason>", logs "RT status <state> <proofs>" every second and "RT result <status>" for RESULT frames (parse the fixed layout by hand; lib/vk.lua does not exist yet).

Already on disk: wallet/signer.h, crypto.h, host/router.h, notify.h, lifecycle.h, permissions.h, lua_registry.h, core/config.h, clock.h, service.h, wallet/pure/vk_frames.h, vk_checks.h.

Host tests first (test_requests.cpp, first line "// LINK: src/vk/features/requests/requests.cpp src/vk/features/requests/presence.cpp", keys from vectors.h, signing with host_ed25519): a third open is busy; expiry closes; app stop closes only that app's requests; CHAL for an unknown req_id is dropped; proofs stop at req_max_proofs and respect req_gap_ms; the PROOF built verifies with the payee key; cache keeps 8 distinct and drops by age and expiry; challenge fills a slot PENDING; a valid PROOF in time is PRESENT, late is LATE, a bad signature is BAD_SIG, a PROOF from another MAC is ignored, the first PROOF decides; the oldest slot is reused; presenceLookup returns the stored key and nonce.
Command: cd "$FW" && test/host/run.sh test_requests   (expect "all requests tests passed").

Device tests to write (you cannot run them): t_req_single.py (one badge: with the clock unset request_open gives "RT err no_time"; after VKTIME the request opens, status is open, stopping the app closes it and a relaunch can open again; a third open is busy) · t_req.py with NEEDS = "two-badges": T-REQ1 to T-REQ4, T-CHK1.

Do not: run arduino-cli or scripts/build.sh, open the serial port, edit any file outside your list, include any header of another feature, or run git add/commit.

Final report: the format in section 3, rule 13.
```

```text
BRIEF 3E — History and balance features (WP24, WP33)

You are one of five coding agents working at the same time on BadgeOS, firmware for an ESP32-S3 badge built as a fork of "Solana OS". REPO=/Users/ayush/Downloads/My Projects/Hackathons/MHacks2026 (contains a space: quote it). FW=$REPO/os. First read $REPO/docs/os/roadmap/execution-plan.md sections 1 (F1, F8, F11), 3, 4 and 5, then docs/os/README.md and architecture/overview.md.

Goal: two independent feature folders: every approval outcome is logged to a ring file that survives reboots, and the badge learns and shows its token balance.

Read: docs/os/wallet/stores.md (Where, History, Tests), wallet/approval.md (ApprovalOutcome, Listeners), platform/lua-api.md (history; balance; the balance and token_account rows of identity), ui/ui.md (Status bar, Balance), wallet/signing.md (Cross-feature interfaces: tokenInfoLookup), roadmap/implementation-plan.md (WP24, WP33, Review focus 3 and 4). Upstream: FW/src/net/net_route.h, wifi_mgr.h.

Files you own (relative to FW; touch nothing else): src/vk/features/history/history.h, history.cpp, lua_history.cpp · src/vk/features/balance/balance.h, balance.cpp, lua_balance.cpp · test/host/test_stores.cpp.

Implement:
- history: the file /vk/history.bin exactly as stores.md (12-byte header, 192-byte records, ring of 128, bad magic → renamed .bad and a fresh file) through vk::fileio::ops (core/fileio.h); vk::history::count and at; a VK_ON_APPROVAL listener writing one record per outcome (outcome codes 1 to 6; time 0 when the clock has no source; domain "confirm" for confirmations; dev flag); it creates /vk/ if missing; a failed write is logged with badge_log::tagf("vk", …) and ignored. Permission "history" (label "read payment history", no consent). Lua wallet.history([max]) returning the table in lua-api.md, with outcome strings signed, cancelled, timeout, blocked, failed, approved.
- balance: namespace vk::balance as in ui.md "Balance"; a service that polls every balance_poll_s seconds only while vk::host::idle(), no approval is active, and wifi_mgr::mode() is Station and connected; one getTokenAccountsByOwner call to rpc_url through net_route::request with a 3 s timeout; the reply scanned for the first account's "pubkey" and "amount" by string search; log "[bal] fetch <ms> ms"; skip silently when Wi-Fi is down; vk::ui::requestShellRepaint() when the value changes; at boot set vk::wallet::tokenInfoLookup. Config key balance_poll_s. Status item "balance" (order 40). Lua balance and token_account (no permission; nil for any symbol but the default token), refresh_balance (permission "net"; true or nil, reason; calls runtime::extendDeadline(7000) first because connect and read each get the timeout).

Already on disk: core/fileio.h, core/config.h, clock.h, service.h, wallet/approval.h, signer.h, host/home.h, permissions.h, lua_registry.h, ui/statusbar.h, wallet/pure/sol.h; test/host/shim/vk_host_fileio.h.

Host tests first (test_stores.cpp, first line "// LINK: src/vk/features/history/history.cpp"): round trip of one record with every field; newest-first order; ring wrap at 128 (the 129th write replaces the oldest, count stays 128); bad magic is renamed and a new file started; a write that fails (vk_host_fileio_fail_writes) returns false and leaves the previous file intact; a missing /vk/ is created. Balance has no host suite: put the reply scanner in a pure function and add its cases to this same suite only if it needs no extra LINK file; otherwise verify by reading.
Command: cd "$FW" && test/host/run.sh test_stores   (expect "all stores tests passed").

Device tests: T-STO1 and T-STO2 are written by agent 3C. The balance check (bar shows the balance within one poll; token_account equals the funded account) needs a network: write test/device/t_bal_net.py? No: that file is not in your list; describe the test in your report and the integrator will record it as deferred.

Do not: run arduino-cli or scripts/build.sh, open the serial port, edit any file outside your list, let either feature include the other's header, or run git add/commit.

Final report: the format in section 3, rule 13.
```

```text
BRIEF I3 — Integrator, Batch 3

You integrate the third batch of BadgeOS (solana domain, Lua wallet, test apps, requests, history, balance). REPO=/Users/ayush/Downloads/My Projects/Hackathons/MHacks2026 (contains a space: quote it). FW=$REPO/os. PY=$REPO/.venv/bin/python. Port /dev/cu.usbserial-10. Read $REPO/docs/os/roadmap/execution-plan.md sections 1, 4, 5, 6 (follow section 6 step by step), 8 and 9, docs/os/wallet/checks.md, platform/lua-api.md, protocol/espnow.md, wallet/stores.md, roadmap/implementation-plan.md (WP13, WP21, WP23, WP24, WP33). The five implementer reports follow this brief.

Batch-specific steps:
- Host: nine suites (add stores, requests). Run "$PY" test/device/fixtures.py (its self-check) before any device test.
- Device tests, in this order: t_boot.py (registries must show domains>=4, routes>=3, lua>=25, permissions>=2) · t_cfg.py (T-CFG3 now runs against tokens) · t_apr.py · t_sign.py · t_chk.py · t_sto.py · t_req_single.py. Permissions are not enforced yet, so no consent prompt appears.
- Measurements: from the log lines of t_sign.py and t_chk.py record M2 (one "[vk] sign solana … ms" and the median "[vk] verify … ms") and M3 (time from "CT begin" to modal true). Write them into docs/os/testing/testing.md. State in your report, on a line of its own, "VERIFY_MS=<n>": if it is above 400 the orchestrator adds agent 4F (Monocypher) to Batch 4.
- If the log shows a loopTask stack overflow at any point, apply Risk 5's fallback (section 9) immediately and record it.

Deferred: T-APR1 on devnet and signtest (network); T-REQ1–4, T-CHK1 (second badge); balance (network).
Gate 1 and Gate 2 of the spec are met only in their offline halves: say so in the tracking notes, with the missing halves named.
Commit message: "WP13: batch 3, solana payments, record checks, requests, history, balance (WP13 WP21 WP23 WP24 WP33)".
Gate: nine host suites pass; the seven device tests above pass.

Final report: the format in section 6, step 11, plus the VERIFY_MS line.
```

### Batch 4

```text
BRIEF 4A — Permissions, consent, manifest (WP30)

You are one of five coding agents working at the same time on BadgeOS, firmware for an ESP32-S3 badge built as a fork of "Solana OS". REPO=/Users/ayush/Downloads/My Projects/Hackathons/MHacks2026 (contains a space: quote it). FW=$REPO/os. First read $REPO/docs/os/roadmap/execution-plan.md sections 1 (F3, F4), 3, 4 and 5, then docs/os/README.md and architecture/overview.md.

Goal: an app can use only what its app.ini asks for, sensitive permissions need the user's approval once, and an app that needs a newer API is refused.

Read: docs/os/platform/app-host.md (Manifest, Permissions, Consent, Lifecycle events, API version, Lua function registry), wallet/stores.md (Consent), wallet/approval.md (confirm; "What a decoder writes": the consent example), architecture/upstream-hooks.md (H7, H8a, H8b, H11), roadmap/implementation-plan.md (WP30), testing/testing.md (Apps and permissions tests). Upstream: FW/src/apps/app_store.h, FW/src/lua_sdk/lua_runtime.cpp (launch, stop).

Files you own (relative to FW; touch nothing else): src/vk/host/permissions.cpp (fill) · src/vk/host/lua_registry.cpp (add filtering) · src/vk/host/manifest.cpp, consent.cpp (create; headers exist; you may add declarations to manifest.h, e.g. a pure parse(const String &iniText, Extra &out)) · test/host/test_manifest.cpp, test_consent.cpp · test/device/t_app.py · test/device/fixtures/noperm/, needsign/, minapi99/, nonet/ (each app.ini and main.lua) · the app.ini of upstream samples: apps/radar (permissions=espnow), apps/whosnear (espnow,net), apps/vumeter (mic,storage), apps/gallery (storage).

Implement:
- permissions.cpp: VK_PERMISSION lines for sign, net, espnow, ble, mic, storage with the labels, consent flags and upstream tables of the table in app-host.md; granted() reading the active slot (true when no app is active; for a native app, its BADGE_APP permissions; native apps always have espnow); promotePending(); a VK_ON_APP_STOP listener clearing the active slot; preLaunch() doing everything in "How it is enforced" step 1 and "Consent": manifest load, unknown permission → "unknown permission: <name>", id longer than 32, a Lua folder /apps/<id> whose id is also a native id (check the folder with LittleFS, not app_store::exists, which is true for native ids after H11), min_api → "needs a newer BadgeOS (API <n>)", native apps skip consent, missing consent → raise the "Allow app" confirmation and return false with an empty error, and on approval save the entry and call runtime::requestLaunch(appId).
- lua_registry.cpp: a registered function whose permission is not granted is replaced by a stub raising "permission '<name>' not granted (add it to permissions= in app.ini)"; each upstream module table named by an ungranted permission is replaced by a table whose every access raises the same message.
- manifest.cpp: load() via app_store::readFile and a pure parser. consent.cpp: the API in section 4 over /vk/consent.bin (stores.md format, through vk::fileio::ops, oldest replaced at 32) and a VK_ON_RESET listener calling eraseAll().

Already on disk: host/permissions.h, manifest.h, consent.h, lifecycle.h, native.h, lua_registry.h, wallet/approval.h, core/fileio.h, core/config.h.

Host tests first: test_manifest.cpp ("// LINK: src/vk/host/manifest.cpp": both keys, defaults, spaces, comments, unknown keys ignored, bad min_api) and test_consent.cpp ("// LINK: src/vk/host/consent.cpp": the hash is order-independent and differs when a name changes; save, has, replace same id, oldest replaced at 33, eraseAll, bad magic recovery, a failing write returns false).
Command: cd "$FW" && test/host/run.sh test_manifest test_consent.

Device test to write (t_app.py; you cannot run it; push fixtures with badge.push): T-APP1 (noperm calls wallet.begin_solana → log shows "permission 'sign' not granted"), T-APP2 (needsign: first run shows title "Allow app", headline "NEW PERMISSIONS", amber, hold; hold starts the app; a second run has no prompt), T-APP3 (push needsign with permissions=sign,net → asked again), T-APP4 (minapi99 → error containing "needs a newer BadgeOS"), T-APP7 (nonet uses badge.http.get → error naming "net"). After each refused launch call common.to_launcher.

Do not: run arduino-cli or scripts/build.sh, open the serial port, edit any file outside your list (native.cpp belongs to agent 4B), or run git add/commit.

Final report: the format in section 3, rule 13.
```

```text
BRIEF 4B — Native runtime, notifications, Inbox (WP31, WP32)

You are one of five coding agents working at the same time on BadgeOS, firmware for an ESP32-S3 badge built as a fork of "Solana OS". REPO=/Users/ayush/Downloads/My Projects/Hackathons/MHacks2026 (contains a space: quote it). FW=$REPO/os. First read $REPO/docs/os/roadmap/execution-plan.md sections 1 (F1, F9), 3, 4 and 5, then docs/os/README.md and architecture/overview.md.

Goal: C++ apps compiled into the firmware run like Lua apps, and a small notification inbox exists with its status item, LED pattern and Inbox app.

Read: docs/os/platform/native-apps.md (all), platform/app-host.md (Two kinds of app, Native runtime, Notifications, System apps), architecture/upstream-hooks.md (H8a to H8f, H11), ui/ui.md (LED patterns: notify; Status bar; The receipt kit; Screens: any list), apps/apps.md (Inbox, Hello), roadmap/implementation-plan.md (WP31, WP32), testing/testing.md (T-APP5, T-APP6, T-REQ5).

Files you own (relative to FW; touch nothing else): src/vk/host/native.cpp (fill) · src/native_apps/hello_native/hello_native.cpp · src/vk/host/notify.cpp (fill) · src/vk/ui/status_dev.cpp · src/native_apps/inbox/inbox.cpp · test/device/t_native.py, t_notify.py.

Implement:
- native.cpp: every function of app-host.md "Native runtime" over the badge::NativeApp registry: count, infoAt and infoById filling every field of app_store::Info (sizeBytes 0, entry "", author "", description ""), exists, start (calls vk::host::promotePending(), creates the object with the registered create function, calls on_start), stop (on_stop, delete), active, update (on_update then on_draw), button, espnow, permissions; badge::exit() stays defined here.
- hello_native.cpp: exactly the example in native-apps.md.
- notify.cpp: the API of app-host.md "Notifications" (8 notes in RAM, newest first, identical title+body within 10 s ignored); status item "inbox" (order 30, "[n]"); LED pattern "notify" (dim purple breathe, 3 s period) played by a service while a note waits and vk::host::idle(), stopped otherwise; vk::ui::requestShellRepaint() when the count changes.
- status_dev.cpp: status item "dev" (order 20, "DEV" in amber) under #if VK_PROFILE_DEV.
- inbox.cpp: BADGE_APP(Inbox, "inbox", "Inbox", "1.0.0", ""), drawn with vk::ui::receipt as a list screen (header, title INBOX, rows with sublines, footer), behaviour exactly as apps.md "Inbox".

Already on disk: host/native.h, notify.h, home.h, permissions.h, sdk/badge_sdk.hpp, ui/receipt.h, statusbar.h, leds.h, theme.h, core/service.h.

Host tests: none specified. Device tests to write (you cannot run them): t_native.py: T-APP5 (run hello_native, VKSTATE app "hello_native" and native true, screenshot not blank, btn b tap exits, run again works); the T-APP6 half that reads the log line "[deny] begin=denied" produced by a temporary native app the integrator adds. t_notify.py: VKNOTE "Test|hello|hello_native" → VKSTATE notes 1; run inbox; btn a tap launches hello_native and notes becomes 0; a second note dismissed with btn right tap. T-REQ5 needs two badges: t_notify_2.py is not in your list; describe it in your report.

Do not: run arduino-cli or scripts/build.sh, open the serial port, edit any file outside your list (permissions.cpp belongs to agent 4A), include a feature header from a native app, or run git add/commit.

Final report: the format in section 3, rule 13.
```

```text
BRIEF 4C — Contacts feature (WP34)

You are one of five coding agents working at the same time on BadgeOS, firmware for an ESP32-S3 badge built as a fork of "Solana OS". REPO=/Users/ayush/Downloads/My Projects/Hackathons/MHacks2026 (contains a space: quote it). FW=$REPO/os. First read $REPO/docs/os/roadmap/execution-plan.md sections 3, 4 and 5, then docs/os/README.md and architecture/overview.md.

Goal: two badges can exchange signed contact cards that are valid for one receiver and one swap, and the saved contacts persist. No second badge is available, so correctness rests on your host suite.

Read: docs/os/protocol/espnow.md (CONTACT_HELLO, CONTACT_CARD, Codec), wallet/stores.md (Where, Contacts), platform/lua-api.md (badge.wallet contacts), wallet/signing.md (Domain table row contact), reference/reasons.md, roadmap/implementation-plan.md (WP34).

Files you own (relative to FW; touch nothing else): src/vk/features/contacts/domain_contact.cpp, contacts.h, contacts.cpp, lua_contacts.cpp · test/host/test_contacts.cpp · test/device/t_con.py.

Implement: domain "contact" (auto, prefix "contact:", max 113, validator: the bytes are a well-formed card body whose own_pubkey is this badge's key); the store /vk/contacts.bin exactly as stores.md through vk::fileio::ops with the vk::contacts API given there (64 records, oldest replaced, upsert updates the name); the swap nonce (16 random bytes, valid 60 s, the same HELLO returned until it expires, rotated after a successful accept); Lua contact_hello, contact_card, contact_accept, contacts, contact_remove with the returns and reasons of lua-api.md (permission "contacts"; contact_card and contact_accept call runtime::extendDeadline(2500)); contact_accept checks in the order espnow.md gives (addressee → mismatch, nonce → expired, signature → bad_proof), then upserts and posts vk::host::notify::post("Contact saved", "Saved <name>", "contacts"). Permission "contacts" (label "read and add contacts", no consent). The name used in HELLO and CARD is config display_name, or settings::deviceName() when empty. Time, random bytes, sign and verify go through function pointers so the logic compiles with VK_HOST_TEST.

Already on disk: wallet/signer.h, crypto.h, wallet/pure/vk_frames.h, core/fileio.h, config.h, clock.h, host/notify.h, permissions.h, lua_registry.h.

Host tests first (test_contacts.cpp, first line "// LINK: src/vk/features/contacts/contacts.cpp", two key pairs from host_ed25519): store round trip, upsert updates, remove, oldest replaced at 65, bad magic recovery; card signed bytes equal vk_card_signed_bytes; accept succeeds for a card made for this badge's current nonce; refuses a wrong nonce (expired), an expired nonce after 60 s, a wrong addressee (mismatch), a flipped signature bit (bad_proof); a second accept of the same card fails (nonce rotated).
Command: cd "$FW" && test/host/run.sh test_contacts.

Device test to write: t_con.py with NEEDS = "two-badges": T-CON1, T-CON2.

Do not: run arduino-cli or scripts/build.sh, open the serial port, edit any file outside your list, include any header of another feature, or run git add/commit.

Final report: the format in section 3, rule 13.
```

```text
BRIEF 4D — lib/vk.lua, badge.theme, app push script (WP35)

You are one of five coding agents working at the same time on BadgeOS, firmware for an ESP32-S3 badge built as a fork of "Solana OS". REPO=/Users/ayush/Downloads/My Projects/Hackathons/MHacks2026 (contains a space: quote it). FW=$REPO/os. First read $REPO/docs/os/roadmap/execution-plan.md sections 3, 4 and 5, then docs/os/README.md and architecture/overview.md.

Goal: the shared Lua library every app uses (JSON, RPC, registry record, frames, the payer flow, the Receipt look), the two Lua theme functions it draws with, and the script that installs apps.

Read: docs/os/platform/lua-api.md (all; "lib/vk.lua" and "vk.pay" are your specification), ui/ui.md (Theme, The receipt kit, Screens), integration/backend.md (all routes), protocol/espnow.md (Frame header, RESULT, CONTACT_HELLO, Type registry), guides/build-flash-provision.md (Installing apps), apps/apps.md (Rules; Sign test), roadmap/implementation-plan.md (WP35, Review focus 4), FW/README.md (upstream's badge.gfx, badge.http, badge.espnow), and docs/design/os-mockups/index.html for the look. The badge runs Lua 5.4 with 32-bit integers and float numbers.

Files you own (relative to FW; touch nothing else): lib/vk.lua · src/vk/ui/lua_theme.cpp · scripts/push-apps.sh · apps/vktest/app.ini, main.lua, config.lua · test/host/test_vk.lua · test/device/t_vk.py · apps/signtest/main.lua (replace its private JSON and RPC code with vk).

Implement:
- vk.lua: every row of the table in lua-api.md "lib/vk.lua" and the vk.pay state machine exactly as described (states presence, record, blockhash, approve, submit, confirm, done, failed; at most one blocking step per update; refresh_balance when token_account is nil; vk.feed after confirm, failures ignored; RESULT frame when paying a request). Every network helper returns nil, message on any failure and vk.pay ends in "failed" with a reason; nothing loops forever. vk.ui draws with badge.gfx in colours from badge.theme.color (falling back to the receipt-light values when badge.theme is absent), using the geometry of the firmware kit (header y 0..19, rows pitch 18, split at x=146, footer rule y=216); amounts use the built-in font scaled up. Amount strings are never converted to numbers.
- lua_theme.cpp: theme.name and theme.color (no permission), tokens and the fixed green, amber, red exactly as lua-api.md "badge.theme".
- push-apps.sh --port <port> | --host <ip> --token <code>, then dev|release: exactly as "Installing apps" (temporary copy with vk.lua, evilgame gets apps/game/*.lua except config.lua, release leaves out signtest, checktest, vktest, reqtest); over serial it calls "$REPO/.venv/bin/python" scripts/vkdev.py push.
- vktest (permissions=sign,net,espnow): runs the JSON table and the frame helpers on the badge, then every network helper with no network, logging "VT json ok", "VT frames ok", "VT rpc nil <message>", "VT record nil <message>", "VT pay failed <reason>", "VT done".

Host test first (test_vk.lua, run by run.sh with the laptop's lua, which is 5.5: keep it version-neutral; stub the badge table): JSON decode and encode round trips (nesting, escapes, unicode escapes, numbers, empty containers, malformed input returns nil), frame_type, result_frame and result_parse round trip, hello_parse, app_frame and app_body, vk.pay reaching "failed" when badge.http returns nil. It must print "all vk tests passed".
Command: cd "$FW" && test/host/run.sh test_vk.

Device test to write (t_vk.py; you cannot run it): push vktest (the test must add lib/vk.lua as extra file "vk.lua"), run it, wait for "VT done", assert each line above and that the app is still running.

Do not: run arduino-cli or scripts/build.sh, open the serial port, edit any file outside your list, or run git add/commit.

Final report: the format in section 3, rule 13.
```

```text
BRIEF 4E — Wallet settings app (WP36)

You are one of five coding agents working at the same time on BadgeOS, firmware for an ESP32-S3 badge built as a fork of "Solana OS". REPO=/Users/ayush/Downloads/My Projects/Hackathons/MHacks2026 (contains a space: quote it). FW=$REPO/os. First read $REPO/docs/os/roadmap/execution-plan.md sections 3, 4 and 5, then docs/os/README.md and architecture/overview.md.

Goal: the native "Wallet" app: five read-only pages showing what this badge is provisioned with, and the wallet reset.

Read: docs/os/apps/apps.md (Wallet (native)), platform/native-apps.md (all, especially the rules), platform/app-host.md (System apps), ui/ui.md (The receipt kit, Screens: any list), platform/config.md (Config store, Keys), docs/design/os-mockups/index.html (the "wallet" list model).

Files you own (relative to FW; touch nothing else): src/native_apps/wallet_settings/wallet_settings.cpp · test/device/t_wallet_app.py.

Implement: BADGE_APP(WalletSettings, "wallet_settings", "Wallet", "1.0.0", ""); pages Status, Tokens, Config, Apps, Reset exactly as apps.md lists them, LEFT/RIGHT change page, UP/DOWN scroll, CANCEL exits, drawn only with vk::ui::receipt. Sources: vk::config (provisioned, tokens, every ConfigKey through its registry with text()), vk::wallet (addressBase58, keyLocation, selfCheckOk, tokenInfoLookup, which may be null: show "—"), vk::clock::source(), VK_PROFILE_DEV, the badge::NativeApp registry (id and permissions), vk::host::consent::count() and at() for Lua apps with stored consent. Reset page: SELECT calls vk::config::requestReset(). Include only the SDK header, ui/receipt.h, ui/theme.h and host/consent.h; no feature header, nothing from src/identity.

Already on disk: sdk/badge_sdk.hpp, ui/receipt.h, ui/theme.h, host/consent.h, core/config.h, core/clock.h, wallet/signer.h.

Host tests: none specified. Device test to write (you cannot run it): run wallet_settings; five screenshots (one per page, btn right tap between) saved to test/device/shots/wallet_<n>.png and pairwise different; on the Reset page btn a tap opens a modal with headline "ERASE WALLET CONFIG" and btn b tap closes it leaving provisioned true; btn b tap exits the app.

Do not: run arduino-cli or scripts/build.sh, open the serial port, edit any file outside your list, or run git add/commit.

Final report: the format in section 3, rule 13.
```

```text
BRIEF 4F — Monocypher verification backend (WP51 fallback; dispatched only if integrator I3 reported VERIFY_MS above 400)

You are one of several coding agents working at the same time on BadgeOS, firmware for an ESP32-S3 badge. REPO=/Users/ayush/Downloads/My Projects/Hackathons/MHacks2026 (contains a space: quote it). FW=$REPO/os. First read $REPO/docs/os/roadmap/execution-plan.md sections 3, 4 and 5, then docs/os/wallet/signing.md (Crypto backend) and roadmap/implementation-plan.md (WP51).

Goal: Ed25519 verification can be switched to Monocypher 4.0.2 by setting VK_ED25519_BACKEND to 1, because TweetNaCl proved too slow on the badge.

Files you own (relative to FW; touch nothing else): src/vk/wallet/vendor/monocypher.c, monocypher.h, monocypher-ed25519.c, monocypher-ed25519.h (unmodified files of release 4.0.2, from https://monocypher.org/download/monocypher-4.0.2.tar.gz: src/monocypher.* and src/optional/monocypher-ed25519.*) · src/vk/wallet/vendor/README (version, source URL, SHA-256 of the tarball, licence) · test/host/test_mono.c.

Implement: nothing but the vendoring; src/vk/wallet/crypto.cpp already calls crypto_ed25519_check under #if VK_ED25519_BACKEND == 1 (read it and report if its include path or call does not match these files; do not edit it).

Host test (test_mono.c, first line "// LINK: src/vk/wallet/vendor/monocypher.c src/vk/wallet/vendor/monocypher-ed25519.c"): crypto_ed25519_check accepts V_RECORD_SIG over "registry:"+V_RECORD with V_ISSUER_PUB and the signature in V_REQ with V_DEVICE_PUB, rejects each with one bit flipped, and agrees with host_ed25519_verify on 50 random messages signed with host_ed25519_sign.
Command: cd "$FW" && test/host/run.sh test_mono.

Do not: run arduino-cli or scripts/build.sh, open the serial port, change VK_ED25519_BACKEND, edit any file outside your list, or run git add/commit. If the download fails, stop and report; do not retype the library.

Final report: the format in section 3, rule 13.
```

```text
BRIEF I4 — Integrator, Batch 4

You integrate the fourth batch of BadgeOS (permissions and consent, native runtime, notifications and Inbox, contacts, lib/vk.lua, Wallet app, and Monocypher if agent 4F ran). REPO=/Users/ayush/Downloads/My Projects/Hackathons/MHacks2026 (contains a space: quote it). FW=$REPO/os. PY=$REPO/.venv/bin/python. Port /dev/cu.usbserial-10. Read $REPO/docs/os/roadmap/execution-plan.md sections 1, 4, 5, 6 (follow section 6 step by step), 8 and 9, docs/os/platform/app-host.md, platform/native-apps.md, platform/lua-api.md, roadmap/implementation-plan.md (WP30 to WP36). The implementer reports follow this brief.

Batch-specific steps:
- Host: thirteen suites (add manifest, consent, contacts, vk; plus mono if 4F ran).
- Before the first build add the temporary native app FW/src/native_apps/zz_denytest/zz_denytest.cpp: a badge::App whose on_start builds any 214-byte buffer, calls vk::wallet::begin("solana", buf, 214, vk::wallet::Ctx{}, "zz_denytest"), logs badge_log::tagf("deny", "begin=%s", vk::wallet::reasonName(r)) and calls badge::exit(); registered with BADGE_APP(ZzDenyTest, "zz_denytest", "Deny test", "1.0.0", ""). T-APP6 passes when running it logs "[deny] begin=denied".
- If 4F ran: set VK_ED25519_BACKEND to 1 in src/vk/vk_build.h, and after flashing rerun t_chk.py and record the new "[vk] verify … ms" as M2 (Monocypher). If verification is still above 400 ms, keep backend 1, record the number, and note that presence will be shown amber until M1 is measured.
- Permissions are enforced from this build on. Every test app launch goes through common.launch, which answers the consent prompt; fix any test that calls badge.run directly on an app with sign or request.
- Device tests, in this order: t_boot.py (permissions>=9, native>=4, status>=4) · t_cfg.py · t_apr.py · t_app.py · t_native.py (then run zz_denytest for T-APP6) · t_notify.py · t_vk.py · t_wallet_app.py · t_sign.py · t_chk.py · t_sto.py (T-STO2 now runs with the storage permission) · t_req_single.py. Launch upstream samples hello and dice once each: they must still run with no permissions.
- Delete zz_denytest, compile once more.

Deferred: T-REQ5, T-CON1, T-CON2, T-HOOK1 (second badge); every network path of vk.lua beyond "returns nil, message" (network).
Commit message: "WP30: batch 4, permissions, native runtime, notifications, contacts, vk.lua, Wallet app (WP30 WP31 WP32 WP34 WP35 WP36)".
Gate: all host suites pass; the device tests above pass; Gate 3 of the spec is met except T-REQ5.

Final report: the format in section 6, step 11.
```

### Batch 5

Batch 5 was redefined by a design change (2026-10-03): the UI as a whole is BadgeOS's own. Upstream's shell is deleted and rewritten in the Receipt layout as `src/vk/shell/` ([ui/shell.md](../ui/shell.md)); there is no native `launcher` app, no native `settings` app and no home service; boot shows no splash; nothing a user can see says "Solana". Eight implementers run at once: 5A (shell framework, launcher, dialogs, boot), 5F and 5G (settings pages, written against the header `src/vk/shell/page.h` whose exact content is in shell.md "page.h"), 5R (rebrand and test tooling), 5B to 5E (the Lua apps). Their file lists are disjoint. Three interfaces cross agents and are fixed by the documents, so each side codes against the document, not against the other agent's file: `src/vk/shell/page.h` and `screens.h` (5A creates; 5F, 5G and 5R include), `src/vk/ui/repaint.h` (5R creates; 5A includes).

```text
BRIEF 5A — BadgeOS shell: framework, launcher, dialogs, boot screen (WP37)

You are one of eight coding agents working at the same time on BadgeOS, firmware for an ESP32-S3 badge built as a fork of "Solana OS". REPO=/Users/ayush/Downloads/My Projects/Hackathons/MHacks2026 (contains a space: quote it). FW=$REPO/os. First read $REPO/docs/os/roadmap/execution-plan.md sections 3, 4 (with the paragraph "Changed by the Batch 5 design change") and 5, then docs/os/README.md and architecture/overview.md.

Goal: upstream's shell is gone and BadgeOS's own shell replaces it. The badge powers on straight into the Receipt boot screen (no splash images), lands on the Receipt launcher (the MENU screen), and every screen upstream's shell had (settings list, delete confirmation, app-store offer, installing, app error) exists in the Receipt layout with no behaviour lost. You write the framework, the launcher, the settings list, the dialogs and the boot screen. Two other agents (5F, 5G) write the settings pages at the same time against the header you create.

Read: docs/os/ui/shell.md (all of it; it is your specification), ui/ui.md (Theme to the end; Boot bar), architecture/upstream-hooks.md (Rules, Table, Replaced upstream files, H23, Checking the hooks), architecture/upstream-baseline.md (shell, boot), roadmap/implementation-plan.md (WP37), and docs/design/os-mockups/index.html (boot, launcher, settings list; the STAGES list). Upstream, BEFORE you delete anything: all of FW/src/ui/shell.cpp (every screen, every button, every upstream call: each must survive exactly as shell.md says), FW/src/ui/shell.h, FW/src/ui/boot.cpp, boot.h, FW/os.ino (where boot:: and shell:: are called), FW/src/vk/ui/receipt.h, theme.h, leds.h, FW/src/apps/app_store.h, FW/src/lua_sdk/lua_runtime.h, FW/src/net/broker_client.h, FW/src/vk/host/native.h, notify.h, FW/src/vk/wallet/signer.h (publicKey, tokenInfoLookup).

Files you own (relative to FW; touch nothing else): src/vk/shell/shell.cpp · src/vk/shell/screens.h · src/vk/shell/page.h · src/vk/shell/page.cpp · src/vk/shell/launcher.cpp · src/vk/shell/settings_list.cpp · src/vk/shell/dialogs.cpp · src/ui/shell.cpp (delete it) · src/ui/boot.cpp (rewrite) · splash_images.h (in FW's root; delete it) · src/vk/ui/boot_screen.cpp (fill) · UPSTREAM-HOOKS.md · scripts/preflash-check.sh · test/device/t_shell.py. You do not write any file under src/vk/shell/pages/. src/ui/shell.h stays untouched: it is the interface os.ino and the runtime keep calling.

Implement, section by section of shell.md:
- "Source layout" and "Framework" (shell.cpp, screens.h): the screen stack (vk::shell::Screen, push, pop, home, repaint), the four upstream functions shell::begin(), update(), onAppStopped(), showError(const String &) with upstream's behaviour, and ::shell::screenName() (the top screen's name; "" while an app runs). The shell repaints on input, on a screen's refresh timer, when vk::ui::consumeShellRepaint() says so, and when the header text or the theme changes, as shell.md states. Include "../ui/repaint.h" for the repaint pair: agent 5R creates that file at the same time (content: namespace vk::ui { void requestShellRepaint(); bool consumeShellRepaint(); }); do not create it.
- "page.h" and "Settings page registry" (page.h, page.cpp): create page.h with EXACTLY the content of the block in shell.md "page.h" (5F and 5G are coding against that block right now; do not rename, reorder or drop anything, and add only below the marked line if you must). Implement every helper it declares in page.cpp.
- "Boot" (src/ui/boot.cpp, src/vk/ui/boot_screen.cpp): boot.cpp becomes the thin file shell.md gives: no splash, no include of splash_images.h, boot::run() sets the backlight and draws the 0 % frame, boot::progress() calls vk::ui::bootScreen. boot_screen.cpp draws the Receipt boot screen (header, brand line "BadgeOS", percent, block bar, detail text, CHECKLIST of the seven stages), flushes the display, calls vk::ui::leds::bootProgress(percent) and returns true. It runs before vk::begin(): use only theme::color (lazy) and the receipt kit.
- "Launcher" (launcher.cpp) and "Delete confirmation" (dialogs.cpp): the MENU screen with every app from app_store::count()/at() in a two-column grid, UP/DOWN by row, a short LEFT or RIGHT press by column, RIGHT held for 800 ms opens the delete confirmation for the selected Lua app (never for a native app), SELECT launches, CANCEL opens Settings; the inbox row's count; the BALANCE or SETUP NEEDED row; the barcode; the footer.
- "Settings list" (settings_list.cpp): the registered pages sorted by order; SELECT opens a page's screen or runs an action row's function.
- "App-store offer", "Installing", "App error" (dialogs.cpp): including the entry-tick guard on the offer, the 20 s escape on a stalled install, and retry on the error screen.
- "Approval, notifications, themes": nothing in the shell runs while vk::modalActive() (os.ino already skips shell::update(); the shell must redraw fully on the first pass after the approval closes).
- Bookkeeping. UPSTREAM-HOOKS.md: replace its content with a copy of both tables of docs/os/architecture/upstream-hooks.md ("Table" and "Replaced upstream files"), keeping the file's short introduction. scripts/preflash-check.sh: check 1 expects the new id list (no H14, H15, H20; H23 present; H18 and H22 optional) and additionally fails if a path listed as deleted in the "Replaced upstream files" table exists or a path listed as rewritten or edited does not; add check 7, which runs the "no Solana in user-visible strings" command (written out in brief I5 in this file; upstream-hooks.md "Checking the hooks" has the same one) and fails if it prints anything. Keep the script's existing structure (fail/pass helpers, numbering).

Already on disk: src/vk/ui/receipt.{h,cpp}, theme.{h,cpp}, leds.{h,cpp}, src/vk/host/native.h, notify.h, home.h, the native apps inbox and wallet_settings (Batch 4). Not on disk yet, written in parallel: src/vk/ui/repaint.h (5R), src/vk/shell/pages/*.cpp (5F, 5G). 5R removes the H14 lines from src/hal/display.cpp and adds the H23 lines; you only describe them in UPSTREAM-HOOKS.md.

Host tests: none. Device test to write (t_shell.py; you cannot run it; use common.to_launcher and badge.wait_state): after badge.reset(), VKSTATE app is "" and screen is "launcher"; a screenshot saved as test/device/shots/shell_launcher_<theme>.png (theme from VKGET theme, "receipt-light" when empty) and not uniform; btn down and btn right taps each change the screenshot; btn b tap gives screen "settings" (screenshot shell_settings_<theme>.png); btn b tap gives "launcher"; launching hello_native and btn b gives app "" and screen "launcher"; a pushed fixture whose main.lua calls error() gives screen "app_error" (screenshot shell_app_error_<theme>.png) and btn b tap gives "launcher"; pushing a tiny Lua fixture, moving the cursor to it (it is listed before the native apps; count taps from the app list) and VKBTN right hold 900 gives screen "app_delete" (screenshot shell_app_delete_<theme>.png), btn b keeps the app and returns to "launcher", the same again and btn a deletes it (the LIST reply no longer names it) and returns to "launcher". The offer and installing screens cannot be raised without a broker, and the boot screen cannot be captured over serial during setup(): say both in a comment at the top of the test.

Do not: run arduino-cli or scripts/build.sh, open the serial port, edit any file outside your list (not os.ino, not src/ui/shell.h, not src/hal/display.cpp, not src/config.h, nothing under src/vk/shell/pages/), or run git add/commit. Deleting your two files with rm is expected.

Final report: the format in section 3, rule 13.
```

```text
BRIEF 5F — Settings pages, group 1: Theme, Wi-Fi, Bluetooth, ESP-NOW, App push (WP37)

You are one of eight coding agents working at the same time on BadgeOS, firmware for an ESP32-S3 badge built as a fork of "Solana OS". REPO=/Users/ayush/Downloads/My Projects/Hackathons/MHacks2026 (contains a space: quote it). FW=$REPO/os. First read $REPO/docs/os/roadmap/execution-plan.md sections 3, 4 and 5, then docs/os/README.md and architecture/overview.md.

Goal: five of BadgeOS's settings pages, each one self-registering file, rewritten from upstream's screens in the Receipt layout with no behaviour lost. Upstream's shell is being deleted by agent 5A while you work; the pages are its replacement.

Read first, before anything else, because 5A deletes it: FW/src/ui/shell.cpp, functions drawWifi/updateWifi, drawBluetooth/updateBluetooth, drawEspnow/updateEspnow, drawPush/updatePush and the helpers they use (if the file is already gone: git show HEAD:os/src/ui/shell.cpp, run from REPO). Then: docs/os/ui/shell.md ("page.h", "Settings page registry", "Settings list", "Settings pages": Theme, Wi-Fi, Bluetooth, ESP-NOW, App push; "Add a settings page"; "Approval, notifications, themes"), ui/ui.md (Theme: tokens, the receipt kit, its conventions), and the headers FW/src/net/wifi_mgr.h, ble_mgr.h, espnow_mgr.h, push_server.h, FW/src/settings.h, FW/src/hal/buttons.h, leds.h, FW/src/badge_log.h, FW/src/vk/ui/receipt.h, theme.h.

Files you own (relative to FW; touch nothing else): src/vk/shell/pages/page_theme.cpp · page_wifi.cpp · page_bluetooth.cpp · page_espnow.cpp · page_push.cpp · test/device/t_pages1.py.

Implement: one file per page, each ending in one VK_SETTINGS_PAGE or VK_SETTINGS_ACTION line with the id and order of section 5.3 (theme 10, wifi 20, bluetooth 30, espnow 40, push 50). Code only against src/vk/shell/page.h as given in the block in shell.md "page.h": the header may not exist on disk yet, and you do not create or edit it. Each page's layout (receipt-kit calls and pixel positions), buttons, value text for the Settings row, refresh interval and the exact upstream calls are in its subsection of shell.md; follow it call for call:
- Theme (action row): SELECT cycles the registered themes with vk::ui::theme::count()/at()/setActive().
- Wi-Fi: status block, the five actions (scan, connect saved, start hotspot, disconnect, forget) and the scan results, with upstream's calls (wifi_mgr::startScan, connect, connectEnterprise, startAccessPoint, disconnect; push_server::begin/stop; settings::wifiSsid, wifiIsEnterprise, enterpriseConfig, wifiPassword, forgetWifi), open networks joined directly, secured ones refused with the log line and the footer hint.
- Bluetooth: ble_mgr::enabled, connected, address, begin(settings::deviceName()), end; settings::bleEnabledAtBoot, setBleEnabledAtBoot.
- ESP-NOW: the radar list of peers (espnow_mgr::peerCount, peerAt, channel), SELECT toggles (begin/end with settings::espnowChannel and setEspnowEnabledAtBoot), LEFT/RIGHT change the channel 1 to 13 (settings::setEspnowChannel, restart when enabled).
- App push: the two addresses, the pairing code, the three rows (settings::pushRequiresPairing, setPushRequiresPairing, regeneratePairingCode, pairingCode; BLE push toggle).
Draw only with the receipt kit and the page.h helpers, colours only from vk::ui::theme (inside namespace vk, upstream's palette is ::theme:: and must not be used). LED pulses use the helper in page.h (theme LED colour), never upstream's purple. Each page file is independent: deleting one removes that page and breaks nothing.

Host tests: none. Device test to write (t_pages1.py; you cannot run it; use common.to_launcher): from the launcher, btn b gives screen "settings". Rows are in order, so page n is reached with n-1 btn down taps from the top (or: tap down until SELECT lands on the wanted screen). For wifi, bluetooth, espnow and push: SELECT, assert VKSTATE screen equals the id, screenshot test/device/shots/shell_<id>_<theme>.png and assert it is not uniform, btn b, assert screen "settings". Theme: SELECT changes VKGET theme, the screen stays "settings" and the screenshot changes; a second SELECT restores the first theme. ESP-NOW: SELECT toggles (the screenshot differs), a second SELECT restores the original state. Do not start the hotspot, disconnect or forget Wi-Fi, or regenerate the pairing code in the test (the test tooling depends on them).

Do not: run arduino-cli or scripts/build.sh, open the serial port, edit any file outside your list (not page.h, not anything else under src/vk/shell/), or run git add/commit.

Final report: the format in section 3, rule 13.
```

```text
BRIEF 5G — Settings pages, group 2: Identity, Display, LEDs, Device info, Console, App store, Wallet and Inbox entries (WP37)

You are one of eight coding agents working at the same time on BadgeOS, firmware for an ESP32-S3 badge built as a fork of "Solana OS". REPO=/Users/ayush/Downloads/My Projects/Hackathons/MHacks2026 (contains a space: quote it). FW=$REPO/os. First read $REPO/docs/os/roadmap/execution-plan.md sections 3, 4 and 5, then docs/os/README.md and architecture/overview.md.

Goal: the other eight entries of BadgeOS's Settings list, each one self-registering file, rewritten from upstream's screens in the Receipt layout with no behaviour lost. Upstream's shell is being deleted by agent 5A while you work; the pages are its replacement.

Read first, before anything else, because 5A deletes it: FW/src/ui/shell.cpp, functions drawIdentity, updateIdentity, drawIdentityNew, updateIdentityNew, drawBroker, updateBroker, drawSlider, updateDisplayScreen, updateLedScreen, drawInfo, updateInfo, drawConsole, updateConsole and the helpers they use (if the file is already gone: git show HEAD:os/src/ui/shell.cpp, run from REPO). Then: docs/os/ui/shell.md ("page.h", "Settings page registry", "Settings list", "Settings pages": App store, Identity, New identity, Display, LEDs, Wallet and Inbox, Device info, Console; "Add a settings page"), ui/ui.md (Theme: tokens, the receipt kit, its conventions), and the headers FW/src/identity/identity.h, FW/src/net/broker_client.h, wifi_mgr.h, FW/src/settings.h, FW/src/hal/display.h, leds.h, power.h, badge_i2c.h, buttons.h, se050.h, FW/src/apps/app_store.h, FW/src/badge_log.h, FW/src/config.h, FW/src/lua_sdk/lua_runtime.h, FW/src/vk/host/notify.h, FW/src/vk/core/config.h, FW/src/vk/ui/receipt.h, theme.h.

Files you own (relative to FW; touch nothing else): src/vk/shell/pages/page_store.cpp · page_identity.cpp (screens "identity" and "identity_new") · page_display.cpp · page_leds.cpp · page_wallet.cpp · page_inbox.cpp · page_info.cpp · page_console.cpp · test/device/t_pages2.py.

Implement: one file per entry, each ending in one VK_SETTINGS_PAGE or VK_SETTINGS_ACTION line with the id and order of section 5.3 (store 60, identity 70, display 80, leds 90, wallet 100, inbox 110, info 120, console 130). Code only against src/vk/shell/page.h as given in the block in shell.md "page.h": the header may not exist on disk yet, and you do not create or edit it. Each entry's layout, buttons, value text, refresh interval and exact upstream calls are in its subsection of shell.md; follow it call for call:
- App store: state, registration, address, last error, the note that the address is set from the web page; rows "App store" (broker::setEnabled(!broker::enabled())) and "Forget registration" (broker::forget()). With the default empty address the page reads "off".
- Identity: badge id, key location (identity::sourceName, source), status, the full public key wrapped by hand; SELECT pushes the second screen "identity_new" (vk::shell::push), whose SELECT raises the hold-SELECT firmware confirmation of shell.md "New identity" (vk::wallet::approval::confirm: title "New identity", headline "ERASE BADGE KEY", amber, hold) and only on approval calls identity::regenerate() then broker::forget(), logs, pulses, pops and requests a repaint; CANCEL returns without doing anything. Never regenerate on a plain press: the identity is the wallet key.
- Display: backlight with LEFT/RIGHT in steps of 8, floor 8, applied live (settings::setBrightness, display::setBrightness).
- LEDs: brightness with LEFT/RIGHT in steps of 8 (settings::setLedBrightness, ::leds::setBrightness); SELECT previews; CANCEL stops the animation, turns the LEDs off and leaves, as upstream does.
- Wallet and Inbox (action rows): runtime::requestLaunch("wallet_settings") and runtime::requestLaunch("inbox") after ::leds::stopAnimation(); the Inbox row's value is the waiting-notification count.
- Device info: the twelve fields upstream shows, firmware as SOLANA_OS_NAME " " SOLANA_OS_VERSION (the macro's value is "BadgeOS"); SELECT re-scans (badge_i2c::retry, buttons::retry, se050::test, badge_i2c::scan).
- Console: the log ring (badge_log::lineCount, line), newest at the bottom, UP/DOWN scroll, lines containing "error", "failed" or "FATAL" in the STAMP_BAD ink.
Draw only with the receipt kit and the page.h helpers, colours only from vk::ui::theme (inside namespace vk, upstream's palette is ::theme:: and must not be used). LED pulses use the helper in page.h. Each file is independent: deleting one removes that entry and breaks nothing.

Host tests: none. Device test to write (t_pages2.py; you cannot run it; use common.to_launcher): from the launcher, btn b gives screen "settings"; rows are in order. For store, identity, display, leds, info, console: SELECT, assert VKSTATE screen equals the id, screenshot test/device/shots/shell_<id>_<theme>.png (not uniform), btn b, assert screen "settings". Identity: SELECT on the page gives screen "identity_new" (screenshot); leave it with btn b ONLY. NEVER send SELECT on "identity_new": it destroys the badge's key and address; put that warning in a comment above the step. Display: btn left then btn right each change the screenshot and VKSTATE stays on "display"; leave the brightness as found. Wallet row: SELECT gives VKSTATE app "wallet_settings"; common.to_launcher returns to screen "launcher". Inbox row: SELECT gives app "inbox"; with VKNOTE a|b|c posted first, the Settings row and the launcher screenshots differ from the empty case.

Do not: run arduino-cli or scripts/build.sh, open the serial port, edit any file outside your list (not page.h, not anything else under src/vk/shell/), or run git add/commit.

Final report: the format in section 3, rule 13.
```

```text
BRIEF 5R — Rebrand to BadgeOS, removal of the status-item registry and the home service, test tooling (WP37)

You are one of eight coding agents working at the same time on BadgeOS, firmware for an ESP32-S3 badge built as a fork of "Solana OS". REPO=/Users/ayush/Downloads/My Projects/Hackathons/MHacks2026 (contains a space: quote it). FW=$REPO/os. First read $REPO/docs/os/roadmap/execution-plan.md sections 3, 4 (with the paragraph "Changed by the Batch 5 design change") and 5, then docs/os/README.md and architecture/overview.md.

Goal: nothing a user can see, and no network identifier, says "Solana" or "SKYRIZZ" or carries upstream's purple and green; the two mechanisms the new shell makes unnecessary (status items, home service) are removed; the test tooling knows which shell screen is showing. Agent 5A rewrites the shell and the boot screen at the same time; you do everything else in this list.

Read: docs/os/architecture/upstream-hooks.md (Rules, Table, H23, Replaced upstream files, Checking the hooks), ui/shell.md ("Framework" for ::shell::screenName(), "What was removed", "Tests"), ui/ui.md (Header, Theme tokens), testing/testing.md (Dev hooks: VKSTATE), platform/config.md, guides/build-flash-provision.md. Code: FW/src/config.h, FW/src/net/espnow_mgr.cpp, FW/src/lua_sdk/lib_gfx.cpp, FW/src/net/push_server.cpp (its embedded web page), FW/src/ui/theme.h, FW/src/hal/display.cpp (statusBar), FW/tools/badge-push.py, FW/README.md, FW/src/vk/ui/statusbar.{h,cpp}, FW/src/vk/vk.cpp, FW/src/vk/host/home.{h,cpp}, FW/src/vk/features/devtools/devtools.cpp (cmdState), FW/test/device/common.py and every test/device/t_*.py and fixtures.py.

Files you own (relative to FW; touch nothing else, and in a file marked "only" change only what is named):
- Names, hook H23, each changed line ending in "// VK: H23": src/config.h (only: SOLANA_OS_NAME value "BadgeOS"; DEFAULT_HOSTNAME "badgeos"; DEFAULT_AP_PASSWORD "badgeos-setup"; DEFAULT_BROKER_URL ""; the macro and constant names stay) · src/net/espnow_mgr.cpp (only: MAGIC becomes 'B','D','O','S') · src/lua_sdk/lib_gfx.cpp (only: the four constants SOLANA_PURPLE, SOLANA_GREEN, SOLANA_TEAL, SOLANA_MAGENTA are removed and one tagged comment line takes their place; gfx.PAPER and gfx.INK are NOT added: apps take colours from badge.theme.color).
- Edited without tags (listed in upstream-hooks.md "Replaced upstream files"): src/net/push_server.cpp (only the embedded page: title and heading "BadgeOS", the :root colours to Receipt-light: paper #F3EFE4, ink #1B1A17, faint #8A8474, sub #6D6759, ok #17804F, bad #C8321E; no purple, no brand green, no gradient) · src/ui/theme.h (only the palette values, to the ones in upstream-hooks.md "Replaced upstream files"; every constant name stays) · tools/badge-push.py (only texts: "BadgeOS badge", example host badgeos.local) · README.md (rewrite, short: what BadgeOS is in a few lines, the one line "BadgeOS is built on Solana OS by spacemandev.", and a pointer to docs/os/, naming docs/os/reference/upstream-readme.md as the copy of upstream's README with the Lua API).
- Deleted: apps/dice, apps/gallery, apps/hello, apps/radar, apps/vumeter, apps/whosnear (whole folders) · src/vk/ui/statusbar.h, statusbar.cpp, status_dev.cpp.
- Status items removed: src/hal/display.cpp (only: remove the two "// VK: H14" lines, the include and the call, so the file is upstream's text again) · src/vk/ui/repaint.h, repaint.cpp (create: namespace vk::ui { void requestShellRepaint(); bool consumeShellRepaint(); } with the bodies statusbar.cpp had; 5A's shell includes this header) · src/vk/core/config.cpp, src/vk/features/balance/balance.cpp, src/vk/host/notify.cpp (only: delete the VK_STATUS_ITEM line and its draw function, include "repaint.h" by the right relative path instead of "statusbar.h"; every requestShellRepaint() call stays, and notify.cpp calls it when a note is posted or removed) · src/vk/wallet/approval.cpp (only the include) · src/vk/vk.cpp (only the "[vk] registries:" line: drop status=, add pages=%u with countOf<shell::SettingsPage>(), including "shell/page.h", which 5A creates from the block in shell.md "page.h").
- Home service removed: src/vk/host/home.h, home.cpp (only bool vk::host::idle() stays, returning !runtime::running(); showShell() goes; fix the comments). No config key home_app is registered anywhere: check with grep and remove it if a Batch 4 file added it.
- Test tooling: src/vk/features/devtools/devtools.cpp (only cmdState: add ,"screen":"<name>" directly after "native", from ::shell::screenName(), declared in "../../shell/screens.h", which 5A creates: namespace shell { const char *screenName(); }; inside namespace vk write ::shell::, see section 3 rule 8) · test/device/common.py (to_launcher: dismiss an open approval with b, stop the app, then tap b until state()["screen"] == "launcher", at most 6 taps, asserting at the end; new helper goto_screen(badge, name): to_launcher, b, then down/SELECT/b through the Settings rows until state()["screen"] == name, for "settings" just the first b; launch unchanged) · every existing test and fixture that assumed upstream's launcher, upstream's status bar or an upstream sample app: t_boot.py (the registries line and the navigation check), t_cfg.py (the SETUP check: assert VKSTATE provisioned false and screen "launcher" instead of reading upstream's bar), t_apr.py (repaint check: screen "launcher"), t_hook.py, t_clock.py, fixtures.py (launch a pushed fixture or the native hello_native, never hello) · scripts/push-apps.sh (only if it names an upstream sample: drop it) · every user-visible "BadgeOS" string under FW becomes "BadgeOS": scripts/vkdev.py (the argparse description only), fixture author= lines in test/device/, and the "needs a newer … (API <n>)" error text in src/vk/host/permissions.cpp (that one string only). Comments may be left.

What stays, on purpose (do not rename): the store registration text "solana-badge-register:" (upstream's store protocol, used only if a broker is configured); everything named after the Solana blockchain (feature folder solana_pay, signing domain "solana", wallet.begin_solana, sol_*.c, rpc_url examples); upstream's macro names SOLANA_OS_NAME, SOLANA_OS_VERSION, SOLANA_OS_API_VERSION (invisible to users; only the value changes); the identity self-test text in src/identity/identity.cpp (never shown or sent); the device name default badge-XXXX; comments in upstream files.

Check before finishing, from FW (must print nothing; if a line you do not own prints, report it, do not edit it):
grep -rniE 'solana|skyrizz' os.ino src tools/badge-push.py README.md | grep -v -e '^src/lua/' -e '^src/vk/features/solana_pay/' -e '^src/vk/wallet/pure/sol' | grep -vE '^[^:]+:[0-9]+:[[:space:]]*(//|/\*|\*)' | grep -vE 'SOLANA_OS_(NAME|VERSION|API_VERSION)|solana_pay|begin_solana|"solana"|solana-badge-register:|solana-badge identity self test|Solana OS by spacemandev'
(src/ui/boot.cpp and src/ui/shell.cpp still print until 5A's work lands; ignore those two.) Run python3 -m py_compile on every Python file you edit, and test/host/run.sh for any suite that links a file you touched.

Device tests to write: none of your own; the tooling and the repaired tests are run by the integrator.

Also yours: in src/vk/host/notify.cpp the `notify` LED pattern must draw in the active theme's LED colour (vk::ui::theme::color(vk::ui::theme::LED)), not upstream's purple (ui.md "LED patterns"); and any other literal of upstream's brand colours (0x9945FF, 0x14F195, 0x00FFA3, 0xDC1FFF, or 0x99,0x45,0xFF as bytes) left under src/vk/ or src/native_apps/ is replaced by a theme colour: grep for them and list what you changed.

Do not: run arduino-cli or scripts/build.sh, open the serial port, edit any file outside your list (not src/ui/shell.cpp, boot.cpp, splash_images.h, UPSTREAM-HOOKS.md, scripts/preflash-check.sh or anything under src/vk/shell/: those are 5A's), or run git add/commit. Deleting the listed files and folders with rm is expected.

Final report: the format in section 3, rule 13.
```

```text
BRIEF 5B — Home and History apps (WP40, WP42)

You are one of eight coding agents working at the same time on BadgeOS, firmware for an ESP32-S3 badge whose apps are written in Lua 5.4 (32-bit integers). REPO=/Users/ayush/Downloads/My Projects/Hackathons/MHacks2026 (contains a space: quote it). FW=$REPO/os. First read $REPO/docs/os/roadmap/execution-plan.md sections 3 and 5, then docs/os/README.md and architecture/overview.md.

Goal: the Home app (the badge's landing screen) and the History app (the signature log), in the Receipt look.

Read: docs/os/apps/apps.md (Rules for every Lua app, Home, History), platform/lua-api.md (identity, history, balance, lib/vk.lua: vk.ui), ui/ui.md (Screens: Home, Any list), reference/reasons.md, FW/lib/vk.lua (read vk.ui before drawing), docs/os/reference/upstream-readme.md (the copy of upstream's README: the upstream Lua API and the app format; FW/README.md is being rewritten by another agent), docs/design/os-mockups/index.html (home and history screens).

Files you own (relative to FW; touch nothing else): apps/home/app.ini, main.lua, config.lua · apps/history/app.ini, main.lua, config.lua · test/device/t_app_home.py, t_app_history.py.

Implement: Home (permissions=net; min_api=2) exactly as apps.md "Home" and the Home layout in ui.md (left stub BALANCE amount and barcode-like block; body rows ADDRESS, KEY, CLOCK, INBOX is not readable from Lua: show the rows the Lua API can fill and omit the rest; rule; THANK YOU FOR HACKING), menu of apps from config.lua filtered by badge.system.apps(), refresh_balance on start and every wallet.config("balance_poll_s") seconds without ever blocking the UI on failure. History (permissions=history; min_api=2) exactly as apps.md "History", list of wallet.history(64) with a detail view. Both draw only with vk.ui, keep every tunable in config.lua, use strings for amounts, and exit on CANCEL (badge.system.exit(): back to the launcher, which is the firmware's shell, not an app).

Device tests to write (you cannot run them; push with extra file vk.lua = lib/vk.lua; launch with common.launch): t_app_home.py: screen drawn (screenshot saved to test/device/shots/home_<theme>.png and not uniform), the address shown matches VKINFO pubkey's first and last 4 characters (assert through a badge.log line "HOME addr <short>" the app prints once), no "[lua]" error line in the log for 10 s with no network, btn b returns to the launcher. t_app_history.py: after at least one approval exists (run VKDEMOAPPROVE green and approve it first), the app logs "HIST n <count>" with count >= 1, btn a opens the detail view (screenshot differs), btn b twice returns to the launcher.

Batch 5 rules: there is no launcher app. The launcher is the firmware's own shell: in a test, "returns to the launcher" means VKSTATE app is "" and screen is "launcher" (badge.wait_state(lambda s: s["app"] == "" and s["screen"] == "launcher"); common.to_launcher gets there from anywhere). Draw only with vk.ui (colours from badge.theme.color; the gfx.SOLANA_* constants no longer exist); no stamps anywhere (vk.ui.stamp does not exist). Upstream's sample apps are deleted: do not launch them or copy from them.

Do not: run arduino-cli or scripts/build.sh, open the serial port, edit any file outside your list (not lib/vk.lua: report what is missing), or run git add/commit.

Final report: the format in section 3, rule 13.
```

```text
BRIEF 5C — Pay and Request apps (WP41)

You are one of eight coding agents working at the same time on BadgeOS, firmware for an ESP32-S3 badge whose apps are written in Lua 5.4 (32-bit integers). REPO=/Users/ayush/Downloads/My Projects/Hackathons/MHacks2026 (contains a space: quote it). FW=$REPO/os. First read $REPO/docs/os/roadmap/execution-plan.md sections 3 and 5, then docs/os/README.md and architecture/overview.md (section 9: the data flow of one payment).

Goal: the two apps of the core demo: Pay (list nearby requests, pay one) and Request (ask to be paid, wait, confirm on chain).

Read: docs/os/apps/apps.md (Rules, Pay, Request), platform/lua-api.md (payments, requests, lib/vk.lua, vk.pay), protocol/espnow.md (Sequences, RESULT), ui/ui.md (Screens: Any list, Request), reference/reasons.md, FW/lib/vk.lua, docs/os/reference/upstream-readme.md (the copy of upstream's README: the upstream Lua API and the app format; FW/README.md is being rewritten by another agent), docs/design/os-mockups/index.html (pay and request screens).

Files you own (relative to FW; touch nothing else): apps/pay/app.ini, main.lua, config.lua · apps/request/app.ini, main.lua, config.lua · test/device/t_app_pay.py, t_app_request.py, t_pay_2.py.

Implement: Pay (permissions=sign,net,espnow; min_api=2) exactly as apps.md "Pay", steps 1 to 3: list from wallet.requests() sorted by rssi, refreshed every 500 ms, claimed name labelled as a claim; SELECT starts vk.pay.start{request = entry}; state shown in words; on failed with unverified, revoked, mismatch, bad_proof or expired call vk.report. Request (permissions=request,net,espnow; min_api=2) exactly as apps.md "Request", steps 1 to 3: amount stepping in integer minor units from config.step, request_open, waiting screen with seconds left and "badges checking: n", RESULT handling with vk.result_parse and vk.confirm every 2 s for up to 30 s, PAID label only after "confirmed". Both draw only with vk.ui, keep tunables in config.lua, never draw anything resembling the firmware approval, and exit or go back on CANCEL (CANCEL on the waiting screen closes the request).

Device tests to write (you cannot run them): t_app_pay.py (one badge): the list shows "No requests nearby" (log line "PAY list 0"), screenshot saved, no Lua error for 10 s, btn b returns to the launcher. t_app_request.py (one badge; VKTIME first): UP changes the amount (log "REQ amount <text>"), SELECT opens (log "REQ open <req_id>"), the waiting screen is drawn, btn b closes the request (log "REQ closed") and a second btn b exits; with the clock unset the app shows the no_time reason and does not crash. t_pay_2.py with NEEDS = "two-badges": request 10.00 on B, listed on A, paid, both show the result (also needs the network for the chain confirmation: say so in a comment).

Batch 5 rules: there is no launcher app. The launcher is the firmware's own shell: in a test, "returns to the launcher" means VKSTATE app is "" and screen is "launcher" (badge.wait_state(lambda s: s["app"] == "" and s["screen"] == "launcher"); common.to_launcher gets there from anywhere). Draw only with vk.ui (colours from badge.theme.color; the gfx.SOLANA_* constants no longer exist); no stamps anywhere (vk.ui.stamp does not exist). Upstream's sample apps are deleted: do not launch them or copy from them.

Do not: run arduino-cli or scripts/build.sh, open the serial port, edit any file outside your list, or run git add/commit.

Final report: the format in section 3, rule 13.
```

```text
BRIEF 5D — Contacts, Game and Evil game apps (WP43, WP44)

You are one of eight coding agents working at the same time on BadgeOS, firmware for an ESP32-S3 badge whose apps are written in Lua 5.4 (32-bit integers). REPO=/Users/ayush/Downloads/My Projects/Hackathons/MHacks2026 (contains a space: quote it). FW=$REPO/os. First read $REPO/docs/os/roadmap/execution-plan.md sections 3 and 5, then docs/os/README.md and architecture/overview.md.

Goal: the Contacts app (list and swap), a small arcade game with a shop that takes payments, and the demo variant whose shop lies.

Read: docs/os/apps/apps.md (Rules, Contacts, Game, Evil game), platform/lua-api.md (contacts, payments, lib/vk.lua, vk.pay), protocol/espnow.md (CONTACT_HELLO, CONTACT_CARD), guides/build-flash-provision.md (Installing apps: how evilgame is assembled), FW/lib/vk.lua, docs/os/reference/upstream-readme.md (the copy of upstream's README: the upstream Lua API and the app format; FW/README.md is being rewritten by another agent), docs/design/os-mockups/index.html (contacts, game screens).

Files you own (relative to FW; touch nothing else): apps/contacts/app.ini, main.lua, config.lua · apps/game/app.ini, main.lua, config.lua · apps/evilgame/app.ini, config.lua · test/device/t_app_contacts.py, t_app_game.py.

Implement: Contacts (permissions=contacts,espnow) exactly as apps.md "Contacts": list with RIGHT to remove after a confirm line; swap mode broadcasting wallet.contact_hello() once a second, answering a HELLO on SELECT with contact_card unicast, accepting a CARD with contact_accept, names labelled "self-named". Game (permissions=sign,net,espnow,storage) exactly as apps.md "Game": the dodge game, high score in badge.storage, shop from config.shop.items buying with vk.pay.start{to = config.shop.recipient, amount = item.price, memo = item.name}, unlock stored on done. All game code must read its behaviour from config.lua, because evilgame is the same main.lua with another config: when config.evil is "amount" the transfer is built for config.evil_amount while the screen shows item.price; when "recipient" the transfer goes to config.evil_recipient while passing the real shop's record (for these two cases the game cannot use vk.pay's defaults: build the transfer with wallet.build_transfer and call wallet.begin_solana itself, sharing one code path with the honest case where practical). evilgame's config.lua repeats game's config and adds evil, evil_amount, evil_recipient. config.shop.recipient is a deployment value: keep a placeholder string and a comment that provisioning replaces it. All screens with vk.ui except the playfield.

Device tests to write (you cannot run them): t_app_contacts.py: empty list shown (log "CON list 0"), SELECT enters swap mode (log "CON swap on"), btn b leaves it, btn b exits. t_app_game.py: title screen drawn, SELECT starts play, the score advances (log "GAME score <n>" once a second), after game over the title returns; entering the shop and buying with no network ends in a visible failure (log "GAME buy failed <reason>") and the app keeps running; btn b exits. The evil-game checks (amount shows 500.00 on the firmware screen; recipient is red) need the registry over a network: describe them in a comment block at the top of t_app_game.py.

Batch 5 rules: there is no launcher app. The launcher is the firmware's own shell: in a test, "returns to the launcher" means VKSTATE app is "" and screen is "launcher" (badge.wait_state(lambda s: s["app"] == "" and s["screen"] == "launcher"); common.to_launcher gets there from anywhere). Draw only with vk.ui (colours from badge.theme.color; the gfx.SOLANA_* constants no longer exist); no stamps anywhere (vk.ui.stamp does not exist). Upstream's sample apps are deleted: do not launch them or copy from them.

Do not: run arduino-cli or scripts/build.sh, open the serial port, edit any file outside your list, or run git add/commit.

Final report: the format in section 3, rule 13.
```

```text
BRIEF 5E — Duel app (WP45)

You are one of eight coding agents working at the same time on BadgeOS, firmware for an ESP32-S3 badge whose apps are written in Lua 5.4 (32-bit integers). REPO=/Users/ayush/Downloads/My Projects/Hackathons/MHacks2026 (contains a space: quote it). FW=$REPO/os. First read $REPO/docs/os/roadmap/execution-plan.md sections 3 and 5, then docs/os/README.md and architecture/overview.md.

Goal: a two-badge reaction duel whose stake is settled through the ordinary request-and-pay flow.

Read: docs/os/apps/apps.md (Rules, Duel), platform/lua-api.md (requests, payments, lib/vk.lua: vk.app_frame, vk.app_body, vk.pay, vk.result_parse, vk.confirm), protocol/espnow.md (Type registry: types 64 to 71), FW/lib/vk.lua, docs/os/reference/upstream-readme.md (the copy of upstream's README: the upstream Lua API and the app format; FW/README.md is being rewritten by another agent).

Files you own (relative to FW; touch nothing else): apps/duel/app.ini, main.lua, config.lua · test/device/t_app_duel.py, t_duel_2.py.

Implement: Duel (permissions=sign,request,net,espnow; min_api=2) exactly as apps.md "Duel", steps 1 to 4: INVITE (type 64: stake string, 8-byte game id), ACCEPT (65), GO (66, delay chosen by the inviter), TIME (67), best of config.rounds; the winner calls wallet.request_open{amount = stake}; the loser finds the request whose payee is the winner's key in wallet.requests() and runs vk.pay.start{request = entry}; the winner shows PAID only after vk.confirm, or "unpaid" after config.settle_timeout_s. Stakes, rounds and timeouts in config.lua. Frames built and matched only with vk.app_frame and vk.app_body. Every state has a timeout back to the title screen; CANCEL always leaves.

Device tests to write (you cannot run them): t_app_duel.py (one badge): title drawn, a stake can be chosen (log "DUEL stake <text>"), inviting with nobody around times out back to the title (log "DUEL invite timeout"), btn b exits. t_duel_2.py with NEEDS = "two-badges": invite, accept, play, settle, and "unpaid" when the loser cancels.

Batch 5 rules: there is no launcher app. The launcher is the firmware's own shell: in a test, "returns to the launcher" means VKSTATE app is "" and screen is "launcher" (badge.wait_state(lambda s: s["app"] == "" and s["screen"] == "launcher"); common.to_launcher gets there from anywhere). Draw only with vk.ui (colours from badge.theme.color; the gfx.SOLANA_* constants no longer exist); no stamps anywhere (vk.ui.stamp does not exist). Upstream's sample apps are deleted: do not launch them or copy from them.

Do not: run arduino-cli or scripts/build.sh, open the serial port, edit any file outside your list, or run git add/commit.

Final report: the format in section 3, rule 13.
```

```text
BRIEF I5 — Integrator, Batch 5

You integrate the fifth batch of BadgeOS (the BadgeOS shell replacing upstream's: launcher, settings list and pages, dialogs, boot screen; the rebrand; and the Lua apps Home, History, Pay, Request, Contacts, Game, Evil game, Duel). REPO=/Users/ayush/Downloads/My Projects/Hackathons/MHacks2026 (contains a space: quote it). FW=$REPO/os. PY=$REPO/.venv/bin/python. Port /dev/cu.usbserial-10. Read $REPO/docs/os/roadmap/execution-plan.md sections 1, 5, 6 (follow section 6 step by step), 8 and 9, docs/os/ui/shell.md (all), ui/ui.md, architecture/upstream-hooks.md (Table, Replaced upstream files, Checking the hooks), apps/apps.md, roadmap/implementation-plan.md (WP37, WP40 to WP45), and open docs/design/os-mockups/index.html. The eight implementer reports (5A, 5F, 5G, 5R, 5B, 5C, 5D, 5E) follow this brief.

Batch-specific steps:
- Collect (section 6 step 1): this batch deletes upstream files on purpose. Expect as deleted: src/ui/shell.cpp, splash_images.h, src/vk/ui/statusbar.h, statusbar.cpp, status_dev.cpp, and the folders apps/dice, gallery, hello, radar, vumeter, whosnear. Check from FW: test ! -e splash_images.h && test ! -e src/ui/shell.cpp, and ls apps shows none of the six upstream samples.
- Build: three interfaces were written by one agent and used by others before they existed: src/vk/shell/page.h and screens.h (5A; used by 5F, 5G, 5R) and src/vk/ui/repaint.h (5R; used by 5A). Where they disagree, the block in shell.md "page.h" is the contract: fix the file that departs from it (Risk 6). If a settings action is missing, upstream's shell is the reference: git show HEAD:os/src/ui/shell.cpp before your commit (Risk 7). The pre-flash check now has the "Replaced upstream files" part of check 1 and check 7; if one fails, fix the cause, not the check.
- Flash. No power cycle is needed. The first boot after this flash changes the network names: hostname badgeos, hotspot password badgeos-setup, ESP-NOW magic BDOS. A badge still on older firmware no longer hears this one over ESP-NOW; write that in the tracking notes.
- The boot line: "[vk] registries:" has no status= any more and has pages=13. Copy it into the tracking notes.
- Install the apps: scripts/push-apps.sh --port /dev/cu.usbserial-10 dev (allow several minutes). VKAUTOSTART home is NOT set during testing (tests expect the launcher after reset).
- Upstream samples pushed to this badge by earlier batches are still on its filesystem: send AUTH, then LIST, then DEL for each of hello, dice, gallery, radar, vumeter, whosnear that LIST names.
- "The launcher" now means the shell's own screen: VKSTATE app is "" and screen is "launcher". There is no launcher app. Where an earlier test still expects upstream's launcher, upstream's status bar or an upstream sample app and 5R did not repair it, fix the test, not the firmware.
- Device tests, in this order: t_boot.py · t_shell.py · t_pages1.py · t_pages2.py · t_apr.py · t_app_home.py · t_app_history.py · t_app_pay.py · t_app_request.py · t_app_contacts.py · t_app_game.py · t_app_duel.py · then the regression set t_cfg.py, t_clock.py, t_app.py, t_native.py, t_notify.py, t_vk.py, t_sign.py, t_chk.py, t_sto.py.
- Run t_shell.py, t_pages1.py, t_pages2.py and t_apr.py once per theme: VKSET theme receipt-light, run the four; VKSET theme receipt-dark, run them again; set the theme back. The screenshots land in os/test/device/shots/ as shell_<screen>_<theme>.png: launcher, settings, wifi, bluetooth, espnow, push, store, identity, identity_new, display, leds, info, console, app_error, app_delete, in both themes (30 files). Read every PNG and compare the launcher, the settings list and the boot layout with the simulation; fix overlapping or clipped text; list remaining visual differences in the tracking notes for a person to judge. Never send SELECT on screen identity_new: it destroys the badge's key.
- Scripted T-REL1: for each shipped app, launch it, tap b up to five times, and assert app "" and screen "launcher"; then launch it again, send VKBTN b hold 1700, and assert the same.
- No "Solana" check, from FW. Expected output: nothing. A printed line whose only match is in a trailing code comment is recorded in the tracking notes; anything else is fixed in the file that owns it (through hook H23 or the "Replaced upstream files" list if it is an upstream file, updating upstream-hooks.md and UPSTREAM-HOOKS.md in the same change):
  cd "$FW" && grep -rniE 'solana|skyrizz' os.ino src tools/badge-push.py README.md \
    | grep -v -e '^src/lua/' -e '^src/vk/features/solana_pay/' -e '^src/vk/wallet/pure/sol' \
    | grep -vE '^[^:]+:[0-9]+:[[:space:]]*(//|/\*|\*)' \
    | grep -vE 'SOLANA_OS_(NAME|VERSION|API_VERSION)|solana_pay|begin_solana|"solana"|solana-badge-register:|solana-badge identity self test|Solana OS by spacemandev'
  The second grep drops the vendored Lua interpreter and the payment code named after the Solana blockchain; the third drops whole-line comments; the fourth drops the names kept on purpose: upstream's macro names, the blockchain identifiers, the store registration text, the identity self-test text, and the README credit line. Also confirm on the badge: PING over serial answers "OK BadgeOS …", and the log banner reads "BadgeOS".
- Boot screen: it cannot be captured over serial. Verify from the log that no "[boot] splash" line appears, that every stage is reached and boot reaches "[os] ready" within 30 s (it is several seconds shorter than before: the two splashes are gone); record "boot screen and LED bar: needs a person (T-LED1)".
- Offer and installing screens: they need a broker and DEFAULT_BROKER_URL is empty; record them as deferred (compiled, not seen).

Deferred: every two-badge test (t_pay_2.py, t_duel_2.py, t_con.py, t_req.py; the ESP-NOW page's peer list), every network path (payment submission, chain confirmation, registry, feed, balance, evil game demo, Wi-Fi page join and hotspot actions, app-store offer and installing screens), T-LED1 and the boot screen's appearance.
Commit message: "WP37: batch 5, BadgeOS shell rewrite and rebrand, all apps (WP37 WP40 WP41 WP42 WP43 WP44 WP45)".
Gate: all host suites pass; every device test above passes; every settings page was reached and screenshotted in both themes; every shipped app returns to the launcher (app "", screen "launcher") on CANCEL and on a 1.7 s hold; the "Solana" check prints nothing; the pre-flash check passes with the new hook list and the replaced-file list.

Final report: the format in section 6, step 11.
```

### Batch 6

```text
BRIEF 6A — Bank rail (WP54, optional; dispatched only if Batches 1 to 5 closed with no open failure)

You are a coding agent working on BadgeOS, firmware for an ESP32-S3 badge. REPO=/Users/ayush/Downloads/My Projects/Hackathons/MHacks2026 (contains a space: quote it). FW=$REPO/os. First read $REPO/docs/os/roadmap/execution-plan.md sections 3, 4 and 5, then docs/os/README.md and architecture/overview.md.

Goal: the optional "bank" signing domain: a text payload authorising a bank payment, decoded and checked by the firmware and shown on the same approval screen. The backend routes it needs do not exist, so it is verified by its host suite and by compiling only.

Read: docs/os/wallet/checks.md (Bank rail; The check chain rows 4 to 9 and 11; Verdict to screen), wallet/signing.md (Domain table row bank; Cross-feature interfaces), wallet/approval.md (The request), platform/lua-api.md (begin_bank), protocol/espnow.md (REQ, rail 2), FW/src/vk/features/solana_pay/domain_solana.cpp (as the model for a decoder).

Files you own (relative to FW; touch nothing else): src/vk/features/bank/domain_bank.cpp, bank_payload.h, bank_payload.c, lua_bank.cpp · test/host/test_bank.c.

Implement: bank_payload.{h,c}: a pure C99 strict parser of the twelve key=value lines in the exact order of checks.md "Bank rail" (no trailing newline, at most 512 bytes) into a struct. domain_bank.cpp: VK_SIGN_DOMAIN(bank, "bank", "bank-auth:", true, "sign", 512, decodeBank, nullptr); decodeBank requires ctx.record, ctx.record_sig and ctx.req, verifies the record (and raises the clock), applies checks 4 to 9 and 11 with req.rail bank using vk_record_parse, vk_record_verify, vk_req_parse and vk_verify_c, then the bank-specific equalities listed in checks.md (payee_ref, payee_name, action by kind, req_id, amount_cents, proof_nonce against the third output of vk::wallet::presenceLookup), treats "not present" as red, and fills the ApprovalRequest with title "Pay", the amount as dollars and cents with symbol "USD", and headlines and reasons from reasons.md. lua_bank.cpp: wallet.begin_bank(payload, [ctx]) = vk::wallet::luaBegin(L, "bank", 1, 2), permission "sign". Do not include any header of another feature.

Host test first (test_bank.c, first line "// LINK: src/vk/features/bank/bank_payload.c"): the canonical payload parses; each of: reordered key, missing key, extra line, trailing newline, non-numeric amount, upper-case hex, over-long payload, is refused.
Command: cd "$FW" && test/host/run.sh test_bank.

Do not: run arduino-cli or scripts/build.sh, open the serial port, edit any file outside your list (test_domains.cpp already lists the bank row), or run git add/commit.

Final report: the format in section 3, rule 13.
```

```text
BRIEF I6 — Final integrator (WP50, WP51, WP52 single-badge part, WP54 if written)

You close out the BadgeOS build on the one badge available. REPO=/Users/ayush/Downloads/My Projects/Hackathons/MHacks2026 (contains a space: quote it). FW=$REPO/os. PY=$REPO/.venv/bin/python. Port /dev/cu.usbserial-10. Read $REPO/docs/os/roadmap/execution-plan.md sections 5, 6 (follow section 6 step by step), 8 and 9, docs/os/testing/testing.md (Secure element, Release gate, Measurements), wallet/signing.md (Key), guides/build-flash-provision.md (Pre-flash checks, SE050 fallback), roadmap/implementation-plan.md (WP50 to WP54). Agent 6A's report follows this brief if it ran.

Steps, after section 6 steps 1 to 5 on the dev profile:
1. Full regression: every test/device/t_*.py without NEEDS, t_boot.py first. Record PASS/FAIL per file.
2. Measurements: M2 (sign and verify, from the logs), M3, M4 (image size from the build; VKSTATE heap in the launcher and during an approval; the "[os] up … heap … psram" heartbeat line). M6: add inside #if VK_PROFILE_DEV in src/vk/features/devtools/devtools.cpp a once-a-minute log of uxTaskGetStackHighWaterMark(NULL), plus one log right after a signature; run t_sign.py and t_chk.py; record the lowest value. If it is under 1024 bytes, apply Risk 5's fallback (hook H22) and re-measure.
3. Secure element: if VKINFO says key=se050, t_sign.py's signature test is T-SE1; add a memo case (checktest transfer with memo) and expect "CT begin err too_long" (T-SE2). If T-SE1 fails, capture the "[id] SE050 refused to sign" line (stage and status word), spend at most one hour, then record the failure and the fallback steps of the build guide as "needs a person" (creating a new identity destroys the badge's address; do not do it unattended). If key=software, record "T-SE1, T-SE2: need an SE050-keyed badge".
4. Release profile: scripts/build.sh release (must pass pre-flash check 6), then --upload, wait-ready, and T-REL2: send VKSTATE, VKBTN a tap, VKSHOT, VKTIME 1, VKPAIR, VKNOTE a|b|c, VKDEMOAPPROVE green; none may answer OK. VKINFO must show profile=release. Record the release image size. Then flash the dev profile again and rerun t_boot.py.
5. Docs: fill testing.md's Measurements table; update every [UNVERIFIED] item in README "Open items" with what is now known; fill all remaining tracking rows; copy section 8 of the execution plan into implementation-plan.md under the tracking table as "Deferred verification", corrected to what actually remains.

Deferred, to be listed and not attempted: M1, M5, T-REL1 by hand, T-REL3, T-APR6, WP52's four badges.
Commit message: "WP51: batch 6, measurements, release build check, deferred list (WP50 WP51 WP52 WP54)".
Gate: regression passes or failures are recorded; release build compiles and passes T-REL2; the badge is left on the dev profile.

Final report: the format in section 6, step 11, plus the measurement table.
```

## 8. Deferred list

Code for everything below is written and compiled; only verification waits. Each has a test file that `vkdev.py test` skips until `--include-deferred`, unless marked "no script".

**Needs a second badge (two USB ports)**

- T-HOOK1: second app receives ESP-NOW after the first exits (`t_hook.py`).
- Settings → ESP-NOW: the peer list with a real peer. Both badges must run BadgeOS: the ESP-NOW magic is `BDOS`, so a badge on upstream firmware or on a build older than Batch 5 is not heard.
- T-REQ1–4: request listed, presence present, replay stays pending, proof cap (`t_req.py`).
- T-CHK1: green VERIFIED - PRESENT.
- T-REQ5: a request raises a notification and the Inbox opens Pay (no script yet: `t_notify_2.py`, steps in the WP32 tracking row).
- T-CON1, T-CON2: contact swap and card replay (`t_con.py`).
- Pay ↔ Request end to end (`t_pay_2.py`); Duel (`t_duel_2.py`).
- M1 (CHAL→PROOF latency), which sets `presence_ms`; U7 and U12 (frame loss while a signature blocks the loop).
- Gate 2's "honest payee green".

**Needs the hotspot, the dashboard listener and devnet**

- U5/U6: SNTP sync (`t_clock_net.py`); `time=sntp` within 10 s.
- T-APR1 on chain: `signtest` against `/badge/pending`, transaction confirmed (`t_sign_net.py`). This is Gate 1.
- `vkdev.py provision --env dashboard/.env` (needs `npm run devnet:setup`; `HACK_MINT` is empty today). U9: authority keypair layout.
- Balance: launcher BALANCE row, `wallet.token_account()`, `[bal] fetch` (M5).
- App-store offer and installing screens (`offer`, `installing`): they need a broker, and `DEFAULT_BROKER_URL` is empty. Compiled, not seen.
- Settings → Wi-Fi by hand: scan, join an open network, start the hotspot, disconnect, forget (the scripted tests leave the network state alone). Settings → App push addresses on a real network; the web page's BadgeOS title and Receipt colours in a browser.
- `vk.rpc`, `vk.blockhash`, `vk.send_tx`, `vk.confirm`, `vk.record`, `vk.feed`, `vk.report` against real endpoints. `vk.pay` beyond its failure path.
- Game shop purchase (amber), Evil game "amount" and "recipient" demos, revoked merchant.
- Backend routes `/registry`, `/feed/*`, `/health` do not exist yet (backend owner).
- Store registration through `store-reg` against a broker.
- Bank rail end to end (`/balance`, `/bank/authorize`).
- `push-apps.sh --host` over Wi-Fi.

**Needs a person**

- The `notify` LED pattern (a dim breathe while a note waits and the badge is idle).
- T-LED1, T-LED2: LED boot bar and severity colours. Boot screen appearance (it cannot be captured over serial), and that power-on shows no splash.
- Settings → Identity → New identity, SELECT: it replaces the badge's key and address, so it is never scripted.
- T-CFG4: `VKINFO` over BLE or HTTP is not recognised.
- T-APR6: a release build never signs a red approval with real buttons.
- T-REL1 by hand; legibility at arm's length; visual comparison of screenshots with the simulation.
- BOOT1/RST1 recovery if auto-reset fails.
- T-REQ6's BLE half.
- SE050 fallback (new identity).

**Needs an SE050-keyed badge** (only if the connected badge reports `key=software`)

- T-SE1, T-SE2, U4, M2 for the SE050.

**Needs four badges**

- WP52: release flash, provisioning, funding, attestations, labels; T-REL3 demo script.

## 9. Risks

| # | Risk | Sign | Fallback (from the spec) | Handled by |
|---|---|---|---|---|
| 1 | **In-place build.** Upstream has only been compiled from a path without spaces; `REPO` has one; a full compile takes minutes. | compile fails in Batch 0, or every build is a full rebuild | compile through a symlinked sketch directory without spaces; fixed `--build-path` per profile for incremental builds; the working combination is written into the build guide | agent 0 establishes it; 1A's `build.sh` follows it |
| 2 | **Static registration dropped by the linker** (U2). A feature then silently does nothing. | `[vk] linkcheck alive` missing; a registry count in `[vk] registries:` lower than the brief expects | `src/vk/registry_anchor.cpp` referencing one symbol per registering file (overview §6); each later file then adds one line there | I1 detects and applies; every integrator checks the counts |
| 3 | **Upload or serial control fails** (U10): auto-reset into the bootloader does not work, or opening the port resets the badge mid-test. | `arduino-cli upload` cannot connect; tests lose state between commands | upload: retry once, then record "needs BOOT1/RST1 by hand" and continue compile-only (section 6 step 4). Port: `vkdev.py` opens with DTR/RTS deasserted and keeps one connection per test run | 1D writes it; every integrator follows step 4 |
| 4 | **TweetNaCl is slow** (U3, F7): about a second per operation. A full-ctx `begin_solana` runs two verifications inside one Lua callback; presence depends on a signature. | `[vk] verify` > 400 ms in Batch 3; "app exceeded its time budget" in the log | `runtime::extendDeadline(2500)` per verification (already in the bindings); Monocypher backend (`VK_ED25519_BACKEND 1`); presence shown amber | I3 measures and reports `VERIFY_MS`; 4F vendors; I4 switches and re-measures |
| 5 | **Loop-task stack** (U8): decoding plus verification inside a Lua callback, or TLS plus signing, overflows the 8 KB `loopTask`. | reboot with `Stack canary watchpoint triggered (loopTask)`; M6 under 1 KB | a new hook H22 in `os.ino`: `SET_LOOP_TASK_STACK_SIZE(16 * 1024);  // VK: H22`, added to `upstream-hooks.md`, `UPSTREAM-HOOKS.md` and the expected list | any integrator on first sight; I6 measures M6 |
| 6 | **Eight agents write the shell against a header that does not exist yet** (`src/vk/shell/page.h`; also `screens.h` and `ui/repaint.h`). | compile errors in `src/vk/shell/pages/`, `devtools.cpp` or `vk.cpp` in the first Batch 5 build | the block in [ui/shell.md](../ui/shell.md) "page.h" is the contract: the integrator fixes whichever file departs from it, pages against 5A's real header where the block is silent | 5A creates the header verbatim; I5 |
| 7 | **Deleting `src/ui/shell.cpp` loses a behaviour** upstream's shell had. | a settings action, a button or a safety guard (offer entry tick, install escape, brightness floor) is missing on the badge | upstream's file stays the reference: `git show <last commit before Batch 5>:os/src/ui/shell.cpp`; shell.md lists every upstream call per screen, and the missing one is added to the page that owns it | 5A, 5F, 5G read it before it is deleted; I5 compares |

Checked and low: the fonts exist in the installed LovyanGFX (fallback `Font4` / `Font2` stays documented). Image size has about 1.4 MB of headroom (upstream 1.85 MB in a 3,342,336-byte slot); every integrator records it.

### Critical Files for Implementation

- /Users/ayush/Downloads/My Projects/Hackathons/MHacks2026/docs/os/roadmap/implementation-plan.md
- /Users/ayush/Downloads/My Projects/Hackathons/MHacks2026/docs/os/architecture/upstream-hooks.md
- /Users/ayush/Downloads/My Projects/Hackathons/MHacks2026/docs/os/wallet/approval.md
- /Users/ayush/Downloads/My Projects/Hackathons/MHacks2026/docs/os/testing/testing.md
- /Users/ayush/Downloads/My Projects/Hackathons/MHacks2026/docs/os/ui/shell.md (Batch 5)
- /private/tmp/claude-502/-Users-ayush-Downloads-My-Projects-Hackathons-MHacks2026/41bb931f-51cc-40a5-9e9e-f24006cd3c71/scratchpad/upstream/firmware/solana-os/src/lua_sdk/lua_runtime.cpp