"""Test fixtures built on the laptop: transfer messages, registry records, payment requests, and
the case file of the checktest app (execution-plan.md, section 5.2; "Offline device tests").

Everything a payment check needs is made here with the test issuer and device keys of
test/host/vectors.json, so the device tests run on one badge with no network:

    build_transfer_msg(...)        the bytes sol_tx_build_transfer() produces (solana-payments.md)
    mutate_*(msg), REFUSAL_VECTORS the five refusal vectors of T-APR5
    make_record(fields, seed)      a canonical registry record and its issuer signature (checks.md)
    make_req(seed, ...)            a signed REQ frame (espnow.md)
    case_lua(dict)                 the text of apps/checktest's case.lua

The second half drives the checktest app on a badge (install, push one case, wait for its log
lines); t_sign.py, t_chk.py and t_sto.py share it.

    python fixtures.py             self-check: rebuilds the record, the REQ and the transfer
                                   messages of vectors.json from their inputs, byte for byte

Needs the `cryptography` package (Ed25519). The self-check needs no badge.
"""

import os
import re
import struct
import sys
import tempfile
import time

from cryptography.exceptions import InvalidSignature
from cryptography.hazmat.primitives import serialization
from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey, Ed25519PublicKey

from common import b58dec, b58enc, launch, load_vectors

_HERE = os.path.dirname(os.path.abspath(__file__))
CHECKTEST_DIR = os.path.normpath(os.path.join(_HERE, "..", "..", "apps", "checktest"))
CHECKTEST = "checktest"

TOKEN_PROGRAM_ID = b58dec("TokenkegQfeZyiNwAJbNbGKPFXCWuBvf9Ss623VQ5DA")
MEMO_PROGRAM_ID = b58dec("MemoSq4gqABAXKb96qnH8TysNcWxMyWCqXgDLGmfcHr")
COMPUTE_BUDGET_PROGRAM_ID = b58dec("ComputeBudget111111111111111111111111111111")

SOL_TX_MSG_MAX = 1232
TRANSFER_CHECKED = 12

RECORD_PREFIX = b"registry:"
PAY_REQ_PREFIX = b"pay-req:"
RECORD_MAX = 512
RAIL_SOLANA = 1
RAIL_BANK = 2


# --------------------------------------------------------------------------------------------
# Ed25519
# --------------------------------------------------------------------------------------------

def _private(seed):
    seed = bytes(seed)
    if len(seed) != 32:
        raise ValueError("an Ed25519 seed is 32 bytes, not %d" % len(seed))
    return Ed25519PrivateKey.from_private_bytes(seed)


def pubkey_of(seed):
    """The 32-byte public key of a 32-byte seed."""
    return _private(seed).public_key().public_bytes(serialization.Encoding.Raw, serialization.PublicFormat.Raw)


def sign(seed, message):
    """The 64-byte Ed25519 signature of `message` (deterministic)."""
    return _private(seed).sign(bytes(message))


def verify(pubkey, message, sig):
    """True if `sig` is a valid signature of `message` by `pubkey`."""
    try:
        Ed25519PublicKey.from_public_bytes(bytes(pubkey)).verify(bytes(sig), bytes(message))
        return True
    except (InvalidSignature, ValueError):
        return False


def short_address(key):
    """How the approval screen shortens a key: base58, first 4 + '..' + last 4 (checks.md)."""
    text = key if isinstance(key, str) else b58enc(key)
    return text[:4] + ".." + text[-4:]


# --------------------------------------------------------------------------------------------
# Transfer messages (solana-payments.md, "Message format" and "Builder")
# --------------------------------------------------------------------------------------------

def _key32(value, what):
    value = bytes(value)
    if len(value) != 32:
        raise ValueError("%s is %d bytes, not 32" % (what, len(value)))
    return value


def _compact_u16(value):
    """Shortest compact-u16 encoding: 7 bits per byte, low bits first."""
    if not 0 <= value <= 0xFFFF:
        raise ValueError("compact-u16 out of range: %d" % value)
    out = bytearray()
    while True:
        low = value & 0x7F
        value >>= 7
        if value:
            out.append(low | 0x80)
        else:
            out.append(low)
            return bytes(out)


def _read_compact_u16(data, at):
    value = 0
    for shift in (0, 7, 14):
        byte = data[at]
        at += 1
        value |= (byte & 0x7F) << shift
        if not byte & 0x80:
            return value, at
    raise ValueError("compact-u16 longer than 3 bytes")


def build_transfer_msg(payer, source, dest, mint, blockhash, amount, decimals, memo=None):
    """The legacy message of one TransferChecked, optionally followed by one Memo, with the layout
    of sol_tx_build_transfer(): key order payer; source and destination in ascending raw-byte
    order; then the readonly keys (mint, Token program, and the Memo program when there is a memo)
    in ascending raw-byte order. `amount` is in raw units. `memo` is str or bytes.

    Raises ValueError where the C builder returns 0."""
    payer, source, dest = _key32(payer, "payer"), _key32(source, "source"), _key32(dest, "destination")
    mint, blockhash = _key32(mint, "mint"), _key32(blockhash, "blockhash")
    if isinstance(memo, str):
        memo = memo.encode("utf-8")
    memo = bytes(memo) if memo else b""
    if source == dest:
        raise ValueError("source and destination are the same account")
    if not 0 < amount < 1 << 64:
        raise ValueError("amount must be 1 .. 2^64-1 raw units")
    if not 0 <= decimals <= 255:
        raise ValueError("decimals must fit one byte")
    if memo:
        memo.decode("utf-8")  # raises UnicodeDecodeError (a ValueError) when not valid UTF-8

    readonly = [mint, TOKEN_PROGRAM_ID] + ([MEMO_PROGRAM_ID] if memo else [])
    ordered = sorted(readonly)
    src_first = source < dest
    keys = [payer] + ([source, dest] if src_first else [dest, source]) + ordered

    out = bytearray([1, 0, len(readonly)])
    out.append(len(keys))
    for key in keys:
        out += key
    out += blockhash
    out.append(2 if memo else 1)
    out.append(3 + ordered.index(TOKEN_PROGRAM_ID))
    out.append(4)
    out.append(1 if src_first else 2)            # source
    out.append(3 + ordered.index(mint))          # mint
    out.append(2 if src_first else 1)            # destination
    out.append(0)                                # authority = payer
    out.append(10)
    out.append(TRANSFER_CHECKED)
    out += struct.pack("<Q", amount)
    out.append(decimals)
    if memo:
        out.append(3 + ordered.index(MEMO_PROGRAM_ID))
        out.append(0)                            # no accounts
        out += _compact_u16(len(memo))
        out += memo
    if len(out) > SOL_TX_MSG_MAX:
        raise ValueError("message is %d bytes; the limit is %d" % (len(out), SOL_TX_MSG_MAX))
    return bytes(out)


def parse_msg(msg):
    """A legacy message as a dict: header (3 ints), keys (list of 32-byte values), blockhash, and
    instructions as (program_index, [account indices], data). For the mutators and the self-check;
    it is not the firmware's decoder and checks nothing but the framing."""
    msg = bytes(msg)
    header = list(msg[0:3])
    count, at = _read_compact_u16(msg, 3)
    keys = [msg[at + 32 * i:at + 32 * (i + 1)] for i in range(count)]
    at += 32 * count
    blockhash = msg[at:at + 32]
    at += 32
    ix_count, at = _read_compact_u16(msg, at)
    instructions = []
    for _ in range(ix_count):
        program = msg[at]
        at += 1
        n_accounts, at = _read_compact_u16(msg, at)
        accounts = list(msg[at:at + n_accounts])
        at += n_accounts
        n_data, at = _read_compact_u16(msg, at)
        data = msg[at:at + n_data]
        at += n_data
        instructions.append((program, accounts, data))
    if at != len(msg) or len(blockhash) != 32 or any(len(key) != 32 for key in keys):
        raise ValueError("not a well-framed legacy message")
    return {"header": header, "keys": keys, "blockhash": blockhash, "instructions": instructions}


def serialize_msg(parts):
    """The inverse of parse_msg()."""
    out = bytearray(parts["header"])
    out += _compact_u16(len(parts["keys"]))
    for key in parts["keys"]:
        out += key
    out += parts["blockhash"]
    out += _compact_u16(len(parts["instructions"]))
    for program, accounts, data in parts["instructions"]:
        out.append(program)
        out += _compact_u16(len(accounts))
        out += bytes(accounts)
        out += _compact_u16(len(data))
        out += data
    return bytes(out)


# --- The five refusal vectors of T-APR5. Each takes a valid 5-key transfer message. -----------

def mutate_second_instruction(msg):
    """Adds a second instruction that is not a Memo (a ComputeBudget SetComputeUnitPrice): one more
    readonly key and one more instruction. The decoder refuses it (SOL_TX_ERR_PROGRAM)."""
    parts = parse_msg(msg)
    parts["header"][2] += 1
    parts["keys"].append(COMPUTE_BUDGET_PROGRAM_ID)
    price = bytes([3]) + struct.pack("<Q", 1000)
    parts["instructions"].append((len(parts["keys"]) - 1, [], price))
    return serialize_msg(parts)


def mutate_wrong_mint(msg):
    """Replaces the mint key with one that is in no token table. The message still decodes; the
    check chain refuses it as UNKNOWN TOKEN."""
    parts = parse_msg(msg)
    token_ix = [ix for ix in parts["instructions"] if parts["keys"][ix[0]] == TOKEN_PROGRAM_ID]
    if len(token_ix) != 1 or len(token_ix[0][1]) != 4:
        raise ValueError("no single Token instruction with four accounts")
    mint_index = token_ix[0][1][1]
    parts["keys"][mint_index] = bytes(byte ^ 0x5A for byte in parts["keys"][mint_index])
    return serialize_msg(parts)


def mutate_two_signers(msg):
    """Sets num_required_signatures to 2 (SOL_TX_ERR_HEADER)."""
    msg = bytearray(msg)
    msg[0] = 2
    return bytes(msg)


def mutate_v0(msg):
    """The same transfer as a versioned (v0) message: the version byte 0x80 in front and an empty
    address-table-lookup list behind (SOL_TX_ERR_VERSION)."""
    return b"\x80" + bytes(msg) + b"\x00"


def mutate_trailing(msg):
    """One byte after the last instruction (SOL_TX_ERR_TRAILING)."""
    return bytes(msg) + b"\x00"


# (name, mutator, the headline the approval must show)
REFUSAL_VECTORS = [
    ("second instruction", mutate_second_instruction, "CANNOT READ PAYMENT"),
    ("wrong mint", mutate_wrong_mint, "UNKNOWN TOKEN"),
    ("two signers", mutate_two_signers, "CANNOT READ PAYMENT"),
    ("v0 message", mutate_v0, "CANNOT READ PAYMENT"),
    ("trailing bytes", mutate_trailing, "CANNOT READ PAYMENT"),
]


# --------------------------------------------------------------------------------------------
# Registry record (checks.md, "Registry record"; backend.md, "/registry: exact behaviour")
# --------------------------------------------------------------------------------------------

RECORD_KEYS = ("v", "attestation", "display_name", "device_pubkey", "kind", "solana_wallet",
               "solana_ata", "bank_ref_hash", "expiry", "status", "issued_at")
_RECORD_DEFAULTS = {"v": 1, "attestation": "none", "kind": "merchant", "solana_wallet": "",
                    "solana_ata": "", "bank_ref_hash": "", "status": "active"}


def record_text(fields):
    """The canonical record text: the eleven key=value lines in order, '\\n' between them, no
    trailing newline. A bytes value is written as lower-case hex, an int as decimal, None as empty.
    Missing keys take: v=1, attestation=none, kind=merchant, status=active, the three optional hex
    fields empty. display_name, device_pubkey, expiry and issued_at must be given."""
    unknown = set(fields) - set(RECORD_KEYS)
    if unknown:
        raise ValueError("not record keys: %s" % ", ".join(sorted(unknown)))
    lines = []
    for key in RECORD_KEYS:
        if key in fields:
            value = fields[key]
        elif key in _RECORD_DEFAULTS:
            value = _RECORD_DEFAULTS[key]
        else:
            raise ValueError("record field %s is required" % key)
        if value is None:
            value = ""
        elif isinstance(value, (bytes, bytearray)):
            value = bytes(value).hex()
        elif isinstance(value, bool):
            raise ValueError("record field %s is a bool" % key)
        elif isinstance(value, int):
            value = "%d" % value
        if "\n" in value:
            raise ValueError("record field %s contains a newline" % key)
        lines.append("%s=%s" % (key, value))
    return "\n".join(lines)


def make_record(fields, issuer_seed):
    """(record bytes, 64-byte signature). The signature is the issuer's over "registry:" followed
    by the record bytes."""
    record = record_text(fields).encode("utf-8")
    return record, sign(issuer_seed, RECORD_PREFIX + record)


def parse_record_text(text):
    """A canonical record text as a dict of strings (the inverse of record_text for str values)."""
    if isinstance(text, (bytes, bytearray)):
        text = bytes(text).decode("utf-8")
    fields = {}
    for line in text.split("\n"):
        key, _, value = line.partition("=")
        fields[key] = value
    return fields


# --------------------------------------------------------------------------------------------
# REQ frame (espnow.md, "REQ")
# --------------------------------------------------------------------------------------------

def make_req(device_seed, amount, currency, req_id, expiry, name, rail=RAIL_SOLANA):
    """A REQ frame signed by the device key: `amount` in raw units, `currency` 1 to 4 ASCII
    characters, `req_id` 8 bytes, `expiry` unix seconds, `name` 1 to 32 printable ASCII characters.
    The signature covers "pay-req:" followed by the frame from its header to the end of the name."""
    req_id = bytes(req_id)
    currency_bytes = currency.encode("ascii")
    name_bytes = name.encode("ascii")
    if len(req_id) != 8:
        raise ValueError("req_id is %d bytes, not 8" % len(req_id))
    if not 1 <= len(currency_bytes) <= 4:
        raise ValueError("currency must be 1 to 4 characters")
    if not 1 <= len(name_bytes) <= 32:
        raise ValueError("name must be 1 to 32 characters")
    signed = (b"VK" + bytes([1, 1, rail]) + pubkey_of(device_seed) + struct.pack("<Q", amount)
              + currency_bytes.ljust(4, b"\0") + req_id + struct.pack("<I", expiry)
              + bytes([len(name_bytes)]) + name_bytes)
    return signed + sign(device_seed, PAY_REQ_PREFIX + signed)


# --------------------------------------------------------------------------------------------
# case.lua
# --------------------------------------------------------------------------------------------

_LUA_NAME = re.compile(r"^[A-Za-z_][A-Za-z0-9_]*$")


def _lua_string(text):
    out = ['"']
    for byte in text.encode("utf-8"):
        char = chr(byte)
        if char in '"\\':
            out.append("\\" + char)
        elif 0x20 <= byte <= 0x7E:
            out.append(char)
        else:
            out.append("\\%03d" % byte)
    out.append('"')
    return "".join(out)


def _lua_value(value, indent):
    if isinstance(value, bool):
        return "true" if value else "false"
    if isinstance(value, int):
        if not -(1 << 31) <= value < 1 << 31:
            raise ValueError("%d does not fit the badge's 32-bit Lua integers; pass a string" % value)
        return "%d" % value
    if isinstance(value, (bytes, bytearray)):
        return _lua_string(bytes(value).hex())
    if isinstance(value, str):
        return _lua_string(value)
    if isinstance(value, dict):
        pad = "  " * (indent + 1)
        rows = []
        for key, item in value.items():
            if item is None:
                continue
            if not isinstance(key, str) or not _LUA_NAME.match(key):
                raise ValueError("not a Lua field name: %r" % (key,))
            rows.append("%s%s = %s,\n" % (pad, key, _lua_value(item, indent + 1)))
        return "{\n" + "".join(rows) + "  " * indent + "}"
    raise ValueError("cannot write %r into case.lua" % (value,))


def case_lua(case):
    """The text of case.lua: `return { ... }` with the case's fields. A bytes value is written as
    a lower-case hex string (so {"msg_hex": msg} works), a nested dict as a table, None is left out.

    Fields the checktest app reads: msg_hex or transfer, record_hex, sig_hex, req_hex, delay_ms,
    flush_draw, history, storage_probe (apps/checktest/main.lua)."""
    if not isinstance(case, dict):
        raise ValueError("a case is a dict")
    return "-- Written by test/device/fixtures.py for one test case.\nreturn %s\n" % _lua_value(case, 0)


# --------------------------------------------------------------------------------------------
# The test keys and the standard payment of the device tests
# --------------------------------------------------------------------------------------------

class Keys:
    """The test keys and accounts of vectors.json, as bytes."""

    def __init__(self, vectors=None):
        vectors = vectors or load_vectors()
        self.vectors = vectors
        self.issuer_seed = bytes.fromhex(vectors["vk"]["issuer"]["seed_hex"])
        self.issuer_pub = bytes.fromhex(vectors["vk"]["issuer"]["pubkey_hex"])
        self.device_seed = bytes.fromhex(vectors["vk"]["device"]["seed_hex"])   # the payee (merchant)
        self.device_pub = bytes.fromhex(vectors["vk"]["device"]["pubkey_hex"])
        self.mint = bytes.fromhex(vectors["tx"]["mint_hex"])                   # the test token (2 decimals)
        self.merchant_wallet = bytes.fromhex(vectors["tx"]["dest_owner_hex"])
        self.merchant_ata = bytes.fromhex(vectors["tx"]["dest_ata1_hex"])      # the record's solana_ata
        self.source_ata = bytes.fromhex(vectors["tx"]["source_ata3_hex"])      # stands for the badge's own account
        self.blockhash = bytes.fromhex(vectors["tx"]["blockhash_hex"])
        self.merchant_name = parse_record_text(vectors["vk"]["record"]["text"])["display_name"]
        self.decimals = 2
        self.symbol = "HACK"

    def transfer(self, payer, amount_raw=1000, dest=None, memo=None, mint=None):
        """A transfer from `payer` (the badge under test: account 0 must be its own key) to the
        merchant's token account. 1000 raw units are 10.00 HACK."""
        return build_transfer_msg(payer, self.source_ata, dest or self.merchant_ata, mint or self.mint,
                                  self.blockhash, amount_raw, self.decimals, memo)

    def record(self, issued_at, expiry, **overrides):
        """(record, signature) for the merchant, signed by the test issuer."""
        fields = {
            "attestation": parse_record_text(self.vectors["vk"]["record"]["text"])["attestation"],
            "display_name": self.merchant_name,
            "device_pubkey": self.device_pub,
            "kind": "merchant",
            "solana_wallet": self.merchant_wallet,
            "solana_ata": self.merchant_ata,
            "expiry": expiry,
            "status": "active",
            "issued_at": issued_at,
        }
        fields.update(overrides)
        return make_record(fields, self.issuer_seed)

    def req(self, amount_raw, expiry, req_id=None, name=None, currency=None):
        """A REQ frame from the merchant's device key."""
        return make_req(self.device_seed, amount_raw, currency or self.symbol, req_id or os.urandom(8),
                        expiry, name or self.merchant_name)


# --------------------------------------------------------------------------------------------
# Driving the checktest app (apps/checktest). These need a badge.
# --------------------------------------------------------------------------------------------

# How long a case waits after the app starts before it acts: common.launch() must first see the
# app running with no approval open.
CASE_DELAY_MS = 1500
# begin_solana with a record and a request verifies two signatures inside one Lua callback and the
# badge answers no command meanwhile, so the wait for its log line sends nothing.
BEGIN_TIMEOUT_S = 40

_CT = r"\[app\] CT "
# What upstream's runtime logs when a callback raised: the error text, then this line.
_LUA_STOPPED = r"\[lua\] stopping '[^']*' after error"


def badge_pubkey(badge):
    """The badge's own key (VKINFO pubkey), 32 bytes."""
    text = badge.info().get("pubkey", "")
    key = b58dec(text) if text else b""
    assert len(key) == 32, "VKINFO pubkey is not a 32-byte key: %r" % text
    return key


def set_time(badge, unix_s=None):
    """VKTIME: sets the badge's clock (source sntp). Now, unless a time is given."""
    unix_s = int(time.time()) if unix_s is None else int(unix_s)
    badge.ok("VKTIME %d" % unix_s)
    return unix_s


def stop_app(badge, app_id=CHECKTEST):
    """Closes an open approval with CANCEL and stops `app_id` if it is the running app."""
    state = badge.state()
    if state["modal"]:
        if state["phase"] != "RESULT":
            badge.btn("b", "tap")
        state = badge.wait_state(lambda s: not s["modal"], timeout=5)
    if state["app"] == app_id:
        badge.stop()
        badge.wait_state(lambda s: s["app"] != app_id, timeout=5)


def install_checktest(badge):
    """Pushes the checktest app (without a case: started like this it logs 'CT nocase')."""
    stop_app(badge)
    badge.push(CHECKTEST_DIR, CHECKTEST)


def push_case(badge, case):
    """Writes one case as /apps/checktest/case.lua. The app must not be running."""
    with tempfile.TemporaryDirectory() as folder:
        with open(os.path.join(folder, "case.lua"), "w", encoding="utf-8") as handle:
            handle.write(case_lua(case))
        badge.push(folder, CHECKTEST)


def lua_errors(badge):
    """The Lua runtime's lines about an app that raised an error, since the last clear_log()."""
    lines = badge.log()
    stops = [i for i, line in enumerate(lines) if re.search(_LUA_STOPPED, line)]
    return [line for i, line in enumerate(lines) if "[lua]" in line and any(i in (stop - 1, stop) for stop in stops)]


def ct_wait(badge, pattern, timeout=10):
    """The first 'CT <pattern>' log line since the last clear_log(), as the text after 'CT '. Fails
    at once if the Lua runtime stops the app for an error instead."""
    line = badge.wait_log("%s(?:%s)|%s" % (_CT, pattern, _LUA_STOPPED), timeout=timeout)
    found = re.search(_CT, line)
    assert found, "the app raised an error instead of logging 'CT %s': %s" % (pattern, " / ".join(lua_errors(badge)))
    return line[found.end():].strip()


def ct_ticks(badge):
    """The numbers of every 'CT tick <n>' line logged since the last clear_log()."""
    return [int(match.group(1)) for match in (re.search(_CT + r"tick (\d+)", line) for line in badge.log()) if match]


def start_case(badge, case):
    """Stops the app, pushes the case, clears the log and starts the app. Returns the case as
    pushed (delay_ms defaults to CASE_DELAY_MS)."""
    case = dict(case)
    case.setdefault("delay_ms", CASE_DELAY_MS)
    stop_app(badge)
    push_case(badge, case)
    badge.clear_log()
    launch(badge, CHECKTEST)
    return case


def open_case(badge, case):
    """start_case(), then waits for the app's begin_solana to open the approval. Returns the
    VKSTATE once the approval is ARMED."""
    start_case(badge, case)
    result = ct_wait(badge, r"begin (?:ok|err \S+)", timeout=BEGIN_TIMEOUT_S)
    assert result == "begin ok", "begin_solana was refused: %s" % result
    return badge.wait_state(lambda s: s["modal"] and s["phase"] == "ARMED", timeout=5)


def poll_result(badge, timeout=15):
    """What the app's wallet.poll() returned after the approval closed: ("sig", 64 bytes) or
    ("err", reason)."""
    result = ct_wait(badge, r"poll (?:sig [0-9a-f]+|err \S+)", timeout=timeout)
    kind, value = result.split()[1:3]
    if kind == "sig":
        sig = bytes.fromhex(value)
        assert len(sig) == 64, "the signature the app logged is %d bytes" % len(sig)
        return "sig", sig
    return "err", value


def cancel(badge):
    """CANCEL on the open approval; returns the poll result the app then logs."""
    badge.btn("b", "tap")
    badge.wait_state(lambda s: not s["modal"], timeout=5)
    return poll_result(badge)


def lines_of(state):
    """VKSTATE lines as a dict: label -> value."""
    return {label: value for label, value in state["lines"]}


def log_ms(badge, pattern):
    """The integers captured by `pattern` (one group) in every log line since the last clear_log()."""
    return [int(match.group(1)) for match in (re.search(pattern, line) for line in badge.log()) if match]


# --------------------------------------------------------------------------------------------
# Self-check
# --------------------------------------------------------------------------------------------

def selfcheck():
    vectors = load_vectors()
    keys = Keys(vectors)
    vk = vectors["vk"]
    tx = vectors["tx"]

    # Keys: the seeds give the public keys of vectors.json.
    assert pubkey_of(keys.issuer_seed) == keys.issuer_pub, "issuer public key"
    assert pubkey_of(keys.device_seed) == keys.device_pub, "device public key"
    assert b58enc(keys.issuer_pub) == vk["issuer"]["pubkey_b58"], "issuer base58"
    assert b58enc(keys.mint) == tx["mint"], "mint base58"

    # Transfer messages: the kit-built vectors, byte for byte.
    payer = bytes.fromhex(tx["payer_hex"])
    plain = build_transfer_msg(payer, keys.source_ata, keys.merchant_ata, keys.mint, keys.blockhash, 1000, 2)
    assert plain.hex() == tx["legacy_1000"]["messageHex"], "build_transfer_msg differs from tx.legacy_1000"
    assert len(plain) == 214
    large = build_transfer_msg(payer, keys.source_ata, keys.merchant_ata, keys.mint, keys.blockhash, 50000, 2)
    assert large.hex() == tx["legacy_50000"]["messageHex"], "build_transfer_msg differs from tx.legacy_50000"
    memo = build_transfer_msg(payer, keys.source_ata, keys.merchant_ata, keys.mint, keys.blockhash, 1000, 2,
                              memo=vk["memo_tx"]["memo"])
    assert memo.hex() == vk["memo_tx"]["messageHex"], "build_transfer_msg differs from vk.memo_tx"
    assert keys.transfer(payer) == plain

    # A two-byte memo length, and the argument checks of the C builder.
    long_memo = build_transfer_msg(payer, keys.source_ata, keys.merchant_ata, keys.mint, keys.blockhash, 1, 2,
                                   memo="x" * 200)
    parts = parse_msg(long_memo)
    assert parts["instructions"][1][2] == b"x" * 200 and serialize_msg(parts) == long_memo
    for bad in (dict(dest=keys.source_ata), dict(amount=0), dict(memo=b"\xff"), dict(memo="x" * 983)):
        args = dict(payer=payer, source=keys.source_ata, dest=keys.merchant_ata, mint=keys.mint,
                    blockhash=keys.blockhash, amount=1000, decimals=2, memo=None)
        args.update(bad)
        try:
            build_transfer_msg(**args)
            raise AssertionError("build_transfer_msg accepted %r" % (bad,))
        except ValueError:
            pass
    assert len(build_transfer_msg(payer, keys.source_ata, keys.merchant_ata, keys.mint, keys.blockhash, 1, 2,
                                  memo="x" * 982)) == SOL_TX_MSG_MAX

    # Key order when the destination sorts before the source and the Token program before the mint.
    low, high = bytes([1]) * 32, bytes([0xF0]) * 32
    swapped = parse_msg(build_transfer_msg(payer, high, low, bytes([0x10]) * 32, keys.blockhash, 7, 2))
    assert swapped["keys"][1:3] == [low, high]
    assert swapped["keys"][3] == TOKEN_PROGRAM_ID and swapped["instructions"][0] == (
        3, [2, 4, 1, 0], bytes([TRANSFER_CHECKED]) + struct.pack("<Q", 7) + bytes([2]))

    # The refusal vectors: each differs from the valid message in the way its name says.
    assert parse_msg(plain) == parse_msg(serialize_msg(parse_msg(plain)))
    second = parse_msg(mutate_second_instruction(plain))
    assert second["header"] == [1, 0, 3] and len(second["keys"]) == 6 and len(second["instructions"]) == 2
    assert second["keys"][second["instructions"][1][0]] == COMPUTE_BUDGET_PROGRAM_ID
    wrong = parse_msg(mutate_wrong_mint(plain))
    assert keys.mint not in wrong["keys"] and len(wrong["keys"]) == 5
    assert [key for key in wrong["keys"] if key not in parse_msg(plain)["keys"]] == [wrong["keys"][3]]
    assert mutate_two_signers(plain)[0] == 2 and mutate_two_signers(plain)[1:] == plain[1:]
    assert mutate_v0(plain).hex() == tx["v0_1000"]["messageHex"], "mutate_v0 differs from tx.v0_1000"
    assert mutate_trailing(plain) == plain + b"\x00"
    assert len({mutator(plain) for _, mutator, _ in REFUSAL_VECTORS} | {plain}) == 6

    # Registry record: the canonical text and the issuer signature of vectors.json.
    assert bytes.fromhex(vk["record"]["hex"]).decode("utf-8") == vk["record"]["text"]
    fields = parse_record_text(vk["record"]["text"])
    assert tuple(fields) == RECORD_KEYS
    record, sig = make_record(fields, keys.issuer_seed)
    assert record.hex() == vk["record"]["hex"], "make_record text differs from vk.record"
    assert sig.hex() == vk["record"]["sig_hex"], "make_record signature differs from vk.record"
    typed, typed_sig = keys.record(vk["record"]["issued_at"], vk["record"]["expiry"])
    assert typed == record and typed_sig == sig, "Keys.record differs from vk.record"
    assert verify(keys.issuer_pub, RECORD_PREFIX + record, sig) and len(record) <= RECORD_MAX
    assert not verify(keys.issuer_pub, record, sig)
    revoked, _ = keys.record(1, 2, status="revoked")
    assert b"\nstatus=revoked\n" in revoked and revoked.endswith(b"issued_at=1")

    # REQ: the signed frame of vectors.json.
    req = make_req(keys.device_seed, int(vk["req"]["amount"]), vk["req"]["currency"],
                   bytes.fromhex(vk["proof"]["req_id_hex"]), vk["req"]["expiry"], vk["req"]["name"])
    assert req.hex() == vk["req"]["frame_hex"], "make_req differs from vk.req"
    assert len(req) == vk["req"]["signed_len"] + 64
    assert verify(keys.device_pub, PAY_REQ_PREFIX + req[:-64], req[-64:])
    assert keys.req(1000, vk["req"]["expiry"], req_id=bytes.fromhex(vk["proof"]["req_id_hex"])) == req
    assert len(make_req(keys.device_seed, 1, "USD", bytes(8), 1, "x", rail=RAIL_BANK)) == 127

    # case.lua
    text = case_lua({"msg_hex": b"\x01\xab", "delay_ms": 1500, "flush_draw": True, "skip": None,
                     "transfer": {"amount": "10.00", "memo": 'a"b\\c\n'}})
    assert text == ('-- Written by test/device/fixtures.py for one test case.\n'
                    'return {\n'
                    '  msg_hex = "01ab",\n'
                    '  delay_ms = 1500,\n'
                    '  flush_draw = true,\n'
                    '  transfer = {\n'
                    '    amount = "10.00",\n'
                    '    memo = "a\\"b\\\\c\\010",\n'
                    '  },\n'
                    '}\n'), text
    for bad in ({"a-b": 1}, {"n": 1 << 31}, {"f": 1.5}, [1]):
        try:
            case_lua(bad)
            raise AssertionError("case_lua accepted %r" % (bad,))
        except ValueError:
            pass

    assert short_address(keys.merchant_ata) == "2awX..6wrr"
    print("fixtures ok")


if __name__ == "__main__":
    selfcheck()
    sys.exit(0)
