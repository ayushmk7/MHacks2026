# Pre-event checklist

The PRD's pre-event checklist turned into actions with a pass condition each, plus four checks the design adds. Work through it before judges arrive.

Audience: operators.

Status: design, not yet built on hardware.

None of these checks has been performed yet: no badge has been flashed, and the dashboard's on-chain steps have not run for real either (see "What is verified" in the [dashboard runbook](../../dashboard/RUNBOOK.md)). The badge-side commands are the upstream tooling from `firmware/solana-os/` at commit `812b8c7` of the [upstream repository](https://github.com/spacemandev-git/solana-defcon-badge-26/tree/812b8c7aca5c366d18c0b040fafd2999f7204d84/firmware/solana-os) and the routes our fork adds.

Run every `npm` command from inside `dashboard/`. Badge commands run from `firmware/solana-os/`.

## Checklist

Tick an item only when its pass condition has been seen, not when the action has been run.

- [ ] 1. Solana OS flashed on all 4 badges; Settings → Identity checked
- [ ] 2. HACK minted on devnet; a token account exists for every badge
- [ ] 3. Every badge pre-funded with devnet SOL
- [ ] 4. A badge reaches devnet RPC through the phone hotspot
- [ ] 5. The .tech domain is registered
- [ ] 6. Registry approach decided and working: SAS
- [ ] 7. (added) ESP-NOW works between badges on the hotspot
- [ ] 8. (added) The dashboard listener is reachable from a badge
- [ ] 9. (added) PROOF round trip measured and `deadline_ms` set
- [ ] 10. (added) Known names seeded on each judge badge

| # | PRD checklist item | Action | Pass when |
|---|---|---|---|
| 1 | Flash Solana OS on all 4 badges; check Settings → Identity | [Build upstream first](build-and-flash.md#build-upstream-first), then [our tree](build-and-flash.md#our-tree), on each; open Settings → Identity | each shows a badge ID and `secure element` or `software`; recorded in `badges.json` |
| 2 | Mint HACK on devnet; create token accounts | `npm run devnet:setup` in `dashboard/` | `/api/status` → `token.mint` set; `/api/badges` shows `hack` ≥ 100 for all four |
| 3 | Pre-fund every badge with devnet SOL | same script (0.05 SOL each) | `/api/badges` → `sol` ≥ 0.02; Home shows SOL |
| 4 | Confirm a badge reaches devnet RPC through a phone hotspot | Home on the hotspot | balances appear; serial shows no `[rpc]` errors; record the request time |
| 5 | Register the .tech domain | outside the OS documentation | not checked here |
| 6 | Read the SAS docs and decide registry approach | decided: SAS | Registry page issues an attestation and the merchant badge's Home shows `verified` |
| 7 | (added) ESP-NOW between badges on the hotspot | Settings → ESP-NOW radar on two badges | each lists the other |
| 8 | (added) Dashboard listener reachable from a badge | `curl -i "http://<laptop-ip>:8788/badge/pending?badge=<pubkey>"` from another hotspot client | `204` |
| 9 | (added) Measure PROOF round trip and set `deadline_ms` | [M1](../testing/measurements.md#m1) | value recorded and pushed to all badges |
| 10 | (added) Seed known names on each judge badge | with the merchant attested and requesting 10.00: Pay on the judge badge, select the request, wait for the approval screen, CANCEL | Screen A green with `MHacks Merch` and `verified`, then CANCEL; the impostor check shows red `NAME MISMATCH` |

## How to run each item

### 1. Flash and check identity

1. Flash each badge: [build-and-flash.md](build-and-flash.md#flashing-four-badges).
2. On each badge open Settings → Identity. It shows the badge ID, `key lives in  secure element` (green) or `software` (amber), and the full public key [UPSTREAM `src/ui/shell.cpp:699-746`].
3. Copy each key and key location into `dashboard/server/config/badges.json` and set `standIn` to `false`:

   ```bash
   BADGE=<badge-ip>; CODE=<pairing code>
   curl -s -H "X-Badge-Token: $CODE" "http://$BADGE/api/identity"
   ```

4. Restart the dashboard server (`npm run server`).

Pass: all four badges show an ID and a key location, and `badges.json` holds four real keys. A badge that reports `software` passes this item; what it means for the pitch is in [../wallet-core/keys-and-se050.md](../wallet-core/keys-and-se050.md).

### 2 and 3. Mint, token accounts, SOL

1. The registry authority on the laptop needs devnet SOL first. The faucet rate-limits; do this well before the event. See "First real devnet run" in the [dashboard runbook](../../dashboard/RUNBOOK.md).
2. Run the setup script. It creates the mint, sends 0.05 SOL to every configured badge holding less than 0.02, creates each token account and mints 1000 HACK to every badge holding less than 100.

   ```bash
   npm run devnet:setup
   ```

3. Restart the server, then check:

   ```bash
   curl -s http://127.0.0.1:8787/api/status     # token.mint is set
   curl -s http://127.0.0.1:8787/api/badges     # every badge: hack >= 100, sol >= 0.02
   ```

4. Give every badge the mint and the registry addresses: [configure.md](configure.md#wallet-config).

Pass: `token.mint` is set, all four badges show `hack` ≥ 100 and `sol` ≥ 0.02, and Home on each badge shows the SOL balance. No faucet call is needed during judging.

### 4. Devnet RPC through the hotspot

1. Join the badge to the phone hotspot ([configure.md](configure.md#wi-fi)) and open Home.
2. Watch the serial monitor or `tools/badge-push.py --host <badge-ip> --token <code> logs`.

Pass: HACK and SOL balances appear; no `[rpc]` error lines. Record the request times from the `[rpc] <method> <n>ms status=<code>` lines in [M5](../testing/measurements.md#m5). [UNVERIFIED: HTTPS latency through a hotspot and connection reuse. Fallback: lengthen polling; a second RPC URL.]

### 5. The .tech domain

Outside the OS. Only the dashboard's read-only feed is published there (PRD).

### 6. Registry: SAS

The decision is made: the Solana Attestation Service, exactly as the dashboard uses it (credential "MHacks Verified", schema `badge-identity` version 1, one string field `name`, nonce = the badge's public key) [OURS: the dashboard is already built on it; the address derivation and account parsing are host-tested].

1. On the dashboard's Registry page: Issue card, pick the merchant badge, type `MHacks Merch`, **Issue attestation**.
2. On the merchant badge: open Home.

Pass: the Registry page shows the attestation with a transaction link, and the merchant badge's Home shows `verified` next to `MHacks Merch` within one refresh (Home re-checks its own attestation every 30 s).

Fallback if the check is not finished in time: a signed allowlist baked into the firmware, with the same screen states and no revocation ([../roadmap/implementation-plan.md](../roadmap/implementation-plan.md#fallback-ladder)).

### 7. ESP-NOW on the hotspot

1. Join two badges to the hotspot.
2. On both: Settings → ESP-NOW [UPSTREAM radar screen].

Pass: each lists the other. If not, the badges are on different channels: ESP-NOW follows the Wi-Fi channel, so both must be joined to the same hotspot [UPSTREAM `README.md:1248-1262`]. Note the signal strength at table distance for tuning `rssi_min` ([measurements](../testing/measurements.md#other-quantities-to-record)).

### 8. Dashboard listener

1. In `dashboard/.env` set `BADGE_LISTEN_HOST` to the laptop's hotspot IP (`ipconfig getifaddr en0` on macOS). Restart the server. The Attack page reads "listener open" and shows the URL the badge polls. With `BADGE_LISTEN_HOST=0.0.0.0` that URL is built from the laptop's first non-internal IPv4 address; check that it is the hotspot address.
2. From another device on the hotspot:

   ```bash
   curl -i "http://<laptop-ip>:8788/badge/pending?badge=<pubkey>"
   ```

Pass: `204`. `404` means that key is not in `badges.json`. No answer means the hotspot isolates clients [UNVERIFIED; fallback: the "Mark rejected" button on the Attack page stays usable, on pending and on expired attempts].

### 9. PROOF round trip

1. Run measurement [M1](../testing/measurements.md#m1) with two badges on the hotspot. M1 needs a build with `PAY_MEASURE 1` on those two badges.
2. Flash the two badges with the normal build again (`PAY_MEASURE 0`), application image only. The measurement build is never used for the demo.
3. Set `deadline_ms` to about p99 × 1.3 and push it to all four badges ([configure.md](configure.md#later-changes)).

Pass: the value is written in the M1 results table and Settings → Wallet shows it as `Presence deadline` on every badge. [UNVERIFIED: the default of 400 ms. Fallback values: 800 ms if any badge signs with the SE050; 2500 ms if the fast Ed25519 backend is not shipped.]

### 10. Seed known names on each judge badge

Red `NAME MISMATCH` and `REVOKED` need the payer's badge to have seen the real merchant verified once. The pre-event seeding step and the demo order guarantee that. A badge that has never seen the merchant shows amber `UNVERIFIED` — still never `verified`.

Do this once on every judge badge, after item 6 (the merchant is attested) and after the last time a filesystem image is flashed. The store survives reboots.

1. On the merchant badge: open Request, choose 10.00, press SELECT on the request screen.
2. On the judge badge: open Pay and select the merchant's request.
3. Wait for the green [Screen A](../wallet-core/screens.md#screen-a) showing `MHacks Merch` and `verified`.
4. Press CANCEL on the judge badge. On the merchant badge press CANCEL in Request.

The attestation check that produced the green screen wrote the merchant to `/wallet/known.bin` on the judge badge ([attestation, Known names](../identity/attestation.md#known-names)).

Check: repeat with the impostor badge named `MHacks Merch` ([flow 1](../testing/attack-scripts.md#flow-1-impostor-using-a-verified-name)). The judge badge must show red `NAME MISMATCH`.

Pass: Screen A was green, then CANCEL; the impostor check is red.

Between seeding and the demo:

- **Never** use Settings → Wallet → Forget known names.
- **Never** re-flash with a filesystem image (`write_flash 0x670000`). Flashing the application image alone keeps the store.
- In the demo, step 1 (pay the verified merchant) always comes before the impostor, compromised-laptop and revocation steps, on the same judge badge.
- After the revocation demo, re-issue the merchant's attestation on the dashboard before the next audience. The known-names entry stays, and the merchant shows `verified` again within 30 s.

## Per-badge sheet

| Check | Merchant | Impostor | Judge A | Judge B |
|---|---|---|---|---|
| Flashed with the fork; `[os] ready` seen | [ ] | [ ] | [ ] | [ ] |
| Key location recorded (`secure element` / `software`) | | | | |
| In `badges.json`, `standIn: false` | [ ] | [ ] | [ ] | [ ] |
| On the hotspot | [ ] | [ ] | [ ] | [ ] |
| Wallet config pushed; Settings → Wallet matches | [ ] | [ ] | [ ] | [ ] |
| Apps installed (`home pay request history checkout tipjar`) | [ ] | [ ] | [ ] | [ ] |
| Home shows HACK ≥ 100 and SOL ≥ 0.02 | [ ] | [ ] | [ ] | [ ] |
| `deadline_ms` set to the measured value | [ ] | [ ] | [ ] | [ ] |
| Normal build flashed (`PAY_MEASURE 0`) | [ ] | [ ] | [ ] | [ ] |
| Known names seeded; impostor check red | n/a | n/a | [ ] | [ ] |
| Physical label matches `badges.json` | [ ] | [ ] | [ ] | [ ] |

## Also before judges arrive

- [ ] The laptop side: the pre-flight checklist in the [dashboard runbook](../../dashboard/RUNBOOK.md).
- [ ] [T-NFR-cancel](../testing/acceptance.md#t-nfr-cancel) passes on every badge: CANCEL leaves every screen, and holding CANCEL 1.5 s leaves every app. This is a release gate.
- [ ] [T-NFR-honesty](../testing/acceptance.md#t-nfr-honesty) passes: the badge, `/api/identity` and the dashboard agree on the key location.
- [ ] The four attack flows rehearsed once: [../testing/attack-scripts.md](../testing/attack-scripts.md).
- [ ] Nothing has erased a judge badge's known names since item 10: no "Forget known names", no filesystem image flashed.
- [ ] The merchant is attested again if the revocation flow was rehearsed (Registry page shows `verified`).

## Requirements covered

None directly. The checklist is the PRD's reliability requirement in executable form: one phone hotspot, Wi-Fi only, every badge pre-funded so that no faucet call happens during judging.

## Open items

- [UNVERIFIED] Nothing has run on a badge. Item 1 is the first time.
- [UNVERIFIED] HTTPS RPC latency through a hotspot (item 4). Fallback: lengthen polling; second RPC URL.
- [UNVERIFIED] The hotspot lets badge and laptop reach each other (item 8). Fallback: "Mark rejected".
- [UNVERIFIED] PROOF deadline (item 9). Fallback values 800 ms and 2500 ms.
- Item 10 is procedure, not firmware: a judge badge that was not seeded, or whose store was erased, shows amber `UNVERIFIED` where the demo expects red `NAME MISMATCH` or `REVOKED`.
- [UNVERIFIED] Devnet rate limits with four badges polling. Fallback: Home polls one call per 3 s and backs off on HTTP 429.
