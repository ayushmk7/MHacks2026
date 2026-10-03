# Badge OS documentation

Index of the design documentation for the badge firmware: what each document covers, in what order to read them, and what exists today.

Audience: everyone on the team.

Status: design, not yet built on hardware.

## What this is

Badge OS is three things on one ESP32-S3 badge:

| Part | What it is | Tag |
|---|---|---|
| Solana OS | The upstream application firmware: Lua 5.4.8 runtime, `badge.*` SDK, launcher, settings, Wi-Fi, BLE, ESP-NOW, app push, and the badge's Ed25519 identity. | [UPSTREAM] `firmware/solana-os/` at commit `812b8c7` of the [upstream repository](https://github.com/spacemandev-git/solana-defcon-badge-26/tree/812b8c7aca5c366d18c0b040fafd2999f7204d84/firmware/solana-os) |
| Wallet and identity layer | A wallet core in native firmware (`src/wallet/`): transaction decoder, signing gate with its own approval screens, payment protocol over ESP-NOW, on-chain attestation check, JSON-RPC client, audit log, history. | [OURS] |
| App platform | One host API ("badge API") with the same names in Lua (`badge.x.y`), C (`badge_x_y`) and C++ (`badge::x::y`). Lua apps are pushed at run time and sandboxed. C++ apps are compiled into the image and trusted. | [UPSTREAM] Lua runtime, [OURS] native runtime, permissions and the new modules |

Credit line, as in the [PRD](../specs/Prd-verified-payment-key.md): "Built on Solana OS by spacemandev; we added the wallet and identity layer."

The upstream repository has no licence file and no licence statement in either of its READMEs [UPSTREAM finding, read at `812b8c7`]. Its vendored parts carry their own terms (Lua: MIT; TweetNaCl: public domain). Credit upstream, and ask the author before publishing a fork. The library this design adds, Monocypher 4.0.2, is BSD-2-Clause OR CC0.

The rule the whole design rests on:

> An app can ask for a signature. Only the wallet core can produce one. A transaction signature is produced only after the wallet core's own approval screen and a SELECT press on that screen. Message signatures exist only in three fixed, domain-separated formats built by the wallet core.

Product name: open. These documents say "the OS" or "Badge OS". The dashboard calls itself `badgepay`. Firmware constants: `WALLET_VERSION "0.1.0"`; `SOLANA_OS_API_VERSION` goes from 1 to 2 because bindings are added [OURS].

### Honest status

- **Nothing here has run on a badge.** No badge has been flashed with upstream or with our changes.
- **One observation of hardware exists.** On 2026-10-03 a badge was connected to the design laptop and enumerated as a WCH CH340 USB-serial bridge (USB id `1a86:7523`) at `/dev/cu.usbserial-10`, which matches the upstream description. The serial port was held by another program, so no command was sent to the badge and nothing was read from it.
- **No build command in these documents has been executed.** `arduino-cli` and `esptool` were not installed on the machine where this was designed. Every statement about on-device behaviour comes from reading source, not from running it.
- **There is no firmware source in this repository yet.** The OS is built in a fork of upstream placed at `firmware/solana-os/` in this repository, by following these documents. That directory does not exist until work package WP1 of the [implementation plan](roadmap/implementation-plan.md).
- **What was host-tested** (compiled and run on a laptop, 2026-10-03), all in [`reference/code/`](reference/code/):

| Files | Result |
|---|---|
| `sol.h`, `sol_b58.c`, `sol_curve.c`, `sol_pda.c`, `sol_tx.c`, `sol_sha256.c` with `test_sol.c` | prints `all sol tests passed`: base58, 24 curve-membership vectors, token-account and attestation address derivation, decoding of legacy and v0 messages built by `@solana/kit`, builder output byte-identical to `@solana/kit` for the first vector and field-identical for a second key set where kit orders the accounts differently, 15 negative decoder checks |
| `pay_proto.h`, `pay_proto.c` with `test_pay.c` (needs Monocypher 4.0.2, which is not vendored in this repository) | prints `all pay tests passed`: round trips of all seven frame types, signature verify and tamper, name rules, size budgets, domain separation |
| `attest_parse.c` (declared in `sdk-headers/wallet/attest.h`) with `test_attest.c` | prints `all attest tests passed`: the attestation account parser accepts the 189-byte account built with `sas-lib`'s layout and refuses every mutated copy (wrong discriminator, subject, credential, schema, lengths, name rule); 21 checks |
| `vectors.mjs`, `vectors-to-h.mjs`, `vectors.json`, `vectors.h` | vectors generated with the dashboard's own `@solana/kit`, `sas-lib` and `@solana-program/token`; regenerating reproduces the committed files byte for byte |
| `sdk-headers/`: `app_host/badge_api.h`; `wallet/` (`wallet.h`, `pay_session.h`, `attest.h`, `wallet_crypto.h`, `wallet_internal.h`, `audit.h`, `history.h`, and copies of `sol.h` and `pay_proto.h`); `identity/identity_private.h`; `sdk/badge_sdk.hpp`; `native_apps/registry.cpp`, `native_apps/tipjar/tipjar.cpp` | syntax-checked only (`cc -std=c99 -fsyntax-only` for the C headers, `c++ -std=c++17 -fno-exceptions -fno-rtti -fsyntax-only` for all); never linked, never run |

To repeat the first test from `docs/os/reference/code/`:

```bash
cc -std=c99 -Wall -Wextra -Wpedantic -O2 -DSOL_HOST_SHA256 \
   sol_b58.c sol_curve.c sol_pda.c sol_tx.c sol_sha256.c test_sol.c -o /tmp/test_sol && /tmp/test_sol
# all sol tests passed
```

The other two suites, the header syntax check, the CMake project and vector regeneration are in [testing/host-tests.md](testing/host-tests.md).

## Status legend

Every factual claim about behaviour carries one of these tags.

| Tag | Meaning |
|---|---|
| **[UPSTREAM]** | Exists in Solana OS at commit `812b8c7`, verified by reading source. The upstream path is given, relative to `firmware/solana-os/`. |
| **[OURS]** | Our design decision, with one line of why. "host-tested" next to it means the reference implementation in [`reference/code/`](reference/code/) passes host tests against vectors from `@solana/kit` and `sas-lib`. |
| **[UNVERIFIED]** | Must be measured or confirmed on hardware. A fallback is given next to it. |

Other conventions used in every document:

- Buttons are named by their silkscreen: SELECT, CANCEL, UP, DOWN, LEFT, RIGHT. The Lua key names `"a"` (SELECT) and `"b"` (CANCEL) appear only in code.
- Example amounts: 10.00 HACK (raw `1000`), attack 500.00 HACK (raw `50000`), cap 100.00, max 1000.00. HACK has 2 decimals.
- Example keys are the stand-in keys from `dashboard/server/config/badges.json`: merchant "MHacks Merch" `Gn2G..Ecxq`, impostor `DhEb..W511` (device name `badge-51A0`), Judge A (the payer) `4vJD..BW97`, Judge B `FdBS..17XD`. The attacker in the compromised-laptop demo is the dashboard's registry authority, `FZEA..ei3K`.
- Firmware paths are relative to `firmware/solana-os/` in the fork.
- `TODO(spec):` marks a detail the design does not fix yet. It is never filled with a guess. None are open as of 2026-10-03.
- Each document ends with "Requirements covered" (F-ids) and "Open items" (its [UNVERIFIED] entries).

## Reading order by role

### Firmware engineer

1. [architecture/overview.md](architecture/overview.md): layers, trust zones, the rule, decisions.
2. [architecture/upstream-baseline.md](architecture/upstream-baseline.md): what Solana OS provides and where it differs from what the PRD assumed.
3. [architecture/runtime-and-boot.md](architecture/runtime-and-boot.md): tasks, memory, boot, main loop, patches P1–P14.
4. [guides/build-and-flash.md](guides/build-and-flash.md): toolchain, build upstream first, then the fork.
5. [wallet-core/api.md](wallet-core/api.md), then [wallet-core/signing-gate.md](wallet-core/signing-gate.md), [wallet-core/screens.md](wallet-core/screens.md), [wallet-core/transaction-decoder.md](wallet-core/transaction-decoder.md).
6. [protocol/payment-protocol.md](protocol/payment-protocol.md), [protocol/transaction-building.md](protocol/transaction-building.md), [identity/attestation.md](identity/attestation.md).
7. [wallet-core/keys-and-se050.md](wallet-core/keys-and-se050.md), [wallet-core/config-limits-audit.md](wallet-core/config-limits-audit.md).
8. [testing/host-tests.md](testing/host-tests.md), [testing/acceptance.md](testing/acceptance.md), [testing/measurements.md](testing/measurements.md).
9. [roadmap/implementation-plan.md](roadmap/implementation-plan.md): the order of work.

### Lua app author

1. [app-platform/overview.md](app-platform/overview.md): manifest, permissions, lifecycle, limits.
2. [app-platform/lua-apps.md](app-platform/lua-apps.md): write, push and debug a Lua app.
3. [app-platform/api-reference.md](app-platform/api-reference.md): every function. Start with [`badge.identity`](app-platform/api-reference.md#badgeidentity) and [`badge.pay`](app-platform/api-reference.md#badgepay).
4. [app-platform/examples.md](app-platform/examples.md): Tip Jar, line by line.
5. [reference/error-codes.md](reference/error-codes.md#badge_err_t): the error strings an app receives.
6. One shipped app as a model: [apps/pay.md](apps/pay.md).
7. [wallet-core/screens.md](wallet-core/screens.md): what the user sees when your app calls `identity.sign`.

### C++ app author

1. [app-platform/overview.md](app-platform/overview.md): the two runtimes compared.
2. [app-platform/cpp-apps.md](app-platform/cpp-apps.md): delivery model, trust statement, rules, SDK, registry.
3. [app-platform/api-reference.md](app-platform/api-reference.md): the C ABI and the parity exceptions.
4. [app-platform/examples.md](app-platform/examples.md): Tip Jar in C++.
5. [`reference/code/sdk-headers/`](reference/code/sdk-headers/): `app_host/badge_api.h`, `sdk/badge_sdk.hpp`, `native_apps/registry.cpp`, `native_apps/tipjar/tipjar.cpp` as syntax-checked listings.
6. [security/security-model.md](security/security-model.md): why native code is trusted and what that means.
7. [guides/build-and-flash.md](guides/build-and-flash.md): a C++ app ships by reflashing.

### Demo operator

1. [guides/pre-event-checklist.md](guides/pre-event-checklist.md): what must be true before judges arrive.
2. [guides/build-and-flash.md](guides/build-and-flash.md#flashing-four-badges): flashing four badges.
3. [guides/configure.md](guides/configure.md): Wi-Fi, wallet config, apps, CA pin.
4. [integration/dashboard.md](integration/dashboard.md): what the badge and the laptop panel must agree on.
5. [testing/attack-scripts.md](testing/attack-scripts.md): the four attacks, step by step, with the seeding step and the demo order they depend on.
6. [wallet-core/screens.md](wallet-core/screens.md): the exact screens a judge sees.
7. [guides/troubleshooting.md](guides/troubleshooting.md): symptoms to fixes.
8. [Dashboard runbook](../dashboard/RUNBOOK.md): the laptop side of the demo.

## Map of folders

39 documents: this index and 38 documents in twelve folders. The reference code under `reference/code/` is not counted.

### `architecture/`

| Document | Contents |
|---|---|
| [overview.md](architecture/overview.md) | Layer diagram, trust zones, the signing rule, decisions D1–D12 with reasons, PRD open questions resolved. |
| [upstream-baseline.md](architecture/upstream-baseline.md) | What Solana OS provides: hardware, firmware facts, source tree, Lua runtime and sandbox, identity, networking, UI, storage, findings that differ from the PRD, upstream issues. |
| [runtime-and-boot.md](architecture/runtime-and-boot.md) | Task model, memory budget, boot sequence, main loop, patch list P1–P14, source tree after our changes. |

### `wallet-core/`

| Document | Contents |
|---|---|
| [api.md](wallet-core/api.md) | The C API: `wallet.h`, `sol.h`, `pay_proto.h` in full, enums, call rules. |
| [signing-gate.md](wallet-core/signing-gate.md) | How a signature happens: state machine, policy checks, severity and gestures, the approval modal, message signing and domain separation, the key gate, rate limits. |
| [screens.md](wallet-core/screens.md) | Exact screens A–H: pixel table, mockups, text strings, LED colours, blocked headlines. |
| [transaction-decoder.md](wallet-core/transaction-decoder.md) | Message format (legacy, v0), annotated 214-byte vector, rule table, why extra instructions are refused, `sol_tx.c` in full. |
| [keys-and-se050.md](wallet-core/keys-and-se050.md) | Where the key lives, honesty rules, the SE050 path and its bring-up, fallback to a software key, the Monocypher backend. |
| [config-limits-audit.md](wallet-core/config-limits-audit.md) | Config keys, how to set them, cap and max, audit log format, history store format. |

### `security/`

| Document | Contents |
|---|---|
| [security-model.md](security/security-model.md) | Assets, adversaries, guarantees with their mechanisms and tests, non-goals and residual risks, what to say when asked. |

### `app-platform/`

| Document | Contents |
|---|---|
| [overview.md](app-platform/overview.md) | The two runtimes compared, manifest, permissions, lifecycle, blocking rules, storage and isolation, limits. |
| [lua-apps.md](app-platform/lua-apps.md) | Smallest app, `app.ini` with permissions, sandbox limits, the 32-bit number rule and string amounts, pushing with `badge-push.py`, logs, common errors. |
| [cpp-apps.md](app-platform/cpp-apps.md) | Delivery model, trust statement, rules, `badge_sdk.hpp`, `BADGE_APP`, registry, build, budgets, memory. |
| [api-reference.md](app-platform/api-reference.md) | Every function of the badge API, upstream modules and ours, Lua and C forms, `badge_api.h` in full. |
| [examples.md](app-platform/examples.md) | Tip Jar in Lua and in C++, mapped line by line. |

### `apps/`

| Document | Contents |
|---|---|
| [home.md](apps/home.md) | Home (F4): balances, name, short address, menu, checkout polling. |
| [pay.md](apps/pay.md) | Pay (F5, F7): pick a request or a nearby badge, choose the amount, sign, submit, confirm. |
| [request.md](apps/request.md) | Request (F6, F7): ask for a payment, answer presence checks, confirm on chain. |
| [history.md](apps/history.md) | History (F16): the list of signed payments. |
| [checkout.md](apps/checkout.md) | Checkout (badge side of F12): the merchant-app stand-in that the compromised laptop lies to. |
| [settings.md](apps/settings.md) | Settings → Identity and Settings → Wallet (shell screens). |
| [receipts.md](apps/receipts.md) | Co-signed receipts (F19, stretch). |

### `protocol/`

| Document | Contents |
|---|---|
| [payment-protocol.md](protocol/payment-protocol.md) | REQ, CHAL, PROOF, HELLO, IAM, PAID, RCPT: byte tables, signed bytes, both state machines, timing, rules, sequences, abort paths, `pay_session.h` and the codec in full. |
| [transaction-building.md](protocol/transaction-building.md) | From request to confirmed: who builds what, inputs, token-account derivation, bytes, RPC bodies, error mapping, confirmation and LEDs. |

### `identity/`

| Document | Contents |
|---|---|
| [attestation.md](identity/attestation.md) | The Solana Attestation Service check: address derivation, fetch, account layout and parser (`attest_parse.c`), decision order, cache, known names and the seeding step, transport trust, clock, `attest.h` in full. |

### `integration/`

| Document | Contents |
|---|---|
| [dashboard.md](integration/dashboard.md) | Badge and dashboard: how each `BADGE-GAP` closes, export commands, checkout protocol, demo walk-through, values that must match. |

### `guides/`

| Document | Contents |
|---|---|
| [build-and-flash.md](guides/build-and-flash.md) | Toolchain, building upstream first, the fork, compile-time switches, pre-flash checks, flashing four badges, resetting identity. |
| [configure.md](guides/configure.md) | Wi-Fi, wallet config with `curl`, apps, CA pin, verifying on Settings → Wallet. |
| [pre-event-checklist.md](guides/pre-event-checklist.md) | The PRD checklist as commands with pass conditions and tick boxes, plus four added checks, among them seeding known names on each judge badge. |
| [troubleshooting.md](guides/troubleshooting.md) | Log tags, symptoms to fixes, SE050 status words, upstream troubleshooting pointers. |

### `testing/`

| Document | Contents |
|---|---|
| [host-tests.md](testing/host-tests.md) | Running the three host suites without hardware: commands, layout, CMake, coverage, vectors and how to regenerate them. |
| [acceptance.md](testing/acceptance.md) | On-device checks per requirement, ids `T-*`. |
| [attack-scripts.md](testing/attack-scripts.md) | The four attack flows as scripts, the seeding step and demo order, the replay app, reset between runs. |
| [measurements.md](testing/measurements.md) | The numbers to measure on hardware, ids M1–M7, with log formats and empty result tables. |

### `roadmap/`

| Document | Contents |
|---|---|
| [implementation-plan.md](roadmap/implementation-plan.md) | Work packages WP0–WP13 with dependencies and "done when", the critical path, the hour plan with cut-offs and the freeze, patches by work package, the fallback ladder. |

### `reference/`

| Document | Contents |
|---|---|
| [error-codes.md](reference/error-codes.md) | `badge_err_t`, decoder error names, where each is shown. |
| [traceability.md](reference/traceability.md) | Each requirement to its document, acceptance test and work package; measurements and patches by work package. |
| [glossary.md](reference/glossary.md) | Terms. |
| [`code/`](reference/code/) | Host-tested reference C (`sol_*.c`, `pay_proto.*`, `attest_parse.c`); three host suites (`test_sol.c`, `test_pay.c`, `test_attest.c`); the vector generator, its converter and the generated vectors; and `sdk-headers/`, the syntax-checked header and example listings for the wallet core (`wallet/`), the key gate (`identity/`), the badge API (`app_host/`), the C++ SDK (`sdk/`) and native apps (`native_apps/`). Becomes `src/` and `test/host/` in the fork. |

### Outside this folder

| Path | Contents |
|---|---|
| [PRD](../specs/Prd-verified-payment-key.md) | Requirements F1–F19, non-functional requirements, demo plan, risks. |
| [`../dashboard/`](../dashboard/RUNBOOK.md) | Documentation of the laptop panel: [API.md](../dashboard/API.md), [ARCHITECTURE.md](../dashboard/ARCHITECTURE.md), [BADGE-GAPS.md](../dashboard/BADGE-GAPS.md), [RUNBOOK.md](../dashboard/RUNBOOK.md), [TIGER-DATA.md](../dashboard/TIGER-DATA.md). |
| `dashboard/` (repository root) | The panel itself. Run its npm scripts from inside `dashboard/`. |

## Requirements F1–F19

Priority and layer are the PRD's. "Layer" there is where the PRD expected the work; where the design moved it, the linked document says so. Section-level links and work packages are in [reference/traceability.md](reference/traceability.md).

| ID | Requirement (short) | Priority | Layer (PRD) | Primary document | Acceptance |
|---|---|---|---|---|---|
| F1 | `identity.pubkey()` and `identity.sign(tx)` in Lua; signing unreachable without the approval screen | P0 | Firmware | [app-platform/api-reference.md](app-platform/api-reference.md#badgeidentity) | [T-F1](testing/acceptance.md#t-f1), [T-F1b](testing/acceptance.md#t-f1b) |
| F2 | Firmware approval screen; SELECT signs, CANCEL rejects; Lua cannot draw over or skip it | P0 | Firmware | [wallet-core/signing-gate.md](wallet-core/signing-gate.md#approval-modal) | [T-F2](testing/acceptance.md#t-f2), [T-F2b](testing/acceptance.md#t-f2b), [T-F2c](testing/acceptance.md#t-f2c) |
| F3 | Decoder for SPL Token `transferChecked`; anything else is "Unknown instruction" and blocked | P0 | Firmware | [wallet-core/transaction-decoder.md](wallet-core/transaction-decoder.md) | [T-F3](testing/acceptance.md#t-f3) |
| F4 | Home app: HACK and SOL balance, badge name, short address | P0 | Lua + RPC | [apps/home.md](apps/home.md) | [T-F4](testing/acceptance.md#t-f4) |
| F5 | Pay app: pick a nearby badge (strongest signal first), choose the amount with the D-pad | P0 | Lua | [apps/pay.md](apps/pay.md) | [T-F5](testing/acceptance.md#t-f5) |
| F6 | Request app: broadcast "pay me X HACK" to the nearest badge | P0 | Lua | [apps/request.md](apps/request.md) | [T-F6](testing/acceptance.md#t-f6) |
| F7 | Submit to devnet, poll confirmation, flash LEDs on both badges | P0 | Lua + RPC | [protocol/transaction-building.md](protocol/transaction-building.md) | [T-F7](testing/acceptance.md#t-f7) |
| F8 | Payment requests signed by the payee badge key | P1 | Lua + firmware | [protocol/payment-protocol.md](protocol/payment-protocol.md#messages) | [T-F8](testing/acceptance.md#t-f8) |
| F9 | Nonce handshake proving the payee is present | P1 | Lua + firmware | [protocol/payment-protocol.md](protocol/payment-protocol.md#timing) | [T-F9](testing/acceptance.md#t-f9) |
| F10 | Attestation check: valid, unrevoked "MHacks Verified" attestation; result cached | P1 | Lua + RPC | [identity/attestation.md](identity/attestation.md#decision) | [T-F10](testing/acceptance.md#t-f10) |
| F11 | Approval screen shows verified, unverified (amber) or mismatch (red) | P1 | Firmware | [wallet-core/screens.md](wallet-core/screens.md#screen-a) | [T-F11](testing/acceptance.md#t-f11) |
| F12 | Compromised-laptop tool that builds a tampered transaction (badge side: Checkout) | P2 | Laptop | [integration/dashboard.md](integration/dashboard.md) | attack flow 3 in [testing/attack-scripts.md](testing/attack-scripts.md) |
| F13 | Dashboard: live feed, explorer links, attestation status, issue and revoke | P2 | Web | [dashboard docs](../dashboard/ARCHITECTURE.md) (already built) | [dashboard runbook](../dashboard/RUNBOOK.md) |
| F14 | Spending cap: payments above a limit need a second confirmation | P2 | Firmware | [wallet-core/config-limits-audit.md](wallet-core/config-limits-audit.md) | [T-F14](testing/acceptance.md#t-f14) |
| F15 | Revocation reflected on badges within one refresh | P2 | On-chain + Lua | [identity/attestation.md](identity/attestation.md#cache) | [T-F15](testing/acceptance.md#t-f15) |
| F16 | Payment history on the badge | P2 | Lua | [apps/history.md](apps/history.md) | [T-F16](testing/acceptance.md#t-f16) |
| F17 | Key held in the SE050, verified on silicon; upstream PR | P3 | Firmware | [wallet-core/keys-and-se050.md](wallet-core/keys-and-se050.md) | [T-SE1](testing/acceptance.md#t-se1) |
| F18 | Voice readout of the approval screen | P3 | Web | out of scope for the OS (web) | none |
| F19 | Co-signed receipts | P3 | Lua + firmware | [apps/receipts.md](apps/receipts.md) | [T-F19](testing/acceptance.md#t-f19) |

## What is not built yet

Everything on the badge. In detail:

| Item | State |
|---|---|
| Fork at `firmware/solana-os/` in this repository | does not exist; created in WP1 |
| Upstream patches P1–P14 | specified in [architecture/runtime-and-boot.md](architecture/runtime-and-boot.md); not applied |
| `sol_*.c`, `pay_proto.*`, `attest_parse.c` | written and host-tested ([`reference/code/`](reference/code/)); not yet compiled with `arduino-cli` |
| `wallet.cpp`, `wallet_ui.cpp`, `wallet_config.cpp`, `wallet_crypto.cpp`, `pay_session.cpp`, `attest.cpp`, `rpc.cpp`, `audit.cpp`, `history.cpp` | specified, not written; their headers are syntax-checked listings |
| Native runtime (`app_host`, `native_runtime`), `badge_api.cpp`, Lua bindings (`lib_wallet.cpp`) | specified, not written; the headers are syntax-checked only |
| Lua apps `home`, `pay`, `request`, `history`, `checkout`, `tipjar`; C++ `tipjar` | specified, not written (`tipjar.cpp` is a syntax-checked listing) |
| SE050 key path (F17) | never run on a real SE050 by upstream or by us [UPSTREAM `src/hal/se050_apdu.cpp:1-4`] |
| Every latency, memory and timing number | unmeasured; empty tables in [testing/measurements.md](testing/measurements.md) |
| On-device acceptance tests | none run; procedures in [testing/acceptance.md](testing/acceptance.md) |
| F13 dashboard | built and documented separately in [`../dashboard/`](../dashboard/RUNBOOK.md); its on-chain paths have their own "not run" list there |
| F18 voice readout | out of scope for the OS |

## Requirements covered

This index covers no requirement by itself. It lists F1–F19 and links each to its primary document.

## Open items

The full register of [UNVERIFIED] items. Each is repeated in the document that depends on it.

| # | Item | Fallback, or how it gets resolved |
|---|---|---|
| U1 | Nothing has run on a badge; `arduino-cli` is not installed on the development machine. One badge has been seen to enumerate over USB, nothing more | WP0 first ([roadmap](roadmap/implementation-plan.md)) |
| U2 | Ed25519 time on the ESP32-S3: TweetNaCl (about 1 s per upstream), Monocypher (30 to 37 times faster on the host), SE050 (about 261 ms cited) | measure [M2](testing/measurements.md#m2); choose backend and `deadline_ms` |
| U3 | PROOF deadline 400 ms; PRD target 250 ms | measure [M1](testing/measurements.md#m1); 800 ms with the SE050; 2500 ms with TweetNaCl |
| U4 | SE050C2: Ed25519 support, no SCP03, byte order, T=1 chaining for a ~230-byte APDU, the 242-byte limit | [T-SE1](testing/acceptance.md#t-se1); else software key, and say so |
| U5 | Loop-task stack need (set to 16 KB) and heap headroom with TLS | measure [M6](testing/measurements.md#m6); raise to 24 KB |
| U6 | HTTPS RPC latency through a hotspot; connection reuse | measure [M5](testing/measurements.md#m5); lengthen polling; second RPC URL |
| U7 | "Request to approval under 2 s" and "SELECT to dashboard under 5 s" | measure [M3](testing/measurements.md#m3), [M4](testing/measurements.md#m4); report honestly if missed |
| U8 | RSSI threshold `rssi_min = -75` | tune at the table; set to -100 to disable |
| U9 | The hotspot lets badge and laptop reach each other; SNTP works through it | [pre-event check](guides/pre-event-checklist.md); without SNTP expiry is not evaluated; without reachability use "Mark rejected" on the dashboard |
| U10 | Arduino core flags (`-std`, exceptions, RTTI); `sodium.h` availability | the SDK rules avoid depending on them; Monocypher is vendored |
| U11 | Which CA root to pin for the RPC host | fetch on the day; unpinned is shown on screen |
| U12 | Devnet rate limits with four badges polling | Home polls one call per 3 s; back off on HTTP 429 |
| U13 | PDA derivation time on the ESP32-S3 | measure [M7](testing/measurements.md#m7); cache token accounts per payee |
| U14 | Flash size increase (about 120 KB estimated) | 1.4 MB headroom in the 3.2 MB slot |
| U15 | Upstream repository licence | credit upstream; ask the author before publishing a fork |
| U16 | Current Solana base fee (5000 lamports per signature) | irrelevant to correctness; badges are funded with 0.05 SOL |
| U17 | The reference C compiles unchanged under the Arduino core | it is plain C99; fix warnings as they appear |
| U18 | Whether a pinned TLS handshake checks certificate validity dates while the badge clock is unset | wait up to 5 s for SNTP before the first pinned request; if it still fails, remove `rpc-ca` and run unpinned (shown on screen) |
| U19 | Exact wording of the node's preflight error for an expired blockhash | match `lockhash`; otherwise the generic line `Send failed, nothing moved` |
| U20 | `__has_include("local_config.h")` under the installed core's compiler | commit an empty `local_config.h` and drop the `#if` |
| U21 | Signal-bar thresholds (-50/-60/-70/-80/-90 dBm) at table distance | show the RSSI number instead |
| U22 | LED colours distinguishable at brightness 72 | raise LED brightness in the modal only |
| U23 | SNTP sync callback availability in the Arduino core (`sntp_set_time_sync_notification_cb`) | fall back to the time threshold alone and note that the WPA2-Enterprise path can then read "synced" |
