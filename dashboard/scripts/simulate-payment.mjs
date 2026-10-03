// npm run pay -- --from <id> --to <id> --amount <n> [--every <sec>]
// A real devnet transferChecked between stand-in badges, signed by the sender's software key.
import { existsSync } from 'node:fs';
import { resolve } from 'node:path';
import { parseArgs } from 'node:util';
import { address } from '@solana/kit';
import { getTransferCheckedInstruction, fetchMint } from '@solana-program/token';
import { env, ROOT, loadBadges } from '../server/src/config.js';
import { rpc, loadOrCreateSigner, sendIxs, ataOf, errMsg } from '../server/src/solana.js';

const die = msg => { console.error(msg); process.exit(1); };
const { values: arg } = parseArgs({ options: {
  from: { type: 'string', default: '3' }, to: { type: 'string', default: '1' },
  amount: { type: 'string', default: '10' }, every: { type: 'string' } } });

const { badges } = loadBadges();
const badge = id => badges.find(b => b.id === Number(id) && b.pubkey) ?? die(`Badge ${id} is not configured. Run \`npm run devnet:setup\`.`);
const from = badge(arg.from), to = badge(arg.to);
const keyFile = resolve(ROOT, `server/.keys/standin-${from.id}.json`);
if (!from.standIn || !existsSync(keyFile)) die(`Badge ${from.id} is a real badge: its key never leaves the badge. Use the badge itself.`);
if (!env.HACK_MINT) die('HACK_MINT is not set. Run `npm run devnet:setup`.');
const amount = Number(arg.amount), every = arg.every == null ? 0 : Number(arg.every);
if (!(amount > 0) || !(every >= 0)) die('--amount must be a positive number and --every a number of seconds');

try {
  const signer = await loadOrCreateSigner(keyFile);
  if (signer.address !== from.pubkey) die(`${keyFile} does not match badge ${from.id} in badges.json`);
  const mint = address(env.HACK_MINT);
  const { decimals } = (await fetchMint(rpc, mint)).data;
  const ix = getTransferCheckedInstruction({
    source: from.tokenAccount ? address(from.tokenAccount) : await ataOf(from.pubkey, mint), mint,
    destination: to.tokenAccount ? address(to.tokenAccount) : await ataOf(to.pubkey, mint),
    authority: signer, amount: BigInt(Math.round(amount * 10 ** decimals)), decimals });
  do {
    const sig = await sendIxs(signer, [ix]).catch(err => console.error(`payment failed: ${errMsg(err)}`));
    if (sig) console.log(`${amount} ${env.HACK_SYMBOL}  badge ${from.id} -> badge ${to.id}  https://explorer.solana.com/tx/${sig}?cluster=devnet`);
    else if (!every) process.exit(1);
    if (every) await new Promise(r => setTimeout(r, every * 1000));
  } while (every);
  process.exit(0);
} catch (err) { die(`payment failed: ${errMsg(err)}`); }
