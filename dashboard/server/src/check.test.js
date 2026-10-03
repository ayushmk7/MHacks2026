import test from 'node:test';
import assert from 'node:assert/strict';
import { parseTransfer } from './solana.js';
import { validate } from './http.js';
import { splitSql } from '../../scripts/db.mjs';

const MINT = 'So11111111111111111111111111111111111111112';
const [PAYER, PAYEE, SRC, DST] = ['PayerWallet', 'PayeeWallet', 'SrcTokenAcct', 'DstTokenAcct'];
const tx = (over = {}) => ({
  slot: 412345678n, blockTime: 1790000000n,
  meta: {
    err: null,
    preTokenBalances: [{ accountIndex: 1, mint: MINT, owner: PAYER }],
    postTokenBalances: [{ accountIndex: 1, mint: MINT, owner: PAYER }, { accountIndex: 2, mint: MINT, owner: PAYEE }],
    ...over.meta,
  },
  transaction: { message: {
    accountKeys: [PAYER, SRC, DST, MINT].map(pubkey => ({ pubkey })),
    instructions: over.instructions ?? [{ program: 'spl-token', parsed: { type: 'transferChecked', info: {
      authority: PAYER, source: SRC, destination: DST, mint: MINT,
      tokenAmount: { amount: '1000', decimals: 2n, uiAmount: 10, uiAmountString: '10' } } } }],
  } },
});

test('parseTransfer reads payer, payee, amount and decimals from a jsonParsed transferChecked', () => {
  assert.deepEqual(parseTransfer(tx(), MINT), {
    payer: PAYER, payee: PAYEE, payerTokenAccount: SRC, payeeTokenAccount: DST,
    amountRaw: '1000', decimals: 2, slot: 412345678, blockTime: 1790000000,
  });
});

test('parseTransfer ignores failed transactions and transactions without our transferChecked', () => {
  assert.equal(parseTransfer(tx({ meta: { err: { InstructionError: [0, 'Custom'] } } }), MINT), null);
  assert.equal(parseTransfer(tx({ instructions: [{ program: 'spl-token', parsed: { type: 'mintTo', info: { mint: MINT } } }] }), MINT), null);
  assert.equal(parseTransfer(tx(), 'SomeOtherMint'), null);
  assert.equal(parseTransfer(null, MINT), null);
});

test('validate.name: 1 to 32 printable ASCII characters', () => {
  assert.equal(validate.name(' MHacks Merch '), 'MHacks Merch');
  for (const bad of ['', '   ', 'x'.repeat(33), 'café', 42, null]) assert.throws(() => validate.name(bad), { code: 'invalid_name' });
});

test('validate.pubkey, amount and limit reject bad input', () => {
  assert.equal(validate.pubkey(MINT), MINT);
  assert.throws(() => validate.pubkey('nope'), { code: 'invalid_pubkey' });
  assert.equal(validate.amount(0.07, 2), 7);
  assert.equal(validate.amount(500, 2), 50000);
  for (const bad of [0, -1, 1_000_001, 0.001, 1e-9, '5', NaN, Infinity]) assert.throws(() => validate.amount(bad, 2), { code: 'invalid_amount' });
  assert.equal(validate.limit(null, 40), 40);
  assert.equal(validate.limit('200', 40), 200);
  for (const bad of ['0', '201', '1.5', 'abc', '']) assert.throws(() => validate.limit(bad, 40), { code: 'invalid_param' });
});

test('splitSql keeps a $$ block whole and splits plain statements', () => {
  assert.deepEqual(splitSql('a; b;'), ['a', 'b']);
  assert.deepEqual(splitSql('-- comment; here\nSELECT 1;\nDO $$ BEGIN PERFORM 1; PERFORM 2; END $$;\nSELECT 2'),
    ['SELECT 1', 'DO $$ BEGIN PERFORM 1; PERFORM 2; END $$', 'SELECT 2']);
});
