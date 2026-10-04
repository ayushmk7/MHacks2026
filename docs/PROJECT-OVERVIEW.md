# Project Overview — How the System Fits Together

A plain-language guide to the project as it stands after the 2026-10-04 decisions (Tier 0 app-level routing, Capital One as identity + settlement + funding, no further OS changes). For exact byte formats and routes, follow the links at the end.

---

## 1. The idea in one paragraph

Payment fraud rarely breaks cryptography — it fools people. You pay the wrong person (an impostor using a real business's name) or approve something different from what you think. Our badge is a **hardware security key for payments**. Before any money moves, it checks three things inside firmware that no app can override: **who** you're paying (Capital One has verified them), whether they're **present** (their badge answers a timed radio challenge), and **what** you're signing (it decodes the real transaction). Only then does a button press sign. Payments move in **HACK on Solana**, can hop across a **mesh of relay badges** to reach the network, and merchants can **settle into their Capital One account**.

> *Your badge won't let you pay anyone Capital One hasn't verified, shows you exactly what you're signing, and the payment can hop across a mesh of badges to settle on Solana and land in the merchant's bank.*

---

## 2. The cast

**Badges (4).** Identical hardware and firmware. A badge's role comes from which app it runs and what the backend has recorded about it:

- **Merchant** ("MHacks Merch") — runs the Request app; sets a price and broadcasts a signed payment request.
- **Judge** — runs the Pay app; the payer.
- **Relay A** and **Relay B** — run the Relay app; carry payments along. Relay B is the **gateway**: the last relay before the merchant and the one that talks to the internet.

Any badge can pay. Roles can be swapped by switching apps and changing one record on the dashboard.

**The laptop backend.** One Node server with two doors: an admin door (localhost only) for the dashboard, and a badge door (on the phone hotspot) that badges call. It holds Capital One's issuer key, the Nessie key, and a TimescaleDB database.

**Solana devnet.** Where HACK lives, where payments settle, and where the registry of verified payees lives (Solana Attestation Service).

**Capital One (Nessie).** The mock bank: customers, accounts, the MHacks Merch merchant, deposits and withdrawals.

---

## 3. Two kinds of keys (the heart of the trust model)

**Badge keys** — one per badge, created on the badge, never leaving it. They prove "this device":
- the **judge's** key signs payments (only after the hardware checks pass and a button is pressed);
- the **merchant's** key signs payment requests and answers presence challenges.

**The issuer key** — Capital One's, on the backend. It proves "this device belongs to MHacks Merch" by signing one record per verified payee. It never signs payments and never touches a badge key.

Think of it like websites and certificate authorities: the device proves it's the device; Capital One vouches whose device it is; your badge checks both before you sign. Without the badge keys the bank's statement would mean nothing — an impostor can claim the name but can't sign with the merchant's key.

---

## 4. Capital One's three roles

1. **Identity (the core role).** Only someone with a Capital One account can be verified. Their record on Solana carries a salted hash of that account, signed by the bank's issuer key. "✓ verified" on the badge means "the bank knows this business". Revoking the record turns them red everywhere within ~30 seconds.
2. **Settlement.** When verified, a merchant chooses to **receive HACK** (into its own wallet) or **settle to Capital One** (payments go to the bank's settlement account on Solana; the backend then deposits the same amount in dollars into the merchant's Nessie account).
3. **Funding.** Judges **top up** HACK from their Capital One account (a Nessie withdrawal, HACK sent to the badge).

The bank never moves money on its own initiative — only after a badge-signed payment has landed on chain.

---

## 5. HACK

HACK is our own token on Solana (2 decimals, like dollars and cents), standing in for a stablecoin; swapping it for USDC would be a configuration change. It is a currency in its own right: badges pay each other in HACK, relays earn HACK. Capital One is where it comes from (top-up) and where merchants can cash out (settlement). Because Nessie only stores whole dollars, demo prices are whole HACK amounts.

---

## 6. A direct payment, step by step

1. The merchant sets 40 HACK; its badge signs a **request** with its own key and broadcasts it over the radio (ESP-NOW).
2. The judge's Pay app lists it and fetches the merchant's **record** from the backend (signed by Capital One's issuer key).
3. The judge's badge sends a **fresh random challenge**; the merchant's badge signs it and answers within a few hundred milliseconds — proof it's the real device and it's right here.
4. The judge's **firmware** checks everything itself: the issuer's signature on the record, the merchant's signature on the request, the presence answer, and the decoded transaction (right recipient, exactly 40.00, real HACK). Screen: **"MHacks Merch ✓ verified ● present · 40.00 HACK"**.
5. The judge presses SELECT; the badge signs; the payment lands on Solana.
6. The merchant's badge confirms it on chain and shows **PAID ✓**. The backend confirms it too, records it, and — if the merchant settles to Capital One — deposits $40 into the merchant's account. The dashboard shows both.

If anything is off: an impostor (no record) → **red**; a tampered amount → **red MISMATCH**; a revoked merchant → **red**; nothing can be signed.

---

## 7. A routed payment through the relay mesh

The judge is **offline** (by policy: still on the hotspot so the radios share a channel, but making no internet calls). The payment reaches the internet through the relays.

1. The merchant broadcasts its signed request; relays pass the **exact same signed bytes** toward the judge. The judge sees **"MHacks Merch · 2 relays"**.
2. The judge asks, through the relays, for the merchant's issuer-signed record and a recent Solana blockhash; the gateway fetches them and sends them back (split into small radio pieces).
3. The judge's firmware runs the same checks as a direct payment. The payment is still verified — right merchant, right amount — but it can't be "present" across relays, so the screen is **amber, "not present"**. The judge presses SELECT.
4. The signed payment hops to the gateway; each relay adds itself to the list of hops.
5. The gateway submits it to Solana and reports the route to the backend.
6. The backend confirms the payment on chain, **rewards each verified relay 0.01 HACK** from the treasury, settles to Capital One if applicable, and draws the route on the dashboard. The merchant shows **PAID ✓**; the judge sees **"confirmed"**.

**Why relays can't cheat:** the request is signed by the merchant, the record by Capital One, and the transaction is checked and signed by the judge's own badge. A relay can delay or drop messages, but cannot change who gets paid or how much.

**What's weaker than a direct payment:** no presence proof (amber, by design — relaying and "nearby" contradict each other); relays' participation is reported unsigned and checked by the backend; the judge's "confirmed" comes from the gateway (the merchant's PAID ✓ is the authoritative one).

---

## 8. What lives where

| Layer | What it's responsible for | Status |
| --- | --- | --- |
| **Badge OS core (C)** | Keys; checking records, requests, presence; decoding transactions; the approval screen; signing | Built; **frozen** (no more OS changes) |
| **Badge apps (Lua)** | Pay (direct + routed), Request (merchant), Relay (relay/gateway), a shared routing library, Home | Direct apps built; routing apps being built by the OS/app agent (spec P3) |
| **Backend (Node)** | Issuer key and payee registry; signed records for badges; confirming payments on chain; relay rewards; Capital One settlement and top-ups; Nessie; database | Core built; routing + settlement + top-up in progress |
| **Dashboard (React)** | Issuer page (verify/revoke payees), payment feed, badges, routes and relay leaderboard, top-ups | Core built; routes, settlement and top-up views in progress |
| **Solana devnet** | HACK token, treasury / settlement account, verified-payee registry, every payment | Set up |
| **Capital One (Nessie)** | Customers, accounts, merchant, deposits (settlement), withdrawals (top-up) | Seeded |

**In the repo:**
- `docs/specs/` — the specs. `00-Interfaces.md` is the shared contract; `P3-demo-apps-tier0.md` is the current plan for the badge apps; `PROJECT-OVERVIEW.md` (this file) is the big picture.
- `dashboard/` — backend (`server/`), dashboard (`web/`), database (`db/`), scripts (`scripts/`, e.g. `nessie:seed`).
- `harness/` — R's test tools (verifiers, fuzz set, timing and signing tests).
- `os/` (on the `badge-os` branch) — the badge firmware and apps.
- `temp_firmware/` — R's simplified stand-in firmware used for early harness tests.

---

## 9. What's on chain vs in the database

**On Solana (public, permanent):** the HACK token and balances; every payment; relay rewards; the verified-payee registry — for each payee, a record saying "this badge key is MHacks Merch, a merchant, paid to this wallet, with this bank-account hash", signed by Capital One's issuer.

**In the backend database (private):** which badge is which; the plaintext Nessie IDs and the salts behind the bank hashes (so bank account numbers never appear on chain); every payment the backend confirmed, refused, routed or settled; relay routes and rewards.

---

## 10. The demo

1. **Verify** — on the Issuer page, Capital One verifies MHacks Merch.
2. **Top up** — the judge tops up HACK from their Capital One account.
3. **Pay** — green "✓ verified ● present", PAID ✓; the dashboard shows the payment and the Capital One settlement.
4. **Impostor** — a fake "MHacks Merch" is red; nothing to approve.
5. **Through the mesh** — the judge goes offline and pays through two relays; amber "via relays"; it lands; relays earn; the route lights up.
6. **Revoke** — Capital One revokes the merchant; the next attempt is red.

---

## 11. Decisions that shaped this (and what we gave up)

- **No more OS changes.** Everything new is apps, backend and configuration. Gave up: the full spec routing (signed hop proofs, payer pays relay fees in the same transaction) and the badge-signed bank authorization.
- **Tier 0 routing.** A routed payment is a normal payment whose radio messages hop through relays. Gave up: presence on routed payments; cryptographic proof of relay work.
- **Capital One as identity, settlement and funding** instead of a separate bank payment rail. Gave up: the bank-specific authorization screen ("via Capital One") and badge-signed bank payloads (`/bank/authorize` stays built but unused).
- **Relays rewarded by the network** (treasury) rather than paid by the payer.

Still strong: who you pay and what you pay are verified by the badge's hardware core on every payment, direct or routed.

---

## 12. Known limitations (say them if asked)

Routed payments are never "present"; relay participation is checked by the backend, not signed; keys are software keys (the secure element was unreliable); the badge listener has no authentication (devnet only); the relay chain is set up in software because every badge is in radio range on a table.

---

## 13. Glossary

- **ESP-NOW** — the badges' direct radio link (no Wi-Fi network needed); badges must be on the same Wi-Fi channel to hear each other.
- **REQ / CHAL / PROOF / RESULT** — the radio messages of a payment: the merchant's request, the payer's challenge, the merchant's signed answer, the outcome.
- **Record** — the issuer-signed statement about a payee that badges verify before paying.
- **Attestation (SAS)** — the on-chain form of that statement (Solana Attestation Service). Revoking = closing it.
- **Issuer** — Capital One's key that signs records.
- **Gateway** — the relay that's online and submits routed payments.
- **Treasury / settlement account** — the issuer's HACK account: source of top-ups and relay rewards, and where settle-to-bank payments land.
- **Green / amber / red** — the approval screen: all checks pass / verified but not present (routed or slow) / something is wrong, signing blocked.
- **Devnet** — Solana's free test network; nothing here is real money.

---

**Go deeper:** [`docs/specs/00-Interfaces.md`](specs/00-Interfaces.md) (formats and routes) · [`docs/specs/P3-demo-apps-tier0.md`](specs/P3-demo-apps-tier0.md) (badge apps plan) · [`docs/specs/Prd-verified-payment-key.md`](specs/Prd-verified-payment-key.md) (original product spec) · [`dashboard/README.md`](../dashboard/README.md) (running the backend and dashboard)
