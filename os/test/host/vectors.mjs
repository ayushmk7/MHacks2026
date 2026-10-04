// Generates the host-test vectors with the same libraries the dashboard uses (@solana/kit, sas-lib,
// @solana-program/token). It prints vectors.json; vectors-to-h.mjs turns that into vectors.h.
// Run from the dashboard directory so node resolves that folder's node_modules:
//   cd dashboard && node ../os/test/host/vectors.mjs > ../os/test/host/vectors.json \
//                && node ../os/test/host/vectors-to-h.mjs
// Everything is derived from fixed inputs (Ed25519 signatures are deterministic), so regenerating
// reproduces the committed vectors.json and vectors.h byte for byte.
import { createRequire } from 'node:module';
import { createHash, createPrivateKey, createPublicKey, sign as edSign } from 'node:crypto';
const require = createRequire(process.cwd() + '/');
const kit = require('@solana/kit');
const sas = require('sas-lib');
const tok = require('@solana-program/token');
const { address, getAddressEncoder, getAddressDecoder, getProgramDerivedAddress, pipe, createTransactionMessage,
  setTransactionMessageFeePayer, setTransactionMessageLifetimeUsingBlockhash, appendTransactionMessageInstructions,
  compileTransaction, getBase64EncodedWireTransaction, createNoopSigner } = kit;
const enc = getAddressEncoder(), dec = getAddressDecoder();
const hex = b => Buffer.from(b).toString('hex');

// ---- independent find_program_address (what the badge must implement) ----
const P = (1n << 255n) - 19n;
const D = ((-121665n * modinv(121666n)) % P + P) % P;
function modpow(b, e) { let r = 1n; b %= P; while (e > 0n) { if (e & 1n) r = r * b % P; b = b * b % P; e >>= 1n; } return r; }
function modinv(a) { return modpow(((a % P) + P) % P, P - 2n); }
function onCurve(bytes) {            // RFC 8032 decompression succeeds?
  let y = 0n; for (let i = 31; i >= 0; i--) y = (y << 8n) | BigInt(bytes[i]);
  y &= (1n << 255n) - 1n;
  // NOTE: curve25519-dalek does not reject non-canonical y (y >= p); it reduces. Keep that behaviour.
  y %= P;
  const y2 = y * y % P;
  const u = (y2 - 1n + P) % P, v = (D * y2 + 1n) % P;
  const x2 = u * modinv(v) % P;
  if (x2 === 0n) return true;
  return modpow(x2, (P - 1n) / 2n) === 1n;   // Euler criterion: x^2 is a square
}
function findPda(seeds, programId) {
  for (let bump = 255; bump >= 0; bump--) {
    const h = createHash('sha256');
    for (const s of seeds) h.update(s);
    h.update(Uint8Array.of(bump)); h.update(programId); h.update('ProgramDerivedAddress');
    const d = h.digest();
    if (!onCurve(d)) return [d, bump];
  }
  throw new Error('no bump');
}

const SAS = address('22zoJMtdu4tQc2PzL74ZUT7FrwgB1Udec8DdW4yw4BdG');
const TOKEN = address('TokenkegQfeZyiNwAJbNbGKPFXCWuBvf9Ss623VQ5DA');
const ATA = address('ATokenGPvbdGVxr1b2hvZbsiqW5xWH25efTNsLJA8knL');
const authority = address('FZEAS6Nayu6nMX1RVmoNwoPA4tu1KNM5pvfoXsQzei3K');   // docs/dashboard/API.md
const badge1 = address('Gn2GQYmcQkatE2594ov2ELKkYWZHwARG7FiAoPWSEcxq');       // stand-in merchant
const badge3 = address('4vJDQvQxRrfLYpjFWXFsVTMsw7cayvWB46pnzdn2BW97');       // stand-in Judge A
const mint = address('So11111111111111111111111111111111111111112');          // any valid 32-byte address works as a vector

const out = {};
const [cred, credBump] = await sas.deriveCredentialPda({ authority, name: 'MHacks Verified' });
const [schema, schemaBump] = await sas.deriveSchemaPda({ credential: cred, name: 'badge-identity', version: 1 });
const [att, attBump] = await sas.deriveAttestationPda({ credential: cred, schema, nonce: badge1 });
out.sas = { program: SAS, authority, credential: cred, credBump, schema, schemaBump,
  attestation_for_badge1: att, attBump, attestation_hex: hex(enc.encode(att)) };

const mine = findPda([Buffer.from('attestation'), enc.encode(cred), enc.encode(schema), enc.encode(badge1)], enc.encode(SAS));
out.sas.independent_matches = dec.decode(mine[0]) === att && mine[1] === attBump;
const mineCred = findPda([Buffer.from('credential'), enc.encode(authority), Buffer.from('MHacks Verified')], enc.encode(SAS));
out.sas.independent_cred_matches = dec.decode(mineCred[0]) === cred;
const mineSchema = findPda([Buffer.from('schema'), enc.encode(cred), Buffer.from('badge-identity'), Uint8Array.of(1)], enc.encode(SAS));
out.sas.independent_schema_matches = dec.decode(mineSchema[0]) === schema;

const [ata1, ata1Bump] = await tok.findAssociatedTokenPda({ owner: badge1, mint, tokenProgram: TOKEN });
const [ata3, ata3Bump] = await tok.findAssociatedTokenPda({ owner: badge3, mint, tokenProgram: TOKEN });
const mineAta = findPda([enc.encode(badge1), enc.encode(TOKEN), enc.encode(mint)], enc.encode(ATA));
out.ata = { mint, owner1: badge1, ata1, ata1Bump, owner3: badge3, ata3, ata3Bump,
  independent_matches: dec.decode(mineAta[0]) === ata1 && mineAta[1] === ata1Bump };

// on-curve vectors for the C check: digest hex + expected result for every bump tried
const curveVec = [];
for (const [label, seeds, prog] of [
  ['att', [Buffer.from('attestation'), enc.encode(cred), enc.encode(schema), enc.encode(badge1)], enc.encode(SAS)],
  ['ata1', [enc.encode(badge1), enc.encode(TOKEN), enc.encode(mint)], enc.encode(ATA)],
  ['ata3', [enc.encode(badge3), enc.encode(TOKEN), enc.encode(mint)], enc.encode(ATA)]]) {
  for (let bump = 255; bump >= 248; bump--) {
    const h = createHash('sha256'); for (const s of seeds) h.update(s);
    h.update(Uint8Array.of(bump)); h.update(prog); h.update('ProgramDerivedAddress');
    const d = h.digest(); curveVec.push({ label, bump, digest: hex(d), onCurve: onCurve(d) });
  }
}
out.curveVectors = curveVec;

// ---- transferChecked messages exactly as the dashboard builds them ----
const blockhash = { blockhash: '4vJ9JU1bJJE96FWSJKvHsmmFADCg4gpZQff4P3bkLKi', lastValidBlockHeight: 1000n }; // 32 bytes of 0x39.. arbitrary valid b58
function build(version, amountRaw) {
  const ix = tok.getTransferCheckedInstruction({ source: ata3, mint, destination: ata1,
    authority: createNoopSigner(badge3), amount: BigInt(amountRaw), decimals: 2 });
  const compiled = compileTransaction(pipe(createTransactionMessage({ version }),
    m => setTransactionMessageFeePayer(badge3, m),
    m => setTransactionMessageLifetimeUsingBlockhash(blockhash, m),
    m => appendTransactionMessageInstructions([ix], m)));
  const wire = Buffer.from(getBase64EncodedWireTransaction(compiled), 'base64');
  return { messageLen: compiled.messageBytes.length, wireLen: wire.length, messageHex: hex(compiled.messageBytes), wireHex: hex(wire) };
}
out.tx = { payer: badge3, payer_hex: hex(enc.encode(badge3)), source_ata3_hex: hex(enc.encode(ata3)), dest_ata1_hex: hex(enc.encode(ata1)),
  dest_owner_hex: hex(enc.encode(badge1)), mint_hex: hex(enc.encode(mint)), token_program_hex: hex(enc.encode(TOKEN)),
  ata_program_hex: hex(enc.encode(ATA)), sas_program_hex: hex(enc.encode(SAS)),
  blockhash_hex: hex(enc.encode(address(blockhash.blockhash))),
  legacy_1000: build('legacy', 1000), v0_1000: build(0, 1000), legacy_50000: build('legacy', 50000) };

// ---- a second key set where @solana/kit's account order differs from ascending raw bytes ----
// kit sorts account keys inside a role class with a base58 text collation; sol_tx_build_transfer sorts by raw
// bytes. Both orders are valid messages and the decoder accepts both. Search deterministic owners and mints
// until kit places the two writable token accounts AND mint/token program in the opposite order to raw bytes.
{
  const cmp = (a, b) => Buffer.compare(Buffer.from(a), Buffer.from(b));
  const TOKEN_BYTES = enc.encode(TOKEN);
  let found = null;
  for (let i = 0; i < 4000 && !found; i++) {
    const owner = dec.decode(createHash('sha256').update('badge-os-vector-owner-' + i).digest());
    const altMint = dec.decode(createHash('sha256').update('badge-os-vector-mint-' + (i % 64)).digest());
    const [src] = await tok.findAssociatedTokenPda({ owner: badge3, mint: altMint, tokenProgram: TOKEN });
    const [dst] = await tok.findAssociatedTokenPda({ owner, mint: altMint, tokenProgram: TOKEN });
    const ix = tok.getTransferCheckedInstruction({ source: src, mint: altMint, destination: dst,
      authority: createNoopSigner(badge3), amount: 1000n, decimals: 2 });
    const compiled = compileTransaction(pipe(createTransactionMessage({ version: 'legacy' }),
      m => setTransactionMessageFeePayer(badge3, m),
      m => setTransactionMessageLifetimeUsingBlockhash(blockhash, m),
      m => appendTransactionMessageInstructions([ix], m)));
    const msg = Buffer.from(compiled.messageBytes);
    const k = n => msg.subarray(4 + 32 * n, 36 + 32 * n);
    const writableSwapped = cmp(k(1), k(2)) > 0;          // kit order is NOT ascending bytes
    const readonlySwapped = cmp(k(3), k(4)) > 0;
    if (writableSwapped && readonlySwapped) {
      found = { owner, owner_hex: hex(enc.encode(owner)), mint: altMint, mint_hex: hex(enc.encode(altMint)),
        source_hex: hex(enc.encode(src)), dest_hex: hex(enc.encode(dst)), messageLen: msg.length, messageHex: hex(msg),
        kit_order: [1, 2, 3, 4].map(n => dec.decode(k(n))), token_program_index: [3, 4].find(n => cmp(k(n), TOKEN_BYTES) === 0) };
    }
  }
  if (!found) throw new Error('no key set with a differing account order found');
  out.tx.legacy_alt_order = found;
}

// ---- a message the decoder must refuse: the same transfer with a ComputeBudget instruction in front ----
// (acceptance test T-F3). SetComputeUnitPrice = tag 3 + u64 LE micro-lamports.
{
  const price = new Uint8Array(9); price[0] = 3; price[1] = 0xe8; price[2] = 0x03;   // 1000 micro-lamports
  const cb = { programAddress: address('ComputeBudget111111111111111111111111111111'), data: price };
  const ix = tok.getTransferCheckedInstruction({ source: ata3, mint, destination: ata1,
    authority: createNoopSigner(badge3), amount: 1000n, decimals: 2 });
  const compiled = compileTransaction(pipe(createTransactionMessage({ version: 'legacy' }),
    m => setTransactionMessageFeePayer(badge3, m),
    m => setTransactionMessageLifetimeUsingBlockhash(blockhash, m),
    m => appendTransactionMessageInstructions([cb, ix], m)));
  const msg = Buffer.from(compiled.messageBytes);
  out.tx.legacy_compute_budget = { messageLen: msg.length, messageHex: hex(msg), messageBase64: msg.toString('base64') };
}

// ---- attestation account bytes as SAS would store them for name "MHacks Merch" ----
const name = Buffer.from('MHacks Merch');
const data = Buffer.concat([Buffer.from(Uint32Array.of(name.length).buffer), name]);
const expiry = 1793664000n; // arbitrary
const exp = Buffer.alloc(8); exp.writeBigInt64LE(expiry);
const acct = Buffer.concat([Buffer.of(2), enc.encode(badge1), enc.encode(cred), enc.encode(schema),
  Buffer.from(Uint32Array.of(data.length).buffer), data, enc.encode(authority), exp, Buffer.alloc(32)]);
out.attestationAccount = { len: acct.length, hex: hex(acct), base64: acct.toString('base64'),
  kitDecoded: (() => { const d = sas.getAttestationDecoder().decode(acct); return { discriminator: d.discriminator, nonce: d.nonce, credential: d.credential, schema: d.schema, dataHex: hex(d.data), signer: d.signer, expiry: String(d.expiry), tokenAccount: d.tokenAccount }; })() };

// ---- Badge OS vectors (object `vk`): keys, registry record, REQ, PROOF, Memo message ----
// TEST KEYS ONLY. The seeds are SHA-256 of fixed labels, so anyone can recompute them.
{
  const PKCS8_ED25519 = Buffer.from('302e020100300506032b657004220420', 'hex');
  const keypair = label => {
    const seed = createHash('sha256').update(label).digest();
    const priv = createPrivateKey({ key: Buffer.concat([PKCS8_ED25519, seed]), format: 'der', type: 'pkcs8' });
    const pub = createPublicKey(priv).export({ format: 'der', type: 'spki' }).subarray(-32);
    return { priv, seed, pub, json: { seed_hex: hex(seed), pubkey_hex: hex(pub), pubkey_b58: dec.decode(pub) } };
  };
  const signWith = (kp, prefix, bytes) => edSign(null, Buffer.concat([Buffer.from(prefix), Buffer.from(bytes)]), kp.priv);
  const issuer = keypair('badge-os-vector-issuer');
  const device = keypair('badge-os-vector-device');     // the payee badge (the merchant)

  // Canonical registry record (docs/os/wallet/checks.md) for the device key. Its token account is the
  // destination of tx.legacy_1000 and of memo_tx, so those messages pay this record's holder.
  const ISSUED_AT = 1790000000, EXPIRY = 1800000000, NAME = 'MHacks Merch';
  const [deviceAtt] = await sas.deriveAttestationPda({ credential: cred, schema, nonce: dec.decode(device.pub) });
  const recordText = [
    'v=1',
    'attestation=' + deviceAtt,
    'display_name=' + NAME,
    'device_pubkey=' + hex(device.pub),
    'kind=merchant',
    'solana_wallet=' + hex(enc.encode(badge1)),
    'solana_ata=' + hex(enc.encode(ata1)),
    'bank_ref_hash=',
    'expiry=' + EXPIRY,
    'status=active',
    'issued_at=' + ISSUED_AT].join('\n');
  const recordBytes = Buffer.from(recordText, 'utf8');
  const record = { text: recordText, hex: hex(recordBytes), sig_hex: hex(signWith(issuer, 'registry:', recordBytes)),
    issued_at: ISSUED_AT, expiry: EXPIRY };

  // REQ frame (docs/os/protocol/espnow.md): the device asks for 10.00 HACK; signature domain pay-req.
  const REQ_EXPIRY = ISSUED_AT + 600, REQ_AMOUNT = 1000n, CURRENCY = 'HACK';
  const reqId = createHash('sha256').update('badge-os-vector-req-id').digest().subarray(0, 8);
  const amount = Buffer.alloc(8); amount.writeBigUInt64LE(REQ_AMOUNT);
  const currency = Buffer.alloc(4); currency.write(CURRENCY, 'ascii');
  const reqExpiry = Buffer.alloc(4); reqExpiry.writeUInt32LE(REQ_EXPIRY);
  const reqSigned = Buffer.concat([Buffer.from([0x56, 0x4b, 1, 1]), Buffer.of(1), device.pub, amount, currency, reqId,
    reqExpiry, Buffer.of(NAME.length), Buffer.from(NAME, 'ascii')]);
  const reqFrame = Buffer.concat([reqSigned, signWith(device, 'pay-req:', reqSigned)]);
  const req = { frame_hex: hex(reqFrame), signed_len: reqSigned.length, expiry: REQ_EXPIRY, amount: String(REQ_AMOUNT),
    currency: CURRENCY, name: NAME };

  // PROOF: the device answers a challenge from the payer of tx.legacy_1000; signature domain pay-proof
  // over req_id || nonce || payer_pubkey.
  const nonce = createHash('sha256').update('badge-os-vector-nonce').digest().subarray(0, 16);
  const payer = Buffer.from(enc.encode(badge3));
  const proof = { req_id_hex: hex(reqId), nonce_hex: hex(nonce), payer_hex: hex(payer),
    sig_hex: hex(signWith(device, 'pay-proof:', Buffer.concat([reqId, nonce, payer]))) };

  // The transfer of tx.legacy_1000 followed by one Memo instruction, built by hand: program address,
  // no accounts, UTF-8 data.
  const MEMO = 'coffee #42';
  const memoIx = { programAddress: address('MemoSq4gqABAXKb96qnH8TysNcWxMyWCqXgDLGmfcHr'), data: new TextEncoder().encode(MEMO) };
  const ix = tok.getTransferCheckedInstruction({ source: ata3, mint, destination: ata1,
    authority: createNoopSigner(badge3), amount: 1000n, decimals: 2 });
  const compiled = compileTransaction(pipe(createTransactionMessage({ version: 'legacy' }),
    m => setTransactionMessageFeePayer(badge3, m),
    m => setTransactionMessageLifetimeUsingBlockhash(blockhash, m),
    m => appendTransactionMessageInstructions([ix, memoIx], m)));
  const memoMsg = Buffer.from(compiled.messageBytes);
  const memo_tx = { messageHex: hex(memoMsg), memo: MEMO, messageLen: memoMsg.length };

  out.vk = { issuer: issuer.json, device: device.json, record, req, proof, memo_tx };
  out.tx.mint = mint;     // base58 of tx.mint_hex: the mint of the test token table (test provisioning)
}
console.log(JSON.stringify(out, null, 1));
