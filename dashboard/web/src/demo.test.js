// node --test web/src/demo.test.js
// The one runnable check for the web side: DEMO fixtures and the fake backend match the API contract (PLAN 4.4).
import test from 'node:test';
import assert from 'node:assert/strict';
import { demoGet, demoPost, demoStatus, finishDemoTopup, mintDemoApproval, mintDemoFollowUps, mintDemoPayment, bumpStats } from './demo.js';

const keys = o => Object.keys(o).sort();
const same = (o, list) => assert.deepEqual(keys(o), [...list].sort());
const B58 = /^[1-9A-HJ-NP-Za-km-z]+$/;
const ISO = /^\d{4}-\d\d-\d\dT\d\d:\d\d:\d\d\.\d{3}Z$/;

const PAYMENT = ['signature', 'slot', 'blockTime', 'payer', 'payee', 'amount', 'amountRaw', 'decimals', 'symbol', 'mint',
  'payerTokenAccount', 'payeeTokenAccount', 'source', 'attackId', 'lagMs', 'prevSignature'];
const ATTESTATION = ['subject', 'badgeId', 'label', 'name', 'kind', 'status', 'pda', 'issuedSig', 'issuedAt', 'revokedSig', 'revokedAt', 'expiresAt', 'settleMode'];
const ATTEMPT = ['id', 'createdAt', 'expiresAt', 'victim', 'attacker', 'displayAmount', 'actualAmount', 'symbol', 'decimals',
  'txBase64', 'messageBase64', 'txBytes', 'txVersion', 'blockhash', 'lastValidBlockHeight', 'decoded', 'outcome', 'signature',
  'deliveredAt', 'resolvedAt', 'warnings'];
const ROUTE = ['txSig', 'time', 'payer', 'payee', 'amount', 'amountRaw', 'hops', 'e2eProof', 'gateway', 'links'];
const HOP = ['position', 'pubkey', 'label', 'verified', 'reward', 'rewardState', 'rewardSig'];
const SETTLEMENT = ['txSig', 'time', 'payee', 'amount', 'status', 'reason', 'nessieDepositId', 'attempts'];
const TOPUP = ['id', 'time', 'pubkey', 'amount', 'status', 'reason', 'nessieWithdrawalId', 'hackSig', 'links'];
const txUrl = sig => `https://explorer.solana.com/tx/${sig}?cluster=devnet`;
const APPROVAL = ['id', 'time', 'rail', 'source', 'payer', 'payee', 'amountCents', 'status', 'reason', 'nessieIds', 'memoSig', 'links'];

function checkApproval(a) {
  same(a, APPROVAL); same(a.payer, ['pubkey']); same(a.payee, ['pubkey', 'name']); same(a.links, ['memo']);
  assert.match(a.time, ISO); assert.match(a.payer.pubkey, B58);
  assert.ok(['nessie', 'solana'].includes(a.rail)); assert.ok(['backend', 'badge_report'].includes(a.source));
  assert.ok(['pending', 'approved', 'blocked', 'failed'].includes(a.status));
  assert.ok(Number.isInteger(a.amountCents) && a.amountCents > 0);
  assert.equal(a.reason == null, a.status === 'approved' || a.status === 'pending');
  assert.ok(Array.isArray(a.nessieIds));
  if (a.memoSig) assert.equal(a.links.memo, `https://explorer.solana.com/tx/${a.memoSig}?cluster=devnet`);
}

function checkRoute(r) {
  same(r, ROUTE); same(r.payer, ['pubkey', 'label']); same(r.payee, ['pubkey', 'name', 'label']); same(r.links, ['tx']);
  assert.match(r.txSig, B58); assert.match(r.time, ISO); assert.equal(r.links.tx, txUrl(r.txSig));
  assert.equal(Number(r.amountRaw), r.amount * 100); assert.equal(typeof r.e2eProof, 'boolean');
  assert.ok(r.hops.length >= 1); assert.equal(r.gateway, r.hops.at(-1).pubkey);
  r.hops.forEach((h, i) => {
    same(h, HOP); assert.equal(h.position, i + 1); assert.match(h.pubkey, B58);
    assert.ok(['none', 'pending', 'paid', 'failed'].includes(h.rewardState));
    assert.equal(h.rewardState === 'none', !h.verified); assert.equal(h.reward, h.verified ? 0.01 : 0);
    assert.equal(h.rewardSig != null, h.rewardState === 'paid');
  });
}
function checkSettlement(s) {
  same(s, SETTLEMENT); same(s.payee, ['pubkey', 'name', 'label']);
  assert.match(s.txSig, B58); assert.match(s.time, ISO); assert.equal(typeof s.amount, 'number'); assert.ok(Number.isInteger(s.attempts));
  assert.ok(['pending', 'settled', 'failed', 'skipped'].includes(s.status));
  assert.equal(s.reason == null, s.status === 'settled' || s.status === 'pending');
  assert.equal(s.nessieDepositId != null, s.status === 'settled');
  if (s.nessieDepositId) assert.match(s.nessieDepositId, /^[0-9a-f]{24}$/);
}
function checkTopup(t) {
  same(t, TOPUP); same(t.links, ['tx']);
  assert.match(t.time, ISO); assert.match(t.pubkey, B58); assert.ok(Number.isInteger(t.amount) && t.amount >= 1 && t.amount <= 1000);
  assert.ok(['pending', 'done', 'failed'].includes(t.status));
  assert.equal(t.reason == null, t.status !== 'failed');
  assert.equal(t.hackSig != null, t.status === 'done'); assert.equal(t.links.tx, t.hackSig ? txUrl(t.hackSig) : null);
}

function checkPayment(p) {
  same(p, PAYMENT);
  same(p.payer, ['pubkey', 'badgeId', 'label']);
  same(p.payee, ['pubkey', 'badgeId', 'label', 'name', 'status']);
  assert.match(p.signature, B58); assert.equal(p.signature.length, 88);
  assert.match(p.blockTime, ISO);
  assert.equal(typeof p.slot, 'number'); assert.equal(typeof p.amount, 'number'); assert.equal(typeof p.amountRaw, 'string');
  assert.equal(Number(p.amountRaw), p.amount * 10 ** p.decimals);
  assert.ok(['verified', 'unverified', 'revoked'].includes(p.payee.status));
  assert.equal(p.source, 'demo');
}

test('status', () => {
  same(demoStatus, ['ok', 'app', 'cluster', 'explorer', 'db', 'rpc', 'ingest', 'token', 'authority', 'registry', 'badgeListener', 'gaps']);
  same(demoStatus.registry, ['program', 'credential', 'schema', 'credentialName', 'schemaName', 'schemaVersion', 'ready']);
  assert.equal(demoStatus.registry.credentialName, 'MHacks Verified Payees'); assert.equal(demoStatus.registry.schemaName, 'payee_v1');
  assert.equal(demoStatus.cluster, 'devnet'); assert.equal(demoStatus.db.ok, true); assert.equal(demoStatus.ingest.state, 'live');
  assert.deepEqual(demoStatus.gaps.map(g => [g.code, g.severity]), [['key_location_unknown', 'warn'], ['attack_delivery_unconfigured', 'warn'], ['attack_outcome_manual', 'info']]);
  for (const g of demoStatus.gaps) same(g, ['code', 'severity', 'badgeGap', 'title', 'fix', 'pages']);
  same(demoGet('status'), keys(demoStatus));
});

test('payments: shape, statuses, newest first, prevSignature chain, paging', () => {
  const { payments } = demoGet('payments');
  assert.equal(payments.length, 25);
  payments.forEach(checkPayment);
  payments.forEach((p, i) => assert.equal(p.prevSignature, payments[i + 1]?.signature ?? null));
  for (let i = 1; i < payments.length; i++) assert.ok(payments[i - 1].blockTime > payments[i].blockTime && payments[i - 1].slot > payments[i].slot);
  for (const s of ['verified', 'unverified', 'revoked']) assert.ok(payments.some(p => p.payee.status === s), s);
  assert.equal(payments.filter(p => p.attackId).length, 1);
  const older = demoGet('payments', { limit: 5, before: payments[9].blockTime }).payments;
  assert.equal(older.length, 5); assert.equal(older[0].signature, payments[10].signature); assert.equal(older[4].prevSignature, null);
});

test('stats and stats/db', () => {
  const s = demoGet('stats');
  same(s, ['window', 'payments', 'volume', 'verifiedShare', 'seedRows', 'lagMs', 'series', 'queryMs']);
  same(s.lagMs, ['p50', 'p95']);
  assert.equal(s.series.length, 61);
  for (const b of s.series) { same(b, ['t', 'n', 'verifiedN', 'volume']); assert.match(b.t, ISO); assert.ok(b.verifiedN <= b.n); }
  const d = demoGet('stats/db');
  same(d, ['timescaledb', 'payments', 'caggs', 'queryMs']);
  same(d.payments, ['rows', 'chunks', 'columnstoreChunks', 'bytesBefore', 'bytesAfter', 'compressionRatio', 'totalBytes']);
  assert.ok(Math.abs(d.payments.compressionRatio - (1 - d.payments.bytesAfter / d.payments.bytesBefore)) < 0.001);
  assert.equal(d.caggs.length, 2);
  for (const c of d.caggs) same(c, ['name', 'realtime', 'scheduleSec', 'lastRefresh', 'lastStatus']);
  same(d.queryMs, ['feed', 'series']);
});

test('badges, attestations, attacks', () => {
  const { badges } = demoGet('badges');
  assert.deepEqual(badges.map(b => b.id), [1, 2, 3, 4]);
  for (const b of badges) {
    same(b, ['id', 'label', 'pubkey', 'tokenAccount', 'keyLocation', 'standIn', 'sol', 'hack', 'attestation', 'received', 'nessie']);
    same(b.attestation, ['status', 'name', 'kind', 'pda', 'issuedSig', 'revokedSig']);
    if (b.nessie) { same(b.nessie, ['accountId', 'usdCents']); assert.match(b.nessie.accountId, /^[0-9a-f]{24}$/); assert.ok(Number.isInteger(b.nessie.usdCents)); }
    same(b.received, ['count', 'amount']);
    assert.match(b.pubkey, B58); assert.equal(b.pubkey.length, 44); assert.ok(b.pubkey.startsWith('Demo'));
    assert.ok(['se050', 'software', 'unknown'].includes(b.keyLocation));
  }
  assert.deepEqual([badges[0].attestation.status, badges[0].attestation.name, badges[1].attestation.status], ['verified', 'MHacks Merch', 'unverified']);
  const { attestations } = demoGet('attestations');
  attestations.forEach(a => { same(a, ATTESTATION); assert.ok(['merchant', 'person', 'relay'].includes(a.kind)); });
  for (const s of ['verified', 'revoked', 'expired']) assert.ok(attestations.some(a => a.status === s), s);
  for (const k of ['merchant', 'person', 'relay']) assert.ok(attestations.some(a => a.kind === k), k);
  assert.deepEqual(badges.map(b => b.nessie == null), [false, true, false, true]);
  const { attempts } = demoGet('attacks');
  attempts.forEach(a => {
    same(a, ATTEMPT); same(a.victim, ['pubkey', 'badgeId', 'label']);
    same(a.decoded, ['program', 'instruction', 'source', 'destination', 'mint', 'authority', 'amount', 'amountRaw', 'decimals']);
  });
  assert.deepEqual(attempts.map(a => a.outcome).sort(), ['pending', 'signed']);
  const signed = attempts.find(a => a.outcome === 'signed');
  assert.equal(demoGet('payments').payments.find(p => p.attackId === signed.id).signature, signed.signature);
});

test('approvals: shape, newest first, every blocked reason, ticker', () => {
  const { approvals } = demoGet('approvals');
  approvals.forEach(checkApproval);
  for (let i = 1; i < approvals.length; i++) assert.ok(approvals[i - 1].time > approvals[i].time);
  const reasons = new Set(approvals.map(a => a.reason));
  for (const r of ['replay', 'bad_sig', 'revoked', 'expired', 'payee_mismatch', 'bad_proof', 'amount_not_whole_dollars', 'not_enrolled', 'stale']) assert.ok(reasons.has(r), r);
  for (const k of ['solana', 'badge_report', 'approved', 'pending', 'failed']) assert.ok(approvals.some(a => [a.rail, a.source, a.status].includes(k)), k);
  assert.equal(demoGet('approvals', { limit: 3 }).approvals.length, 3);
  const a = mintDemoApproval();
  checkApproval(a);
  assert.equal(demoGet('approvals').approvals[0].id, a.id);
});

test('ticker payment joins the head of the chain and bumps stats', () => {
  const before = demoGet('payments').payments[0], stats = demoGet('stats');
  const p = mintDemoPayment();
  checkPayment(p);
  assert.equal(p.prevSignature, before.signature);
  assert.equal(demoGet('payments').payments[0].signature, p.signature);
  assert.equal(demoGet('stats').payments, stats.payments + 1);
  const bumped = bumpStats(stats, p);
  assert.equal(bumped.payments, stats.payments + 1); assert.equal(bumped.volume, stats.volume + p.amount);
  assert.ok(bumped.series.length <= 61); assert.equal(stats.series.reduce((a, b) => a + b.n, 0) + 1, bumped.series.reduce((a, b) => a + b.n, 0));
});

test('POST flows work in memory with contract shapes and error codes', () => {
  const [, impostor, , judgeB] = demoGet('badges').badges;
  const issued = demoPost('/api/attestations', { pubkey: impostor.pubkey, name: 'Totally Legit', kind: 'merchant', nessieRef: 'cust-1' });
  same(issued, ['attestation', 'signature', 'explorerUrl']); same(issued.attestation, ATTESTATION);
  assert.equal(issued.attestation.status, 'verified'); assert.equal(issued.attestation.kind, 'merchant');
  assert.equal(demoGet('badges').badges[1].attestation.kind, 'merchant');
  assert.equal(issued.explorerUrl, `https://explorer.solana.com/tx/${issued.signature}?cluster=devnet`);
  assert.deepEqual(demoGet('badges').badges[1].attestation.status, 'verified');
  assert.equal(demoGet('attestations').attestations[0].subject, impostor.pubkey);

  const revoked = demoPost('/api/attestations/revoke', { pubkey: impostor.pubkey });
  same(revoked, ['attestation', 'signature', 'explorerUrl']);
  assert.equal(revoked.attestation.status, 'revoked'); assert.equal(revoked.attestation.revokedSig, revoked.signature);
  assert.equal(demoGet('badges').badges[1].attestation.status, 'revoked');
  assert.throws(() => demoPost('/api/attestations/revoke', { pubkey: impostor.pubkey }), { code: 'conflict', status: 409 });
  assert.throws(() => demoPost('/api/attestations', { pubkey: 'nope', name: 'x', kind: 'person' }), { code: 'invalid_pubkey', status: 400 });
  assert.throws(() => demoPost('/api/attestations', { pubkey: impostor.pubkey, name: 'café', kind: 'person' }), { code: 'invalid_name' });
  assert.throws(() => demoPost('/api/attestations', { pubkey: impostor.pubkey, name: 'x'.repeat(33), kind: 'person' }), { code: 'invalid_name' });
  assert.throws(() => demoPost('/api/attestations', { pubkey: impostor.pubkey, name: 'x' }), { code: 'invalid_kind' });
  assert.throws(() => demoPost('/api/attestations', { pubkey: impostor.pubkey, name: 'x', kind: 'relay', nessieRef: 'c' }), { code: 'invalid_param' });
  assert.equal(issued.attestation.settleMode, 'hack'); // default for a merchant or person

  // relays: one live relay attestation per operator id
  const relay = demoPost('/api/attestations', { pubkey: judgeB.pubkey, name: 'Relay B', kind: 'relay', operatorId: 'op-b' });
  assert.equal(relay.attestation.kind, 'relay'); assert.ok(!('operatorId' in relay.attestation));
  assert.throws(() => demoPost('/api/attestations', { pubkey: impostor.pubkey, name: 'Relay C', kind: 'relay', operatorId: 'op-b' }), { code: 'conflict', status: 409 });

  // enroll: opens (or links) a Nessie account and the badge reports it
  const e = demoPost('/api/enroll', { pubkey: judgeB.pubkey });
  same(e, ['customerId', 'accountId']);
  assert.deepEqual(demoGet('badges').badges[3].nessie, { accountId: e.accountId, usdCents: 100000 });
  assert.equal(demoPost('/api/enroll', { pubkey: impostor.pubkey, nessieCustomerId: 'c'.repeat(24), nessieAccountId: 'a'.repeat(24) }).accountId, 'a'.repeat(24));
  assert.throws(() => demoPost('/api/enroll', { pubkey: 'nope' }), { code: 'invalid_pubkey' });

  const a = demoPost('/api/attacks', { victim: judgeB.pubkey });
  same(a, ATTEMPT);
  assert.deepEqual([a.displayAmount, a.actualAmount, a.decoded.amount, a.decoded.amountRaw, a.outcome], [5, 500, 500, '50000', 'pending']);
  assert.equal(demoGet('attacks').attempts[0].id, a.id);
  assert.throws(() => demoPost('/api/attacks', { victim: demoStatus.authority.pubkey }), { code: 'unknown_badge', status: 404 });
  assert.throws(() => demoPost('/api/attacks', { victim: judgeB.pubkey, actualAmount: -1 }), { code: 'invalid_amount' });
  assert.throws(() => demoPost('/api/attacks/outcome', { id: a.id, outcome: 'signed' }), { code: 'invalid_param' });
  const r = demoPost('/api/attacks/outcome', { id: a.id, outcome: 'rejected' });
  same(r, ATTEMPT); assert.equal(r.outcome, 'rejected'); assert.match(r.resolvedAt, ISO);
  assert.throws(() => demoPost('/api/attacks/outcome', { id: a.id, outcome: 'rejected' }), { code: 'conflict', status: 409 });
});

test('routes and relay leaderboard: shape, newest first, an unverified hop, every reward state', () => {
  const { routes } = demoGet('routes');
  assert.ok(routes.length >= 3);
  routes.forEach(checkRoute);
  for (let i = 1; i < routes.length; i++) assert.ok(routes[i - 1].time >= routes[i].time);
  assert.ok(routes.some(r => r.hops.length === 2 && r.hops.some(h => !h.verified)), 'a route with an unverified hop');
  for (const st of ['none', 'pending', 'paid', 'failed']) assert.ok(routes.some(r => r.hops.some(h => h.rewardState === st)), st);
  assert.ok(routes.some(r => r.e2eProof) && routes.some(r => !r.e2eProof));
  const sigs = new Set(demoGet('payments', { limit: 200 }).payments.map(p => p.signature));
  for (const r of routes) assert.ok(sigs.has(r.txSig), 'every route is a payment on the chain');
  assert.equal(demoGet('routes', { limit: 2 }).routes.length, 2);

  const { relays } = demoGet('relays/leaderboard');
  assert.ok(relays.length >= 3);
  for (const x of relays) { same(x, ['pubkey', 'label', 'routes', 'earned']); assert.ok(x.routes >= 1); assert.equal(typeof x.earned, 'number'); }
  for (let i = 1; i < relays.length; i++) assert.ok(relays[i - 1].earned >= relays[i].earned);
  const paid = routes.flatMap(r => r.hops).filter(h => h.rewardState === 'paid').reduce((a, h) => a + h.reward, 0);
  assert.ok(Math.abs(relays.reduce((a, x) => a + x.earned, 0) - paid) < 1e-9);
});

test('settlements: shape, newest first, every status, joined to payments by txSig', () => {
  const { settlements } = demoGet('settlements');
  settlements.forEach(checkSettlement);
  for (let i = 1; i < settlements.length; i++) assert.ok(settlements[i - 1].time >= settlements[i].time);
  for (const st of ['pending', 'settled', 'failed', 'skipped']) assert.ok(settlements.some(s => s.status === st), st);
  const skipped = settlements.find(s => s.status === 'skipped');
  assert.equal(skipped.reason, 'amount_not_whole_dollars'); assert.ok(skipped.amount % 1 !== 0);
  const pay = new Map(demoGet('payments', { limit: 200 }).payments.map(p => [p.signature, p]));
  for (const s of settlements) { assert.ok(pay.has(s.txSig)); assert.equal(pay.get(s.txSig).payee.pubkey, s.payee.pubkey); assert.equal(pay.get(s.txSig).amount, s.amount); }
});

test('top-ups: seed shape, POST validation, pending then done or failed', () => {
  const { topups } = demoGet('topups');
  topups.forEach(checkTopup);
  for (const st of ['done', 'failed']) assert.ok(topups.some(t => t.status === st), st);
  const [, impostor, judgeA] = demoGet('badges').badges;
  const before = demoGet('badges').badges[2];
  const t = demoPost('/api/topups', { pubkey: judgeA.pubkey, amount: 25 });
  checkTopup(t); assert.equal(t.status, 'pending');
  assert.equal(demoGet('topups').topups[0].id, t.id);
  const done = finishDemoTopup(t.id);
  checkTopup(done); assert.equal(done.status, 'done'); assert.match(done.nessieWithdrawalId, /^[0-9a-f]{24}$/);
  const after = demoGet('badges').badges[2];
  assert.equal(after.hack, before.hack + 25); assert.equal(after.nessie.usdCents, before.nessie.usdCents - 2500);
  assert.equal(finishDemoTopup(t.id).status, 'done'); // idempotent
  const broke = demoPost('/api/topups', { pubkey: judgeA.pubkey, amount: 1000 });
  demoPost('/api/topups', { pubkey: judgeA.pubkey, amount: 1000 });
  finishDemoTopup(broke.id);
  const last = finishDemoTopup(demoGet('topups').topups[0].id);
  assert.equal(last.status, 'failed'); assert.equal(last.reason, 'insufficient_funds'); checkTopup(last);
  assert.throws(() => demoPost('/api/topups', { pubkey: judgeA.pubkey, amount: 2.5 }), { code: 'invalid_amount' });
  assert.throws(() => demoPost('/api/topups', { pubkey: judgeA.pubkey, amount: 0 }), { code: 'invalid_amount' });
  assert.throws(() => demoPost('/api/topups', { pubkey: judgeA.pubkey, amount: 1001 }), { code: 'invalid_amount' });
  assert.throws(() => demoPost('/api/topups', { pubkey: 'nope', amount: 5 }), { code: 'invalid_pubkey' });
  assert.throws(() => demoPost('/api/topups', { pubkey: demoStatus.authority.pubkey, amount: 5 }), { code: 'unknown_badge', status: 404 });
  if (demoGet('badges').badges[1].nessie == null) assert.throws(() => demoPost('/api/topups', { pubkey: impostor.pubkey, amount: 5 }), { code: 'not_enrolled', status: 409 });
});

test('attestations: settle mode and the Capital One account rules', () => {
  const [, , , judgeB] = demoGet('badges').badges;
  const { attestations } = demoGet('attestations');
  for (const a of attestations) assert.ok(a.kind === 'relay' ? a.settleMode === null : ['hack', 'bank'].includes(a.settleMode));
  assert.equal(attestations.find(a => a.name === 'MHacks Merch').settleMode, 'bank');
  const base = { pubkey: judgeB.pubkey, name: 'Judge B', kind: 'person' };
  assert.throws(() => demoPost('/api/attestations', base), { code: 'invalid_param', message: /Capital One account holders/ });
  assert.throws(() => demoPost('/api/attestations', { ...base, nessieRef: '   ' }), { code: 'invalid_param' });
  assert.throws(() => demoPost('/api/attestations', { ...base, nessieRef: 'c', settleMode: 'bank' }), { code: 'invalid_param', message: /nessieAccountId/ });
  assert.throws(() => demoPost('/api/attestations', { ...base, nessieRef: 'c', settleMode: 'usd' }), { code: 'invalid_param' });
  assert.throws(() => demoPost('/api/attestations', { ...base, kind: 'relay', settleMode: 'hack' }), { code: 'invalid_param' });
  const a = demoPost('/api/attestations', { ...base, kind: 'merchant', nessieRef: 'c', settleMode: 'bank', nessieAccountId: 'a'.repeat(24) }).attestation;
  same(a, ATTESTATION); assert.equal(a.settleMode, 'bank'); assert.ok(!('nessieAccountId' in a));
  assert.equal(demoPost('/api/attestations', { ...base, kind: 'relay' }).attestation.settleMode, null);
});

test('follow-ups: pending work lands, a merchant payment can route and settle', () => {
  const pendingRoute = demoGet('routes').routes.find(r => r.hops.some(h => h.rewardState === 'pending'));
  const pendingSettlement = demoGet('settlements').settlements.find(s => s.status === 'pending');
  let events = [];
  for (let i = 0; i < 40 && !(events.some(e => e.type === 'route' && e.data.txSig !== pendingRoute?.txSig)); i++) events.push(...mintDemoFollowUps(mintDemoPayment()));
  for (const e of events) {
    assert.ok(['route', 'settlement'].includes(e.type));
    if (e.type === 'route') checkRoute(e.data); else checkSettlement(e.data);
  }
  if (pendingRoute) {
    const r = demoGet('routes', { limit: 200 }).routes.find(x => x.txSig === pendingRoute.txSig);
    assert.ok(r.hops.every(h => h.rewardState === 'paid' || h.rewardState === 'none'));
  }
  if (pendingSettlement) assert.equal(demoGet('settlements', { limit: 200 }).settlements.find(s => s.txSig === pendingSettlement.txSig).status, 'settled');
  assert.ok(events.some(e => e.type === 'route'), 'a new route within 40 payments');
  demoGet('routes').routes.forEach(checkRoute); demoGet('settlements').settlements.forEach(checkSettlement);
});
