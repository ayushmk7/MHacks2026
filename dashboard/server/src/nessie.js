import { readFileSync } from 'node:fs';
import { resolve } from 'node:path';
import { ROOT } from './config.js';

// Capital One Nessie client. This is the 2026 rebuild, not the classic docs (verified against the live API):
// amounts are integer dollars, IDs are UUIDs, unknown routes answer 403, a transfer has no recipient (a person
// payment = transfer on the payer + deposit on the payee), and recording money never moves the balance.
// The key travels as ?key= and is redacted from every error message, since errors reach logs and API responses.
export const NESSIE_CONFIG = resolve(ROOT, 'server/config/nessie.json');

const today = () => new Date().toISOString().slice(0, 10);
const createdId = (r, what) => r?.objectCreated?._id ?? r?._id ?? fail(502, `no id in Nessie ${what} response`);
function fail(status, message) { throw Object.assign(new Error(message), { code: 'nessie_error', status }); }

// Seed IDs written by scripts/seed-nessie.mjs. Not secret (the key is not in it). null when the seed never ran.
export function loadNessieConfig(path = NESSIE_CONFIG) {
  try { return JSON.parse(readFileSync(path, 'utf8')); } catch { return null; }
}

// key/base/fetch are read per call by default so tests (and a late-loaded .env) need no re-import.
export function createNessie({ key = () => process.env.NESSIE_KEY || '', base = () => process.env.NESSIE_BASE_URL || 'https://prod-api.nessieisreal.com',
                               fetch: fetchFn = (...a) => globalThis.fetch(...a), timeoutMs = 10_000 } = {}) {
  const val = v => (typeof v === 'function' ? v() : v);

  async function api(method, path, body) {
    const k = val(key);
    if (!k) fail(503, 'NESSIE_KEY is not configured');
    const redact = s => String(s).split(k).join('<key>').split(encodeURIComponent(k)).join('<key>');
    const url = `${val(base).replace(/\/$/, '')}${path}?key=${encodeURIComponent(k)}`;
    let res;
    try {
      res = await fetchFn(url, { method, headers: body ? { 'Content-Type': 'application/json' } : {},
                                 body: body ? JSON.stringify(body) : undefined, signal: AbortSignal.timeout(timeoutMs) });
    } catch (err) { fail(502, redact(`${method} ${path}: ${err.message}`)); }
    const text = await res.text();
    let json; try { json = JSON.parse(text); } catch { json = text; }
    if (!res.ok) fail(res.status, redact(`${method} ${path} -> HTTP ${res.status}: ${(typeof json === 'string' ? json : JSON.stringify(json)).slice(0, 300)}`));
    return json;
  }

  // Integer dollars only: a fractional amount would be silently rounded or refused by Nessie, so refuse it here first.
  const dollars = amount => (Number.isSafeInteger(amount) && amount > 0 ? amount : fail(400, `amount must be a positive whole number of dollars, got ${amount}`));

  return {
    api,
    createCustomer: async ({ first_name, last_name, address }) =>
      createdId(await api('POST', '/customers', { first_name, last_name, address }), 'customer'),
    createAccount: async (customerId, { nickname, balance = 0, rewards = 0 }) =>
      createdId(await api('POST', `/customers/${encodeURIComponent(customerId)}/accounts`, { type: 'Checking', nickname, rewards, balance }), 'account'),
    getAccount: id => api('GET', `/accounts/${encodeURIComponent(id)}`),
    // status is required: a purchase created without it cannot be read back (GET /purchase/{id}).
    purchase: async (accountId, { merchantId, amount, description }) => ({
      purchase_id: createdId(await api('POST', `/accounts/${encodeURIComponent(accountId)}/purchases`, {
        merchant_id: merchantId, medium: 'balance', purchase_date: today(), status: 'completed', amount: dollars(amount), description }), 'purchase'),
    }),
    // Two writes, not atomic. If the deposit fails the error carries transfer_id so the caller can record the half-done payment.
    transferWithDeposit: async (fromAccountId, toAccountId, { amount, description }) => {
      const transfer_id = createdId(await api('POST', `/accounts/${encodeURIComponent(fromAccountId)}/transfers`, {
        transaction_date: today(), status: 'completed', amount: dollars(amount), description }), 'transfer');
      try {
        const deposit_id = createdId(await api('POST', `/accounts/${encodeURIComponent(toAccountId)}/deposits`, {
          medium: 'balance', transaction_date: today(), status: 'completed', amount, description: `${description} (transfer ${transfer_id})` }), 'deposit');
        return { transfer_id, deposit_id };
      } catch (err) { throw Object.assign(err, { transfer_id }); }
    },
  };
}

export const nessie = createNessie();
