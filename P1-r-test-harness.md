# P1-R — Test Harness: Prove the Risky Parts First

Owner: **R** · Part 1 (≈ hours 0–8) · Gate contribution: the laptop side of the first badge-signed devnet payment
Reads: `00-interfaces.md` (§1, §3, §5) · Parent: `PRD-verified-payment-key.md`

## 1. Why this exists

Four unknowns can sink the project. The harness answers them in the first hours, before anyone builds on top of them, then becomes the **verifier and attack console** used in the demo. Nothing here is throwaway.

| Unknown | Kills | Measured by |
| --- | --- | --- |
| Does the SE050 hold and sign with the Ed25519 key? | the "secure element" claim | R1 |
| Can a badge reach devnet RPC and the backend over the hotspot? | every payment | R2 |
| Does a badge-produced signature verify on the laptop and land on devnet? | the whole product | R3 |
| What is the ESP-NOW round-trip time? | the presence deadline | R4 |

## 2. What already exists
- Solana OS on the badges: identity key, `badge.espnow`, `badge.http`, serial console, app sideloading over Wi-Fi/BLE/USB, and a **virtual badge** browser emulator that runs the same Lua apps with ESP-NOW relayed over a server (https://github.com/spacemandev-git/solana-defcon-badge-26/blob/main/firmware/solana-os/README.md).
- The broker already does challenge-response signing with the badge key; reading its client code shows how signatures and public keys are encoded today.

You do **not** need A's wallet module to start R1, R2 and R4.

## 3. Scope

| ID | Deliverable | Language | Priority |
| --- | --- | --- | --- |
| R0 | `harness/` repo folder, Python 3.11 venv: `pynacl`, `solders`, `solana`, `pyserial`, `requests` | — | P0 |
| R1 | **SE050 survey:** script that reads each badge's serial log and Settings → Identity result; table of badge → key location → failing step if any | Python + manual | P0 |
| R2 | **Network probe app (Lua):** on button press, `GET /health` on backend and `getHealth` on devnet RPC; show latency and result on screen | Lua | P0 |
| R3 | **Signing harness (laptop):** build an unsigned `transferChecked` (+ optional memo) message for a badge's pubkey; push it to the badge; receive the signature; verify with PyNaCl; attach and `sendTransaction`; print explorer link | Python | P0 |
| R4 | **ESP-NOW ping-pong app (Lua):** two badges exchange 60 B and 160 B messages 200×; report p50 / p95 / max RTT on screen and over serial | Lua | P0 |
| R5 | **Laptop verifier library** `vk_verify.py`: verify REQ, PROOF, bank payload and registry record signatures per 00 §3/§5/§6/§7; used by U's backend and the attack console | Python | P1 |
| R6 | **Message fuzz set:** malformed / oversized / wrong-prefix inputs for A's decoder and `sign_*` functions | Python | P1 |

### Transport for R3 (pick the first that works)
1. Serial: a tiny Lua test app reads a base64 line from USB serial, calls `wallet.sign_solana`, prints the base64 signature. (Simplest; recommended.)
2. HTTP: the badge polls `GET /test/next` on the laptop and posts the signature back.
Until A's `wallet.sign_solana` exists, use the broker's existing signing path or a temporary software key to prove the laptop side (build → verify → submit).

## 4. Technical notes

### 4.1 Building the test transaction (R3)
```python
from solders.pubkey import Pubkey
from solders.message import Message
from solders.hash import Hash
from spl.token.instructions import transfer_checked, TransferCheckedParams
from spl.token.constants import TOKEN_PROGRAM_ID
# payer = badge pubkey (signer 0); source/dest = ATAs; mint/decimals from 00 §2
ix = transfer_checked(TransferCheckedParams(program_id=TOKEN_PROGRAM_ID,
        source=src_ata, mint=mint, dest=dst_ata, owner=badge_pk,
        amount=1_000_000, decimals=6))
msg = Message.new_with_blockhash([ix], badge_pk, Hash.from_string(latest_blockhash))
raw = bytes(msg)                      # this is what the badge signs
# after badge returns sig:
# VerifyKey(bytes(badge_pk)).verify(raw, sig)
# tx = Transaction.populate(msg, [Signature.from_bytes(sig)]); client.send_raw_transaction(bytes(tx))
```
The blockhash expires in about a minute — fetch it right before pushing to the badge, and keep approval fast during tests.

### 4.2 Pubkey encoding
The badge ID is the first 8 base58 characters of the public key; the full pubkey is the Solana address. Confirm the byte order matches what PyNaCl expects in R3 — a mismatch shows up as a signature that never verifies.

### 4.3 ESP-NOW (R4)
- Both badges on the same hotspot channel (or Wi-Fi off on both).
- Timestamp with the badge's microsecond clock; RTT measured on the initiator.
- Run once with Wi-Fi connected and once without; channel effects show up here.

## 5. Interfaces you provide
- R3 script usable by A as the "done" test for A6.
- R4 numbers → A sets `PRESENCE_DEADLINE_MS` (suggest p99 × 2, floor 100 ms).
- R5 library → U's backend imports it for `/bank/authorize` and `/registry` checks.

## 6. Dependencies
- A: `wallet.sign_solana` for the final R3 run.
- U: funded badge ATAs + mint (for R3 submit), backend `/health` (for R2; mock it locally if not ready).

## 7. Done when
- [ ] R1 table filled for all 4 badges.
- [ ] R2 shows green for backend and devnet from a badge on the hotspot.
- [ ] R3: badge-signed transfer confirmed on devnet, explorer link saved.
- [ ] R4: RTT numbers posted to the team; deadline agreed.
- [ ] R5: verifies a known-good and rejects a tampered sample for each message type.

## 8. Risks
| Risk | Mitigation |
| --- | --- |
| Venue Wi-Fi interference | Hotspot only; test R4 in the actual room |
| Devnet RPC rate limits | Second RPC endpoint in config; retry with backoff |
| Waiting on A | Every item except the final R3 run works without A |