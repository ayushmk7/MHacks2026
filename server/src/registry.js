import sas from 'sas-lib';   // CJS bundle: default import only
import { address, fetchEncodedAccount } from '@solana/kit';
import { env } from './config.js';
import { rpc, sendIxs } from './solana.js';
import { q, Q } from './db.js';

const { deriveCredentialPda, deriveSchemaPda, deriveAttestationPda,
        getCreateCredentialInstruction, getCreateSchemaInstruction,
        getCreateAttestationInstruction, getCloseAttestationInstruction,
        fetchSchema, fetchAllMaybeAttestation, serializeAttestationData, deserializeAttestationData } = sas;

const LAYOUT = new Uint8Array([12]);   // one field, type 12 = String
const FIELDS = ['name'];

// Solana Attestation Service registry: one credential (the issuer), one schema ({name: String}), and one
// attestation PDA per badge (nonce = the badge pubkey). Revoke = close the PDA, so "no account" = not verified.
export const registryState = { credential: null, schema: null, ready: false, authority: null, authoritySol: null };
let authority, schemaAcc;

const exists = async addr => (await fetchEncodedAccount(rpc, addr, { commitment: 'confirmed' })).exists;
export const attestationPda = async pubkey => (await deriveAttestationPda({ credential: registryState.credential, schema: registryState.schema, nonce: address(pubkey) }))[0];
const getSchema = async () => (schemaAcc ??= await fetchSchema(rpc, registryState.schema, { commitment: 'confirmed' }));
const closeIx = pda => getCloseAttestationInstruction({ payer: authority, authority, credential: registryState.credential, attestation: pda });
const nameOf = async acc => deserializeAttestationData((await getSchema()).data, acc.data.data).name;
// ponytail: an attestation past its expiry still counts as verified; issue sets 30 days, the event lasts 24 h.
const expiryOf = acc => (acc.data.expiry > 0n ? new Date(Number(acc.data.expiry) * 1000) : null);

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
    name: env.SAS_SCHEMA_NAME, description: 'Badge identity for MHacks demo', layout: LAYOUT, fieldNames: FIELDS }));
  if (ixs.length) await sendIxs(authority, ixs);
  registryState.ready = true;
}

const mirror = (sql, params) => q(sql, params).catch(err => console.error('[registry] mirror write failed, the 30 s sync reconciles:', err.message));

// Re-issue or rename = close + create in one transaction.
export async function issue(pubkey, name) {
  await ensureRegistry();
  const pda = await attestationPda(pubkey);
  const expiry = Math.floor(Date.now() / 1000) + 30 * 24 * 3600;
  const ixs = [];
  if (await exists(pda)) ixs.push(closeIx(pda));
  ixs.push(getCreateAttestationInstruction({
    payer: authority, authority, credential: registryState.credential, schema: registryState.schema, attestation: pda,
    nonce: address(pubkey), expiry, data: serializeAttestationData((await getSchema()).data, { name }) }));
  const signature = await sendIxs(authority, ixs);
  const expiresAt = new Date(expiry * 1000);
  await mirror(Q.upsertVerified, [pubkey, name, pda, signature, expiresAt]);
  return { signature, pda, expiresAt };
}

export async function revoke(pubkey) {
  const pda = await attestationPda(pubkey);
  const [acc] = await fetchAllMaybeAttestation(rpc, [pda]);
  if (!acc.exists) throw Object.assign(new Error('no on-chain attestation exists for this pubkey'), { code: 'conflict' });
  const name = await nameOf(acc);
  const signature = await sendIxs(authority, [closeIx(pda)]);
  await mirror(Q.upsertVerified, [pubkey, name, pda, null, expiryOf(acc)]);   // make sure the mirror row exists
  await mirror(Q.markRevoked, [pubkey, signature]);
  return { signature, pda };
}

// chain -> attestations table, every 30 s. Chain is the source of truth.
export async function syncAttestations() {
  const subjects = (await q(Q.syncSubjects)).rows.map(r => r.subject);
  if (!subjects.length) return;
  const ready = (await probe()).every(Boolean);
  const pdas = await Promise.all(subjects.map(attestationPda));
  const accounts = ready ? await fetchAllMaybeAttestation(rpc, pdas) : pdas.map(() => ({ exists: false }));
  for (const [i, acc] of accounts.entries()) {
    if (acc.exists) await q(Q.upsertVerified, [subjects[i], await nameOf(acc), pdas[i], null, expiryOf(acc)]);
    else await q(Q.markRevoked, [subjects[i], null]);
  }
}
