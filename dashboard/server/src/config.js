import { readFileSync } from 'node:fs';
import { resolve } from 'node:path';
import { fileURLToPath } from 'node:url';
import { isAddress } from '@solana/kit';

export const ROOT = fileURLToPath(new URL('../../', import.meta.url));
export const BADGES_FILE = resolve(ROOT, 'server/config/badges.json');
const e = process.env;

export const env = Object.freeze({
  DATABASE_URL: e.DATABASE_URL || 'postgres://postgres:badgepay@127.0.0.1:5433/badgepay',
  RPC_URL: e.RPC_URL || 'https://api.devnet.solana.com',
  WS_URL: e.WS_URL || 'wss://api.devnet.solana.com',
  // BADGE-GAP(hack-mint): empty until `npm run devnet:setup` writes the stand-in mint, or the badge team supplies theirs.
  HACK_MINT: isAddress(e.HACK_MINT || '') ? e.HACK_MINT : '',
  HACK_SYMBOL: e.HACK_SYMBOL || 'HACK',
  HACK_DECIMALS: Number(e.HACK_DECIMALS || 2),
  AUTHORITY_KEYPAIR: resolve(ROOT, e.AUTHORITY_KEYPAIR || 'server/.keys/authority.json'),
  SAS_CREDENTIAL_NAME: e.SAS_CREDENTIAL_NAME || 'MHacks Verified',
  SAS_SCHEMA_NAME: e.SAS_SCHEMA_NAME || 'badge-identity',
  SAS_SCHEMA_VERSION: Number(e.SAS_SCHEMA_VERSION || 1),
  HOST: e.HOST || '127.0.0.1',
  PORT: Number(e.PORT || 8787),
  WEB_ORIGINS: (e.WEB_ORIGINS || 'http://localhost:5173,http://127.0.0.1:5173').split(',').map(s => s.trim().replace(/\/+$/, '')).filter(Boolean),   // an Origin header never ends in "/"
  ATTACKER_PUBKEY: isAddress(e.ATTACKER_PUBKEY || '') ? e.ATTACKER_PUBKEY : '',
  // BADGE-GAP(attack-payload): which message version the badge decoder parses is unknown until firmware exists.
  ATTACK_TX_VERSION: e.ATTACK_TX_VERSION === '0' ? '0' : 'legacy',
  // BADGE-GAP(attack-delivery): empty = the badge LAN listener stays closed.
  BADGE_LISTEN_HOST: e.BADGE_LISTEN_HOST || '',
  BADGE_LISTEN_PORT: Number(e.BADGE_LISTEN_PORT || 8788),
});

// Mint facts read from the chain at boot (index.js). decimals falls back to HACK_DECIMALS for seed-only display.
export const token = { mint: env.HACK_MINT || null, symbol: env.HACK_SYMBOL, decimals: env.HACK_DECIMALS, found: false };

const KEY_LOCATIONS = ['se050', 'software', 'unknown'];

// BADGE-GAP(badge-pubkeys, key-location): reads server/config/badges.json. Never throws: bad entries are
// reported in `invalid` (shown as the config_invalid gap) and their bad field is neutralised.
export function loadBadges() {
  const invalid = [];
  let raw;
  try { raw = JSON.parse(readFileSync(BADGES_FILE, 'utf8')).badges; }
  catch (err) { return { badges: [], invalid: [`badges.json unreadable: ${err.message}`] }; }
  if (!Array.isArray(raw)) return { badges: [], invalid: ['badges.json: "badges" must be an array'] };
  const badges = [];
  for (const b of raw) {
    const id = b?.id;
    if (!Number.isInteger(id) || id < 1 || id > 99 || badges.some(x => x.id === id)) { invalid.push(`badge id ${JSON.stringify(id)} is missing, out of range or duplicated`); continue; }
    const addr = (v, field) => (v == null ? null : isAddress(v) ? v : (invalid.push(`badge ${id}: ${field} is not a base58 address`), null));
    const pubkey = addr(b.pubkey, 'pubkey');
    if (pubkey && badges.some(x => x.pubkey === pubkey)) { invalid.push(`badge ${id}: pubkey duplicates another badge`); continue; }
    if (!KEY_LOCATIONS.includes(b.keyLocation)) invalid.push(`badge ${id}: keyLocation must be se050, software or unknown`);
    badges.push({
      id, label: typeof b.label === 'string' && b.label ? b.label : `Badge ${id}`, pubkey,
      keyLocation: KEY_LOCATIONS.includes(b.keyLocation) ? b.keyLocation : 'unknown',
      tokenAccount: addr(b.tokenAccount, 'tokenAccount'), standIn: b.standIn === true,
    });
  }
  return { badges, invalid };
}

const ALL = ['feed', 'badges', 'registry', 'attack'];
const gap = (code, severity, badgeGap, title, fix, pages) => ({ code, severity, badgeGap, title, fix, pages });

// state: { db:{ok,schema,error}, rpcOk, mintSet, mintFound, badges, invalid, authority, authoritySol, registryReady, listenerOpen }
export function computeGaps(s) {
  const out = [];
  const configured = s.badges.filter(b => b.pubkey);
  if (!s.db.ok) out.push(gap('db_offline', 'blocker', null, 'Database is offline', 'Run `npm run db:up` (or fix DATABASE_URL in .env), the server reconnects by itself.', ALL));
  else if (!s.db.schema) out.push(gap('db_schema_missing', 'blocker', null, 'Database schema is missing', 'Run `npm run db:migrate`.', ALL));
  if (s.rpcOk === false) out.push(gap('rpc_unreachable', 'blocker', null, 'Solana RPC is unreachable', 'Check the network and RPC_URL / WS_URL in .env.', ALL));
  if (!s.mintSet || !s.mintFound) out.push(gap('hack_mint_missing', 'blocker', 'hack-mint',
    s.mintSet ? 'HACK mint account not found on chain' : 'HACK mint not configured',
    'Run `npm run devnet:setup` or set HACK_MINT in .env, then restart `npm run server`.', ['feed', 'badges', 'attack']));
  if (!configured.length) out.push(gap('badges_unconfigured', 'blocker', 'badge-pubkeys', 'No badges configured',
    'Paste each badge\'s public key into server/config/badges.json (Settings > Identity on the badge), or run `npm run devnet:setup` for stand-ins.', ['badges', 'registry', 'attack']));
  else if (configured.length < s.badges.length) out.push(gap('badges_partial', 'warn', 'badge-pubkeys',
    `${s.badges.length - configured.length} of ${s.badges.length} badges not configured`,
    'Paste each badge\'s public key into server/config/badges.json (Settings > Identity on the badge).', ['badges', 'registry', 'attack']));
  if (s.badges.some(b => b.standIn)) out.push(gap('badges_stand_in', 'info', 'badge-pubkeys', 'Stand-in software badges in use',
    'Replace stand-in entries in server/config/badges.json with the real badge public keys and set standIn to false.', ['badges']));
  if (configured.some(b => b.keyLocation === 'unknown')) out.push(gap('key_location_unknown', 'warn', 'key-location', 'Key location unknown for a configured badge',
    'Set keyLocation to "se050" or "software" for each badge in server/config/badges.json.', ['badges']));
  if (s.invalid.length) out.push(gap('config_invalid', 'warn', null, `Invalid badges.json: ${s.invalid.join('; ')}`,
    'Fix the listed entries in server/config/badges.json.', ['badges']));
  if (s.authoritySol != null && s.authoritySol < 0.02) out.push(gap('authority_unfunded', 'warn', null, 'Registry authority has no SOL',
    `Fund ${s.authority} at https://faucet.solana.com (devnet) or run \`npm run devnet:setup\`.`, ['registry', 'attack']));
  if (!s.registryReady) out.push(gap('registry_uninitialised', 'info', null, 'Registry credential and schema not on chain yet',
    'Nothing to do: both are created by the first issue on the Registry page (or `npm run devnet:setup`).', ['registry']));
  if (!s.listenerOpen) out.push(gap('attack_delivery_unconfigured', 'warn', 'attack-delivery', 'Badge delivery listener is closed',
    'Set BADGE_LISTEN_HOST in .env to the laptop hotspot IP and restart `npm run server`.', ['attack']));
  out.push(gap('attack_outcome_manual', 'info', 'attack-outcome', 'Rejected outcomes are marked manually',
    'Use "Mark rejected" on the Attack page until badge firmware POSTs /badge/outcome. Signed is detected from chain.', ['attack']));
  return out;
}
