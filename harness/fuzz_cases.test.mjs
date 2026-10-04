// R6 fuzz set: every refuse case must be rejected by the reference rules it targets, every control
// accepted, so a badge "passing" R6 means its decoder refused inputs that really are invalid.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { buildCases, reference, referenceSolana, exportCase, compactU16, TEST_PAYER, TEST_MINT, MAX_SERVED, MAX_MESSAGE } from './fuzz_cases.mjs';

const ctx = { payer: TEST_PAYER, mint: TEST_MINT, decimals: 2 };
const cases = buildCases(ctx);

test('every fuzz input is rejected by its reference, every control accepted', () => {
  for (const c of cases) {
    const r = reference(c, ctx);
    assert.equal(r.ok, c.expect !== 'refuse', `${c.id} ${c.name}: reference says ${r.detail ?? 'ok'}`);
  }
});

test('ids unique, inputs servable, every spec group present', () => {
  assert.equal(new Set(cases.map(c => c.id)).size, cases.length);
  for (const c of cases) for (const a of c.args) if (a.t === 'b') assert.ok(a.v.length <= MAX_SERVED, c.id);
  const groups = new Set(cases.filter(c => c.fn === 'begin_solana').map(c => c.group));
  for (const g of ['malformed', 'oversized', 'wrong-prefix', 'two-signer', 'versioned', 'wrong-mint', 'trailing']) assert.ok(groups.has(g), g);
  for (const fn of ['begin_solana', 'sign_request', 'sign_proof', 'begin_bank']) {
    assert.ok(cases.some(c => c.fn === fn && c.expect !== 'refuse'), `${fn} has a control`);
  }
});

test('the 1232 B boundary: one memo byte fewer than the 1233 B case decodes', () => {
  const big = cases.find(c => c.name.startsWith('1233 B well-formed')).args[0].v;
  assert.equal(big.length, MAX_MESSAGE + 1);
  assert.equal(referenceSolana(big, ctx).detail, 'oversized');
  let n = 0;                                   // the memo is the last instruction: 'x' * n at the end
  while (big[big.length - 1 - n] === 0x78) n++;
  const lenAt = big.length - n - 2;            // its 2-byte compact-u16 length
  assert.deepEqual([...big.subarray(lenAt, lenAt + 2)], [...compactU16(n)]);
  const atLimit = Buffer.concat([big.subarray(0, lenAt), compactU16(n - 1), big.subarray(lenAt + 2, big.length - 1)]);
  assert.equal(atLimit.length, MAX_MESSAGE);
  assert.equal(referenceSolana(atLimit, ctx).ok, true);
});

test('deterministic, and harness/fuzz-vectors.json is current', () => {
  const again = buildCases(ctx).map(exportCase);
  assert.deepEqual(again, cases.map(exportCase));
  const file = fileURLToPath(new URL('./fuzz-vectors.json', import.meta.url));
  const saved = JSON.parse(readFileSync(file, 'utf8'));
  assert.deepEqual(saved.cases, again, 'regenerate: node harness/r6_fuzz.mjs --export harness/fuzz-vectors.json');
});
