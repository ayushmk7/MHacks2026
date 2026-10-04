#!/usr/bin/env python3
"""R5 verifier, Python twin of dashboard/server/src/verify.js (attack console).

Verifies the four signed message types in docs/specs/00-Interfaces.md:
  REQ   (§5, ESP-NOW, payee signs  "pay-req:"   + frame from header to name)
  PROOF (§5, ESP-NOW, payee signs  "pay-proof:" + req_id + nonce + payer_pubkey)
  bank  (§6, payer signs "bank-auth:" + canonical key=value payload)
  registry record (§7, issuer signs "registry:" + canonical key=value record)

Every function returns {"ok": bool, "reason": str, "fields": dict}. Reasons are
identical to verify.js, and both are held to harness/vectors.json:

  ok            valid
  bad_length    wrong size, truncated or trailing bytes
  bad_magic     header magic is not "VK"
  bad_version   header or payload version is not 1
  bad_type      wrong message type byte
  bad_field     a field is out of range or malformed
  bad_format    text payload: wrong keys, order, line count or trailing newline
  mismatch      PROOF does not belong to the given CHAL
  bad_signature Ed25519 signature does not verify for the expected key
  expired       REQ or registry expiry is in the past
  stale         bank issued_at outside the approval window, or registry older than its TTL
  revoked       registry record with status=revoked

Checks run in the same order in both languages: structure, then signature, then
time and status. Time checks run only when `now` (unix seconds) is passed.

  python harness/vk_verify.py --check   # run every vector in harness/vectors.json
"""

import base64
import json
import re
import struct
import sys
from pathlib import Path

from nacl.exceptions import BadSignatureError
from nacl.signing import VerifyKey

MAGIC = b"VK"
VERSION = 1
TYPE_REQ, TYPE_CHAL, TYPE_PROOF = 1, 2, 3
CHAL_LEN, PROOF_LEN = 60, 76
REQ_FIXED = 4 + 1 + 32 + 8 + 4 + 8 + 4 + 1  # header .. name_len
CURRENCY = {1: b"HACK", 2: b"USD\x00"}

APPROVAL_TIMEOUT_S = 45
BANK_WINDOW_S = APPROVAL_TIMEOUT_S + 30  # §6: backend accepts issued_at within this
RECORD_TTL_S = 30                         # §4

HEX64 = re.compile(r"[0-9a-f]{64}\Z")
HEX32 = re.compile(r"[0-9a-f]{32}\Z")
HEX16 = re.compile(r"[0-9a-f]{16}\Z")
UINT = re.compile(r"(0|[1-9][0-9]*)\Z")
PRINTABLE = re.compile(r"[\x20-\x7e]*\Z")

BANK_KEYS = ["v", "rail", "action", "amount_cents", "currency", "from_acct", "payee_name",
             "payee_ref", "attestation", "req_id", "proof_nonce", "issued_at"]
REGISTRY_KEYS = ["v", "attestation", "display_name", "device_pubkey", "kind", "solana_wallet",
                 "solana_ata", "bank_ref_hash", "expiry", "status", "issued_at"]


def _result(ok, reason, fields=None):
    return {"ok": ok, "reason": reason, "fields": fields or {}}


def _sig_ok(pubkey, message, sig):
    if len(pubkey) != 32 or len(sig) != 64:
        return False
    try:
        VerifyKey(bytes(pubkey)).verify(bytes(message), bytes(sig))
        return True
    except (BadSignatureError, ValueError):
        return False


def _header(frame, msg_type):
    if len(frame) < 4:
        return "bad_length"
    if frame[0:2] != MAGIC:
        return "bad_magic"
    if frame[2] != VERSION:
        return "bad_version"
    if frame[3] != msg_type:
        return "bad_type"
    return None


# --- §5 ESP-NOW ---------------------------------------------------------------

def parse_chal(frame):
    frame = bytes(frame)
    err = _header(frame, TYPE_CHAL)
    if err:
        return _result(False, err)
    if len(frame) != CHAL_LEN:
        return _result(False, "bad_length")
    return _result(True, "ok", {"req_id": frame[4:12], "nonce": frame[12:28],
                                "payer_pubkey": frame[28:60]})


def verify_req(frame, now=None):
    frame = bytes(frame)
    err = _header(frame, TYPE_REQ)
    if err:
        return _result(False, err)
    if len(frame) < REQ_FIXED:
        return _result(False, "bad_length")
    rail = frame[4]
    payee = frame[5:37]
    (amount,) = struct.unpack_from("<Q", frame, 37)
    currency = frame[45:49]
    req_id = frame[49:57]
    (expiry,) = struct.unpack_from("<I", frame, 57)
    name_len = frame[61]
    if name_len > 32:
        return _result(False, "bad_field")
    signed_end = REQ_FIXED + name_len
    if len(frame) != signed_end + 64:
        return _result(False, "bad_length")
    name = frame[REQ_FIXED:signed_end]
    if rail not in CURRENCY or currency != CURRENCY[rail] or amount == 0:
        return _result(False, "bad_field")
    if not PRINTABLE.match(name.decode("latin-1")):
        return _result(False, "bad_field")
    fields = {"rail": rail, "payee_pubkey": payee, "amount": amount, "currency": currency,
              "req_id": req_id, "expiry": expiry, "name": name.decode("ascii")}
    if not _sig_ok(payee, b"pay-req:" + frame[:signed_end], frame[signed_end:]):
        return _result(False, "bad_signature", fields)
    if now is not None and now > expiry:
        return _result(False, "expired", fields)
    return _result(True, "ok", fields)


def verify_proof(proof, chal, payee_pubkey):
    """PROOF answers one CHAL and is signed by the payee named in the REQ."""
    proof = bytes(proof)
    err = _header(proof, TYPE_PROOF)
    if err:
        return _result(False, err)
    if len(proof) != PROOF_LEN:
        return _result(False, "bad_length")
    challenge = parse_chal(chal)
    if not challenge["ok"]:
        return challenge
    c = challenge["fields"]
    req_id = proof[4:12]
    if req_id != c["req_id"]:
        return _result(False, "mismatch")
    signed = b"pay-proof:" + c["req_id"] + c["nonce"] + c["payer_pubkey"]
    if not _sig_ok(bytes(payee_pubkey), signed, proof[12:76]):
        return _result(False, "bad_signature", {"req_id": req_id})
    return _result(True, "ok", {"req_id": req_id})


# --- §6 / §7 canonical text -----------------------------------------------------

def _parse_lines(payload, keys):
    """Exact key order, one key=value per line, no trailing newline."""
    try:
        text = bytes(payload).decode("utf-8")
    except UnicodeDecodeError:
        return None
    lines = text.split("\n")
    if len(lines) != len(keys):
        return None
    out = {}
    for line, key in zip(lines, keys):
        if not line.startswith(key + "="):
            return None
        out[key] = line[len(key) + 1:]
    return out


def _printable(value, max_len, allow_empty=False):
    return (allow_empty or len(value) > 0) and len(value) <= max_len and bool(PRINTABLE.match(value))


def verify_bank(payload, sig, payer_pubkey, now=None):
    payload = bytes(payload)
    f = _parse_lines(payload, BANK_KEYS)
    if f is None:
        return _result(False, "bad_format")
    if f["v"] != "1":
        return _result(False, "bad_version")
    valid = (f["rail"] == "nessie"
             and f["action"] in ("purchase", "transfer")
             and UINT.match(f["amount_cents"]) and f["amount_cents"] != "0"
             and f["currency"] == "USD"
             and _printable(f["from_acct"], 64)
             and _printable(f["payee_name"], 32)
             and HEX64.match(f["payee_ref"])
             and _printable(f["attestation"], 64)
             and HEX16.match(f["req_id"])
             and HEX32.match(f["proof_nonce"])
             and UINT.match(f["issued_at"]))
    if not valid:
        return _result(False, "bad_field", f)
    if not _sig_ok(bytes(payer_pubkey), b"bank-auth:" + payload, bytes(sig)):
        return _result(False, "bad_signature", f)
    if now is not None and abs(now - int(f["issued_at"])) > BANK_WINDOW_S:
        return _result(False, "stale", f)
    return _result(True, "ok", f)


def verify_registry(record, sig, issuer_pubkey, now=None):
    record = bytes(record)
    f = _parse_lines(record, REGISTRY_KEYS)
    if f is None:
        return _result(False, "bad_format")
    if f["v"] != "1":
        return _result(False, "bad_version")

    def hex_or_empty(v):
        return v == "" or HEX64.match(v)

    valid = (_printable(f["attestation"], 64)
             and _printable(f["display_name"], 32)
             and HEX64.match(f["device_pubkey"])
             and f["kind"] in ("merchant", "person")
             and hex_or_empty(f["solana_wallet"])
             and hex_or_empty(f["solana_ata"])
             and (f["solana_wallet"] == "") == (f["solana_ata"] == "")
             and hex_or_empty(f["bank_ref_hash"])
             and UINT.match(f["expiry"])
             and f["status"] in ("active", "revoked")
             and UINT.match(f["issued_at"]))
    if not valid:
        return _result(False, "bad_field", f)
    if not _sig_ok(bytes(issuer_pubkey), b"registry:" + record, bytes(sig)):
        return _result(False, "bad_signature", f)
    if f["status"] == "revoked":
        return _result(False, "revoked", f)
    if now is not None and now > int(f["expiry"]):
        return _result(False, "expired", f)
    if now is not None and now - int(f["issued_at"]) > RECORD_TTL_S:
        return _result(False, "stale", f)
    return _result(True, "ok", f)


def verify_registry_response(response, issuer_pubkey, now=None):
    """The JSON GET /registry/:pubkey returns: {"record": b64, "sig": b64}."""
    try:
        record = base64.b64decode(response["record"], validate=True)
        sig = base64.b64decode(response["sig"], validate=True)
    except (KeyError, TypeError, ValueError):
        return _result(False, "bad_format")
    return verify_registry(record, sig, issuer_pubkey, now)


# --- vectors ------------------------------------------------------------------

def run_vector(v, now):
    i = v["input"]
    h = bytes.fromhex
    if v["kind"] == "req":
        return verify_req(h(i["frame"]), now)
    if v["kind"] == "proof":
        return verify_proof(h(i["proof"]), h(i["chal"]), h(i["payee_pubkey"]))
    if v["kind"] == "bank":
        return verify_bank(i["payload"].encode(), h(i["sig"]), h(i["payer_pubkey"]), now)
    if v["kind"] == "registry":
        return verify_registry(i["record"].encode(), h(i["sig"]), h(i["issuer_pubkey"]), now)
    raise ValueError(f"unknown kind {v['kind']}")


def check(path=Path(__file__).resolve().parent / "vectors.json"):
    data = json.loads(path.read_text())
    failures = 0
    for v in data["vectors"]:
        got = run_vector(v, data["now"])["reason"]
        if got != v["expect"]:
            failures += 1
            print(f"FAIL {v['name']}: expected {v['expect']}, got {got}")
    total = len(data["vectors"])
    print(f"{total - failures}/{total} vectors passed")
    return failures == 0


if __name__ == "__main__":
    if "--check" in sys.argv:
        sys.exit(0 if check() else 1)
    print(__doc__)
