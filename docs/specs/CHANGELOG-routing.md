# Changelog — relay network (DePIN) update

Date: 2026-10-03 · Scope: `docs/specs/` only · Code, keys and `docs/os/` untouched

**Why:** the MLH Solana judge wants Solana used beyond plain payments (DePIN, x402, MetaDAO) and liked the hands-on badge demo. We add a relay network: badges carry payments hop by hop to a destination, and each relay earns a HACK fee. **Nothing already specified is removed.** Direct payment stays fully specified and is the fallback demo.

In the specs, new sections are tagged **🆕 Routing** and edited sections **✏️ Changed (routing)**, with a *Previously:* note wherever a decision changed.

## Per file

### `00-Interfaces.md`
- **§0 Decisions:** added routing decisions (additive network, routed presence semantics, self-carried compact records, fragmentation, `kind=relay`, 4-badge demo chain, x402-style framing) and the clock change.
- **§2 Constants / §4 `time_ok()`:** `time_ok()` is now true only after an SNTP sync since boot. *Previously, a verified record could also set it, which made freshness checks circular.* This is also needed so an offline payer keeps valid time.
- **§3 Prefixes:** added `route-att:`, `route-quote:`, `registry-c:`. `pay-proof:` reused on a route, with `route_id` in the `req_id` slot. The collision argument now covers `r` (0x72 = 114 signers → 3,648 B > 1,232 B).
- **§4.1 (new) Routed wallet API:** `new_route`, `check_route`, `begin_route_solana`, `begin_route_bank`, `sign_route_att`, `sign_route_quote`, `check_crec`. Firmware checks Q1–Q7. Routed screen states (green "via N relays", amber "a hop was slow", red "unverified hop" / "broken chain" / "fee mismatch" / "stale quote"). Second confirmation above the fee cap.
- **§5:** pointer to the new message types 5–10. RESULT forwarded hop by hop on a route.
- **§6 Bank payload:** new last lines `route_id` and `route_fee_total`, always present (`none`/`0` when direct). *Previously the payload ended at `issued_at`.*
- **§7 Registry:** `kind` gains `relay`; one relay attestation per verified operator.
- **§8 Endpoints:**
  - new `GET /registry/:pubkey?format=compact` and `POST /feed/route`;
  - optional `route` block on `/bank/authorize`;
  - admin `/api/routes` and `/api/relays`;
  - SSE `route` events.
- **§9 (new) Routing:** roles; the full flow; compact record format; RREQ / RREP / FRAG / RPAY / REQ_FWD / FRAG_ACK byte layouts with sizes; signed formats; fee formula; route selection (equivalent to Dijkstra on small graphs; the Lightning-style backward Dijkstra over gossip is the scaling path, never on the backend); atomic settlement; bank over the route; constants; timing budget; demo topology.

### `P1-a-firmware-wallet-core.md`
- **A8:** new clock rule.
- **A11:** presence slots keyed by (id, peer), 8 slots. *Previously: 4 slots keyed by `req_id`.*
- **New A14:** routing-ready decoder (a list of legs) and screen layout.
- **§4.5:** route prefixes refused in `begin_*`.
- **New §4.6:** routing hooks.
- **Done-when:** two new checks.
- **Why:** routing is built in Part 2. Part 1 only avoids a rewrite later.

### `P2-a-payer-flow.md`
- **New §2.1, PA11–PA19:** routed Pay list, route discovery, routed decoder, route verification in C, routed screen, `begin_route_solana`, routed bank (stretch), FRAG, the payer's cached source ATA.
- **New §3.4:** building the routed transaction (account order, leg order, quoted blockhash, sizes).
- **Done-when:** 2-hop runs, one red state per tamper, cheapest-route enforcement, direct fallback still passing.

### `P1-r-test-harness.md`
- **Unknowns:** a fifth unknown ("can ~1.3 KB cross two radio hops?").
- **New R7:** FRAG prototype and multi-hop radio test, with a pass bar in §4.4. It sits in Part 1 because it's the riskiest routing piece.
- **New R8:** routing test vectors (compact records, `route-att`, `route-quote`, full RREP, transactions with K = 1–3 fee legs).
- **Interfaces, done-when, risks:** updated to match.

### `P2-r-payee-presence-attacks.md`
- **Title and purpose:** now include the relay network.
- **New §2.1, PR8–PR13:**
  - relay app;
  - gateway role;
  - destination support in the Request app;
  - FRAG;
  - routing attacks;
  - relay status screen.
- **§3.3:** the impostor is now a separate direct scene, run by relay B. *Previously: a dedicated impostor badge.* The virtual-badge emulator is not used.
- **New §3.4, routing attacks:**
  - an unverified relay joins;
  - a relay inflates its fee;
  - the Pay app overpays;
  - a relay drops the transaction;
  - a replayed quote;
  - a swapped next hop.
- **Done-when and pending decisions:** 2-hop criteria, badge-role mapping.

### `P1-u-economies-registry-app-spec.md`
- **New U8:** relay identity: `kind=relay` decided before the schema is created, relay attestations, Sybil policy, compact-record endpoint.
- **New U9:** relay funding. Relays need a HACK ATA, but no SOL.
- **§4.2:** `kind` values and the one-attestation-per-badge consequence.
- **§5 app decision doc:** relay app and routed Pay rows; the role question answered (4-badge chain); new questions 7–10 (hop limit, fee defaults, auto-approve cap, routed bank).
- **Done-when:** schema with the relay kind, relays attested, compact record verified.

### `P2-u-backend-dashboard-home.md`
- **Title and new items:**
  - PU10: `/feed/route` verification and earnings;
  - PU11: routed `/bank/authorize`, with the fee transaction submitted only after Nessie succeeds;
  - PU12: Routes view and relay leaderboard;
  - PU13: TimescaleDB route metrics (Tiger Data angle).
- **§3.2–3.5:** routed feed rows, dashboard additions, routed bank checks 10–12, `routes` / `route_hops` tables and aggregates.
- **§4 Demo:** 8 beats with the 2-hop relay beat; the judge holds the payer badge; the impostor via relay B; the "unverified relay" variant; drop beat 3 if routing isn't ready.
- **Done-when and pending decisions:** updated to match.

### `Prd-verified-payment-key.md` (master)
- **Overview:** relay network summary. Hardware row: the 4-badge chain. *Previously: 2 team badges and 2 judge badges.*
- **Thesis and positioning:** new subsection with:
  - the judge's feedback;
  - the DePIN concept;
  - the Lightning and Helium analogy, not miners;
  - the two fees;
  - why identity makes the incentives safe;
  - x402-style framing, with PlaiPin as prior art;
  - Say / Don't say lines;
  - path to production (emissions).
- **Goals and metrics:** goal 6; routed quote under 4 s; unverified relays refused.
- **Users:** a relay operator role; the attacker updated.
- **Guarantees:** the routed presence semantics and a routed screen mock-up; two new "not stopped" items.
- **User flows:** a routed-flow diagram and a routed-attack table.
- **Functional requirements:** FW9–FW11, AP9–AP11, BE6–BE7, DB5–DB6.
- **Architecture:** relay roles, routing on the payer, relays as untrusted-for-content.
- **Data model:** `kind` adds `relay`; `routes` and `route_hops` tables.
- **Protocols:** routed payments do cross ESP-NOW. *Previously: "never over ESP-NOW".*
- **Backend API:** marked stale, with a pointer to 00 §8.
- **Dashboard:** Routes view.
- **Track mapping:** Solana now covers identity, settlement, atomic routing fees and DePIN incentives. Optional Tiger Data row. (This also fixed a missing blank line after the table.)
- **Demo script:** 8 beats, one judge on the payer badge, the relay beat, the impostor via relay B; drop the relay beat if routing isn't ready.
- **Build plan:** routing in hours 14–20, in parallel with the bank rail, only after the direct gates. G3 gains a routing condition. New cut-order items.
- **Risks and limitations:** fragmentation loss, timing, team stretch; seven routing limitations.
- **Open questions and pre-event checklist:** updated to match.

### `README.md` (this folder)
Index descriptions updated, the changelog listed, the gate line extended, and the tag convention explained.

## Decisions made in this update

1. **Routing is additive.** Direct payment is unchanged and is the fallback demo.
2. **Routed presence:** the screen says "✓ verified · via N relays", never "● present". Presence is timed at every hop. The destination's proof over the payer's nonce is carried back signed but untimed. *(Confirmed by U.)*
3. **Clock:** `time_ok()` needs an SNTP sync since boot. An offline payer keeps that time. *(Confirmed by U.)*
4. **Records on the route:** each hop carries its own issuer-signed compact record (≤ 236 B). The payer needs no Wi-Fi.
5. **Fragmentation:** FRAG (type 7) + FRAG_ACK (type 10), 213 B payload per frame, at most 10 frames (2,130 B), whole-message retry. A 2-relay quote is 1,341 B (7 frames); a routed transaction is 438 B (3 frames).
6. **Relay identity:** `kind=relay` in `payee_v1`, with no new schema field. One relay attestation per verified operator.
7. **Gateway:** the last relay before the destination signs the quote (blockhash + time), submits, and reports.
8. **Fees:** `base + rate × amount` per hop. Defaults 0.01 HACK + 0.1 %, cap 0.10 HACK per hop, auto-approve up to 0.10 HACK total.
9. **Selection:** the cheapest valid route, enforced in firmware. Never computed on the backend.
10. **Settlement:** Solana is atomic (one transaction). On the bank rail, the HACK fee transaction is submitted only after Nessie succeeds.
11. **Timing:** live routing only. Quote TTL 30 s, routed approval 30 s, routed record freshness 60 s; direct values unchanged.
12. **Demo:** all 4 badges in one chain. The judge holds the **payer** badge. The chain is enforced with `DEMO_NEIGHBORS`. The impostor is relay B in impostor mode. No virtual badge. *(Confirmed by U.)*
13. **x402:** "x402-style" only, unless the gateway's HTTP side speaks real x402 headers.

## Open decisions (for U)

| Decision | Current default | Alternative |
| --- | --- | --- |
| Fragmentation vs compact hop entries | FRAG with self-carried compact records | Compact hop entries (8-byte ids + fee + signature, about 2 frames) with records fetched by the payer over Wi-Fi. Smaller, but needs an online payer. Use this if R7 misses its pass bar |
| Record freshness on a route | 60 s (direct stays 30 s) | 30 s everywhere: faster revocation, but relays must refresh more often |
| Fee defaults | 0.01 HACK + 0.1 %, cap 0.10/hop, auto-approve ≤ 0.10 total | Flat 0.01 HACK per hop: simpler to explain |
| Relay as `kind` vs a flag | `kind=relay` (a badge is a payee **or** a relay) | A separate `relay` flag field, so a merchant can also relay. This changes the schema and must be decided before it is created |
| Hop cap | 3 relays (demo uses 2) | 2 relays: smaller quotes and fewer frames |
| Routed bank payments | Stretch (P2-A PA17, P2-U PU11) | Solana-only routing |
| Tiger Data prize entry | Optional, via route metrics | Not entered |
| Demo beat 5 / tamper format | Still open from before | — |

## Not changed (known gaps)

- `docs/os/` (the badge OS design) was **not** touched, by instruction. Its wallet API (`badge.wallet.*`) and protocol don't include routing. The two document sets now differ further.
- The repository `README.md` outside `docs/specs/` was not touched.
- Several specs still give pre-restructure paths (`server/...`, `scripts/...`, `db/schema.sql`). Since the restructure these live under `dashboard/`. That fix is not part of this update.
