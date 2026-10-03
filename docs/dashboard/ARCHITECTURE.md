# Architecture

BadgePay dev panel: the laptop dashboard from the PRD ("Dashboard (laptop)", F12 and F13). It shows every HACK payment live, issues and revokes badge verifications, and builds the tampered transfer for the attack demo. Badges never talk to the dashboard for payments; it reads everything from devnet.

Paths in this document are relative to `dashboard/` (so `server/src/http.js:124` is `dashboard/server/src/http.js`, line 124). Commands run from `dashboard/`. Line numbers are as of 2026-10-03.

## Components

| Part | Where | What it is |
|---|---|---|
| Web panel | `web/` | React 19 + Vite 8, plain JS, no router or chart library. Served on `127.0.0.1:5173`, proxies `/api` to the server |
| API server | `server/src/` | Node ESM on `node:http` (no framework). Listens on `127.0.0.1:8787` |
| Database | `db/`, `docker-compose.yml` | TimescaleDB 2.30.2 (PostgreSQL 17) in Docker on `127.0.0.1:5433` |
| Scripts | `scripts/` | Database migrate/seed/reset, devnet setup, payment simulator |
| Solana devnet | external | HACK SPL mint, token accounts, Solana Attestation Service program |

`docker-compose.yml` sets `name: badgepay`. The compose project, the container (`badgepay-db-1`) and the volume (`badgepay_badgepay-db`) keep those names wherever the folder lives.

Server modules:

| File | Job |
|---|---|
| `index.js` | Boot order, the 30 s tick, wiring NOTIFY to SSE, the `[unhandled]` safety net |
| `config.js` | `.env` parsing, `badges.json` loading and validation, `computeGaps()` |
| `db.js` | Connection pool, the `LISTEN` session, every SQL string (`Q`), row-to-JSON mappers |
| `solana.js` | RPC client with a 10 s timeout, keypair files, send helper, `parseTransfer`, the `getTransaction` pacing slot, balances, the attack transaction builder |
| `ingest.js` | devnet subscription + safety poll (backfill and retries), insert into `payments` |
| `registry.js` | Solana Attestation Service: derive, issue, revoke, sync |
| `http.js` | Route table, validation, SSE hub, badge listener |

Runtime dependencies: `@solana/kit` 5.5.1, `@solana-program/token` 0.9.0, `@solana-program/system` 0.10.0, `sas-lib` 1.0.10, `pg` 8.23.1. No dotenv (`node --env-file-if-exists`), no nodemon, no express. `npm run server` has no watch mode.

## Data flow

```
 badge or `npm run pay`
        |  SPL transferChecked (HACK)
        v
 Solana devnet
        |  logsSubscribe(mentions: HACK mint), commitment confirmed      [live path]
        |  getSignaturesForAddress(mint) every 15 s + the retry set      [safety poll]
        v
 server/src/ingest.js
        |  getTransaction(jsonParsed), one paced slot per call (1.1 s apart, live first)
        |  parseTransfer() -> payer, payee, amount, slot, block time
        |  match against attack attempts (same victim, attacker, raw amount; idempotent per signature)
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
- Dedupe has three layers: an in-memory `seen` set, the `until` cursor of the safety poll (newest chain signature in the database), and `UNIQUE (signature, block_time)` with `ON CONFLICT DO NOTHING`.
- Failed signatures are kept. If the fetch returns nothing, the block time is missing, or the insert throws, the signature leaves `seen` and joins an in-memory `retry` set (`server/src/ingest.js:16`). Every safety poll handles the retry set first, then the new page of signatures. Before this, such a signature was lost as soon as a newer payment moved the `until` cursor past it. The retry set is not persisted and has no give-up rule.
- The safety poll runs after every (re)subscribe and every 15 s. It reads one page of at most 200 signatures and walks them oldest first. It does not run while a previous poll is still going or while the schema is missing.
- The attack match (`Q.matchAttack`, `server/src/db.js:41`) is idempotent per signature. If the payment insert fails after the match and the transfer is handled again, the attempt that already holds the signature is returned. A retry no longer marks a second pending attempt `signed`.
- The subscription reconnects with backoff (1 s doubling to 30 s). State goes `backfilling` -> `live` -> `reconnecting`.
- `ingest_lag_ms` (block time to insert) is recorded for live-path rows only. Rows that arrive through the safety poll have `null`.

### getTransaction pacing

Code: `server/src/solana.js:77-105`. Public devnet allows about 10 `getTransaction` calls per 10 s per IP, and a 429 counts against the limit. Un-paced retries kept the limit saturated: on 2026-10-03 a 429 storm on a busy mint starved the live path. Since then every `getTransaction` call, live or poll, takes a slot from one queue:

| Rule | Value |
|---|---|
| Gap between call starts | `TX_GAP_MS = 1100` (`server/src/solana.js:81`) |
| After a 429 | The next slot is pushed out to at least 5 s from now |
| Priority | The safety poll waits (checks every 250 ms) while any live signature is queued |
| Tries per signature | Up to 4, one slot each. After that the signature goes to the retry set |

Consequences, from the code: the live path handles at most one transaction per 1.1 s, so a burst of N payments shows the Nth about N x 1.1 s late. Catch-up after an outage is also about one transaction per 1.1 s. `TX_GAP_MS` is a constant, not an env variable.

Measured on live devnet USDC traffic through this pipeline (not with our own mint):

| Date | Pacing | Live ingest lag |
|---|---|---|
| 2026-10-02 | none (before the slot existed) | 0.8 to 1.6 s |
| 2026-10-03 | paced slot, safety poll running at the same time | p50 about 2.5 s, p95 about 2.7 s, zero 429s |

The PRD target is "about 3 seconds". The paced figures meet it with little margin. See [RUNBOOK.md](RUNBOOK.md#what-is-verified).

### Boot order (`server/src/index.js`)

1. Load `server/config/badges.json`.
2. Load or create the authority keypair. Log the public key only.
3. Derive the registry PDAs and check whether they exist (read-only, awaited).
4. Connect to Postgres (first check awaited, 3 s connect timeout). It does not block boot if it is down; a 5 s health check keeps trying. Whenever the schema appears: sync `badges.json` into the `badges` table, then sync attestations.
5. Start the tick and repeat it every 30 s: authority balance, find the mint (then start the ingest), sync attestations from chain, broadcast `status` if the gap list changed. The first tick is not awaited.
6. Start the `LISTEN` session.
7. Start HTTP (and the badge listener if `BADGE_LISTEN_HOST` is set).

Every RPC or database failure after step 2 becomes a gap in `/api/status`, never a crash. A stray promise rejection in a background loop is logged with the `[unhandled]` tag instead of ending the process (`server/src/index.js:14`).

RPC requests time out after 10 s (`server/src/solana.js:17-19`). `/api/status` answers while the first tick is still in flight. Step 3 is awaited, so with an RPC endpoint that accepts connections and never answers, the HTTP listener opens about 10 s after start. Measured with an unroutable `RPC_URL`: first `/api/status` answer after 10.4 s, `rpc_unreachable` in `gaps` one timeout later.

### Database connections (`server/src/db.js`)

| Connection | Settings | Behaviour |
|---|---|---|
| Pool (`initDb`, `server/src/db.js:166`) | 5 sessions, 3 s connect timeout, sessions kept open, `query_timeout` 5 s | A query that takes over 5 s fails as `db_error` (503) instead of holding its session forever. A health check every 5 s keeps `db.ok` and `db.schema` current and broadcasts `status` when they change |
| `LISTEN` session (`listen`, `server/src/db.js:199`) | Dedicated client, 3 s connect timeout, `query_timeout` 5 s, `SELECT 1` heartbeat every 10 s | On error, end or a timed-out heartbeat it reconnects after 2 s. After a reconnect the server broadcasts one `status` frame, because notifications sent while the session was down are gone |

A NOTIFY payload that does not parse as JSON, or a handler that throws, is logged as `[db] notify handler: …` and dropped.

## Database model

Schema: `db/schema.sql` (idempotent). Details and measured numbers: [TIGER-DATA.md](TIGER-DATA.md).

| Object | Kind | Notes |
|---|---|---|
| `payments` | hypertable, 1-hour chunks, columnstore | One row per transfer. Unique on `(signature, block_time)`. `payee_status` / `payee_name` are a snapshot at insert time. `source` is `chain` or `seed` |
| `attack_attempts` | hypertable, 1-day chunks, rowstore | One row per built attack. `outcome`: `pending`, `signed`, `rejected` |
| `badges` | table | Mirror of `server/config/badges.json` (the file is the source of truth; the table is replaced on every server start and `db:migrate`) |
| `attestations` | table | Mirror of the on-chain registry (the chain is the source of truth; reconciled every 30 s) |
| `payments_1m` | continuous aggregate, real-time | Per minute and `source`: count, verified count, volume |
| `payee_volume_1h` | continuous aggregate, real-time | Per hour, payee and `source`: count, volume. `/api/badges` reads only `source = 'chain'`, so seed rows stay out of "received" |
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
- The mirror write after an issue or revoke may fail without failing the request. It is logged (`[registry] mirror write failed, the 30 s sync reconciles`), and the HTTP response is built from what the chain call returned ([API.md](API.md#post-apiattestations)).

The PRD's signed-allowlist fallback was not implemented. SAS is the only path.

## Attack console

`buildTamperedTx` (`server/src/solana.js:149`) builds an ordinary SPL `transferChecked` from the victim badge's token account to the attacker's, for the real amount (default 500 HACK), and returns it unsigned. The page shows the claimed amount (default 5 HACK) next to the decoded real one.

- The server never holds a badge key and never submits the transaction. The victim is a no-op signer; the wire format carries 64 zero bytes where the signature goes.
- `txBytes` is the size of that wire transaction (signature slots + message). The page labels it "Transaction".
- The victim must be a pubkey from `badges.json`. The console cannot target other wallets.
- Delivery to the badge is a stub: the badge polls `GET /badge/pending` on the separate, unauthenticated badge listener. See [BADGE-GAPS.md](BADGE-GAPS.md).
- If the transfer ever lands on chain, the ingest matches it (victim, attacker, raw amount) and flips the attempt to `signed`. "Rejected" is marked by hand or by the badge.
- Expiry is 90 s everywhere: the `expired` outcome, the `expiresAt` field, how long `/badge/pending` serves an attempt, and the DEMO fixtures. It is an estimate of the blockhash lifetime, not read from the chain.

What the page does (`web/src/pages/Attack.jsx`):

| Case | Rendering |
|---|---|
| Actual equals claimed | `=` between the amounts, no red panel, "the same as the claim" |
| Actual higher, ratio rounds to more than 1 | `≠`, red double-bordered panel, "N× the claim" |
| Actual lower, or higher by a ratio that rounds to 1 | `≠`, red panel, "X HACK less / more than the claim" |
| POST returned `warnings` | Shown as amber notes under the attempt that was just built |
| Attempt `pending` or `expired` | "Mark rejected" button, in the panel and in the Attempts table |
| A selected victim leaves `badges.json` | The selection falls back to the default badge on the next render |
| `/api/status` not loaded yet | The Delivery card shows a skeleton |

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

### Client error codes

`http()` (`web/src/data.jsx:19-28`) turns every failed request into an `Error` with a `code`:

| Response | `code` |
|---|---|
| `fetch` itself fails (nothing listening, network down) | `offline` |
| No JSON body, and status is 2xx, 404 or 502 (the Vite proxy answers 502 `text/plain` when nothing is on 8787; a static host with no `/api` answers 2xx or 404) | `offline` |
| No JSON body, any other status | `bad_response`, message "The server answered HTTP N without a JSON error body. Check the server log." |
| JSON body with `error` | the server's code (`db_error`, `chain_error`, …), with `status` and `gap` |

### Stream recovery

The LIVE stream recovers by itself after a server restart or a dropped connection. The pieces:

1. The Vite proxy destroys the browser's SSE response when the upstream connection closes (`web/vite.config.js:17`). Without this the proxy left the response open and the browser never noticed the server had died.
2. The status poll runs every 5 s and is the backend heartbeat (`web/src/data.jsx:109-113`). While it fails, no `EventSource` is open, so there is no reconnect noise. When it succeeds again, every mounted resource is refetched (`web/src/data.jsx:116-119`) and a new `EventSource` opens.
3. An outage shorter than the poll can still close the stream for good: the browser's automatic retry gets a 502 from the proxy, and an `EventSource` that gets a non-200 answer does not retry again. The client detects the closed state and creates a new `EventSource` after 3 s (`web/src/data.jsx:128`). When that one opens, it refetches everything (`web/src/data.jsx:129`).
4. On the server side, a reconnect of the `LISTEN` session sends a `status` frame, which makes clients refetch everything.

Verified 2026-10-03 in headless Chrome against the real server: the pill goes `OFFLINE` while the API is down, and a new block arrives about 9 s after the API restarts. Checked again with `curl`: a proxied stream ends the moment the server is killed, and the proxy answers 502 `text/plain` while it is down.

## Gap system

A "gap" is one known reason the panel is degraded. The server computes the list (`computeGaps()`, `server/src/config.js:67`) and returns it in `/api/status.gaps`. Each gap has a `code`, a `severity`, a `title`, a one-sentence `fix` naming the exact file, variable or command, the `pages` it affects, and optionally a `badgeGap` id that ties it to a `BADGE-GAP(<id>)` marker in the code. The full table is in [API.md](API.md#gaps).

Degraded states in LIVE (`web/src/App.jsx:36`, `web/src/App.jsx:54-69`):

| State | Trigger | What renders |
|---|---|---|
| `CONNECTING` | The first `/api/status` has not answered yet | Grey pill. Pages show skeletons |
| `OFFLINE` | `/api/status` failed with code `offline` (see the table above) | Red pill. Red "Backend offline" banner under the top bar with a Retry button. Pages keep their last data; a page with no data shows a "Backend offline" placard |
| `ERROR` | `/api/status` failed with any other code: a non-JSON error status (`bad_response`), or a JSON error such as 403 `forbidden_origin` | Red pill. Red "Backend error" banner with the error message and a Retry button |
| Blocker gap that affects all four pages (`db_offline`, `db_schema_missing`, `rpc_unreachable`) | In `/api/status.gaps` | A `GapCard` under the top bar on every page. Hidden while the status request itself is failing |
| Page-level gap | In `/api/status.gaps` for that page | The page renders the `GapCard` itself via `useGaps(page)`. Blocker = panel with the fix, warn = strip, info = muted line. A `BADGE-GAP(<id>)` chip appears when the gap is waiting on badge hardware |
| One resource fails, status is fine | For example `/api/payments` returns `db_error` | That card shows "Feed unavailable", "Badges unavailable", "Attestations unavailable", "Attempts unavailable" or "Stats unavailable" with the server's message. Stale data stays on screen if there is any |
| Loading | No data and no error yet | Skeletons sized like the real content |
| No rows | Empty list | An `Empty` placard with a hint |

Other pill values come from `ingest.state`: `LIVE`, `BACKFILLING`, `RECONNECTING`, and `IDLE` for anything else (no mint).

Form errors on the Registry page: a validation error shows under its field. If the field is not on screen (the public key field only exists when "Other public key…" is selected), or the error is not a field error, it shows in a red note above the attestations table. The submit button is disabled while the badge list loads.

## Security

- The API binds `127.0.0.1`. A non-loopback `HOST` logs a warning at start.
- `Host` must be `localhost` or `127.0.0.1` (DNS-rebinding guard).
- POST requires `Content-Type: application/json` and, when an `Origin` header is present, one listed in `WEB_ORIGINS`. No CORS headers are ever sent, so a cross-site page cannot get a preflight through.
- Bodies are capped at 16 KB.
- The authority keypair (`server/.keys/authority.json`, mode 0600 in a 0700 directory, gitignored) signs registry transactions and pays fees. It never leaves the laptop, is never logged and never appears in a response. Only its public key is exposed. If the file is corrupt, the error names the path and does not quote the file's content (`server/src/solana.js:42`).
- Error responses carry a code and a message, never a stack trace.
- The badge listener is a separate server on a separate port, off unless `BADGE_LISTEN_HOST` is set. It serves `/badge/pending` and `/badge/outcome` only; none of the admin API is reachable from the hotspot. It has no authentication (see [Known limits](#known-limits)).
- The PRD also asks for a read-only feed published on a .tech domain. That is not built. The whole panel is localhost-only.

## Design system: "Terminal Ledger"

Pure black or pure white, one signal green, two more status colours. Built to be read from about 2 metres. All tokens are at the top of `web/src/styles.css` (lines 5-42).

| | |
|---|---|
| Display | Barlow Condensed 200 / 800 (headings, uppercase) |
| Body | Barlow 300 / 600 |
| Data | JetBrains Mono 400 / 800 (numbers, hashes, slots) |
| Surfaces, dark | `--bg`, `--bg-1`, `--bg-2`, `--block`, `--block-sel` are all `#000` (`web/src/styles.css:29`, `:32`) |
| Surfaces, light | The same five tokens are all `#fff` (`web/src/styles.css:37`, `:40`) |
| Ink and lines, dark | `--ink #fff`, `--ink-2 #a3a3a3`, `--ink-3 #6b6b6b`, `--line #333` |
| Ink and lines, light | `--ink #000`, `--ink-2 #525252`, `--ink-3 #8a8a8a`, `--line #d4d4d4` |
| Status colours | green `--ok #17b978` = verified / live; amber `--warn #e8a33d` = unverified / pending; red `--bad #e5484d` = revoked / tampered / blocker. Same hues in both themes. `--accent` is the same green |
| Status text | `--ok-text`, `--warn-text`, `--bad-text`: the same hues, darkened in the light theme (`#0b7a4e`, `#8a5a00`, `#c22d33`) so small text keeps 4.5:1 |
| Themes | `html[data-theme="dark"]` and `html[data-theme="light"]` |
| Motion | One page-load stagger (`rise`), one block entrance (`blockIn`, `pop`, `ring`, `linkIn`), the NEXT slot's `scan` and `pulse`, the skeleton `shimmer`. All disabled under `prefers-reduced-motion` |

There is no background grid; `body` is a flat `var(--bg)`. Because every surface in a theme is the same colour, nothing is separated by shade:

- Cards, tables, fields and blocks are separated by 1 px borders (`--line`). Inputs and selects use the stronger `--ink-3` border (`web/src/pages/admin.css:9`).
- Ledger rows: hover changes the row's bottom border to `--ink-2`; the selected row gets a green bottom border and a 3 px green bar on its first cell (`web/src/styles.css:155-157`).
- Chain blocks: hover changes the border and lifts the block 2 px. The selected block has a green border and a 2 px green ring and lifts 4 px (`web/src/styles.css:252`). The other blocks dim to 70% only when the selected block is in the rail (`web/src/styles.css:253`, `web/src/pages/Feed.jsx:59`); selecting an older row from the ledger does not dim the rail.
- Keyboard focus is a 2 px `--ok-text` outline. The rail's own focus ring is drawn on the chain card, because the card clips the rail. A block focused by keyboard is scrolled clear of the sticky NEXT slot (`web/src/styles.css:211-214`, `web/src/pages/Feed.jsx:63`).
- Long numbers size themselves by container query instead of clipping: stat tiles (`web/src/styles.css:122-123`), badge balances (`web/src/pages/admin.css:16-17`), the two amounts on the Attack page (`web/src/pages/admin.css:30`, `:33`).

What the CSS still contains that is not flat black or white, stated so nobody is surprised:

| Where | What |
|---|---|
| Notes, gap cards, the offline banner, the NEXT slot, the red "transaction moves" panel | A 7% to 12% tint of the status colour mixed into the surface (`color-mix`) |
| Top bar | 86% opaque with a 10 px backdrop blur (`web/src/styles.css:84`) |
| Skeleton placeholders | A moving `linear-gradient` shimmer (`web/src/styles.css:200`) |
| Selected chain block | A soft green drop shadow from the `--glow` token (`web/src/styles.css:31`, `:39`, `:252`) |
| New block entrance | The `ring` keyframe fades a 30 px green shadow (`web/src/styles.css:318`) |

Fonts load from Google Fonts with system fallbacks, so the panel still works offline with fallback type. Pages add no colours; `web/src/pages/admin.css` only uses the tokens.

The chain strip (Feed): newest block at the left next to a `NEXT` placeholder, at most 30 blocks in the rail, older rows in the ledger table below. Block states: verified (green, check), unverified (amber, `?`), revoked (red, `⊘`), tampered (red double border, `!`, "TAMPERED — signed"), and off-chain (`seed` or `demo`: dashed border, muted, `~seed` / `~demo` tag). If the reader has scrolled right, new blocks do not yank the rail; a "N new" chip appears instead.

## Decisions

| Decision | Reason |
|---|---|
| Database NOTIFY drives the browser | One source of truth. Whatever inserts a row (ingest, a script, psql) updates every open panel. No polling loop in the server |
| NOTIFY payload is `{type, id}` only | The server re-reads the row with the REST query, so SSE and REST shapes cannot drift |
| Verification status is snapshotted per payment | A revoke must not rewrite what the feed showed at the time |
| `badges.json` is the source of truth for badges | Edit one file, restart the server, and the `badges` table is resynced at boot |
| No watch mode on the server | With `--env-file-if-exists`, `node --watch` on Node 26.10 watched the whole working directory, so Vite cache writes kept restarting the API (found during the build; not re-tested). Restart by hand after editing `.env`, `server/config/badges.json` or server code |
| One paced slot for `getTransaction` | Public devnet rate limits per IP and counts 429s. Pacing costs about a second of lag and removes the 429 storms |
| Responses to issue/revoke come from the chain call | The transaction has already landed; a database problem must not turn it into an error |
| `sas-lib` 1.0.10 on `@solana/kit` 5.5.1 | The only published, non-beta SAS client at build time; it pins the kit major |
| Stand-in badges | Software keypairs fill empty badge slots so LIVE works before real hardware exists |
| Seed rows are marked and muted | Faucet insurance, but never passed off as live data |
| `signed` only from chain evidence | A client can only mark an attack `rejected`; `signed` requires the transfer to appear on devnet |

## Known shortcuts

Deliberate ceilings, each marked with a `ponytail:` comment in the code:

| Where | Ceiling |
|---|---|
| `server/src/ingest.js:12` | The `seen` signature set is never pruned (about 100 B per transaction; fine for a 24 h event) |
| `server/src/ingest.js:14` | The retry set is in memory only (a restart before the retry drops it) and never gives up on a signature that can never be fetched |
| `server/src/ingest.js:47` | The safety poll reads one page of 200 signatures. A longer outage loses the older ones |
| `server/src/solana.js:59` | Only the first top-level `transferChecked` for our mint is parsed. Inner (CPI) transfers and multi-transfer transactions are ignored |
| `server/src/solana.js:80` | `TX_GAP_MS` is fixed for public devnet. It would need to become an env variable for a paid RPC endpoint |
| `server/src/registry.js:25` | An attestation past its expiry still counts as verified (issue sets 30 days; the event lasts 24 h) |
| `server/src/http.js:124` | `before` is a strict timestamp cursor. Rows sharing the page's last block time are skipped when paging |
| `web/src/pages/Feed.jsx:181` | The client's workaround for that (ask from 1 ms later, de-duplicate) cannot page past 40 rows in one millisecond |
| `server/src/http.js:235` | The claimed merchant name sent to the badge is fixed to "MHacks Merch"; only the amount lies |
| `server/src/http.js:259` | With `BADGE_LISTEN_HOST=0.0.0.0` the displayed URL uses the first non-internal IPv4. With several interfaces up it can be the wrong one |
| `web/src/data.jsx:47` | The set of mounted resources is never pruned |
| `web/src/data.jsx:48` | A refetch during an in-flight GET reuses that GET |
| `web/src/data.jsx:80` | Only the default `payments` list gets the live prepend; other payment queries refetch |
| `web/src/demo.js:93` | A live payment for an older minute is counted in the newest bar until the next `/api/stats` refetch |
| `web/src/demo.js:61` | DEMO keeps at most 500 blocks in memory |
| `web/src/pages/Feed.jsx:173` | If the base list is refetched after new blocks arrived, rows between it and the "load older" pages can go missing until reload |
| `web/src/pages/Feed.jsx:42` | The map of how each block entered the rail only grows (fine for one session) |
| `web/src/pages/Registry.jsx:8` | The client-side pubkey check is base58 shape only; the server does the real 32-byte decode |
| `web/src/pages/Attack.jsx:88` | The default victim is the first badge whose label contains "Judge", else the first badge |
| `web/src/pages/Attack.jsx:114` | Amount fields use native number validation; the server re-checks |
| `web/src/styles.css:121` | Stat tile sizing assumes the mono font's glyph width |
| `web/src/pages/admin.css:15` | Badge balance and Attack amount sizing assume the mono glyph width and a 4-letter symbol |
| `web/src/ui.jsx:4` | Explorer links are hardcoded to devnet |
| `web/src/ui.jsx:175` | Amounts use `Number` (exact to 2^53 raw units) |

Find them all (24 lines on 2026-10-03):

```bash
grep -rn "ponytail:" server/src scripts web/src db
```

## Known limits

Known and deliberately not fixed. None of these has a `ponytail:` marker of its own unless listed above.

| Limit | Detail |
|---|---|
| `?before=` paging skips rows at a page seam | Rows that share the block time of a page's last row. The web client works around it; a hand-written client has to do the same ([API.md](API.md#paging-caveat)) |
| The badge listener is unauthenticated | `BADGE-GAP(attack-delivery)` stub. Anyone on the hotspot who knows a badge's public key can read its pending attempt; anyone with an attempt id can mark it rejected |
| `payments_1m` can show `lastStatus: "Failed"` | For a few minutes right after `npm run db:seed`, until the next scheduled refresh succeeds. Reported by the debugging pass on 2026-10-03; not reproduced for this document |
| Bare 400 / 431 from Node | A request without `Host` (HTTP/1.1) or with more than 16 KB of headers is answered by Node itself, with no JSON body |
| Attack expiry is 90 s everywhere | An estimate, not the real blockhash lifetime |
| Live ingest rate | One `getTransaction` per 1.1 s: bursts queue up, and the lag tile grows with them |

## Tests

`npm run check` runs `node --test` over `server/src/check.test.js` (transfer parsing, validators, SQL splitter: 5 tests) and `web/src/demo.test.js` (demo fixtures match the API shapes, demo POST flows: 6 tests), then builds the web app. 11 tests pass as of 2026-10-03.

No UI tests ship in the repo. The UI was verified with headless Chrome scripts (Playwright MCP was not available). The on-chain flows were exercised on 2026-10-03 against an in-memory stand-in chain that is also not in the repo; see [RUNBOOK.md](RUNBOOK.md#what-is-verified).
