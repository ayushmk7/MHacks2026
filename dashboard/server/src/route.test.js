import test from 'node:test';
import assert from 'node:assert/strict';
import { generateKeyPairSync, sign } from 'node:crypto';
import { createNoopSigner } from '@solana/kit';
import { token } from './config.js';
import { createRoutes, toRoute } from './route.js';
import { b58 } from './frames.js';

Object.assign(token, { mint: 'So11111111111111111111111111111111111111112', decimals: 2, found: true });

const { publicKey, privateKey } = generateKeyPairSync('ed25519');
const payee32 = publicKey.export({ format: 'der', type: 'spki' }).subarray(12);
const key = n => b58(Buffer.alloc(32, n));
const PAYEE = b58(payee32), PAYEE_ATA = key(9), PAYER = key(3), RELAY_A = key(4), RELAY_B = key(5), STRANGER = key(6);
const AUTHORITY = createNoopSigner(key(7));
const FUTURE = Math.floor(Date.now() / 1000) + 3600;
// Rewards are sent in the background: wait until no hop is still pending (or 1 s).
async function settled(h) {
  for (let i = 0; i < 100 && [...h.hops.values()].flat().some(x => x.reward_state === 'pending'); i++) await new Promise(r => setTimeout(r, 10));
}

// REQ frame (00 §5) signed by the payee over 'pay-req:' + header..name.
function req({ amount = 4000n } = {}) {
  const name = Buffer.from('MHacks Merch');
  const head = Buffer.alloc(62);
  head.write('VK', 0); head[2] = 1; head[3] = 1; head[4] = 1;
  payee32.copy(head, 5); head.writeBigUInt64LE(amount, 37); head.write('HACK', 45);
  Buffer.alloc(8, 0xab).copy(head, 49); head.writeUInt32LE(2_000_000_000, 57); head[61] = name.length;
  const body = Buffer.concat([head, name]);
  return Buffer.concat([body, sign(null, Buffer.concat([Buffer.from('pay-req:'), body]), privateKey)]).toString('base64');
}

// Fake db keeps routes/route_hops in memory, keyed like the real tables.
function harness({ transfer, records = {}, sendFails = false, ...opts } = {}) {
  const routes = new Map(), hops = new Map(), sent = [], confirmed = [];
  let txFetches = 0;
  const db = async (sql, p) => {
    if (sql.includes('FROM route_hops WHERE tx_sig')) return { rows: hops.get(p[0]) ?? [] };
    if (sql.includes('INSERT INTO routes')) {
      if (routes.has(p[0])) return { rows: [] };
      routes.set(p[0], p.slice(0, 9));
      hops.set(p[0], p[9].map((position, i) => ({ position, relay: p[10][i], verified: p[11][i], reward_raw: p[12][i], reward_state: p[13][i], reward_sig: null })));
      return { rows: p[9].map(position => ({ position })) };
    }
    if (sql.includes('UPDATE route_hops')) {
      for (const h of hops.get(p[0])) if (h.reward_state === 'pending') Object.assign(h, { reward_state: p[1], reward_sig: p[2] });
      return { rows: [] };
    }
    throw new Error(`unexpected SQL ${sql}`);
  };
  const r = createRoutes({
    db, ...opts,
    send: async (signer, ixs) => { sent.push(ixs); if (sendFails) throw new Error('devnet down'); return 'rewardSig'; },
    getTx: async () => (txFetches++, {}), ata: async () => PAYEE_ATA,
    parse: () => transfer ?? { payer: PAYER, payee: PAYEE, payeeTokenAccount: PAYEE_ATA, amountRaw: '4000', decimals: 2 },
    onConfirmed: f => confirmed.push(f),
  });
  const relay = { kind: 'relay', status: 'active', expiry: FUTURE };
  const recs = { [PAYEE]: { display_name: 'MHacks Merch', solana_ata: null, kind: 'merchant' }, [RELAY_A]: relay, [RELAY_B]: relay, ...records };
  const recordFields = async pk => recs[pk] ?? null;
  return { routes, hops, sent, confirmed, txFetches: () => txFetches,
           run: (body, extra = {}) => r.confirmRoute({ tx_sig: 'sig1', req: req(), hops: [RELAY_A, RELAY_B], ...body }, { recordFields, authority: AUTHORITY, ...extra }) };
}

test('feed/route: two verified relays get pending rewards, settled once in the background', async () => {
  const h = harness();
  const out = await h.run({ e2e_proof: true });
  assert.deepEqual(out, { ok: true, rewards: [{ pubkey: RELAY_A, state: 'pending' }, { pubkey: RELAY_B, state: 'pending' }] });
  assert.deepEqual(h.routes.get('sig1'), ['sig1', PAYER, PAYEE, 'MHacks Merch', '4000', 'abababababababab', 2, true, RELAY_B]);
  assert.deepEqual(h.hops.get('sig1').map(x => [x.position, x.relay, x.verified, x.reward_raw]), [[1, RELAY_A, true, '1'], [2, RELAY_B, true, '1']]);
  assert.equal(h.confirmed.length, 1);
  assert.deepEqual(h.confirmed[0], { payer: PAYER, payee: PAYEE, payeeName: 'MHacks Merch', amountRaw: '4000',
                                     reqId: 'abababababababab', txSig: 'sig1', routed: true, hops: [RELAY_A, RELAY_B] });
  await settled(h);
  assert.equal(h.sent.length, 1);
  assert.equal(h.sent[0].length, 4);   // ATA create + transferChecked per hop, one transaction
  assert.deepEqual(h.hops.get('sig1').map(x => [x.reward_state, x.reward_sig]), [['paid', 'rewardSig'], ['paid', 'rewardSig']]);
});

test('feed/route: a repeat report returns the stored result without a second insert, reward or hook', async () => {
  const h = harness();
  await h.run();
  await settled(h);
  const fetches = h.txFetches();
  assert.deepEqual(await h.run(), { ok: true, rewards: [{ pubkey: RELAY_A, state: 'paid' }, { pubkey: RELAY_B, state: 'paid' }] });
  assert.equal(h.txFetches(), fetches);
  assert.equal(h.sent.length, 1);
  assert.equal(h.confirmed.length, 1);
});

test('feed/route: unverified, revoked and expired relays are recorded with no reward', async () => {
  const h = harness({ records: { [RELAY_B]: { kind: 'relay', status: 'revoked', expiry: FUTURE },
                                 [key(8)]: { kind: 'merchant', status: 'active', expiry: FUTURE } } });
  const out = await h.run({ hops: [RELAY_A, RELAY_B, STRANGER] });
  assert.deepEqual(out.rewards.map(r => r.state), ['pending', 'none', 'none']);
  const expired = harness({ records: { [RELAY_A]: { kind: 'relay', status: 'active', expiry: 1000 } } });
  assert.deepEqual((await expired.run({ hops: [RELAY_A, key(8)] })).rewards.map(r => r.state), ['none', 'none']);
  await settled(h); await settled(expired);
  assert.equal(h.sent[0].length, 2);
  assert.equal(expired.sent.length, 0);
  assert.deepEqual(expired.hops.get('sig1').map(x => x.verified), [false, false]);
});

test('feed/route: refuses bad hop lists', async () => {
  const h = harness();
  for (const hops of [undefined, [], [RELAY_A, RELAY_B, STRANGER, key(8)], [RELAY_A, RELAY_A], ['not-a-key'], [RELAY_A, 7], [PAYER], [PAYEE]])
    assert.deepEqual(await h.run({ hops }), { ok: false, reason: 'bad_hops' }, JSON.stringify(hops));
  assert.equal(h.routes.size, 0);
  assert.equal(h.confirmed.length, 0);
});

test('feed/route: refuses a wrong recipient or amount and records nothing', async () => {
  const wrongAta = harness({ transfer: { payer: PAYER, payee: PAYEE, payeeTokenAccount: PAYER, amountRaw: '4000', decimals: 2 } });
  assert.equal((await wrongAta.run()).reason, 'wrong_recipient');
  const wrongAmount = harness({ transfer: { payer: PAYER, payee: PAYEE, payeeTokenAccount: PAYEE_ATA, amountRaw: '400', decimals: 2 } });
  assert.equal((await wrongAmount.run()).reason, 'amount_mismatch');
  for (const h of [wrongAta, wrongAmount]) { assert.equal(h.routes.size, 0); assert.equal(h.confirmed.length, 0); assert.equal(h.sent.length, 0); }
});

test('feed/route: total reward per route is capped', async () => {
  const records = { [STRANGER]: { kind: 'relay', status: 'active', expiry: FUTURE } };
  const h = harness({ records, rewardRaw: 4n, rewardCap: 10n });
  assert.deepEqual((await h.run({ hops: [RELAY_A, RELAY_B, STRANGER] })).rewards.map(r => r.state), ['pending', 'pending', 'pending']);
  assert.deepEqual(h.hops.get('sig1').map(x => x.reward_raw), ['4', '4', '2']);
  const tight = harness({ records, rewardRaw: 5n, rewardCap: 5n });
  assert.deepEqual((await tight.run({ hops: [RELAY_A, RELAY_B] })).rewards.map(r => r.state), ['pending', 'none']);
});

test('feed/route: a failed reward transaction marks the hops failed, the response and hook are unaffected', async () => {
  const h = harness({ sendFails: true });
  const out = await h.run({}, { onConfirmed: () => { throw new Error('settlement down'); } });
  assert.equal(out.ok, true);
  await settled(h);
  assert.deepEqual(h.hops.get('sig1').map(x => x.reward_state), ['failed', 'failed']);
  const noAuth = harness();
  await noAuth.run({}, { authority: null });
  await settled(noAuth);
  assert.deepEqual(noAuth.hops.get('sig1').map(x => x.reward_state), ['failed', 'failed']);
});

test('route/ctx: signed record plus a fresh blockhash, 404 for an unknown payee', async () => {
  const rpc = { getLatestBlockhash: () => ({ send: async () => ({ value: { blockhash: 'BH', lastValidBlockHeight: 99n } }) }) };
  const signedRecord = async (signer, pk) => (pk === PAYEE ? { status: 200, body: { record: 'cmVj', sig: 'c2ln' } } : { status: 404 });
  const r = createRoutes({ rpc, signedRecord });
  assert.deepEqual(await r.routeCtx(AUTHORITY, PAYEE), { status: 200, body: { record: 'cmVj', sig: 'c2ln', blockhash: 'BH', lastValidBlockHeight: 99 } });
  assert.deepEqual(await r.routeCtx(AUTHORITY, STRANGER), { status: 404 });
});

test('toRoute: UI amounts, labels and explorer link', () => {
  const r = toRoute({ tx_sig: 'S', created_at: new Date(0), payer: PAYER, payer_label: 'Judge', payee: PAYEE, payee_name: 'MHacks Merch',
    payee_label: null, amount_raw: '4000', e2e_proof: false, gateway: RELAY_B,
    hops: [{ position: 1, relay: RELAY_A, label: 'Relay A', verified: true, reward_raw: '1', reward_state: 'paid', reward_sig: 'R' }] });
  assert.equal(r.amount, 40); assert.equal(r.amountRaw, '4000');
  assert.deepEqual(r.hops[0], { position: 1, pubkey: RELAY_A, label: 'Relay A', verified: true, reward: 0.01, rewardState: 'paid', rewardSig: 'R' });
  assert.equal(r.links.tx, 'https://explorer.solana.com/tx/S?cluster=devnet');
});

test('feed/solana: onConfirmed fires once for a new payment, never on a repeat, and cannot fail the response', async () => {
  const { createFeed } = await import('./feed.js');
  const seen = new Set(), calls = [];
  const db = async (sql, p) => ({ rows: sql.includes('SELECT id FROM approvals') ? (seen.has(p[0]) ? [{ id: 'x' }] : []) : (seen.add(p[5]), [{ id: 'n' }]) });
  const confirm = createFeed({ db, getTx: async () => ({}), ata: async () => PAYEE_ATA,
    parse: () => ({ payer: PAYER, payee: PAYEE, payeeTokenAccount: PAYEE_ATA, amountRaw: '4000', decimals: 2 }),
    onConfirmed: f => { calls.push(f); throw new Error('boom'); } });
  const recordFields = async () => ({ display_name: 'MHacks Merch', solana_ata: null });
  assert.deepEqual(await confirm({ tx_sig: 'sig1', req: req() }, { recordFields }), { ok: true });
  assert.deepEqual(await confirm({ tx_sig: 'sig1', req: req() }, { recordFields }), { ok: true });
  assert.equal(calls.length, 1);
  assert.equal(calls[0].routed, false);
});
