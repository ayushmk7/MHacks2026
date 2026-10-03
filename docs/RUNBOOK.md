# Runbook

Demo-day sequence for the laptop panel, mapped to the PRD demo plan. Read "What is verified" first: as of 2026-10-03 the on-chain steps have not run for real.

## What is verified

"Run" means the command or route was executed on this laptop and behaved as documented.

| Area | State | How |
|---|---|---|
| `npm run check` (11 tests + web build) | Run, passes | 2026-10-03 |
| `db:up`, `db:migrate` (re-run on an existing schema), `db:psql` | Run | 2026-10-03 |
| `db:seed` after `db:reset`: 51,840 rows, 70 of 73 chunks in columnstore, ratio 0.668 | Run by the implementers; numbers re-read from the database | 2026-10-03 |
| Every GET route, all validation errors, Host / Origin / Content-Type / size checks | Run with `curl` | See [API.md](API.md) |
| Postgres NOTIFY -> server -> SSE | Run (a manual `pg_notify` arrived on `/api/events`) | |
| Badge listener routes (204, 400, 404 cases) | Run on a second instance | |
| Vite proxy for `/api` and SSE | Run | |
| Ingest pipeline (subscribe, parse, insert, notify) | Run by the implementers against live devnet USDC traffic, lag 1.0 to 1.6 s | Not with our own mint |
| SAS transactions (create credential, schema, attestation; close attestation) | Simulated only (`simulateTransaction` returned no error) | Never sent |
| UI in DEMO and LIVE | Checked by hand with headless Chrome screenshots | No automated UI tests |
| HACK mint creation, badge funding (`devnet:setup` past the airdrop) | **Not run** | Faucet returned 429 |
| `POST /api/attestations` and `/revoke` success paths | **Not run** | Authority has 0 SOL |
| `npm run pay` | **Not run** | No mint, no funded stand-ins |
| `POST /api/attacks` success path, `/badge/pending` returning an attempt | **Not run with our own mint** | Needs the mint |
| Attack "signed" auto-detection | **Not run** | Needs a signed transfer on chain |
| Real badges (any gap in [BADGE-GAPS.md](BADGE-GAPS.md)) | **Not run** | No hardware yet |
| Tiger Cloud (`DATABASE_URL` swap, TLS) | **Not run** | Local Docker only |

## First real devnet run

Blocked until the registry authority has devnet SOL. The faucet rate-limited this laptop's IP (HTTP 429, retry after about 24 h).

1. Get the authority address. It is the first line of `npm run devnet:setup`, the `authority.pubkey` field of `GET /api/status`, and the Authority field on the Registry page. On this laptop: `FZEAS6Nayu6nMX1RVmoNwoPA4tu1KNM5pvfoXsQzei3K`.
2. Fund it with at least 0.5 SOL at <https://faucet.solana.com> (devnet). From another network or a phone if this IP is still limited.
3. `npm run devnet:setup`. Re-run until it prints `Done.` and the summary table. Expect: mint created and written to `.env`, four badges funded (0.05 SOL, 1000 HACK each), credential and schema created, "MHacks Merch" issued to badge 1.
4. Restart the server.
5. Confirm:

   ```bash
   curl -s http://127.0.0.1:8787/api/status
   ```

   `ingest.state` is `live`, `token.mint` is set, `registry.ready` is `true`, and `gaps` has no `blocker`.
6. `npm run pay -- --from 3 --to 1 --amount 10`. The script prints an explorer link. A green block should appear on the Feed within about 3 seconds.
7. On the Registry page, issue a name to badge 2, then revoke it. Each should show a "View transaction" link that opens on the devnet explorer.
8. On the Attack page, charge Judge A. The claimed-vs-actual panel should appear with a byte size.
9. Update the table above and the Status section of the README.

If any of 3 to 8 fails, the error text is in the terminal running the server or in the red note on the page. Report it; these paths have never run.

## Pre-flight checklist (before judges arrive)

- [ ] Laptop and badges on the same phone hotspot.
- [ ] `npm run db:up` (container healthy).
- [ ] `npm run db:migrate`.
- [ ] `npm run db:seed` as faucet insurance. It only replaces seed rows; chain rows are kept.
- [ ] `npm run devnet:setup` finishes with `Done.` and every badge shows SOL and HACK in its table.
- [ ] Start the server and the web app in two terminals (or both with `npm run dev`):

  ```bash
  npm run server
  npm run web
  ```

  (`npm run dev` works too, but its server restarts on any file change in the project, which drops the live stream for a moment.) Open <http://127.0.0.1:5173>.
- [ ] Switch to LIVE. The pill reads `LIVE`. No banner under the top bar.
- [ ] Badges page: four cards, each with a HACK balance and SOL for fees. Badge 1 shows `verified` and "MHacks Merch".
- [ ] Registry page: Authority shows more than 0.02 SOL; the chip reads `on chain`.
- [ ] `npm run pay -- --from 3 --to 1 --amount 10`: a block appears within about 3 seconds. Click it; the Explorer link opens.
- [ ] Feed "Ingest lag · p95" tile is under 3 s (green).
- [ ] Attack page: if a real badge will receive the charge, `BADGE_LISTEN_HOST` is set and the Delivery card reads "listener open". Otherwise expect the "Badge delivery listener is closed" card.
- [ ] Pick the theme that reads best on the projector (top-right switch).
- [ ] Leave a terminal open in the project root for `npm run pay`.

With stand-in badges only, steps 1, 2 and 4 below can be rehearsed with `npm run pay`. With real badges, `npm run pay` refuses to sign for them ("its key never leaves the badge"); the badge sends the payment.

## Demo sequence

PRD demo plan, about three minutes. The panel is on the screen behind the judges, in LIVE, on the Feed.

### 1. Pay a verified merchant

Panel: **FEED** (`#/feed`).

1. The judge pays "MHacks Merch" from their badge. Rehearsal: `npm run pay -- --from 3 --to 1 --amount 10`.
2. A block slides in at the left of the chain: green, check mark, the amount, "MHacks Merch".
3. Click the block. The detail panel shows payer, payee, amount, slot, ingest lag and an **Explorer** link to the transaction on devnet.
4. Point at the "Payments · 24h" tile and the "Powered by Tiger Data" card if the Tiger Data judges are present.

### 2. Impostor

Panel: stay on **FEED**; optionally show **BADGES** (`#/badges`).

1. The impostor badge requests payment while claiming "MHacks Merch". The judge's badge shows red and the judge rejects. Nothing reaches the chain, so nothing appears on the Feed. That is the correct result.
2. To show why: Badges page, badge 2 "Impostor (team)" reads `unverified` and "no verified name", while badge 1 reads `verified` "MHacks Merch".
3. If someone pays the impostor anyway (rehearsal: `npm run pay -- --from 3 --to 2 --amount 10`), the block is amber with a `?`.

### 3. Compromised app

Panel: **ATTACK** (`#/attack`).

1. In "Fake checkout": Victim badge = the judge's badge (defaults to the first badge whose label contains "Judge"). "Checkout shows" = 5. "Transaction really moves" = 500.
2. Click **Charge 5 HACK**.
3. The right panel shows `5 HACK ≠ 500 HACK`, "100× the claim", the destination, the byte size and the message version.
4. Delivery to the badge (see `attack-delivery` in [BADGE-GAPS.md](BADGE-GAPS.md)): with the listener open, the Delivery card shows the URL the badge polls. Without badge firmware for this, narrate from the panel.
5. The badge shows 500. The judge presses CANCEL.
6. Click **Mark rejected**. The note turns green: "The holder saw the real amount on the badge and refused. The defence held."

Timing: the transaction's blockhash is good for roughly 90 seconds (the panel counts down), and the badge listener serves an attempt for the same 90 seconds. Build the charge right before handing it over. If it expires, click Charge again.

If the judge signs instead: the attempt flips to `signed` (red) by itself once the transfer lands, and the Feed shows a red double-bordered block labelled "TAMPERED — signed".

### 4. Revocation (if F15 ships on the badge)

Panel: **REGISTRY** (`#/registry`).

1. In the Attestations table, find the badge's row and click **Revoke**. Confirm the dialog.
2. A green note appears: `Revoked "<name>" for <badge>` with a **View transaction** link. The row's status becomes `revoked`.
3. The badge's next request shows red on the payer's badge (firmware side).
4. If a payment to the revoked badge still goes through, its block on the Feed is red with `⊘`.

To verify a badge live during the demo instead: Issue card, pick the badge, type the name (1 to 32 printable ASCII characters), click **Issue attestation**. Issuing again to the same badge replaces its name.

### 5. Close

Back to **FEED**. Leave the chain on screen.

## Recovery

| Symptom | Move |
|---|---|
| Devnet or the hotspot is down, nothing arrives | Flip the data switch to **DEMO**. The chain keeps moving on fixture data. Blocks are dashed and tagged `~demo`, and the pill reads `DEMO`; say so |
| Faucet never recovered, no mint | LIVE with `npm run db:seed`: the ledger and the stat tiles show seeded history (tiles say "includes N seeded rows"; rows are tagged `~seed`). The chain strip itself shows the "HACK mint not configured" card until a mint exists. For a moving chain, use DEMO |
| A payment did not show up | Check the pill. `RECONNECTING`: wait, the subscription retries (1 s doubling to 30 s) and a 15 s poll backfills missed transfers. Then send another: `npm run pay -- --from 3 --to 1 --amount 10` |
| Red "Backend offline" banner | In the terminal: `node --env-file-if-exists=.env server/src/index.js`. Then click **Retry** (the panel also re-checks every 5 s) |
| "Database is offline" card | `npm run db:up`. The server reconnects by itself within about 5 s |
| "Database schema is missing" card | `npm run db:migrate` |
| "Solana RPC is unreachable" card | Check the hotspot. The server retries on its 30 s tick |
| "HACK mint not configured" after setup | The server was not restarted after `devnet:setup` wrote `.env`. Restart it |
| Issue or revoke fails with "registry authority … has … SOL" | Fund the authority at <https://faucet.solana.com>, try again |
| Attack attempt shows `expired` | Click Charge again |
| A page shows "This page crashed" | Click another tab and come back, or reload. The other pages are unaffected |
| `npm run web` fails: port 5173 in use | Another Vite is running. Stop it; the port is fixed |
| Edited `badges.json` or `.env` | Restart the server (there is no watch mode) |

DEMO is the safe default for anything unexpected: it needs no server, no database and no network.
