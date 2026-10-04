import test from 'node:test';
import assert from 'node:assert/strict';
import { randomUUID } from 'node:crypto';
import { address, createNoopSigner } from '@solana/kit';
import { TOKEN_PROGRAM_ADDRESS, ASSOCIATED_TOKEN_PROGRAM_ADDRESS, findAssociatedTokenPda } from '@solana-program/token';
import { createTopup, SQL, MAX_TOPUP } from './topup.js';

// Offline: the DB, Nessie and the Solana sender are fakes. Instructions are built for real (PDA math is local).
console.error = () => {};   // failures are expected here, keep the output readable
const BADGE = '9xQeWvG816bUx9EPjHmaT23yvVM2ZWbrrpZb9PusVFin', STRANGER = 'Stake11111111111111111111111111111111111111';
const MINT = 'So11111111111111111111111111111111111111112';
const authority = createNoopSigner(address('Vote111111111111111111111111111111111111111'));
const ata = async owner => (await findAssociatedTokenPda({ owner: address(owner), mint: address(MINT), tokenProgram: TOKEN_PROGRAM_ADDRESS }))[0];

function world({ withdrawal, send, mint = { mint: MINT, decimals: 2, found: true } } = {}) {
  const db = { enrollments: new Map([[BADGE, { badge_pubkey: BADGE, nessie_customer_id: 'c-1', nessie_account_id: 'judge-acct' }]]), topups: [] };
  const rows = r => ({ rows: r, rowCount: r.length });
  const fakeQ = async (sql, params) => {
    switch (sql) {
      case SQL.enrollment: return rows([db.enrollments.get(params[0])].filter(Boolean));
      case SQL.insert: {
        const row = { id: randomUUID(), created_at: new Date(), badge_pubkey: params[0], amount_raw: params[1], nessie_account_id: params[2],
                      nessie_withdrawal_id: null, hack_sig: null, status: 'pending', reason: null };
        db.topups.push(row);
        return rows([{ id: row.id }]);
      }
      case SQL.finish: Object.assign(db.topups.find(t => t.id === params[0]), { status: params[1], reason: params[2], nessie_withdrawal_id: params[3], hack_sig: params[4] }); return rows([]);
      case SQL.list: return rows([...db.topups].reverse().filter(t => !params[1] || t.id === params[1]).slice(0, params[0]).map(t => ({ ...t, label: 'Badge 2' })));
      default: throw new Error(`unexpected SQL ${sql}`);
    }
  };
  const calls = [], sent = [];
  const nessie = { withdrawal: async (acct, o) => { calls.push([acct, o]); return withdrawal ? withdrawal() : { withdrawal_id: 'w-1' }; } };
  const sender = async (signer, ixs) => { sent.push({ signer, ixs }); return send ? send() : 'hackSig111'; };
  return { db, calls, sent, t: createTopup({ db: fakeQ, nessie, send: sender, mint: () => mint }) };
}

test('happy path: Nessie withdrawal of the dollars, then HACK from the treasury ATA to the badge ATA', async () => {
  const w = world();
  const r = await w.t.topup({ pubkey: BADGE, amount: 25 }, { authority });
  assert.deepEqual(w.calls, [['judge-acct', { amount: 25, description: `top up to badge ${BADGE.slice(0, 8)}` }]]);
  assert.deepEqual({ ...r, id: null, time: null }, { id: null, time: null, pubkey: BADGE, label: 'Badge 2', amount: 25, status: 'done', reason: null,
    nessieWithdrawalId: 'w-1', hackSig: 'hackSig111', links: { tx: 'https://explorer.solana.com/tx/hackSig111?cluster=devnet' } });
  assert.equal(w.db.topups[0].amount_raw, '2500');
  assert.equal(w.sent.length, 1);
  const [{ signer, ixs: [create, transfer] }] = w.sent;
  assert.equal(signer, authority);
  assert.equal(create.programAddress, ASSOCIATED_TOKEN_PROGRAM_ADDRESS);
  assert.equal(create.data[0], 1);   // CreateIdempotent
  assert.equal(create.accounts[1].address, await ata(BADGE));
  assert.equal(transfer.programAddress, TOKEN_PROGRAM_ADDRESS);
  assert.equal(transfer.data[0], 12);   // TransferChecked
  assert.deepEqual(transfer.accounts.slice(0, 4).map(a => a.address), [await ata(authority.address), MINT, await ata(BADGE), authority.address]);
  assert.equal(Buffer.from(transfer.data).readBigUInt64LE(1), 2500n);
  assert.equal(transfer.data[9], 2);
  assert.deepEqual(await w.t.getTopup(r.id), r);
  assert.deepEqual(await w.t.listTopups(5), [r]);
});

test('a Nessie failure sends no HACK and returns a failed row', async () => {
  const w = world({ withdrawal: () => { throw Object.assign(new Error('HTTP 500'), { code: 'nessie_error' }); } });
  const r = await w.t.topup({ pubkey: BADGE, amount: 10 }, { authority });
  assert.deepEqual([r.status, r.reason, r.nessieWithdrawalId, r.hackSig, r.links.tx], ['failed', 'nessie_error', null, null, null]);
  assert.equal(w.sent.length, 0);
});

test('a chain failure after the withdrawal keeps the withdrawal id, status failed', async () => {
  const w = world({ send: () => { throw new Error('blockhash expired'); } });
  const r = await w.t.topup({ pubkey: BADGE, amount: 10 }, { authority });
  assert.deepEqual([r.status, r.reason, r.nessieWithdrawalId, r.hackSig], ['failed', 'chain_error', 'w-1', null]);
});

test('refused before anything is recorded', async () => {
  const w = world();
  await assert.rejects(w.t.topup({ pubkey: 'nope', amount: 10 }, { authority }), { code: 'invalid_pubkey' });
  for (const amount of [0, -1, 1.5, MAX_TOPUP + 1, '10', null]) await assert.rejects(w.t.topup({ pubkey: BADGE, amount }, { authority }), { code: 'invalid_amount' });
  await assert.rejects(w.t.topup({ pubkey: STRANGER, amount: 10 }, { authority }), { code: 'not_found' });
  await assert.rejects(w.t.topup({ pubkey: BADGE, amount: 10 }, {}), { code: 'not_configured' });
  await assert.rejects(world({ mint: { mint: null, decimals: 2, found: false } }).t.topup({ pubkey: BADGE, amount: 10 }, { authority }), { code: 'not_configured' });
  assert.equal(w.db.topups.length, 0);
  assert.deepEqual([w.calls, w.sent], [[], []]);
  assert.equal((await w.t.topup({ pubkey: BADGE, amount: MAX_TOPUP }, { authority })).status, 'done');
});
