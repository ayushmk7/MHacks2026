// npm run nessie:seed [-- --check]
// Seeds Capital One Nessie for the demo: one customer + checking account per payer, one person payee
// (customer + account), the "MHacks Merch" merchant, the business customer + checking account that receives MHacks
// Merch's Capital One settlements, one test purchase and one test transfer.
// Idempotent: IDs are saved to server/config/nessie.json after every create and reused on re-runs.
// --check is read-only: verifies the key and prints what the seed would create. The key is never printed.
import { readFileSync, writeFileSync, existsSync } from 'node:fs';
import { fileURLToPath } from 'node:url';

const OUT = fileURLToPath(new URL('../server/config/nessie.json', import.meta.url));
const KEY = process.env.NESSIE_KEY || '';
const BASE = (process.env.NESSIE_BASE_URL || 'https://prod-api.nessieisreal.com').replace(/\/$/, '');
const CHECK = process.argv.includes('--check');

const ADDRESS = { street_number: '500', street_name: 'S State St', city: 'Ann Arbor', state: 'MI', zip: '48109' };
const PAYERS = [
  { role: 'judge_a', first_name: 'Judge', last_name: 'A', balance: 500 },
  { role: 'judge_b', first_name: 'Judge', last_name: 'B', balance: 500 },
];
const PERSON = { role: 'person_payee', first_name: 'Sam', last_name: 'Witwicky', balance: 100 };
const MERCHANT = { name: 'MHacks Merch', category: 'Merchandise', address: ADDRESS, geocode: { lat: 42.2780, lng: -83.7382 } };
// The business behind the merchant: Capital One settlement deposits land in this account (merchant.account_id).
// Nessie customers have no business flag, so it is a customer named after the business.
const MERCHANT_HOLDER = { first_name: 'MHacks', last_name: 'Merch', balance: 0 };
const TEST_AMOUNT = 1;   // dollars: Nessie amounts are dollars, the badge payload is cents

const redact = s => (KEY ? String(s).split(KEY).join('<key>') : String(s));
const today = () => new Date().toISOString().slice(0, 10);

async function api(method, path, body) {
  const url = `${BASE}${path}${path.includes('?') ? '&' : '?'}key=${encodeURIComponent(KEY)}`;
  let res;
  try {
    res = await fetch(url, { method, headers: body ? { 'Content-Type': 'application/json' } : {}, body: body ? JSON.stringify(body) : undefined });
  } catch (err) { throw new Error(redact(`${method} ${path}: ${err.message}`)); }
  const text = await res.text();
  let json; try { json = JSON.parse(text); } catch { json = text; }
  if (!res.ok) throw Object.assign(new Error(redact(`${method} ${path} -> HTTP ${res.status}: ${typeof json === 'string' ? json.slice(0, 300) : JSON.stringify(json).slice(0, 300)}`)), { status: res.status });
  return json;
}
const createdId = (r, what) => r?.objectCreated?._id ?? r?._id ?? (() => { throw new Error(`no id in ${what} response: ${redact(JSON.stringify(r)).slice(0, 300)}`); })();
const stillThere = async path => { try { await api('GET', path); return true; } catch (e) { if (e.status === 404) return false; throw e; } };

const state = existsSync(OUT) ? JSON.parse(readFileSync(OUT, 'utf8')) : { base: BASE, payers: {}, person_payee: null, merchant: null, tests: {} };
const save = () => writeFileSync(OUT, `${JSON.stringify(state, null, 2)}\n`);

async function ensureHolder(p) {
  const cur = p.role === PERSON.role ? state.person_payee : state.payers[p.role];
  if (cur?.account_id && await stillThere(`/accounts/${cur.account_id}`)) return { ...cur, reused: true };
  const customer_id = cur?.customer_id && await stillThere(`/customers/${cur.customer_id}`) ? cur.customer_id
    : createdId(await api('POST', '/customers', { first_name: p.first_name, last_name: p.last_name, address: ADDRESS }), 'customer');
  const account_id = createdId(await api('POST', `/customers/${customer_id}/accounts`,
    { type: 'Checking', nickname: `${p.first_name} ${p.last_name} checking`, rewards: 0, balance: p.balance }), 'account');
  const entry = { name: `${p.first_name} ${p.last_name}`, customer_id, account_id };
  if (p.role === PERSON.role) state.person_payee = entry; else state.payers[p.role] = entry;
  save();
  return entry;
}

async function main() {
  if (!KEY) { console.error('NESSIE_KEY is empty: paste your key into dashboard/.env (NESSIE_KEY=...)'); process.exit(1); }
  console.log(`base      ${BASE}`);

  if (CHECK) {
    const customers = await api('GET', '/customers');
    console.log(`key       OK (GET /customers returned ${Array.isArray(customers) ? customers.length : '?'} customers under this key)`);
    if (Array.isArray(customers) && customers.length) console.log(`existing  ${customers.map(c => `${c.first_name} ${c.last_name}`).join(', ')}`);
    console.log(`state     ${existsSync(OUT) ? `${OUT} exists, IDs in it are reused` : 'no nessie.json yet, everything would be created'}`);
    console.log('would create:');
    for (const p of [...PAYERS, PERSON]) console.log(`  customer ${p.first_name} ${p.last_name} + Checking account ($${p.balance})`);
    console.log(`  merchant ${MERCHANT.name} (${MERCHANT.address.city}, ${MERCHANT.address.state})`);
    const m = state.merchant ?? {};
    console.log(`  business customer ${MERCHANT_HOLDER.first_name} ${MERCHANT_HOLDER.last_name} + Checking account ($${MERCHANT_HOLDER.balance}) for settlements`
      + (m.account_id ? ` (saved: customer ${m.customer_id}, account ${m.account_id}, reused if still there)` : ''));
    console.log(`  purchase Judge A -> ${MERCHANT.name} $${TEST_AMOUNT}.00 (medium balance)`);
    console.log(`  transfer Judge A -> ${PERSON.first_name} ${PERSON.last_name} $${TEST_AMOUNT}.00 (transfer on payer + matching deposit on payee)`);
    console.log('\nCheck done. Nothing was created.');
    return;
  }

  for (const p of PAYERS) { const e = await ensureHolder(p); console.log(`payer     ${e.name}  customer ${e.customer_id}  account ${e.account_id}${e.reused ? '  (reused)' : ''}`); }
  const person = await ensureHolder(PERSON);
  console.log(`person    ${person.name}  customer ${person.customer_id}  account ${person.account_id}${person.reused ? '  (reused)' : ''}`);

  if (!(state.merchant?.id && await stillThere(`/merchants/${state.merchant.id}`))) {
    state.merchant = { ...state.merchant, name: MERCHANT.name, id: createdId(await api('POST', '/merchants', MERCHANT), 'merchant') };
    save();
  }
  console.log(`merchant  ${state.merchant.name}  ${state.merchant.id}`);

  // Settlement account: issue the merchant with settleMode 'bank' and nessieAccountId = this account id.
  if (!(state.merchant.account_id && await stillThere(`/accounts/${state.merchant.account_id}`))) {
    const h = MERCHANT_HOLDER;
    const customer_id = state.merchant.customer_id && await stillThere(`/customers/${state.merchant.customer_id}`) ? state.merchant.customer_id
      : createdId(await api('POST', '/customers', { first_name: h.first_name, last_name: h.last_name, address: ADDRESS }), 'customer');
    state.merchant.customer_id = customer_id;
    save();
    state.merchant.account_id = createdId(await api('POST', `/customers/${customer_id}/accounts`,
      { type: 'Checking', nickname: `${MERCHANT.name} settlement`, rewards: 0, balance: h.balance }), 'account');
    save();
  }
  console.log(`business  ${MERCHANT.name}  customer ${state.merchant.customer_id}  account ${state.merchant.account_id}  (settlement deposits)`);

  // This Nessie (2026 rebuild) differs from the classic docs: amounts are integers (whole dollars), purchases are created with `purchase_date`,
  // other records use `transaction_date`, `status` is required to read a record back, and a transfer has no
  // recipient field (it only debits). A person-to-person payment is therefore a transfer on the payer
  // plus a deposit on the payee, linked by the transfer id in the deposit's description.
  const judgeA = state.payers.judge_a.account_id;
  if (state.tests.purchase_id) {
    // A purchase created without status can't be read back, and PurchaseUpdate doesn't accept status:
    // delete that one record and recreate it below.
    try { await api('GET', `/purchase/${state.tests.purchase_id}`); }
    catch (e) {
      if (e.status !== 400) throw e;
      await api('DELETE', `/purchase/${state.tests.purchase_id}`);
      console.log(`deleted   unreadable purchase ${state.tests.purchase_id}`);
      delete state.tests.purchase_id;
      save();
    }
  }
  if (!state.tests.purchase_id) {
    state.tests.purchase_id = createdId(await api('POST', `/accounts/${judgeA}/purchases`, {
      merchant_id: state.merchant.id, medium: 'balance', purchase_date: today(), status: 'completed',
      amount: TEST_AMOUNT, description: 'seed test purchase' }), 'purchase');
    save();
  }
  if (!state.tests.transfer_id) {
    state.tests.transfer_id = createdId(await api('POST', `/accounts/${judgeA}/transfers`, {
      transaction_date: today(), status: 'completed', amount: TEST_AMOUNT,
      description: `seed test transfer to ${person.name} (account ${person.account_id})` }), 'transfer');
    save();
  }
  if (!state.tests.deposit_id) {
    state.tests.deposit_id = createdId(await api('POST', `/accounts/${person.account_id}/deposits`, {
      medium: 'balance', transaction_date: today(), status: 'completed', amount: TEST_AMOUNT,
      description: `seed test transfer from Judge A (transfer ${state.tests.transfer_id})` }), 'deposit');
    save();
  }

  const show = async (label, path) => {
    try { const r = await api('GET', path); console.log(`${label.padEnd(9)} ${path}  status=${r.status}  amount=${r.amount}`); }
    catch (e) { console.log(`${label.padEnd(9)} ${path}  read failed: ${redact(e.message)}`); }
  };
  await show('purchase', `/purchase/${state.tests.purchase_id}`);
  await show('transfer', `/transfers/${state.tests.transfer_id}`);
  await show('deposit', `/deposits/${state.tests.deposit_id}`);
  for (const [label, id] of [['Judge A', judgeA], ['Judge B', state.payers.judge_b.account_id], [person.name, person.account_id], [MERCHANT.name, state.merchant.account_id]]) {
    const a = await api('GET', `/accounts/${id}`);
    console.log(`balance   ${label.padEnd(12)} $${a.balance}`);
  }
  console.log(`\nSaved IDs to ${OUT}`);
}

main().catch(err => { console.error(`nessie seed failed: ${redact(err.message)}`); process.exit(1); });
