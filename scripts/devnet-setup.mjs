// npm run devnet:setup — idempotent: re-run until it is green.
// Authority, stand-in badges, airdrop, HACK mint, funding, attacker token account, registry + first attestation.
import { readFileSync, writeFileSync, existsSync, copyFileSync } from 'node:fs';
import { resolve } from 'node:path';
import { address, airdropFactory, lamports, generateKeyPairSigner, fetchEncodedAccount } from '@solana/kit';
import { getCreateAccountInstruction, getTransferSolInstruction } from '@solana-program/system';
import { TOKEN_PROGRAM_ADDRESS, getMintSize, getInitializeMint2Instruction, getMintToInstruction,
         getCreateAssociatedTokenIdempotentInstructionAsync, fetchMaybeMint } from '@solana-program/token';
import { env, ROOT, BADGES_FILE, loadBadges } from '../server/src/config.js';
import { rpc, rpcSubs, loadOrCreateSigner, sendIxs, ataOf, ensureAta, errMsg } from '../server/src/solana.js';
import { initDb, dbState, syncBadges } from '../server/src/db.js';
import { registryState, initRegistry, ensureRegistry, issue, attestationPda } from '../server/src/registry.js';

const sleep = ms => new Promise(r => setTimeout(r, ms));
const solOf = async addr => Number((await rpc.getBalance(addr, { commitment: 'confirmed' }).send()).value) / 1e9;
const MIN_SOL = 0.3;   // a full first run spends about 0.25 SOL (4 badges x 0.05 + rent)

async function main() {
  // 1. authority (public key only)
  const authority = await loadOrCreateSigner(env.AUTHORITY_KEYPAIR);
  console.log(`authority  ${authority.address}`);

  // 2. stand-in badges. Needs no SOL, so it runs before the faucet can fail.
  //    BADGE-GAP(badge-pubkeys, key-location): software keypairs fill empty slots only.
  //    A slot that already has a pubkey is never touched.
  const cfg = JSON.parse(readFileSync(BADGES_FILE, 'utf8'));
  const empty = cfg.badges.filter(b => b.pubkey == null);
  for (const b of empty) {
    const signer = await loadOrCreateSigner(resolve(ROOT, `server/.keys/standin-${b.id}.json`));
    Object.assign(b, { pubkey: signer.address, keyLocation: 'software', standIn: true });
    console.log(`badge ${b.id}    stand-in ${signer.address}`);
  }
  if (empty.length) writeFileSync(BADGES_FILE, `${JSON.stringify(cfg, null, 2)}\n`);
  const { badges } = loadBadges();

  // 3. airdrop. The public faucet is often rate-limited, so try smaller amounts before giving up.
  let sol = await solOf(authority.address);
  if (sol < 0.5) {
    const airdrop = airdropFactory({ rpc, rpcSubscriptions: rpcSubs });
    for (const amount of [1, 0.5, 0.5]) {
      try {
        await airdrop({ recipientAddress: authority.address, lamports: lamports(BigInt(amount * 1e9)), commitment: 'confirmed' });
        console.log(`airdropped ${amount} SOL`);
        break;
      } catch (err) { console.warn(`airdrop of ${amount} SOL failed: ${errMsg(err)}`); await sleep(4000); }
    }
    sol = await solOf(authority.address);
    if (sol < MIN_SOL) {
      console.error(`Faucet rate-limited. Fund ${authority.address} at https://faucet.solana.com then re-run.`);
      process.exit(1);
    }
  }
  console.log(`balance    ${sol} SOL`);

  // 4. mint. BADGE-GAP(hack-mint): a stand-in mint the authority controls, unless HACK_MINT already points at a real one.
  let mint = env.HACK_MINT, decimals = env.HACK_DECIMALS;
  const existing = mint ? await fetchMaybeMint(rpc, address(mint), { commitment: 'confirmed' }) : null;
  if (existing?.exists) decimals = existing.data.decimals;
  else {
    const mintSigner = await generateKeyPairSigner();
    const space = BigInt(getMintSize());
    await sendIxs(authority, [
      getCreateAccountInstruction({ payer: authority, newAccount: mintSigner, space, programAddress: TOKEN_PROGRAM_ADDRESS,
                                    lamports: await rpc.getMinimumBalanceForRentExemption(space).send() }),
      getInitializeMint2Instruction({ mint: mintSigner.address, decimals, mintAuthority: authority.address }),
    ]);
    mint = mintSigner.address;
    const envFile = resolve(ROOT, '.env');
    if (!existsSync(envFile)) copyFileSync(resolve(ROOT, '.env.example'), envFile);
    const text = readFileSync(envFile, 'utf8');
    writeFileSync(envFile, /^HACK_MINT=.*$/m.test(text) ? text.replace(/^HACK_MINT=.*$/m, `HACK_MINT=${mint}`) : `${text}\nHACK_MINT=${mint}\n`);
    console.log(`created mint, wrote HACK_MINT to .env`);
  }
  console.log(`mint       ${mint} (${decimals} decimals)`);

  // 5. fund every configured badge: SOL for fees, token account, 1000 HACK. One transaction per badge.
  const summary = [];
  for (const b of badges.filter(b => b.pubkey)) {
    const owner = address(b.pubkey);
    const ta = b.tokenAccount ? address(b.tokenAccount) : await ataOf(owner, mint);
    const read = async () => {
      const { value: [wallet, tok] } = await rpc.getMultipleAccounts([owner, ta], { encoding: 'jsonParsed', commitment: 'confirmed' }).send();
      return { sol: Number(wallet?.lamports ?? 0n) / 1e9, tok: !!tok, hack: Number(tok?.data?.parsed?.info?.tokenAmount?.uiAmountString ?? 0) };
    };
    let s = await read();
    const ixs = [];
    if (s.sol < 0.02) ixs.push(getTransferSolInstruction({ source: authority, destination: owner, amount: lamports(50_000_000n) }));
    if (!s.tok && !b.tokenAccount) ixs.push(await getCreateAssociatedTokenIdempotentInstructionAsync({ payer: authority, owner, mint: address(mint) }));
    if (s.hack < 100) ixs.push(getMintToInstruction({ mint: address(mint), token: ta, mintAuthority: authority, amount: BigInt(1000 * 10 ** decimals) }));
    if (ixs.length) { await sendIxs(authority, ixs); s = await read(); }
    summary.push({ id: b.id, label: b.label, pubkey: b.pubkey, standIn: b.standIn, sol: s.sol, hack: s.hack, funded: ixs.length ? 'now' : 'already' });
  }

  // 6. attacker token account (where a signed attack transfer would land)
  const attacker = env.ATTACKER_PUBKEY || authority.address;
  await ensureAta(authority, attacker, mint);

  // 7. registry + first attestation. Badge 1 becomes "MHacks Merch", the rest stay unverified so the feed shows both states.
  await initDb();
  if (dbState.schema) await syncBadges(badges);
  else console.warn('database not reachable or not migrated: the server mirrors attestations on its next start');
  await initRegistry(authority);
  await ensureRegistry();
  const merchant = badges.find(b => b.id === 1 && b.pubkey);
  let attested = 'badge 1 not configured';
  if (merchant) {
    const pda = await attestationPda(merchant.pubkey);
    if ((await fetchEncodedAccount(rpc, pda, { commitment: 'confirmed' })).exists) attested = `already issued (${pda})`;
    else attested = `issued "MHacks Merch" to badge 1, tx ${(await issue(merchant.pubkey, 'MHacks Merch')).signature}`;
  }

  // 8. summary
  console.table(summary);
  console.log(`credential ${registryState.credential}\nschema     ${registryState.schema}\nattestation ${attested}`);
  console.log(`attacker   ${attacker}\nauthority  ${await solOf(authority.address)} SOL left`);
  console.log('\nDone. Restart `npm run server` so it picks up HACK_MINT from .env.');
}

main().then(() => process.exit(0), err => { console.error(`devnet setup failed: ${errMsg(err)}`); process.exit(1); });
