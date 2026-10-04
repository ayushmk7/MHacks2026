// R5 verifier: REQ, PROOF, bank payload and registry record, per docs/specs/00-Interfaces.md §5-§7.
// Python twin: harness/vk_verify.py (attack console). Both are held to harness/vectors.json by
// verify.test.js and `vk_verify.py --check`; keep reasons and check order identical in both.
//
// Every function returns { ok, reason, fields }. Reasons: ok, bad_length, bad_magic, bad_version,
// bad_type, bad_field, bad_format, mismatch, bad_signature, expired, stale, revoked (meanings in
// vk_verify.py). Checks run structure -> signature -> time/status; time checks only when `now`
// (unix seconds) is passed. Byte inputs are Uint8Array/Buffer; pubkeys are raw 32 bytes.
import { createPublicKey, verify as edVerify } from 'node:crypto';

const MAGIC = [0x56, 0x4b]; // "VK"
const VERSION = 1;
const TYPE = { REQ: 1, CHAL: 2, PROOF: 3 };
const CHAL_LEN = 60, PROOF_LEN = 76;
const REQ_FIXED = 4 + 1 + 32 + 8 + 4 + 8 + 4 + 1; // header .. name_len
const CURRENCY = { 1: 'HACK', 2: 'USD\0' };

export const APPROVAL_TIMEOUT_S = 45;
const BANK_WINDOW_S = APPROVAL_TIMEOUT_S + 30; // §6
export const RECORD_TTL_S = 30; // §4

const HEX64 = /^[0-9a-f]{64}$/, HEX32 = /^[0-9a-f]{32}$/, HEX16 = /^[0-9a-f]{16}$/;
const UINT = /^(0|[1-9][0-9]*)$/, PRINTABLE = /^[\x20-\x7e]*$/;

const BANK_KEYS = ['v', 'rail', 'action', 'amount_cents', 'currency', 'from_acct', 'payee_name',
  'payee_ref', 'attestation', 'req_id', 'proof_nonce', 'issued_at'];
const REGISTRY_KEYS = ['v', 'attestation', 'display_name', 'device_pubkey', 'kind', 'solana_wallet',
  'solana_ata', 'bank_ref_hash', 'expiry', 'status', 'issued_at'];

const result = (ok, reason, fields = {}) => ({ ok, reason, fields });
const enc = s => Buffer.from(s, 'utf8');
const SPKI_ED25519 = Buffer.from('302a300506032b6570032100', 'hex');

export function sigOk(pubkey, message, sig) {
  if (pubkey?.length !== 32 || sig?.length !== 64) return false;
  try {
    const key = createPublicKey({ key: Buffer.concat([SPKI_ED25519, Buffer.from(pubkey)]), format: 'der', type: 'spki' });
    return edVerify(null, Buffer.from(message), key, Buffer.from(sig));
  } catch { return false; }
}

function header(frame, type) {
  if (frame.length < 4) return 'bad_length';
  if (frame[0] !== MAGIC[0] || frame[1] !== MAGIC[1]) return 'bad_magic';
  if (frame[2] !== VERSION) return 'bad_version';
  if (frame[3] !== type) return 'bad_type';
  return null;
}

// --- §5 ESP-NOW ---------------------------------------------------------------

export function parseChal(chal) {
  const frame = Buffer.from(chal);
  const err = header(frame, TYPE.CHAL);
  if (err) return result(false, err);
  if (frame.length !== CHAL_LEN) return result(false, 'bad_length');
  return result(true, 'ok', { reqId: frame.subarray(4, 12), nonce: frame.subarray(12, 28), payerPubkey: frame.subarray(28, 60) });
}

export function verifyReq(req, { now } = {}) {
  const frame = Buffer.from(req);
  const err = header(frame, TYPE.REQ);
  if (err) return result(false, err);
  if (frame.length < REQ_FIXED) return result(false, 'bad_length');
  const rail = frame[4];
  const payeePubkey = frame.subarray(5, 37);
  const amount = frame.readBigUInt64LE(37);
  const currency = frame.subarray(45, 49).toString('latin1');
  const reqId = frame.subarray(49, 57);
  const expiry = frame.readUInt32LE(57);
  const nameLen = frame[61];
  if (nameLen > 32) return result(false, 'bad_field');
  const signedEnd = REQ_FIXED + nameLen;
  if (frame.length !== signedEnd + 64) return result(false, 'bad_length');
  const name = frame.subarray(REQ_FIXED, signedEnd).toString('latin1');
  if (CURRENCY[rail] === undefined || currency !== CURRENCY[rail] || amount === 0n) return result(false, 'bad_field');
  if (!PRINTABLE.test(name)) return result(false, 'bad_field');
  const fields = { rail, payeePubkey, amount, currency, reqId, expiry, name };
  if (!sigOk(payeePubkey, Buffer.concat([enc('pay-req:'), frame.subarray(0, signedEnd)]), frame.subarray(signedEnd))) {
    return result(false, 'bad_signature', fields);
  }
  if (now !== undefined && now > expiry) return result(false, 'expired', fields);
  return result(true, 'ok', fields);
}

// A PROOF answers one CHAL and must be signed by the payee named in the REQ.
export function verifyProof(proof, chal, payeePubkey) {
  const frame = Buffer.from(proof);
  const err = header(frame, TYPE.PROOF);
  if (err) return result(false, err);
  if (frame.length !== PROOF_LEN) return result(false, 'bad_length');
  const challenge = parseChal(chal);
  if (!challenge.ok) return challenge;
  const c = challenge.fields;
  const reqId = frame.subarray(4, 12);
  if (!reqId.equals(c.reqId)) return result(false, 'mismatch');
  const signed = Buffer.concat([enc('pay-proof:'), c.reqId, c.nonce, c.payerPubkey]);
  if (!sigOk(payeePubkey, signed, frame.subarray(12, 76))) return result(false, 'bad_signature', { reqId });
  return result(true, 'ok', { reqId });
}

// --- §6 / §7 canonical text -----------------------------------------------------

// Exact key order, one key=value per line, no trailing newline. null when malformed.
function parseLines(bytes, keys) {
  let text;
  try { text = new TextDecoder('utf-8', { fatal: true }).decode(bytes); } catch { return null; }
  const lines = text.split('\n');
  if (lines.length !== keys.length) return null;
  const out = {};
  for (let i = 0; i < keys.length; i++) {
    if (!lines[i].startsWith(`${keys[i]}=`)) return null;
    out[keys[i]] = lines[i].slice(keys[i].length + 1);
  }
  return out;
}

const printable = (v, max) => v.length > 0 && v.length <= max && PRINTABLE.test(v);
const hexOrEmpty = v => v === '' || HEX64.test(v);

export function verifyBank(payload, sig, payerPubkey, { now } = {}) {
  const bytes = Buffer.from(payload);
  const f = parseLines(bytes, BANK_KEYS);
  if (!f) return result(false, 'bad_format');
  if (f.v !== '1') return result(false, 'bad_version');
  const valid = f.rail === 'nessie'
    && (f.action === 'purchase' || f.action === 'transfer')
    && UINT.test(f.amount_cents) && f.amount_cents !== '0'
    && f.currency === 'USD'
    && printable(f.from_acct, 64)
    && printable(f.payee_name, 32)
    && HEX64.test(f.payee_ref)
    && printable(f.attestation, 64)
    && HEX16.test(f.req_id)
    && HEX32.test(f.proof_nonce)
    && UINT.test(f.issued_at);
  if (!valid) return result(false, 'bad_field', f);
  if (!sigOk(payerPubkey, Buffer.concat([enc('bank-auth:'), bytes]), sig)) return result(false, 'bad_signature', f);
  if (now !== undefined && Math.abs(now - Number(f.issued_at)) > BANK_WINDOW_S) return result(false, 'stale', f);
  return result(true, 'ok', f);
}

export function verifyRegistry(record, sig, issuerPubkey, { now } = {}) {
  const bytes = Buffer.from(record);
  const f = parseLines(bytes, REGISTRY_KEYS);
  if (!f) return result(false, 'bad_format');
  if (f.v !== '1') return result(false, 'bad_version');
  const valid = printable(f.attestation, 64)
    && printable(f.display_name, 32)
    && HEX64.test(f.device_pubkey)
    && (f.kind === 'merchant' || f.kind === 'person')
    && hexOrEmpty(f.solana_wallet)
    && hexOrEmpty(f.solana_ata)
    && (f.solana_wallet === '') === (f.solana_ata === '')
    && hexOrEmpty(f.bank_ref_hash)
    && UINT.test(f.expiry)
    && (f.status === 'active' || f.status === 'revoked')
    && UINT.test(f.issued_at);
  if (!valid) return result(false, 'bad_field', f);
  if (!sigOk(issuerPubkey, Buffer.concat([enc('registry:'), bytes]), sig)) return result(false, 'bad_signature', f);
  if (f.status === 'revoked') return result(false, 'revoked', f);
  if (now !== undefined && now > Number(f.expiry)) return result(false, 'expired', f);
  if (now !== undefined && now - Number(f.issued_at) > RECORD_TTL_S) return result(false, 'stale', f);
  return result(true, 'ok', f);
}

// The JSON GET /registry/:pubkey returns: { record: base64, sig: base64 }.
export function verifyRegistryResponse(response, issuerPubkey, opts) {
  const b64 = /^[A-Za-z0-9+/]*={0,2}$/;
  if (typeof response?.record !== 'string' || typeof response?.sig !== 'string'
    || !b64.test(response.record) || !b64.test(response.sig)) return result(false, 'bad_format');
  return verifyRegistry(Buffer.from(response.record, 'base64'), Buffer.from(response.sig, 'base64'), issuerPubkey, opts);
}

// R3: a badge signature over a serialized LEGACY Solana message (00 §3, first row: no prefix).
// Checks the message is legacy with exactly one required signer, that signer is `pubkey`, and the
// signature verifies. The full decode (one transferChecked, pinned mint) is the firmware's job.
export function verifySolanaMessage(message, sig, pubkey) {
  const msg = Buffer.from(message);
  if (msg.length < 3 + 1 + 32) return result(false, 'bad_length');
  if (msg[0] & 0x80) return result(false, 'bad_version');   // versioned (v0) messages set the top bit
  if (msg[0] !== 1) return result(false, 'bad_field');       // num_required_signatures
  // header (3) then compact-u16 account count; < 128 accounts fits in one byte
  if (msg[3] === 0 || msg[3] & 0x80) return result(false, 'bad_field');
  const signer = msg.subarray(4, 36);
  if (!signer.equals(Buffer.from(pubkey))) return result(false, 'mismatch');
  if (!sigOk(pubkey, msg, sig)) return result(false, 'bad_signature');
  return result(true, 'ok', { signer });
}
