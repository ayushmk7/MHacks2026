# P2-R — Payee Flow, Presence Handshake, Relay Network, and Attack Console

Owner: **R** · Part 2 (≈ hours 8–20) · Status: **draft — finalize after U6 (app decision doc)**
Reads: [`00-Interfaces.md`](00-Interfaces.md) (§3, §5, §8, §9) · Builds on: [P1-R](P1-r-test-harness.md) (R4 timing, R5 verifier) · Parent: [`Prd-verified-payment-key.md`](Prd-verified-payment-key.md)

## 1. Why this exists
Every verified payment starts with a payee asking for money and proving it is physically there. R owns that side, the relay app that carries payments across badges (🆕 Routing), and the tools that let judges watch attacks fail.

## 2. Scope — ✏️ Changed (routing)

| ID | Item | Layer | Priority |
| --- | --- | --- | --- |
| PR1 | Request / Merchant app: enter amount (or pick preset item per U6) and rail; build the REQ frame, sign it **once** with `wallet.sign_request`, rebroadcast the same bytes ≈1 Hz until RESULT, local CANCEL, or expiry | Lua | P0 |
| PR2 | Handshake responder: on CHAL for our `req_id`, add the payer as an ESP-NOW peer and unicast PROOF via `wallet.sign_proof` | Lua | P0 |
| PR3 | RESULT handling: on status=0, confirm the tx signature over RPC (Solana) or the backend (bank) before showing "PAID ✓" and flashing `badge.led`; otherwise "Rejected" | Lua | P0 |
| PR4 | Impostor mode: a menu option that broadcasts REQs with the merchant's display name but its own unattested key (expected: red "unverified") | Lua | P0 |
| PR5 | Attack scripts (Python, from R5 + R3): tamper proxy, replay, unsigned backend call. Wired into the dashboard Attack page or standalone, per U6 | Python | P1 |
| PR6 | Merchant price list (if U6 says yes) | Lua | P2 |
| PR7 | Final `PRESENCE_DEADLINE_MS` with A from R4 numbers; written into 00 §4 | — | P0 |

### 2.1 Relay network — 🆕 Routing
Built **after the direct-payment gate passes**. Direct payment stays the fallback demo. Contract: 00 §9.

| ID | Item | Layer | Priority |
| --- | --- | --- | --- |
| PR8 | **Relay app:**<br>• rebroadcast heard REQs as `REQ_FWD`<br>• handle RREQ per the flood rules (00 §9.2), keeping route state {route_id, path, upstream MAC, downstream MAC}<br>• on RREP from downstream: CHAL the downstream hop, call `wallet.sign_route_att`, prepend the hop entry, forward upstream<br>• forward RPAY downstream and RESULT upstream<br>• honour `DEMO_NEIGHBORS`<br>• refresh its own compact record every `CREC_REFRESH_S` | Lua | P1 |
| PR9 | **Gateway role** (the relay next to the destination):<br>• `getLatestBlockhash` + `wallet.sign_route_quote`<br>• on RPAY: `sendTransaction`, poll, RESULT upstream and to the destination, then `POST /feed/route`<br>• bank: `POST /bank/authorize` with `route` | Lua | P1 |
| PR10 | **Destination support in the Request app:**<br>• answer an RREQ matching its open REQ with `dest_proof` (`wallet.sign_proof(req_id, e2e_nonce, payer_pubkey)`) and its own compact record<br>• answer the gateway's CHAL with the existing responder (PR2; `route_id` in the `req_id` slot) | Lua | P1 |
| PR11 | FRAG / FRAG_ACK in the relay, reusing R7 from Part 1 | Lua | P1 |
| PR12 | Routing attack scenarios (§3.4) | Lua + Python | P1 |
| PR13 | Relay status screen: routes forwarded, fees earned (HACK balance delta over RPC). Gives the judge something to see on a relay | Lua | P2 |

## 3. Technical notes

### 3.1 Timing budget
The PROOF must arrive within `PRESENCE_DEADLINE_MS`, measured on the payer from `new_nonce`. Signing on the SE050 alone may take ~261 ms (unmeasured), which is why the deadline depends on where the key lives (00 §4). Check in the Solana OS source whether the ESP-NOW receive callback runs on the Lua task.
- **If it does:** sign directly in the callback, since the signing binding extends the Lua deadline.
- **If not:** queue the CHAL and sign on the next tick. Measure the added latency with the R4 harness.

### 3.2 REQ rebroadcast
Broadcast the REQ periodically (≈1 Hz) so a payer who opens Pay late still sees it. Stop on RESULT, local CANCEL or expiry. Use a fresh `req_id` for each new request and never reuse one. If two payers send CHAL for the same `req_id`, answer both; whoever pays first wins, and the merchant ends the request on the first confirmed RESULT.

### 3.3 Attack scenarios (each must show on the badge, and on the feed as blocked) — ✏️ Changed (routing)
| Scenario | Method | Expected | How it reaches the feed |
| --- | --- | --- | --- |
| Impostor merchant | PR4 badge | Payer red "unverified" | Payer `POST /feed/event` |
| Impostor using the real merchant's pubkey in its REQ | PR4 variant | REQ sig fails against the record → red | Payer `/feed/event` |
| Replayed PROOF | A spare badge re-sends an old PROOF for a new CHAL | `check_proof` → `bad_sig` → red "bad proof" | Payer `/feed/event` |
| Replayed REQ, payee absent | Re-broadcast an old REQ from another badge | No PROOF → amber "not present" (bank: blocked) | Payer `/feed/event` if cancelled |
| Tampered bank payload | Laptop proxy between payer badge and backend edits `amount_cents` after signing | Backend signature check fails → refused | Backend writes blocked row |
| Tampered Solana tx (compromised app) | Laptop serves a tx with different amount/recipient via `/badge/pending` (existing Attack page) | Firmware red MISMATCH vs REQ/record | Payer `/feed/event` |
| Tampered registry response | Proxy edits the `/registry` JSON | Issuer sig fails → red | Payer `/feed/event` |
| Unsigned backend call | `POST /bank/authorize` without a valid badge sig | Refused, logged. Pitch it honestly as "the bank's API refuses unsigned payments", not as calling Nessie directly | Backend writes blocked row |
| Revoked payee | U revokes on the dashboard mid-demo | Payer red "revoked" | Payer `/feed/event` |

For the tamper proxy, point the payer badge's backend IP at the laptop proxy (mitmproxy or a 20-line relay) during the attack beat. Settings, or a Pay-app config, must allow changing that IP (coordinate with A/U).

✏️ Changed (routing): with all four badges in the relay chain there is no spare badge. The impostor runs as a **separate direct scene**: relay B switches to impostor mode (PR4) for that beat. *Previously: a dedicated impostor badge.* The virtual-badge emulator is **not** used.

### 3.4 Routing attack scenarios — 🆕 Routing
| Scenario | Method | Expected on the payer | How it reaches the feed |
| --- | --- | --- | --- |
| Unverified relay joins a route | Revoke relay A's attestation on the dashboard (or flash a badge that never had one), then route through it | Red "unverified hop"; no route through it is offered as valid | Payer `POST /feed/event` (reason `unverified_hop`) |
| Relay inflates its fee after quoting | Relay A edits the `fee` in its forwarded hop entry without re-signing | `route-att:` no longer verifies → red "broken chain" | Payer `/feed/event` |
| Compromised Pay app overpays a relay | A test build of the Pay app puts a higher fee leg in the transaction than the quote | Red "fee mismatch" | Payer `/feed/event` |
| Relay drops the transaction | Relay B swallows RPAY (test flag) | No RESULT. The transaction was never submitted, so **nobody is paid**. The payer shows "route failed" and retries: with one chain that means a new route, or falls back to direct | Payer `/feed/event` (reason `route_dropped`) when next online |
| Replayed route quote | Re-send an old RREP (an old `route_id` or old `quote_time`) | No payer slot for that `route_id` → red "broken chain"; old `quote_time` → red "stale quote" | Payer `/feed/event` |
| Relay swaps the next hop | Relay B attests a different `next_hop_pubkey` | Red "broken chain" | Payer `/feed/event` |

## 4. Dependencies
- P1-A `sign_request`, `sign_proof`; P2-A payer handshake and `/feed/event` posts; U's registry, feed, revoke and Attack page (P2-U).

## 5. Done when — ✏️ Changed (routing)
- [ ] Merchant badge request → judge badge green → paid → merchant badge shows PAID (verified on chain) within 5 s on the Solana rail, on 10 consecutive runs. Approval time doesn't count; switch the RPC to a provider if public devnet rate-limits.
- [ ] Every scenario in §3.3 produces the expected result on both the badge and the feed.
- [ ] Deadline value fixed and documented in 00.
- [ ] 🆕 2-hop chain (judge → relay A → relay B → merchant): the quote arrives in ≤ 4 s, and the payment and both fee legs confirm in one transaction. 5 runs in a row. The gateway posts `/feed/route` and the dashboard shows both relays' earnings.
- [ ] 🆕 Every scenario in §3.4 produces the expected result on both the badge and the feed.
- [ ] 🆕 With routing code loaded, the direct flow (§5 first item) still passes (fallback intact).

## 6. Pending decisions (U finalizes in U6) — ✏️ Changed (routing)
- Typed amount vs preset price list.
- Attack console ownership (existing web Attack page vs standalone).
- ✏️ Badge roles in the demo. Default: badge 1 merchant, badge 2 relay B (gateway; impostor for the direct scene), badge 4 relay A, badge 3 judge payer. *Previously: merchant vs impostor only.*
- 🆕 Relay fee defaults and the hop limit (P1-U §5).
- Demo beat 5 format (team decision, later).
