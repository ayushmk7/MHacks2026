import test from 'node:test';
import assert from 'node:assert/strict';
import { createSettlement, SQL, MAX_ATTEMPTS } from './settlement.js';

// Offline: the DB and Nessie are fakes.
console.error = () => {};   // Nessie failures are expected here, keep the output readable
const SIG = n => `${n}${'5'.repeat(87)}`;   // 88 base58 chars, distinct per n
const MERCH = 'MerchBadge1111111111111111111111111111111111', PERSON = 'PersonBadge111111111111111111111111111111111', HACKY = 'HackBadge11111111111111111111111111111111111';

function world(depositImpl) {
  const db = {
    payees: new Map([
      [MERCH, { pubkey: MERCH, display_name: 'MHacks Merch', settle_mode: 'bank', nessie_account_id: 'merch-acct' }],
      [PERSON, { pubkey: PERSON, display_name: 'Sam', settle_mode: 'bank', nessie_account_id: null }],
      [HACKY, { pubkey: HACKY, display_name: 'Coffee', settle_mode: 'hack', nessie_account_id: null }],
    ]),
    settlements: new Map(),
  };
  const rows = r => ({ rows: r, rowCount: r.length });
  const fakeQ = async (sql, params = []) => {
    switch (sql) {
      case SQL.payee: return rows([db.payees.get(params[0])].filter(Boolean));
      case SQL.insert: {
        const [tx_sig, payee, amount_raw, nessie_account_id, status, reason] = params;
        if (db.settlements.has(tx_sig)) return rows([]);
        db.settlements.set(tx_sig, { tx_sig, created_at: new Date(), updated_at: new Date(), payee, amount_raw, nessie_account_id,
                                     nessie_deposit_id: null, status, reason, attempts: 0 });
        return rows([{ tx_sig }]);
      }
      case SQL.claim: {
        const r = db.settlements.get(params[0]);
        if (!r || r.attempts !== params[1] || r.attempts >= MAX_ATTEMPTS || !['pending', 'failed'].includes(r.status)) return rows([]);
        Object.assign(r, { status: 'pending', attempts: r.attempts + 1, updated_at: new Date() });
        return rows([r]);
      }
      case SQL.finish: Object.assign(db.settlements.get(params[0]), { status: params[1], reason: params[2], nessie_deposit_id: params[3] }); return rows([]);
      case SQL.retryable: return rows([...db.settlements.values()].filter(r => r.attempts < MAX_ATTEMPTS &&
        (r.status === 'failed' || (r.status === 'pending' && r.updated_at < new Date(Date.now() - 60_000)))));
      case SQL.list: return rows([...db.settlements.values()].reverse().filter(r => !params[1] || r.tx_sig === params[1]).slice(0, params[0])
        .map(r => ({ ...r, payee_name: db.payees.get(r.payee)?.display_name ?? null, payee_label: r.payee === MERCH ? 'Badge 1' : null })));
      default: throw new Error(`unexpected SQL ${sql}`);
    }
  };
  const calls = [];
  let n = 0;
  const nessie = { deposit: async (acct, o) => { calls.push([acct, o]); return depositImpl ? depositImpl(acct, o) : { deposit_id: `d-${++n}` }; } };
  return { db, calls, s: createSettlement({ db: fakeQ, nessie, decimals: () => 2 }) };
}

test('a bank payee: deposit of amountRaw/100 dollars into its Nessie account, row settled', async () => {
  const w = world();
  const tx = SIG(2);
  const r = await w.s.settleIfNeeded({ txSig: tx, payee: MERCH, amountRaw: '4000' });
  assert.deepEqual(w.calls, [['merch-acct', { amount: 40, description: `settlement ${tx.slice(0, 16)}` }]]);
  assert.deepEqual({ ...r, time: null }, {
    txSig: tx, time: null, payee: { pubkey: MERCH, name: 'MHacks Merch', label: 'Badge 1' }, amount: 40, status: 'settled', reason: null,
    nessieDepositId: 'd-1', attempts: 1, links: { tx: `https://explorer.solana.com/tx/${tx}?cluster=devnet` } });
  assert.deepEqual(await w.s.getSettlement(tx), r);
  assert.equal(await w.s.getSettlement(SIG(3)), null);
});

test('idempotent on the signature: a second call never deposits again', async () => {
  const w = world();
  await w.s.settleIfNeeded({ txSig: SIG(2), payee: MERCH, amountRaw: 4000 });
  const again = await w.s.settleIfNeeded({ txSig: SIG(2), payee: MERCH, amountRaw: 4000n });
  assert.equal(again.status, 'settled');
  assert.equal(w.calls.length, 1);
});

test('a hack payee or an unknown payee is not applicable, nothing recorded', async () => {
  const w = world();
  assert.deepEqual(await w.s.settleIfNeeded({ txSig: SIG(2), payee: HACKY, amountRaw: 4000 }), { status: 'not_applicable' });
  assert.deepEqual(await w.s.settleIfNeeded({ txSig: SIG(3), payee: 'Unknown1111111111111111111111111111111111111', amountRaw: 4000 }), { status: 'not_applicable' });
  assert.equal(w.db.settlements.size, 0);
  assert.equal(w.calls.length, 0);
});

test('fractional HACK and a missing account are skipped, never deposited or retried', async () => {
  const w = world();
  assert.deepEqual([(await w.s.settleIfNeeded({ txSig: SIG(2), payee: MERCH, amountRaw: 4050 })).status, w.db.settlements.get(SIG(2)).reason], ['skipped', 'fractional_amount']);
  assert.deepEqual([(await w.s.settleIfNeeded({ txSig: SIG(3), payee: PERSON, amountRaw: 100 })).reason], ['no_nessie_account']);
  assert.deepEqual([(await w.s.settleIfNeeded({ txSig: SIG(4), payee: MERCH, amountRaw: 0 })).reason], ['bad_amount']);
  assert.deepEqual(await w.s.retryPendingSettlements(), { retried: 0, settled: 0 });
  assert.equal(w.calls.length, 0);
});

test('a Nessie failure leaves a failed row that the tick retries until it settles, at most 5 attempts', async () => {
  let down = true;
  const w = world(() => { if (down) throw Object.assign(new Error('HTTP 500'), { code: 'nessie_error', status: 500 }); return { deposit_id: 'd-late' }; });
  const r = await w.s.settleIfNeeded({ txSig: SIG(2), payee: MERCH, amountRaw: 1000 });
  assert.deepEqual([r.status, r.reason, r.attempts, r.nessieDepositId], ['failed', 'nessie_error', 1, null]);
  assert.deepEqual(await w.s.retryPendingSettlements(), { retried: 1, settled: 0 });
  assert.equal(w.db.settlements.get(SIG(2)).attempts, 2);
  down = false;
  assert.deepEqual(await w.s.retryPendingSettlements(), { retried: 1, settled: 1 });
  assert.deepEqual([w.db.settlements.get(SIG(2)).status, w.db.settlements.get(SIG(2)).nessie_deposit_id, w.db.settlements.get(SIG(2)).attempts], ['settled', 'd-late', 3]);
  assert.deepEqual(await w.s.retryPendingSettlements(), { retried: 0, settled: 0 });

  const w2 = world(() => { throw new Error('down'); });
  await w2.s.settleIfNeeded({ txSig: SIG(3), payee: MERCH, amountRaw: 1000 });
  for (let i = 0; i < 10; i++) await w2.s.retryPendingSettlements();
  assert.equal(w2.calls.length, MAX_ATTEMPTS);
  assert.equal(w2.db.settlements.get(SIG(3)).status, 'failed');
});

test('a pending row is retried only once it is stale (its deposit may still be in flight)', async () => {
  const w = world();
  w.db.settlements.set(SIG(2), { tx_sig: SIG(2), created_at: new Date(), updated_at: new Date(), payee: MERCH, amount_raw: '2000',
                                 nessie_account_id: 'merch-acct', nessie_deposit_id: null, status: 'pending', reason: null, attempts: 1 });
  assert.deepEqual(await w.s.retryPendingSettlements(), { retried: 0, settled: 0 });
  w.db.settlements.get(SIG(2)).updated_at = new Date(Date.now() - 120_000);
  assert.deepEqual(await w.s.retryPendingSettlements(), { retried: 1, settled: 1 });
  assert.deepEqual(w.calls, [['merch-acct', { amount: 20, description: `settlement ${SIG(2).slice(0, 16)}` }]]);
});

test('bad input is refused before any lookup', async () => {
  const w = world();
  await assert.rejects(w.s.settleIfNeeded({ txSig: 'nope', payee: MERCH, amountRaw: 100 }), { code: 'invalid_param' });
  await assert.rejects(w.s.settleIfNeeded({ txSig: SIG(2), payee: MERCH, amountRaw: '1.5' }), { code: 'invalid_param' });
  const list = await w.s.listSettlements(10);
  assert.deepEqual(list, []);
});
