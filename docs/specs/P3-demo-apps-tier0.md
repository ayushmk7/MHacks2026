# P3 — Demo Apps (Tier 0): Verified Payments and the Relay Mesh, App Level Only

Owner: **A (OS/app agent)** · Status: **active, this file wins over older app plans** · Decided by U, 2026-10-04
Reads: [`00-Interfaces.md`](00-Interfaces.md) §3–§5, §7, §8.1 · Supersedes for the demo: the routing items in [P2-A](P2-a-payer-flow.md) (PA11–PA19) and [P2-R](P2-r-payee-presence-attacks.md) (PR8–PR13), and 00-Interfaces §9 (that design is "Tier 2", not being built)

---

## 0. North star — read this first

We are building **one demo**, not a platform. Every hour of work must make one of the beats in §2 work on real badges, or make it more reliable. If a task does not serve a beat, do not do it.

**The product in one sentence:** *your badge won't let you pay anyone Capital One hasn't verified, it shows you exactly what you are signing, and the payment can hop across a mesh of badges to settle on Solana.*

**The three guarantees the demo exists to show** (all already enforced by the OS core — protect them, don't rebuild them):

1. **Who** — the payee's issuer-signed record verifies (Capital One's issuer key `2SXh6X…`). Impostor → red.
2. **Present** — direct payments: the payee's badge answers a timed challenge. Routed payments are honestly **amber, "not present"**.
3. **What** — the OS decodes the transaction and shows the real recipient and amount. Tampered → red.

**Hard rules**

- **No OS / C changes.** Everything in this file is Lua apps, Lua libraries, and badge configuration (provisioning values). If something turns out to need an OS change, **stop and report it** with the smallest change that would unblock it — do not make it.
- **Solana rail only.** No bank rail on the badge (`begin_bank` stays unbuilt). Capital One is handled entirely by the backend (§9).
- **The direct payment flow must keep working at every commit.** It is the fallback demo.
- **Freeze everything not listed here** (§11). No new features, screens, games, settings pages, store work, themes, or tooling beyond what a gate needs.
- **Report with evidence levels**: SEEN ON BADGE / HOST TEST / CODE ONLY / NOT BUILT.

---

## 1. Fixed values

| Name | Value |
| --- | --- |
| Cluster | Solana **devnet** only |
| Issuer key (`issuer_key`) | `2SXh6Xng9b1qQBCEn3pt2Ucwcb4ibBWwBKuuTwNgEzJy` — provision with `--issuer`, never read a keypair file |
| HACK (`tokens`) | `3VmWnzfWTfS5UGkwEjbMZnpjd1DPtKyMcwKeJc4QM9wP:2:HACK:<cap>:<max>` — classic Token program, 2 decimals |
| SAS credential / schema (backend only) | `6CZjxuTd9sStV46fdi1moo44m6QxXBFBKkdXYk59phrU` / `Ah14pDiYtnF8osNeh63LivqftenHmtdYerKJJSTsZ5hY` |
| Backend badge listener | `http://<U's laptop hotspot IP>:8788` (U supplies the IP) |
| Network | One 2.4 GHz phone hotspot for **all** badges (iPhone: "Maximize Compatibility") |
| `record_ttl_s` | **60** on every badge (routed payments need the headroom) |
| Presence deadline | keep ≥ 400 ms (measured 234–251 ms with Wi-Fi off; Wi-Fi on is unmeasured) |
| Demo prices | whole HACK amounts (e.g. 40.00) |

---

## 2. The demo (what "done" means)

| # | Beat | What the audience sees | Priority |
| --- | --- | --- | --- |
| D1 | **Direct verified payment** | Merchant badge (Request app) asks 40 HACK → judge badge (Pay app) shows **"MHacks Merch ✓ verified ● present · 40.00 HACK"** → SELECT → merchant shows **PAID ✓** → dashboard shows the payment | **P0** |
| D2 | **Impostor** | A badge without a record claims "MHacks Merch" → judge sees **red, unverified**; nothing can be signed | **P0** |
| D3 | **Tampered amount** | Merchant says 5, transaction moves 500 → **red MISMATCH** (existing attack console / `/badge/pending`) | P1 |
| D4 | **Revocation** | U revokes the merchant on the dashboard → next attempt is **red** within ~30 s | P1 |
| D5 | **Routed payment through the mesh** | Judge, offline by policy, sees **"MHacks Merch ✓ · via 2 relays"**, screen is **amber "not present"** → SELECT → payment hops relay A → relay B (gateway) → lands on devnet → merchant **PAID ✓**, judge **"confirmed"**, relays show **"+0.01 HACK"**, dashboard draws the route | **P1** (the key differentiator) |
| D6 | **Merchant live through the mesh** (stretch) | Routed payment also shows the merchant's device signed the judge's fresh challenge (untimed) | P2 |

Everything in this file serves D1–D6. D1 + D2 must work before routing work starts landing on the demo badges.

---

## 3. Badges and roles

All four badges run the same firmware. Roles come from **which app is open + the backend's records** (U issues attestations). Any badge can pay.

| Badge | App | Role |
| --- | --- | --- |
| Merchant | Request | Verified payee "MHacks Merch" |
| Judge | Pay | Payer (direct and routed) |
| Relay A | Relay (relay mode) | Forwards frames, earns rewards |
| Relay B | Relay (gateway mode) | Last hop, online: fetches records/blockhash, submits transactions, reports to the backend |

Chain for D5: **Judge ↔ Relay A ↔ Relay B ↔ Merchant**, enforced in software by each app's neighbour list (§6.4). Every badge is physically in range on the table; say so if asked.

---

## 4. Gates (do them in order; each needs evidence before the next)

| Gate | Done when | Notes |
| --- | --- | --- |
| **G0 Radio + Wi-Fi** | U's test: ESP-NOW REQ arrives with (a) one badge on the hotspot, one off; (b) both on. Presence time measured with Wi-Fi on. Result recorded | Decides whether "offline payer" means truly disconnected or "on the hotspot, no HTTP" (default assumption: the latter) |
| **G1 Provisioned** | All 4 badges provisioned with §1 values; each badge's full pubkey + port + physical label reported to U; SNTP synced on the hotspot (`time_ok()` true) | No full erase from here on — keys must stay stable |
| **G2 Backend reachable** | Each badge gets `{ok:true}` from `GET /health` and a 200 from `GET /registry/<merchant pubkey>` (after U attests it) | |
| **G3 = D1** | Real Pay app → real Request app, green, devnet transaction, merchant PAID, `POST /feed/solana` returns `{ok:true}` | First real payment. Explorer link to U |
| **G4 = D2 (+D3, D4)** | Impostor red; tamper red; revoke → red | |
| **G5 Routed, 1 relay** | Judge → Relay B (gateway) → Merchant, 5/5 successful payments | 3 badges |
| **G6 = D5** | Judge → Relay A → Relay B → Merchant, judge offline by policy, 5/5 within the blockhash window | 4 badges |
| G7 = D6 | Forwarded challenge/proof shows merchant-live | Only if §8 Q8–Q9 come back yes |

---

## 5. Answer these before writing routing code (feasibility — no OS changes allowed)

Answer each from code/badge evidence. If any of Q1–Q4 is "no", stop and report: Tier 0 needs a redesign before building.

1. Can a Lua app **receive the merchant's REQ (type 1) and get its raw signed bytes**? If the firmware consumes type 1, can the merchant's Request app read its own REQ bytes (from `request_open`) and send them in a custom frame?
2. Can Lua **send and receive custom frame types** in the block chosen for routing (§6), see the **sender MAC**, and send **unicast to a chosen MAC** (and broadcast)? Use whatever envelope Duel (types 64–68) uses.
3. Does `begin_solana` (or the Pay app's wrapper) accept a **record + sig and a blockhash that did not come from HTTP**? The record is issuer-signed, so its source should not matter.
4. Can the judge press **SELECT on amber** for a Solana payment?
5. Can the Pay app build the payer's transfer **with no network** (payer's own HACK source token account known/derivable; `build_transfer` takes an external blockhash)?
6. Does the merchant's payment confirmation (`vk_payment` / `vk.receive`) accept payment to **the wallet in its own record** (`record.solana_ata`), not only its own token account? (Merchants may settle to a Capital One settlement wallet, §9.)
7. Can `record_ttl_s` be set to 60 by provisioning?
8. (D6) Can a relay app receive and forward **CHAL (type 2) and PROOF (type 3)** frames, will the merchant's OS answer a CHAL that arrived from a relay's MAC, and will the payer's OS accept a PROOF that came back via a relay?
9. (stretch) Can a relay app run `wallet.challenge` / `wallet.presence` against its neighbour outside a payment?

---

## 6. Routing protocol (Tier 0, all Lua)

### 6.1 Principle

A routed payment is **a normal direct payment whose radio messages hop through relays**. The judge's OS verifies the merchant's issuer-signed record and the merchant-signed REQ, decodes a normal **single `transferChecked` + memo** transaction, and signs — exactly as for D1. Relays only carry bytes; **nothing they do can change the recipient or amount**, because the REQ is merchant-signed, the record is issuer-signed, and the transaction is checked by the payer's OS. Relays are paid by the backend (§9), not in the payer's transaction.

### 6.2 Frames

Use the same envelope as Duel. Suggested types (pick a free block; record the choice in `docs/os/`):

| Type | Name | Direction | Payload (little-endian) | Size |
| --- | --- | --- | --- | --- |
| 70 | `FWD` | merchant side → payer side | `msg_id[4] · ttl u8 · hops u8 · req_len u8 · req[req_len]` (the merchant's **original signed REQ bytes**, untouched) | ≤ 168 B |
| 71 | `CTX_ASK` | payer → gateway | `msg_id[4] · ttl u8 · req_id[8] · payee[32] · payer[32]` | 78 B |
| 72 | `CTX` | gateway → payer (chunked) | `rec_len u16 · record · sig[64] · blockhash[32] · last_valid_height u64` | ≈ 520 B |
| 73 | `TX` | payer → gateway (chunked) | `req_id[8] · tx_len u16 · tx · n_hops u8 · hop_pubkey[32] × n_hops` (each relay appends itself) | ≈ 420 B |
| 74 | `RESULT` | gateway → payer side, and → merchant | `msg_id[4] · req_id[8] · status u8 (0 ok, 1 refused, 2 failed) · tx_sig[64]` | 77 B |
| 75 | `CHUNK` | either | `msg_id[4] · inner_type u8 · idx u8 · total u8 · data[≤ 213]` | ≤ 224 B |
| 76 | `ACK` | either | `msg_id[4] · idx u8` | 5 B |

- **De-duplicate** on `msg_id` (keep the last ~64 for `ROUTE_STATE_TTL_S` = 90 s). Decrement `ttl` per hop; drop at 0 (start at 3).
- **Chunking:** split at 213 B; ACK each chunk; resend un-ACKed chunks up to 2 times, 500 ms reassembly timeout (00 §9.8 values). Keep it simple — no windowing.
- `FWD` and `RESULT` are sent toward the payer side; `CTX_ASK` and `TX` toward the gateway side. Each relay knows which neighbour is on which side (§6.4).

### 6.3 Who does what

| Badge | Behaviour |
| --- | --- |
| **Merchant** (Request app) | Unchanged: opens and broadcasts its signed REQ, confirms payment on chain, shows PAID. Only addition if Q1 requires it: also emit its REQ bytes in a `FWD` frame |
| **Relay** (Relay app, relay mode) | Accept frames only from its two configured neighbours. Forward `FWD`/`RESULT` toward the payer side, `CTX_ASK`/`TX` toward the gateway side. Append own pubkey to `TX`. Status screen: routes carried, HACK earned (HACK balance delta via RPC when online) |
| **Gateway** (Relay app, gateway mode, online) | Hear the merchant's REQ directly → wrap as `FWD` and send toward the payer side. On `CTX_ASK`: `GET /registry/<payee>` on the backend + `getLatestBlockhash` on devnet RPC → send `CTX` (chunked). On `TX`: `sendTransaction`, poll confirmation, `POST /feed/route` (or `/feed/solana` until U ships `/feed/route`), send `RESULT` toward the payer side **and** to the merchant |
| **Judge** (Pay app, routed mode) | List `FWD` requests as "Name · N relays" (name shown as a claim until the record verifies). On SELECT of a routed request: send `CTX_ASK`, wait for `CTX`, build a normal transfer to `record.solana_ata` with memo = hex `req_id` and the provided blockhash, call `begin_solana` with `{record, record_sig, req}`, poll, send `TX`, show **"submitted"**, then **"confirmed"** on `RESULT`. **No HTTP or RPC calls in routed mode** |

### 6.4 Neighbour configuration

A small app config file per badge (not an OS config key): `{ role = "relay"|"gateway", payer_side = <pubkey/MAC>, gateway_side = <pubkey/MAC> }`. The judge's Pay app has `first_hop = <Relay A>`. The gateway's `gateway_side` is the merchant. Frames from anyone else are ignored by routing code (direct-flow frames are untouched).

### 6.5 Timing

The whole routed payment — `CTX_ASK` → `CTX` → approval → `TX` → submit — must fit the ~60 s blockhash life. Fetch the blockhash only in response to `CTX_ASK` (i.e. right before approval); keep the routed approval timeout at 30 s. Measure each leg and report.

---

## 7. Presence

- **Direct (D1):** unchanged — the OS's timed challenge; green "● present".
- **Routed (D5):** the payer's OS will mark presence late/absent → **amber "not present"**. This is correct and honest: relaying and "nearby" contradict each other. Do not try to make routed payments green.
- **D6 (stretch, only if Q8 = yes):** forward the payer's CHAL through the relays to the merchant and the PROOF back. The payer's OS verifies the merchant's signature (still "late" → amber). Show "merchant device answered ✓" in the Pay app result and include `{e2e_proof:true}` in the gateway's report.

---

## 8. Backend contract (U builds anything marked "new")

| Route (badge listener :8788) | Shape | Status |
| --- | --- | --- |
| `GET /health` | → `{ok:true}` | exists |
| `GET /registry/:pubkey` | → `{record, sig}` (base64 canonical `registry:` record, issuer signature) · 404 = unverified | exists — the badge parser accepted it in the compat test |
| `POST /feed/solana` | `{tx_sig, req}` (req = base64 REQ frame) → `{ok, reason?}` | exists |
| `POST /feed/event` | `{payer_pubkey, req?, reason}` → `{ok}` | exists |
| `GET /route/ctx/:payee` | → `{record, sig, blockhash, lastValidBlockHeight}` (one call for the gateway) | **new** — until it exists, use `/registry` + RPC |
| `POST /feed/route` | `{tx_sig, req, hops:[base58 pubkey…], e2e_proof?:bool}` → `{ok, reason?, rewards?:[{pubkey, state}]}` (state `pending` or `none`; rewards are sent asynchronously) | **new** — backend confirms on chain, pays each attested relay 0.01 HACK, settles to Capital One if applicable, draws the route |

Refusals are HTTP 200 with `{ok:false, reason}`.

---

## 9. What the badges do NOT do (backend's job — don't build it on the badge)

- **Capital One identity:** U verifies payees on the dashboard; only Capital One account holders get a record. The badge just checks the record.
- **Settlement to Capital One:** a merchant's record may name a Capital One **settlement wallet** instead of the merchant's own; the payer simply pays `record.solana_ata` (never derive the recipient from the merchant pubkey). The backend deposits dollars to the merchant's Nessie account after it confirms the payment.
- **Relay rewards:** paid by the backend from the treasury after `/feed/route`.
- **Top-ups:** done from the dashboard.

---

## 10. Known limitations (state them honestly in UI copy and to the team)

- Routed payments are never "present" (amber).
- Relay participation (hop list) is unsigned app data; only rewards depend on it.
- The judge's routed "confirmed" comes from the gateway; the merchant's PAID ✓ (its own chain check) is the authoritative one.
- Keys are software keys (SE050 quarantined).
- The relay chain is enforced in software because all badges are in range.

---

## 11. Frozen (no further work; keep out of the demo unless already working and harmless)

App store / publisher signing / permissions UI beyond what exists · on-badge setup pages · themes · games (Duel, Game shop, Dice) · Contacts · on-screen keyboard · battery/sleep/dim · daily limit · signature log v2 · 8-suite Tests app · Inbox · QR/barcode · LED language beyond PAID/refused · bank rail · 00-Interfaces §9 Tier 2 features (FRAG per spec, RREQ/RREP flooding, route quotes, `route-att`, `registry-c` parsing, routed decoder, fee legs).

Hide frozen apps from the launcher for the demo badges if they could confuse a judge.

---

## 12. Reporting

After each gate: a short table — what was run, on which badges (label + pubkey prefix), evidence level, Explorer links for any transaction, timings, and anything that needs U (keys, attestations, funding, backend routes). Record any deviation from this file in `docs/os/reference/differences-from-specs.md`.
