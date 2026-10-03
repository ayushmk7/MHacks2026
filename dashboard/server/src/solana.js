import { readFileSync, writeFileSync, mkdirSync, chmodSync, existsSync } from 'node:fs';
import { dirname } from 'node:path';
import {
  address, createDefaultRpcTransport, createSolanaRpcFromTransport, createSolanaRpcSubscriptions, getAddressEncoder,
  createKeyPairSignerFromBytes, createKeyPairSignerFromPrivateKeyBytes, createNoopSigner,
  pipe, createTransactionMessage, setTransactionMessageFeePayer, setTransactionMessageFeePayerSigner,
  setTransactionMessageLifetimeUsingBlockhash, appendTransactionMessageInstructions,
  compileTransaction, getBase64EncodedWireTransaction, signTransactionMessageWithSigners,
  getSignatureFromTransaction, sendAndConfirmTransactionFactory, fetchEncodedAccount,
} from '@solana/kit';
import { TOKEN_PROGRAM_ADDRESS, findAssociatedTokenPda, getTransferCheckedInstruction,
         getCreateAssociatedTokenIdempotentInstructionAsync } from '@solana-program/token';
import { env, token } from './config.js';

// createSolanaRpc(url) plus a 10 s timeout per request: fetch has none, so a stalled endpoint would hang boot and
// every route that reads the chain. Timed out, it is an ordinary RPC error (the rpc_unreachable gap, a 502).
const transport = createDefaultRpcTransport({ url: env.RPC_URL });
export const rpc = createSolanaRpcFromTransport(({ signal, ...req }) =>
  transport({ ...req, signal: AbortSignal.any([AbortSignal.timeout(10_000), ...(signal ? [signal] : [])]) }));
export const rpcSubs = createSolanaRpcSubscriptions(env.WS_URL);
export const rpcState = { ok: null };
const sendAndConfirm = sendAndConfirmTransactionFactory({ rpc, rpcSubscriptions: rpcSubs });
const sleep = ms => new Promise(r => setTimeout(r, ms));

// Solana errors keep the useful part (program logs, custom error) on .cause.
export const errMsg = err => [err?.message, err?.cause?.message].filter(Boolean).join(': ') || 'unknown error';

// File format = Solana CLI keypair (64 bytes: 32 seed + 32 pubkey), so `solana airdrop -k` also works.
// Created 0600 inside a 0700 directory. The secret is never logged or returned: the signer holds a non-extractable CryptoKey.
export async function loadOrCreateSigner(path) {
  if (!existsSync(path)) {
    const seed = crypto.getRandomValues(new Uint8Array(32));
    const s = await createKeyPairSignerFromPrivateKeyBytes(seed);
    const bytes = new Uint8Array(64); bytes.set(seed); bytes.set(getAddressEncoder().encode(s.address), 32);
    mkdirSync(dirname(path), { recursive: true, mode: 0o700 });
    chmodSync(dirname(path), 0o700);
    writeFileSync(path, JSON.stringify([...bytes]), { mode: 0o600, flag: 'wx' });
    chmodSync(path, 0o600);
  }
  let bytes;
  try { bytes = new Uint8Array(JSON.parse(readFileSync(path, 'utf8'))); }
  catch { throw new Error(`${path} is not a readable Solana CLI keypair file`); }   // never rethrow the parser's message: it quotes file content
  return createKeyPairSignerFromBytes(bytes);
}

export async function sendIxs(signer, ixs) {
  const { value: bh } = await rpc.getLatestBlockhash({ commitment: 'confirmed' }).send();
  const msg = pipe(
    createTransactionMessage({ version: 0 }),
    m => setTransactionMessageFeePayerSigner(signer, m),
    m => setTransactionMessageLifetimeUsingBlockhash(bh, m),
    m => appendTransactionMessageInstructions(ixs, m),
  );
  const tx = await signTransactionMessageWithSigners(msg);   // also picks up extra signers embedded in ixs
  await sendAndConfirm(tx, { commitment: 'confirmed' });
  return getSignatureFromTransaction(tx);
}

// ponytail: first top-level transferChecked for our mint only; inner (CPI) transfers and multi-transfer txs are ignored.
export function parseTransfer(tx, mint) {
  if (!tx || tx.meta?.err) return null;
  const ix = tx.transaction.message.instructions.find(i =>
    i.program === 'spl-token' && i.parsed?.type === 'transferChecked' && i.parsed.info.mint === mint);
  if (!ix) return null;
  const { source, destination, authority, multisigAuthority, tokenAmount } = ix.parsed.info;
  const keys = tx.transaction.message.accountKeys.map(k => String(k.pubkey));
  const bals = [...(tx.meta.postTokenBalances ?? []), ...(tx.meta.preTokenBalances ?? [])];
  const ownerOf = acct => bals.find(b => keys[b.accountIndex] === acct)?.owner ?? null;
  const payee = ownerOf(destination);
  if (!payee) return null;
  return { payer: ownerOf(source) ?? authority ?? multisigAuthority, payee,
           payerTokenAccount: source, payeeTokenAccount: destination,
           amountRaw: String(tokenAmount.amount), decimals: Number(tokenAmount.decimals),   // kit parses JSON integers as BigInt
           slot: Number(tx.slot), blockTime: tx.blockTime == null ? null : Number(tx.blockTime) };
}

// Public devnet allows about 10 getTransaction calls per 10 s per IP, and a 429 still counts against that limit, so
// un-paced retries keep it saturated and the live path never recovers. Every call takes a slot: starts are 1.1 s apart,
// a 429 pushes the next slot out 5 s, and backfill waits while a live signature is queued.
// ponytail: fixed for public devnet; turn TX_GAP_MS into an env knob if the team moves to a paid RPC endpoint.
const TX_GAP_MS = 1100;
let nextSlot = 0, liveQueued = 0;
async function txSlot(live) {
  if (live) liveQueued++;
  else while (liveQueued) await sleep(250);
  const at = Math.max(Date.now(), nextSlot);
  nextSlot = at + TX_GAP_MS;
  await sleep(at - Date.now());
  if (live) liveQueued--;
}

// A just-confirmed signature can take a moment to be servable: up to 4 tries, one slot each.
export async function getTxWithRetry(signature, live = false) {
  let lastErr;
  for (let i = 0; i < 4; i++) {
    await txSlot(live);
    lastErr = null;
    const tx = await rpc.getTransaction(signature, { commitment: 'confirmed', encoding: 'jsonParsed', maxSupportedTransactionVersion: 0 })
      .send().catch(err => { lastErr = err; return null; });
    if (tx) return tx;
    if (lastErr && errMsg(lastErr).includes('429')) nextSlot = Math.max(nextSlot, Date.now() + 5000);
  }
  if (lastErr) console.error('[solana] getTransaction failed, the safety poll retries:', errMsg(lastErr));
  return null;
}

export async function ataOf(owner, mint) {
  return (await findAssociatedTokenPda({ owner: address(owner), mint: address(mint), tokenProgram: TOKEN_PROGRAM_ADDRESS }))[0];
}

// One getMultipleAccounts call for every badge wallet + token account, cached 5 s.
// Returns Map<pubkey, { sol, hack, tokenAccount }>. Values are null when the RPC or the mint is unavailable.
let balCache = { at: 0, map: new Map() };
export async function getBalances(badges, mint = token.found ? token.mint : null) {
  if (Date.now() - balCache.at < 5000) return balCache.map;
  const live = badges.filter(b => b.pubkey);
  const map = new Map();
  try {
    const tas = mint ? await Promise.all(live.map(b => b.tokenAccount ?? ataOf(b.pubkey, mint))) : [];
    const { value } = live.length
      ? await rpc.getMultipleAccounts([...live.map(b => address(b.pubkey)), ...tas.map(t => address(t))], { encoding: 'jsonParsed', commitment: 'confirmed' }).send()
      : { value: [] };
    live.forEach((b, i) => map.set(b.pubkey, {
      sol: Number(value[i]?.lamports ?? 0n) / 1e9,
      hack: mint ? Number(value[live.length + i]?.data?.parsed?.info?.tokenAmount?.uiAmountString ?? 0) : null,
      tokenAccount: tas[i] ?? null,
    }));
    rpcState.ok = true;
  } catch (err) {
    rpcState.ok = false;
    console.error('[solana] balances unavailable:', errMsg(err));
  }
  balCache = { at: Date.now(), map };
  return map;
}

export async function ensureAta(payerSigner, owner, mint) {
  const ata = await ataOf(owner, mint);
  if (!(await fetchEncodedAccount(rpc, ata, { commitment: 'confirmed' })).exists) {
    await sendIxs(payerSigner, [await getCreateAssociatedTokenIdempotentInstructionAsync({ payer: payerSigner, owner: address(owner), mint: address(mint) })]);
  }
  return ata;
}

// Attack console (PRD F12): an ordinary, UNSIGNED SPL transferChecked whose real amount differs from the
// amount the checkout claims. The server never holds the victim key: the victim is a no-op signer, kit
// writes 64 zero bytes where the signature would go, and the server never submits this transaction.
// The badge must decode these bytes and show the human the real amount ("what you see is what you sign").
export async function buildTamperedTx({ victim, victimTokenAccount, attacker, mint, decimals, actualAmountRaw }) {
  const destTokenAccount = await ataOf(attacker, mint);
  const ix = getTransferCheckedInstruction({
    source: address(victimTokenAccount), mint: address(mint), destination: destTokenAccount,
    authority: createNoopSigner(address(victim)), amount: BigInt(actualAmountRaw), decimals,
  });
  const { value: bh } = await rpc.getLatestBlockhash({ commitment: 'confirmed' }).send();
  // BADGE-GAP(attack-payload): legacy vs v0 message. Which one the badge decoder accepts is unknown until firmware exists.
  const compiled = compileTransaction(pipe(
    createTransactionMessage({ version: env.ATTACK_TX_VERSION === '0' ? 0 : 'legacy' }),
    m => setTransactionMessageFeePayer(address(victim), m),
    m => setTransactionMessageLifetimeUsingBlockhash(bh, m),
    m => appendTransactionMessageInstructions([ix], m),
  ));
  const txBase64 = getBase64EncodedWireTransaction(compiled);
  return {
    txBase64, messageBase64: Buffer.from(compiled.messageBytes).toString('base64'),
    txBytes: Buffer.from(txBase64, 'base64').length, txVersion: env.ATTACK_TX_VERSION,   // wire size: signature slots + message
    blockhash: bh.blockhash, lastValidBlockHeight: Number(bh.lastValidBlockHeight), destTokenAccount,
  };
}
