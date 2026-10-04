import { createPublicKey, verify } from 'node:crypto';
import { address, getAddressDecoder, getAddressEncoder } from '@solana/kit';

// Wire formats shared with the badge firmware: 00-Interfaces §3 (signing prefixes), §5 (REQ frame, little-endian)
// and §6 (bank payload). The firmware parsers are strict, so these are too: anything not byte-exact is refused.

export const PREFIX = Object.freeze({
  BANK_AUTH: 'bank-auth:', PAY_REQ: 'pay-req:', PAY_PROOF: 'pay-proof:', REGISTRY: 'registry:', REGISTRY_C: 'registry-c:',
});

export const RAIL = Object.freeze({ SOLANA: 1, BANK: 2 });
const REQ_HEAD = 62;              // magic 2 + version 1 + type 1 + rail 1 + payee 32 + amount 8 + currency 4 + req_id 8 + expiry 4 + name_len 1
const PRINTABLE = /^[\x20-\x7e]*$/;

const bad = (code, message) => { throw Object.assign(new Error(message), { code }); };
export const b58 = bytes => getAddressDecoder().decode(bytes);
export const fromB58 = s => Buffer.from(getAddressEncoder().encode(address(s)));   // address() throws on anything but a 32-byte base58 key

// REQ (type 1): header..name is what the payee signed under "pay-req:". Throws { code: 'bad_req' }.
export function parseReq(buf) {
  if (!Buffer.isBuffer(buf)) buf = Buffer.from(buf ?? []);
  if (buf.length < REQ_HEAD + 64) bad('bad_req', `REQ is ${buf.length} bytes, too short`);
  if (buf[0] !== 0x56 || buf[1] !== 0x4b) bad('bad_req', 'REQ magic is not "VK"');
  if (buf[2] !== 1) bad('bad_req', `REQ version ${buf[2]} is not 1`);
  if (buf[3] !== 1) bad('bad_req', `frame type ${buf[3]} is not REQ`);
  const rail = buf[4];
  if (rail !== RAIL.SOLANA && rail !== RAIL.BANK) bad('bad_req', `REQ rail ${rail} is unknown`);
  const nameLen = buf[61];
  if (nameLen > 32) bad('bad_req', `REQ name_len ${nameLen} exceeds 32`);
  if (buf.length !== REQ_HEAD + nameLen + 64) bad('bad_req', `REQ is ${buf.length} bytes, name_len says ${REQ_HEAD + nameLen + 64}`);
  const cur = buf.subarray(45, 49);
  const currency = cur.equals(Buffer.from('HACK')) ? 'HACK' : cur.equals(Buffer.from('USD\0')) ? 'USD' : bad('bad_req', 'REQ currency is neither "HACK" nor "USD\\0"');
  const name = buf.subarray(REQ_HEAD, REQ_HEAD + nameLen).toString('latin1');
  if (!PRINTABLE.test(name)) bad('bad_req', 'REQ name is not printable ASCII');
  const payee_pubkey = Buffer.from(buf.subarray(5, 37));
  return {
    rail, payee_pubkey, payee: b58(payee_pubkey), amount: buf.readBigUInt64LE(37), currency,
    req_id: buf.subarray(49, 57).toString('hex'), expiry: buf.readUInt32LE(57), name,
    signedBytes: Buffer.from(buf.subarray(0, REQ_HEAD + nameLen)), sig: Buffer.from(buf.subarray(REQ_HEAD + nameLen)),
  };
}

// Raw 32-byte Ed25519 key -> KeyObject: node:crypto only takes keys wrapped in SPKI DER (fixed 12-byte header).
const SPKI_ED25519 = Buffer.from('302a300506032b6570032100', 'hex');
export function verifyEd25519(msg, sig, pubkey32) {
  try {
    if (pubkey32?.length !== 32 || sig?.length !== 64) return false;
    const key = createPublicKey({ key: Buffer.concat([SPKI_ED25519, Buffer.from(pubkey32)]), format: 'der', type: 'spki' });
    return verify(null, Buffer.from(msg), key, Buffer.from(sig));
  } catch { return false; }
}

export const withPrefix = (prefix, bytes) => Buffer.concat([Buffer.from(prefix, 'utf8'), Buffer.from(bytes)]);
// pay-proof: || req_id[8] || nonce[16] || payer_pubkey[32], all raw bytes.
export const proofMessage = (reqIdHex, nonceHex, payer32) =>
  withPrefix(PREFIX.PAY_PROOF, Buffer.concat([Buffer.from(reqIdHex, 'hex'), Buffer.from(nonceHex, 'hex'), Buffer.from(payer32)]));

// §6 payload. The firmware (badge-os checks.md) emits the 12 lines ending at issued_at; 00 §6 adds route_id and
// route_fee_total. Both are accepted; the two route lines are all-or-nothing and come last.
const NUM = /^(0|[1-9][0-9]{0,15})$/;
const FIELDS = [
  ['v', v => v === '1'],
  ['rail', v => v === 'nessie'],
  ['action', v => v === 'purchase' || v === 'transfer'],
  ['amount_cents', v => NUM.test(v)],
  ['currency', v => v === 'USD'],
  ['from_acct', v => /^[A-Za-z0-9-]{1,64}$/.test(v)],
  ['payee_name', v => v.length >= 1 && v.length <= 32 && PRINTABLE.test(v)],
  ['payee_ref', v => /^[0-9a-f]{64}$/.test(v)],
  ['attestation', v => v === 'none' || /^[1-9A-HJ-NP-Za-km-z]{32,44}$/.test(v)],
  ['req_id', v => /^[0-9a-f]{16}$/.test(v)],
  ['proof_nonce', v => /^[0-9a-f]{32}$/.test(v)],
  ['issued_at', v => NUM.test(v)],
];
const ROUTE_FIELDS = [
  ['route_id', v => v === 'none' || /^[0-9a-f]{16}$/.test(v)],
  ['route_fee_total', v => NUM.test(v)],
];

// Throws { code: 'bad_payload' }. Returns the fields as strings, with amount_cents / issued_at / route_fee_total as numbers.
export function parseBankPayload(str) {
  if (typeof str !== 'string') bad('bad_payload', 'payload is not text');
  const lines = str.split('\n');
  const spec = lines.length === FIELDS.length ? FIELDS : lines.length === FIELDS.length + ROUTE_FIELDS.length ? [...FIELDS, ...ROUTE_FIELDS]
    : bad('bad_payload', `payload has ${lines.length} lines, expected ${FIELDS.length} or ${FIELDS.length + ROUTE_FIELDS.length}`);
  const out = {};
  spec.forEach(([key, ok], i) => {
    const line = lines[i], eq = line.indexOf('=');
    if (eq < 0 || line.slice(0, eq) !== key) bad('bad_payload', `line ${i + 1} must be ${key}=...`);
    const v = line.slice(eq + 1);
    if (!ok(v)) bad('bad_payload', `bad value for ${key}`);
    out[key] = v;
  });
  out.route_id ??= 'none';
  out.route_fee_total ??= '0';
  for (const k of ['amount_cents', 'issued_at', 'route_fee_total']) out[k] = Number(out[k]);
  if (!Number.isSafeInteger(out.amount_cents)) bad('bad_payload', 'amount_cents is too large');
  return out;
}
