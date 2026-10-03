// DEMO mode: fixtures + an in-memory fake backend + the block ticker.
// Every object here has the exact shape of the real API (PLAN 4.4); demo.test.js asserts it.
// Plain JS on purpose (no JSX, no DOM at import time) so `node --test` can load it.

const B58 = '123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz';
let seed = 20261003;
const rnd = () => (seed = (seed * 1664525 + 1013904223) >>> 0) / 2 ** 32; // LCG: fixtures are stable per load
const pick = (...xs) => xs[Math.floor(rnd() * xs.length)];
const b58 = (len, prefix = '') => prefix + Array.from({ length: len - prefix.length }, () => B58[Math.floor(rnd() * 58)]).join('');
const b64 = n => btoa(String.fromCharCode(...Array.from({ length: n }, () => Math.floor(rnd() * 256))));
const iso = ms => new Date(ms).toISOString();
const clone = x => JSON.parse(JSON.stringify(x));
const fail = (code, message, status = 400) => Object.assign(new Error(message), { code, status });

const EXPLORER = 'https://explorer.solana.com';
const txUrl = sig => `${EXPLORER}/tx/${sig}?cluster=devnet`;
const T0 = Date.now();
const DEC = 2;

// Looks like base58, 44 chars, but every demo key starts with "Demo" so nobody mistakes it for a real one.
const MINT = b58(44, 'DemoHACK'), AUTH = b58(44, 'DemoAuth'), OLD = b58(44, 'DemoXMerch');
const ATA = { [AUTH]: b58(44, 'DemoAta'), [OLD]: b58(44, 'DemoAta') };

const none = { status: 'unverified', name: null, pda: null, issuedSig: null, revokedSig: null };
const mkBadge = (id, label, prefix, keyLocation, standIn, hack) => {
  const b = { id, label, pubkey: b58(44, prefix), tokenAccount: b58(44, 'DemoAta'), keyLocation, standIn,
    sol: 0.05, hack, attestation: { ...none }, received: { count: 0, amount: 0 } };
  ATA[b.pubkey] = b.tokenAccount;
  return b;
};
const badges = [
  mkBadge(1, 'Merchant (team)', 'DemoMerch1', 'se050', false, 1000),
  mkBadge(2, 'Impostor (team)', 'DemoFake2', 'software', true, 1000),
  mkBadge(3, 'Judge A', 'DemoJudge3', 'se050', false, 1500),
  mkBadge(4, 'Judge B', 'DemoJudge4', 'unknown', false, 1000),
];
const badgeOf = pubkey => badges.find(b => b.pubkey === pubkey);

const attestations = [
  { subject: badges[0].pubkey, badgeId: 1, label: badges[0].label, name: 'MHacks Merch', status: 'verified', pda: b58(44, 'DemoAtt'),
    issuedSig: b58(88), issuedAt: iso(T0 - 3 * 3600e3), revokedSig: null, revokedAt: null, expiresAt: iso(T0 + 30 * 86400e3) },
  { subject: OLD, badgeId: null, label: null, name: 'Old Merch', status: 'revoked', pda: b58(44, 'DemoAtt'),
    issuedSig: b58(88), issuedAt: iso(T0 - 26 * 3600e3), revokedSig: b58(88), revokedAt: iso(T0 - 2 * 3600e3), expiresAt: iso(T0 + 29 * 86400e3) },
];
const syncBadge = a => { const b = badgeOf(a.subject); if (b) b.attestation = { status: a.status, name: a.name, pda: a.pda, issuedSig: a.issuedSig, revokedSig: a.revokedSig }; };
attestations.forEach(syncBadge);

const party = pubkey => ({ pubkey, badgeId: badgeOf(pubkey)?.id ?? null, label: badgeOf(pubkey)?.label ?? null });
const payments = []; // newest first
// One payment = one block. The payee's verification is snapshotted at mint time, like the server does at ingest.
function mint({ payer, payee, amount, ms, attackId = null }) {
  const att = attestations.find(a => a.subject === payee), head = payments[0];
  const p = {
    signature: b58(88), slot: (head?.slot ?? 412345000) + 6 + Math.floor(rnd() * 30), blockTime: iso(ms),
    payer: party(payer), payee: { ...party(payee), name: att?.name ?? null, status: att?.status ?? 'unverified' },
    amount, amountRaw: String(Math.round(amount * 10 ** DEC)), decimals: DEC, symbol: 'HACK', mint: MINT,
    payerTokenAccount: ATA[payer], payeeTokenAccount: ATA[payee],
    source: 'demo', attackId, lagMs: 900 + Math.floor(rnd() * 1700), prevSignature: head?.signature ?? null,
  };
  payments.unshift(p);
  if (payments.length > 500) payments.pop(); // ponytail: demo keeps 500 blocks in memory, plenty for a session
  const from = badgeOf(payer), to = badgeOf(payee);
  if (from) from.hack -= amount;
  if (to) { to.hack += amount; to.received.count++; to.received.amount += amount; }
  return p;
}
const [merchant, impostor, judgeA, judgeB] = badges.map(b => b.pubkey);
const randomTransfer = () => {
  const payee = rnd() < 0.7 ? merchant : pick(impostor, impostor, judgeB);
  const payer = pick(...[judgeA, judgeB, impostor].filter(k => k !== payee));
  return { payer, payee, amount: 1 + Math.floor(rnd() * 20) };
};

// ~25 seed blocks over the last few minutes (oldest minted first so the prevSignature chain is real).
const SIGNED_ATTACK = '5e0c7d1a-3b52-4f0e-9a61-2d7f4c8b1e90', PENDING_ATTACK = '0b7e2f44-91c3-4d1a-8e57-6a3c9f2d5b18';
for (let i = 24; i >= 0; i--) {
  const ms = T0 - 6000 - i * 11000 - Math.floor(rnd() * 4000);
  if (i === 6) mint({ payer: judgeA, payee: AUTH, amount: 500, ms, attackId: SIGNED_ATTACK });
  else if (i === 13) mint({ payer: judgeB, payee: OLD, amount: 8, ms });
  else mint({ ...randomTransfer(), ms });
}

const minute = ms => iso(Math.floor(ms / 60000) * 60000);
const series = Array.from({ length: 61 }, (_, i) => {
  const n = Math.max(0, Math.round(2.4 + 2 * Math.sin(i / 5) + (rnd() - 0.5) * 3));
  const verifiedN = Math.round(n * (0.6 + rnd() * 0.4));
  return { t: minute(T0 - (60 - i) * 60000), n, verifiedN, volume: n * (4 + Math.floor(rnd() * 9)) };
});
let stats = { window: '24h', payments: 128, volume: 1840, verifiedShare: 0.82, seedRows: 0,
  lagMs: { p50: 1900, p95: 2800 }, series, queryMs: 2.1 };

// Shared by the demo store and the live SSE path: one payment arrives, the tiles and the live minute move at once.
// ponytail: a payment for an older minute is counted in the newest bucket; the next /api/stats refetch corrects it.
export function bumpStats(s, p) {
  const t = p.blockTime.slice(0, 17) + '00.000Z', v = p.payee.status === 'verified' ? 1 : 0;
  const out = s.series.map(b => ({ ...b }));
  if (!out.length || out.at(-1).t < t) { out.push({ t, n: 0, verifiedN: 0, volume: 0 }); if (out.length > 61) out.shift(); }
  const last = out.at(-1);
  last.n++; last.verifiedN += v; last.volume = +(last.volume + p.amount).toFixed(p.decimals);
  return { ...s, payments: s.payments + 1, volume: +(s.volume + p.amount).toFixed(p.decimals),
    verifiedShare: (s.verifiedShare * s.payments + v) / (s.payments + 1), series: out };
}

const dbStats = {
  timescaledb: '2.30.2',
  payments: { rows: 51968, chunks: 74, columnstoreChunks: 70, bytesBefore: 18350080, bytesAfter: 2228224, compressionRatio: 0.879, totalBytes: 3145728 },
  caggs: [
    { name: 'payments_1m', realtime: true, scheduleSec: 60, lastRefresh: iso(T0 - 20000), lastStatus: 'Success' },
    { name: 'payee_volume_1h', realtime: true, scheduleSec: 300, lastRefresh: iso(T0 - 140000), lastStatus: 'Success' },
  ],
  queryMs: { feed: 1.2, series: 0.9 },
};

function attempt({ id, victim, displayAmount, actualAmount, ms }) {
  const v = badgeOf(victim);
  return {
    id, createdAt: iso(ms), expiresAt: iso(ms + 90000),
    victim: { pubkey: victim, badgeId: v.id, label: v.label }, attacker: AUTH,
    displayAmount, actualAmount, symbol: 'HACK', decimals: DEC,
    txBase64: b64(280), messageBase64: b64(215), txBytes: 280, txVersion: 'legacy',
    blockhash: b58(44), lastValidBlockHeight: 398765432,
    decoded: { program: 'spl-token', instruction: 'transferChecked', source: v.tokenAccount, destination: ATA[AUTH],
      mint: MINT, authority: victim, amount: actualAmount, amountRaw: String(Math.round(actualAmount * 10 ** DEC)), decimals: DEC },
    outcome: 'pending', signature: null, deliveredAt: null, resolvedAt: null,
    warnings: v.hack < actualAmount ? [`Victim holds ${v.hack} HACK, less than the actual amount ${actualAmount}; the transfer would fail on chain.`] : [],
  };
}
const forged = payments.find(p => p.attackId);
const attempts = [
  attempt({ id: PENDING_ATTACK, victim: judgeB, displayAmount: 5, actualAmount: 500, ms: T0 - 15000 }),
  { ...attempt({ id: SIGNED_ATTACK, victim: judgeA, displayAmount: 5, actualAmount: 500, ms: Date.parse(forged.blockTime) - 9000 }),
    outcome: 'signed', signature: forged.signature, deliveredAt: iso(Date.parse(forged.blockTime) - 7000), resolvedAt: forged.blockTime },
];

export const demoStatus = {
  ok: true, app: 'badgepay', cluster: 'devnet', explorer: EXPLORER,
  db: { ok: true, schema: true, error: null },
  rpc: { ok: true, url: 'https://api.devnet.solana.com' },
  ingest: { state: 'live', lastSignature: payments[0].signature, lastEventAt: payments[0].blockTime },
  token: { mint: MINT, symbol: 'HACK', decimals: DEC },
  authority: { pubkey: AUTH, sol: 0.93 },
  registry: { program: '22zoJMtdu4tQc2PzL74ZUT7FrwgB1Udec8DdW4yw4BdG', credential: b58(44, 'DemoCred'), schema: b58(44, 'DemoSchema'),
    credentialName: 'MHacks Verified', schemaName: 'badge-identity', schemaVersion: 1, ready: true },
  badgeListener: { open: false, url: null },
  gaps: [
    // DEMO keeps the badge-dependent gaps on screen, so the placeholders are visible before any badge exists.
    { code: 'key_location_unknown', severity: 'warn', badgeGap: 'key-location', title: 'Key location unknown for a configured badge',
      fix: 'Set keyLocation to "se050" or "software" in server/config/badges.json (Settings > Identity on the badge).', pages: ['badges'] },
    { code: 'attack_delivery_unconfigured', severity: 'warn', badgeGap: 'attack-delivery', title: 'Badge delivery listener is closed',
      fix: 'Set `BADGE_LISTEN_HOST` in `.env` to the laptop\'s hotspot IP to open the badge-only listener.', pages: ['attack'] },
    // BADGE-GAP(attack-outcome): kept in DEMO so the Attack page still explains the manual "Mark rejected" step.
    { code: 'attack_outcome_manual', severity: 'info', badgeGap: 'attack-outcome', title: 'Rejected outcomes are marked manually',
      fix: 'Use "Mark rejected" on the Attack page until badge firmware POSTs /badge/outcome. Signed is detected from chain.', pages: ['attack'] },
  ],
};

/** GET, in memory. Returns a fresh copy shaped exactly like `/api/<resource>`. */
export function demoGet(resource, params = {}) {
  const limit = Math.min(200, Math.max(1, Number(params.limit) || 40));
  switch (resource) {
    case 'status': return clone({ ...demoStatus, ingest: { state: 'live', lastSignature: payments[0].signature, lastEventAt: payments[0].blockTime } });
    case 'payments': {
      const page = (params.before ? payments.filter(p => p.blockTime < params.before) : payments).slice(0, limit);
      return clone({ payments: page.map((p, i) => ({ ...p, prevSignature: page[i + 1]?.signature ?? null })) });
    }
    case 'stats': return clone(stats);
    case 'stats/db': return clone(dbStats);
    case 'badges': return clone({ badges });
    case 'attestations': return clone({ attestations });
    case 'attacks': return clone({ attempts: attempts.slice(0, Number(params.limit) || 20).map(a => // 'expired' is derived, never stored
      ({ ...a, outcome: a.outcome === 'pending' && Date.now() - Date.parse(a.createdAt) > 90000 ? 'expired' : a.outcome })) });
    default: throw fail('not_found', `No such resource: ${resource}`, 404);
  }
}

const isPubkey = v => typeof v === 'string' && /^[1-9A-HJ-NP-Za-km-z]{32,44}$/.test(v);
const isAmount = v => Number.isFinite(v) && v <= 1e6 && Math.round(v * 10 ** DEC) >= 1 && Number.isInteger(+(v * 10 ** DEC).toFixed(6));

/** POST, in memory. Same request/response/error shapes as the server, so the admin pages are fully clickable in DEMO. */
export function demoPost(path, body = {}) {
  const now = Date.now();
  if (path === '/api/attestations') {
    const name = typeof body.name === 'string' ? body.name.trim() : '';
    if (!isPubkey(body.pubkey)) throw fail('invalid_pubkey', 'pubkey must be a base58 32-byte address');
    if (!/^[\x20-\x7E]{1,32}$/.test(name)) throw fail('invalid_name', 'name must be 1 to 32 printable ASCII characters');
    const b = badgeOf(body.pubkey), old = attestations.findIndex(a => a.subject === body.pubkey);
    const a = { subject: body.pubkey, badgeId: b?.id ?? null, label: b?.label ?? null, name, status: 'verified',
      pda: old >= 0 ? attestations[old].pda : b58(44, 'DemoAtt'), issuedSig: b58(88), issuedAt: iso(now),
      revokedSig: null, revokedAt: null, expiresAt: iso(now + 30 * 86400e3) };
    if (old >= 0) attestations.splice(old, 1);
    attestations.unshift(a); syncBadge(a);
    return clone({ attestation: a, signature: a.issuedSig, explorerUrl: txUrl(a.issuedSig) });
  }
  if (path === '/api/attestations/revoke') {
    if (!isPubkey(body.pubkey)) throw fail('invalid_pubkey', 'pubkey must be a base58 32-byte address');
    const a = attestations.find(x => x.subject === body.pubkey && x.status === 'verified');
    if (!a) throw fail('conflict', 'No live attestation exists for this pubkey', 409);
    Object.assign(a, { status: 'revoked', revokedSig: b58(88), revokedAt: iso(now) });
    attestations.unshift(...attestations.splice(attestations.indexOf(a), 1)); syncBadge(a);
    return clone({ attestation: a, signature: a.revokedSig, explorerUrl: txUrl(a.revokedSig) });
  }
  if (path === '/api/attacks') {
    const { victim, displayAmount = 5, actualAmount = 500 } = body;
    if (!isPubkey(victim)) throw fail('invalid_pubkey', 'victim must be a base58 32-byte address');
    if (!badgeOf(victim)) throw fail('unknown_badge', 'victim must be one of the configured badges', 404);
    if (!isAmount(displayAmount) || !isAmount(actualAmount)) throw fail('invalid_amount', 'amounts must be > 0, <= 1000000, with at most 2 decimals');
    const a = attempt({ id: crypto.randomUUID(), victim, displayAmount, actualAmount, ms: now });
    attempts.unshift(a);
    return clone(a);
  }
  if (path === '/api/attacks/outcome') {
    const a = attempts.find(x => x.id === body.id);
    if (body.outcome !== 'rejected') throw fail('invalid_param', 'outcome must be "rejected" (signed is only set from chain)');
    if (!a) throw fail('not_found', 'No such attempt', 404);
    if (a.outcome !== 'pending') throw fail('conflict', `Attempt is already ${a.outcome}`, 409);
    Object.assign(a, { outcome: 'rejected', resolvedAt: iso(now) });
    return clone(a);
  }
  throw fail('not_found', `No such route: POST ${path}`, 404);
}

/** Mints one fake payment at the head of the demo chain and returns it (API Payment shape). */
export function mintDemoPayment() {
  const p = mint({ ...randomTransfer(), ms: Date.now() });
  stats = bumpStats(stats, p);
  dbStats.payments.rows++;
  return clone(p);
}

/** Pushes `{type:'payment', data}` (the SSE message shape) about every 4 s, or 10 s under reduced motion. Returns stop(). */
export function startDemoTicker(push) {
  const base = globalThis.matchMedia?.('(prefers-reduced-motion: reduce)').matches ? 10000 : 4000;
  let t = setTimeout(function tick() {
    push({ type: 'payment', data: mintDemoPayment() });
    t = setTimeout(tick, base * (0.7 + Math.random() * 0.6));
  }, base * 0.6);
  return () => clearTimeout(t);
}
