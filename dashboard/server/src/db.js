import pg from 'pg';
import { env, token } from './config.js';

const PAYMENT_SELECT = `
SELECT p.signature, p.block_time, p.slot, p.payer, p.payee, p.payer_token_account, p.payee_token_account,
       p.mint, p.amount_raw, p.decimals, p.payee_status, p.payee_name, p.source, p.attack_id, p.ingest_lag_ms,
       pb.id AS payer_badge_id, pb.label AS payer_label, yb.id AS payee_badge_id, yb.label AS payee_label
FROM payments p
LEFT JOIN badges pb ON pb.pubkey = p.payer
LEFT JOIN badges yb ON yb.pubkey = p.payee`;

// Every SQL string the server runs lives here.
export const Q = {
  // $1 limit, $2 before (timestamptz or NULL). Relational badges joined to the hypertable, twice.
  payments: `${PAYMENT_SELECT}
WHERE p.block_time < COALESCE($2::timestamptz, 'infinity'::timestamptz)
ORDER BY p.block_time DESC, p.slot DESC, p.signature DESC
LIMIT $1`,

  // $1 signature. Used for the SSE payload, so it also looks up the next-older row for prevSignature.
  paymentBySig: `SELECT x.*, (SELECT o.signature FROM payments o
                    WHERE (o.block_time, o.slot, o.signature) < (x.block_time, x.slot, x.signature)
                    ORDER BY o.block_time DESC, o.slot DESC, o.signature DESC LIMIT 1) AS prev_signature
FROM (${PAYMENT_SELECT} WHERE p.signature = $1 LIMIT 1) x`,

  // The verification snapshot is taken in SQL at insert time.
  insertPayment: `
INSERT INTO payments (block_time, signature, slot, payer, payee, payer_token_account, payee_token_account,
                      mint, amount_raw, decimals, payee_status, payee_name, source, attack_id, ingest_lag_ms)
SELECT $1::timestamptz, $2::text, $3::bigint, $4::text, $5::text, $6::text, $7::text,
       $8::text, $9::bigint, $10::smallint, COALESCE(a.status, 'unverified'), a.name, 'chain', $11::uuid, $12::integer
FROM (SELECT 1) AS one LEFT JOIN attestations a ON a.subject = $5::text
ON CONFLICT (signature, block_time) DO NOTHING
RETURNING signature`,

  latestChainSig: `SELECT signature FROM payments WHERE source = 'chain' ORDER BY block_time DESC, slot DESC LIMIT 1`,

  // $1 sig, $2 victim, $3 attacker, $4 actual_amount_raw. Run before insertPayment. Idempotent per signature: when
  // that insert fails and the poll handles the transaction again, the attempt already holding $1 is returned,
  // instead of a second pending attempt being marked signed (or the payment losing its attack_id).
  matchAttack: `
UPDATE attack_attempts SET outcome = 'signed', signature = $1, resolved_at = COALESCE(resolved_at, now())
WHERE id = (SELECT id FROM attack_attempts
            WHERE signature = $1 OR (outcome = 'pending' AND victim = $2 AND attacker = $3 AND actual_amount_raw = $4)
            ORDER BY (signature = $1) DESC NULLS LAST, created_at DESC LIMIT 1)
RETURNING id`,

  // Continuous aggregate, includes the live minute via real-time aggregation.
  stats24h: `
SELECT COALESCE(sum(n), 0)::int AS payments,
       COALESCE(sum(verified_n), 0)::int AS verified,
       COALESCE(sum(volume_raw), 0)::text AS volume_raw,
       COALESCE(sum(n) FILTER (WHERE source = 'seed'), 0)::int AS seed_rows
FROM payments_1m WHERE bucket >= now() - INTERVAL '24 hours'`,

  // Continuous aggregate + gapfill: about 61 rows, empty minutes are zeros. The lower bound is bucket-aligned:
  // gapfill emits the minute that contains now() - 60 min, and an unaligned bound would always report it as 0.
  series60m: `
SELECT time_bucket_gapfill('1 minute', bucket, now() - INTERVAL '60 minutes', now()) AS t,
       COALESCE(sum(n), 0)::int AS n,
       COALESCE(sum(verified_n), 0)::int AS verified_n,
       COALESCE(sum(volume_raw), 0)::text AS volume_raw
FROM payments_1m
WHERE bucket >= time_bucket('1 minute', now() - INTERVAL '60 minutes') AND bucket < now()
GROUP BY t ORDER BY t`,

  // Raw hypertable: chain-to-database latency, the PRD 3 second requirement measured in SQL.
  lag: `
SELECT percentile_cont(0.5)  WITHIN GROUP (ORDER BY ingest_lag_ms) AS p50,
       percentile_cont(0.95) WITHIN GROUP (ORDER BY ingest_lag_ms) AS p95
FROM payments
WHERE source = 'chain' AND ingest_lag_ms IS NOT NULL AND block_time >= now() - INTERVAL '1 hour'`,

  // Relational badges + relational attestations + continuous aggregate, one query.
  badges: `
SELECT b.id, b.label, b.pubkey, b.token_account, b.key_location, b.stand_in,
       COALESCE(a.status, 'unverified') AS att_status, a.name AS att_name, a.attestation_pda,
       a.issued_sig, a.revoked_sig, a.expires_at AS att_expires_at, y.kind AS att_kind, y.settle_mode AS att_settle_mode,
       COALESCE(v.n, 0)::int AS received_n, COALESCE(v.volume_raw, 0)::text AS received_raw
FROM badges b
LEFT JOIN attestations a ON a.subject = b.pubkey
LEFT JOIN payees y ON y.pubkey = b.pubkey
LEFT JOIN (SELECT payee, sum(n) AS n, sum(volume_raw) AS volume_raw FROM payee_volume_1h WHERE source = 'chain' GROUP BY payee) v
       ON v.payee = b.pubkey
ORDER BY b.id`,

  // $1 subject or NULL for all.
  attestations: `
SELECT a.*, b.id AS badge_id, b.label, y.kind, y.settle_mode FROM attestations a LEFT JOIN badges b ON b.pubkey = a.subject
LEFT JOIN payees y ON y.pubkey = a.subject
WHERE ($1::text IS NULL OR a.subject = $1)
ORDER BY a.updated_at DESC`,

  // $1 subject, $2 name, $3 pda, $4 issued_sig (NULL when discovered by sync), $5 expires_at.
  // The WHERE keeps the 30 s sync from rewriting (and re-NOTIFYing) rows that have not changed.
  upsertVerified: `
INSERT INTO attestations (subject, name, status, attestation_pda, issued_sig, issued_at, expires_at)
VALUES ($1, $2, 'verified', $3, $4, now(), $5)
ON CONFLICT (subject) DO UPDATE SET name = EXCLUDED.name, status = 'verified', attestation_pda = EXCLUDED.attestation_pda,
  issued_sig = COALESCE(EXCLUDED.issued_sig, attestations.issued_sig), issued_at = now(),
  revoked_sig = NULL, revoked_at = NULL, expires_at = EXCLUDED.expires_at, updated_at = now()
WHERE EXCLUDED.issued_sig IS NOT NULL OR attestations.status <> 'verified' OR attestations.name <> EXCLUDED.name`,

  // $1 subject, $2 revoked_sig (NULL when discovered by sync).
  markRevoked: `
UPDATE attestations SET status = 'revoked', revoked_sig = $2, revoked_at = now(), updated_at = now()
WHERE subject = $1 AND status = 'verified'`,

  // Everything the sync must look up on chain: configured badges plus anything the mirror says is verified.
  syncSubjects: `
SELECT pubkey AS subject FROM badges WHERE pubkey IS NOT NULL
UNION SELECT subject FROM attestations WHERE status = 'verified'`,

  // $1 limit, $2 id or NULL. 'expired' is derived, never stored.
  attacks: `
SELECT t.*, CASE WHEN t.outcome = 'pending' AND t.created_at < now() - INTERVAL '90 seconds'
                 THEN 'expired' ELSE t.outcome END AS outcome_now,
       b.id AS victim_badge_id, b.label AS victim_label
FROM attack_attempts t LEFT JOIN badges b ON b.pubkey = t.victim
WHERE ($2::uuid IS NULL OR t.id = $2)
ORDER BY t.created_at DESC LIMIT $1`,

  insertAttack: `
INSERT INTO attack_attempts (victim, attacker, display_amount_raw, actual_amount_raw, decimals, mint,
                             source_token_account, dest_token_account, tx_base64, message_base64, tx_bytes,
                             tx_version, blockhash, last_valid_block_height)
VALUES ($1, $2, $3, $4, $5, $6, $7, $8, $9, $10, $11, $12, $13, $14)
RETURNING id`,

  // $1 id. Only pending -> rejected. 'signed' is set only by matchAttack, from on-chain evidence.
  rejectAttack: `
UPDATE attack_attempts SET outcome = 'rejected', resolved_at = now()
WHERE id = $1 AND outcome = 'pending' RETURNING id`,

  // $1 victim pubkey. Marks delivery on the first poll only: a badge polls every second or so, and an UPDATE per
  // poll would NOTIFY (and make every browser refetch) each time.
  pendingForBadge: `
WITH t AS (SELECT id, created_at FROM attack_attempts WHERE victim = $1 AND outcome = 'pending'
           AND created_at > now() - INTERVAL '90 seconds' ORDER BY created_at DESC LIMIT 1),
     d AS (UPDATE attack_attempts a SET delivered_at = now() FROM t
           WHERE a.id = t.id AND a.created_at = t.created_at AND a.delivered_at IS NULL)
SELECT a.* FROM attack_attempts a JOIN t ON a.id = t.id AND a.created_at = t.created_at`,

  // The "Tiger Data" card. LEFT JOIN because the stats function returns no row on an empty hypertable.
  // Exact count(*): approximate_row_count() was off by 3x on freshly converted columnstore chunks.
  dbVersion: `SELECT extversion FROM pg_extension WHERE extname = 'timescaledb'`,
  dbHypertable: `
SELECT (SELECT count(*) FROM payments) AS rows, hypertable_size('payments') AS total_bytes, s.*
FROM (SELECT 1) AS one LEFT JOIN hypertable_columnstore_stats('payments') s ON true`,
  dbCaggs: `
SELECT ca.view_name, ca.materialized_only, EXTRACT(EPOCH FROM j.schedule_interval)::int AS schedule_sec,
       js.last_successful_finish, js.last_run_status
FROM timescaledb_information.continuous_aggregates ca
LEFT JOIN timescaledb_information.jobs j
       ON j.hypertable_schema = ca.view_schema AND j.hypertable_name = ca.view_name
      AND j.proc_name = 'policy_refresh_continuous_aggregate'
LEFT JOIN timescaledb_information.job_stats js ON js.job_id = j.job_id
ORDER BY ca.view_name DESC`,

  schemaReady: `SELECT to_regclass('public.payments') IS NOT NULL AS ok`,
};

export const dbState = { ok: false, error: null, schema: false };
export const queryMs = { feed: null, series: null };
let pool;

// Never blocks boot for long and never throws: a 5 s health check keeps dbState honest in both directions.
export async function initDb(onChange) {
  // idleTimeoutMillis 0: a fresh backend spends ~40 ms warming its catalog cache over 73 chunks, so keep the 5 sessions open.
  // query_timeout: a session that died without a FIN/RST (NAT timeout, Wi-Fi change) never answers. Without it the
  // query hangs, the session is never returned, and five of those leave every route at 503 although Postgres is up.
  pool = new pg.Pool({ connectionString: env.DATABASE_URL, max: 5, connectionTimeoutMillis: 3000, idleTimeoutMillis: 0, query_timeout: 5000 });
  pool.on('error', () => {});
  const check = async () => {
    const before = JSON.stringify(dbState);
    try { Object.assign(dbState, { ok: true, error: null, schema: (await pool.query(Q.schemaReady)).rows[0].ok }); }
    catch (err) { Object.assign(dbState, { ok: false, error: err.message || err.code || 'connection failed', schema: false }); }
    if (JSON.stringify(dbState) !== before) onChange?.();
  };
  await check();
  setInterval(check, 5000).unref();
}

export const closeDb = () => pool?.end();

export function q(sql, params) {
  return pool.query(sql, params).catch(err => {
    throw Object.assign(new Error(err.message || err.code || 'database error'), { code: 'db_error' });
  });
}

export async function timed(key, sql, params) {
  const t0 = performance.now();
  const res = await q(sql, params);
  queryMs[key] = Math.round((performance.now() - t0) * 10) / 10;
  return res;
}

// Dedicated session for LISTEN (a pooled client would lose the subscription). Reconnects after 2 s.
// Notifications sent while the session was down are gone, so onReconnect lets the caller tell clients to refetch.
export function listen(onNotify, onReconnect) {
  let dropped = false;
  const connect = async () => {
    const c = new pg.Client({ connectionString: env.DATABASE_URL, connectionTimeoutMillis: 3000, query_timeout: 5000 });
    let dead = false, beat;
    const retry = () => { if (dead) return; dead = dropped = true; clearInterval(beat); c.end().catch(() => {}); setTimeout(connect, 2000); };
    c.on('error', retry);
    c.on('end', retry);
    // onNotify is async: a promise chain also catches its rejections (a try/catch would let them crash the process).
    c.on('notification', m => Promise.resolve(m.payload).then(JSON.parse).then(onNotify).catch(err => console.error('[db] notify handler:', err.message)));
    try {
      await c.connect(); await c.query('LISTEN feed');
      // Heartbeat: an idle LISTEN session never writes, so a silently dead connection would stay "connected" and
      // deliver nothing while the SSE sockets stay open. A timed-out SELECT 1 reconnects it.
      beat = setInterval(() => c.query('SELECT 1').catch(retry), 10_000);
      if (dropped) { dropped = false; onReconnect?.(); }
    } catch { retry(); }
  };
  connect();
}

// badges.json is the source of truth: replace the table contents in one transaction.
export async function syncBadges(badges) {
  const c = await pool.connect();
  try {
    await c.query('BEGIN');
    await c.query('DELETE FROM badges');
    await c.query(`INSERT INTO badges (id, label, pubkey, token_account, key_location, stand_in)
      SELECT id, label, pubkey, "tokenAccount", "keyLocation", "standIn"
      FROM json_to_recordset($1::json) AS t(id smallint, label text, pubkey text, "tokenAccount" text, "keyLocation" text, "standIn" boolean)`,
      [JSON.stringify(badges)]);
    await c.query('COMMIT');
  } catch (err) { await c.query('ROLLBACK').catch(() => {}); throw err; }
  finally { c.release(); }
}

// ── row -> API JSON (contract: plan section 4.4) ──
const ui = (raw, decimals) => Number(raw) / 10 ** decimals;
const iso = d => (d ? d.toISOString() : null);

export const toPayment = (r, prevSignature = r.prev_signature ?? null) => ({
  signature: r.signature, slot: Number(r.slot), blockTime: iso(r.block_time),
  payer: { pubkey: r.payer, badgeId: r.payer_badge_id ?? null, label: r.payer_label ?? null },
  payee: { pubkey: r.payee, badgeId: r.payee_badge_id ?? null, label: r.payee_label ?? null, name: r.payee_name, status: r.payee_status },
  amount: ui(r.amount_raw, r.decimals), amountRaw: String(r.amount_raw), decimals: r.decimals, symbol: token.symbol, mint: r.mint,
  payerTokenAccount: r.payer_token_account, payeeTokenAccount: r.payee_token_account,
  source: r.source, attackId: r.attack_id, lagMs: r.ingest_lag_ms, prevSignature,
});

export const toBadge = (r, bal) => ({
  id: r.id, label: r.label, pubkey: r.pubkey, tokenAccount: r.token_account ?? bal?.tokenAccount ?? null,
  keyLocation: r.key_location, standIn: r.stand_in, sol: bal?.sol ?? null, hack: bal?.hack ?? null,
  attestation: { status: shownStatus(r.att_status, r.revoked_sig, r.att_expires_at), kind: r.att_kind ?? null, settleMode: r.att_settle_mode ?? null, name: r.att_name, pda: r.attestation_pda, issuedSig: r.issued_sig, revokedSig: r.revoked_sig },
  received: { count: r.received_n, amount: ui(r.received_raw, token.decimals) },
});

// The attestations CHECK only allows verified|revoked, so registry.js mirrors an expired attestation as revoked
// with no revoked_sig. Show it as what it is.
const shownStatus = (status, revokedSig, expiresAt) =>
  (status === 'revoked' && !revokedSig && expiresAt && new Date(expiresAt) <= new Date() ? 'expired' : status);

export const toAttestation = r => ({
  subject: r.subject, badgeId: r.badge_id ?? null, label: r.label ?? null, name: r.name, kind: r.kind ?? null, settleMode: r.settle_mode ?? null,
  status: shownStatus(r.status, r.revoked_sig, r.expires_at), pda: r.attestation_pda,
  issuedSig: r.issued_sig, issuedAt: iso(r.issued_at), revokedSig: r.revoked_sig, revokedAt: iso(r.revoked_at), expiresAt: iso(r.expires_at),
});

export const toAttack = (r, warnings = []) => ({
  id: r.id, createdAt: iso(r.created_at), expiresAt: new Date(r.created_at.getTime() + 90_000).toISOString(),
  victim: { pubkey: r.victim, badgeId: r.victim_badge_id ?? null, label: r.victim_label ?? null }, attacker: r.attacker,
  displayAmount: ui(r.display_amount_raw, r.decimals), actualAmount: ui(r.actual_amount_raw, r.decimals), symbol: token.symbol, decimals: r.decimals,
  txBase64: r.tx_base64, messageBase64: r.message_base64, txBytes: r.tx_bytes, txVersion: r.tx_version,
  blockhash: r.blockhash, lastValidBlockHeight: Number(r.last_valid_block_height),
  decoded: { program: 'spl-token', instruction: 'transferChecked', source: r.source_token_account, destination: r.dest_token_account,
             mint: r.mint, authority: r.victim, amount: ui(r.actual_amount_raw, r.decimals), amountRaw: String(r.actual_amount_raw), decimals: r.decimals },
  outcome: r.outcome_now ?? r.outcome, signature: r.signature, deliveredAt: iso(r.delivered_at), resolvedAt: iso(r.resolved_at), warnings,
});
