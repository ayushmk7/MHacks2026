import { createHash, randomBytes } from 'node:crypto';
import sas from 'sas-lib';   // CJS bundle: default import only
import { address, isAddress, fetchEncodedAccount, getAddressEncoder, getAddressDecoder } from '@solana/kit';
import { env, token } from './config.js';
import { rpc, sendIxs, ataOf, errMsg } from './solana.js';
import { q, Q } from './db.js';

const { deriveCredentialPda, deriveSchemaPda, deriveAttestationPda,
        getCreateCredentialInstruction, getCreateSchemaInstruction,
        getCreateAttestationInstruction, getCloseAttestationInstruction,
        fetchSchema, fetchAllMaybeAttestation, serializeAttestationData, deserializeAttestationData } = sas;

// payee_v1. Only used to create the schema, which already exists on devnet; SAS schemas are immutable.
// 12 = String, 13 = Vec<u8>, 0 = u8. Vec<u8> fields are 32 bytes or empty.
const LAYOUT = new Uint8Array([12, 13, 0, 13, 13]);
const FIELDS = ['display_name', 'device_pubkey', 'kind', 'solana_wallet', 'bank_ref_hash'];
export const KINDS = { merchant: 1, person: 2, relay: 3 };
const KIND_NAME = Object.fromEntries(Object.entries(KINDS).map(([k, v]) => [v, k]));
const TTL_S = 30 * 24 * 3600;

// Solana Attestation Service registry: one credential (the issuer), one schema (payee_v1), and one
// attestation PDA per badge (nonce = the badge pubkey). Revoke = close the PDA, so "no account" = not verified.
export const registryState = { credential: null, schema: null, ready: false, authority: null, authoritySol: null };
let authority, schemaAcc;

const enc = getAddressEncoder(), dec = getAddressDecoder();
const bytesOf = addr => Buffer.from(enc.encode(address(addr)));
const bytesOrNull = v => (v && v.length ? Buffer.from(v) : null);
const fail = (code, message) => { throw Object.assign(new Error(message), { code }); };

const exists = async addr => (await fetchEncodedAccount(rpc, addr, { commitment: 'confirmed' })).exists;
export const attestationPda = async pubkey => (await deriveAttestationPda({ credential: registryState.credential, schema: registryState.schema, nonce: address(pubkey) }))[0];
const getSchema = async () => (schemaAcc ??= await fetchSchema(rpc, registryState.schema, { commitment: 'confirmed' }));
const closeIx = pda => getCloseAttestationInstruction({ payer: authority, authority, credential: registryState.credential, attestation: pda });
const dataOf = async acc => deserializeAttestationData((await getSchema()).data, acc.data.data);
const nameOf = async acc => (await dataOf(acc)).display_name;
const expiryOf = acc => (acc.data.expiry > 0n ? new Date(Number(acc.data.expiry) * 1000) : null);
const isExpired = acc => acc.data.expiry > 0n && Number(acc.data.expiry) * 1000 <= Date.now();

// Read-only: which of credential / schema exist. Once both do they cannot disappear, so stop asking.
async function probe() {
  if (registryState.ready) return [true, true];
  const found = await Promise.all([exists(registryState.credential), exists(registryState.schema)]);
  registryState.ready = found[0] && found[1];
  return found;
}

export async function initRegistry(authoritySigner) {
  authority = authoritySigner;
  registryState.authority = authority.address;
  [registryState.credential] = await deriveCredentialPda({ authority: authority.address, name: env.SAS_CREDENTIAL_NAME });
  [registryState.schema] = await deriveSchemaPda({ credential: registryState.credential, name: env.SAS_SCHEMA_NAME, version: env.SAS_SCHEMA_VERSION });
  await probe();
}

// Costs SOL, so it runs lazily on first issue, not at boot.
export async function ensureRegistry() {
  const [hasCredential, hasSchema] = await probe();
  const ixs = [];
  if (!hasCredential) ixs.push(getCreateCredentialInstruction({
    payer: authority, credential: registryState.credential, authority, name: env.SAS_CREDENTIAL_NAME, signers: [authority.address] }));
  if (!hasSchema) ixs.push(getCreateSchemaInstruction({
    payer: authority, authority, credential: registryState.credential, schema: registryState.schema,
    name: env.SAS_SCHEMA_NAME, description: 'Verified payee for MHacks demo', layout: LAYOUT, fieldNames: FIELDS }));
  if (ixs.length) await sendIxs(authority, ixs);
  registryState.ready = true;
}

const mirror = (sql, params) => q(sql, params).catch(err => console.error('[registry] mirror write failed, the 30 s sync reconciles:', err.message));

const UPSERT_PAYEE = `
INSERT INTO payees (pubkey, kind, display_name, nessie_ref, salt, bank_ref_hash, solana_wallet, attestation, operator_id,
                    settle_mode, nessie_account_id, created_at, updated_at)
VALUES ($1, $2, $3, $4, $5, $6, $7, $8, $9, $10, $11, now(), now())
ON CONFLICT (pubkey) DO UPDATE SET kind = EXCLUDED.kind, display_name = EXCLUDED.display_name, nessie_ref = EXCLUDED.nessie_ref,
  salt = EXCLUDED.salt, bank_ref_hash = EXCLUDED.bank_ref_hash, solana_wallet = EXCLUDED.solana_wallet,
  attestation = EXCLUDED.attestation, operator_id = EXCLUDED.operator_id, settle_mode = EXCLUDED.settle_mode,
  nessie_account_id = EXCLUDED.nessie_account_id, updated_at = now()`;
// "Active" = attestation not cleared by revoke.
const OTHER_RELAY = `SELECT pubkey FROM payees WHERE kind = 'relay' AND operator_id = $1 AND pubkey <> $2 AND attestation IS NOT NULL LIMIT 1`;
// The attestations CHECK only allows verified|revoked, so an expired attestation is mirrored as revoked with no
// revoked_sig and its real expires_at. The WHERE keeps the 30 s sync from re-NOTIFYing an unchanged row.
const MIRROR_EXPIRED = `
INSERT INTO attestations (subject, name, status, attestation_pda, expires_at, revoked_at) VALUES ($1, $2, 'revoked', $3, $4, now())
ON CONFLICT (subject) DO UPDATE SET name = EXCLUDED.name, status = 'revoked', attestation_pda = EXCLUDED.attestation_pda,
  expires_at = EXCLUDED.expires_at, revoked_at = COALESCE(attestations.revoked_at, now()), updated_at = now()
WHERE attestations.status <> 'revoked' OR attestations.name <> EXCLUDED.name OR attestations.expires_at IS DISTINCT FROM EXCLUDED.expires_at`;

// Capital One's rules for one record, no I/O (exported for tests). Identity: only a Capital One account holder can be
// verified, so a merchant or person needs nessieRef (merchant id / account id). Settlement: 'bank' pays the issuer's
// settlement wallet (the treasury owner) on chain and the backend deposits dollars into nessieAccountId afterwards.
export function issueParams(pubkey, { name, kind, solanaWallet = pubkey, nessieRef = null, settleMode = 'hack', nessieAccountId = null } = {},
                            settlementWallet = registryState.authority) {
  if (!isAddress(pubkey ?? '')) fail('invalid_pubkey', 'pubkey must be a base58 32-byte address');
  if (!KINDS[kind]) fail('invalid_param', 'kind must be merchant, person or relay');
  if (typeof name !== 'string' || !/^[\x20-\x7E]{1,32}$/.test(name)) fail('invalid_name', 'name must be 1 to 32 printable ASCII characters');
  if (settleMode !== 'hack' && settleMode !== 'bank') fail('invalid_param', 'settleMode must be hack or bank');
  if (kind === 'relay') {
    if (settleMode === 'bank') fail('invalid_param', 'a relay cannot settle to Capital One');
    if (!isAddress(solanaWallet ?? '')) fail('invalid_param', 'solanaWallet must be a base58 32-byte address');
    return { solanaWallet, bankRef: null, settleMode, nessieAccountId: null };
  }
  const bankRef = nessieRef == null ? '' : String(nessieRef).trim();
  if (!bankRef) fail('invalid_param', 'only Capital One account holders can be verified: nessieRef is required for a merchant or person');
  if (settleMode === 'hack') {
    if (!isAddress(solanaWallet ?? '')) fail('invalid_param', 'solanaWallet must be a base58 32-byte address');
    return { solanaWallet, bankRef, settleMode, nessieAccountId: null };
  }
  if (typeof nessieAccountId !== 'string' || !/^[A-Za-z0-9-]{1,64}$/.test(nessieAccountId))
    fail('invalid_param', 'settleMode bank needs nessieAccountId, the Nessie account that receives the deposits (1 to 64 letters, digits or dashes)');
  if (!isAddress(settlementWallet ?? '')) fail('not_configured', 'the issuer settlement wallet is not loaded yet');
  return { solanaWallet: settlementWallet, bankRef, settleMode, nessieAccountId };
}

// Re-issue or rename = close + create in one transaction. The salt and plaintext Nessie id stay in the DB only;
// the chain sees sha256(salt || nessie id), so the bank reference can't be read or brute-forced from the attestation.
export async function issue(pubkey, opts = {}) {
  const { name, kind, operatorId = null } = opts;
  const { solanaWallet, bankRef, settleMode, nessieAccountId } = issueParams(pubkey, opts);
  if (kind === 'relay' && operatorId != null && (await q(OTHER_RELAY, [operatorId, pubkey])).rowCount)
    fail('conflict', 'this operator already holds an active relay attestation');

  const salt = bankRef ? randomBytes(16) : null;
  const bankRefHash = bankRef ? createHash('sha256').update(salt).update(bankRef, 'utf8').digest() : null;

  await ensureRegistry();
  const pda = await attestationPda(pubkey);
  const expiry = Math.floor(Date.now() / 1000) + TTL_S;
  const data = { display_name: name, device_pubkey: [...bytesOf(pubkey)], kind: KINDS[kind],
                 solana_wallet: [...bytesOf(solanaWallet)], bank_ref_hash: bankRefHash ? [...bankRefHash] : [] };
  const ixs = [];
  if (await exists(pda)) ixs.push(closeIx(pda));
  ixs.push(getCreateAttestationInstruction({
    payer: authority, authority, credential: registryState.credential, schema: registryState.schema, attestation: pda,
    nonce: address(pubkey), expiry, data: serializeAttestationData((await getSchema()).data, data) }));
  const signature = await sendIxs(authority, ixs);
  const expiresAt = new Date(expiry * 1000);
  await mirror(Q.upsertVerified, [pubkey, name, pda, signature, expiresAt]);
  // Not a mirror: losing the salt would orphan the on-chain hash. A failure here surfaces; re-issuing heals it.
  await q(UPSERT_PAYEE, [pubkey, kind, name, bankRef, salt, bankRefHash, solanaWallet, pda, operatorId, settleMode, nessieAccountId]);
  return { signature, pda, expiresAt, solanaWallet, settleMode };
}

export async function revoke(pubkey) {
  const pda = await attestationPda(pubkey);
  const [acc] = await fetchAllMaybeAttestation(rpc, [pda]);
  if (!acc.exists) throw Object.assign(new Error('no on-chain attestation exists for this pubkey'), { code: 'conflict' });
  const name = await nameOf(acc);
  const signature = await sendIxs(authority, [closeIx(pda)]);
  await mirror(Q.upsertVerified, [pubkey, name, pda, null, expiryOf(acc)]);   // make sure the mirror row exists
  await mirror(Q.markRevoked, [pubkey, signature]);
  await mirror(`UPDATE payees SET attestation = NULL, updated_at = now() WHERE pubkey = $1`, [pubkey]);   // frees the operator's relay slot
  return { signature, pda, name, expiresAt: expiryOf(acc) };
}

// chain -> attestations table, every 30 s. Chain is the source of truth.
export async function syncAttestations() {
  const subjects = (await q(Q.syncSubjects)).rows.map(r => r.subject);
  if (!subjects.length) return;
  const ready = (await probe()).every(Boolean);
  const pdas = await Promise.all(subjects.map(attestationPda));
  const accounts = ready ? await fetchAllMaybeAttestation(rpc, pdas) : pdas.map(() => ({ exists: false }));
  for (const [i, acc] of accounts.entries()) {
    if (!acc.exists) await q(Q.markRevoked, [subjects[i], null]);
    else if (isExpired(acc)) await q(MIRROR_EXPIRED, [subjects[i], await nameOf(acc), pdas[i], expiryOf(acc)]);
    else await q(Q.upsertVerified, [subjects[i], await nameOf(acc), pdas[i], null, expiryOf(acc)]);
  }
}

const PAYEE_ROW = `
SELECT p.kind, p.display_name, p.solana_wallet, p.bank_ref_hash, p.updated_at, a.status AS att_status, a.expires_at
FROM payees p LEFT JOIN attestations a ON a.subject = p.pubkey WHERE p.pubkey = $1`;

const unix = d => Math.floor(new Date(d).getTime() / 1000);
const withAta = async f => ({ ...f, solana_ata: f.solana_wallet && token.mint ? bytesOf(await ataOf(dec.decode(f.solana_wallet), token.mint)) : null });

// The facts behind one registry record (00 §7), or null = unknown key (404). Chain first; the payees row
// supplies the facts of a revoked key (its attestation is closed) and of every key while the registry is unreachable.
export async function recordFields(pubkey) {
  const row = (await q(PAYEE_ROW, [pubkey]).catch(err => (console.error('[registry] payees lookup failed:', err.message), null)))?.rows[0];
  const issued_at = Math.floor(Date.now() / 1000);
  const device_pubkey = bytesOf(pubkey);
  let pda = null, acc = null;
  try {
    if ((await probe()).every(Boolean)) { pda = await attestationPda(pubkey); [acc] = await fetchAllMaybeAttestation(rpc, [pda]); }
  } catch (err) { console.error('[registry] attestation fetch failed, serving from the DB:', errMsg(err)); }

  if (acc?.exists) {
    const d = await dataOf(acc);
    return withAta({ attestation: pda, display_name: d.display_name, device_pubkey, kind: KIND_NAME[d.kind] ?? null,
      solana_wallet: bytesOrNull(d.solana_wallet), bank_ref_hash: bytesOrNull(d.bank_ref_hash),
      expiry: Number(acc.data.expiry), status: 'active', issued_at });
  }
  if (!row) return null;
  const fromDb = { display_name: row.display_name, device_pubkey, kind: row.kind,
    solana_wallet: row.solana_wallet ? bytesOf(row.solana_wallet) : null, bank_ref_hash: bytesOrNull(row.bank_ref_hash), issued_at };
  // Chain answered "no account": the key was verified and is now revoked.
  if (acc) return withAta({ ...fromDb, attestation: pda, expiry: row.expires_at ? unix(row.expires_at) : 0, status: 'revoked' });
  // Issuer-only fallback (spec: attestation=none). Revocation and expiry come from the last mirrored state.
  return withAta({ ...fromDb, attestation: null, status: row.att_status === 'revoked' ? 'revoked' : 'active',
    expiry: row.expires_at ? unix(row.expires_at) : unix(row.updated_at) + TTL_S });
}
