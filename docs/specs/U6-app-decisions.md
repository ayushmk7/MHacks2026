# U6 — App and Demo Decisions (final scope)

Owner: **U** · Decided 2026-10-04 · Status: **final for the demo** (change only by telling A and R)
Implements: P1-U §5 (the app decision doc) · Read with: [`../PROJECT-OVERVIEW.md`](../PROJECT-OVERVIEW.md) (big picture)

Where this file and an older spec disagree, **this file wins** for the demo. Older sections stay as the long-term design.

## 1. Scope decisions

| # | Decision | Consequence |
| --- | --- | --- |
| 1 | **No further OS (C) changes.** Everything new is Lua apps, backend and configuration | Dropped: `begin_bank` / check 5 (P2-A PA5–PA6), every routing item that needs C |
| 2 | **No relay routing.** 00-Interfaces §9, P2-A PA11–PA19, P2-R PR8–PR13, P3 (Tier 0) are all cancelled for time | Backend routing endpoints (`/route/ctx`, `/feed/route`) exist but are unused; the dashboard doesn't show routes |
| 3 | **Badges use the Solana rail only.** Capital One plays three backend roles: **identity** (only Capital One account holders can be verified), **settlement** (merchants may settle to Capital One; the backend deposits dollars), **funding** (top-ups) | `/bank/authorize` stays built but unused. The "via Capital One" approval screen is out |
| 4 | **Direct payments only**, the three guarantees: who (record), present (timed challenge), what (decoded transaction) | The whole demo is §4 below |

## 2. The P1-U §5 questions

| Question | Decision |
| --- | --- |
| Payer picks from a list or auto pop-up? | **List** of nearby requests |
| Who sets the amount? | **The payee** (merchant enters it in the Request app). Whole HACK amounts |
| Remote (not present) Solana payments? | Amber warning (no answer in time); SELECT allowed on Solana. The demo shows green |
| Typed amounts or preset price list? | **Typed amounts** |
| Which badge plays which role? | Merchant ("MHacks Merch", Request app), Judge (Pay app), Impostor (claims "MHacks Merch", no record), Spare |
| Audit memos for Solana payments? | No separate memo; the payment carries memo = hex `req_id`; the transaction is the audit trail |
| Attack console owner | Dashboard Attack page (tampered amount); impostor is a badge without a record |
| Bank payments | Not on the badge (decision 3) |
| Spending cap | Existing firmware cap (`cap` / `max` provisioning values); daily limit frozen |

## 3. Approval states the demo relies on

| State | When |
| --- | --- |
| Green "✓ verified ● present" | All checks pass |
| Amber "not present" | Payee didn't answer the challenge in time (SELECT allowed on Solana; not shown in the demo) |
| Red | No/revoked/expired record (impostor, revoked merchant), tampered amount or recipient, wrong token, clock not set |

## 4. Demo and gates

Demo: verify MHacks Merch → judge tops up → judge pays 40 HACK (green, PAID ✓, settlement shown if enabled) → impostor red → tampered amount red → revoke → red.

| Gate | Done when |
| --- | --- |
| G1 Provisioned | All 4 badges provisioned with issuer `2SXh6Xng9b1qQBCEn3pt2Ucwcb4ibBWwBKuuTwNgEzJy`, HACK `3VmWnzfWTfS5UGkwEjbMZnpjd1DPtKyMcwKeJc4QM9wP` (2 decimals), `record_ttl_s` 60, listener `http://<A's hotspot IP>:8788`; SNTP synced. No full flash erase — badges are funded at their current keys |
| G2 Backend reachable | Each badge: `GET /health` ok, `GET /registry/<merchant>` 200 |
| G3 First payment | Real Pay → Request apps, green, devnet transaction, merchant PAID ✓, `/feed/solana` ok |
| G4 Attacks | Impostor red, tampered amount red, revoked merchant red |

**Open item for A:** does the merchant's payment confirmation accept payment to the wallet in its own record (needed for settle-to-Capital-One)? Until confirmed, MHacks Merch is issued with "Receive HACK".
