import { address } from '@solana/kit';
import { fetchMint } from '@solana-program/token';
import sas from 'sas-lib';
import { env, token, loadBadges, computeGaps } from './config.js';
import { initDb, dbState, listen, syncBadges, q, Q, toPayment } from './db.js';
import { rpc, rpcState, loadOrCreateSigner, errMsg } from './solana.js';
import { ingestState, startIngest } from './ingest.js';
import { registryState, initRegistry, syncAttestations } from './registry.js';
import { startHttp, broadcast, badgeListener } from './http.js';

const log = tag => err => console.error(`[${tag}]`, errMsg(err));
const statusChanged = () => broadcast({ type: 'status', data: {} });
// Safety net for a live demo: a stray rejection in a background loop is logged, it does not take the server down.
process.on('unhandledRejection', log('unhandled'));

// 1. config
const { badges, invalid } = loadBadges();

const gaps = () => computeGaps({
  db: dbState, rpcOk: rpcState.ok, mintSet: !!token.mint, mintFound: token.found, badges, invalid,
  authority: registryState.authority, authoritySol: registryState.authoritySol,
  registryReady: registryState.ready, listenerOpen: badgeListener.open,
});

const status = () => ({
  ok: true, app: 'badgepay', cluster: 'devnet', explorer: 'https://explorer.solana.com',
  db: { ok: dbState.ok, schema: dbState.schema, error: dbState.error },
  rpc: { ok: rpcState.ok !== false, url: new URL(env.RPC_URL).origin },   // origin only: never echo an API key
  ingest: { ...ingestState },
  token: { mint: token.mint, symbol: token.symbol, decimals: token.decimals },
  authority: { pubkey: registryState.authority, sol: registryState.authoritySol },
  registry: { program: sas.SOLANA_ATTESTATION_SERVICE_PROGRAM_ADDRESS, credential: registryState.credential, schema: registryState.schema,
              credentialName: env.SAS_CREDENTIAL_NAME, schemaName: env.SAS_SCHEMA_NAME, schemaVersion: env.SAS_SCHEMA_VERSION, ready: registryState.ready },
  badgeListener: { ...badgeListener },
  gaps: gaps(),
});

// 4 + 5. authority and registry PDAs (before the database so the callbacks below can use them).
// Only the public key is ever logged. Every RPC/database failure from here on becomes a gap, never a crash.
const authority = await loadOrCreateSigner(env.AUTHORITY_KEYPAIR);
console.log(`[boot] registry authority ${authority.address}`);
await initRegistry(authority).catch(log('registry'));

// 2 + 3. database. Does not block boot when Postgres is down, and re-syncs whenever the schema (re)appears.
await initDb(() => {
  statusChanged();
  if (dbState.schema) syncBadges(badges).catch(log('db')).then(syncAttestations).catch(log('registry'));
});

// 5 + 6. authority balance, mint, attestation mirror. Repeats every 30 s.
async function tick() {
  const before = JSON.stringify(gaps());
  try {
    registryState.authoritySol = Number((await rpc.getBalance(authority.address, { commitment: 'confirmed' }).send()).value) / 1e9;
    rpcState.ok = true;
  } catch (err) { rpcState.ok = false; log('rpc')(err); }
  if (token.mint && !token.found && rpcState.ok) {
    // BADGE-GAP(hack-mint): decimals come from the chain. A missing mint account stays a hack_mint_missing gap.
    await fetchMint(rpc, address(token.mint), { commitment: 'confirmed' }).then(m => {
      Object.assign(token, { decimals: m.data.decimals, found: true });
      startIngest({ mint: token.mint, onStateChange: statusChanged });
    }, log('mint'));
  }
  if (dbState.schema) await syncAttestations().catch(log('registry'));
  if (JSON.stringify(gaps()) !== before) statusChanged();
}
tick();   // not awaited: the API must come up (and report gaps) even while the first RPC round is slow
setInterval(tick, 30_000);

// 7. Postgres NOTIFY -> SSE. Payments are re-read so the SSE payload is the REST shape.
listen(async ({ type, id }) => {
  if (type !== 'payment') return broadcast({ type, data: { id } });
  const row = (await q(Q.paymentBySig, [id]).catch(log('db')))?.rows[0];
  if (row) broadcast({ type: 'payment', data: toPayment(row) });
}, statusChanged);

// 8. http
startHttp({ authority, badges, status, onChange: statusChanged });
