import test from 'node:test';
import assert from 'node:assert/strict';
import { generateKeyPairSync, sign } from 'node:crypto';
import { token } from './config.js';
import { createFeed } from './feed.js';
import { b58 } from './frames.js';

Object.assign(token, { mint: 'So11111111111111111111111111111111111111112', decimals: 2, found: true });

const { publicKey, privateKey } = generateKeyPairSync('ed25519');
const payee32 = publicKey.export({ format: 'der', type: 'spki' }).subarray(12);
const PAYEE = b58(payee32), PAYEE_ATA = b58(Buffer.alloc(32, 9)), PAYER = b58(Buffer.alloc(32, 3));

// REQ frame (00 §5) signed by the payee over 'pay-req:' + header..name.
function req({ rail = 1, amount = 4000n, key = privateKey } = {}) {
  const name = Buffer.from('MHacks Merch');
  const head = Buffer.alloc(62);
  head.write('VK', 0); head[2] = 1; head[3] = 1; head[4] = rail;
  payee32.copy(head, 5); head.writeBigUInt64LE(amount, 37); head.write('HACK', 45);
  Buffer.alloc(8, 0xab).copy(head, 49); head.writeUInt32LE(2_000_000_000, 57); head[61] = name.length;
  const body = Buffer.concat([head, name]);
  return Buffer.concat([body, sign(null, Buffer.concat([Buffer.from('pay-req:'), body]), key)]).toString('base64');
}

function harness({ transfer, existing = false } = {}) {
  const writes = [];
  const db = async (sql, params) => {
    if (sql.includes('SELECT id FROM approvals')) return { rows: existing ? [{ id: 'x' }] : [] };
    writes.push(params);
    return { rows: [{ id: 'new' }] };
  };
  const confirm = createFeed({
    db, getTx: async () => ({}), ata: async () => PAYEE_ATA,
    parse: () => transfer ?? { payer: PAYER, payee: PAYEE, payeeTokenAccount: PAYEE_ATA, amountRaw: '4000', decimals: 2 },
  });
  const recordFields = async () => ({ display_name: 'MHacks Merch', solana_ata: null });
  return { writes, run: body => confirm(body, { recordFields }) };
}

test('feed/solana: a matching on-chain transfer is recorded once', async () => {
  const h = harness();
  assert.deepEqual(await h.run({ tx_sig: 'sig1', req: req() }), { ok: true });
  assert.equal(h.writes.length, 1);
  assert.deepEqual(h.writes[0], [PAYER, PAYEE, 'MHacks Merch', '4000', 'abababababababab', 'sig1']);
});

test('feed/solana: retries of a recorded signature do not write again', async () => {
  const h = harness({ existing: true });
  assert.deepEqual(await h.run({ tx_sig: 'sig1', req: req() }), { ok: true });
  assert.equal(h.writes.length, 0);
});

test('feed/solana: refuses wrong recipient, wrong amount, bad REQ signature, bank rail', async () => {
  const wrongAta = harness({ transfer: { payer: PAYER, payee: PAYEE, payeeTokenAccount: PAYER, amountRaw: '4000', decimals: 2 } });
  assert.equal((await wrongAta.run({ tx_sig: 's', req: req() })).reason, 'wrong_recipient');
  const wrongAmount = harness({ transfer: { payer: PAYER, payee: PAYEE, payeeTokenAccount: PAYEE_ATA, amountRaw: '400', decimals: 2 } });
  assert.equal((await wrongAmount.run({ tx_sig: 's', req: req() })).reason, 'amount_mismatch');
  const other = generateKeyPairSync('ed25519').privateKey;
  assert.equal((await harness().run({ tx_sig: 's', req: req({ key: other }) })).reason, 'bad_req_sig');
  assert.equal((await harness().run({ tx_sig: 's', req: req({ rail: 2 }) })).reason, 'wrong_rail');
  assert.equal((await harness().run({ tx_sig: 's' })).reason, 'bad_request');
  for (const h of [wrongAta, wrongAmount]) assert.equal(h.writes.length, 0);
});
