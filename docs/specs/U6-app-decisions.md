# U6 — App and Demo Decisions

Owner: **U** · Decided 2026-10-04 · Status: **final for the demo** (change only by telling A and R)
Implements: P1-U §5 (the app decision doc) · Read with: [`../PROJECT-OVERVIEW.md`](../PROJECT-OVERVIEW.md) (big picture) and [`P3-demo-apps-tier0.md`](P3-demo-apps-tier0.md) (badge apps plan)

Where this file and an older spec disagree, **this file wins** for the demo. Older sections stay as the long-term design.

## 1. Scope decisions

| # | Decision | Consequence |
| --- | --- | --- |
| 1 | **No further OS (C) changes.** Everything new is Lua apps, backend and configuration | Spec items that need C are dropped or deferred: `begin_bank` / check 5 (P2-A PA5–PA6), routed decoder and route verification (PA13–PA15), `kind=relay` and `registry-c:` parsing on the badge, `route-att:` / `route-quote:` signing |
| 2 | **Routing = Tier 0** (P3 §6): a routed payment is a normal single-transfer payment whose radio frames hop through relays; the gateway submits it | Replaces 00-Interfaces §9 for the demo. The payer's OS verifies the merchant and amount exactly as for a direct payment |
| 3 | **Badges use the Solana rail only.** Capital One plays three backend roles instead of a separate bank rail: **identity** (only Capital One account holders can be verified), **settlement** (merchants may settle to Capital One; the backend deposits dollars), **funding** (top-ups) | `/bank/authorize` stays built but unused. The "via Capital One" approval screen is out |
| 4 | **Relays are rewarded by the network** (treasury), 0.01 HACK per verified hop, capped per route; rewards stay on chain (not booked in Nessie) | Relay fees are not in the payer's transaction |
| 5 | **Payer offline by policy** for routed payments: stays on the hotspot (radio channel) but makes no internet calls | Revisit after the G0 channel test if a disconnected badge stays reachable |

## 2. The P1-U §5 questions

| Question | Decision |
| --- | --- |
| Payer picks from a list or auto pop-up? | **List** of nearby requests; routed ones shown as "Name · N relays" |
| Who sets the amount? | **The payee** (merchant enters it in the Request app). Whole HACK amounts in the demo |
| Remote (not present) Solana payments? | **Allowed with amber warning** — required for routed payments. Direct payments without presence are also amber |
| Typed amounts or preset price list? | **Typed amounts** (no price list) |
| Which badge plays which role? | 1 merchant ("MHacks Merch", Request app), 1 judge (Pay app), relay A, relay B (gateway). Roles are swappable: app + attestation |
| Audit memos for Solana payments? | **No separate memo.** The payment itself carries memo = hex `req_id`; the transaction is the audit trail |
| Attack console owner | The existing dashboard Attack page (tampered amount); impostor is a badge without a record |
| Bank payments | Not on the badge. Capital One = identity + settlement + funding (decision 3) |
| Spending cap | Existing firmware cap stays (`cap` / `max` provisioning values); daily limit frozen |

## 3. Approval states the demo relies on

| State | When |
| --- | --- |
| Green "✓ verified ● present" | Direct payment, all checks pass |
| Amber "not present" | Routed payment, or a direct payment whose payee didn't answer in time. SELECT allowed (Solana) |
| Red | No/revoked/expired record (impostor, revoked merchant), tampered amount or recipient, wrong token, clock not set |

Open item for A: P3 §5 Q6 — the merchant's payment confirmation must accept payment to the wallet in its own record (needed for settle-to-bank merchants). Until confirmed, MHacks Merch stays on "receive HACK".

## 4. Frozen

See P3 §11. Nothing frozen appears in the demo.

## 5. Gates (replacing the README gates for the demo)

G0 radio/Wi-Fi channel test · G1 all four badges provisioned with the real issuer, keys reported · G2 backend reachable · **G3 first real direct payment** · G4 impostor / tamper / revoke · G5 routed with one relay · **G6 routed with two relays (offline payer)** · G7 merchant-live through the mesh (stretch). Details in P3 §4.
