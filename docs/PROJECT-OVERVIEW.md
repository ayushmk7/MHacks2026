# Project Overview — How the System Fits Together

A plain-language guide to the project in its **final demo scope** (decided 2026-10-04): direct payments between badges, Capital One as identity + settlement + funding, no further OS changes, no relay routing. For exact byte formats and routes, follow the links at the end.

---

## 1. The idea in one paragraph

Payment fraud rarely breaks cryptography — it fools people. You pay the wrong person (an impostor using a real business's name) or approve something different from what you think. Our badge is a **hardware security key for payments**. Before any money moves, it checks three things inside firmware that no app can override: **who** you're paying (Capital One has verified them), whether they're **present** (their badge answers a timed radio challenge), and **what** you're signing (it decodes the real transaction). Only then does a button press sign. Payments move in **HACK on Solana**, and merchants can **settle into their Capital One account**.

> *Capital One vouches for who you're paying, your badge proves they're standing in front of you and shows exactly what you sign, and the payment settles on Solana into the merchant's bank.*

---

## 2. The cast

**Badges (4).** Identical hardware and firmware. A badge's role comes from which app it runs and what the backend has recorded about it:

- **Merchant** ("MHacks Merch") — runs the Request app; sets a price and broadcasts a signed payment request. Has a Capital One–backed record.
- **Judge** — runs the Pay app; the payer.
- **Impostor** — a badge that claims to be "MHacks Merch" but has no record. Exists to be caught.
- **Spare** — a second judge or a backup.

Any badge can pay. Roles can be swapped by switching apps and changing one record on the dashboard.

**The backend.** One Node server (running on A's laptop for the demo) with two doors: an admin door (localhost only) for the dashboard, and a badge door (on the phone hotspot) that badges call. It holds Capital One's issuer key, the Nessie key, and a TimescaleDB database.

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

HACK is our own token on Solana (2 decimals, like dollars and cents), standing in for a stablecoin; swapping it for USDC would be a configuration change. Capital One is where it comes from (top-up) and where merchants can cash out (settlement). Because Nessie only stores whole dollars, demo prices are whole HACK amounts.

---

## 6. A payment, step by step

1. The merchant sets 40 HACK; its badge signs a **request** with its own key and broadcasts it over the radio (ESP-NOW).
2. The judge's Pay app lists it and fetches the merchant's **record** from the backend (signed by Capital One's issuer key).
3. The judge's badge sends a **fresh random challenge**; the merchant's badge signs it and answers within a few hundred milliseconds — proof it's the real device and it's right here.
4. The judge's **firmware** checks everything itself: the issuer's signature on the record, the merchant's signature on the request, the presence answer, and the decoded transaction (right recipient, exactly 40.00, real HACK). Screen: **"MHacks Merch ✓ verified ● present · 40.00 HACK"**.
5. The judge presses SELECT; the badge signs; the payment lands on Solana.
6. The merchant's badge confirms it on chain and shows **PAID ✓**. The backend confirms it too, records it, and — if the merchant settles to Capital One — deposits $40 into the merchant's account. The dashboard shows both.

If anything is off: an impostor (no record) → **red**; a tampered amount → **red MISMATCH**; a revoked merchant → **red**; nothing can be signed. Refusals reported by badges show up on the dashboard as "badge decisions".

---

## 7. What lives where

| Layer | What it's responsible for | Status |
| --- | --- | --- |
| **Badge OS core (C)** | Keys; checking records, requests, presence; decoding transactions; the approval screen; signing | Built; **frozen** (no more OS changes) |
| **Badge apps (Lua)** | Pay (judge), Request (merchant), Home | Built; needs provisioning + first live run |
| **Backend (Node)** | Issuer key and payee registry; signed records for badges; confirming payments on chain; Capital One settlement and top-ups; Nessie; database | Built |
| **Dashboard (React)** | Issuer page (verify/revoke payees, enroll at the bank), payment feed with settlements and badge decisions, badges with balances and top-ups, attack console (tampered amount) | Built |
| **Solana devnet** | HACK token, treasury / settlement account, verified-payee registry, every payment | Set up; badges funded |
| **Capital One (Nessie)** | Customers, accounts, merchant, deposits (settlement), withdrawals (top-up) | Seeded |

**In the repo:**
- `docs/specs/` — the specs. `U6-app-decisions.md` is the final demo scope; `00-Interfaces.md` is the shared contract (its §9 routing is not built); `PROJECT-OVERVIEW.md` (this file) is the big picture.
- `dashboard/` — backend (`server/`), dashboard (`web/`), database (`db/`), scripts (`scripts/`, e.g. `nessie:seed`).
- `os/` — the badge firmware and apps (BadgeOS).
- `harness/` — R's test tools (verifiers, fuzz set, timing and signing tests).

---

## 8. What's on chain vs in the database

**On Solana (public, permanent):** the HACK token and balances; every payment; the verified-payee registry — for each payee, a record saying "this badge key is MHacks Merch, a merchant, paid to this wallet, with this bank-account hash", signed by Capital One's issuer.

**In the backend database (private):** which badge is which; the plaintext Nessie IDs and the salts behind the bank hashes (so bank account numbers never appear on chain); every payment the backend confirmed or refused; settlements and top-ups. Because the salts live only here, **all verifying happens on one backend** (A's laptop).

---

## 9. The demo

1. **Verify** — on the Issuer page, Capital One verifies MHacks Merch.
2. **Top up** — the judge tops up HACK from their Capital One account.
3. **Pay** — green "✓ verified ● present", PAID ✓; the dashboard shows the payment and, if the merchant settles to Capital One, the dollar deposit.
4. **Impostor** — a fake "MHacks Merch" is red; nothing to approve.
5. **Tampered amount** — the attack console sends "5 HACK" that really moves 500; the badge shows red MISMATCH.
6. **Revoke** — Capital One revokes the merchant; the next attempt is red.

---

## 10. Decisions that shaped this (and what we gave up)

- **No more OS changes.** Everything new is apps, backend and configuration. Gave up: the badge-signed bank authorization ("via Capital One" screen; `/bank/authorize` stays built but unused).
- **No relay routing.** Cut for time (3–5 h of app work). The backend's routing endpoints exist but are unused; the dashboard doesn't show them.
- **Capital One as identity, settlement and funding** instead of a separate bank payment rail.

Still strong: who you pay, that they're present, and what you pay are verified by the badge's hardware core on every payment.

---

## 11. Known limitations (say them if asked)

Keys are software keys (the secure element latched the button bus on warm resets, so it was disabled); the badge listener has no authentication (devnet only); settle-to-Capital-One depends on the merchant badge accepting payment to its record's wallet (being confirmed by A); Nessie only takes whole dollars.

---

## 12. Glossary

- **ESP-NOW** — the badges' direct radio link (no Wi-Fi network needed); badges must be on the same Wi-Fi channel to hear each other.
- **REQ / CHAL / PROOF / RESULT** — the radio messages of a payment: the merchant's request, the payer's challenge, the merchant's signed answer, the outcome.
- **Record** — the issuer-signed statement about a payee that badges verify before paying.
- **Attestation (SAS)** — the on-chain form of that statement (Solana Attestation Service). Revoking = closing it.
- **Issuer** — Capital One's key that signs records.
- **Treasury / settlement account** — the issuer's HACK account: source of top-ups, and where settle-to-bank payments land.
- **Provisioning** — the one-time USB setup that tells a badge the issuer key, the HACK mint, the Wi-Fi and the backend address.
- **Green / amber / red** — the approval screen: all checks pass / verified but not present / something is wrong, signing blocked.
- **Devnet** — Solana's free test network; nothing here is real money.

---

**Go deeper:** [`docs/specs/U6-app-decisions.md`](specs/U6-app-decisions.md) (final scope) · [`docs/specs/00-Interfaces.md`](specs/00-Interfaces.md) (formats and routes) · [`docs/specs/Prd-verified-payment-key.md`](specs/Prd-verified-payment-key.md) (original product spec) · [`dashboard/README.md`](../dashboard/README.md) (running the backend and dashboard)
