#!/usr/bin/env python3
"""Builds harness/vectors.json: known-good and tampered copies of every message
type in docs/specs/00-Interfaces.md §5-§7, signed with fixed TEST keys.

Each vector's expected reason is set here from the spec, by construction, not by
running either verifier - so vk_verify.py and verify.js are both held to it. A
also uses this file for firmware tests.

The keys are derived from public, fixed seeds. They are test keys only: never
fund them, never put them in badges.json.

  harness/.venv/bin/python harness/make_vectors.py
"""

import json
import struct
from pathlib import Path

from nacl.signing import SigningKey

NOW = 1790000000  # fixed "current time" every time-dependent vector is judged at


def key(label):
    seed = (label.encode() * 32)[:32]
    sk = SigningKey(seed)
    return sk, bytes(sk.verify_key)


PAYEE_SK, PAYEE = key("payee")
PAYER_SK, PAYER = key("payer")
ISSUER_SK, ISSUER = key("issuer")
OTHER_SK, OTHER = key("other")

REQ_ID = bytes.fromhex("0102030405060708")
NONCE = bytes.fromhex("a0a1a2a3a4a5a6a7a8a9aaabacadaeaf")

vectors = []


def add(name, kind, expect, **inputs):
    vectors.append({"name": name, "kind": kind, "expect": expect, "input": inputs})


def sign(sk, message):
    return sk.sign(message).signature


# --- REQ (§5) -------------------------------------------------------------------

def req_body(rail=1, amount=4000, currency=b"HACK", expiry=NOW + 60, name=b"Nessie Cafe",
             name_len=None, magic=b"VK", version=1, msg_type=1, payee=PAYEE):
    return (magic + bytes([version, msg_type, rail]) + payee + struct.pack("<Q", amount)
            + currency + REQ_ID + struct.pack("<I", expiry)
            + bytes([len(name) if name_len is None else name_len]) + name)


def req(sk=PAYEE_SK, prefix=b"pay-req:", **kw):
    body = req_body(**kw)
    return body + sign(sk, prefix + body)


good_req = req()
add("req ok (HACK)", "req", "ok", frame=good_req.hex())
add("req ok (bank, USD)", "req", "ok",
    frame=req(rail=2, currency=b"USD\x00", name=b"Corner Shop").hex())
add("req ok (empty name)", "req", "ok", frame=req(name=b"").hex())
flipped = bytearray(good_req)
flipped[40] ^= 0x01  # inside amount
add("req tampered amount", "req", "bad_signature", frame=bytes(flipped).hex())
add("req signed by another key", "req", "bad_signature", frame=req(sk=OTHER_SK).hex())
add("req signed without prefix", "req", "bad_signature", frame=req(prefix=b"").hex())
add("req signed with pay-proof prefix", "req", "bad_signature", frame=req(prefix=b"pay-proof:").hex())
add("req trailing byte", "req", "bad_length", frame=(good_req + b"\x00").hex())
add("req truncated", "req", "bad_length", frame=good_req[:-1].hex())
add("req bad magic", "req", "bad_magic", frame=req(magic=b"XK").hex())
add("req version 2", "req", "bad_version", frame=req(version=2).hex())
add("req wrong type", "req", "bad_type", frame=req(msg_type=3).hex())
add("req name_len over 32", "req", "bad_field",
    frame=(req_body(name=b"A" * 33) + bytes(64)).hex())
add("req rail/currency mismatch", "req", "bad_field", frame=req(rail=1, currency=b"USD\x00").hex())
add("req unknown rail", "req", "bad_field", frame=req(rail=3).hex())
add("req zero amount", "req", "bad_field", frame=req(amount=0).hex())
add("req non-printable name", "req", "bad_field", frame=req(name=b"Caf\x07").hex())
add("req expired", "req", "expired", frame=req(expiry=NOW - 1).hex())

# --- CHAL / PROOF (§5) ---------------------------------------------------------

def chal(req_id=REQ_ID, nonce=NONCE, payer=PAYER, length=None):
    frame = b"VK\x01\x02" + req_id + nonce + payer
    return frame if length is None else frame[:length]


def proof(sk=PAYEE_SK, req_id=REQ_ID, signed_nonce=NONCE, signed_payer=PAYER, prefix=b"pay-proof:",
          msg_type=3):
    sig = sign(sk, prefix + REQ_ID + signed_nonce + signed_payer)
    return b"VK\x01" + bytes([msg_type]) + req_id + sig


good_chal = chal()
good_proof = proof()
add("proof ok", "proof", "ok", proof=good_proof.hex(), chal=good_chal.hex(), payee_pubkey=PAYEE.hex())
add("proof replayed against a new nonce", "proof", "bad_signature", proof=good_proof.hex(),
    chal=chal(nonce=bytes(16)).hex(), payee_pubkey=PAYEE.hex())
add("proof for a different payer", "proof", "bad_signature", proof=good_proof.hex(),
    chal=chal(payer=OTHER).hex(), payee_pubkey=PAYEE.hex())
add("proof signed by impostor", "proof", "bad_signature", proof=proof(sk=OTHER_SK).hex(),
    chal=good_chal.hex(), payee_pubkey=PAYEE.hex())
add("proof signed with pay-req prefix", "proof", "bad_signature", proof=proof(prefix=b"pay-req:").hex(),
    chal=good_chal.hex(), payee_pubkey=PAYEE.hex())
add("proof for another req_id", "proof", "mismatch",
    proof=proof(req_id=bytes.fromhex("ffffffffffffffff")).hex(), chal=good_chal.hex(),
    payee_pubkey=PAYEE.hex())
add("proof trailing byte", "proof", "bad_length", proof=(good_proof + b"\x00").hex(),
    chal=good_chal.hex(), payee_pubkey=PAYEE.hex())
add("proof wrong type", "proof", "bad_type", proof=proof(msg_type=4).hex(), chal=good_chal.hex(),
    payee_pubkey=PAYEE.hex())
add("proof with truncated chal", "proof", "bad_length", proof=good_proof.hex(),
    chal=chal(length=59).hex(), payee_pubkey=PAYEE.hex())

# --- bank payload (§6) ----------------------------------------------------------

BANK = [("v", "1"), ("rail", "nessie"), ("action", "purchase"), ("amount_cents", "4000"),
        ("currency", "USD"), ("from_acct", "5f1b2c3d4e5f60718293a4b5"), ("payee_name", "Nessie Cafe"),
        ("payee_ref", "ab" * 32), ("attestation", "none"), ("req_id", REQ_ID.hex()),
        ("proof_nonce", NONCE.hex()), ("issued_at", str(NOW - 10))]


def bank_text(pairs=BANK, **override):
    return "\n".join(f"{k}={override.get(k, v)}" for k, v in pairs)


def bank(text, sk=PAYER_SK, prefix=b"bank-auth:", signed_text=None):
    signed = (signed_text if signed_text is not None else text).encode()
    return {"payload": text, "sig": sign(sk, prefix + signed).hex(), "payer_pubkey": PAYER.hex()}


good_bank = bank_text()
add("bank ok", "bank", "ok", **bank(good_bank))
add("bank ok (transfer)", "bank", "ok", **bank(bank_text(action="transfer")))
add("bank tampered amount", "bank", "bad_signature",
    **bank(bank_text(amount_cents="4001"), signed_text=good_bank))
add("bank signed by another key", "bank", "bad_signature", **bank(good_bank, sk=OTHER_SK))
add("bank signed without prefix", "bank", "bad_signature", **bank(good_bank, prefix=b""))
add("bank signed with registry prefix", "bank", "bad_signature", **bank(good_bank, prefix=b"registry:"))
add("bank trailing newline", "bank", "bad_format", **bank(good_bank + "\n"))
swapped = BANK[:]
swapped[1], swapped[2] = swapped[2], swapped[1]
add("bank keys out of order", "bank", "bad_format", **bank(bank_text(swapped)))
add("bank missing line", "bank", "bad_format", **bank("\n".join(good_bank.split("\n")[:-1])))
add("bank version 2", "bank", "bad_version", **bank(bank_text(v="2")))
add("bank wrong currency", "bank", "bad_field", **bank(bank_text(currency="EUR")))
add("bank uppercase hex", "bank", "bad_field", **bank(bank_text(payee_ref="AB" * 32)))
add("bank zero amount", "bank", "bad_field", **bank(bank_text(amount_cents="0")))
add("bank leading-zero amount", "bank", "bad_field", **bank(bank_text(amount_cents="04000")))
add("bank unknown action", "bank", "bad_field", **bank(bank_text(action="refund")))
add("bank stale issued_at", "bank", "stale", **bank(bank_text(issued_at=str(NOW - 76))))
add("bank issued_at far in future", "bank", "stale", **bank(bank_text(issued_at=str(NOW + 76))))

# --- registry record (§7) -------------------------------------------------------

REG = [("v", "1"), ("attestation", "9xQeWvG816bUx9EPjHmaT23yvVM2ZWbrrpZb9PusVFin"),
       ("display_name", "Nessie Cafe"), ("device_pubkey", PAYEE.hex()), ("kind", "merchant"),
       ("solana_wallet", "11" * 32), ("solana_ata", "22" * 32), ("bank_ref_hash", "ab" * 32),
       ("expiry", str(NOW + 3600)), ("status", "active"), ("issued_at", str(NOW - 5))]


def reg_text(pairs=REG, **override):
    return "\n".join(f"{k}={override.get(k, v)}" for k, v in pairs)


def reg(text, sk=ISSUER_SK, prefix=b"registry:", signed_text=None):
    signed = (signed_text if signed_text is not None else text).encode()
    return {"record": text, "sig": sign(sk, prefix + signed).hex(), "issuer_pubkey": ISSUER.hex()}


good_reg = reg_text()
add("registry ok", "registry", "ok", **reg(good_reg))
add("registry ok (person, no wallet)", "registry", "ok",
    **reg(reg_text(kind="person", solana_wallet="", solana_ata="")))
add("registry tampered device_pubkey", "registry", "bad_signature",
    **reg(reg_text(device_pubkey=OTHER.hex()), signed_text=good_reg))
add("registry signed by a badge key", "registry", "bad_signature", **reg(good_reg, sk=PAYEE_SK))
add("registry signed without prefix", "registry", "bad_signature", **reg(good_reg, prefix=b""))
add("registry trailing newline", "registry", "bad_format", **reg(good_reg + "\n"))
add("registry extra line", "registry", "bad_format", **reg(good_reg + "\nnote=x"))
add("registry version 2", "registry", "bad_version", **reg(reg_text(v="2")))
add("registry ata without wallet", "registry", "bad_field", **reg(reg_text(solana_wallet="")))
add("registry name over 32", "registry", "bad_field", **reg(reg_text(display_name="N" * 33)))
add("registry bad kind", "registry", "bad_field", **reg(reg_text(kind="bank")))
add("registry revoked", "registry", "revoked", **reg(reg_text(status="revoked")))
add("registry expired", "registry", "expired", **reg(reg_text(expiry=str(NOW - 1))))
add("registry older than TTL", "registry", "stale", **reg(reg_text(issued_at=str(NOW - 31))))

out = {
    "about": "R5 vectors for docs/specs/00-Interfaces.md §5-§7. TEST keys from fixed public seeds - "
             "never fund them. Regenerate with harness/make_vectors.py.",
    "now": NOW,
    "keys": {"payee": PAYEE.hex(), "payer": PAYER.hex(), "issuer": ISSUER.hex(), "other": OTHER.hex()},
    "vectors": vectors,
}
path = Path(__file__).resolve().parent / "vectors.json"
path.write_text(json.dumps(out, indent=1) + "\n", encoding="utf-8")
print(f"wrote {len(vectors)} vectors to {path}")
