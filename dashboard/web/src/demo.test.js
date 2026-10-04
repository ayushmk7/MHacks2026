// node --test web/src/demo.test.js
// The one runnable check for the web side: DEMO fixtures and the fake backend match the API contract (PLAN 4.4).
import test from 'node:test';
import assert from 'node:assert/strict';
import { demoGet, demoPost, demoStatus, mintDemoApproval, mintDemoPayment, bumpStats } from './demo.js';

const keys = o => Object.keys(o).sort();
const same = (o, list) => assert.deepEqual(keys(o), [...list].sort());
const B58 = /^[1-9A-HJ-NP-Za-km-z]+$/;
const ISO = /^\d{4}-\d\d-\d\dT\d\d:\d\d:\d\d\.\d{3}Z$/;

const PAYMENT = ['signature', 'slot', 'blockTime', 'payer', 'payee', 'amount', 'amountRaw', 'decimals', 'symbol', 'mint',
  'payerTokenAccount', 'payeeTokenAccount', 'source', 'attackId', 'lagMs', 'prevSignature'];
const ATTESTATION = ['subject', 'badgeId', 'label', 'name', 'kind', 'status', 'pda', 'issuedSig', 'issuedAt', 'revokedSig', 'revokedAt', 'expiresAt'];
const ATTEMPT = ['id', 'createdAt', 'expiresAt', 'victim', 'attacker', 'displayAmount', 'actualAmount', 'symbol', 'decimals',
  'txBase64', 'messageBase64', 'txBytes', 'txVersion', 'blockhash', 'lastValidBlockHeight', 'decoded', 'outcome', 'signature',
  'deliveredAt', 'resolvedAt', 'warnings'];
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
