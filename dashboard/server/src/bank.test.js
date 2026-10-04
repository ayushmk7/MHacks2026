import test from 'node:test';
import assert from 'node:assert/strict';
import { createHash, generateKeyPairSync, randomUUID, sign } from 'node:crypto';
import { setImmediate as tick } from 'node:timers/promises';
import { createBank, SQL, MEMO_PROGRAM, toApproval } from './bank.js';
import { PREFIX, withPrefix, proofMessage, b58 } from './frames.js';

// Offline: the DB, Nessie and the Solana sender are fakes. Keys are generated per run.
const NOW = 1_800_000_000;
console.warn = () => {};   // refusals are expected here, keep the output readable
const keyPair = () => {
  const { publicKey, privateKey } = generateKeyPairSync('ed25519');
  return { pub: publicKey.export({ format: 'der', type: 'spki' }).subarray(12), sign: msg => sign(null, msg, privateKey) };
};
const sha256 = (...p) => createHash('sha256').update(Buffer.concat(p)).digest();
const b64 = b => Buffer.from(b).toString('base64');

function buildReq(kp, { payeePub = kp.pub, rail = 2, amount = 4000n, currency = 'USD\0', reqId, expiry = NOW + 60, name = 'Shop' }) {
  const head = Buffer.alloc(62);
  head.write('VK', 0, 'latin1'); head[2] = 1; head[3] = 1; head[4] = rail;
  payeePub.copy(head, 5);
  head.writeBigUInt64LE(BigInt(amount), 37);
  head.write(currency, 45, 'latin1');
  Buffer.from(reqId, 'hex').copy(head, 49);
  head.writeUInt32LE(expiry, 57);
  head[61] = name.length;
  const signed = Buffer.concat([head, Buffer.from(name, 'latin1')]);
  return Buffer.concat([signed, kp.sign(withPrefix(PREFIX.PAY_REQ, signed))]);
}

// World: one enrolled payer, a merchant payee and a person payee, each with a record and a payees row.
function world(nessieOver = {}) {
  const payer = keyPair(), merchant = keyPair(), person = keyPair();
  const payerPk = b58(payer.pub);
  const mk = (kp, kind, name, ref) => {
    const salt = Buffer.alloc(16, kind.length);
    const hash = ref ? sha256(salt, Buffer.from(ref)) : null;
    return { kp, pk: b58(kp.pub), ref, salt, hash,
             record: { attestation: 'none', display_name: name, device_pubkey: Buffer.from(kp.pub), kind, solana_wallet: null, solana_ata: null,
                       bank_ref_hash: hash, expiry: NOW + 86_400, status: 'active', issued_at: NOW } };
  };
  const payees = { merchant: mk(merchant, 'merchant', 'MHacks Merch', 'merchant-uuid'), person: mk(person, 'person', 'Sam Witwicky', 'sam-acct') };

  const db = { enrollments: new Map([[payerPk, { badge_pubkey: payerPk, nessie_customer_id: 'cust-1', nessie_account_id: 'acct-1' }]]),
               payees: new Map(Object.values(payees).map(p => [p.pk, { pubkey: p.pk, kind: p.record.kind, nessie_ref: p.ref, salt: p.salt }])),
               nonces: new Set(), approvals: [], topups: [], settlements: [] };
  const fakeQ = async (sql, params) => {
    const rows = r => ({ rows: r, rowCount: r.length });
    switch (sql) {
      case SQL.enrollment: return rows([db.enrollments.get(params[0])].filter(Boolean));
      case SQL.upsertEnrollment: db.enrollments.set(params[0], { badge_pubkey: params[0], nessie_customer_id: params[1], nessie_account_id: params[2] }); return rows([]);
      case SQL.payee: return rows([db.payees.get(params[0])].filter(Boolean));
      case SQL.insertNonce: {
        const [p, r, n] = params;
        if (db.nonces.has(`r|${p}|${r}`) || db.nonces.has(`n|${p}|${n}`)) return rows([]);
        db.nonces.add(`r|${p}|${r}`); db.nonces.add(`n|${p}|${n}`);
        return rows([{ payer: p }]);
      }
      case SQL.insertApproval: {
        const [rail, source, payer, payee, payee_name, amount_cents, status, reason, req_id, payload_hash] = params;
        const row = { id: randomUUID(), created_at: new Date(), rail, source, payer, payee, payee_name, amount_cents, status, reason, req_id, payload_hash, nessie_ids: null, memo_sig: null };
        db.approvals.push(row);
        return rows([{ id: row.id }]);
      }
      case SQL.finishApproval: Object.assign(db.approvals.find(a => a.id === params[0]), { status: params[1], reason: params[2], nessie_ids: JSON.parse(params[3]) }); return rows([]);
      case SQL.setMemo: db.approvals.find(a => a.id === params[0]).memo_sig = params[1]; return rows([]);
      case SQL.ledger: {
        const ok = db.approvals.filter(a => a.rail === 'nessie' && a.source === 'backend' && a.status === 'approved');
        const sum = (xs, f = 'amount_cents') => xs.reduce((s, a) => s + Number(a[f]), 0);
        const acct = (xs, status) => xs.filter(x => x.nessie_account_id === params[1] && x.status === status);
        return rows([{ debits: String(sum(ok.filter(a => a.payer === params[0])) + sum(acct(db.topups, 'done'), 'amount_raw')),
                       credits: String(sum(ok.filter(a => a.payee === params[0] && a.nessie_ids?.deposit_id)) + sum(acct(db.settlements, 'settled'), 'amount_raw')) }]);
      }
      case SQL.approvals: return rows([...db.approvals].reverse().filter(a => !params[1] || a.id === params[1]).slice(0, params[0]));
      default: throw new Error(`unexpected SQL ${sql}`);
    }
  };
  const calls = [];
  const nessie = {
    purchase: async (acct, o) => { calls.push(['purchase', acct, o]); return { purchase_id: 'p-1' }; },
    transferWithDeposit: async (from, to, o) => { calls.push(['transfer', from, to, o]); return { transfer_id: 't-1', deposit_id: 'd-1' }; },
    getAccount: async id => { calls.push(['getAccount', id]); return { _id: id, balance: 500, customer_id: 'cust-9' }; },
    createCustomer: async () => { calls.push(['createCustomer']); return 'cust-new'; },
    createAccount: async c => { calls.push(['createAccount', c]); return 'acct-new'; },
    ...nessieOver,
  };
  const sent = [];
  const send = async (signer, ixs) => { sent.push(ixs); return 'memoSig111'; };
  const bank = createBank({ db: fakeQ, nessie, send });
  const recordFields = async pk => Object.values(payees).find(p => p.pk === pk)?.record ?? null;

  // A valid request to `to`, with any field overridable, signed by the right keys unless told otherwise.
  let seq = 0;
  const request = (to = 'merchant', o = {}) => {
    const p = payees[to];
    const reqId = o.reqId ?? (++seq).toString(16).padStart(16, '0');
    const nonce = o.nonce ?? randomUUID().replaceAll('-', '');
    const amount = o.amount ?? 4000;
    const payload = [
      'v=1', 'rail=nessie', `action=${o.action ?? (p.record.kind === 'person' ? 'transfer' : 'purchase')}`, `amount_cents=${amount}`, 'currency=USD',
      `from_acct=${o.from_acct ?? 'acct-1'}`, `payee_name=${o.payee_name ?? p.record.display_name}`,
      `payee_ref=${o.payee_ref ?? (p.hash ? p.hash.toString('hex') : '00'.repeat(32))}`, `attestation=${p.record.attestation}`,
      `req_id=${reqId}`, `proof_nonce=${nonce}`, `issued_at=${o.issued_at ?? NOW - 5}`,
    ].join('\n') + (o.payloadSuffix ?? '');
    const req = buildReq(o.reqSigner ?? p.kp, { payeePub: p.kp.pub, reqId, amount: o.reqAmount ?? amount, ...o.req });
    const proofSigner = o.proofSigner ?? p.kp;
    return {
      payload: b64(Buffer.from(payload)), payer_pubkey: payerPk, req: b64(req),
      sig: b64((o.payerSigner ?? payer).sign(withPrefix(PREFIX.BANK_AUTH, Buffer.from(payload)))),
      proof_sig: b64(proofSigner.sign(proofMessage(reqId, nonce, payer.pub))),
    };
  };
  const authorize = body => bank.authorize(body, { recordFields, authority: { address: 'Authority1111' }, now: () => NOW });
  return { db, calls, sent, bank, payees, payerPk, request, authorize, keyPair };
}

test('happy path, merchant: Nessie purchase to the stored merchant id, approved row, memo lands', async () => {
  const w = world();
  const body = w.request('merchant');
  assert.deepEqual(await w.authorize(body), { ok: true, nessie_id: 'p-1' });
  assert.deepEqual(w.calls, [['purchase', 'acct-1', { merchantId: 'merchant-uuid', amount: 40, description: w.calls[0][2].description }]]);
  const [row] = w.db.approvals;
  assert.equal(row.status, 'approved');
  assert.equal(row.amount_cents, 4000);
  assert.equal(row.payee, w.payees.merchant.pk);
  assert.deepEqual(row.nessie_ids, { purchase_id: 'p-1' });
  await tick(); await tick();
  assert.equal(row.memo_sig, 'memoSig111');
  const [[ix]] = w.sent;
  assert.equal(ix.programAddress, MEMO_PROGRAM);
  assert.deepEqual(ix.accounts, []);
  const hash = createHash('sha256').update(Buffer.from(body.payload, 'base64')).digest('hex');
  assert.equal(Buffer.from(ix.data).toString(), `appr:v1:nessie:${hash}`);
  assert.equal(row.payload_hash, hash);
  assert.equal(toApproval(row).links.memo, 'https://explorer.solana.com/tx/memoSig111?cluster=devnet');
});

test('happy path, person: transfer on the payer plus deposit on the payee account', async () => {
  const w = world();
  assert.deepEqual(await w.authorize(w.request('person', { amount: 1500 + 500 })), { ok: true, nessie_id: 't-1' });
  assert.equal(w.calls[0][0], 'transfer');
  assert.deepEqual(w.calls[0].slice(1, 3), ['acct-1', 'sam-acct']);
  assert.equal(w.calls[0][3].amount, 20);
  assert.deepEqual(w.db.approvals[0].nessie_ids, { transfer_id: 't-1', deposit_id: 'd-1' });
});

test('a raw-text payload is accepted as well as base64', async () => {
  const w = world();
  const body = w.request('merchant');
  assert.equal((await w.authorize({ ...body, payload: Buffer.from(body.payload, 'base64').toString() })).ok, true);
});

// Each case breaks exactly one step and must be refused with its reason, a blocked row, and no Nessie call.
const cases = {
  not_enrolled: w => ({ ...w.request(), payer_pubkey: b58(w.keyPair().pub) }),
  bad_sig: w => w.request('merchant', { payerSigner: w.keyPair() }),
  bad_payload: w => w.request('merchant', { payloadSuffix: '\n' }),
  stale: w => w.request('merchant', { issued_at: NOW - 76 }),
  bad_req_sig: w => w.request('merchant', { reqSigner: w.keyPair() }),
  req_expired: w => w.request('merchant', { req: { expiry: NOW - 1 } }),
  wrong_rail: w => w.request('merchant', { req: { rail: 1, currency: 'HACK' } }),
  amount_mismatch: w => w.request('merchant', { reqAmount: 9900 }),
  revoked: w => { w.payees.merchant.record.status = 'revoked'; return w.request(); },
  expired: w => { w.payees.merchant.record.expiry = NOW - 1; return w.request(); },
  payee_ref_mismatch: w => w.request('merchant', { payee_ref: 'cd'.repeat(32) }),
  payee_name_mismatch: w => w.request('merchant', { payee_name: 'MHacks Merch2' }),
  action_mismatch: w => w.request('merchant', { action: 'transfer' }),
  relay_payee: w => { Object.assign(w.payees.merchant.record, { kind: 'relay', bank_ref_hash: null }); return w.request(); },
  bad_proof: w => w.request('merchant', { proofSigner: w.keyPair() }),
  acct_mismatch: w => w.request('merchant', { from_acct: 'acct-2' }),
  bad_amount: w => w.request('merchant', { amount: 0 }),
  amount_not_whole_dollars: w => w.request('merchant', { amount: 4050 }),
  unknown_payee: w => { const b = w.request(); w.payees.merchant.record = null; return b; },
  payee_not_configured: w => { w.db.payees.clear(); return w.request(); },
};
for (const [reason, make] of Object.entries(cases)) {
  test(`refused: ${reason}`, async () => {
    const w = world();
    const body = make(w);
    assert.deepEqual(await w.authorize(body), { ok: false, reason });
    assert.deepEqual(w.calls, []);
    assert.equal(w.db.approvals.length, 1);
    assert.equal(w.db.approvals[0].status, 'blocked');
    assert.equal(w.db.approvals[0].reason, reason);
    assert.equal(w.sent.length, 0);
  });
}

test('a payload that does not hash to the record, though it signs, is refused (stored target swapped)', async () => {
  const w = world();
  w.db.payees.get(w.payees.merchant.pk).nessie_ref = 'attacker-merchant';
  assert.deepEqual(await w.authorize(w.request()), { ok: false, reason: 'payee_ref_mismatch' });
  assert.deepEqual(w.calls, []);
});

test('replay: the same nonce or the same req_id authorizes once', async () => {
  const w = world();
  const body = w.request('merchant', { nonce: 'ab'.repeat(16) });
  assert.equal((await w.authorize(body)).ok, true);
  assert.deepEqual(await w.authorize(body), { ok: false, reason: 'replay' });
  assert.deepEqual(await w.authorize(w.request('merchant', { nonce: 'ab'.repeat(16) })), { ok: false, reason: 'replay' });
  assert.deepEqual(await w.authorize(w.request('merchant', { reqId: '0000000000000001' })), { ok: false, reason: 'replay' });
  assert.equal(w.calls.length, 1);
});

test('routed payloads and malformed bodies are refused', async () => {
  const w = world();
  assert.deepEqual(await w.authorize({ ...w.request(), route: { route_id: 'x' } }), { ok: false, reason: 'route_unsupported' });
  assert.deepEqual(await w.authorize({}), { ok: false, reason: 'bad_request' });
  assert.deepEqual(await w.authorize({ ...w.request(), sig: 'not base64!' }), { ok: false, reason: 'bad_request' });
  assert.deepEqual(await w.authorize({ ...w.request(), req: b64(Buffer.alloc(10)) }), { ok: false, reason: 'bad_req' });
  assert.deepEqual(w.calls, []);
});

test('a Nessie failure marks the pending row failed and keeps a half-done transfer id', async () => {
  const w = world({ transferWithDeposit: async () => { throw Object.assign(new Error('deposit -> HTTP 500'), { status: 500, transfer_id: 't-9' }); } });
  assert.deepEqual(await w.authorize(w.request('person')), { ok: false, reason: 'nessie_error' });
  assert.equal(w.db.approvals.length, 1);
  assert.equal(w.db.approvals[0].status, 'failed');
  assert.deepEqual(w.db.approvals[0].nessie_ids, { transfer_id: 't-9' });
  assert.equal(w.sent.length, 0);
});

test('balance = Nessie opening balance - approved debits + approved deposits', async () => {
  const w = world();
  assert.deepEqual(await w.bank.balance(w.payerPk), { usd_cents: 50_000, account_id: 'acct-1' });
  await w.authorize(w.request('merchant', { amount: 4000 }));
  await w.authorize(w.request('merchant', { amount: 4050 }));   // blocked: not counted
  assert.deepEqual(await w.bank.balance(w.payerPk), { usd_cents: 46_000, account_id: 'acct-1' });
  w.db.enrollments.set(w.payees.person.pk, { badge_pubkey: w.payees.person.pk, nessie_customer_id: 'c', nessie_account_id: 'sam-acct' });
  await w.authorize(w.request('person', { amount: 1000 }));
  assert.deepEqual(await w.bank.balance(w.payees.person.pk), { usd_cents: 51_000, account_id: 'sam-acct' });
  await assert.rejects(w.bank.balance(b58(w.keyPair().pub)), { code: 'not_found' });
});

test('balance also counts completed top-ups as debits and settled settlements as credits, by Nessie account', async () => {
  const w = world();
  w.db.topups.push({ nessie_account_id: 'acct-1', status: 'done', amount_raw: 2500 },
                   { nessie_account_id: 'acct-1', status: 'failed', amount_raw: 9900 },
                   { nessie_account_id: 'acct-1', status: 'pending', amount_raw: 9900 },
                   { nessie_account_id: 'acct-2', status: 'done', amount_raw: 9900 });
  w.db.settlements.push({ nessie_account_id: 'acct-1', status: 'settled', amount_raw: 4000 },
                        { nessie_account_id: 'acct-1', status: 'failed', amount_raw: 9900 },
                        { nessie_account_id: 'acct-1', status: 'skipped', amount_raw: 9950 },
                        { nessie_account_id: 'acct-9', status: 'settled', amount_raw: 9900 });
  assert.deepEqual(await w.bank.balance(w.payerPk), { usd_cents: 50_000 - 2500 + 4000, account_id: 'acct-1' });
});

test('enroll: given ids are checked read-only, missing ids create a customer and account', async () => {
  const w = world();
  const pk = b58(w.keyPair().pub);
  assert.deepEqual(await w.bank.enroll({ pubkey: pk, nessieAccountId: 'acct-7' }), { customer_id: 'cust-9', account_id: 'acct-7' });
  assert.deepEqual(w.calls, [['getAccount', 'acct-7']]);
  assert.deepEqual(await w.bank.enroll({ pubkey: pk }), { customer_id: 'cust-new', account_id: 'acct-new' });
  assert.equal(w.db.enrollments.get(pk).nessie_account_id, 'acct-new');
  await assert.rejects(w.bank.enroll({ pubkey: 'nope' }), { code: 'invalid_pubkey' });
  await assert.rejects(w.bank.enroll({ pubkey: pk, nessieAccountId: '../x' }), { code: 'invalid_param' });
});

test('recordBadgeEvent stores a badge_report row from either body shape, clipping untrusted input', async () => {
  const w = world();
  const req = Buffer.from(w.request().req, 'base64');
  await w.bank.recordBadgeEvent({ payer_pubkey: w.payerPk, req: b64(req), reason: 'impostor' });
  await w.bank.recordBadgeEvent({ payer: w.payerPk, payee: w.payees.person.pk, reason: '<script>' });
  const [a, b] = w.db.approvals;
  assert.deepEqual([a.source, a.status, a.rail, a.payer, a.payee, a.amount_cents, a.reason], ['badge_report', 'blocked', 'nessie', w.payerPk, w.payees.merchant.pk, 4000, 'impostor']);
  assert.deepEqual([b.rail, b.payee, b.reason], ['unknown', w.payees.person.pk, 'unspecified']);
  const list = await w.bank.listApprovals(10);
  assert.equal(list.length, 2);
  assert.equal(list[0].source, 'badge_report');
  assert.deepEqual(Object.keys(list[0]).sort(), ['amountCents', 'id', 'links', 'memoSig', 'nessieIds', 'payee', 'payer', 'rail', 'reason', 'reqId', 'solanaSig', 'source', 'status', 'time']);
});
