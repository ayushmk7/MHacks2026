# PRD: Verified Hardware Payments on the Solana Badge

Oct 2, 2026 · @UAgarwal

## Overview

A wallet on the Solana DEF CON badge that proves who you are paying and shows exactly what you are signing, so impostors, spoofed requests and compromised apps cannot trick you into a payment.

| Item | Value |
| --- | --- |
| Event | MHacks 2026, 24-hour build |
| Target tracks | MLH Best Use of Solana (primary), FinTech main track |
| Bonus prizes | .Tech domain; ElevenLabs and Gemini if stretch features land |
| Hardware | 4 Solana DEF CON 34 badges: 2 for the team, 2 handed to judges |
| Network | Solana devnet, demo token HACK (SPL) |
| Base software | Solana OS by spacemandev (open source); we add the wallet and identity layer |

## Problem

Most peer-to-peer payment losses come from people authorizing payments to someone pretending to be someone else, not from hacked accounts.

- **Impersonation.** The CFPB alleged that customers of Zelle's three largest bank owners lost more than $870 million over seven years, and argued that limited identity verification made fake accounts easy to create ([eMarketer](https://www.emarketer.com/content/zelle-fraud-problem-cfpb-lawsuit)). The suit targeted "induced fraud": consumers tricked into sending money under false pretenses ([American Banker](https://www.americanbanker.com/news/cfpb-dismisses-lawsuit-against-zelle-and-three-big-banks)).
- **Lying confirmation screens.** Any app that draws the confirm screen can misrepresent the payment. In the February 2025 Bybit hack, a tampered Safe{Wallet} UI showed a normal transfer while hardware wallets signed a malicious payload ([Cyfrin](https://www.cyfrin.io/blog/safe-wallet-hack-bybit-exploit)).
- **Addresses are unreadable.** Wallets show base58 addresses that people cannot meaningfully verify by eye.

Zelle shows the name an account was enrolled under. That says who opened the account, not that the person in front of you controls it.

## Goals and non-goals

The build succeeds if a judge, holding a badge, completes a real payment and personally catches at least two attacks.

**Goals**

1. A judge pays a verified payee end to end on devnet with no help beyond on-screen hints.
2. The badge refuses to present an impostor as verified, even when it claims the same name.
3. The badge shows the true amount and recipient when a compromised app tampers with a transaction.
4. Every signature requires a physical button press, enforced below the Lua app layer.
5. Solana is load-bearing: token transfers, the identity registry and revocation all live on-chain.

**Non-goals**

- Mainnet or real money.
- Protecting a payer from a verified party who is dishonest (identity names them; escrow is a later app).
- Full relay-attack immunity; there is no UWB distance bounding.
- A general-purpose wallet for arbitrary tokens or programs.
- Native mobile apps; the phone only relays when Wi-Fi is unavailable.

## Users

Four roles share one badge design; only the registry admin needs a laptop.

| Role | Who in the demo | Needs |
| --- | --- | --- |
| Payer | A judge | Know who they are paying and exactly how much, with no crypto knowledge |
| Payee / merchant | Team badge running "MHacks Merch" | Request payment and get paid instantly; prove it is the real merchant |
| Registry admin | A teammate on the dashboard | Issue and revoke verifications for badge keys |
| Attacker | Team laptop and an impostor badge | Spoof a merchant name or tamper with a transaction |

## Security properties

The product is three guarantees; each maps to an attack the judge can try.

| Property | Mechanism | Attack it stops |
| --- | --- | --- |
| Verified identity bound to hardware | A Solana Attestation Service attestation binds a verified name to the badge's Ed25519 key; the key is generated on the badge (SE050 if available) | Impostor claiming a verified merchant's name |
| Proof of presence | Payer badge sends a fresh nonce over ESP-NOW; payee badge signs it with the attested key | Replayed or relayed old requests; a laptop posing as a badge |
| What you see is what you sign | Firmware decodes the raw transaction bytes, renders amount and recipient, and signs only after a SELECT press | Compromised phone, laptop or merchant app altering amount or recipient |

The approval screen shows both answers at once:

```
PAY  10 HACK
TO   MHacks Merch  ✓ verified, present
     7Qk2…M3Q
```

## User flows

The payer's badge decides on its own: a bad signature, a slow reply or a missing attestation blocks the payment before anything is signed.

&#91;embedded content: payment flow · 7 steps, 1 decision\]

The attack flows reuse the same path and fail at a named step:

| Flow | Where it stops | What the judge sees |
| --- | --- | --- |
| Impostor badge using a verified name | Attestation check | Red "unverified" warning |
| Replayed old request | Nonce proof | "Not present" warning |
| Compromised laptop alters amount | Approval screen | True amount; judge presses CANCEL |
| Revoked badge | Attestation check | Red "revoked" warning |

## Functional requirements

P0 must ship for a demo; P1 carries the differentiation; P2 makes it competitive; P3 is stretch.

| ID | Requirement | Priority | Layer |
| --- | --- | --- | --- |
| F1 | Expose `identity.pubkey()` and `identity.sign(tx)` to Lua; signing is unreachable without the approval screen | P0 | Firmware |
| F2 | Firmware-enforced approval screen: decoded amount, token, recipient; SELECT signs, CANCEL rejects; Lua cannot draw over or skip it | P0 | Firmware |
| F3 | On-badge decoder for SPL Token `transferChecked`; anything else is shown as "Unknown instruction" and blocked | P0 | Firmware |
| F4 | Home app: HACK and SOL balance, badge name, short address | P0 | Lua + RPC |
| F5 | Pay app: pick a nearby badge (strongest signal first), choose amount with the D-pad | P0 | Lua |
| F6 | Request app: broadcast "pay me X HACK" to the nearest badge | P0 | Lua |
| F7 | Submit to devnet, poll confirmation, flash LEDs on both badges | P0 | Lua + RPC |
| F8 | Payment requests signed by the payee badge key | P1 | Lua + firmware |
| F9 | Nonce handshake proving the payee is present | P1 | Lua + firmware |
| F10 | Attestation check: payee key has a valid, unrevoked "MHacks Verified" attestation; result cached for the session | P1 | Lua + RPC |
| F11 | Approval screen shows verified ✓, unverified (amber) or mismatch (red) | P1 | Firmware |
| F12 | Compromised-laptop tool that builds a tampered transaction | P2 | Laptop |
| F13 | Dashboard: live transaction feed, explorer links, attestation status; admin issue/revoke | P2 | Web |
| F14 | Spending cap: payments above a limit need a second confirmation | P2 | Firmware |
| F15 | Revocation reflected on badges within one refresh | P2 | On-chain + Lua |
| F16 | Payment history on the badge | P2 | Lua |
| F17 | Key held in the SE050, verified on silicon; upstream PR to Solana OS | P3 | Firmware |
| F18 | Voice readout of the approval screen (ElevenLabs, via laptop) | P3 | Web |
| F19 | Co-signed receipts | P3 | Lua + firmware |

## Architecture

We build two badge layers, a dashboard and the on-chain setup; the key, radios and sandbox already exist in Solana OS.

&#91;embedded content: system architecture · badge layers, devnet, web\]

The security claim rests on one rule: Lua apps can ask for a signature, but only the C wallet core can produce one, and only after its own approval screen. Solana OS already generates the Ed25519 key on first boot, in the SE050 when it answers and in NVS otherwise; its SE050 path has not been run on real hardware ([Solana OS manual](https://github.com/spacemandev-git/solana-defcon-badge-26/blob/main/firmware/solana-os/README.md)).

## Dashboard (laptop)

The dashboard runs on a laptop at the table and does three jobs: shows every payment live, lets the admin issue and revoke verifications, and drives the attack demo.

| View | Shows | Demo step |
| --- | --- | --- |
| Live feed | Each payment, newest first: time, payer badge, payee name with verified status, amount, explorer link | 1, 2 |
| Badges | All 4 badges: ID, name, key location (secure element or software), HACK and SOL balance, attestation status | Setup, 4 |
| Registry admin | Issue an attestation (badge public key to verified name); revoke one; link to the on-chain transaction | 4 |
| Attack console | A "malicious merchant" checkout that displays 5 HACK while sending the judge's badge a prebuilt 500 HACK transfer to sign | 3 |


have every transaction appear as a new block in the blockchain similar to how it does on existing cryptocurrency tracking websites 
all this is built on top of crypto
Requirements:

- Reads everything from devnet (RPC plus websocket subscriptions on the HACK token accounts and the registry); badges never talk to the dashboard directly.
- A confirmed payment appears in the feed within about 3 seconds.
- The registry authority keypair lives only on this laptop. The admin view and attack console run on localhost; only the read-only feed is published on the .tech domain.
- Large type, readable by judges from about 2 metres.
- Uses the same phone hotspot as the badges.

Open: framework choice (suggested: a single-page web app on `@solana/kit`), and how the attack console delivers its transaction to the badge (Wi-Fi HTTP or BLE; a full transaction exceeds the 240-byte ESP-NOW limit).

## Protocol

Three short ESP-NOW messages authenticate a request; the payment itself goes straight to devnet, so no transaction ever crosses ESP-NOW.

| Message | Direction | Fields | Size budget |
| --- | --- | --- | --- |
| REQ | Payee → broadcast | version, payee pubkey (32 B), amount, token mint id, request id, expiry, signature over all fields (64 B) | ≤ 240 B |
| CHAL | Payer → payee (unicast) | request id, fresh 16 B nonce from the hardware RNG | ≤ 64 B |
| PROOF | Payee → payer (unicast) | request id, signature over "pay-proof" + request id + nonce + payer pubkey | ≤ 128 B |

Rules:

- The payer accepts a PROOF only within a short deadline after CHAL (target: under 250 ms); a slow round trip is shown as "not present".
- Signatures use domain-separated prefixes (`pay-req:`, `pay-proof:`) so a request signature can never be replayed as a transaction signature.
- Nonces are single-use; a reused request id is rejected.
- ESP-NOW payloads are capped at 240 bytes and only reach badges on the same channel, so all badges join one hotspot ([Solana OS docs](https://github.com/spacemandev-git/solana-defcon-badge-26/blob/main/firmware/solana-os/README.md)).

Open: exact field encoding (binary vs base64) and the deadline value, set after measuring on hardware.

## Non-functional requirements

| Area | Requirement |
| --- | --- |
| Latency | Request to approval screen under 2 s; SELECT to confirmed on dashboard under 5 s on devnet |
| Security | No signing path exists outside the firmware approval screen; keys never exported; unknown instructions blocked, not blind-signed |
| Sandbox fit | Signing extends the Lua watchdog deadline; an SE050 signature takes about 261 ms against a 250 ms callback budget ([wolfSSL](https://www.wolfssl.com/wolfssl-nxp-se050-support/)) |
| Reliability | Works on one phone hotspot with Wi-Fi only; every badge pre-funded so no faucet call during judging |
| Judge UX | Every screen names the silkscreen button ("Press SELECT"); any flow abandons cleanly with CANCEL; amounts capped |
| Honesty | Settings shows whether the key lives in the secure element or in software |

## Demo plan

About three minutes, with judges holding two badges and the dashboard on a screen behind them.

1. **Pay a verified merchant.** The judge buys a sticker from "MHacks Merch ✓"; the transaction appears on the dashboard within seconds.
2. **Impostor.** A team badge claiming "MHacks Merch" requests payment; the approval screen shows red, and the judge rejects it.
3. **Compromised app.** The laptop says 5 HACK but submits 500; the badge shows 500, and the judge rejects it.
4. **Revocation** (if F15 ships). The admin revokes a badge on the dashboard; its next request shows red.
5. **Close.** "Zelle cannot do any of these three. This badge does, on open hardware."

Success metrics:

- Each judge completes step 1 without a teammate touching their badge.
- Steps 2 and 3 are caught on the judge's own screen every time.
- No faucet, Wi-Fi or reflash fix needed during judging.

## Positioning

No product found combines a verified counterparty, proof of presence and on-device decoding; each neighbour covers at most one.

| Product | What it has | What it lacks |
| --- | --- | --- |
| Zelle, Venmo | Instant bank-linked payments | Hardware-bound identity, proof of presence; the app controls what you see |
| Ledger, [Keystone](https://solanacompass.com/projects/keystone) | Hardware keys, clear signing of transfers | Any check of who the counterparty is |
| [SolWear](https://github.com/SolWear/SolWear) | ESP32 wearable that signs SOL transfers over NFC | Needs a phone each time; signs only what its own app builds; no identity |
| [PlaiPin](https://arena.colosseum.org/projects/explore/plaipin) | Proximity mutual authentication between wearables | Verified identity and payments |
| [Arx HaLo](https://arx.org/) | NFC chips with presence attestations | Screen, payments, Solana-native signing |

Credit line for the pitch: "Built on Solana OS by spacemandev; we added the wallet and identity layer."

## Risks and limitations

| Risk | Likelihood | Fallback |
| --- | --- | --- |
| SE050 Ed25519 path fails on silicon (never tested by its author) | Medium | Software key in NVS; drop the secure-element pitch line |
| Venue Wi-Fi blocks badges or splits ESP-NOW channels | High | One phone hotspot for all badges |
| Devnet faucet rate limits | High | Pre-fund every badge before the event |
| SAS integration takes too long | Medium | Minimal registry program or a signed allowlist; same demo, less ecosystem credit |
| Nonce handshake unfinished | Medium | Signed requests only; impostor demo still works |
| Judges get stuck in a flow | Medium | CANCEL always exits; hold CANCEL 1.5 s force-quits |

Limitations to state before judges ask:

- Identity names a dishonest verified party; it does not stop the payment. Escrow is the follow-on app.
- Someone must issue verifications. In the demo that is MHacks; in practice a bank, employer or marketplace.
- Relay-resistant, not relay-proof: timing plus a button press, no UWB.
- Devnet and a demo token, not real money.

## Open questions

- SAS or a custom registry program for attestations?
- Does the badge submit transactions over Wi-Fi itself, or relay through the phone bridge?
- Spending-cap value and whether judges can change it.
- Product name.

## Pre-event checklist

- [ ] Flash Solana OS on all 4 badges; check Settings → Identity reports secure element or software
- [ ] Mint HACK on devnet; create token accounts for every badge
- [ ] Pre-fund every badge with devnet SOL
- [ ] Confirm a badge reaches devnet RPC through a phone hotspot
- [ ] Register the .tech domain
- [ ] Read the SAS docs and decide registry approach

## Sources

- [Solana badge repository and Solana OS manual](https://github.com/spacemandev-git/solana-defcon-badge-26/blob/main/firmware/solana-os/README.md)
- [Badge broker protocol (identity and ESP-NOW)](https://github.com/spacemandev-git/solana-defcon-badge-26/blob/main/broker/PROTOCOL.md)
- [Solana Attestation Service docs](https://solana.com/docs/tools/attestations)
- [sRFC 39: Solana Clear Sign (draft)](https://github.com/solana-foundation/SRFCs/discussions/4)
- [CFPB Zelle suit coverage, eMarketer](https://www.emarketer.com/content/zelle-fraud-problem-cfpb-lawsuit)
- [CFPB dismissal coverage, American Banker](https://www.americanbanker.com/news/cfpb-dismisses-lawsuit-against-zelle-and-three-big-banks)
- [Bybit exploit analysis, Cyfrin](https://www.cyfrin.io/blog/safe-wallet-hack-bybit-exploit)
- [wolfSSL SE050 benchmarks](https://www.wolfssl.com/wolfssl-nxp-se050-support/)
