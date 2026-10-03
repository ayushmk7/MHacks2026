# Architecture

BadgePay dev panel: the laptop dashboard from the PRD ("Dashboard (laptop)", F12 and F13). It shows every HACK payment live, issues and revokes badge verifications, and builds the tampered transfer for the attack demo. Badges never talk to the dashboard for payments; it reads everything from devnet.

## Components

| Part | Where | What it is |
|---|---|---|
| Web panel | `web/` | React 19 + Vite, plain JS, no router or chart library. Served on `127.0.0.1:5173`, proxies `/api` to the server |
| API server | `server/src/` | Node ESM on `node:http` (no framework). Listens on `127.0.0.1:8787` |
| Database | `db/`, `docker-compose.yml` | TimescaleDB 2.30.2 (PostgreSQL 17) in Docker on `127.0.0.1:5433` |
| Scripts | `scripts/` | Database migrate/seed/reset, devnet setup, payment simulator |
| Solana devnet | external | HACK SPL mint, token accounts, Solana Attestation Service program |

Server modules:

| File | Job |
|---|---|
| `index.js` | Boot order, the 30 s tick, wiring NOTIFY to SSE |
| `config.js` | `.env` parsing, `badges.json` loading and validation, `computeGaps()` |
| `db.js` | Connection pool, the `LISTEN` session, every SQL string (`Q`), row-to-JSON mappers |
| `solana.js` | RPC clients, keypair files, send helper, `parseTransfer`, balances, the attack transaction builder |
| `ingest.js` | devnet subscription + backfill poll, insert into `payments` |
| `registry.js` | Solana Attestation Service: derive, issue, revoke, sync |
| `http.js` | Route table, validation, SSE hub, badge listener |

Runtime dependencies: `@solana/kit` 5.5.1, `@solana-program/token`, `@solana-program/system`, `sas-lib` 1.0.10, `pg`. No dotenv (`node --env-file-if-exists`), no nodemon, no express.

## Data flow

```
 badge or `npm run pay`
        |  SPL transferChecked (HACK)
        v
 Solana devnet
        |  logsSubscribe(mentions: HACK mint), commitment confirmed      [fast path]
        |  getSignaturesForAddress(mint) every 15 s                      [safety net]
        v
 server/src/ingest.js
        |  getTransaction(jsonParsed) -> parseTransfer() -> payer, payee, amount, slot, block time
        |  match against pending attack attempts (same victim, attacker, raw amount)
        v
 INSERT INTO payments  (hypertable; payee's verification status snapshotted in the same statement)
        |  trigger payments_notify -> pg_notify('feed', {type:'payment', id:<signature>})
        v
 server LISTEN session (db.js) -> re-read the row -> broadcast()
        |  SSE  GET /api/events   data: {"type":"payment","data":{…}}
        v
 web/src/data.jsx  (one EventSource for the whole app)
        |  merge into the payments list, bump the stat tiles, refetch stats/badges (debounced 250 ms)
        v
 Feed page: a new block slides in at the head of the chain
```

Attestation and attack changes take the same path from the trigger onward (`{type:'attestation'|'attack', id}`); the browser refetches the affected list.

### Ingest details (`server/src/ingest.js`)

- Starts only once `HACK_MINT` is set and the mint account is found on chain. Until then `ingest.state` is `idle`.
- Every `transferChecked` lists the mint as an account, so one `logsSubscribe` on the mint catches all HACK payments. It also catches mint-to and create-account transactions; `parseTransfer` returns `null` for those.
- Dedupe has three layers: an in-memory `seen` set, the `until` cursor of the backfill (newest chain signature in the database), and `UNIQUE (signature, block_time)` with `ON CONFLICT DO NOTHING`.
- The backfill runs after every (re)subscribe and every 15 s. It walks at most 200 signatures, oldest first, one transaction per 1.1 s, because public devnet allows about 10 `getTransaction` calls per 10 s per IP and a burst would starve the live path.
- `getTransaction` is retried 4 times (400 ms apart, 1.5 s after an RPC error).
- The subscription reconnects with backoff (1 s doubling to 30 s). State goes `backfilling` -> `live` -> `reconnecting`.
- `ingest_lag_ms` (block time to insert) is recorded for live-path rows only.

Validated by ingesting live devnet USDC traffic through this pipeline (lag 1.0 to 1.6 s). Not yet run with our own HACK mint; see [RUNBOOK.md](RUNBOOK.md).

### Boot order (`server/src/index.js`)

1. Load `server/config/badges.json`.
2. Load or create the authority keypair. Log the public key only.
3. Derive the registry PDAs and check whether they exist (read-only).
4. Connect to Postgres. Does not block boot if it is down; a 5 s health check keeps trying. When the schema is present: sync `badges.json` into the `badges` table, then sync attestations.
5. Tick now and every 30 s: authority balance, find the mint (then start the ingest), sync attestations from chain, broadcast `status` if the gap list changed.
6. Start the `LISTEN` session.
7. Start HTTP (and the badge listener if `BADGE_LISTEN_HOST` is set).

Every RPC or database failure after step 2 becomes a gap in `/api/status`, never a crash.

## Database model

Schema: `db/schema.sql` (idempotent). Details and measured numbers: [TIGER-DATA.md](TIGER-DATA.md).

| Object | Kind | Notes |
|---|---|---|
| `payments` | hypertable, 1-hour chunks, columnstore | One row per transfer. Unique on `(signature, block_time)`. `payee_status` / `payee_name` are a snapshot at insert time. `source` is `chain` or `seed` |
| `attack_attempts` | hypertable, 1-day chunks, rowstore | One row per built attack. `outcome`: `pending`, `signed`, `rejected` |
| `badges` | table | Mirror of `server/config/badges.json` (the file is the source of truth; the table is replaced on every server start and `db:migrate`) |
| `attestations` | table | Mirror of the on-chain registry (the chain is the source of truth; reconciled every 30 s) |
| `payments_1m` | continuous aggregate, real-time | Per minute and `source`: count, verified count, volume |
| `payee_volume_1h` | continuous aggregate, real-time | Per hour, payee and source: count, volume |
| `notify_feed()` + 3 triggers | function | `pg_notify('feed', {type, id})` on payment insert (non-seed), attack insert/update, attestation insert/update |

All SQL the server runs is in the `Q` object in `server/src/db.js`.

## Registry (Solana Attestation Service)

`server/src/registry.js`, program `22zoJMtdu4tQc2PzL74ZUT7FrwgB1Udec8DdW4yw4BdG`.

- One credential, named `MHacks Verified`, owned by the laptop's authority key.
- One schema, `badge-identity` version 1, with a single string field `name`.
- One attestation per badge. The badge public key is the attestation nonce, so each badge has exactly one attestation address.
- Issue = create the attestation (30-day expiry). If one exists, close and re-create in one transaction.
- Revoke = close the attestation account. "No account" means "not verified".
- The credential and schema are created lazily by the first issue (they cost SOL), not at boot.
- Every 30 s the server reads the attestation accounts for all configured badges and reconciles the `attestations` table. A row that says `verified` with no account on chain is flipped to `revoked`.

The PRD's signed-allowlist fallback was not implemented. SAS is the only path.

## Attack console

`buildTamperedTx` in `server/src/solana.js` builds an ordinary SPL `transferChecked` from the victim badge's token account to the attacker's, for the real amount (default 500 HACK), and returns it unsigned. The page shows the claimed amount (default 5 HACK) next to the decoded real one.

- The server never holds a badge key and never submits the transaction. The victim is a no-op signer; the wire format carries 64 zero bytes where the signature goes.
- The victim must be a pubkey from `badges.json`. The console cannot target other wallets.
- Delivery to the badge is a stub: the badge polls `GET /badge/pending` on the separate badge listener. See [BADGE-GAPS.md](BADGE-GAPS.md).
- If the transfer ever lands on chain, the ingest matches it (victim, attacker, raw amount) and flips the attempt to `signed`. "Rejected" is marked by hand or by the badge.

## Mode switch (DEMO / LIVE)

One module, `web/src/data.jsx`, with two transports behind the same hooks (`useData`, `useApi`, `useStatus`, `useGaps`). Pages do not know which mode is active.

| | DEMO (default) | LIVE |
|---|---|---|
| Reads | `demoGet()` in `web/src/demo.js`, in memory, 350 ms artificial delay | `fetch('/api/…')` |
| Writes | `demoPost()`, in memory, 700 ms delay | `POST /api/…` |
| Realtime | `startDemoTicker()` mints a fake payment about every 4 s (10 s under reduced motion) | one `EventSource('/api/events')` |
| Gaps | three fixed badge-dependent gaps, so the placeholders are visible (`key_location_unknown`, `attack_delivery_unconfigured`, `attack_outcome_manual`) | whatever `/api/status` reports |
| Needs | nothing | server + database |

- The choice is stored in `localStorage['badgepay.mode']`. Default is `demo`, so a first-time visitor sees blocks arriving with no setup.
- Switching mode clears the client store. Demo data is never shown under a LIVE label.
- Both realtime sources call the same `onEvent({type, data})`, so the block animation is identical.
- Demo fixtures have the same shapes as the API. `web/src/demo.test.js` asserts that.
- Demo payments carry `source: "demo"` and are drawn dashed with a `~demo` tag; demo keys start with `Demo`.
- Theme is a separate toggle stored in `localStorage['badgepay.theme']`. An inline script in `web/index.html` sets it before first paint (saved choice, else the OS preference).

Routing is by hash: `#/feed` (default), `#/badges`, `#/registry`, `#/attack`. `App.jsx` loads pages with `import.meta.glob('./pages/*.jsx', { eager: true })`; a missing page file renders a "Page not built yet" placard instead of breaking the build. A page that throws is caught by an error boundary and shows "This page crashed".

## Gap system

A "gap" is one known reason the panel is degraded. The server computes the list (`computeGaps()` in `server/src/config.js`) and returns it in `/api/status.gaps`. Each gap has a `code`, a `severity`, a `title`, a one-sentence `fix` naming the exact file, variable or command, the `pages` it affects, and optionally a `badgeGap` id that ties it to a `BADGE-GAP(<id>)` marker in the code. The full table is in [API.md](API.md#gaps).

How the UI uses it (LIVE only):

| State | What renders |
|---|---|
| Backend unreachable | Red "Backend offline" banner under the top bar with a Retry button; the mode pill reads `OFFLINE`. Pages keep their last data |
| Blocker gap that affects all four pages (`db_offline`, `db_schema_missing`, `rpc_unreachable`) | A `GapCard` under the top bar on every page |
| Page-level gap | The page renders the `GapCard` itself via `useGaps(page)`. Blocker = panel with the fix, warn = strip, info = muted line. A `BADGE-GAP(<id>)` chip appears when the gap is waiting on badge hardware |
| Loading | Skeletons sized like the real content |
| No rows | An `Empty` placard with a hint |

The status poll (every 5 s) doubles as the backend heartbeat. When the backend comes back, everything mounted is refetched.

## Security

- The API binds `127.0.0.1`. A non-loopback `HOST` logs a warning at start.
- `Host` must be `localhost` or `127.0.0.1` (DNS-rebinding guard).
- POST requires `Content-Type: application/json` and, when an `Origin` header is present, one listed in `WEB_ORIGINS`. No CORS headers are ever sent, so a cross-site page cannot get a preflight through.
- Bodies are capped at 16 KB.
- The authority keypair (`server/.keys/authority.json`, mode 0600 in a 0700 directory, gitignored) signs registry transactions and pays fees. It never leaves the laptop, is never logged and never appears in a response. Only its public key is exposed.
- Error responses carry a code and a message, never a stack trace.
- The badge listener is a separate server on a separate port, off unless `BADGE_LISTEN_HOST` is set. It serves `/badge/pending` and `/badge/outcome` only; none of the admin API is reachable from the hotspot.
- The PRD also asks for a read-only feed published on a .tech domain. That is not built. The whole panel is localhost-only.

## Design system: "Terminal Ledger"

A monochrome financial terminal with one signal-green accent and two status colours. Built to be read from about 2 metres.

| | |
|---|---|
| Display | Barlow Condensed 200 / 800 (headings, uppercase) |
| Body | Barlow 300 / 600 |
| Data | JetBrains Mono 400 / 800 (numbers, hashes, slots) |
| Status colours | green `--ok` = verified / live; amber `--warn` = unverified / pending; red `--bad` = revoked / tampered / blocker. Same hues in both themes |
| Themes | `html[data-theme="dark"]` and `html[data-theme="light"]` redefine the surface and ink tokens |
| Tokens | `:root` in `web/src/styles.css` (fonts, spacing, radius, durations, block size) |
| Motion | One page-load stagger (`rise`), one block entrance (`blockIn` + green `ring`), fast toggles. All disabled under `prefers-reduced-motion` |

Fonts load from Google Fonts with system fallbacks, so the panel still works offline with fallback type. Pages add no colours; `web/src/pages/admin.css` only uses the tokens.

The chain strip (Feed): newest block at the left next to a `NEXT` placeholder, at most 30 blocks in the rail, older rows in the ledger table below. Block states: verified (green, check), unverified (amber, `?`), revoked (red, `⊘`), tampered (red double border, `!`, "TAMPERED — signed"), and off-chain (`seed` or `demo`: dashed border, muted, `~seed` / `~demo` tag). If the reader has scrolled right, new blocks do not yank the rail; a "N new" chip appears instead.

## Decisions

| Decision | Reason |
|---|---|
| Database NOTIFY drives the browser | One source of truth. Whatever inserts a row (ingest, a script, psql) updates every open panel. No polling loop in the server |
| NOTIFY payload is `{type, id}` only | The server re-reads the row with the REST query, so SSE and REST shapes cannot drift |
| Verification status is snapshotted per payment | A revoke must not rewrite what the feed showed at the time |
| `badges.json` is the source of truth for badges | Edit one file, restart the server, and the `badges` table is resynced at boot. No watch mode: with `--env-file-if-exists` Node 26.10 watched the whole project root, so Vite cache writes kept restarting the API |
| `sas-lib` 1.0.10 on `@solana/kit` 5.5.1 | The only published, non-beta SAS client at build time; it pins the kit major |
| Stand-in badges | Software keypairs fill empty badge slots so LIVE works before real hardware exists |
| Seed rows are marked and muted | Faucet insurance, but never passed off as live data |
| `signed` only from chain evidence | A client can only mark an attack `rejected`; `signed` requires the transfer to appear on devnet |

## Known shortcuts

Deliberate ceilings, each marked with a `ponytail:` comment in the code:

| Where | Ceiling |
|---|---|
| `server/src/ingest.js:12` | The `seen` signature set is never pruned (about 100 B per transaction; fine for a 24 h event) |
| `server/src/ingest.js:42` | Backfill reads one page of 200 signatures. A longer outage loses the older ones |
| `server/src/solana.js:52` | Only the first top-level `transferChecked` for our mint is parsed. Inner (CPI) transfers and multi-transfer transactions are ignored |
| `server/src/registry.js:25` | An attestation past its expiry still counts as verified (issue sets 30 days; the event lasts 24 h) |
| `server/src/http.js:117`, `web/src/pages/Feed.jsx:178` | `before` is a strict timestamp cursor. Rows sharing the page's last block time are skipped when paging |
| `server/src/http.js:226` | The claimed merchant name sent to the badge is fixed to "MHacks Merch"; only the amount lies |
| `web/src/data.jsx:45-46` | The set of mounted resources is never pruned; a refetch during an in-flight GET reuses that GET |
| `web/src/data.jsx:77` | Only the default `payments` list gets the live prepend; other payment queries refetch |
| `web/src/demo.js:93` | A live payment for an older minute is counted in the newest bar until the next `/api/stats` refetch |
| `web/src/pages/Feed.jsx:171` | If the base list is refetched after new blocks arrived, rows between it and the "load older" pages can go missing until reload |
| `web/src/pages/Feed.jsx:42` | The map of how each block entered the rail only grows (fine for one session) |
| `web/src/demo.js:61` | DEMO keeps at most 500 blocks in memory |
| `web/src/pages/Registry.jsx:8` | The client-side pubkey check is base58 shape only; the server does the real 32-byte decode |
| `web/src/pages/Attack.jsx:81` | The default victim is the first badge whose label contains "Judge" |
| `web/src/pages/Attack.jsx:106` | Amount fields use native number validation; the server re-checks |
| `web/src/ui.jsx:4` | Explorer links are hardcoded to devnet |
| `web/src/ui.jsx:175` | Amounts use `Number` (exact to 2^53 raw units) |

Find them all:

```bash
grep -rn "ponytail:" server/src scripts web/src db
```

## Tests

`npm run check` runs `node --test` over `server/src/check.test.js` (transfer parsing, validators, SQL splitter) and `web/src/demo.test.js` (demo fixtures match the API shapes, demo POST flows), then builds the web app. There are no automated UI tests; the UI was checked by hand with headless Chrome screenshots.
