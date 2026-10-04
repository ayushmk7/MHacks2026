import { signBytes, getAddressEncoder } from '@solana/kit';
import { KINDS, recordFields } from './registry.js';

// Issuer-signed registry records: the full text form (00 §7) served on GET /registry/:pubkey, and the compact
// binary form (00 §9.3, CREC) carried over the radio on routes. Both are built from the same fields.
export const PREFIX = { full: 'registry:', compact: 'registry-c:' };
const STATUS = { active: 1, revoked: 2 };
const ZERO32 = Buffer.alloc(32);
const enc = getAddressEncoder();

const fail = msg => { throw new Error(`registry record: ${msg}`); };
const b32 = (v, field, optional = true) => {
  if (v == null || v.length === 0) return optional ? null : fail(`${field} is required`);
  return v.length === 32 ? Buffer.from(v) : fail(`${field} must be 32 bytes`);
};
const u32 = (v, field) => (Number.isInteger(v) && v >= 0 && v <= 0xFFFFFFFF ? v : fail(`${field} must be a u32`));

// The badge's parser (vk_record.c) is strict, so refuse to sign anything it would reject.
function check(f) {
  if (typeof f.display_name !== 'string' || !/^[\x20-\x7E]{1,32}$/.test(f.display_name)) fail('display_name must be 1 to 32 printable ASCII characters');
  if (!KINDS[f.kind]) fail('kind must be merchant, person or relay');
  if (!STATUS[f.status]) fail('status must be active or revoked');
  return {
    device_pubkey: b32(f.device_pubkey, 'device_pubkey', false), solana_wallet: b32(f.solana_wallet, 'solana_wallet'),
    solana_ata: b32(f.solana_ata, 'solana_ata'), bank_ref_hash: b32(f.bank_ref_hash, 'bank_ref_hash'),
    expiry: u32(f.expiry, 'expiry'), issued_at: u32(f.issued_at, 'issued_at'),
  };
}

const hex = b => (b ? b.toString('hex') : '');

// 00 §7, exactly as vk_record_parse reads it: fixed key order, '\n' between lines, no trailing newline,
// lowercase hex, empty value for a missing optional field, "none" when there is no on-chain attestation.
export function canonicalRecord(fields) {
  const c = check(fields);
  return Buffer.from([
    'v=1',
    `attestation=${fields.attestation ?? 'none'}`,
    `display_name=${fields.display_name}`,
    `device_pubkey=${hex(c.device_pubkey)}`,
    `kind=${fields.kind}`,
    `solana_wallet=${hex(c.solana_wallet)}`,
    `solana_ata=${hex(c.solana_ata)}`,
    `bank_ref_hash=${hex(c.bank_ref_hash)}`,
    `expiry=${c.expiry}`,
    `status=${fields.status}`,
    `issued_at=${c.issued_at}`,
  ].join('\n'), 'utf8');
}

// 00 §9.3 without the trailing sig: v · device_pubkey · kind · status · attestation · solana_ata · bank_ref_hash ·
// expiry u32 LE · issued_at u32 LE · name_len · name. 140 + name bytes (≤ 172). Missing 32-byte fields are zeros.
export function compactRecord(fields) {
  const c = check(fields);
  const name = Buffer.from(fields.display_name, 'ascii');
  const times = Buffer.alloc(8);
  times.writeUInt32LE(c.expiry, 0);
  times.writeUInt32LE(c.issued_at, 4);
  return Buffer.concat([
    Buffer.of(1), c.device_pubkey, Buffer.of(KINDS[fields.kind], STATUS[fields.status]),
    fields.attestation ? Buffer.from(enc.encode(fields.attestation)) : ZERO32,
    c.solana_ata ?? ZERO32, c.bank_ref_hash ?? ZERO32, times, Buffer.of(name.length), name,
  ]);
}

// signer: a kit KeyPairSigner. Its CryptoKey is non-extractable, so sign through WebCrypto rather than node:crypto.
export async function signRecord(signer, bytes, prefix) {
  if (prefix !== PREFIX.full && prefix !== PREFIX.compact) fail('prefix must be registry: or registry-c:');
  return Buffer.from(await signBytes(signer.keyPair.privateKey, Buffer.concat([Buffer.from(prefix), bytes])));
}

export async function buildSignedRecord(signer, fields, format = 'full') {
  if (!PREFIX[format]) fail('format must be full or compact');
  const record = format === 'compact' ? compactRecord(fields) : canonicalRecord(fields);
  return { record: record.toString('base64'), sig: (await signRecord(signer, record, PREFIX[format])).toString('base64') };
}

// Signed fresh per call: the badge rejects a record whose issued_at is older than record_ttl_s (30 s).
export async function getSignedRecord(signer, pubkey, format = 'full', lookup = recordFields) {
  const fields = await lookup(pubkey);
  return fields ? { status: 200, body: await buildSignedRecord(signer, fields, format) } : { status: 404 };
}
