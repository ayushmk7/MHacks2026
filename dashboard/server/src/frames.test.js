import test from 'node:test';
import assert from 'node:assert/strict';
import { generateKeyPairSync, sign } from 'node:crypto';
import { PREFIX, parseReq, verifyEd25519, withPrefix, proofMessage, parseBankPayload, b58 } from './frames.js';

const keyPair = () => {
  const { publicKey, privateKey } = generateKeyPairSync('ed25519');
  return { pub: publicKey.export({ format: 'der', type: 'spki' }).subarray(12), sign: msg => sign(null, msg, privateKey) };
};

// REQ frame per 00 §5, signed with pay-req:.
function buildReq(kp, { rail = 2, amount = 4000n, currency = 'USD\0', reqId = '0102030405060708', expiry = 2_000_000_000, name = 'MHacks Merch' } = {}) {
  const head = Buffer.alloc(62);
  head.write('VK', 0, 'latin1'); head[2] = 1; head[3] = 1; head[4] = rail;
  kp.pub.copy(head, 5);
  head.writeBigUInt64LE(BigInt(amount), 37);
  head.write(currency, 45, 'latin1');
  Buffer.from(reqId, 'hex').copy(head, 49);
  head.writeUInt32LE(expiry, 57);
  head[61] = name.length;
  const signed = Buffer.concat([head, Buffer.from(name, 'latin1')]);
  return Buffer.concat([signed, kp.sign(withPrefix(PREFIX.PAY_REQ, signed))]);
}

const payloadOf = (o = {}) => [
  'v=1', 'rail=nessie', `action=${o.action ?? 'purchase'}`, `amount_cents=${o.amount_cents ?? 4000}`, 'currency=USD',
  `from_acct=${o.from_acct ?? 'acct-1'}`, `payee_name=${o.payee_name ?? 'MHacks Merch'}`, `payee_ref=${o.payee_ref ?? 'ab'.repeat(32)}`,
  `attestation=${o.attestation ?? 'none'}`, `req_id=${o.req_id ?? '0102030405060708'}`, `proof_nonce=${o.proof_nonce ?? '11'.repeat(16)}`,
  `issued_at=${o.issued_at ?? 1_800_000_000}`, ...(o.route ? [`route_id=${o.route_id ?? 'none'}`, `route_fee_total=${o.route_fee_total ?? 0}`] : []),
].join('\n');

test('parseReq reads every field and the signed range verifies', () => {
  const kp = keyPair();
  const r = parseReq(buildReq(kp, { amount: 123456789012n, name: 'Shop' }));
  assert.equal(r.rail, 2);
  assert.equal(r.payee, b58(kp.pub));
  assert.equal(r.amount, 123456789012n);
  assert.equal(r.currency, 'USD');
  assert.equal(r.req_id, '0102030405060708');
  assert.equal(r.expiry, 2_000_000_000);
  assert.equal(r.name, 'Shop');
  assert.equal(r.signedBytes.length, 66);
  assert.equal(r.sig.length, 64);
  assert.ok(verifyEd25519(withPrefix(PREFIX.PAY_REQ, r.signedBytes), r.sig, r.payee_pubkey));
  assert.equal(parseReq(buildReq(kp, { rail: 1, currency: 'HACK', name: '' })).currency, 'HACK');
});

test('parseReq rejects malformed frames', () => {
  const ok = buildReq(keyPair());
  const mut = (i, v) => { const b = Buffer.from(ok); b[i] = v; return b; };
  for (const bad of [ok.subarray(0, 100), Buffer.concat([ok, Buffer.alloc(1)]), mut(0, 0x41), mut(2, 2), mut(3, 2), mut(4, 3),
                     mut(45, 0x45), mut(61, 33), mut(62, 0x07)]) {
    assert.throws(() => parseReq(bad), { code: 'bad_req' });
  }
});

test('verifyEd25519 accepts only the right key, message and signature', () => {
  const a = keyPair(), b = keyPair(), msg = Buffer.from('hello');
  const sig = a.sign(msg);
  assert.equal(verifyEd25519(msg, sig, a.pub), true);
  assert.equal(verifyEd25519(msg, sig, b.pub), false);
  assert.equal(verifyEd25519(Buffer.from('hellO'), sig, a.pub), false);
  assert.equal(verifyEd25519(msg, Buffer.alloc(64), a.pub), false);
  assert.equal(verifyEd25519(msg, sig.subarray(1), a.pub), false);
  assert.equal(verifyEd25519(msg, sig, a.pub.subarray(1)), false);
});

test('proofMessage is pay-proof: || req_id || nonce || payer, raw bytes', () => {
  const m = proofMessage('0102030405060708', '11'.repeat(16), Buffer.alloc(32, 7));
  assert.equal(m.length, 10 + 8 + 16 + 32);
  assert.equal(m.subarray(0, 10).toString(), 'pay-proof:');
  assert.equal(m[10], 1);
  assert.equal(m[18], 0x11);
  assert.equal(m[34], 7);
});

test('parseBankPayload accepts the 12-line firmware form and the 14-line 00 §6 form', () => {
  const p = parseBankPayload(payloadOf());
  assert.equal(p.amount_cents, 4000);
  assert.equal(p.issued_at, 1_800_000_000);
  assert.equal(p.route_id, 'none');
  assert.equal(p.route_fee_total, 0);
  assert.equal(parseBankPayload(payloadOf({ route: true })).req_id, '0102030405060708');
  assert.equal(parseBankPayload(payloadOf({ route: true, route_id: 'aa'.repeat(8), route_fee_total: 3 })).route_fee_total, 3);
});

test('parseBankPayload enforces order, values and no trailing newline', () => {
  const ok = payloadOf();
  const lines = ok.split('\n');
  const swapped = [...lines]; [swapped[2], swapped[3]] = [swapped[3], swapped[2]];
  for (const bad of [`${ok}\n`, swapped.join('\n'), lines.slice(0, 11).join('\n'), ok.replace('v=1', 'v=2'), ok.replace('rail=nessie', 'rail=solana'),
                     ok.replace('currency=USD', 'currency=EUR'), payloadOf({ action: 'refund' }), payloadOf({ amount_cents: '040' }),
                     payloadOf({ amount_cents: '-1' }), payloadOf({ payee_ref: 'AB'.repeat(32) }), payloadOf({ req_id: '0102' }),
                     payloadOf({ proof_nonce: 'zz'.repeat(16) }), payloadOf({ payee_name: 'x'.repeat(33) }), ok.replace('\n', '\r\n'),
                     `${ok}\nroute_id=none`, payloadOf({ route: true, route_id: 'nope' })]) {
    assert.throws(() => parseBankPayload(bad), { code: 'bad_payload' }, JSON.stringify(bad));
  }
});
