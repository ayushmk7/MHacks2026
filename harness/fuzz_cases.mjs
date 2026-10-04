// R6 message fuzz set (docs/specs/P1-r-test-harness.md R6): inputs that A's decoder and wallet.sign_*
// functions must refuse, plus a few controls they must accept, served to a badge by harness/r6_fuzz.mjs.
// Node built-ins only, so it runs from harness/ without the dashboard's node_modules.
//
// The set is checked against a reference implementation of the rules it targets, so a case can never
// "pass" just because the generator built something accidentally valid:
//   referenceSolana  P1-A §4.2 (legacy message, one signer, one transferChecked + optional memo, pinned mint)
//   referenceReq     00 §5 REQ frame, header to name (the bytes sign_request covers)
//   referenceProof   00 §4 sign_proof argument sizes
//   referenceBank    00 §6 / P2-A §3.2 bank payload
// fuzz_cases.test.mjs asserts every `refuse` case is rejected by its reference and every control is accepted.
//
// Each case: { id, fn, group, name, args, expect, reason, basis, ref }
//   fn      wallet function the badge calls: begin_solana | begin_bank | sign_request | sign_proof
//   args    [{ t, v }]: t = 'b' bytes (Buffer) | 'n' number | 'nil' | 'T' empty table | 'B' boolean
//   expect  'refuse' for fuzz inputs; 'screen' (begin_* returns true) or 'signed' for controls
//   reason  the refusal reason the spec names, when it names one ('undecodable' for begin_*)
//   basis   'spec' = the rule is written in 00 / P1-A / P2-A; 'interp' = R's reading, agree with A
//   ref     why the reference rejects it (reference decoder's detail string)
// ── base58 (Solana addresses), so this file needs no @solana/kit ──────────────
const B58 = '123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz';

export function b58encode(bytes) {
  let n = 0n;
  for (const b of bytes) n = n * 256n + BigInt(b);
  let out = '';
  while (n > 0n) { out = B58[Number(n % 58n)] + out; n /= 58n; }
  for (const b of bytes) { if (b !== 0) break; out = '1' + out; }
  return out;
}

// A base58 Solana address as its 32 bytes; throws on anything that is not exactly 32 bytes.
export function addressBytes(text) {
  if (typeof text !== 'string' || !/^[1-9A-HJ-NP-Za-km-z]{32,44}$/.test(text)) throw new Error(`not an address: ${text}`);
  let n = 0n;
  for (const ch of text) n = n * 58n + BigInt(B58.indexOf(ch));
  const body = [];
  while (n > 0n) { body.unshift(Number(n % 256n)); n /= 256n; }
  const zeros = text.length - text.replace(/^1+/, '').length;
  const bytes = Buffer.from([...new Array(zeros).fill(0), ...body]);
  if (bytes.length !== 32) throw new Error(`not an address: ${text}`);
  return bytes;
}

export function isAddress(text) {
  try { addressBytes(text); return true; } catch { return false; }
}

export const MAX_MESSAGE = 1232;       // Solana packet limit (P1-A §4.2)
export const MAX_SERVED = 8192;        // biggest input served over HTTP; past 1233 B only the size matters
export const PREFIXES = ['pay-req:', 'pay-proof:', 'bank-auth:', 'registry:'];   // 00 §3

const TOKEN_PROGRAM = 'TokenkegQfeZyiNwAJbNbGKPFXCWuBvf9Ss623VQ5DA';
const TOKEN_2022 = 'TokenzQdBNbLqP5VEhdkAS6EPFLC1PHnBqCXEpPxuEb';
const MEMO_PROGRAM = 'MemoSq4gqABAXKb96qnH8TysNcWxMyWCqXgDLGmfcHr';
const SYSTEM_PROGRAM = '11111111111111111111111111111111';
const TRANSFER_CHECKED = 12, TRANSFER = 3;

// Fixed TEST values for the exported vector file (A's host tests); a live run uses the badge and HACK_MINT.
export const TEST_PAYER = b58encode(new Uint8Array(32).fill(0x11));
export const TEST_MINT = b58encode(new Uint8Array(32).fill(0x22));

const key = a => addressBytes(a);
const u64 = n => { const b = Buffer.alloc(8); b.writeBigUInt64LE(BigInt(n)); return b; };
const u32 = n => { const b = Buffer.alloc(4); b.writeUInt32LE(n); return b; };

export function compactU16(n) {
  const out = [];
  for (let v = n; ; ) {
    const b = v & 0x7f; v >>>= 7;
    if (v) out.push(b | 0x80); else { out.push(b); break; }
  }
  return Buffer.from(out);
}

// Seeded PRNG so the set is identical on every run and in the exported vectors.
function mulberry32(seed) {
  return () => {
    seed = (seed + 0x6d2b79f5) | 0;
    let t = Math.imul(seed ^ (seed >>> 15), 1 | seed);
    t = (t + Math.imul(t ^ (t >>> 7), 61 | t)) ^ t;
    return ((t ^ (t >>> 14)) >>> 0) / 4294967296;
  };
}

// ── Solana message serializer (legacy, or v0 with `version: 0`) ───────────────
// Raw overrides (`countBytes`, `ixCountBytes`) let a case write a non-canonical or lying length.
export function serializeMessage({ header, keys, blockhash, ixs, version, countBytes, ixCountBytes }) {
  const parts = [];
  if (version === 0) parts.push(Buffer.from([0x80]));
  parts.push(Buffer.from(header));
  parts.push(countBytes ?? compactU16(keys.length), ...keys);
  parts.push(blockhash);
  parts.push(ixCountBytes ?? compactU16(ixs.length));
  for (const ix of ixs) {
    parts.push(Buffer.from([ix.prog]), ix.accCountBytes ?? compactU16(ix.accts.length), Buffer.from(ix.accts),
      ix.lenBytes ?? compactU16(ix.data.length), Buffer.from(ix.data));
  }
  if (version === 0) parts.push(compactU16(0));   // no address table lookups
  return Buffer.concat(parts);
}

// ── reference decoder: P1-A §4.2 ─────────────────────────────────────────────
// Returns { ok, reason: 'undecodable', detail } or { ok: true, fields }.
export function referenceSolana(bytes, { payer, mint, decimals }) {
  const msg = Buffer.from(bytes);
  const bad = detail => ({ ok: false, reason: 'undecodable', detail });
  if (msg.length > MAX_MESSAGE) return bad('oversized');
  for (const p of PREFIXES) if (msg.subarray(0, p.length).toString('latin1') === p) return bad('reserved_prefix');
  if (msg.length === 0) return bad('truncated');
  if (msg[0] & 0x80) return bad('versioned');

  let pos = 0;
  const need = n => { if (pos + n > msg.length) throw 'truncated'; };
  const byte = () => { need(1); return msg[pos++]; };
  const take = n => { need(n); const b = msg.subarray(pos, pos + n); pos += n; return b; };
  const cu16 = () => {   // canonical short_vec, as Solana's sanitizer requires
    let v = 0;
    for (let i = 0; i < 3; i++) {
      const b = byte();
      v |= (b & 0x7f) << (7 * i);
      if (!(b & 0x80)) {
        if (i > 0 && b === 0) throw 'compact_u16';   // non-minimal encoding
        if (v > 0xffff) throw 'compact_u16';
        return v;
      }
    }
    throw 'compact_u16';
  };

  try {
    const [required, roSigned, roUnsigned] = [byte(), byte(), byte()];
    if (required !== 1) return bad('signer_count');
    if (roSigned !== 0) return bad('header');               // the fee payer must be writable
    const nKeys = cu16();
    if (nKeys < 1 || required + roUnsigned > nKeys) return bad('header');
    const keys = [];
    for (let i = 0; i < nKeys; i++) keys.push(take(32));
    if (!keys[0].equals(key(payer))) return bad('payer');
    take(32);                                                // recent blockhash
    const nIx = cu16();
    const ixs = [];
    for (let i = 0; i < nIx; i++) {
      const prog = byte();
      const nAcc = cu16();
      const accts = [...take(nAcc)];
      const data = take(cu16());
      if (prog >= nKeys || accts.some(a => a >= nKeys)) return bad('index');
      ixs.push({ program: keys[prog], accts, data });
    }
    if (pos !== msg.length) return bad('trailing');

    const token = ixs.filter(ix => ix.program.equals(key(TOKEN_PROGRAM)));
    const memos = ixs.filter(ix => ix.program.equals(key(MEMO_PROGRAM)));
    if (token.length !== 1 || memos.length > 1 || token.length + memos.length !== ixs.length) return bad('instructions');
    const t = token[0];
    if (t.data.length !== 10 || t.data[0] !== TRANSFER_CHECKED) return bad('not_transfer_checked');
    if (t.accts.length !== 4) return bad('accounts');
    if (t.accts[3] !== 0) return bad('owner');
    if (!keys[t.accts[1]].equals(key(mint))) return bad('mint');
    if (t.data[9] !== decimals) return bad('decimals');
    let memo = null;
    if (memos.length) {
      if (memos[0].accts.length !== 0) return bad('memo_accounts');
      try { memo = new TextDecoder('utf-8', { fatal: true }).decode(memos[0].data); } catch { return bad('memo_utf8'); }
    }
    return { ok: true, reason: 'ok', fields: { amount: t.data.readBigUInt64LE(1), source: keys[t.accts[0]], destination: keys[t.accts[2]], memo } };
  } catch (e) {
    if (typeof e === 'string') return bad(e);
    throw e;
  }
}

// ── reference REQ (sign_request input: 00 §5 frame from header to name, no sig) ─
const REQ_FIXED = 4 + 1 + 32 + 8 + 4 + 8 + 4 + 1;
const CURRENCY = { 1: 'HACK', 2: 'USD\0' };
export function referenceReq(bytes, { payer }) {
  const f = Buffer.from(bytes);
  const bad = detail => ({ ok: false, reason: 'refused', detail });
  if (f.length < REQ_FIXED) return bad('truncated');
  if (f[0] !== 0x56 || f[1] !== 0x4b) return bad('magic');
  if (f[2] !== 1) return bad('version');
  if (f[3] !== 1) return bad('type');
  const nameLen = f[61];
  if (nameLen > 32) return bad('name_len');
  if (f.length !== REQ_FIXED + nameLen) return bad(f.length < REQ_FIXED + nameLen ? 'truncated' : 'trailing');
  const rail = f[4];
  if (CURRENCY[rail] === undefined || f.subarray(45, 49).toString('latin1') !== CURRENCY[rail]) return bad('rail_currency');
  if (f.readBigUInt64LE(37) === 0n) return bad('amount');
  if (!f.subarray(5, 37).equals(key(payer))) return bad('payee_not_self');
  if (!/^[\x20-\x7e]*$/.test(f.subarray(REQ_FIXED).toString('latin1'))) return bad('name');
  return { ok: true, reason: 'ok' };
}

// ── reference sign_proof arguments (00 §4: req_id[8], nonce[16], payer_pubkey[32]) ─
export function referenceProof(args) {
  const sizes = [8, 16, 32];
  for (let i = 0; i < 3; i++) {
    const a = args[i];
    if (!a || a.t !== 'b') return { ok: false, reason: 'refused', detail: `arg${i + 1}_type` };
    if (a.v.length !== sizes[i]) return { ok: false, reason: 'refused', detail: `arg${i + 1}_length` };
  }
  return { ok: true, reason: 'ok' };
}

// ── reference bank payload (00 §6, P2-A §3.2) ─────────────────────────────────
const BANK_KEYS = ['v', 'rail', 'action', 'amount_cents', 'currency', 'from_acct', 'payee_name',
  'payee_ref', 'attestation', 'req_id', 'proof_nonce', 'issued_at'];
export function referenceBank(bytes) {
  const bad = detail => ({ ok: false, reason: 'undecodable', detail });
  const b = Buffer.from(bytes);
  if (b.length > MAX_MESSAGE) return bad('oversized');
  for (const p of PREFIXES) if (b.subarray(0, p.length).toString('latin1') === p) return bad('reserved_prefix');
  let text;
  try { text = new TextDecoder('utf-8', { fatal: true }).decode(b); } catch { return bad('utf8'); }
  const lines = text.split('\n');
  if (lines.length !== BANK_KEYS.length) return bad('line_count');
  const f = {};
  for (let i = 0; i < BANK_KEYS.length; i++) {
    if (!lines[i].startsWith(`${BANK_KEYS[i]}=`)) return bad('key_order');
    f[BANK_KEYS[i]] = lines[i].slice(BANK_KEYS[i].length + 1);
  }
  const printable = (v, max) => v.length > 0 && v.length <= max && /^[\x20-\x7e]*$/.test(v) && v.trim() === v;
  const ok = f.v === '1' && f.rail === 'nessie' && (f.action === 'purchase' || f.action === 'transfer')
    && /^[1-9][0-9]{0,11}$/.test(f.amount_cents) && f.currency === 'USD'
    && printable(f.from_acct, 64) && printable(f.payee_name, 32) && /^[0-9a-f]{64}$/.test(f.payee_ref)
    && printable(f.attestation, 64) && /^[0-9a-f]{16}$/.test(f.req_id) && /^[0-9a-f]{32}$/.test(f.proof_nonce)
    && /^[1-9][0-9]{0,10}$/.test(f.issued_at);
  return ok ? { ok: true, reason: 'ok' } : bad('field');
}

export function reference(c, ctx) {
  if (c.fn === 'begin_solana') return c.args[0]?.t === 'b' ? referenceSolana(c.args[0].v, ctx) : { ok: false, detail: 'arg_type' };
  if (c.fn === 'begin_bank') return c.args[0]?.t === 'b' ? referenceBank(c.args[0].v) : { ok: false, detail: 'arg_type' };
  if (c.fn === 'sign_request') return c.args[0]?.t === 'b' ? referenceReq(c.args[0].v, ctx) : { ok: false, detail: 'arg_type' };
  if (c.fn === 'sign_proof') return referenceProof(c.args);
  throw new Error(`unknown fn ${c.fn}`);
}

// ── the set ──────────────────────────────────────────────────────────────────
// `source` / `destination` only need to be distinct 32-byte keys: the decoder compares the destination to
// the registry record, which R6 does not supply, so real ATAs are not needed here.
export function buildCases({ payer, mint, decimals = 2, seed = 0x5236 }) {
  const rand = mulberry32(seed);
  const bytesOf = n => Buffer.from(Array.from({ length: n }, () => Math.floor(rand() * 256)));
  const P = key(payer), M = key(mint), TOK = key(TOKEN_PROGRAM), MEMO = key(MEMO_PROGRAM);
  const SRC = Buffer.alloc(32, 0x5a), DST = Buffer.alloc(32, 0xd5), OTHER = Buffer.alloc(32, 0x0e);
  const BH = Buffer.alloc(32, 0xb1);
  const amount = 100n;

  const xfer = (o = {}) => ({ prog: 4, accts: o.accts ?? [1, 3, 2, 0],
    data: o.data ?? Buffer.concat([Buffer.from([TRANSFER_CHECKED]), u64(amount), Buffer.from([o.decimals ?? decimals])]) });
  const memoIx = (text = 'r6', accts = []) => ({ prog: 5, accts, data: Buffer.isBuffer(text) ? text : Buffer.from(text) });
  // Base layout matches what @solana/kit compiles for R3: payer | source, dest (writable) | mint, token, memo.
  const spec = (o = {}) => ({
    header: o.header ?? [1, 0, o.memo === false ? 2 : 3],
    keys: o.keys ?? (o.memo === false ? [P, SRC, DST, M, TOK] : [P, SRC, DST, M, TOK, MEMO]),
    blockhash: BH, ixs: o.ixs ?? (o.memo === false ? [xfer()] : [xfer(), memoIx()]),
    version: o.version, countBytes: o.countBytes, ixCountBytes: o.ixCountBytes,
  });
  const msg = o => serializeMessage(spec(o));
  const good = msg({ memo: false }), goodMemo = msg();

  const cases = [];
  const add = (fn, group, name, args, expect = 'refuse', basis = 'spec') => cases.push({ fn, group, name, args, expect, basis });
  const sol = (group, name, bytes, basis) => add('begin_solana', group, name, [{ t: 'b', v: bytes }, { t: 'T' }], 'refuse', basis);

  // controls: the harness only proves something if valid input reaches the screen
  add('begin_solana', 'control', 'valid transferChecked', [{ t: 'b', v: good }, { t: 'T' }], 'screen');
  add('begin_solana', 'control', 'valid transferChecked + memo', [{ t: 'b', v: goodMemo }, { t: 'T' }], 'screen');

  // two-signer / wrong signer / wrong owner
  sol('two-signer', 'num_required_signatures = 2 (a second signer key)', msg({ header: [2, 0, 2], keys: [P, OTHER, SRC, DST, M, TOK],
    ixs: [{ ...xfer(), accts: [2, 4, 3, 0], prog: 5 }] }));
  sol('two-signer', 'num_required_signatures = 2 (memo program as 2nd)', goodMemo.map((b, i) => (i === 0 ? 2 : b)));
  sol('two-signer', 'num_required_signatures = 0', goodMemo.map((b, i) => (i === 0 ? 0 : b)));
  sol('two-signer', 'num_required_signatures = 127', goodMemo.map((b, i) => (i === 0 ? 127 : b)));
  sol('two-signer', 'account 0 is not this badge', msg({ keys: [OTHER, SRC, DST, M, TOK, MEMO] }));
  sol('two-signer', 'token owner is not account 0', msg({ ixs: [xfer({ accts: [1, 3, 2, 1] }), memoIx()] }));
  sol('two-signer', 'fee payer read-only (num_readonly_signed = 1)', msg({ header: [1, 1, 3] }));

  // header consistency
  sol('header', 'num_readonly_unsigned > account count', msg({ header: [1, 0, 9] }));
  sol('header', 'zero account keys', Buffer.concat([Buffer.from([1, 0, 0, 0]), BH, compactU16(0)]));

  // versioned
  sol('versioned', 'v0 message (0x80 prefix), otherwise identical', msg({ version: 0 }));
  sol('versioned', 'v1 marker (0x81)', Buffer.concat([Buffer.from([0x81]), goodMemo]));
  sol('versioned', 'first byte 0xff', Buffer.concat([Buffer.from([0xff]), goodMemo.subarray(1)]));

  // wrong prefix: every reserved domain, bare and in front of a valid message
  const reqFrame = (o = {}) => Buffer.concat([Buffer.from(o.magic ?? 'VK', 'latin1'), Buffer.from([o.version ?? 1, o.type ?? 1, o.rail ?? 1]),
    o.payee ?? P, u64(o.amount ?? 400), Buffer.from(o.currency ?? 'HACK', 'latin1'), Buffer.alloc(8, 0x77), u32(o.expiry ?? 1_900_000_000),
    Buffer.from([o.nameLen ?? Buffer.byteLength(o.name ?? 'Coffee')]), Buffer.from(o.name ?? 'Coffee', 'latin1'), o.tail ?? Buffer.alloc(0)]);
  const bankText = (o = {}) => [
    'v=1', 'rail=nessie', 'action=purchase', `amount_cents=${o.amount ?? '4000'}`, 'currency=USD', 'from_acct=5f2a91c0d4e8b7a6', 'payee_name=Coffee Cart',
    `payee_ref=${'ab'.repeat(32)}`, 'attestation=none', `req_id=${'77'.repeat(8)}`, `proof_nonce=${'c3'.repeat(16)}`, `issued_at=${o.issuedAt ?? '1790000000'}`,
  ];
  const bank = (lines = bankText()) => Buffer.from(lines.join('\n'), 'utf8');
  const registry = Buffer.from(['v=1', 'attestation=none', 'display_name=Coffee Cart', `device_pubkey=${P.toString('hex')}`, 'kind=merchant',
    'solana_wallet=', 'solana_ata=', 'bank_ref_hash=', 'expiry=1900000000', 'status=active', 'issued_at=1790000000'].join('\n'));
  sol('wrong-prefix', '"pay-req:" + REQ frame', Buffer.concat([Buffer.from('pay-req:'), reqFrame()]));
  sol('wrong-prefix', '"pay-proof:" + req_id, nonce, payer', Buffer.concat([Buffer.from('pay-proof:'), Buffer.alloc(8, 7), Buffer.alloc(16, 3), P]));
  sol('wrong-prefix', '"bank-auth:" + bank payload', Buffer.concat([Buffer.from('bank-auth:'), bank()]));
  sol('wrong-prefix', '"registry:" + registry record', Buffer.concat([Buffer.from('registry:'), registry]));
  for (const p of PREFIXES) sol('wrong-prefix', `"${p}" + valid message`, Buffer.concat([Buffer.from(p), goodMemo]));
  sol('wrong-prefix', 'bare bank payload (starts "v=1")', bank());
  sol('wrong-prefix', 'REQ frame without prefix (starts "VK")', reqFrame());

  // wrong mint / decimals / program
  sol('wrong-mint', 'mint is another key', msg({ keys: [P, SRC, DST, OTHER, TOK, MEMO] }));
  sol('wrong-mint', 'mint account index points at destination', msg({ ixs: [xfer({ accts: [1, 2, 2, 0] }), memoIx()] }));
  sol('wrong-mint', `decimals ${decimals + 4} instead of ${decimals}`, msg({ ixs: [xfer({ decimals: decimals + 4 }), memoIx()] }));
  sol('wrong-mint', 'decimals 0', msg({ ixs: [xfer({ decimals: 0 }), memoIx()] }));
  sol('wrong-mint', 'Token-2022 program instead of SPL Token', msg({ keys: [P, SRC, DST, M, key(TOKEN_2022), MEMO] }));
  sol('wrong-mint', 'System program instead of SPL Token', msg({ keys: [P, SRC, DST, M, key(SYSTEM_PROGRAM), MEMO] }));

  // instruction shape
  const T = xfer(), m = memoIx();
  sol('instructions', 'zero instructions', msg({ ixs: [] }));
  sol('instructions', 'two transferChecked', msg({ ixs: [T, T] }));
  sol('instructions', 'transferChecked + memo + memo', msg({ ixs: [T, m, m] }));
  sol('instructions', 'memo only', msg({ ixs: [m] }));
  sol('instructions', 'second non-memo instruction (System program)', msg({ keys: [P, SRC, DST, M, TOK, key(SYSTEM_PROGRAM)],
    ixs: [T, { prog: 5, accts: [0, 1], data: Buffer.concat([u32(2), u64(1)]) }] }));
  sol('instructions', 'unchecked Transfer (discriminator 3)', msg({ ixs: [{ prog: 4, accts: [1, 2, 0], data: Buffer.concat([Buffer.from([TRANSFER]), u64(amount)]) }, m] }));
  sol('instructions', 'transferChecked data 9 B (no decimals)', msg({ ixs: [xfer({ data: T.data.subarray(0, 9) }), m] }));
  sol('instructions', 'transferChecked data 11 B', msg({ ixs: [xfer({ data: Buffer.concat([T.data, Buffer.from([0])]) }), m] }));
  sol('instructions', 'transferChecked data empty', msg({ ixs: [xfer({ data: Buffer.alloc(0) }), m] }));
  sol('instructions', 'transferChecked with 3 accounts', msg({ ixs: [xfer({ accts: [1, 3, 2] }), m] }));
  sol('instructions', 'transferChecked with 5 accounts (multisig signer)', msg({ ixs: [xfer({ accts: [1, 3, 2, 0, 0] }), m] }));
  sol('instructions', 'memo with one account', msg({ ixs: [T, memoIx('r6', [0])] }));
  sol('instructions', 'memo data not UTF-8', msg({ ixs: [T, memoIx(Buffer.from([0xc3, 0x28, 0xff]))] }));

  // indices out of range
  sol('index', 'program_id_index 200', msg({ ixs: [{ ...T, prog: 200 }, m] }));
  sol('index', 'program_id_index = account count', msg({ ixs: [T, { ...m, prog: 6 }] }));
  sol('index', 'account index 99 in transferChecked', msg({ ixs: [xfer({ accts: [1, 3, 99, 0] }), m] }));
  sol('index', 'account index 255', msg({ ixs: [xfer({ accts: [255, 3, 2, 0] }), m] }));

  // lying / malformed lengths
  sol('malformed', 'account count says 7, has 6', msg({ countBytes: compactU16(7) }));
  sol('malformed', 'account count 0xffff', msg({ countBytes: Buffer.from([0xff, 0xff, 0x03]) }));
  sol('malformed', 'account count non-canonical (0x86 0x00)', msg({ countBytes: Buffer.from([0x86, 0x00]) }));
  sol('malformed', 'account count 4-byte compact-u16', msg({ countBytes: Buffer.from([0x86, 0x80, 0x80, 0x00]) }));
  sol('malformed', 'instruction count says 3, has 2', msg({ ixCountBytes: compactU16(3) }));
  sol('malformed', 'instruction count 0xffff', msg({ ixCountBytes: Buffer.from([0xff, 0xff, 0x03]) }));
  sol('malformed', 'memo data length 0xffff', msg({ ixs: [T, { ...m, lenBytes: Buffer.from([0xff, 0xff, 0x03]) }] }));
  sol('malformed', 'memo data length non-canonical', msg({ ixs: [T, { ...m, lenBytes: Buffer.from([0x82, 0x00]) }] }));
  sol('malformed', 'transferChecked account count says 200, has 4', msg({ ixs: [{ ...T, accCountBytes: compactU16(200) }, m] }));

  // trailing bytes
  sol('trailing', '+1 zero byte', Buffer.concat([goodMemo, Buffer.from([0])]));
  sol('trailing', '+1 0xff', Buffer.concat([goodMemo, Buffer.from([0xff])]));
  sol('trailing', '+64 B signature-shaped tail', Buffer.concat([goodMemo, bytesOf(64)]));
  sol('trailing', 'valid message twice', Buffer.concat([goodMemo, goodMemo]));
  sol('trailing', 'v0 address-table count after a legacy message', Buffer.concat([goodMemo, compactU16(0)]));

  // oversized
  const at = len => {   // a well-formed message of exactly `len` bytes, padded in the memo
    for (let n = Math.max(0, len - goodMemo.length - 2); ; n++) {
      const b = msg({ ixs: [T, memoIx('x'.repeat(n))] });
      if (b.length === len) return b;
      if (b.length > len) throw new Error(`cannot build a ${len} B message`);
    }
  };
  sol('oversized', '1233 B well-formed message (memo padding)', at(1233));
  sol('oversized', '1233 B: valid message + zero padding', Buffer.concat([goodMemo, Buffer.alloc(1233 - goodMemo.length)]));
  sol('oversized', '4096 B well-formed message', at(4096));
  sol('oversized', `${MAX_SERVED} B random tail`, Buffer.concat([goodMemo, bytesOf(MAX_SERVED - goodMemo.length)]));

  // truncation at every field boundary and every 7th byte
  const cuts = new Set([0, 1, 2, 3, 4, 5, 36, 37, 68, 100, 4 + 6 * 32, 4 + 6 * 32 + 16, 4 + 6 * 32 + 32, 4 + 6 * 32 + 33,
    goodMemo.length - 13, goodMemo.length - 4, goodMemo.length - 3, goodMemo.length - 1]);
  for (let i = 0; i < goodMemo.length; i += 7) cuts.add(i);
  for (const n of [...cuts].sort((a, b) => a - b)) sol('truncated', `first ${n} of ${goodMemo.length} B`, goodMemo.subarray(0, n));

  // random bytes and single-bit flips; keep only what the reference rejects (a flip in the blockhash or the
  // amount is still a valid payment, so it is not a fuzz input)
  const ctx = { payer, mint, decimals };
  for (let i = 0; i < 16; i++) {
    const g = bytesOf(1 + Math.floor(rand() * 1300));
    if (i % 2 === 0) g[0] = 1;                       // get past the first byte half the time
    if (!referenceSolana(g, ctx).ok) sol('random', `random ${g.length} B${i % 2 === 0 ? ' (byte 0 = 1)' : ''}`, g);
  }
  for (let i = 0, kept = 0; i < 200 && kept < 24; i++) {
    const pos = Math.floor(rand() * goodMemo.length), bit = Math.floor(rand() * 8);
    const f = Buffer.from(goodMemo); f[pos] ^= 1 << bit;
    if (!referenceSolana(f, ctx).ok) { sol('bitflip', `byte ${pos} bit ${bit}`, f); kept++; }
  }

  // ── sign_request: unsigned REQ frame (header..name). sign_request has no written input rules beyond the
  // frame format, so every refusal here is R's interpretation; agree with A.
  const rq = (name, frame) => add('sign_request', name[0], name[1], [{ t: 'b', v: frame }], 'refuse', 'interp');
  add('sign_request', 'control', 'valid REQ frame naming this badge', [{ t: 'b', v: reqFrame() }], 'signed');
  rq(['shape', 'empty'], Buffer.alloc(0));
  rq(['shape', 'header only'], reqFrame().subarray(0, 4));
  rq(['shape', 'name_len 6, name missing'], reqFrame().subarray(0, REQ_FIXED));
  rq(['shape', 'bad magic "VX"'], reqFrame({ magic: 'VX' }));
  rq(['shape', 'version 2'], reqFrame({ version: 2 }));
  rq(['shape', 'type 2 (CHAL)'], reqFrame({ type: 2 }));
  rq(['shape', 'name_len 33'], reqFrame({ name: 'x'.repeat(33) }));
  rq(['shape', 'name_len 255, 6-byte name'], reqFrame({ nameLen: 255 }));
  rq(['trailing', 'trailing byte after name'], reqFrame({ tail: Buffer.from([0]) }));
  rq(['trailing', 'full frame including a 64 B sig'], reqFrame({ tail: bytesOf(64) }));
  rq(['wrong-prefix', 'already prefixed "pay-req:"'], Buffer.concat([Buffer.from('pay-req:'), reqFrame()]));
  rq(['wrong-prefix', 'raw Solana message'], goodMemo);
  rq(['field', 'rail 3'], reqFrame({ rail: 3 }));
  rq(['field', 'rail 1 with currency USD'], reqFrame({ currency: 'USD\0' }));
  rq(['field', 'amount 0'], reqFrame({ amount: 0 }));
  rq(['field', 'payee is another badge'], reqFrame({ payee: OTHER }));
  rq(['field', 'name with control bytes'], reqFrame({ name: 'Cof\x00\x1bfee' }));
  rq(['oversized', '4096 B'], Buffer.concat([reqFrame(), bytesOf(4096 - reqFrame().length)]));

  // ── sign_proof: fixed-size arguments (00 §4) ───────────────────────────────
  const ok3 = [{ t: 'b', v: Buffer.alloc(8, 0x77) }, { t: 'b', v: Buffer.alloc(16, 0xc3) }, { t: 'b', v: OTHER }];
  const pf = (name, args) => add('sign_proof', 'args', name, args);
  const with_ = (i, a) => ok3.map((x, j) => (j === i ? a : x));
  add('sign_proof', 'control', 'valid req_id, nonce, payer_pubkey', ok3, 'signed');
  for (const [i, label, size] of [[0, 'req_id', 8], [1, 'nonce', 16], [2, 'payer_pubkey', 32]]) {
    for (const n of [0, size - 1, size + 1, size * 4]) pf(`${label} ${n} B`, with_(i, { t: 'b', v: bytesOf(n) }));
    pf(`${label} nil`, with_(i, { t: 'nil' }));
    pf(`${label} number`, with_(i, { t: 'n', v: 12345678 }));
    pf(`${label} table`, with_(i, { t: 'T' }));
  }
  pf('no arguments', []);
  pf('raw Solana message as req_id', with_(0, { t: 'b', v: goodMemo }));

  // ── begin_bank (P2-A): 00 §6 payload. Skipped on firmware without it. ───────
  const bk = (group, name, bytes, basis = 'spec') => add('begin_bank', group, name, [{ t: 'b', v: bytes }, { t: 'T' }], 'refuse', basis);
  const lines = bankText();
  add('begin_bank', 'control', 'valid bank payload', [{ t: 'b', v: bank() }, { t: 'T' }], 'screen');
  bk('shape', 'empty', Buffer.alloc(0));
  bk('trailing', 'trailing newline', Buffer.concat([bank(), Buffer.from('\n')]));
  bk('trailing', 'extra key at the end', bank([...lines, 'memo=hi']));
  bk('shape', 'CRLF line endings', Buffer.from(lines.join('\r\n')));
  bk('shape', 'keys reordered (amount_cents before action)', bank([lines[0], lines[1], lines[3], lines[2], ...lines.slice(4)]));
  bk('shape', 'missing issued_at', bank(lines.slice(0, -1)));
  bk('shape', 'unknown key in place of attestation', bank(lines.map(l => (l.startsWith('attestation=') ? 'attest=none' : l))));
  bk('shape', 'space before "="', bank(lines.map(l => (l.startsWith('amount_cents') ? 'amount_cents =4000' : l))));
  bk('shape', 'trailing space on a value', bank(lines.map(l => (l.startsWith('payee_name') ? 'payee_name=Coffee Cart ' : l))));
  bk('field', 'v=2', bank(['v=2', ...lines.slice(1)]));
  bk('field', 'rail=solana', bank(lines.map(l => (l.startsWith('rail=') ? 'rail=solana' : l))));
  bk('field', 'action=refund', bank(lines.map(l => (l.startsWith('action=') ? 'action=refund' : l))));
  bk('field', 'amount_cents negative', bank(bankText({ amount: '-4000' })), 'interp');
  bk('field', 'amount_cents with leading zero', bank(bankText({ amount: '04000' })), 'interp');
  bk('field', 'amount_cents 2^64', bank(bankText({ amount: '18446744073709551616' })), 'interp');
  bk('field', 'NUL inside payee_name', bank(lines.map(l => (l.startsWith('payee_name') ? 'payee_name=Coffee\0Cart' : l))), 'interp');
  bk('field', 'invalid UTF-8', Buffer.concat([bank(), Buffer.from([0xc3, 0x28])]));
  bk('wrong-prefix', 'already prefixed "bank-auth:"', Buffer.concat([Buffer.from('bank-auth:'), bank()]));
  bk('wrong-prefix', 'raw Solana message', goodMemo);
  bk('oversized', '4096 B (long from_acct)', bank(lines.map(l => (l.startsWith('from_acct') ? 'from_acct=' + 'a'.repeat(4000) : l))));

  // ids, expected reasons, reference details
  const prefix = { begin_solana: 'sol', sign_request: 'req', sign_proof: 'prf', begin_bank: 'bnk' };
  const counters = {};
  for (const c of cases) {
    counters[c.fn] = (counters[c.fn] ?? 0) + 1;
    c.id = `${prefix[c.fn]}-${String(counters[c.fn]).padStart(3, '0')}`;
    c.reason = c.expect === 'refuse' && c.fn.startsWith('begin_') ? 'undecodable' : null;
    c.ref = reference(c, ctx).detail ?? 'ok';
  }
  return cases;
}

// JSON-safe form (bytes as hex) for harness/fuzz-vectors.json and the run report.
export function exportCase(c) {
  return { ...c, args: c.args.map(a => (a.t === 'b' ? { t: 'b', hex: a.v.toString('hex') } : a)) };
}
