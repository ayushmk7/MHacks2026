import http from 'node:http';
import { networkInterfaces } from 'node:os';
import { isAddress } from '@solana/kit';
import { env, token } from './config.js';
import { Q, q, timed, queryMs, toPayment, toBadge, toAttestation, toAttack } from './db.js';
import { rpc, getBalances, buildTamperedTx, ensureAta, ataOf, errMsg } from './solana.js';
import { registryState, issue, revoke, recordFields } from './registry.js';
import { getSignedRecord } from './records.js';
import { enroll, balance, authorize, listApprovals, recordBadgeEvent } from './bank.js';
import { confirmSolana } from './feed.js';
import { routeCtx, confirmRoute, listRoutes, relayLeaderboard } from './route.js';
import { settleIfNeeded, listSettlements } from './settlement.js';
import { topup, listTopups } from './topup.js';

const STATUS = {
  bad_json: 400, invalid_pubkey: 400, invalid_name: 400, invalid_amount: 400, invalid_param: 400,
  unknown_badge: 404, not_found: 404, forbidden_origin: 403, too_large: 413, unsupported_media_type: 415,
  conflict: 409, chain_error: 502, nessie_error: 502, not_configured: 503, db_error: 503, internal: 500,
};
const MAX_BODY = 16 * 1024;
const UUID = /^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$/i;
const EXPLORER = 'https://explorer.solana.com';

// Errors are plain Errors tagged with a contract code; `extra` is merged into the error object (e.g. gap).
const fail = (code, message, extra) => { throw Object.assign(new Error(message), { code, extra }); };
// Anything a Solana call throws that is not already tagged becomes chain_error (message only, never a stack).
const chain = p => p.catch(err => (STATUS[err.code] ? Promise.reject(err) : fail('chain_error', errMsg(err))));

// Each validator returns the cleaned value or throws the matching 400.
export const validate = {
  pubkey: v => (typeof v === 'string' && isAddress(v) ? v : fail('invalid_pubkey', 'pubkey must be a base58 32-byte address')),
  name: v => {
    const s = typeof v === 'string' ? v.trim() : '';
    return s.length >= 1 && s.length <= 32 && /^[\x20-\x7E]+$/.test(s) ? s : fail('invalid_name', 'name must be 1 to 32 printable ASCII characters');
  },
  // UI amount -> raw integer units.
  amount: (v, decimals) => {
    const raw = typeof v === 'number' ? v * 10 ** decimals : NaN;
    return Number.isFinite(raw) && v <= 1_000_000 && Math.round(raw) >= 1 && Math.abs(raw - Math.round(raw)) < 1e-6 ? Math.round(raw)
      : fail('invalid_amount', `amount must be a number > 0 and <= 1000000 with at most ${decimals} decimal places`);
  },
  limit: (v, fallback) => {
    if (v == null) return fallback;
    const n = /^\d+$/.test(String(v)) ? Number(v) : NaN;
    return n >= 1 && n <= 200 ? n : fail('invalid_param', 'limit must be an integer from 1 to 200');
  },
};

// ── SSE hub ──
const clients = new Set();
export function broadcast(event) {
  const frame = `data: ${JSON.stringify(event)}\n\n`;
  for (const res of clients) res.write(frame);
}
function sse(req, res) {
  res.writeHead(200, { 'Content-Type': 'text/event-stream', 'Cache-Control': 'no-cache', 'X-Accel-Buffering': 'no', Connection: 'keep-alive' });
  res.write(': connected\n\n');
  clients.add(res);
  req.on('close', () => clients.delete(res));
}

function send(res, status, body) {
  if (res.headersSent) return;
  const headers = { 'Content-Type': 'application/json; charset=utf-8', 'Cache-Control': 'no-store' };
  if (status === 413) headers.Connection = 'close';
  res.writeHead(status, body === undefined ? {} : headers).end(body === undefined ? undefined : JSON.stringify(body));
}
function sendError(res, err) {
  const code = STATUS[err?.code] ? err.code : 'internal';
  if (code === 'internal') console.error('[http]', err);
  send(res, STATUS[code], { error: { code, message: code === 'internal' ? 'internal error' : err.message, ...err.extra } });
}

// strict = browser-facing API: Origin allowlist + JSON content type (forces a CORS preflight we never answer).
async function readJson(req, strict) {
  const origin = req.headers.origin;
  if (strict && origin && !env.WEB_ORIGINS.includes(origin)) fail('forbidden_origin', 'Origin not allowed');
  if (strict && !(req.headers['content-type'] ?? '').toLowerCase().startsWith('application/json')) fail('unsupported_media_type', 'Content-Type must be application/json');
  if (Number(req.headers['content-length']) > MAX_BODY) fail('too_large', 'body exceeds 16 KB');
  const chunks = [];
  let size = 0;
  for await (const c of req) {
    if ((size += c.length) > MAX_BODY) fail('too_large', 'body exceeds 16 KB');
    chunks.push(c);
  }
  let body;
  try { body = JSON.parse(Buffer.concat(chunks).toString('utf8')); } catch { fail('bad_json', 'body is not valid JSON'); }
  if (!body || typeof body !== 'object' || Array.isArray(body)) fail('bad_json', 'body must be a JSON object');
  return body;
}

// Runs once per newly confirmed payment (direct or routed), never awaited by the badge-facing response.
// Routed payments also get an approvals row so the feed lists them next to direct ones (direct rows come from feed.js).
const ROUTED_APPROVAL = `
INSERT INTO approvals (rail, source, payer, payee, payee_name, amount_cents, status, reason, req_id, solana_sig)
SELECT 'solana', 'backend', $1, $2, $3, $4, 'approved', 'routed', $5, $6
WHERE NOT EXISTS (SELECT 1 FROM approvals WHERE solana_sig = $6)`;
export async function onConfirmed(facts) {
  if (facts.routed) await q(ROUTED_APPROVAL, [facts.payer, facts.payee, facts.payeeName ?? null, String(facts.amountRaw), facts.reqId, facts.txSig])
    .catch(err => console.error('[route] approvals row failed:', err.message));
  await settleIfNeeded({ txSig: facts.txSig, payee: facts.payee, amountRaw: facts.amountRaw })
    .catch(err => console.error('[settlement]', err.message));
}

const attackById = async (id, warnings) => toAttack((await q(Q.attacks, [1, id])).rows[0], warnings);

// Only pending -> rejected. 'signed' is never accepted from a client: it is set from on-chain evidence by the ingest.
async function rejectAttack(body) {
  if (typeof body.id !== 'string' || !UUID.test(body.id)) fail('invalid_param', 'id must be a UUID');
  if (body.outcome !== 'rejected') fail('invalid_param', 'outcome must be "rejected"');
  if (!(await q(Q.rejectAttack, [body.id])).rowCount) {
    if (!(await q(Q.attacks, [1, body.id])).rowCount) fail('not_found', 'no such attempt');
    fail('conflict', 'attempt is not pending');
  }
  return body.id;
}

async function requireFundedAuthority(authority) {
  const sol = Number((await chain(rpc.getBalance(authority.address, { commitment: 'confirmed' }).send())).value) / 1e9;
  registryState.authoritySol = sol;
  if (sol < 0.02) fail('not_configured', `registry authority ${authority.address} has ${sol} SOL, fund it at https://faucet.solana.com`, { gap: 'authority_unfunded' });
}

// The transaction has already landed when this runs, so the response must not depend on the mirror: if the row
// is missing, stale or the database is down, answer from what the chain call returned (`known`, a mirror-shaped row).
const attestationResult = async (ctx, pubkey, signature, known) => {
  const row = (await q(Q.attestations, [pubkey]).catch(() => null))?.rows.find(r => r.issued_sig === signature || r.revoked_sig === signature);
  const badge = ctx.badges.find(b => b.pubkey === pubkey);
  return {
    attestation: toAttestation(row ?? { subject: pubkey, badge_id: badge?.id, label: badge?.label, ...known }),
    signature, explorerUrl: `${EXPLORER}/tx/${signature}?cluster=devnet`,
  };
};

// A handler returns the 200 body, or [status, body].
const routes = {
  'GET /api/status': ({ ctx }) => ctx.status(),

  'GET /api/payments': async ({ query }) => {
    const limit = validate.limit(query.get('limit'), 40);
    const before = query.get('before');
    if (before != null && Number.isNaN(Date.parse(before))) fail('invalid_param', 'before must be an ISO timestamp');
    // ponytail: `before` is a strict block_time cursor, so rows sharing the page's last second are skipped when paging
    const { rows } = await timed('feed', Q.payments, [limit, before == null ? null : new Date(before)]);
    return { payments: rows.map((r, i) => toPayment(r, rows[i + 1]?.signature ?? null)) };
  },

  'GET /api/stats': async () => {
    const [{ rows: [s] }, series, { rows: [lag] }] = await Promise.all([q(Q.stats24h), timed('series', Q.series60m), q(Q.lag)]);
    const ui = raw => Number(raw) / 10 ** token.decimals;
    const ms = v => (v == null ? null : Math.round(v));
    return {
      window: '24h', payments: s.payments, volume: ui(s.volume_raw),
      verifiedShare: s.payments ? Math.round((s.verified / s.payments) * 1000) / 1000 : 0, seedRows: s.seed_rows,
      lagMs: { p50: ms(lag.p50), p95: ms(lag.p95) },
      series: series.rows.map(r => ({ t: r.t.toISOString(), n: r.n, verifiedN: r.verified_n, volume: ui(r.volume_raw) })),
      queryMs: queryMs.series,
    };
  },

  'GET /api/stats/db': async () => {
    const [{ rows: [v] }, { rows: [h] }, caggs] = await Promise.all([q(Q.dbVersion), q(Q.dbHypertable), q(Q.dbCaggs)]);
    // Timed one at a time, after the others, so queryMs is each query's own cost and not pool contention.
    await timed('feed', Q.payments, [40, null]);
    await timed('series', Q.series60m);
    const before = Number(h.before_compression_total_bytes ?? 0), after = Number(h.after_compression_total_bytes ?? 0);
    return {
      timescaledb: v?.extversion ?? null,
      payments: {
        rows: Number(h.rows), chunks: Number(h.total_chunks ?? 0), columnstoreChunks: Number(h.number_compressed_chunks ?? 0),
        bytesBefore: before, bytesAfter: after,
        compressionRatio: before ? Math.round((1 - after / before) * 1000) / 1000 : null, totalBytes: Number(h.total_bytes ?? 0),
      },
      caggs: caggs.rows.map(c => ({
        name: c.view_name, realtime: !c.materialized_only, scheduleSec: c.schedule_sec,
        // pg hands back -Infinity (not a Date) for a job that has never finished
        lastRefresh: c.last_successful_finish instanceof Date ? c.last_successful_finish.toISOString() : null, lastStatus: c.last_run_status,
      })),
      queryMs: { ...queryMs },
    };
  },

  'GET /api/badges': async ({ ctx }) => {
    const [{ rows }, balances] = await Promise.all([q(Q.badges), getBalances(ctx.badges)]);
    // Bank balance per badge: null = not enrolled; omitted when Nessie can't be reached (the UI shows "unknown").
    const bank = await Promise.all(rows.map(r => balance(r.pubkey).then(
      b => ({ accountId: b.account_id, usdCents: b.usd_cents }), err => (err.code === 'not_found' ? null : undefined))));
    return { badges: rows.map((r, i) => ({ ...toBadge(r, balances.get(r.pubkey)), ...(bank[i] === undefined ? {} : { nessie: bank[i] }) })) };
  },

  'GET /api/attestations': async () => ({ attestations: (await q(Q.attestations, [null])).rows.map(toAttestation) }),

  'POST /api/attestations': async ({ body, ctx }) => {
    const pubkey = validate.pubkey(body.pubkey), name = validate.name(body.name);
    await requireFundedAuthority(ctx.authority);
    // issue() validates kind, wallet and the relay-per-operator rule itself (invalid_param / conflict).
    const { kind, solanaWallet, nessieRef, operatorId, settleMode, nessieAccountId } = body;
    const { signature, pda, expiresAt } = await chain(issue(pubkey, { name, kind, solanaWallet: solanaWallet || pubkey,
      nessieRef: nessieRef || null, operatorId: operatorId || null, settleMode: settleMode || 'hack', nessieAccountId: nessieAccountId || null }));
    return [201, await attestationResult(ctx, pubkey, signature, { name, kind, settle_mode: settleMode || 'hack', status: 'verified', attestation_pda: pda,
      issued_sig: signature, issued_at: new Date(), revoked_sig: null, revoked_at: null, expires_at: expiresAt })];
  },

  'POST /api/attestations/revoke': async ({ body, ctx }) => {
    const pubkey = validate.pubkey(body.pubkey);
    await requireFundedAuthority(ctx.authority);
    const { signature, pda, name, expiresAt } = await chain(revoke(pubkey));
    return attestationResult(ctx, pubkey, signature, { name, status: 'revoked', attestation_pda: pda,
      issued_sig: null, issued_at: null, revoked_sig: signature, revoked_at: new Date(), expires_at: expiresAt });
  },

  // Binds a badge key to a Nessie customer + account (creates both when no ids are given).
  'POST /api/enroll': async ({ body }) => {
    const r = await enroll({ pubkey: validate.pubkey(body.pubkey), nessieCustomerId: body.nessieCustomerId || undefined,
                             nessieAccountId: body.nessieAccountId || undefined });
    return { customerId: r.customer_id, accountId: r.account_id };
  },

  'GET /api/approvals': async ({ query }) => ({ approvals: await listApprovals(validate.limit(query.get('limit'), 40)) }),

  'GET /api/routes': async ({ query }) => ({ routes: await listRoutes(validate.limit(query.get('limit'), 40)) }),
  'GET /api/relays/leaderboard': async () => ({ relays: await relayLeaderboard() }),
  'GET /api/settlements': async ({ query }) => ({ settlements: await listSettlements(validate.limit(query.get('limit'), 40)) }),
  'GET /api/topups': async ({ query }) => ({ topups: await listTopups(validate.limit(query.get('limit'), 40)) }),

  // Capital One top-up: Nessie withdrawal, then HACK from the treasury to the badge.
  'POST /api/topups': async ({ body, ctx }) => {
    await requireFundedAuthority(ctx.authority);
    return [201, await topup({ pubkey: body.pubkey, amount: body.amount }, { authority: ctx.authority })];
  },

  'GET /api/attacks': async ({ query }) =>
    ({ attempts: (await q(Q.attacks, [validate.limit(query.get('limit'), 20), null])).rows.map(r => toAttack(r)) }),

  'POST /api/attacks': async ({ body, ctx }) => {
    // The console can only target our own badges: the victim must be a pubkey from server/config/badges.json.
    const victim = validate.pubkey(body.victim);
    const badge = ctx.badges.find(b => b.pubkey === victim) ?? fail('unknown_badge', 'victim must be the pubkey of a configured badge');
    const { mint, decimals, symbol } = token;
    const displayRaw = validate.amount(body.displayAmount ?? 5, decimals), actualRaw = validate.amount(body.actualAmount ?? 500, decimals);
    if (!token.found) fail('not_configured', 'HACK mint is not configured or not found on chain', { gap: 'hack_mint_missing' });
    const attacker = env.ATTACKER_PUBKEY || ctx.authority.address;
    const warnings = [];
    await ensureAta(ctx.authority, attacker, mint).catch(err =>
      warnings.push(`attacker token account could not be created (${errMsg(err)}), the transfer would fail on chain if signed`));
    const source = badge.tokenAccount ?? await ataOf(victim, mint);
    const tx = await chain(buildTamperedTx({ victim, victimTokenAccount: source, attacker, mint, decimals, actualAmountRaw: actualRaw }));
    const held = (await getBalances(ctx.badges)).get(victim)?.hack;
    if (held != null && held < actualRaw / 10 ** decimals) warnings.push(`victim holds ${held} ${symbol}, less than the actual amount, the transfer would fail on chain if signed`);
    const { rows: [{ id }] } = await q(Q.insertAttack, [victim, attacker, displayRaw, actualRaw, decimals, mint, source, tx.destTokenAccount,
      tx.txBase64, tx.messageBase64, tx.txBytes, tx.txVersion, tx.blockhash, tx.lastValidBlockHeight]);
    return [201, await attackById(id, warnings)];
  },

  'POST /api/attacks/outcome': async ({ body }) => attackById(await rejectAttack(body)),
};

async function handle(req, res, ctx) {
  // Host allowlist: blocks DNS rebinding (a hostile page that resolves its own name to 127.0.0.1).
  const host = (req.headers.host ?? '').replace(/:\d+$/, '');
  if (host !== 'localhost' && host !== '127.0.0.1') fail('forbidden_origin', 'Host not allowed');
  const url = URL.parse(req.url, 'http://localhost') ?? fail('not_found', 'no such route');   // e.g. "GET //"
  if (req.method === 'GET' && url.pathname === '/api/events') return sse(req, res);
  const route = routes[`${req.method} ${url.pathname}`] ?? fail('not_found', 'no such route');
  const out = await route({ query: url.searchParams, body: req.method === 'POST' ? await readJson(req, true) : null, ctx });
  if (Array.isArray(out)) send(res, out[0], out[1]); else send(res, 200, out);
}

// ── Badge LAN listener (00 §8.1): badge routes only, never admin. Closed unless BADGE_LISTEN_HOST is set. ──
// No authentication: acceptable on devnet and stated as a limitation. Everything a badge sends is re-verified.
// BADGE-GAP(attack-delivery): stub transport = badge polls this over hotspot Wi-Fi. BLE is the alternative.
// BADGE-GAP(attack-outcome): badge may report 'rejected' here; 'signed' is only trusted from chain.
export const badgeListener = { open: false, url: null };
async function handleBadge(req, res, ctx) {
  const url = URL.parse(req.url, 'http://badge') ?? fail('not_found', 'no such route');
  if (req.method === 'GET' && url.pathname === '/badge/pending') {
    const badge = validate.pubkey(url.searchParams.get('badge'));
    if (!ctx.badges.some(b => b.pubkey === badge)) fail('unknown_badge', 'not a configured badge');
    const r = (await q(Q.pendingForBadge, [badge])).rows[0];
    if (!r) return send(res, 204);
    // ponytail: the claimed merchant name is fixed; the lie the demo needs is the amount.
    return send(res, 200, { id: r.id, claimed: { merchant: 'MHacks Merch', amount: Number(r.display_amount_raw) / 10 ** r.decimals, symbol: token.symbol },
                            txBase64: r.tx_base64, messageBase64: r.message_base64, txVersion: r.tx_version, txBytes: r.tx_bytes });
  }
  if (req.method === 'POST' && url.pathname === '/badge/outcome') {
    await rejectAttack(await readJson(req, false));
    return send(res, 200, { ok: true });
  }
  if (req.method === 'GET' && url.pathname === '/health') return send(res, 200, { ok: true });
  const [, route, arg] = url.pathname.split('/');
  if (req.method === 'GET' && route === 'registry' && arg) {
    // Signed fresh on every request (RECORD_TTL_S = 30 on the badge). Unknown key -> 404 = unverified.
    const r = await getSignedRecord(ctx.authority, validate.pubkey(arg), url.searchParams.get('format') === 'compact' ? 'compact' : 'full');
    return r.status === 200 ? send(res, 200, r.body) : fail('not_found', 'no registry record for this key');
  }
  if (req.method === 'GET' && route === 'balance' && arg) return send(res, 200, await balance(validate.pubkey(arg)));
  if (req.method === 'POST' && url.pathname === '/bank/authorize')
    return send(res, 200, await authorize(await readJson(req, false), { recordFields, authority: ctx.authority }));
  if (req.method === 'POST' && url.pathname === '/feed/solana')
    return send(res, 200, await confirmSolana(await readJson(req, false), { recordFields, onConfirmed }));
  if (req.method === 'GET' && route === 'route' && arg === 'ctx') {
    const r = await routeCtx(ctx.authority, validate.pubkey(url.pathname.split('/')[3]));
    return r.status === 200 ? send(res, 200, r.body) : fail('not_found', 'no registry record for this key');
  }
  if (req.method === 'POST' && url.pathname === '/feed/route')
    return send(res, 200, await confirmRoute(await readJson(req, false), { recordFields, authority: ctx.authority, onConfirmed }));
  if (req.method === 'POST' && url.pathname === '/feed/event')
    return send(res, 200, await recordBadgeEvent(await readJson(req, false)));
  fail('not_found', 'no such route');
}

export function startHttp(ctx) {
  const serve = handler => http.createServer((req, res) => handler(req, res, ctx).catch(err => sendError(res, err)));
  if (env.HOST !== '127.0.0.1' && env.HOST !== 'localhost') console.warn(`[http] WARNING: HOST=${env.HOST} is not loopback. The admin API (issue/revoke) is exposed beyond this machine.`);
  serve(handle)
    .on('error', err => { console.error('[http] cannot listen:', err.message); process.exit(1); })
    .listen(env.PORT, env.HOST, () => console.log(`[http] api on http://${env.HOST}:${env.PORT}`));
  setInterval(() => { for (const res of clients) res.write(': ping\n\n'); }, 15_000).unref();

  if (env.BADGE_LISTEN_HOST) {
    serve(handleBadge)
      .on('error', err => console.error('[http] badge listener failed, attack delivery stays closed:', err.message))
      .listen(env.BADGE_LISTEN_PORT, env.BADGE_LISTEN_HOST, () => {
        // A wildcard bind is not an address a badge can dial: show this machine's LAN address instead.
        // ponytail: first non-internal IPv4. With several interfaces up, set BADGE_LISTEN_HOST to the hotspot IP.
        const host = env.BADGE_LISTEN_HOST !== '0.0.0.0' ? env.BADGE_LISTEN_HOST
          : Object.values(networkInterfaces()).flat().find(i => i.family === 'IPv4' && !i.internal)?.address ?? env.BADGE_LISTEN_HOST;
        Object.assign(badgeListener, { open: true, url: `http://${host}:${env.BADGE_LISTEN_PORT}/badge/pending` });
        console.warn(`[http] badge listener OPEN on ${env.BADGE_LISTEN_HOST}:${env.BADGE_LISTEN_PORT}, badges poll ${badgeListener.url} (reachable from the LAN)`);
        ctx.onChange?.();
      });
  }
}
