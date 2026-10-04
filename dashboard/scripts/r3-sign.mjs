// npm run r3 -- [--badge 1] [--to 2|<pubkey>] [--amount 1] [--memo text] [--port 8789] [--stub-key file] [--no-send]
//
// R3 signing harness (docs/specs/P1-r-test-harness.md §3, §4.1): the laptop half.
//   1. Build an UNSIGNED legacy HACK transferChecked whose payer, fee payer and token owner is the badge.
//   2. Queue it: serve it at GET /badge/pending?badge=<pubkey> (same response shape as the server's route).
//   3. The badge's Lua test app signs it with wallet.begin_solana / wallet.poll and POSTs the signature
//      to /badge/signature {id, sig}. Before A's wallet exists, --stub-key signs locally instead.
//   4. Verify the signature against the badge pubkey, attach it ([0x01, sig, message]), send to devnet,
//      print the explorer link and append it to harness/RESULTS.md.
//
// Why its own listener rather than the server's /badge/pending: that route serves attack_attempts rows
// from Postgres, so an R3 payment queued there would show up as an attack on the dashboard. This listener
// has the same GET shape, so the Lua app can point at either. It exposes no admin routes (00 §8).
import http from 'node:http';
import { appendFileSync, existsSync, readFileSync } from 'node:fs';
import { resolve } from 'node:path';
import { parseArgs } from 'node:util';
import { networkInterfaces } from 'node:os';
import {
  address, isAddress, pipe, createTransactionMessage, setTransactionMessageFeePayer,
  setTransactionMessageLifetimeUsingBlockhash, appendTransactionMessageInstructions, compileTransaction,
  getBase64EncodedWireTransaction, createNoopSigner, createKeyPairSignerFromBytes, getAddressEncoder,
} from '@solana/kit';
import { getTransferCheckedInstruction, fetchMint } from '@solana-program/token';
import { env, ROOT, loadBadges } from '../server/src/config.js';
import { rpc, ataOf, errMsg } from '../server/src/solana.js';
import { verifySolanaMessage } from '../server/src/verify.js';

const MEMO_PROGRAM = 'MemoSq4gqABAXKb96qnH8TysNcWxMyWCqXgDLGmfcHr';
const REBUILD_AFTER_MS = 45_000;   // blockhash lives ~60-90 s; rebuild well before (§4.1)
const RESULTS = resolve(ROOT, '../harness/RESULTS.md');

const die = msg => { console.error(`r3: ${msg}`); process.exit(1); };
const { values: arg } = parseArgs({ options: {
  badge: { type: 'string', default: '1' }, to: { type: 'string', default: '2' },
  amount: { type: 'string', default: '1' }, memo: { type: 'string' },
  mint: { type: 'string' }, decimals: { type: 'string' },
  host: { type: 'string', default: '0.0.0.0' }, port: { type: 'string', default: '8789' },
  'stub-key': { type: 'string' }, 'no-send': { type: 'boolean', default: false },
} });

// ── who pays whom ───────────────────────────────────────────────────────────
const { badges } = loadBadges();
const byId = id => badges.find(b => String(b.id) === String(id) && b.pubkey);
let stub = null;
if (arg['stub-key']) {
  const file = resolve(arg['stub-key']);
  if (!existsSync(file)) die(`--stub-key ${file} not found`);
  stub = await createKeyPairSignerFromBytes(new Uint8Array(JSON.parse(readFileSync(file, 'utf8'))));
}
// In stub mode the stand-in key IS the badge; otherwise the badge comes from badges.json.
const payer = stub ? stub.address : (byId(arg.badge)?.pubkey ?? die(`badge ${arg.badge} has no pubkey in badges.json`));
const payee = isAddress(arg.to) ? arg.to : (byId(arg.to)?.pubkey ?? die(`--to ${arg.to}: not a pubkey or configured badge`));
if (payee === payer) die('--to is the paying badge itself');

const mint = address(arg.mint ?? (env.HACK_MINT || die('HACK_MINT is not set: run `npm run devnet:setup` or pass --mint')));
const decimals = arg.decimals != null ? Number(arg.decimals)
  : (await fetchMint(rpc, mint).catch(err => die(`cannot read mint ${mint}: ${errMsg(err)} (pass --decimals to build offline)`))).data.decimals;
const amountRaw = BigInt(Math.round(Number(arg.amount) * 10 ** decimals));
if (!(amountRaw > 0n)) die('--amount must be positive');

// ── 1. build ─────────────────────────────────────────────────────────────────
// Legacy, one signer. Source and destination are the existing HACK ATAs: creating one here would add a
// second instruction, which the badge decoder refuses (00 §4 check 4).
async function build() {
  const ixs = [getTransferCheckedInstruction({
    source: await ataOf(payer, mint), mint, destination: await ataOf(payee, mint),
    authority: createNoopSigner(address(payer)), amount: amountRaw, decimals,
  })];
  if (arg.memo) ixs.push({ programAddress: address(MEMO_PROGRAM), accounts: [], data: new TextEncoder().encode(arg.memo) });
  const { value: bh } = await rpc.getLatestBlockhash({ commitment: 'confirmed' }).send();
  const compiled = compileTransaction(pipe(
    createTransactionMessage({ version: 'legacy' }),
    m => setTransactionMessageFeePayer(address(payer), m),
    m => setTransactionMessageLifetimeUsingBlockhash(bh, m),
    m => appendTransactionMessageInstructions(ixs, m),
  ));
  const message = Buffer.from(compiled.messageBytes);
  return { id: `r3-${Date.now().toString(36)}`, message, builtAt: Date.now(),
           txBase64: getBase64EncodedWireTransaction(compiled), lastValidBlockHeight: bh.lastValidBlockHeight };
}

// ── 4. verify, attach, send ─────────────────────────────────────────────────
async function finish(job, sig) {
  const payerBytes = Buffer.from(getAddressEncoder().encode(address(payer)));
  const check = verifySolanaMessage(job.message, sig, payerBytes);
  if (!check.ok) return { ok: false, reason: check.reason };
  const wire = Buffer.concat([Buffer.from([1]), Buffer.from(sig), job.message]);   // compact-u16(1), sig, message
  console.log(`r3: signature verified against ${payer}`);
  if (arg['no-send']) {
    console.log(`r3: --no-send, signed transaction (base64):\n${wire.toString('base64')}`);
    return { ok: true, sent: false };
  }
  const txSig = await rpc.sendTransaction(wire.toString('base64'), { encoding: 'base64', preflightCommitment: 'confirmed' }).send();
  console.log(`r3: sent ${txSig}, waiting for confirmation...`);
  for (;;) {
    const { value: [status] } = await rpc.getSignatureStatuses([txSig]).send();
    if (status?.err) throw new Error(`transaction failed on chain: ${JSON.stringify(status.err)}`);
    if (status?.confirmationStatus === 'confirmed' || status?.confirmationStatus === 'finalized') break;
    if ((await rpc.getBlockHeight().send()) > job.lastValidBlockHeight) throw new Error('blockhash expired before confirmation');
    await new Promise(r => setTimeout(r, 1000));
  }
  const link = `https://explorer.solana.com/tx/${txSig}?cluster=devnet`;
  console.log(`r3: CONFIRMED ${link}`);
  if (!existsSync(RESULTS)) appendFileSync(RESULTS, '# Harness results\n\n## R3 badge-signed devnet transfers\n\n| Time (UTC) | Signer | Key | Payee | Amount | Explorer |\n| --- | --- | --- | --- | --- | --- |\n');
  appendFileSync(RESULTS, `| ${new Date().toISOString()} | ${payer} | ${stub ? 'stub (laptop key)' : 'badge'} | ${payee} | ${arg.amount} ${env.HACK_SYMBOL} | ${link} |\n`);
  return { ok: true, sent: true, link };
}

let job = await build();
console.log(`r3: built legacy transferChecked ${arg.amount} ${env.HACK_SYMBOL} ${payer} -> ${payee}${arg.memo ? ' + memo' : ''} (${job.message.length} B message)`);

if (stub) {
  // Stand-in for wallet.begin_solana: sign the raw message bytes, exactly as the badge would.
  const sig = new Uint8Array(await crypto.subtle.sign('Ed25519', stub.keyPair.privateKey, job.message));
  const out = await finish(job, sig).catch(err => die(errMsg(err)));
  if (!out.ok) die(`signature rejected: ${out.reason}`);
  process.exit(0);
}

// ── 2-3. queue for the badge ────────────────────────────────────────────────
const rebuild = setInterval(async () => {
  if (Date.now() - job.builtAt < REBUILD_AFTER_MS) return;
  job = await build().catch(err => { console.error(`r3: rebuild failed: ${errMsg(err)}`); return job; });
  console.log(`r3: blockhash aging, rebuilt as ${job.id}`);
}, 5000);

const json = (res, code, body) => { res.writeHead(code, { 'content-type': 'application/json' }); res.end(body === undefined ? '' : JSON.stringify(body)); };
const readBody = req => new Promise((ok, fail) => {
  let data = '';
  req.on('data', c => { data += c; if (data.length > 4096) { req.destroy(); fail(new Error('body too large')); } });
  req.on('end', () => { try { ok(JSON.parse(data)); } catch { fail(new Error('invalid JSON')); } });
});

const server = http.createServer(async (req, res) => {
  const url = URL.parse(req.url, 'http://badge');
  try {
    if (req.method === 'GET' && url?.pathname === '/badge/pending') {
      if (url.searchParams.get('badge') !== payer) return json(res, 204);
      return json(res, 200, { id: job.id, claimed: { merchant: 'R3 signing test', amount: Number(arg.amount), symbol: env.HACK_SYMBOL },
        txBase64: job.txBase64, messageBase64: job.message.toString('base64'), txVersion: 'legacy', txBytes: Buffer.from(job.txBase64, 'base64').length });
    }
    if (req.method === 'POST' && url?.pathname === '/badge/signature') {
      const { id, sig } = await readBody(req);
      if (id !== job.id) return json(res, 409, { ok: false, reason: 'stale_id' });   // signed an old build: poll again
      const out = await finish(job, Buffer.from(String(sig), 'base64'));
      json(res, out.ok ? 200 : 400, out);
      if (out.ok) { clearInterval(rebuild); server.close(); }
      else console.error(`r3: badge signature rejected: ${out.reason}`);
      return;
    }
    if (req.method === 'POST' && url?.pathname === '/badge/outcome') {
      const body = await readBody(req);
      console.log(`r3: badge reported ${JSON.stringify(body)} - not signed, still queued`);
      return json(res, 200, { ok: true });
    }
    json(res, 404, { ok: false, reason: 'not_found' });
  } catch (err) {
    console.error(`r3: ${errMsg(err)}`);
    json(res, 500, { ok: false, reason: 'error' });
  }
});
server.listen(Number(arg.port), arg.host, () => {
  const ip = Object.values(networkInterfaces()).flat().find(i => i.family === 'IPv4' && !i.internal)?.address ?? arg.host;
  console.log(`r3: queued ${job.id}. Badge polls http://${ip}:${arg.port}/badge/pending?badge=${payer}`);
  console.log(`r3: and POSTs {"id","sig"(base64)} to http://${ip}:${arg.port}/badge/signature. Ctrl+C to stop.`);
});
