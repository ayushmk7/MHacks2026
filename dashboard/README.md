# BadgePay dev panel

The laptop dashboard for "Verified Hardware Payments on the Solana Badge" (MHacks 2026). It does the three jobs the PRD gives the dashboard: it shows every HACK payment on Solana devnet as a new block in a live chain, it lets the registry admin issue and revoke badge verifications through the Solana Attestation Service, and it drives the "compromised app" attack demo by building a transfer whose real amount differs from the amount the checkout claims. Payments are stored in TimescaleDB (Tiger Data), and the database itself pushes each new row to the browser.

Product spec: [Verified Payment Key PRD](../docs/specs/Prd-verified-payment-key.md). Repository overview: [`../README.md`](../README.md).

This folder is `dashboard/` in the repository. Every command below runs from inside it, and every path below is relative to it.

## Status (2026-10-03)

| Area | State |
|---|---|
| Panel, API, database, scripts | Built. `npm run check` passes (11 tests + web build) |
| Read path on real devnet (ingest, database, SSE, browser) | Verified with live devnet USDC traffic. Live lag p50 about 2.5 s, p95 about 2.7 s |
| Write paths with our own mint (mint creation, badge funding, issue, revoke, `npm run pay`, attack `signed` detection) | **Nothing has been sent on real devnet.** The faucet is rate-limiting this laptop, so the authority has 0 SOL. These flows were exercised against an in-memory stand-in chain (51 scripted checks pass; it does not verify signatures or run program logic), and the SAS instruction sequences pass `simulateTransaction` on real devnet |
| Real badges | None connected. The four badge slots hold stand-in software keypairs. Everything waiting on hardware is in [BADGE-GAPS.md](../docs/dashboard/BADGE-GAPS.md) |
| Tiger Cloud | Not tested. Local Docker only |
| UI tests | None ship in the repo. The UI was verified with headless Chrome scripts |

To unblock the write paths, see [First real devnet run](#first-real-devnet-run). The full verified / not verified tables are in [RUNBOOK.md](../docs/dashboard/RUNBOOK.md#what-is-verified).

## Quickstart

Needs Node 22.12 or newer and Docker.

```bash
cd dashboard
npm run setup         # npm install for the server and for web/
cp .env.example .env  # optional: the defaults in code are the same values
npm run db:up         # TimescaleDB in Docker on 127.0.0.1:5433
npm run db:migrate    # create the schema (safe to re-run)
npm run db:seed       # optional: 51,840 fake historical payments
npm run dev           # API on 127.0.0.1:8787 + web on 127.0.0.1:5173
```

Open <http://127.0.0.1:5173>.

The panel opens in **DEMO** mode: fixture data and a ticker that adds a fake block about every 4 seconds. DEMO needs no database, no server and no network; `npm run setup && npm run web` is enough. Flip the switch to **LIVE** to read the real API.

In LIVE, before the devnet setup has run, the Feed shows a "HACK mint not configured" card instead of the chain. That is expected. The ledger table below it still lists seeded rows if you ran `db:seed`.

The server has no watch mode. Restart it by hand after editing `.env`, `server/config/badges.json` or server code. The web app hot-reloads.

## The two switches

Both are in the top bar and both persist in `localStorage`.

| Switch | Values | Key | Default |
|---|---|---|---|
| Data mode | `DEMO` / `LIVE` | `badgepay.mode` | `DEMO` |
| Theme | `LIGHT` / `DARK` | `badgepay.theme` | OS preference |

The pill next to them shows `DEMO`, or in LIVE one of:

| Pill | Meaning |
|---|---|
| `CONNECTING` | Waiting for the first `/api/status` answer |
| `LIVE` | Subscribed to devnet |
| `BACKFILLING` | The first devnet subscription is being opened |
| `RECONNECTING` | The devnet subscription dropped and is being retried |
| `IDLE` | No HACK mint configured or found |
| `OFFLINE` | Nothing answers on 8787 (network failure, or the proxy's 502). The panel reconnects by itself when the server is back |
| `ERROR` | Something answers, but with an error instead of the status. A red "Backend error" banner shows the message |

Pages are hash routes: `#/feed`, `#/badges`, `#/registry`, `#/attack`.

## First real devnet run

Do this once, when the authority has SOL.

1. Print the authority address (it is also in `/api/status` and on the Registry page):

   ```bash
   npm run devnet:setup
   ```

   The first line is `authority  <address>`. On this laptop it is `FZEAS6Nayu6nMX1RVmoNwoPA4tu1KNM5pvfoXsQzei3K`. If the faucet refuses, the script exits with `Faucet rate-limited. Fund <address> at https://faucet.solana.com then re-run.`
2. Fund that address with at least 0.5 devnet SOL at <https://faucet.solana.com> (a full first run spends about 0.25).
3. Run `npm run devnet:setup` again. It is idempotent; re-run until it finishes. It creates the HACK mint and writes `HACK_MINT` into `.env`, gives every configured badge 0.05 SOL, a token account and 1000 HACK, creates the attacker token account, creates the registry credential and schema, and issues "MHacks Merch" to badge 1. If `HACK_MINT` already points at a mint the authority does not control, it skips the minting, prints a warning, and still sends SOL and creates token accounts.
4. Restart the server so it reads the new `HACK_MINT`.
5. Send a test payment and watch it arrive in LIVE:

   ```bash
   npm run pay -- --from 3 --to 1 --amount 10
   ```

## Folder structure

```
dashboard/
├─ package.json              server deps and every npm script
├─ .env.example              every variable, with BADGE-GAP markers
├─ docker-compose.yml        TimescaleDB 2.30.2-pg17 on 127.0.0.1:5433, project name "badgepay"
├─ db/
│  ├─ schema.sql             tables, hypertables, continuous aggregates, policies, NOTIFY triggers (idempotent)
│  └─ seed.sql               51,840 fake payments (source='seed'), aggregate refresh, columnstore conversion
├─ server/
│  ├─ config/badges.json     the four badge slots (source of truth for badges)
│  ├─ .keys/                 generated: authority.json, standin-N.json (0600, gitignored)
│  └─ src/
│     ├─ index.js            boot order, 30 s tick, NOTIFY -> SSE wiring
│     ├─ config.js           env parsing, badges.json loading, gap computation
│     ├─ db.js               pg pool, LISTEN session, every SQL string, row -> JSON mappers
│     ├─ solana.js           RPC client (10 s timeout), keypair files, transfer parser, getTransaction pacing, balances, attack transaction builder
│     ├─ ingest.js           devnet subscription + safety poll (backfill, retries) -> INSERT
│     ├─ registry.js         Solana Attestation Service: issue, revoke, sync
│     ├─ http.js             routes, validation, SSE hub, badge listener
│     └─ check.test.js       node --test: parser, validators, SQL splitter
├─ scripts/
│  ├─ db.mjs                 migrate | seed | reset
│  ├─ devnet-setup.mjs       authority, stand-in badges, mint, funding, first attestation
│  └─ simulate-payment.mjs   a real devnet transfer between stand-in badges
├─ web/
│  ├─ vite.config.js         127.0.0.1:5173, proxies /api to 127.0.0.1:8787
│  ├─ index.html             fonts, no-flash theme script
│  └─ src/
│     ├─ main.jsx            mounts the app
│     ├─ App.jsx             shell: top bar, hash routing, switches, offline / error banner, blocker cards
│     ├─ data.jsx            data layer: DEMO and LIVE transports behind the same hooks, stream recovery
│     ├─ demo.js             DEMO fixtures, in-memory fake backend, block ticker
│     ├─ demo.test.js        node --test: fixtures match the API shapes
│     ├─ ui.jsx              shared components
│     ├─ styles.css          design tokens (both themes, at the top) and shared classes
│     └─ pages/
│        ├─ Feed.jsx         stat tiles, block chain, block detail, minute chart, Tiger Data card, ledger
│        ├─ Badges.jsx       the four badges: key location, balances, attestation
│        ├─ Registry.jsx     issue and revoke attestations
│        ├─ Attack.jsx       fake checkout: claimed amount vs real amount
│        └─ admin.css        styles for the three admin pages
└─ README.md                 this file
```

The docs live outside this folder, in [`../docs/dashboard/`](../docs/dashboard/).

## npm scripts

Run from `dashboard/`.

| Script | What it does |
|---|---|
| `npm run setup` | `npm install` here and in `web/` |
| `npm run db:up` | Start the database container (`badgepay-db-1`) and wait until healthy |
| `npm run db:down` | Stop and remove the container (the data volume `badgepay_badgepay-db` is kept) |
| `npm run db:psql` | `psql` inside the container. Pass a query with `npm run db:psql -- -c "SELECT 1"` |
| `npm run db:migrate` | Apply `db/schema.sql` and sync `badges.json` into the `badges` table. Idempotent |
| `npm run db:seed` | Replace the seed rows with 51,840 fresh ones, refresh the aggregates, convert old chunks to columnstore, print the measured compression |
| `npm run db:reset` | **Drops every BadgePay table**, then migrates |
| `npm run devnet:setup` | One-time devnet setup (see above). Idempotent |
| `npm run pay` | Send a real HACK transfer between stand-in badges: `npm run pay -- --from 3 --to 1 --amount 10 [--every 5]`. Defaults: from 3, to 1, amount 10 |
| `npm run server` | API server. No watch mode: restart it after editing `.env`, `server/config/badges.json` or server code |
| `npm run web` | Vite dev server (port 5173 is fixed; it fails if the port is taken) |
| `npm run dev` | Server and web together |
| `npm run check` | `node --test` (server + demo fixtures), then a production build of `web/` |

## Configuration

Everything is in `.env` (see `.env.example`). The ones you are likely to touch:

| Variable | Default | Meaning |
|---|---|---|
| `DATABASE_URL` | local Docker | Postgres connection. For Tiger Cloud use the direct connection, not the pooler |
| `RPC_URL`, `WS_URL` | public devnet | Solana RPC and websocket |
| `HACK_MINT` | empty | HACK mint address. Written by `devnet:setup`. Empty = the feed is idle |
| `HOST`, `PORT` | `127.0.0.1`, `8787` | Where the API listens |
| `WEB_ORIGINS` | the two Vite origins | Comma-separated `Origin` allowlist for POSTs. A trailing slash on an entry is ignored |
| `ATTACKER_PUBKEY` | empty | Where the attack transfer would send funds. Empty = the authority address |
| `ATTACK_TX_VERSION` | `legacy` | `legacy` or `0`: message version of the attack transaction |
| `BADGE_LISTEN_HOST` | empty | Set to the laptop's hotspot IP (or `0.0.0.0`) to open the badge listener. Empty = closed |
| `BADGE_LISTEN_PORT` | `8788` | Port of the badge listener |

Badges are configured in `server/config/badges.json`, not in `.env`.

One variable is read by Vite, not by the server, and is not in `.env`: `API_URL=http://host:port npm run web` points the panel's `/api` proxy at another backend.

## Security

- The API and the web server bind `127.0.0.1` only. The admin pages (Registry, Attack) are not reachable from the network.
- The API rejects requests whose `Host` is not `localhost` or `127.0.0.1`, and POSTs without `Content-Type: application/json` or with an `Origin` outside `WEB_ORIGINS`. It sends no CORS headers.
- The registry authority keypair lives in `server/.keys/authority.json` (mode 0600, gitignored). It never leaves the laptop, is never logged and never appears in an API response. Whoever holds this file can issue and revoke verifications, so do not copy it around.
- `server/.keys/standin-N.json` are throwaway devnet keys for the stand-in badges.
- The only thing that can listen on the hotspot is the badge listener (`BADGE_LISTEN_HOST`), which serves two routes for the attack demo and is off by default. It has no authentication; leave it off when no badge needs it.
- Devnet and a demo token only. No real funds.

## Docs

| Doc | Contents |
|---|---|
| [ARCHITECTURE.md](../docs/dashboard/ARCHITECTURE.md) | Components, data flow, ingest pacing, database model, mode switch, stream recovery, gap system, design system, decisions, shortcuts and limits |
| [API.md](../docs/dashboard/API.md) | Every route with real examples, errors, SSE events, paging caveat, badge listener |
| [TIGER-DATA.md](../docs/dashboard/TIGER-DATA.md) | Which Tiger Data / TimescaleDB features are used, where, and the measured numbers |
| [BADGE-GAPS.md](../docs/dashboard/BADGE-GAPS.md) | Everything left open for the badge hardware, and what to fill in when a badge arrives |
| [RUNBOOK.md](../docs/dashboard/RUNBOOK.md) | Demo-day sequence, pre-flight checklist, what has and has not been verified, recovery moves |

The badge firmware is documented separately in [`../docs/os/README.md`](../docs/os/README.md).

## Known limits

- On-chain write paths with our own mint are unverified on real devnet until the first real devnet run (above).
- Measured columnstore compression is 66.8% on the seed data, not 90%: transaction signatures do not compress. Details in [TIGER-DATA.md](../docs/dashboard/TIGER-DATA.md).
- Tiger Cloud was not tested (only local Docker).
- The public read-only feed on a .tech domain from the PRD is not built. The whole panel is localhost-only.
- The PRD's signed-allowlist fallback for the registry is not built. SAS is the only registry path.
- No UI tests ship in the repo. The UI was verified with headless Chrome scripts.
- Public devnet RPC is rate-limited (about 10 `getTransaction` calls per 10 s per IP). The server makes one `getTransaction` call per 1.1 s, live and catch-up together, so a burst of payments queues up and catch-up after an outage runs at the same pace.
- The badge listener is unauthenticated (the `BADGE-GAP(attack-delivery)` stub).
- `GET /api/payments?before=` skips rows that share the block time of a page's last row. The web client works around it; see [API.md](../docs/dashboard/API.md#paging-caveat).
- `/api/stats/db` can report `lastStatus: "Failed"` for `payments_1m` for a few minutes right after `npm run db:seed`.
- A request without a `Host` header, or with more than 16 KB of headers, gets Node's bare 400 / 431 with no JSON body.
- Attack attempts expire after a fixed 90 s, which is an estimate of the blockhash lifetime.
- Code-level shortcuts are listed in [ARCHITECTURE.md](../docs/dashboard/ARCHITECTURE.md#known-shortcuts).
