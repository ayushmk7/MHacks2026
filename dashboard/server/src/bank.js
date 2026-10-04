import { createHash } from 'node:crypto';
import { address, isAddress } from '@solana/kit';
import { q } from './db.js';
import { sendIxs, errMsg } from './solana.js';
import { nessie as nessieClient } from './nessie.js';
import { PREFIX, RAIL, parseReq, parseBankPayload, verifyEd25519, withPrefix, proofMessage, fromB58 } from './frames.js';

// Bank rail: the backend plays Capital One's authorization layer in front of Nessie (P2-U §3.1).
// The only Nessie money movement in the server is behind authorize()'s full check chain.

export const APPROVAL_TIMEOUT_S = 45;
const SKEW_S = 30;
export const MEMO_PROGRAM = address('MemoSq4gqABAXKb96qnH8TysNcWxMyWCqXgDLGmfcHr');
const EXPLORER = 'https://explorer.solana.com';
const ADDRESS = { street_number: '500', street_name: 'S State St', city: 'Ann Arbor', state: 'MI', zip: '48109' };
const OPENING_BALANCE = 500;   // dollars, same as the seeded judges

export const SQL = {
  enrollment: `SELECT badge_pubkey, nessie_customer_id, nessie_account_id FROM enrollments WHERE badge_pubkey = $1`,

  upsertEnrollment: `
INSERT INTO enrollments (badge_pubkey, nessie_customer_id, nessie_account_id) VALUES ($1, $2, $3)
ON CONFLICT (badge_pubkey) DO UPDATE SET nessie_customer_id = EXCLUDED.nessie_customer_id,
  nessie_account_id = EXCLUDED.nessie_account_id, enrolled_at = now()`,

  payee: `SELECT pubkey, kind, nessie_ref, salt FROM payees WHERE pubkey = $1`,

  // DO NOTHING without a target covers both UNIQUE constraints. q() hides the pg error code, so a
  // unique violation is detected as "no row returned" rather than by catching 23505.
  insertNonce: `INSERT INTO nonces (payer, req_id, nonce) VALUES ($1, $2, $3) ON CONFLICT DO NOTHING RETURNING payer`,

  insertApproval: `
INSERT INTO approvals (rail, source, payer, payee, payee_name, amount_cents, status, reason, req_id, payload_hash)
VALUES ($1, $2, $3, $4, $5, $6, $7, $8, $9, $10) RETURNING id`,

  finishApproval: `UPDATE approvals SET status = $2, reason = $3, nessie_ids = $4::jsonb WHERE id = $1`,

  setMemo: `UPDATE approvals SET memo_sig = $2 WHERE id = $1`,

  // Nessie never moves a balance, so the ledger is ours, in cents. $1 badge pubkey, $2 its Nessie account.
  // Debits: approved payments as payer + completed top-ups (withdrawals) from the account.
  // Credits: approved person deposits as payee + settled Capital One settlement deposits into the account.
  // topups/settlements store HACK base units; HACK has 2 decimals, so amount_raw is cents.
  ledger: `
SELECT (a.debits + t.debits)::text AS debits, (a.credits + s.credits)::text AS credits
FROM (SELECT COALESCE(sum(amount_cents) FILTER (WHERE payer = $1), 0) AS debits,
             COALESCE(sum(amount_cents) FILTER (WHERE payee = $1 AND nessie_ids ? 'deposit_id'), 0) AS credits
      FROM approvals WHERE rail = 'nessie' AND source = 'backend' AND status = 'approved' AND (payer = $1 OR payee = $1)) a,
     (SELECT COALESCE(sum(amount_raw), 0) AS debits FROM topups WHERE nessie_account_id = $2 AND status = 'done') t,
     (SELECT COALESCE(sum(amount_raw), 0) AS credits FROM settlements WHERE nessie_account_id = $2 AND status = 'settled') s`,

  // $1 limit, $2 id or NULL.
  approvals: `
SELECT a.*, pb.label AS payer_label, yb.label AS payee_label
FROM approvals a LEFT JOIN badges pb ON pb.pubkey = a.payer LEFT JOIN badges yb ON yb.pubkey = a.payee
WHERE ($2::uuid IS NULL OR a.id = $2)
ORDER BY a.created_at DESC LIMIT $1`,
};

const fail = (code, message) => { throw Object.assign(new Error(message), { code }); };
// A refusal: the reason goes to the badge and onto the feed; detail is logged only.
const block = (reason, detail = reason) => { throw Object.assign(new Error(detail), { reason }); };
const sha256 = (...parts) => createHash('sha256').update(Buffer.concat(parts.map(p => Buffer.from(p)))).digest();
const iso = d => (d ? d.toISOString() : null);
const unix = v => (v instanceof Date ? Math.floor(v.getTime() / 1000) : Number(v));

// Strict base64 (re-encodes to the same text) of an exact length, else null.
function b64(v, len) {
  if (typeof v !== 'string' || !/^[A-Za-z0-9+/]*={0,2}$/.test(v)) return null;
  const buf = Buffer.from(v, 'base64');
  if (buf.toString('base64') !== v || (len != null && buf.length !== len)) return null;
  return buf;
}
// The payload is text: accept it raw or base64 ("binaries in base64", 00 §8). Raw text starts with "v=1\n", never valid base64.
const payloadBytes = v => (typeof v === 'string' && v.startsWith('v=1\n') ? Buffer.from(v, 'utf8') : b64(v));
const clip = (v, n) => (typeof v === 'string' ? v.slice(0, n) : null);

export const toApproval = r => ({
  id: r.id, time: iso(r.created_at), rail: r.rail, source: r.source,
  payer: { pubkey: r.payer, label: r.payer_label ?? null },
  payee: { pubkey: r.payee, name: r.payee_name, label: r.payee_label ?? null },
  amountCents: r.amount_cents == null ? null : Number(r.amount_cents), status: r.status, reason: r.reason, reqId: r.req_id,
  nessieIds: r.nessie_ids ?? null, memoSig: r.memo_sig, solanaSig: r.solana_sig,
  links: {
    memo: r.memo_sig ? `${EXPLORER}/tx/${r.memo_sig}?cluster=devnet` : null,
    solana: r.solana_sig ? `${EXPLORER}/tx/${r.solana_sig}?cluster=devnet` : null,
  },
});

// Dependencies are injectable so the check chain is tested offline (fake q, Nessie and sender).
export function createBank({ db = q, nessie = nessieClient, send = sendIxs } = {}) {
  const enrollment = async pubkey => (await db(SQL.enrollment, [pubkey])).rows[0] ?? null;

  // Admin only. With no ids: create a customer + checking account in Nessie. With an account id: confirm it exists (read-only).
  async function enroll({ pubkey, nessieCustomerId, nessieAccountId, name } = {}) {
    if (typeof pubkey !== 'string' || !isAddress(pubkey)) fail('invalid_pubkey', 'pubkey must be a base58 32-byte address');
    for (const v of [nessieCustomerId, nessieAccountId]) {
      if (v != null && (typeof v !== 'string' || !/^[A-Za-z0-9-]{1,64}$/.test(v))) fail('invalid_param', 'Nessie ids must be 1 to 64 letters, digits or dashes');
    }
    let customer_id = nessieCustomerId ?? null, account_id = nessieAccountId ?? null;
    if (account_id) {
      const acct = await nessie.getAccount(account_id).catch(err => (err.status === 404 ? fail('not_found', `Nessie account ${account_id} not found`) : Promise.reject(err)));
      customer_id ??= acct?.customer_id ?? fail('invalid_param', 'Nessie did not say which customer owns that account, pass nessie_customer_id too');
    } else {
      const label = typeof name === 'string' && /^[\x20-\x7e]{1,32}$/.test(name) ? name : pubkey.slice(0, 8);
      customer_id ??= await nessie.createCustomer({ first_name: 'Badge', last_name: label, address: ADDRESS });
      account_id = await nessie.createAccount(customer_id, { nickname: `${label} checking`, balance: OPENING_BALANCE });
    }
    await db(SQL.upsertEnrollment, [pubkey, customer_id, account_id]);
    return { customer_id, account_id };
  }

  // Displayed balance = Nessie's static opening balance (dollars) - debits + credits (cents), see SQL.ledger.
  async function balance(pubkey) {
    const e = await enrollment(pubkey) ?? fail('not_found', 'badge is not enrolled');
    const [acct, { rows: [l] }] = await Promise.all([nessie.getAccount(e.nessie_account_id), db(SQL.ledger, [pubkey, e.nessie_account_id])]);
    const opening = Math.round(Number(acct?.balance ?? 0) * 100);
    return { usd_cents: opening - Number(l.debits) + Number(l.credits), account_id: e.nessie_account_id };
  }

  // Audit memo (PU2): fire and forget, the payment never waits on devnet. The feed picks memo_sig up via NOTIFY.
  function queueMemo(id, payloadHash, authority) {
    if (!authority) return null;
    const ix = { programAddress: MEMO_PROGRAM, accounts: [], data: new TextEncoder().encode(`appr:v1:nessie:${payloadHash}`) };
    return send(authority, [ix])
      .then(sig => db(SQL.setMemo, [id, sig]))
      .catch(err => console.error(`[bank] memo for approval ${id} failed:`, errMsg(err)));
  }

  // P2-U §3.1, in order. Every refusal returns { ok:false, reason } and leaves a row on the feed.
  async function authorize(body, { recordFields, authority = null, now = () => Math.floor(Date.now() / 1000) } = {}) {
    const ctx = { payer: clip(body?.payer_pubkey, 64) };
    try {
      return await chain(body ?? {}, ctx, recordFields, authority, now());
    } catch (err) {
      const reason = err.reason ?? 'internal_error';
      if (err.reason) console.warn(`[bank] blocked ${ctx.req_id ?? '-'}: ${err.message}`);
      else console.error('[bank] authorize failed:', err.message);
      // A pending row (Nessie or the DB failed mid-payment) becomes 'failed'. Otherwise record the refusal.
      const write = ctx.approvalId
        ? db(SQL.finishApproval, [ctx.approvalId, 'failed', reason, ctx.nessieIds ? JSON.stringify(ctx.nessieIds) : null])
        : db(SQL.insertApproval, ['nessie', 'backend', ctx.payer, ctx.payee ?? null, ctx.payee_name ?? null, ctx.amount ?? null,
                                  'blocked', reason, ctx.req_id ?? null, ctx.payload_hash ?? null]);
      await write.catch(e => console.error('[bank] could not record the refusal:', e.message));
      return { ok: false, reason };
    }
  }

  async function chain(body, ctx, recordFields, authority, now) {
    if (typeof recordFields !== 'function') throw new Error('authorize needs a recordFields dependency');

    // 1. Enrolled payer.
    if (typeof body.payer_pubkey !== 'string' || !isAddress(body.payer_pubkey)) block('bad_request', 'payer_pubkey is not a base58 address');
    const payer32 = fromB58(body.payer_pubkey);
    const enr = await enrollment(body.payer_pubkey) ?? block('not_enrolled', `${body.payer_pubkey} is not enrolled`);

    // 2. Payer signature over bank-auth: + payload.
    const payload = payloadBytes(body.payload) ?? block('bad_request', 'payload is missing or not base64');
    ctx.payload_hash = sha256(payload).toString('hex');
    const sig = b64(body.sig, 64) ?? block('bad_request', 'sig is not 64 bytes of base64');
    if (!verifyEd25519(withPrefix(PREFIX.BANK_AUTH, payload), sig, payer32)) block('bad_sig', 'payer signature does not verify');

    // 3. Payload format and freshness.
    let p;
    try { p = parseBankPayload(payload.toString('utf8')); } catch (err) { block('bad_payload', err.message); }
    Object.assign(ctx, { amount: p.amount_cents, payee_name: p.payee_name, req_id: p.req_id });
    // ponytail: routed bank payments (PU11, P2) are not built. Refuse rather than ignore the fee binding.
    if (p.route_id !== 'none' || p.route_fee_total !== 0 || body.route != null) block('route_unsupported', 'routed bank payments are not supported');
    if (Math.abs(now - p.issued_at) > APPROVAL_TIMEOUT_S + SKEW_S) block('stale', `issued_at ${p.issued_at} is ${now - p.issued_at} s from now`);

    // 4. The payee's signed REQ, bound to this payload.
    const reqBuf = b64(body.req) ?? block('bad_req', 'req is not base64');
    let req;
    try { req = parseReq(reqBuf); } catch (err) { block('bad_req', err.message); }
    ctx.payee = req.payee;
    if (!verifyEd25519(withPrefix(PREFIX.PAY_REQ, req.signedBytes), req.sig, req.payee_pubkey)) block('bad_req_sig', 'REQ signature does not verify');
    if (req.expiry < now) block('req_expired', `REQ expired at ${req.expiry}`);
    if (req.rail !== RAIL.BANK || req.currency !== 'USD') block('wrong_rail', `REQ rail ${req.rail} currency ${req.currency}`);
    if (p.req_id !== req.req_id) block('req_mismatch', `payload req_id ${p.req_id} != REQ ${req.req_id}`);
    if (BigInt(p.amount_cents) !== req.amount) block('amount_mismatch', `payload amount ${p.amount_cents} != REQ ${req.amount}`);

    // 5. The payee's issuer record.
    const rec = await recordFields(req.payee) ?? block('unknown_payee', `no record for ${req.payee}`);
    if (!rec.device_pubkey || !Buffer.from(rec.device_pubkey).equals(req.payee_pubkey)) block('device_mismatch', 'REQ signer is not the attested device key');
    if (rec.status !== 'active') block('revoked', `payee status ${rec.status}`);
    if (!(unix(rec.expiry) > now)) block('expired', `payee record expired at ${unix(rec.expiry)}`);
    if (rec.kind === 'relay') block('relay_payee', 'a relay cannot receive a bank payment');
    if (!rec.bank_ref_hash || Buffer.from(rec.bank_ref_hash).toString('hex') !== p.payee_ref) block('payee_ref_mismatch', 'payee_ref does not match the record');
    if (p.attestation !== rec.attestation) block('attestation_mismatch', 'attestation does not match the record');
    if (p.payee_name !== rec.display_name) block('payee_name_mismatch', 'payee_name does not match the record');
    const want = { merchant: 'purchase', person: 'transfer' }[rec.kind];
    if (p.action !== want) block('action_mismatch', `action ${p.action} for a ${rec.kind}`);
    ctx.payee_name = rec.display_name;

    // 6. Presence: the attested device signed the payer's nonce.
    const proofSig = b64(body.proof_sig, 64) ?? block('bad_proof', 'proof_sig is not 64 bytes of base64');
    if (!verifyEd25519(proofMessage(p.req_id, p.proof_nonce, payer32), proofSig, rec.device_pubkey)) block('bad_proof', 'presence proof does not verify');

    // 7. Account, amount, payee target, replay, whole dollars.
    if (p.from_acct !== enr.nessie_account_id) block('acct_mismatch', 'from_acct is not the enrolled account');
    if (p.amount_cents <= 0) block('bad_amount', 'amount must be positive');
    // The plaintext target comes from our DB and must hash to the public bank_ref_hash the payer approved.
    const payee = (await db(SQL.payee, [req.payee])).rows[0];
    if (!payee?.nessie_ref || !payee.salt || payee.kind !== rec.kind) block('payee_not_configured', `payee ${req.payee} has no Nessie target`);
    if (!sha256(payee.salt, Buffer.from(payee.nessie_ref, 'utf8')).equals(Buffer.from(rec.bank_ref_hash))) block('payee_ref_mismatch', 'stored Nessie target does not hash to bank_ref_hash');
    if (!(await db(SQL.insertNonce, [body.payer_pubkey, p.req_id, p.proof_nonce])).rows.length) block('replay', `req_id ${p.req_id} or nonce already used`);
    if (p.amount_cents % 100 !== 0) block('amount_not_whole_dollars', 'Nessie only records whole dollars');

    // 8. Pending row, then exactly the payload's amount to the DB's target.
    ctx.approvalId = (await db(SQL.insertApproval, ['nessie', 'backend', body.payer_pubkey, req.payee, rec.display_name, p.amount_cents,
                                                   'pending', null, p.req_id, ctx.payload_hash])).rows[0].id;
    const amount = p.amount_cents / 100, description = `badge payment ${p.req_id} to ${rec.display_name}`;
    let ids;
    try {
      ids = rec.kind === 'merchant'
        ? await nessie.purchase(enr.nessie_account_id, { merchantId: payee.nessie_ref, amount, description })
        : await nessie.transferWithDeposit(enr.nessie_account_id, payee.nessie_ref, { amount, description });
    } catch (err) {
      if (err.transfer_id) ctx.nessieIds = { transfer_id: err.transfer_id };
      block('nessie_error', err.message);
    }

    // 9. Approved, memo queued.
    await db(SQL.finishApproval, [ctx.approvalId, 'approved', null, JSON.stringify(ids)]);
    queueMemo(ctx.approvalId, ctx.payload_hash, authority);
    return { ok: true, nessie_id: ids.purchase_id ?? ids.transfer_id };
  }

  const listApprovals = async (limit = 40) => (await db(SQL.approvals, [limit, null])).rows.map(toApproval);
  const getApproval = async id => { const r = (await db(SQL.approvals, [1, id])).rows[0]; return r ? toApproval(r) : null; };

  // POST /feed/event: unauthenticated, so everything is clipped and the REQ is parsed but not trusted.
  // Accepts both body shapes: 00 §8 { payer_pubkey, req?, reason } and badge-os { payer, payee, reason, req }.
  async function recordBadgeEvent({ payer_pubkey, payer, payee, req, reason } = {}) {
    let r = null;
    try { r = req ? parseReq(b64(req) ?? []) : null; } catch {}
    const who = [payer_pubkey, payer].find(v => typeof v === 'string' && isAddress(v)) ?? null;
    const to = r?.payee ?? (typeof payee === 'string' && isAddress(payee) ? payee : null);
    const why = typeof reason === 'string' && /^[a-z0-9_ -]{1,48}$/i.test(reason) ? reason : 'unspecified';
    const rail = r ? (r.rail === RAIL.BANK ? 'nessie' : 'solana') : 'unknown';
    const { rows: [{ id }] } = await db(SQL.insertApproval, [rail, 'badge_report', who, to, r?.name || null,
      r && r.amount <= BigInt(Number.MAX_SAFE_INTEGER) ? Number(r.amount) : null, 'blocked', why, r?.req_id ?? null, null]);
    return { ok: true, id };
  }

  return { enroll, balance, authorize, listApprovals, getApproval, recordBadgeEvent };
}

export const { enroll, balance, authorize, listApprovals, getApproval, recordBadgeEvent } = createBank();
