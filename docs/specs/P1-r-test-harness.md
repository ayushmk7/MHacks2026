# P1-R — Test Harness: Prove the Risky Parts First

Owner: **R** · Part 1 (≈ hours 0–8) · Gate contribution: the laptop side of the first badge-signed devnet payment
Reads: [`00-Interfaces.md`](00-Interfaces.md) (§0, §1, §3, §5, §9) · Parent: [`Prd-verified-payment-key.md`](Prd-verified-payment-key.md)

## 1. Why this exists

Five unknowns can sink the project. The harness answers them in the first hours, before anyone builds on top of them, then becomes the **verifier and attack console** used in the demo. Nothing here is throwaway.

| Unknown | Kills | Measured by |
| --- | --- | --- |
| Does the SE050 hold and sign with the Ed25519 key? | the "secure element" claim | R1 |
| Can a badge reach devnet RPC, the badge listener and SNTP over the hotspot? | every payment | R2 |
| Does a badge-produced signature verify on the laptop and land on devnet? | the whole product | R3 |
| How long does CHAL → signed PROOF take? | the presence deadline | R4 |
| 🆕 Can a ~1.3 KB message cross two radio hops reliably? | the relay network (00 §9) | R7 |

> ✏️ Changed (routing): a fifth unknown and R7/R8 added. Reads now also 00 §9.

## 2. What already exists
- **Solana OS on the badges:** identity key, `badge.espnow`, `badge.http`, the serial console (push protocol only, **no Lua serial read**) and app sideloading. See https://github.com/spacemandev-git/solana-defcon-badge-26/blob/main/firmware/solana-os/README.md.
- **The broker** already does challenge-response signing with the badge key. Its client code shows how signatures and public keys are encoded today.
- **The existing Node server's badge listener** (port 8788) serves `GET /badge/pending?badge=<pubkey>` → `{messageBase64, txBase64}` and accepts `POST /badge/outcome` (see the [API](../dashboard/API.md) and [badge-gap notes](../dashboard/BADGE-GAPS.md)). That is the transport for R3.
- **`npm run pay`** and `scripts/simulate-payment.mjs` already send HACK transfers between stand-in keys. Use them as a reference for building transactions in JS.

You do **not** need A's wallet module to start R1, R2 and R4.

## 3. Scope — ✏️ Changed (routing)

| ID | Deliverable | Language | Priority |
| --- | --- | --- | --- |
| R0 | `harness/` folder: Python 3.11 venv (`pynacl`, `solders`, `solana`, `requests`) for attack scripts; JS work lives in the existing Node project | — | P0 |
| R1 | **SE050 survey:** table of badge → pubkey → key location → failing step if any. The serial log is parsed by script, Settings → Identity is read by hand. Hand pubkeys to U for `server/config/badges.json` | Python + manual | P0 |
| R2 | **Network probe app (Lua):** on button press, `GET /health` on the badge listener, `getHealth` on devnet RPC, and show `wallet.time_ok()` / SNTP result; display latency and result | Lua | P0 |
| R3 | **Signing harness:** build an unsigned legacy `transferChecked` (+ optional zero-account memo) for a badge; queue it on `/badge/pending`; a tiny Lua test app polls it, calls `wallet.begin_solana` / `wallet.poll`, posts the sig back; verify; attach and `sendTransaction`; print explorer link | JS (in `server/`/`scripts/`) + Lua | P0 |
| R4 | **Presence timing app (Lua):** two badges run the real exchange 200×: CHAL (60 B) → payee signs with `wallet.sign_proof` (or a stub sign before A10) → PROOF (76 B). Report p50 / p95 / max on screen and serial, for Wi-Fi on and off | Lua | P0 |
| R5 | **Verifier library** for REQ, PROOF, bank payload and registry record per 00 §3/§5/§6/§7: `server/src/verify.js` (used by U's backend) + `harness/vk_verify.py` twin (attack console), both passing the same `harness/vectors.json` | JS + Python | P1 |
| R6 | **Message fuzz set:** malformed / oversized / wrong-prefix / two-signer / versioned / wrong-mint / trailing-bytes inputs for A's decoder and `sign_*` functions, served through the R3 path | JS or Python | P1 |
| R7 | 🆕 **FRAG prototype + multi-hop radio test (Lua):** implement FRAG / FRAG_ACK (00 §9.4) and send a 1,341 B message (a 2-relay RREP) across badge → badge → badge, 100×. Report per-hop time, whole-message success rate with and without retries, and frame loss %. This is the riskiest part of routing, so it is proven in Part 1 | Lua | P1 |
| R8 | 🆕 **Routing vectors:** add to `vectors.json` the compact records (`registry-c:`), `route-att:` and `route-quote:` signatures, a full 2-relay RREP, and routed transactions with K = 1, 2, 3 fee legs (byte-for-byte references for A's builder and decoder). Extend R5's verifiers to these formats | JS + Python | P1 |

## 4. Technical notes

### 4.1 Building the test transaction (R3)
Build it in JS in the existing project. That reuses `@solana/kit` and the server's keypair and RPC config, and the badge listener already serves the result. The message **must be legacy** (A's decoder rejects versioned messages). Note that the server's `sendIxs` builds v0, so don't reuse it for this. The attack builder (`buildTamperedTx`, `ATTACK_TX_VERSION=legacy`) is the closest template.

Requirements:
- Payer and fee payer = badge pubkey (signer 0, also the token owner).
- `source`/`dest` = the HACK associated token accounts. They must already exist; creating one would add a second instruction, which the decoder refuses.
- Mint and decimals from 00 §2 (HACK, 2).
- Any memo has zero accounts.

After the badge returns its signature:
1. Verify it with `node:crypto` Ed25519 or tweetnacl.
2. Assemble the wire transaction as `[0x01, sig64, message]`.
3. Send it base64-encoded and print the explorer link.

Python equivalent for the attack console, with the fixes:
- `Transaction.populate(msg, [Signature.from_bytes(sig)])` from `solders.transaction`.
- ATAs via `spl.token.instructions.get_associated_token_address`.
- `VerifyKey.verify` raises `BadSignatureError` rather than returning False.

The blockhash lasts about 60–90 s. Fetch it right before queueing, and rebuild if the approval exceeds about 45 s.

### 4.2 Pubkey encoding
The badge ID is the first 8 base58 characters of the public key; the full pubkey is the Solana address. Confirm the byte order matches what the verifier expects in R3. A mismatch shows up as a signature that never verifies.

### 4.3 Presence timing (R4)
- Both badges on the same hotspot channel (or Wi-Fi off on both).
- Time with `badge.millis()`; there is no microsecond clock. Measure on the payer from `new_nonce` to PROOF receipt, which is exactly what `check_proof` will time.
- Run once with Wi-Fi connected and once without, and in the actual room.

### 4.4 Multi-hop radio (R7) — 🆕 Routing
- Use three badges with `DEMO_NEIGHBORS` set, so A only talks to B and B only to C. Then the message really crosses two radio hops even though all three badges are in range.
- Frames are ≤ 240 B, with ~2 ms between frames. Measure with and without Wi-Fi associated, on the venue hotspot channel.
- Pass bar: ≥ 95 % of 1,341 B messages delivered within 2 retries, and ≤ 150 ms per hop for a 7-frame message. If it misses, report back before P2. The fallback is compact hop entries (`CHANGELOG-routing.md`, open decisions).

## 5. Interfaces you provide — ✏️ Changed (routing)
- 🆕 R7 numbers → A and R confirm `FRAG_REASSEMBLY_MS`, `FRAG_RETRIES` and the routing timing budget (00 §9.8–9.9).
- 🆕 R7's FRAG code is reused by R's relay app and A's payer (P2-R, P2-A).
- R3 harness: A's "done" test for A6.
- R4 numbers → `PRESENCE_DEADLINE_MS` = p95 (including signature) × 1.5, separately for SE050 and NVS keys. Write the agreed values into 00 §4.
- R5 `verify.js` + `vk_verify.py` + `vectors.json` → U's backend and the attack console. The vectors also go to A for firmware tests.

## 6. Dependencies
- A: `wallet.begin_solana` / `wallet.poll` for the final R3 run (a `VK_DEV_ALLOW_UNVERIFIED` build is fine before the registry exists); `sign_proof` for the final R4 run.
- U: HACK mint, funded badge SOL + ATAs (`npm run devnet:setup` after real pubkeys are in `badges.json`), badge listener with `/health` open on the hotspot.

## 7. Done when — ✏️ Changed (routing)
- [ ] R1 table filled for all 4 badges; pubkeys handed to U.
- [ ] R2 shows green for the badge listener, devnet and the clock, from a badge on the hotspot.
- [ ] R3: badge-signed HACK transfer confirmed on devnet; explorer link saved in `harness/RESULTS.md`.
- [ ] R4: p50/p95/max and packet loss % for both Wi-Fi states posted; deadline agreed and written into 00.
- [ ] R5: JS and Python both verify every known-good vector and reject a tampered copy of each message type.
- [ ] R6: every fuzz input refused by the badge without a crash or watchdog reset.
- [ ] 🆕 R7: multi-hop FRAG numbers posted against the pass bar in §4.4.
- [ ] 🆕 R8: routing vectors committed; JS and Python verifiers agree on all of them.

## 8. Risks — ✏️ Changed (routing)
| Risk | Mitigation |
| --- | --- |
| Venue Wi-Fi interference | Hotspot only; test R4 in the actual room |
| Devnet RPC rate limits | `RPC_URL` in `.env` swapped to a provider; retry with backoff |
| Waiting on A | Every item except the final R3/R4 runs works without A (stub signing in Lua or a software key) |
| 🆕 Multi-frame loss across hops | R7 measures it early; whole-message retry; fallback to compact hop entries |
