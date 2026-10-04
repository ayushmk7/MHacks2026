import test from 'node:test';
import assert from 'node:assert/strict';
import { issueParams } from './registry.js';

// Offline: issueParams is the rule set issue() applies before it touches the chain or the DB.
const BADGE = '9xQeWvG816bUx9EPjHmaT23yvVM2ZWbrrpZb9PusVFin';
const WALLET = 'So11111111111111111111111111111111111111112';
const ISSUER = 'Vote111111111111111111111111111111111111111';
const params = (o, wallet = ISSUER) => issueParams(BADGE, { name: 'MHacks Merch', ...o }, wallet);

test('a merchant or person needs a Capital One reference, a relay does not', () => {
  for (const kind of ['merchant', 'person']) {
    for (const nessieRef of [undefined, null, '', '  ']) {
      assert.throws(() => params({ kind, nessieRef }), { code: 'invalid_param', message: /only Capital One account holders can be verified/ });
    }
    assert.deepEqual(params({ kind, nessieRef: 'ref-1' }), { solanaWallet: BADGE, bankRef: 'ref-1', settleMode: 'hack', nessieAccountId: null });
  }
  assert.deepEqual(params({ kind: 'relay' }), { solanaWallet: BADGE, bankRef: null, settleMode: 'hack', nessieAccountId: null });
  assert.equal(params({ kind: 'relay', nessieRef: 'ignored' }).bankRef, null);   // unchanged: a relay carries no bank reference
});

test('settleMode hack keeps the given wallet, or the badge key by default', () => {
  assert.equal(params({ kind: 'merchant', nessieRef: 'm', solanaWallet: WALLET }).solanaWallet, WALLET);
  assert.equal(params({ kind: 'merchant', nessieRef: 'm', settleMode: 'hack', nessieAccountId: 'acct-1' }).nessieAccountId, null);
  assert.throws(() => params({ kind: 'merchant', nessieRef: 'm', solanaWallet: 'nope' }), { code: 'invalid_param' });
});

test('settleMode bank attests the issuer settlement wallet and needs the deposit account', () => {
  assert.deepEqual(params({ kind: 'merchant', nessieRef: 'm', settleMode: 'bank', nessieAccountId: 'acct-1', solanaWallet: WALLET }),
    { solanaWallet: ISSUER, bankRef: 'm', settleMode: 'bank', nessieAccountId: 'acct-1' });
  assert.equal(params({ kind: 'person', nessieRef: 'a', settleMode: 'bank', nessieAccountId: 'a' }).solanaWallet, ISSUER);
  for (const nessieAccountId of [undefined, '', '../x', 'a'.repeat(65), 5]) {
    assert.throws(() => params({ kind: 'merchant', nessieRef: 'm', settleMode: 'bank', nessieAccountId }), { code: 'invalid_param' });
  }
  assert.throws(() => params({ kind: 'relay', settleMode: 'bank', nessieAccountId: 'a' }), { code: 'invalid_param', message: /relay/ });
  assert.throws(() => params({ kind: 'merchant', nessieRef: 'm', settleMode: 'usd' }), { code: 'invalid_param' });
  assert.throws(() => params({ kind: 'merchant', nessieRef: 'm', settleMode: 'bank', nessieAccountId: 'a' }, null), { code: 'not_configured' });
});

test('existing checks still come first', () => {
  assert.throws(() => issueParams('nope', { name: 'x', kind: 'merchant', nessieRef: 'm' }, ISSUER), { code: 'invalid_pubkey' });
  assert.throws(() => params({ kind: 'shop', nessieRef: 'm' }), { code: 'invalid_param' });
  assert.throws(() => params({ kind: 'merchant', nessieRef: 'm', name: 'é' }), { code: 'invalid_name' });
});
