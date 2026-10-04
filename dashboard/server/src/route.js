import { address, isAddress } from '@solana/kit';
import { getTransferCheckedInstruction, getCreateAssociatedTokenIdempotentInstructionAsync } from '@solana-program/token';
import { token } from './config.js';
import { q } from './db.js';
import { rpc as solanaRpc, sendIxs, ataOf, errMsg } from './solana.js';
import { getSignedRecord } from './records.js';
import { createConfirm, notifyConfirmed, refuse } from './feed.js';

// Tier 0 routing (P3 §6, §8): a routed payment is an ordinary single-transferChecked payment whose radio frames
// hopped through relays. The gateway reports it on POST /feed/route; the hop list is unsigned, so it only decides
// who gets a reward, and only relays with an active issuer record are paid. Rewards come from the treasury
// (the authority's HACK account), never from the payer.
const EXPLORER = 'https://explorer.solana.com';
const MAX_HOPS = 3;
const rawEnv = (v, dflt) => (/^\d{1,15}$/.test(v ?? '') ? BigInt(v) : dflt);
export const ROUTE_REWARD_RAW = rawEnv(process.env.ROUTE_REWARD_RAW, 1n);         // 0.01 HACK per verified hop
export const ROUTE_REWARD_CAP_RAW = rawEnv(process.env.ROUTE_REWARD_CAP_RAW, 10n); // per route

export const SQL = {
  existing: `SELECT relay, reward_state FROM route_hops WHERE tx_sig = $1 ORDER BY position`,

  // One statement, so the routes NOTIFY is delivered with its hops already committed. ON CONFLICT makes a
  // concurrent duplicate report insert nothing: no rows back = someone else recorded it first.
  insert: `
WITH r AS (
  INSERT INTO routes (tx_sig, payer, payee, payee_name, amount_raw, req_id, hop_count, e2e_proof, gateway)
  VALUES ($1, $2, $3, $4, $5, $6, $7, $8, $9) ON CONFLICT (tx_sig) DO NOTHING RETURNING tx_sig)
INSERT INTO route_hops (tx_sig, position, relay, verified, reward_raw, reward_state)
SELECT r.tx_sig, h.position, h.relay, h.verified, h.reward_raw, h.reward_state
FROM r, unnest($10::int[], $11::text[], $12::boolean[], $13::bigint[], $14::text[]) AS h(position, relay, verified, reward_raw, reward_state)
RETURNING position`,

  rewarded: `UPDATE route_hops SET reward_state = $2, reward_sig = $3 WHERE tx_sig = $1 AND reward_state = 'pending'`,

  // $1 limit, $2 tx_sig or NULL.
  routes: `
SELECT r.*, pb.label AS payer_label, yb.label AS payee_label,
  COALESCE((SELECT json_agg(json_build_object('position', h.position, 'relay', h.relay, 'label', hb.label, 'verified', h.verified,
              'reward_raw', h.reward_raw::text, 'reward_state', h.reward_state, 'reward_sig', h.reward_sig) ORDER BY h.position)
            FROM route_hops h LEFT JOIN badges hb ON hb.pubkey = h.relay WHERE h.tx_sig = r.tx_sig), '[]'::json) AS hops
FROM routes r LEFT JOIN badges pb ON pb.pubkey = r.payer LEFT JOIN badges yb ON yb.pubkey = r.payee
WHERE ($2::text IS NULL OR r.tx_sig = $2)
ORDER BY r.created_at DESC LIMIT $1`,

  leaderboard: `
SELECT h.relay AS pubkey, b.label, count(DISTINCT h.tx_sig)::int AS routes,
       COALESCE(sum(h.reward_raw) FILTER (WHERE h.reward_state = 'paid'), 0)::text AS earned
FROM route_hops h LEFT JOIN badges b ON b.pubkey = h.relay
GROUP BY h.relay, b.label ORDER BY COALESCE(sum(h.reward_raw) FILTER (WHERE h.reward_state = 'paid'), 0) DESC, routes DESC`,
};

const ui = raw => Number(raw ?? 0) / 10 ** token.decimals;
const iso = d => (d ? new Date(d).toISOString() : null);

export const toRoute = r => ({
  txSig: r.tx_sig, time: iso(r.created_at),
  payer: { pubkey: r.payer, label: r.payer_label ?? null },
  payee: { pubkey: r.payee, name: r.payee_name, label: r.payee_label ?? null },
  amount: ui(r.amount_raw), amountRaw: String(r.amount_raw),
  hops: (r.hops ?? []).map(h => ({ position: h.position, pubkey: h.relay, label: h.label ?? null, verified: h.verified,
                                    reward: ui(h.reward_raw), rewardState: h.reward_state, rewardSig: h.reward_sig ?? null })),
  e2eProof: r.e2e_proof, gateway: r.gateway,
  links: { tx: `${EXPLORER}/tx/${r.tx_sig}?cluster=devnet` },
});

// 1..3 distinct base58 keys, or null.
function parseHops(hops) {
  if (!Array.isArray(hops) || hops.length < 1 || hops.length > MAX_HOPS) return null;
  if (!hops.every(h => typeof h === 'string' && isAddress(h)) || new Set(hops).size !== hops.length) return null;
  return hops;
}

// Same test the badge applies to a record: an active relay attestation that has not expired.
const isActiveRelay = (rec, now) => rec?.kind === 'relay' && rec.status === 'active' && Number(rec.expiry) > now;

// One transaction for every rewarded hop: create its ATA if missing (idempotent), then transferChecked.
async function rewardIxs(authority, hops) {
  const mint = address(token.mint), source = await ataOf(authority.address, token.mint);
  const ixs = [];
  for (const h of hops) {
    ixs.push(await getCreateAssociatedTokenIdempotentInstructionAsync({ payer: authority, owner: address(h.relay), mint }));
    ixs.push(getTransferCheckedInstruction({ source, mint, destination: await ataOf(h.relay, token.mint), authority,
                                             amount: h.reward_raw, decimals: token.decimals }));
  }
  return ixs;
}

// Dependencies are injectable so routing is tested offline (fake db, tx fetch, sender, rpc).
export function createRoutes({ db = q, send = sendIxs, rpc = solanaRpc, signedRecord = getSignedRecord,
                               rewardRaw = ROUTE_REWARD_RAW, rewardCap = ROUTE_REWARD_CAP_RAW, onConfirmed = () => {},
                               now = () => Math.floor(Date.now() / 1000), ...confirmDeps } = {}) {
  const confirmPayment = createConfirm(confirmDeps);

  // GET /route/ctx/:payee: everything the offline payer needs for CTX (P3 §6.2) in one gateway call.
  async function routeCtx(signer, payee) {
    const [rec, { value: bh }] = await Promise.all([
      signedRecord(signer, payee, 'full'),
      rpc.getLatestBlockhash({ commitment: 'confirmed' }).send(),
    ]);
    if (rec.status !== 200) return { status: 404 };
    return { status: 200, body: { ...rec.body, blockhash: bh.blockhash, lastValidBlockHeight: Number(bh.lastValidBlockHeight) } };
  }

  // Fire and forget: the gateway never waits on devnet. The row stays 'pending' until this settles.
  function payRewards(txSig, hops, authority) {
    const paid = hops.filter(h => h.reward_state === 'pending');
    if (!paid.length) return null;
    return Promise.resolve()
      .then(async () => {
        if (!authority) throw new Error('no treasury authority');
        return send(authority, await rewardIxs(authority, paid));
      })
      .then(
        sig => db(SQL.rewarded, [txSig, 'paid', sig]).catch(e => console.error(`[route] rewards for ${txSig} paid in ${sig}, row not updated:`, e.message)),
        err => {
          console.error(`[route] rewards for ${txSig} failed:`, errMsg(err));
          return db(SQL.rewarded, [txSig, 'failed', null]).catch(e => console.error('[route] could not mark rewards failed:', e.message));
        });
  }

  // POST /feed/route. Refusals are { ok:false, reason } (HTTP 200, P3 §8).
  async function confirmRoute(body, { recordFields, authority = null, onConfirmed: hook = onConfirmed }) {
    const hops = parseHops(body?.hops);
    if (!hops) return refuse('bad_hops');
    const prior = rows => rows.length && { ok: true, rewards: rows.map(h => ({ pubkey: h.relay, state: h.reward_state })) };
    const r = await confirmPayment(body, { recordFields, seen: async () => prior((await db(SQL.existing, [body.tx_sig])).rows) });
    if (!r.ok) return r;
    if (r.repeat) return r.repeat;
    const f = r.facts;
    if (hops.includes(f.payer) || hops.includes(f.payee)) return refuse('bad_hops');

    const t = now();
    const recs = await Promise.all(hops.map(h => Promise.resolve().then(() => recordFields(h)).catch(() => null)));
    let budget = rewardCap;
    const rows = hops.map((relay, i) => {
      const verified = isActiveRelay(recs[i], t);
      const reward = verified ? (rewardRaw < budget ? rewardRaw : budget) : 0n;
      budget -= reward;
      return { position: i + 1, relay, verified, reward_raw: reward, reward_state: reward > 0n ? 'pending' : 'none' };
    });
    // ponytail: the gateway is taken to be the last relay to append itself (P3 §6.3); the report is unsigned anyway.
    const gateway = hops.at(-1);
    const col = k => rows.map(h => (typeof h[k] === 'bigint' ? String(h[k]) : h[k]));
    const ins = await db(SQL.insert, [f.txSig, f.payer, f.payee, f.payeeName, f.amountRaw, f.reqId, hops.length, body.e2e_proof === true, gateway,
                                      col('position'), col('relay'), col('verified'), col('reward_raw'), col('reward_state')]);
    if (!ins.rows.length) return prior((await db(SQL.existing, [f.txSig])).rows) || refuse('internal_error');   // lost a race to a retry

    payRewards(f.txSig, rows, authority);
    notifyConfirmed(hook, { ...f, routed: true, hops });
    return { ok: true, rewards: rows.map(h => ({ pubkey: h.relay, state: h.reward_state })) };
  }

  const listRoutes = async (limit = 40) => (await db(SQL.routes, [limit, null])).rows.map(toRoute);
  const getRoute = async txSig => { const r = (await db(SQL.routes, [1, txSig])).rows[0]; return r ? toRoute(r) : null; };
  const relayLeaderboard = async () => (await db(SQL.leaderboard)).rows.map(r => ({ pubkey: r.pubkey, label: r.label ?? null, routes: r.routes, earned: ui(r.earned) }));

  return { routeCtx, confirmRoute, listRoutes, getRoute, relayLeaderboard };
}

export const { routeCtx, confirmRoute, listRoutes, getRoute, relayLeaderboard } = createRoutes();
