# Glossary

The terms used across the badge OS documentation, each with a short definition and the document that treats it in full.

Audience: everyone.

Status: design, not yet built on hardware.

Definitions describe the design, not a running system: nothing has been built or run on a badge. "Upstream" means Solana OS, `firmware/solana-os/` at commit `812b8c7` of the [upstream repository](https://github.com/spacemandev-git/solana-defcon-badge-26/tree/812b8c7aca5c366d18c0b040fafd2999f7204d84/firmware/solana-os).

## Solana

| Term | Meaning | See |
|---|---|---|
| Address, public key | A 32-byte Ed25519 public key, written in base58. The badge's public key is its Solana address [UPSTREAM]. | [keys-and-se050.md](../wallet-core/keys-and-se050.md) |
| ATA (associated token account) | The standard token account of an owner for a mint. Its address is a PDA of the Associated Token Account program with seeds `[owner, Token program id, mint]`, so it can be computed without asking the network. The badge always keeps its HACK in its ATA and pays into the payee's ATA [OURS, host-tested]. | [transaction-building.md](../protocol/transaction-building.md) |
| base58 | The text encoding of addresses, blockhashes and signatures. Public keys cross the Lua API as base58 strings and the C API as 32 raw bytes. | [api-reference.md](../app-platform/api-reference.md) |
| Blockhash (recent blockhash) | A 32-byte value from the cluster that every transaction must carry. It stays valid for roughly 60 to 90 s, which is why one signing call has a budget of 60 s for everything it shows. | [transaction-building.md](../protocol/transaction-building.md) |
| Bump | The one-byte value, tried from 255 downwards, that makes a PDA derivation land off the curve. | [attestation.md, Derivation](../identity/attestation.md#derivation) |
| Devnet | The Solana test cluster the project runs on. No real money. Default RPC URL `https://api.devnet.solana.com`. | [configure.md](../guides/configure.md) |
| HACK | The demo SPL token, 2 decimals, minted on devnet by the dashboard's setup script. | [BADGE-GAPS.md](../../dashboard/BADGE-GAPS.md) |
| Lamport | The base unit of SOL. SOL balances cross the API as decimal strings of lamports. | [api-reference.md, `badge.rpc`](../app-platform/api-reference.md#badgerpc) |
| Legacy message, v0 message | The two transaction message formats. A v0 message is a legacy message with a leading `0x80` byte and a trailing list of address-table lookups. The decoder accepts legacy, and v0 with no lookups. | [transaction-decoder.md](../wallet-core/transaction-decoder.md) |
| Message (transaction message) | The bytes that are signed. A HACK transfer is 214 bytes as a legacy message and 216 as v0. `identity.sign` takes a message, not a wire transaction. | [transaction-decoder.md](../wallet-core/transaction-decoder.md) |
| Mint | The account that defines a token. The config key `mint` holds the HACK mint and must equal the dashboard's `HACK_MINT`. | [config-limits-audit.md](../wallet-core/config-limits-audit.md) |
| PDA (program-derived address) | An address computed from seeds and a program id: the first SHA-256 of `seeds ‖ [bump] ‖ program id ‖ "ProgramDerivedAddress"`, for bump 255 down to 0, that is not a point on the Ed25519 curve. No private key exists for it. Token accounts (ATAs) and attestation accounts are PDAs. | [attestation.md, Derivation](../identity/attestation.md#derivation) |
| Raw units | An amount as an integer count of the token's smallest unit. 10.00 HACK is raw `1000` at 2 decimals. Amounts cross every API as decimal strings of raw units, because the badge's Lua has 32-bit numbers [UPSTREAM constraint]. | [lua-apps.md](../app-platform/lua-apps.md) |
| RPC | The JSON-RPC endpoint of the cluster. The badge calls it directly over Wi-Fi for balances, blockhashes, account data, sending and status. | [transaction-building.md](../protocol/transaction-building.md) |
| SPL Token, Token program | The classic token program, `TokenkegQfeZyiNwAJbNbGKPFXCWuBvf9Ss623VQ5DA`. Token-2022 is not accepted. | [transaction-decoder.md](../wallet-core/transaction-decoder.md) |
| `TransferChecked` | The only instruction the badge signs: SPL Token instruction tag 12 with a 64-bit amount and the mint's decimals. The chain rejects it unless the decimals match the mint, so the amount on screen is the amount that moves. | [transaction-decoder.md](../wallet-core/transaction-decoder.md) |
| Wire transaction | What is submitted: `0x01`, the 64-byte signature, then the message. 279 bytes for a legacy transfer. | [transaction-building.md](../protocol/transaction-building.md) |

## Identity registry

| Term | Meaning | See |
|---|---|---|
| SAS (Solana Attestation Service) | The on-chain program that holds credentials, schemas and attestations: `22zoJMtdu4tQc2PzL74ZUT7FrwgB1Udec8DdW4yw4BdG`. The registry of this project. | [attestation.md](../identity/attestation.md) |
| Authority (registry authority) | The keypair that issues and revokes attestations. It lives only on the laptop that runs the dashboard. | [dashboard architecture](../../dashboard/ARCHITECTURE.md) |
| Credential | The SAS account that names the issuer, here "MHacks Verified". Its address depends on the authority, so it is configuration: config key `cred`. | [attestation.md, Derivation](../identity/attestation.md#derivation) |
| Schema | The SAS account that defines the attested data: `badge-identity` version 1, one string field `name`. Config key `schema`. | [attestation.md, Account layout](../identity/attestation.md#account-layout) |
| Attestation | The SAS account that binds a verified name to one badge key. Its address is a PDA of the credential, the schema and the badge's public key. Issuing creates it; revoking closes it. | [attestation.md](../identity/attestation.md) |
| Attested name | The name inside a valid attestation. It is the only name ever drawn in the name row of the approval screen. | [screens.md, Screen A](../wallet-core/screens.md#screen-a) |
| Claimed name | A name a counterparty or an app asserts (in a REQ, an IAM, or a hint). It is shown only in a detail line, never in the name row: `claims: <name>` when the identity status is mismatch, `calls itself: <name>` when it is unverified, revoked, expired or unknown, and not at all when it is verified. | [screens.md, Screen B](../wallet-core/screens.md#screen-b) |
| Identity status | The result of the attestation check: verified, unverified, mismatch, revoked, expired, or unknown. On screen: `verified`, `UNVERIFIED`, `NAME MISMATCH`, `REVOKED`, `EXPIRED`, `NOT CHECKED`. | [attestation.md, Decision](../identity/attestation.md#decision) |
| Mismatch | A verified key that claims a different name than its attested one, or an unverified key that claims a name this badge has seen verified for another key. Red. | [attestation.md, Decision](../identity/attestation.md#decision) |
| Revoked | No attestation account exists for a key that this badge has previously seen verified. Red. | [attestation.md, Known names](../identity/attestation.md#known-names) |
| Attestation cache | 16 entries in RAM. An entry is fresh for `attest_ttl` seconds (default 30); the signing gate re-fetches a stale one before showing the approval screen. | [attestation.md, Cache](../identity/attestation.md#cache) |
| Known-names store | The file `/wallet/known.bin`: 16 records of keys and names this badge has seen verified. It tells "revoked" from "never verified" and catches a second badge claiming a known name. Survives reboots; erased by Settings → Wallet → Forget known names and by flashing a filesystem image. | [attestation.md, Known names](../identity/attestation.md#known-names) |
| Seeding step | The pre-event step, done once on every judge badge, that puts the real merchant in the known-names store: open the merchant's request in Pay, wait for the green approval screen showing `MHacks Merch` and `verified`, press CANCEL. Red `NAME MISMATCH` and `REVOKED` need it; an unseeded badge shows amber `UNVERIFIED`. | [pre-event-checklist.md](../guides/pre-event-checklist.md#10-seed-known-names-on-each-judge-badge) |
| Demo order | The honest payment to the verified merchant always comes before the impostor, compromised-laptop and revocation steps, on the same judge badge. | [attack-scripts.md](../testing/attack-scripts.md#before-any-flow) |
| `attest_parse()` | The pure C function that checks every field of one attestation account. Reference code with its own host suite. | [attestation.md, Account layout](../identity/attestation.md#account-layout) |
| `clock_synced` | True once the SNTP sync callback has fired and the time is past a fixed threshold. Gates the attestation expiry check. | [attestation.md, Clock](../identity/attestation.md#clock) |
| Nonce (SAS) | The value that makes an attestation address unique. Here it is the badge's public key. Not the same thing as the nonce of a CHAL. | [attestation.md, Derivation](../identity/attestation.md#derivation) |
| RPC CA pin | A CA certificate named `rpc-ca` installed on the badge so that TLS to the RPC host is validated. Without it the screen says `RPC TLS not pinned`. | [attestation.md, Transport trust](../identity/attestation.md#transport-trust) |

## Payment protocol

| Term | Meaning | See |
|---|---|---|
| REQ | Payment request, broadcast by the payee every second while its session is open: payee key, amount, request id, time to live (`ttl_s`, at most 120 s), name, and a signature. 155 bytes. | [payment-protocol.md, Messages](../protocol/payment-protocol.md#messages) |
| CHAL | Challenge from payer to payee: request id, a fresh 16-byte nonce, the payer's key. 60 bytes. | [payment-protocol.md, Messages](../protocol/payment-protocol.md#messages) |
| PROOF | The payee's answer: a signature over `pay-proof:`, the request id, the nonce and the payer's key. 108 bytes. | [payment-protocol.md, Messages](../protocol/payment-protocol.md#messages) |
| HELLO, IAM | HELLO asks a nearby badge for its public key; IAM is the unsigned answer (key and name). A key learned from IAM counts as "presence not checked" until a PROOF arrives. 4 and 69 bytes. | [payment-protocol.md, Messages](../protocol/payment-protocol.md#messages) |
| PAID | An unsigned hint from payer to payee carrying the transaction signature. The payee still verifies on chain. 76 bytes. | [payment-protocol.md, Messages](../protocol/payment-protocol.md#messages) |
| RCPT | Receipt signed by the payee over the request id and the transaction signature (F19, stretch). 140 bytes. Sent by `pay.receipt()` to the badge the PAID hint came from; the payer's History then shows `co-signed`. | [receipts.md](../apps/receipts.md) |
| Application frame | An ESP-NOW frame of upstream's type `0x02`; every protocol message is the payload of one (at most 240 bytes) [UPSTREAM]. | [payment-protocol.md](../protocol/payment-protocol.md) |
| Domain separation | Each signed message starts with a fixed prefix (`pay-req:`, `pay-proof:`, `pay-rcpt:`), so a message signature can never be used as a transaction signature, or the reverse. Host-tested. | [signing-gate.md, Message signing](../wallet-core/signing-gate.md#message-signing) |
| Request id | 8 random bytes that name one request. A used id is remembered and not listed again. | [payment-protocol.md, Rules](../protocol/payment-protocol.md#rules) |
| Nonce (CHAL) | 16 fresh random bytes, single-use, that the payee must sign to prove it is there now. | [payment-protocol.md, Rules](../protocol/payment-protocol.md#rules) |
| `mint_tag` | The first 4 bytes of the mint's public key, carried in a REQ so that requests for another token are dropped. | [payment-protocol.md, Messages](../protocol/payment-protocol.md#messages) |
| Presence | Whether the payee answered a fresh challenge with a valid PROOF within the deadline. On screen: `present`, `presence not checked`, `NOT PRESENT`. | [payment-protocol.md, Timing](../protocol/payment-protocol.md#timing) |
| Deadline (`deadline_ms`) | The longest challenge-to-proof time that still counts as present. Default 400 ms; set from measurement M1. | [measurements.md, M1](../testing/measurements.md#m1) |
| Measurement build (`PAY_MEASURE 1`) | A build in which the PROOF count limits are lifted so that 50 round trips can be measured from one request. Never used for the demo. | [measurements.md, M1](../testing/measurements.md#m1) |
| Session | The payee's open request (or receive mode). One at a time; it ends when the app that opened it stops. | [payment-protocol.md, Payee state machine](../protocol/payment-protocol.md#payee-state-machine) |
| Receive mode | A session without a request: the payee agrees for 10 minutes to answer presence checks from payers who pick it from the nearby list. No payment is made by it. | [screens.md, Screen G](../wallet-core/screens.md#screen-g) |
| Inbox | The payer's list of up to 4 verified requests, strongest signal first. An entry keeps the MAC of its first receipt and lives `ttl_s` seconds. | [payment-protocol.md, Rules](../protocol/payment-protocol.md#rules) |
| Presence table | The payer's record of payees that proved presence in the last 60 s (8 entries). The signing gate reads it. | [payment-protocol.md, Rules](../protocol/payment-protocol.md#rules) |
| Replay ring | The last 32 request ids that reached an end state on this badge. A REQ with one of them is not listed. RAM only. | [payment-protocol.md, Rules](../protocol/payment-protocol.md#rules) |
| RSSI, `rssi_min` | Received signal strength. Used to order lists ("nearest badge") and, below `rssi_min`, to hide a request. A hint for ordering, not a security property. | [payment-protocol.md](../protocol/payment-protocol.md) |
| Relay | Forwarding CHAL and PROOF between a distant payee and the payer within the deadline. The design resists it by timing; it does not prevent it. | [security-model.md](../security/security-model.md) |

## Wallet core

| Term | Meaning | See |
|---|---|---|
| Wallet core | The native firmware under `src/wallet/` that decodes, checks, displays and signs. Apps can ask it for a signature; only it can produce one. | [wallet-core/api.md](../wallet-core/api.md) |
| Gate (signing gate) | The code path in `wallet.cpp` that is the only way to the key. A signing call needs a token object that only the gate can construct; the gate always runs the approval modal first. | [signing-gate.md, Key gate](../wallet-core/signing-gate.md#key-gate) |
| Modal (approval modal) | The blocking screen loop inside the signing call. While it runs, the calling app is suspended and the main loop does not run, so nothing else can draw, read buttons, change the backlight or dismiss it. | [signing-gate.md, Approval modal](../wallet-core/signing-gate.md#approval-modal) |
| Approval screen | The wallet's screen for a payment: amount, recipient, identity line, presence line. Screens A (green), B (amber), C (red); D is the second confirmation. | [screens.md, Screen A](../wallet-core/screens.md#screen-a) |
| Blocked screen | Screen E: the wallet refuses to sign and says why ("Unknown instruction", "Unknown token", …). | [screens.md, Screen E](../wallet-core/screens.md#screen-e) |
| Decoder | The strict parser that accepts exactly one `TransferChecked` instruction and nothing else. Host-tested. | [transaction-decoder.md](../wallet-core/transaction-decoder.md) |
| Policy checks | The ordered checks after decoding: permission, readiness, display, rate limit, signer, mint, decimals, source, limit, then recipient, identity, presence and claimed amount. | [signing-gate.md, Policy checks](../wallet-core/signing-gate.md#policy-checks) |
| Hint | A value an app passes along with a signing request: `recipient`, `claimed_name`, `claimed_amount`, `request_id`. The wallet verifies each against what it derives itself. A hint can lower the trust level shown, never raise it. | [api-reference.md, `badge.identity`](../app-platform/api-reference.md#badgeidentity) |
| Severity | The colour of the approval screen, from identity and presence together: green, amber or red. Payee-side prompts are purple. | [signing-gate.md, Severity and gestures](../wallet-core/signing-gate.md#severity-and-gestures) |
| Gesture | What the user must do to sign: press SELECT (green), hold SELECT 2 s (amber), nothing possible when red blocks signing, or hold SELECT 3 s when `block_red` is 0. | [signing-gate.md, Severity and gestures](../wallet-core/signing-gate.md#severity-and-gestures) |
| `cap` | The amount above which a second confirmation screen is required. Default 100.00 HACK. | [config-limits-audit.md](../wallet-core/config-limits-audit.md) |
| `max` | The amount above which the wallet refuses. Default 1000.00 HACK. | [config-limits-audit.md](../wallet-core/config-limits-audit.md) |
| `block_red` | Config switch: when 1 (default), a red screen cannot be signed through. | [config-limits-audit.md](../wallet-core/config-limits-audit.md) |
| Rate limit | Bounds on how often an app can raise a prompt and how many proofs a session signs. | [signing-gate.md, Rate limits](../wallet-core/signing-gate.md#rate-limits) |
| Audit log | An append-only text file written by the wallet core, one line per signing decision or config change, with ten space-separated fields. Not reachable from apps. | [config-limits-audit.md](../wallet-core/config-limits-audit.md) |
| History | The firmware-owned record of payments: a ring of 50 fixed-size records. The wallet core adds an entry for every transaction it signs. | [apps/history.md](../apps/history.md) |
| Error code (`badge_err_t`) | One enum for the whole badge API. Lua receives the name as a string (`nil, "rejected"`); C receives the number. | [error-codes.md](error-codes.md#badge_err_t) |

## Keys and trust

| Term | Meaning | See |
|---|---|---|
| Badge ID | The first eight base58 characters of the badge's public key [UPSTREAM]. | [upstream-baseline.md](../architecture/upstream-baseline.md) |
| Key location (key source) | Where the private key lives: `secure element` or `software`. Shown on Settings → Identity, on the first row of Settings → Wallet, and reported by `/api/identity`; the dashboard calls it `keyLocation` (`se050`, `software`). | [keys-and-se050.md](../wallet-core/keys-and-se050.md) |
| SE050 | The NXP secure element on the badge. A key generated inside it never leaves it. Upstream's driver has never run on a real part. | [keys-and-se050.md](../wallet-core/keys-and-se050.md) |
| Software key | A 32-byte seed stored in plaintext in NVS. Readable by anyone with the badge and a USB cable. The fallback when the SE050 path fails. | [keys-and-se050.md](../wallet-core/keys-and-se050.md) |
| TweetNaCl | The Ed25519 implementation upstream ships. Slow on the badge by upstream's own account. | [keys-and-se050.md](../wallet-core/keys-and-se050.md) |
| Monocypher | The Ed25519 implementation this design vendors (version 4.0.2). Produces byte-identical signatures; faster on the host by a factor of about 30. On-device speed is unmeasured. | [keys-and-se050.md](../wallet-core/keys-and-se050.md) |
| TCB (trusted computing base) | Everything that must be correct for the guarantees to hold: all native firmware, including compiled-in C++ apps. Lua apps are outside it. | [security-model.md](../security/security-model.md) |
| Trust zones | Key holder (identity module and SE050), signing gate, trusted computing base, untrusted (Lua apps, radio, HTTP responses, laptop, phone). | [architecture/overview.md](../architecture/overview.md) |
| Guarantee (G1–G12) | A numbered security property with its mechanism and test. | [security-model.md](../security/security-model.md) |

## App platform

| Term | Meaning | See |
|---|---|---|
| Badge API | The one host API, with identical names in Lua (`badge.x.y`), C (`badge_x_y`) and the C++ SDK (`badge::x::y`). | [api-reference.md](../app-platform/api-reference.md) |
| Lua app | A directory `/apps/<id>/` with `main.lua` and `app.ini`, pushed at run time, sandboxed, untrusted [UPSTREAM runtime]. | [lua-apps.md](../app-platform/lua-apps.md) |
| C++ app (native app) | An app compiled into the firmware image and listed in a registry table. Trusted code; changing one means reflashing. | [cpp-apps.md](../app-platform/cpp-apps.md) |
| Manifest (`app.ini`) | The `key=value` file that names an app and lists its permissions. | [app-platform/overview.md](../app-platform/overview.md) |
| Permission | A capability an app declares in its manifest: `sign`, `net`, `radio`, `wallet`, `system`. Holding `sign` lets an app ask; the wallet's screen still decides. | [app-platform/overview.md](../app-platform/overview.md) |
| Native app registry | The explicit table of compiled-in apps (`src/native_apps/registry.cpp`). Unrelated to the identity registry. | [cpp-apps.md](../app-platform/cpp-apps.md) |
| Time budget | The wall-clock limit per callback: 250 ms, and 5 s for start. Enforced by a VM hook for Lua [UPSTREAM]; measured after the fact for C++. Signing pauses it. | [app-platform/overview.md](../app-platform/overview.md) |
| Force quit | Holding CANCEL for 1.5 s stops the running app from the main loop [UPSTREAM]. | [app-platform/overview.md](../app-platform/overview.md) |

## Badge and upstream

| Term | Meaning | See |
|---|---|---|
| Solana OS | The upstream application firmware this project builds on. | [upstream-baseline.md](../architecture/upstream-baseline.md) |
| Fork | Our copy of upstream's `firmware/solana-os/`, with patches P1–P14 and the new directories. Not created yet. | [build-and-flash.md](../guides/build-and-flash.md#our-tree) |
| Patch (P1–P14) | A numbered change to an upstream file. | [runtime-and-boot.md](../architecture/runtime-and-boot.md#upstream-patches) |
| Freeze | Hour 22 of the build: flash four badges, seed known names, run the release-gate test and the four attack flows, rehearse. | [implementation-plan.md](../roadmap/implementation-plan.md#hour-marks) |
| Shell, launcher | The upstream UI that runs when no app does: the app list and Settings. | [upstream-baseline.md](../architecture/upstream-baseline.md) |
| Push server | The badge's HTTP server on port 80, running while Wi-Fi is connected: app upload, logs, identity, and our wallet config routes. | [configure.md](../guides/configure.md) |
| Pairing code | The six-digit code on Settings → Push that authenticates the push API (`X-Badge-Token`). Whoever holds it can install apps and change the wallet config. | [configure.md](../guides/configure.md) |
| `badge-push.py` | The upstream command-line client for the push server (`tools/badge-push.py`). | [lua-apps.md](../app-platform/lua-apps.md) |
| ESP-NOW | Espressif's connectionless radio protocol. Unencrypted and unauthenticated here; it shares the Wi-Fi channel, so all badges join one hotspot. | [payment-protocol.md](../protocol/payment-protocol.md) |
| Phone bridge | Upstream's HTTP-over-BLE tunnel through a phone. The phone terminates TLS, so identity is not shown as verified over it. | [attestation.md, Transport trust](../identity/attestation.md#transport-trust) |
| Broker | Upstream's app-store client. Compiled out by default in the fork. | [architecture/overview.md](../architecture/overview.md) |
| NVS | The ESP32's small key-value flash store. Holds settings, the software key, the wallet config. | [upstream-baseline.md](../architecture/upstream-baseline.md) |
| LittleFS | The badge's filesystem partition: apps, certificates, and the wallet's files. | [upstream-baseline.md](../architecture/upstream-baseline.md) |
| Silkscreen names | SELECT, CANCEL, UP, DOWN, LEFT, RIGHT, as printed on the board. In Lua code SELECT is `"a"` and CANCEL is `"b"`. | [upstream-baseline.md](../architecture/upstream-baseline.md) |

## Dashboard

| Term | Meaning | See |
|---|---|---|
| Dashboard (`badgepay`) | The laptop panel: live feed, badges, registry admin, attack console. Already built. | [dashboard runbook](../../dashboard/RUNBOOK.md) |
| Badge listener | The dashboard's second HTTP port (default 8788) that a badge polls for a pending attack transaction and reports a rejection to. | [dashboard API](../../dashboard/API.md) |
| `BADGE-GAP` | A marker in the dashboard code for something that depends on the badge. Seven ids; each is closed by a badge-side deliverable. | [integration/dashboard.md](../integration/dashboard.md) |
| Stand-in badge | A software keypair in `badges.json` that takes the place of a real badge until one is flashed. The example keys in these documents are the stand-in keys. | [BADGE-GAPS.md](../../dashboard/BADGE-GAPS.md) |
| Checkout | The badge app that plays an honest merchant app and is lied to by the compromised laptop. | [apps/checkout.md](../apps/checkout.md) |

## Roles

| Term | Meaning |
|---|---|
| Payer | The badge that sends HACK. In the demo, a judge (badge J). |
| Payee, merchant | The badge that asks for and receives HACK. In the demo, badge M, attested as `MHacks Merch`. |
| Impostor | A badge (I) with its own key that claims the merchant's name without an attestation. |
| Registry admin | The teammate who issues and revokes attestations on the dashboard. |

## Documentation conventions

| Term | Meaning |
|---|---|
| [UPSTREAM] | Exists in Solana OS at commit `812b8c7`, verified by reading source. |
| [OURS] | Our design decision. |
| host-tested | The reference implementation in [`code/`](code/) passes its three suites (`test_sol`, `test_pay`, `test_attest`) on a laptop against vectors from `@solana/kit`, `sas-lib` and `@solana-program/token`. Not the same as tested on a badge. |
| syntax-checked | A header or example listing under [`code/sdk-headers/`](code/sdk-headers/) compiles with `-fsyntax-only`. It was never linked or run. |
| [UNVERIFIED] | Must be measured or confirmed on hardware; a fallback is given. |
| `TODO(spec):` | A detail the design does not fix. Left open rather than guessed. None are open as of 2026-10-03. |
| F1–F19 | PRD functional requirements. |
| D1–D12 | Structural design decisions ([architecture/overview.md](../architecture/overview.md)). |
| P1–P14 | Patches to upstream files ([runtime-and-boot.md](../architecture/runtime-and-boot.md#upstream-patches)). Not to be confused with priorities P0–P3. |
| P0–P3 | PRD priorities. |
| `T-*` | On-device acceptance tests ([acceptance.md](../testing/acceptance.md)). |
| M1–M7 | Measurements ([measurements.md](../testing/measurements.md)). |
| WP0–WP13 | Work packages ([implementation-plan.md](../roadmap/implementation-plan.md)). |
| U1–U23 | The register of unverified items ([README](../README.md#open-items)). |

## Requirements covered

None. This document defines terms.

## Open items

- [UNVERIFIED] Statements here about on-device behaviour (speed of Monocypher, the SE050 path, the default deadline) repeat unverified items from the documents they link to. Fallbacks are given there.
