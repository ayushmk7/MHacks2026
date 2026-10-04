# Badge Technical PRD — How the Payment Key Works

Short technical summary of the badge as built for the demo (2026-10-04). Big picture: [`PROJECT-OVERVIEW.md`](PROJECT-OVERVIEW.md) · Scope: [`specs/U6-app-decisions.md`](specs/U6-app-decisions.md) · Byte formats: [`specs/00-Interfaces.md`](specs/00-Interfaces.md)

## 1. Goal

Before any payment, the badge itself proves three things, in firmware no app can override:

| Guarantee | Question | How |
| --- | --- | --- |
| **Who** | Is this the payee Capital One verified? | Issuer-signed registry record, checked on the badge |
| **Present** | Is that payee's device right here? | Timed, signed challenge over radio |
| **What** | What exactly am I signing? | The badge decodes the real transaction bytes |

Only if all pass does a button press sign. Otherwise the screen is red and signing is disabled.

## 2. Hardware

ESP32-S3 (16 MB flash, 8 MB PSRAM) · 2.8" 320×240 screen · 6 buttons (SELECT = confirm, CANCEL = back) · 2 RGB LEDs · Wi-Fi + ESP-NOW radio · NXP SE050 secure element (**disabled**: it latched the shared I²C bus and killed the buttons on warm resets, so keys are stored in flash).

## 3. Software layers

| Layer | Language | Owns | Trust |
| --- | --- | --- | --- |
| **OS core** (BadgeOS, fork of Solana OS) | C/C++ | The key; record, request and presence checks; transaction decoder; approval screen; signing | **Trusted.** Makes every security decision |
| **Apps** | Lua | Pay (payer), Request (merchant), Home | **Untrusted.** UI and radio messaging only; hands bytes to the core and gets back a signature or a refusal |

An app can never sign directly, choose what the screen says during approval, or skip a check.

## 4. Keys and trust

- **Badge key** (one per badge, Ed25519): generated on first boot, never leaves the badge. Signs payments (payer) and requests and presence answers (merchant). Signing ≈ 211 ms; verify ≈ 18 ms (Monocypher).
- **Issuer key** (Capital One's, on the backend): signs one record per verified payee. Its **public** key is provisioned onto every badge; the badge trusts records only if this key signed them.
- Every signature carries a **domain prefix** (`registry:`, `pay-req:`, `pay-proof:`, or a raw Solana message), so a signature for one purpose can't be replayed as another.

## 5. Provisioning (one time, over USB)

| Setting | Value | Protected? |
| --- | --- | --- |
| `issuer_key` | `2SXh6Xng9b1qQBCEn3pt2Ucwcb4ibBWwBKuuTwNgEzJy` | Yes: changing it needs a hold on SELECT |
| `tokens` | HACK mint `3VmWnz…M9wP`, 2 decimals, cap 100, max 1000 | Yes |
| `listener_url` | `http://<backend IP>:8788` | — |
| `rpc_url` | Solana devnet | — |
| `record_ttl_s` | 60 (max age of a record) | Yes |
| Wi-Fi | The phone hotspot (shared channel for ESP-NOW) | — |

Keys and funds survive reflashing; only a full flash erase creates a new key.

## 6. A payment

```
Merchant badge                 Judge badge                      Backend (laptop)          Solana
Request app: 40 HACK
  core signs REQ ──radio──►  Pay app lists "MHacks Merch"
                              GET /registry/<merchant> ───────► issuer-signed record
                              CHAL (random nonce) ──radio──►
  core signs PROOF ◄──────── (≤ 400 ms deadline)
                              OS core checks record, REQ, PROOF, decoded tx
                              screen: "MHacks Merch ✓ verified ● present · 40.00 HACK"
                              SELECT → core signs ──────────────────────────────────► transfer lands
  confirms on chain, PAID ✓   POST /feed/solana ─────────────► confirms on chain, logs it
```

**Radio messages** (ESP-NOW, ≤ 240 B): `REQ` (payee key, amount, req id, expiry, name, merchant signature) · `CHAL` (req id, 16-byte nonce, payer key) · `PROOF` (merchant signature over the nonce) · `RESULT` (outcome; unauthenticated, so the merchant always re-checks the chain).

## 7. What the OS core checks before showing the approval screen

1. **Record:** signed by the provisioned issuer, `status=active`, not expired, fetched within `record_ttl_s`.
2. **Request:** signed by the record's device key; not expired; amount and payee match.
3. **Presence:** the merchant's signed answer to *this* nonce arrived within the deadline (measured 234–251 ms with Wi-Fi off).
4. **Transaction:** exactly one HACK `transferChecked` (+ optional memo with the req id), right mint and decimals, recipient = the record's token account, amount = the request's amount, a single signer (this badge).
5. **Limits:** above the cap needs a second hold; above the max is blocked.
6. **Clock:** set by SNTP; without it the badge refuses ("clock not set").

## 8. Screens

| Colour | Meaning | SELECT |
| --- | --- | --- |
| **Green** "✓ verified ● present" | All checks pass | Enabled |
| **Amber** "not present" | Verified, but no timely presence answer | Enabled (Solana), with a warning |
| **Red** | Unverified, revoked or expired payee · amount/recipient mismatch · undecodable · clock not set | Disabled |

Refusals are reported to the backend (`POST /feed/event`) and appear on the dashboard as badge decisions.

## 9. What the badge needs from the backend

`GET /health` · `GET /registry/:pubkey` (signed record; 404 = unverified) · `POST /feed/solana` (report a payment; the backend re-checks it on chain) · `POST /feed/event` (report a refusal) · `GET /badge/pending` (attack demo: a tampered transaction to inspect). The badge never trusts the backend's word alone: records are issuer-signed and payments are on chain.

## 10. Proven on hardware (2026-10-04)

Direct payment green and landed on devnet (G3) · impostor claiming "MHacks Merch" → red, unverified · tampered "5 shown / 500 real" → shows the real 500, refused · revoked merchant → red within ~20 s, green again after re-issue (G4).

## 11. Limitations

Software keys (SE050 disabled) · the backend listener has no authentication (devnet only) · the merchant only accepts payment to its own account (no settle-to-bank) · no relay routing · RESULT frames are unauthenticated (the merchant confirms on chain).
