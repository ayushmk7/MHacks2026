// node scripts/db.mjs <migrate | seed | reset --yes>
import { readFileSync } from 'node:fs';
import { resolve } from 'node:path';
import { pathToFileURL } from 'node:url';
import pg from 'pg';
import { env, ROOT, loadBadges } from '../server/src/config.js';
import { initDb, syncBadges, closeDb } from '../server/src/db.js';

// Strips full-line `--` comments, then splits on `;` outside `$$ … $$`.
// The .sql files follow the matching authoring rules (see the header of db/schema.sql).
export function splitSql(sql) {
  const out = []; let buf = '';
  for (const part of sql.replace(/^\s*--.*$/gm, '').split(/(\$\$[\s\S]*?\$\$)/)) {
    if (part.startsWith('$$')) { buf += part; continue; }
    const bits = part.split(';');
    bits.forEach((b, i) => { buf += b; if (i < bits.length - 1) { if (buf.trim()) out.push(buf.trim()); buf = ''; } });
  }
  if (buf.trim()) out.push(buf.trim());
  return out;
}

// One query per statement: a multi-statement string is one implicit transaction, and
// refresh_continuous_aggregate and the policy calls refuse to run inside one.
async function runFile(client, file) {
  for (const stmt of splitSql(readFileSync(resolve(ROOT, file), 'utf8'))) {
    try { await client.query(stmt); }
    catch (err) { throw new Error(`${file}: ${err.message}\n  in: ${stmt.split('\n')[0].slice(0, 100)}`); }
  }
}

async function migrate(client) {
  await runFile(client, 'db/schema.sql');
  const { badges, invalid } = loadBadges();
  await initDb();
  await syncBadges(badges).finally(closeDb);
  if (invalid.length) console.warn('badges.json problems:', invalid.join('; '));
  console.log(`migrated: schema applied, ${badges.length} badges synced (${badges.filter(b => b.pubkey).length} configured)`);
}

async function seed(client) {
  await runFile(client, 'db/seed.sql');
  const { rows: [r] } = await client.query(`
    SELECT (SELECT count(*) FROM payments) AS rows, s.total_chunks, s.number_compressed_chunks,
           s.before_compression_total_bytes AS before, s.after_compression_total_bytes AS after
    FROM (SELECT 1) AS one LEFT JOIN hypertable_columnstore_stats('payments') s ON true`);
  const ratio = Number(r.before) ? (1 - Number(r.after) / Number(r.before)).toFixed(3) : null;
  console.log(`seeded: ${r.rows} payment rows, ${r.number_compressed_chunks ?? 0}/${r.total_chunks ?? 0} chunks in columnstore`);
  console.log(`compression: ${r.before ?? 0} -> ${r.after ?? 0} bytes, ratio ${ratio}`);
}

async function main() {
  const [cmd, flag] = process.argv.slice(2);
  if (!['migrate', 'seed', 'reset'].includes(cmd)) { console.error('usage: node scripts/db.mjs <migrate | seed | reset --yes>'); process.exit(2); }
  if (cmd === 'reset' && flag !== '--yes') { console.error('reset drops every BadgePay table. Re-run with --yes to confirm.'); process.exit(2); }
  const client = new pg.Client({ connectionString: env.DATABASE_URL });
  try {
    await client.connect();
    if (cmd === 'reset') {
      await client.query('DROP MATERIALIZED VIEW IF EXISTS payments_1m CASCADE');
      await client.query('DROP MATERIALIZED VIEW IF EXISTS payee_volume_1h CASCADE');
      await client.query('DROP TABLE IF EXISTS payments, attack_attempts, attestations, badges CASCADE');
    }
    await (cmd === 'seed' ? seed : migrate)(client);
  } catch (err) {
    console.error(`db ${cmd} failed: ${err.message || err.code}`);
    process.exitCode = 1;
  } finally { await client.end().catch(() => {}); }
}

if (import.meta.url === pathToFileURL(process.argv[1]).href) main();
