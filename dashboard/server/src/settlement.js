import { q } from './db.js';
import { token } from './config.js';
import { nessie as nessieClient } from './nessie.js';

// Capital One settlement (PROJECT-OVERVIEW §4.2): a settle_mode='bank' payee is paid on chain into the issuer's
// settlement wallet, then the backend deposits the same amount in dollars into the payee's Nessie account.
// Only ever called for a payment already confirmed on chain. One row per tx_sig: a signature settles at most once.

export const MAX_ATTEMPTS = 5;
const EXPLORER = 'https://explorer.solana.com';

export const SQL = {
  payee: `SELECT pubkey, display_name, settle_mode, nessie_account_id FROM payees WHERE pubkey = $1`,

  // DO NOTHING on the PK: a second call for the same signature inserts nothing and returns no row.
  insert: `
INSERT INTO settlements (tx_sig, payee, amount_raw, nessie_account_id, status, reason) VALUES ($1, $2, $3, $4, $5, $6)
ON CONFLICT (tx_sig) DO NOTHING RETURNING tx_sig`,

  // Claim one attempt: only the caller that saw `attempts` = $2 wins, so two retries never deposit twice.
  claim: `
UPDATE settlements SET status = 'pending', attempts = attempts + 1, updated_at = now()
WHERE tx_sig = $1 AND attempts = $2 AND attempts < ${MAX_ATTEMPTS} AND status IN ('pending', 'failed')
RETURNING tx_sig, nessie_account_id, amount_raw, attempts`,

  finish: `UPDATE settlements SET status = $2, reason = $3, nessie_deposit_id = $4, updated_at = now() WHERE tx_sig = $1`,

  // A pending row younger than a minute may still have its deposit in flight (Nessie times out after 10 s).
  retryable: `
SELECT tx_sig, attempts FROM settlements
WHERE attempts < ${MAX_ATTEMPTS} AND (status = 'failed' OR (status = 'pending' AND updated_at < now() - INTERVAL '60 seconds'))
ORDER BY created_at LIMIT 20`,

  // $1 limit, $2 tx_sig or NULL.
  list: `
SELECT s.*, p.display_name AS payee_name, b.label AS payee_label
FROM settlements s LEFT JOIN payees p ON p.pubkey = s.payee LEFT JOIN badges b ON b.pubkey = s.payee
WHERE ($2::text IS NULL OR s.tx_sig = $2)
ORDER BY s.created_at DESC LIMIT $1`,
};

const fail = (code, message) => { throw Object.assign(new Error(message), { code }); };
const iso = d => (d ? d.toISOString() : null);

export const toSettlement = r => ({
  txSig: r.tx_sig, time: iso(r.created_at),
  payee: { pubkey: r.payee, name: r.payee_name ?? null, label: r.payee_label ?? null },
  amount: Number(r.amount_raw) / 10 ** token.decimals, status: r.status, reason: r.reason,
  nessieDepositId: r.nessie_deposit_id, attempts: r.attempts,
  links: { tx: `${EXPLORER}/tx/${r.tx_sig}?cluster=devnet` },
});

// Dependencies are injectable so settlement is tested offline (fake q and Nessie).
export function createSettlement({ db = q, nessie = nessieClient, decimals = () => token.decimals } = {}) {
  const getSettlement = async txSig => { const r = (await db(SQL.list, [1, txSig])).rows[0]; return r ? toSettlement(r) : null; };
  const listSettlements = async (limit = 40) => (await db(SQL.list, [limit, null])).rows.map(toSettlement);

  // One deposit attempt for a row whose attempts count is `seen`. false = another caller holds it or it is exhausted.
  async function attempt(txSig, seen) {
    const row = (await db(SQL.claim, [txSig, seen])).rows[0];
    if (!row) return false;
    try {
      const { deposit_id } = await nessie.deposit(row.nessie_account_id, {
        amount: Number(BigInt(row.amount_raw) / 10n ** BigInt(decimals())), description: `settlement ${txSig.slice(0, 16)}` });
      await db(SQL.finish, [txSig, 'settled', null, deposit_id]);
      return true;
    } catch (err) {
      console.error(`[settlement] deposit for ${txSig.slice(0, 16)} failed (attempt ${row.attempts} of ${MAX_ATTEMPTS}):`, err.message);
      await db(SQL.finish, [txSig, 'failed', 'nessie_error', null]);
      return false;
    }
  }

  // Called once a payment to `payee` is confirmed on chain. Idempotent on txSig; Nessie errors never throw (the row is
  // 'failed' and the 30 s tick retries it), DB errors do.
  async function settleIfNeeded({ txSig, payee, amountRaw } = {}) {
    if (typeof txSig !== 'string' || !/^[1-9A-HJ-NP-Za-km-z]{32,90}$/.test(txSig)) fail('invalid_param', 'txSig must be a base58 signature');
    let raw;
    try { raw = BigInt(amountRaw); } catch { fail('invalid_param', 'amountRaw must be an integer number of HACK base units'); }
    const p = (await db(SQL.payee, [payee])).rows[0];
    if (!p || p.settle_mode !== 'bank') return { status: 'not_applicable' };

    // Nessie only records whole dollars, and 1 HACK = $1.
    const unit = 10n ** BigInt(decimals());
    const skip = raw <= 0n ? 'bad_amount' : raw % unit !== 0n ? 'fractional_amount' : !p.nessie_account_id ? 'no_nessie_account' : null;
    const inserted = (await db(SQL.insert, [txSig, payee, raw.toString(), p.nessie_account_id ?? null, skip ? 'skipped' : 'pending', skip])).rows.length;
    if (inserted && !skip) await attempt(txSig, 0);
    return getSettlement(txSig);
  }

  // The integrator's 30 s tick. Sequential: Nessie is slow and this is a handful of rows at most.
  async function retryPendingSettlements() {
    const rows = (await db(SQL.retryable)).rows;
    let settled = 0;
    for (const r of rows) if (await attempt(r.tx_sig, r.attempts)) settled++;
    return { retried: rows.length, settled };
  }

  return { settleIfNeeded, retryPendingSettlements, listSettlements, getSettlement };
}

export const { settleIfNeeded, retryPendingSettlements, listSettlements, getSettlement } = createSettlement();
