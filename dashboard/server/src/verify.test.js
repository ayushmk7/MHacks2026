import test from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { verifyReq, verifyProof, verifyBank, verifyRegistry, verifyRegistryResponse } from './verify.js';

// Shared with harness/vk_verify.py; regenerate with harness/make_vectors.py.
const vectors = JSON.parse(readFileSync(new URL('../../../harness/vectors.json', import.meta.url), 'utf8'));
const hex = s => Buffer.from(s, 'hex');
const opts = { now: vectors.now };

function run({ kind, input: i }) {
  if (kind === 'req') return verifyReq(hex(i.frame), opts);
  if (kind === 'proof') return verifyProof(hex(i.proof), hex(i.chal), hex(i.payee_pubkey));
  if (kind === 'bank') return verifyBank(Buffer.from(i.payload, 'utf8'), hex(i.sig), hex(i.payer_pubkey), opts);
  if (kind === 'registry') return verifyRegistry(Buffer.from(i.record, 'utf8'), hex(i.sig), hex(i.issuer_pubkey), opts);
  throw new Error(`unknown kind ${kind}`);
}

for (const v of vectors.vectors) {
  test(`vector: ${v.name}`, () => assert.equal(run(v).reason, v.expect));
}

test('every message type has a known-good vector and a rejected tampered copy', () => {
  for (const kind of ['req', 'proof', 'bank', 'registry']) {
    const of = vectors.vectors.filter(v => v.kind === kind);
    assert.ok(of.some(v => v.expect === 'ok'), `${kind}: no good vector`);
    assert.ok(of.some(v => v.expect === 'bad_signature'), `${kind}: no tampered vector`);
  }
});

test('registry JSON response form', () => {
  const good = vectors.vectors.find(v => v.name === 'registry ok').input;
  const response = { record: Buffer.from(good.record).toString('base64'), sig: hex(good.sig).toString('base64') };
  assert.equal(verifyRegistryResponse(response, hex(good.issuer_pubkey), opts).reason, 'ok');
  assert.equal(verifyRegistryResponse({ record: 'not base64!' }, hex(good.issuer_pubkey), opts).reason, 'bad_format');
});

test('R3: legacy Solana message signature', async () => {
  const { verifySolanaMessage } = await import('./verify.js');
  const { generateKeyPairSync, sign } = await import('node:crypto');
  const { privateKey, publicKey } = generateKeyPairSync('ed25519');
  const pub = publicKey.export({ format: 'der', type: 'spki' }).subarray(-32);
  // header [1 signer, 0, 1 readonly unsigned] + 2 account keys + blockhash + 0 instructions
  const msg = Buffer.concat([Buffer.from([1, 0, 1, 2]), pub, Buffer.alloc(32, 7), Buffer.alloc(32, 9), Buffer.from([0])]);
  const sig = sign(null, msg, privateKey);
  assert.equal(verifySolanaMessage(msg, sig, pub).reason, 'ok');
  const tampered = Buffer.from(msg); tampered[tampered.length - 2] ^= 1;   // inside the blockhash
  assert.equal(verifySolanaMessage(tampered, sig, pub).reason, 'bad_signature');
  assert.equal(verifySolanaMessage(Buffer.concat([Buffer.from([0x80]), msg]), sig, pub).reason, 'bad_version');
  assert.equal(verifySolanaMessage(msg, sig, Buffer.alloc(32, 1)).reason, 'mismatch');
  const two = Buffer.from(msg); two[0] = 2;
  assert.equal(verifySolanaMessage(two, sig, pub).reason, 'bad_field');
});
