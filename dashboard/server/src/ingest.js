import { address } from '@solana/kit';
import { rpc, rpcSubs, getTxWithRetry, parseTransfer, errMsg } from './solana.js';
import { q, Q, dbState } from './db.js';

export const ingestState = { state: 'idle', lastSignature: null, lastEventAt: null };
const sleep = ms => new Promise(r => setTimeout(r, ms));

// devnet logsSubscribe (fast path) + a 15 s getSignaturesForAddress poll (safety net) -> INSERT.
// The INSERT trigger NOTIFYs, and the server's LISTEN client turns that into the SSE event.
export function startIngest({ mint, onStateChange }) {
  const mintAddr = address(mint);
  const seen = new Set();   // ponytail: unbounded; ~100 B per tx, fine for a 24 h event
  // Signatures whose fetch or insert failed. The poll's `until` cursor moves past them as soon as a newer payment
  // lands, so backfill re-tries these itself. ponytail: in memory only (a restart before the retry drops them)
  // and never given up on (a signature that can never be fetched is re-tried every poll).
  const retry = new Set();
  const setState = s => { if (ingestState.state !== s) { ingestState.state = s; onStateChange?.(); } };

  async function handle(sig, live) {
    if (seen.has(sig)) return;
    seen.add(sig); retry.delete(sig);
    const again = () => { seen.delete(sig); retry.add(sig); };     // the safety poll retries it
    try {
      const tx = await getTxWithRetry(sig, live);
      if (!tx) return again();
      const t = parseTransfer(tx, mint);
      if (!t) return;                                              // mintTo, create-ATA, failed tx: not a payment
      if (t.blockTime == null) return again();
      const attackId = (await q(Q.matchAttack, [sig, t.payer, t.payee, t.amountRaw])).rows[0]?.id ?? null;
      const lag = live ? Math.max(0, Date.now() - t.blockTime * 1000) : null;
      await q(Q.insertPayment, [new Date(t.blockTime * 1000), sig, t.slot, t.payer, t.payee, t.payerTokenAccount,
                                t.payeeTokenAccount, mint, t.amountRaw, t.decimals, attackId, lag]);
      ingestState.lastSignature = sig;
      ingestState.lastEventAt = new Date().toISOString();
    } catch (err) {
      again();
      console.error('[ingest]', sig, errMsg(err));
    }
  }

  let running = false;
  async function backfill() {
    if (running || !dbState.schema) return;
    running = true;
    try {
      const until = (await q(Q.latestChainSig)).rows[0]?.signature;
      // ponytail: one page of 200; a longer outage drops older rows
      const sigs = await rpc.getSignaturesForAddress(mintAddr, { ...(until && { until }), limit: 200, commitment: 'confirmed' }).send();
      for (const sig of new Set([...retry, ...[...sigs].reverse().filter(s => !s.err).map(s => s.signature)])) {   // oldest first
        if (seen.has(sig)) continue;
        await handle(sig, false);   // paced inside getTxWithRetry, and it yields to live signatures
      }
    } catch (err) {
      console.error('[ingest] backfill:', errMsg(err));
    } finally { running = false; }
  }

  (async function subscribeLoop() {
    setState('backfilling');
    for (let attempt = 0; ; attempt++) {
      const ac = new AbortController();
      try {
        const sub = await rpcSubs.logsNotifications({ mentions: [mintAddr] }, { commitment: 'confirmed' }).subscribe({ abortSignal: ac.signal });
        attempt = 0;
        setState('live');
        backfill();
        for await (const n of sub) if (!n.value.err) handle(n.value.signature, true);
      } catch (err) {
        console.error('[ingest] subscription:', errMsg(err));
      }
      ac.abort();
      setState('reconnecting');
      await sleep(Math.min(30_000, 1000 * 2 ** attempt));
    }
  })();

  setInterval(backfill, 15_000);
}
