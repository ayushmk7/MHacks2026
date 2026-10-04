import { address, isAddress } from '@solana/kit';
import { getTransferCheckedInstruction, getCreateAssociatedTokenIdempotentInstructionAsync } from '@solana-program/token';
import { q } from './db.js';
import { token } from './config.js';
import { sendIxs, ataOf, errMsg } from './solana.js';
import { nessie as nessieClient } from './nessie.js';
import { SQL as BANK_SQL } from './bank.js';

// Capital One funding (PROJECT-OVERVIEW §4.3): a badge holder tops up HACK from their Capital One account.
// A Nessie withdrawal is recorded first; HACK goes from the authority's (treasury) token account to the badge only
// after it succeeds. 1 HACK = $1, and Nessie only records whole dollars, so amounts are whole HACK.

export const MAX_TOPUP = 1000;
const EXPLORER = 'https://explorer.solana.com';

export const SQL = {
  enrollment: BANK_SQL.enrollment,
  insert: `INSERT INTO topups (badge_pubkey, amount_raw, nessie_account_id, status) VALUES ($1, $2, $3, 'pending') RETURNING id`,
  finish: `UPDATE topups SET status = $2, reason = $3, nessie_withdrawal_id = $4, hack_sig = $5 WHERE id = $1`,
  // $1 limit, $2 id or NULL.
  list: `
SELECT t.*, b.label FROM topups t LEFT JOIN badges b ON b.pubkey = t.badge_pubkey
WHERE ($2::uuid IS NULL OR t.id = $2)
ORDER BY t.created_at DESC LIMIT $1`,
};

const fail = (code, message) => { throw Object.assign(new Error(message), { code }); };
const iso = d => (d ? d.toISOString() : null);

export const toTopup = r => ({
  id: r.id, time: iso(r.created_at), pubkey: r.badge_pubkey, label: r.label ?? null,
  amount: Number(r.amount_raw) / 10 ** token.decimals, status: r.status, reason: r.reason,
  nessieWithdrawalId: r.nessie_withdrawal_id, hackSig: r.hack_sig,
  links: { tx: r.hack_sig ? `${EXPLORER}/tx/${r.hack_sig}?cluster=devnet` : null },
});

// Dependencies are injectable so the flow is tested offline (fake q, Nessie and sender; mint facts as a getter).
export function createTopup({ db = q, nessie = nessieClient, send = sendIxs, mint = () => token } = {}) {
  const getTopup = async id => { const r = (await db(SQL.list, [1, id])).rows[0]; return r ? toTopup(r) : null; };
  const listTopups = async (limit = 40) => (await db(SQL.list, [limit, null])).rows.map(toTopup);

  // Throws (nothing recorded) for bad input, an unenrolled badge or a missing mint/authority. Once the pending row
  // exists it never throws for Nessie or chain errors: the returned row is 'failed' with reason nessie_error / chain_error.
  async function topup({ pubkey, amount } = {}, { authority } = {}) {
    if (typeof pubkey !== 'string' || !isAddress(pubkey)) fail('invalid_pubkey', 'pubkey must be a base58 32-byte address');
    if (!Number.isSafeInteger(amount) || amount < 1 || amount > MAX_TOPUP) fail('invalid_amount', `amount must be a whole number of HACK from 1 to ${MAX_TOPUP}`);
    const t = mint();
    if (!t?.mint || !t.found) fail('not_configured', 'HACK mint is not configured or not found on chain');
    if (!authority?.address) fail('not_configured', 'the treasury authority is not loaded');
    const e = (await db(SQL.enrollment, [pubkey])).rows[0] ?? fail('not_found', 'badge is not enrolled with a Capital One account');

    const raw = BigInt(amount) * 10n ** BigInt(t.decimals);
    const id = (await db(SQL.insert, [pubkey, raw.toString(), e.nessie_account_id])).rows[0].id;
    let withdrawalId = null;
    try {
      ({ withdrawal_id: withdrawalId } = await nessie.withdrawal(e.nessie_account_id, { amount, description: `top up to badge ${pubkey.slice(0, 8)}` }));
    } catch (err) {
      console.error(`[topup] ${id}: Nessie withdrawal failed, no HACK sent:`, err.message);
      await db(SQL.finish, [id, 'failed', 'nessie_error', null, null]);
      return getTopup(id);
    }
    await db(SQL.finish, [id, 'pending', null, withdrawalId, null]);   // the withdrawal id survives a crash before the transfer

    try {
      const [source, destination] = await Promise.all([ataOf(authority.address, t.mint), ataOf(pubkey, t.mint)]);
      const ixs = [
        await getCreateAssociatedTokenIdempotentInstructionAsync({ payer: authority, owner: address(pubkey), mint: address(t.mint) }),
        getTransferCheckedInstruction({ source, mint: address(t.mint), destination, authority, amount: raw, decimals: t.decimals }),
      ];
      const sig = await send(authority, ixs);
      await db(SQL.finish, [id, 'done', null, withdrawalId, sig]);
    } catch (err) {
      // ponytail: the Nessie withdrawal stays recorded; the ledger only counts 'done' top-ups, so the balance is right.
      console.error(`[topup] ${id}: HACK transfer failed after withdrawal ${withdrawalId}:`, errMsg(err));
      await db(SQL.finish, [id, 'failed', 'chain_error', withdrawalId, null]);
    }
    return getTopup(id);
  }

  return { topup, listTopups, getTopup };
}

export const { topup, listTopups, getTopup } = createTopup();
