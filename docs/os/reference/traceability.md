# Traceability

Where each PRD requirement is specified, how it is checked, and which work package builds it.

Audience: everyone.

Status: design, not yet built on hardware.

Every row points at a design document, not at running code: nothing has been built or run on a badge. Requirement ids, priorities and layers are those of the [PRD](../../specs/Prd-verified-payment-key.md). Patch ids (P1–P14) are defined in [../architecture/runtime-and-boot.md](../architecture/runtime-and-boot.md#upstream-patches). Test ids (`T-*`) are defined in [../testing/acceptance.md](../testing/acceptance.md), attack flows in [../testing/attack-scripts.md](../testing/attack-scripts.md), measurement ids (M1–M7) in [../testing/measurements.md](../testing/measurements.md), work packages (WP0–WP13) in [../roadmap/implementation-plan.md](../roadmap/implementation-plan.md). Upstream is `firmware/solana-os/` at commit `812b8c7` of the [upstream repository](https://github.com/spacemandev-git/solana-defcon-badge-26/tree/812b8c7aca5c366d18c0b040fafd2999f7204d84/firmware/solana-os).

## Functional requirements

| F | Requirement (short) | Priority | Primary document and section | Also | Acceptance | Work package |
|---|---|---|---|---|---|---|
| F1 | `identity.pubkey()`, `identity.sign(tx)` | P0 | [api-reference.md, `badge.identity`](../app-platform/api-reference.md#badgeidentity) | [signing-gate.md, Key gate](../wallet-core/signing-gate.md#key-gate) | [T-F1](../testing/acceptance.md#t-f1), [T-F1b](../testing/acceptance.md#t-f1b) | [WP3](../roadmap/implementation-plan.md#wp3), including T-F1b: patch P12 (manifest permissions) is applied there |
| F2 | firmware approval screen | P0 | [signing-gate.md, Approval modal](../wallet-core/signing-gate.md#approval-modal) | [signing-gate.md, State machine](../wallet-core/signing-gate.md#state-machine); [screens.md, Screen A](../wallet-core/screens.md#screen-a) | [T-F2](../testing/acceptance.md#t-f2), [T-F2b](../testing/acceptance.md#t-f2b), [T-F2c](../testing/acceptance.md#t-f2c) | [WP3](../roadmap/implementation-plan.md#wp3) |
| F3 | `transferChecked` decoder, unknown blocked | P0 | [transaction-decoder.md](../wallet-core/transaction-decoder.md) | [screens.md, Screen E](../wallet-core/screens.md#screen-e); [signing-gate.md, Policy checks](../wallet-core/signing-gate.md#policy-checks) | [T-F3](../testing/acceptance.md#t-f3); host suite `test_sol` ([host-tests.md](../testing/host-tests.md)) | [WP2](../roadmap/implementation-plan.md#wp2) (decoder), [WP3](../roadmap/implementation-plan.md#wp3) (gate and screen) |
| F4 | Home | P0 | [apps/home.md](../apps/home.md) | [transaction-building.md](../protocol/transaction-building.md) | [T-F4](../testing/acceptance.md#t-f4) | [WP4](../roadmap/implementation-plan.md#wp4), [WP5](../roadmap/implementation-plan.md#wp5) |
| F5 | Pay | P0 | [apps/pay.md](../apps/pay.md) | [payment-protocol.md, Payer state machine](../protocol/payment-protocol.md#payer-state-machine) | [T-F5](../testing/acceptance.md#t-f5) | [WP6](../roadmap/implementation-plan.md#wp6) |
| F6 | Request | P0 | [apps/request.md](../apps/request.md) | [payment-protocol.md, Payee state machine](../protocol/payment-protocol.md#payee-state-machine); [screens.md, Screen F](../wallet-core/screens.md#screen-f) | [T-F6](../testing/acceptance.md#t-f6) | [WP6](../roadmap/implementation-plan.md#wp6) |
| F7 | submit, confirm, LEDs | P0 | [transaction-building.md](../protocol/transaction-building.md) | [apps/pay.md](../apps/pay.md), [apps/request.md](../apps/request.md) | [T-F7](../testing/acceptance.md#t-f7) | [WP6](../roadmap/implementation-plan.md#wp6) |
| F8 | signed requests | P1 | [payment-protocol.md, Messages](../protocol/payment-protocol.md#messages) | [signing-gate.md, Message signing](../wallet-core/signing-gate.md#message-signing); [payment-protocol.md, Rules](../protocol/payment-protocol.md#rules) | [T-F8](../testing/acceptance.md#t-f8) | [WP6](../roadmap/implementation-plan.md#wp6) (signing), [WP7](../roadmap/implementation-plan.md#wp7) (verification) |
| F9 | nonce handshake | P1 | [payment-protocol.md, Timing](../protocol/payment-protocol.md#timing) | [measurements.md, M1](../testing/measurements.md#m1); [payment-protocol.md, Sequences](../protocol/payment-protocol.md#sequences); [screens.md, Screen G](../wallet-core/screens.md#screen-g) | [T-F9](../testing/acceptance.md#t-f9); attack flow 2 | [WP8](../roadmap/implementation-plan.md#wp8) |
| F10 | attestation check | P1 | [attestation.md, Decision](../identity/attestation.md#decision) | [attestation.md, Derivation](../identity/attestation.md#derivation), [Account layout](../identity/attestation.md#account-layout), [Cache](../identity/attestation.md#cache) | [T-F10](../testing/acceptance.md#t-f10); host suites `test_sol` (address) and `test_attest` (account parser) | [WP9](../roadmap/implementation-plan.md#wp9) |
| F11 | verified / unverified / mismatch on screen | P1 | [screens.md, Screen A](../wallet-core/screens.md#screen-a), [Screen B](../wallet-core/screens.md#screen-b), [Screen C](../wallet-core/screens.md#screen-c) | [signing-gate.md, Severity and gestures](../wallet-core/signing-gate.md#severity-and-gestures) | [T-F11](../testing/acceptance.md#t-f11); attack flows 1 and 1b | [WP9](../roadmap/implementation-plan.md#wp9) |
| F12 | compromised-laptop tool (badge side) | P2 | [integration/dashboard.md](../integration/dashboard.md) | [apps/checkout.md](../apps/checkout.md), [attack-scripts.md](../testing/attack-scripts.md) | attack flow 3 | [WP10](../roadmap/implementation-plan.md#wp10) |
| F13 | dashboard | P2 | [`docs/dashboard/`](../../dashboard/ARCHITECTURE.md) (exists) | [integration/dashboard.md](../integration/dashboard.md) | [dashboard runbook](../../dashboard/RUNBOOK.md) | none: already built |
| F14 | spending cap | P2 | [config-limits-audit.md](../wallet-core/config-limits-audit.md) | [screens.md, Screen D](../wallet-core/screens.md#screen-d) | [T-F14](../testing/acceptance.md#t-f14) (driven by a 150.00 request from the `bigreq` app, because the pickers stop at `cap`) | [WP11](../roadmap/implementation-plan.md#wp11) |
| F15 | revocation within one refresh | P2 | [attestation.md, Cache](../identity/attestation.md#cache) | [attestation.md, Known names](../identity/attestation.md#known-names); [attack-scripts.md](../testing/attack-scripts.md); the seeding step in [pre-event-checklist.md](../guides/pre-event-checklist.md#10-seed-known-names-on-each-judge-badge) | [T-F15](../testing/acceptance.md#t-f15); attack flow 4 | [WP11](../roadmap/implementation-plan.md#wp11) |
| F16 | history | P2 | [apps/history.md](../apps/history.md) | [config-limits-audit.md](../wallet-core/config-limits-audit.md) | [T-F16](../testing/acceptance.md#t-f16) | [WP11](../roadmap/implementation-plan.md#wp11) |
| F17 | SE050 | P3 | [keys-and-se050.md](../wallet-core/keys-and-se050.md) | [troubleshooting.md, SE050 status words](../guides/troubleshooting.md#se050-status-words) | [T-SE1](../testing/acceptance.md#t-se1) | [WP13](../roadmap/implementation-plan.md#wp13) |
| F18 | voice readout | P3 | out of scope for the OS (web) | none | none | none |
| F19 | co-signed receipts | P3 | [apps/receipts.md](../apps/receipts.md) | [payment-protocol.md, Messages](../protocol/payment-protocol.md#messages) | [T-F19](../testing/acceptance.md#t-f19) | [WP13](../roadmap/implementation-plan.md#wp13) |

## Non-functional requirements

Summary row, then one row per area of the PRD's non-functional table.

| NFR | Requirement (PRD) | Primary document and section | Also | Check | Work package |
|---|---|---|---|---|---|
| All | latency, honesty, sandbox fit, judge UX | [measurements.md](../testing/measurements.md) | [security-model.md](../security/security-model.md), [signing-gate.md](../wallet-core/signing-gate.md) | see rows below | see rows below |
| Latency | request to approval screen under 2 s; SELECT to confirmed on dashboard under 5 s | [measurements.md, M3](../testing/measurements.md#m3), [M4](../testing/measurements.md#m4) | [payment-protocol.md, Timing](../protocol/payment-protocol.md#timing); [M1](../testing/measurements.md#m1), [M5](../testing/measurements.md#m5) | M3, M4 | flow built in [WP6](../roadmap/implementation-plan.md#wp6) to [WP9](../roadmap/implementation-plan.md#wp9); M3 and M4 are recorded in [WP10](../roadmap/implementation-plan.md#wp10), the first point at which the whole flow exists; M1 in WP8, M5 in WP4 |
| Security | no signing path outside the firmware approval screen; keys never exported; unknown instructions blocked | [security-model.md](../security/security-model.md) | [signing-gate.md, Key gate](../wallet-core/signing-gate.md#key-gate), [Message signing](../wallet-core/signing-gate.md#message-signing); [transaction-decoder.md](../wallet-core/transaction-decoder.md) | T-F1, T-F2, T-F3; the `grep` rules in [build-and-flash.md](../guides/build-and-flash.md#pre-flash-checks); host tests | [WP3](../roadmap/implementation-plan.md#wp3) |
| Sandbox fit | signing extends the Lua watchdog deadline (an SE050 signature takes about 261 ms against a 250 ms budget) | [signing-gate.md, Approval modal](../wallet-core/signing-gate.md#approval-modal) (the deadline is paused for the whole call, patch P3) | [runtime-and-boot.md](../architecture/runtime-and-boot.md) | no test id of its own; T-F1 passing shows the app survives a prompt longer than its budget; [M2](../testing/measurements.md#m2), [M6](../testing/measurements.md#m6) | [WP1](../roadmap/implementation-plan.md#wp1) (P3), [WP3](../roadmap/implementation-plan.md#wp3); M2 in WP7, M6 in WP10 |
| Reliability | one phone hotspot, Wi-Fi only; every badge pre-funded | [pre-event-checklist.md](../guides/pre-event-checklist.md) | [configure.md](../guides/configure.md) | the checklist's pass conditions | [WP0](../roadmap/implementation-plan.md#wp0) (hotspot, radar); funding is a dashboard script |
| Judge UX | every screen names the silkscreen button; any flow abandons with CANCEL; amounts capped | [screens.md](../wallet-core/screens.md) | [config-limits-audit.md](../wallet-core/config-limits-audit.md); the app documents under [`../apps/`](../apps/home.md) | [T-NFR-cancel](../testing/acceptance.md#t-nfr-cancel) (release gate), [T-F5](../testing/acceptance.md#t-f5), [T-F14](../testing/acceptance.md#t-f14) | every package that adds a screen; T-NFR-cancel is run at the freeze (hour 22) |
| Honesty | Settings shows whether the key lives in the secure element or in software | [keys-and-se050.md](../wallet-core/keys-and-se050.md) | [apps/settings.md](../apps/settings.md) | [T-NFR-honesty](../testing/acceptance.md#t-nfr-honesty), which compares four places | Settings → Identity is upstream; the added `/api/identity` fields are patch P11 in [WP4](../roadmap/implementation-plan.md#wp4); Settings → Wallet, whose first row is the key location, is [WP11](../roadmap/implementation-plan.md#wp11), where the test is run |

## PRD goals

| Goal | Where it is met | Shown by |
|---|---|---|
| 1. A judge pays a verified payee end to end on devnet with no help beyond on-screen hints | [apps/pay.md](../apps/pay.md), [apps/request.md](../apps/request.md), [payment-protocol.md, Sequences](../protocol/payment-protocol.md#sequences) | T-F7; demo step 1 |
| 2. The badge refuses to present an impostor as verified, even when it claims the same name | [screens.md, Screen C](../wallet-core/screens.md#screen-c); [attestation.md, Decision](../identity/attestation.md#decision) | attack flow 1; T-F11 |
| 3. The badge shows the true amount and recipient when a compromised app tampers with a transaction | [transaction-decoder.md](../wallet-core/transaction-decoder.md); [signing-gate.md, Policy checks](../wallet-core/signing-gate.md#policy-checks) | attack flow 3; T-F11c |
| 4. Every signature requires a physical button press, enforced below the Lua app layer | [signing-gate.md, Key gate](../wallet-core/signing-gate.md#key-gate), [Message signing](../wallet-core/signing-gate.md#message-signing) | T-F1, T-F2; the `grep` rules |
| 5. Solana is load-bearing: token transfers, the identity registry and revocation all live on-chain | [transaction-building.md](../protocol/transaction-building.md); [attestation.md](../identity/attestation.md) | T-F7, T-F10, T-F15 |

Goal 4 is refined by the design, and the refinement is stated openly: every transaction signature has its own SELECT press on the approval screen; every message signature (request, proof, receipt) traces to one press that named what it authorises. See [Message signing](../wallet-core/signing-gate.md#message-signing) and [security-model.md](../security/security-model.md).

## Tests not tied to an F-id

| Test | Checks | Document | Work package |
|---|---|---|---|
| [T-F11c](../testing/acceptance.md#t-f11c) | guarantee G3: the recipient shown is the recipient paid | [signing-gate.md, Policy checks](../wallet-core/signing-gate.md#policy-checks); [security-model.md](../security/security-model.md) | [WP9](../roadmap/implementation-plan.md#wp9) (policy checks 12 to 15) |
| [T-APP1](../testing/acceptance.md#t-app1) | app platform: Lua and C++ behave the same; the wallet screens differ only in the context line | [app-platform/examples.md](../app-platform/examples.md) | [WP12](../roadmap/implementation-plan.md#wp12) |
| [T-APP2](../testing/acceptance.md#t-app2) | app platform: patch P1 (ESP-NOW handler survives app exit) | [runtime-and-boot.md](../architecture/runtime-and-boot.md) | [WP1](../roadmap/implementation-plan.md#wp1) |

Tests that belong to an F-id or an NFR but are not the package's headline test:

| Test | Work package |
|---|---|
| [T-F1b](../testing/acceptance.md#t-f1b), [T-F2b](../testing/acceptance.md#t-f2b), [T-F2c](../testing/acceptance.md#t-f2c) | [WP3](../roadmap/implementation-plan.md#wp3) |
| [T-F11c](../testing/acceptance.md#t-f11c) | [WP9](../roadmap/implementation-plan.md#wp9) |
| [T-NFR-honesty](../testing/acceptance.md#t-nfr-honesty) | [WP11](../roadmap/implementation-plan.md#wp11) |
| [T-NFR-cancel](../testing/acceptance.md#t-nfr-cancel) | release gate at the freeze |

## Attack flows

The four flows of the PRD, scripted in [attack-scripts.md](../testing/attack-scripts.md).

| Flow | Requirements exercised | Stops at | Work package that makes it demonstrable |
|---|---|---|---|
| 1. Impostor using a verified name (and 1b, payer has never seen the merchant) | F10, F11 | attestation check; red needs the judge badge seeded | [WP9](../roadmap/implementation-plan.md#wp9) |
| 2. Replayed old request | F8, F9 | presence check | [WP8](../roadmap/implementation-plan.md#wp8) |
| 3. Compromised laptop alters amount | F2, F3, F12 | approval screen | [WP10](../roadmap/implementation-plan.md#wp10) |
| 4. Revoked badge | F15 | attestation check; red needs the judge badge seeded | [WP11](../roadmap/implementation-plan.md#wp11) |

## Measurements

| Id | Serves | Recorded in work package |
|---|---|---|
| [M1](../testing/measurements.md#m1) | F9 (`deadline_ms`); latency | WP8, on a `PAY_MEASURE 1` build |
| [M2](../testing/measurements.md#m2) | F17, the Ed25519 backend choice; sandbox fit | WP7 |
| [M3](../testing/measurements.md#m3) | latency: request to approval screen | WP10 |
| [M4](../testing/measurements.md#m4) | latency: SELECT to confirmed on dashboard | WP10 |
| [M5](../testing/measurements.md#m5) | F4 polling intervals; reliability | WP4 |
| [M6](../testing/measurements.md#m6) | stack and heap headroom | WP10 |
| [M7](../testing/measurements.md#m7) | token-account derivation time | WP3 |

M3, M4 and M6 are recorded in WP10 because that is the first point at which the whole flow exists.

## Patches

The upstream patches P1–P14 ([runtime-and-boot.md](../architecture/runtime-and-boot.md#upstream-patches)) and the package that applies each ([implementation plan](../roadmap/implementation-plan.md#patches-by-work-package)).

| Patch | Work package |
|---|---|
| P1, P2, P3, P7 | WP1 |
| P4, P9, P12 | WP3 |
| P11: `/api/wallet/config`, `SETWALLET`, the `/api/identity` fields | WP4 |
| P5 | WP7 |
| P11: `/api/wallet/audit`; P10: Settings → Wallet and the token-account line | WP11 |
| P10: launcher part; P13 | WP12 |
| P6, P14 | WP13 |
| P8 | not scheduled; required before `WALLET_ENABLE_BROKER` is ever set to 1 |

## Work packages to requirements

| WP | Requirements | Done when |
|---|---|---|
| [WP0](../roadmap/implementation-plan.md#wp0) | precondition for all | upstream log lines seen; `/api/identity` answers |
| [WP1](../roadmap/implementation-plan.md#wp1) | precondition for all | T-APP2 |
| [WP2](../roadmap/implementation-plan.md#wp2) | F3 (decoder), F8 (frame codec), F10 (derivation, account parser) as host-tested code | `ctest` green: `test_sol`, `test_pay`, `test_attest` |
| [WP3](../roadmap/implementation-plan.md#wp3) | F1, F2, F3 | T-F1, T-F1b, T-F2, T-F2b, T-F2c, T-F3; M7 recorded |
| [WP4](../roadmap/implementation-plan.md#wp4) | supports F4, F7, F10 | a test app prints balances; config round-trips; M5 recorded |
| [WP5](../roadmap/implementation-plan.md#wp5) | F4 | T-F4 |
| [WP6](../roadmap/implementation-plan.md#wp6) | F5, F6, F7 | T-F5, T-F6, T-F7 |
| [WP7](../roadmap/implementation-plan.md#wp7) | F8 | T-F8; M2 recorded |
| [WP8](../roadmap/implementation-plan.md#wp8) | F9 | T-F9; M1 recorded; `deadline_ms` set |
| [WP9](../roadmap/implementation-plan.md#wp9) | F10, F11 | T-F10, T-F11, T-F11c; attack flow 1 |
| [WP10](../roadmap/implementation-plan.md#wp10) | F12 (badge side) | attack flow 3; dashboard gaps closed; M3, M4, M6 recorded |
| [WP11](../roadmap/implementation-plan.md#wp11) | F14, F15, F16; honesty | T-F14, T-F15, T-F16, T-NFR-honesty; attack flow 4 |
| [WP12](../roadmap/implementation-plan.md#wp12) | app platform | T-APP1 |
| [WP13](../roadmap/implementation-plan.md#wp13) | F17, F19 | T-SE1 or documented fallback; T-F19 |
| Freeze (hour 22) | judge UX; the demo | T-NFR-cancel; known names seeded on each judge badge; the four attack flows |

## Requirements covered

This document covers no requirement itself. It indexes F1–F19 and the PRD's non-functional requirements.

## Open items

- [UNVERIFIED] Every acceptance and measurement id above refers to a check that has not been run. Fallback: none; the checks are the definition of done.
- [UNVERIFIED] The full register, U1 to U23, is in the [README](../README.md#open-items).
