import { token } from './config.js';
import { q } from './db.js';
import { getTxWithRetry, parseTransfer, ataOf } from './solana.js';
import { parseReq, verifyEd25519, withPrefix, PREFIX, RAIL, b58 } from './frames.js';

// POST /feed/solana (00 §8.1). The badge only *claims* a payment, so nothing is written until the chain shows a
// transferChecked of exactly req.amount HACK into the payee's attested token account. The ingest already records
// the raw transfer; this row ties it to its REQ so the feed can show it as a verified, present payment.
const SQL = {
  existing: `SELECT id FROM approvals WHERE solana_sig = $1 LIMIT 1`,
  insert: `
INSERT INTO approvals (rail, source, payer, payee, payee_name, amount_cents, status, req_id, solana_sig)
VALUES ('solana', 'backend', $1, $2, $3, $4, 'approved', $5, $6) RETURNING id`,
};

const refuse = reason => ({ ok: false, reason });

export function createFeed({ db = q, getTx = getTxWithRetry, ata = ataOf, parse = parseTransfer } = {}) {
  return async function confirmSolana(body, { recordFields }) {
    if (typeof body?.tx_sig !== 'string' || typeof body?.req !== 'string') return refuse('bad_request');
    if (!token.found) return refuse('mint_not_configured');
    let req;
    try { req = parseReq(Buffer.from(body.req, 'base64')); } catch { return refuse('bad_req'); }
    if (req.rail !== RAIL.SOLANA || req.currency !== 'HACK') return refuse('wrong_rail');
    if (!verifyEd25519(withPrefix(PREFIX.PAY_REQ, req.signedBytes), req.sig, req.payee_pubkey)) return refuse('bad_req_sig');

    if ((await db(SQL.existing, [body.tx_sig])).rows[0]) return { ok: true };   // the badge may retry
    const tx = await getTx(body.tx_sig);
    if (!tx) return refuse('tx_not_found');
    const t = parse(tx, token.mint);
    if (!t) return refuse('no_hack_transfer');

    const rec = await recordFields(req.payee);
    const expectedAta = rec?.solana_ata ? b58(rec.solana_ata) : String(await ata(req.payee, token.mint));
    if (t.payeeTokenAccount !== expectedAta) return refuse('wrong_recipient');
    if (BigInt(t.amountRaw) !== req.amount || t.decimals !== token.decimals) return refuse('amount_mismatch');

    await db(SQL.insert, [t.payer, req.payee, rec?.display_name ?? null, String(req.amount), req.req_id, body.tx_sig]);
    return { ok: true };
  };
}

export const confirmSolana = createFeed();
