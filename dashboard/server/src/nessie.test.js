import test from 'node:test';
import assert from 'node:assert/strict';
import { createNessie } from './nessie.js';

const KEY = 'sekret-key-123';
const ok = body => ({ ok: true, status: 201, text: async () => JSON.stringify(body) });

function fake(responses) {
  const calls = [];
  const fetch = async (url, init) => { calls.push({ url, method: init.method, body: init.body ? JSON.parse(init.body) : null }); return responses.shift()(url); };
  return { calls, client: createNessie({ key: KEY, base: 'https://nessie.test/', fetch }) };
}

test('purchase posts integer dollars with status and medium, key as ?key=', async () => {
  const { calls, client } = fake([() => ok({ code: 201, objectCreated: { _id: 'p-1' } })]);
  assert.deepEqual(await client.purchase('acct-1', { merchantId: 'm-1', amount: 40, description: 'd' }), { purchase_id: 'p-1' });
  assert.equal(calls[0].url, `https://nessie.test/accounts/acct-1/purchases?key=${KEY}`);
  assert.equal(calls[0].method, 'POST');
  assert.deepEqual({ ...calls[0].body, purchase_date: 'x' },
    { merchant_id: 'm-1', medium: 'balance', purchase_date: 'x', status: 'completed', amount: 40, description: 'd' });
});

test('transferWithDeposit: transfer without medium on the payer, deposit on the payee', async () => {
  const { calls, client } = fake([() => ok({ objectCreated: { _id: 't-1' } }), () => ok({ objectCreated: { _id: 'd-1' } })]);
  assert.deepEqual(await client.transferWithDeposit('a', 'b', { amount: 5, description: 'x' }), { transfer_id: 't-1', deposit_id: 'd-1' });
  assert.match(calls[0].url, /\/accounts\/a\/transfers\?/);
  assert.equal(calls[0].body.medium, undefined);
  assert.match(calls[1].url, /\/accounts\/b\/deposits\?/);
  assert.equal(calls[1].body.medium, 'balance');
  assert.match(calls[1].body.description, /t-1/);
});

test('a failed deposit still reports the transfer id', async () => {
  const { client } = fake([() => ok({ objectCreated: { _id: 't-1' } }), () => ({ ok: false, status: 500, text: async () => 'boom' })]);
  await assert.rejects(client.transferWithDeposit('a', 'b', { amount: 5, description: 'x' }), { status: 500, transfer_id: 't-1' });
});

test('errors carry the status and never the key', async () => {
  const { client } = fake([url => ({ ok: false, status: 403, text: async () => `{"message":"bad ${url}"}` })]);
  const err = await client.getAccount('nope').catch(e => e);
  assert.equal(err.status, 403);
  assert.equal(err.code, 'nessie_error');
  assert.ok(!err.message.includes(KEY), err.message);
  const net = createNessie({ key: KEY, base: 'https://n.test', fetch: async url => { throw new Error(`connect failed ${url}`); } });
  const e2 = await net.getAccount('x').catch(e => e);
  assert.equal(e2.status, 502);
  assert.ok(!e2.message.includes(KEY), e2.message);
});

test('refuses fractional dollars and a missing key before calling out', async () => {
  const { calls, client } = fake([]);
  await assert.rejects(client.purchase('a', { merchantId: 'm', amount: 40.5, description: 'd' }), { status: 400 });
  assert.equal(calls.length, 0);
  await assert.rejects(createNessie({ key: '', fetch: () => assert.fail('no call') }).getAccount('a'), { status: 503 });
});

test('deposit and withdrawal post the deposit body to their own route', async () => {
  const { calls, client } = fake([() => ok({ objectCreated: { _id: 'd-1' } }), () => ok({ objectCreated: { _id: 'w-1' } })]);
  assert.deepEqual(await client.deposit('m-acct', { amount: 40, description: 'settlement x' }), { deposit_id: 'd-1' });
  assert.deepEqual(await client.withdrawal('j-acct', { amount: 25, description: 'top up' }), { withdrawal_id: 'w-1' });
  assert.equal(calls[0].url, `https://nessie.test/accounts/m-acct/deposits?key=${KEY}`);
  assert.equal(calls[1].url, `https://nessie.test/accounts/j-acct/withdrawals?key=${KEY}`);
  for (const [c, amount, description] of [[calls[0], 40, 'settlement x'], [calls[1], 25, 'top up']]) {
    assert.equal(c.method, 'POST');
    assert.match(c.body.transaction_date, /^\d{4}-\d{2}-\d{2}$/);
    assert.deepEqual({ ...c.body, transaction_date: 'x' }, { medium: 'balance', transaction_date: 'x', status: 'completed', amount, description });
  }
  await assert.rejects(client.withdrawal('j', { amount: 0.5, description: 'd' }), { status: 400 });
  assert.equal(calls.length, 2);
});
