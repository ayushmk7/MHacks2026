# Runbook

Demo-day sequence for the laptop panel, mapped to the PRD demo plan. Read "What is verified" first: as of 2026-10-03 nothing has been sent on real devnet with our own mint.

Every command in this document runs from `dashboard/`:

```bash
cd dashboard
```

Paths are relative to `dashboard/` as well.

## What is verified

"Run" means the command or route was executed on this laptop and behaved as documented. The last column says when, and whether it was re-run while this document was updated ("docs pass") or reported by the debugging pass earlier the same day.

### Run for real

| Area | Result | When |
|---|---|---|
| `npm run check` (11 tests + web build) | Passes | 2026-10-03, docs pass |
| `db:up`, `db:down`, `db:migrate` (re-run on an existing schema), `db:psql` | Run. `db:down` then `db:up` keeps the data (same volume, `badgepay_badgepay-db`) | 2026-10-03, docs pass |
| Seeded database: 51,840 rows, 72 of 74 chunks in columnstore, ratio 0.668 | Numbers re-read from the existing database. `db:reset` and `db:seed` were not re-run | 2026-10-03 18:10 UTC, docs pass |
| Every GET route, all validation errors, Host / Origin / Content-Type / size checks | Run with `curl`; outputs are in [API.md](API.md) | 2026-10-03, docs pass |
| Postgres NOTIFY -> server -> SSE | A manual `pg_notify` arrived on `/api/events`. A malformed payload was logged and the server kept running | 2026-10-03, docs pass |
| Database outage and recovery | `db:down`: `db_offline` in `/api/status` within 3 s. `db:up`: the server reconnected by itself, sent `status` frames, and a NOTIFY sent afterwards arrived on the stream | 2026-10-03, docs pass |
| RPC outage | With an `RPC_URL` that never answers: requests fail after 10 s as `chain_error`, `rpc_unreachable` appears in `gaps`, `/api/badges` returns `sol: null` | 2026-10-03, docs pass |
| Badge listener routes | 204, 400, 404 cases; and 200, `{"ok":true}`, 409 with an attempt row inserted by hand (dummy transaction bytes), deleted afterwards. Three polls produced one `attack` frame | 2026-10-03, docs pass |
| Vite proxy for `/api` and SSE | A proxied stream ends the moment the server is killed; the proxy answers 502 while it is down | 2026-10-03, docs pass |
| Panel recovery in a browser | Headless Chrome against the real server: pill goes `OFFLINE` while the API is down; a new block arrives about 9 s after the API restarts | 2026-10-03, debugging pass |
| Ingest read path (subscribe, parse, insert, notify, browser) on real devnet | Live devnet USDC traffic, not our own mint. Live lag p50 about 2.5 s, p95 about 2.7 s, zero 429s while a backfill was running | 2026-10-03, debugging pass, with `getTransaction` pacing |
| Same pipeline before pacing existed | Lag 0.8 to 1.6 s. Pacing was added on 2026-10-03 after a 429 storm starved the live path on a busy mint | 2026-10-02 |
| UI in DEMO and LIVE | Checked with headless Chrome scripts. Playwright MCP was not available. No UI tests ship in the repo | 2026-10-03, debugging pass |

### Run against a stand-in chain only

On 2026-10-03 the on-chain flows were exercised against an in-memory stand-in chain: a fake JSON-RPC and WebSocket server that applies the server's real instruction bytes. It does not verify signatures and does not run program logic. 51 scripted checks pass. The stand-in is not in this repository.

| Flow | Stand-in chain | Real devnet |
|---|---|---|
| HACK mint creation, badge funding (`devnet:setup` past the airdrop) | Exercised | **Not run** (no SOL) |
| `POST /api/attestations` and `/revoke` success paths | Exercised | **Not sent.** The SAS instruction sequences pass `simulateTransaction` on real devnet |
| `npm run pay` | Exercised | **Not run** (no mint, no funded stand-ins). Without a mint it stops with `HACK_MINT is not set` (docs pass) |
| `POST /api/attacks` success path, `/badge/pending` serving a real attempt | Exercised | **Not run** (needs the mint) |
| Attack `signed` auto-detection | Exercised | **Not run** (needs a signed transfer on chain) |

### Not run at all

| Area | Why |
|---|---|
| Real badges (any gap in [BADGE-GAPS.md](BADGE-GAPS.md)) | No hardware yet |
| `devnet:setup` against a mint the authority does not control (skip `mintTo`, warn) | From the code only |
| Tiger Cloud (`DATABASE_URL` swap, TLS) | Local Docker only |

## First real devnet run

Blocked until the registry authority has devnet SOL. The faucet is rate-limiting this laptop's IP. It was retried once on 2026-10-03 and failed again.

1. Get the authority address. It is the first line of `npm run devnet:setup`, the `authority.pubkey` field of `GET /api/status`, and the Authority field on the Registry page. On this laptop: `FZEAS6Nayu6nMX1RVmoNwoPA4tu1KNM5pvfoXsQzei3K`.
2. Fund it with at least 0.5 SOL at <https://faucet.solana.com> (devnet). From another network or a phone if this IP is still limited.
3. `npm run devnet:setup`. Re-run until it prints `Done.` and the summary table. Expect: mint created and written to `.env`, four badges funded (0.05 SOL, 1000 HACK each), credential and schema created, "MHacks Merch" issued to badge 1.
4. Restart the server (there is no watch mode).
5. Confirm:

   ```bash
   curl -s http://127.0.0.1:8787/api/status
   ```

   `ingest.state` is `live`, `token.mint` is set, `registry.ready` is `true`, and `gaps` has no `blocker`.
6. `npm run pay -- --from 3 --to 1 --amount 10`. The script prints an explorer link. A green block should appear on the Feed within about 3 seconds (measured on USDC traffic: p50 about 2.5 s, p95 about 2.7 s).
7. On the Registry page, issue a name to badge 2, then revoke it. Each should show a "View transaction" link that opens on the devnet explorer.
8. On the Attack page, charge Judge A. The claimed-vs-actual panel should appear with a "Transaction" size in bytes.
9. Update the tables above and the Status section of [the dashboard README](../../dashboard/README.md) and [the root README](../../README.md).

If any of 3 to 8 fails, the error text is in the terminal running the server or in the red note on the page. Report it; these paths have only run against the stand-in chain.

## Pre-flight checklist (before judges arrive)

- [ ] Laptop and badges on the same phone hotspot.
- [ ] `npm run db:up` (container `badgepay-db-1` healthy).
- [ ] `npm run db:migrate`.
- [ ] `npm run db:seed` as faucet insurance. It only replaces seed rows; chain rows are kept.
- [ ] `npm run devnet:setup` finishes with `Done.` and every badge shows SOL and HACK in its table.
- [ ] Start the server and the web app in two terminals (or both with `npm run dev`):

  ```bash
  npm run server
  npm run web
  ```

  Open <http://127.0.0.1:5173>.
- [ ] Switch to LIVE. The pill reads `LIVE`. No banner under the top bar.
- [ ] Badges page: four cards, each with a HACK balance and SOL for fees. Badge 1 shows `verified` and "MHacks Merch".
- [ ] Registry page: Authority shows more than 0.02 SOL; the chip reads `on chain`.
- [ ] `npm run pay -- --from 3 --to 1 --amount 10`: a block appears within about 3 seconds. Click it; the Explorer link opens.
- [ ] Feed "Ingest lag · p95" tile is at or under 3 s (green).
- [ ] Attack page: if a real badge will receive the charge, `BADGE_LISTEN_HOST` is set and the Delivery card reads "listener open". Otherwise expect the "Badge delivery listener is closed" card. Leave the listener closed when no badge needs it: it has no authentication.
- [ ] Pick the theme that reads best on the projector (top-right switch).
- [ ] Leave a terminal open in `dashboard/` for `npm run pay`.

With stand-in badges only, steps 1, 2 and 4 below can be rehearsed with `npm run pay`. With real badges, `npm run pay` refuses to sign for them ("its key never leaves the badge"); the badge sends the payment.

Do not send payments faster than about one per second during the demo. The server fetches one transaction per 1.1 s, so a burst queues up and each block arrives later than the one before. This follows from the code ([ARCHITECTURE.md](ARCHITECTURE.md#gettransaction-pacing)); a burst was not measured.

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
3. The right panel shows `5 HACK ≠ 500 HACK`, "100× the claim", the destination, and the "Transaction" field (wire size in bytes, message version). Amber notes under it are warnings from the server, for example that the victim holds less than the real amount.
4. Delivery to the badge (see `attack-delivery` in [BADGE-GAPS.md](BADGE-GAPS.md)): with the listener open, the Delivery card shows the URL the badge polls. Without badge firmware for this, narrate from the panel.
5. The badge shows 500. The judge presses CANCEL.
6. Click **Mark rejected**. The note turns green: "The holder saw the real amount on the badge and refused. The defence held."

Timing: the attempt expires 90 seconds after it is built (the panel counts down), and the badge listener serves it for the same 90 seconds. Build the charge right before handing it over. If it expires, click Charge again. An expired attempt can still be marked rejected.

If the judge signs instead: the attempt flips to `signed` (red) by itself once the transfer lands, and the Feed shows a red double-bordered block labelled "TAMPERED — signed".

### 4. Revocation (if F15 ships on the badge)

Panel: **REGISTRY** (`#/registry`).

1. In the Attestations table, find the badge's row and click **Revoke**. Confirm the dialog.
2. A green note appears: Revoked “name” for badge, with a **View transaction** link. The row's status becomes `revoked`.
3. The badge's next request shows red on the payer's badge (firmware side).
4. If a payment to the revoked badge still goes through, its block on the Feed is red with `⊘`.

To verify a badge live during the demo instead: Issue card, pick the badge, type the name (1 to 32 printable ASCII characters), click **Issue attestation**. Issuing again to the same badge replaces its name.

### 5. Close

Back to **FEED**. Leave the chain on screen.

## Recovery

The panel recovers from a server or database restart by itself. The status poll runs every 5 s, a closed event stream is re-opened after 3 s, and everything on screen is refetched when the backend answers again. The Retry button in the banner does the same thing at once.

| Symptom | Move |
|---|---|
| Devnet or the hotspot is down, nothing arrives | Flip the data switch to **DEMO**. The chain keeps moving on fixture data. Blocks are dashed and tagged `~demo`, and the pill reads `DEMO`; say so |
| Faucet never recovered, no mint | LIVE with `npm run db:seed`: the ledger and the stat tiles show seeded history (tiles say "includes N seeded rows"; rows are tagged `~seed`). The chain strip itself shows the "HACK mint not configured" card until a mint exists. For a moving chain, use DEMO |
| A payment did not show up | Check the pill. `RECONNECTING`: wait, the subscription retries (1 s doubling to 30 s). A transfer that was missed or whose fetch failed is picked up by the 15 s safety poll, which also retries failed signatures. Then send another: `npm run pay -- --from 3 --to 1 --amount 10` |
| Pill `OFFLINE`, red "Backend offline" banner | Nothing answers on 8787. In a terminal in `dashboard/`: `npm run server`. The panel reconnects within a few seconds (measured: a new block about 9 s after the restart) |
| Pill `ERROR`, red "Backend error" banner | Something answers, but not with the API's JSON. The banner shows the message, for example "The server answered HTTP 500 without a JSON error body". Check the terminal running the server, and check that port 8787 is this server and not another program. Then Retry |
| "Database is offline" card | `npm run db:up`. The server reconnects by itself within about 5 s |
| "Database schema is missing" card | `npm run db:migrate` |
| "Solana RPC is unreachable" card | Check the hotspot. The server retries on its 30 s tick. While it lasts, issue, revoke and charge fail with the RPC error (after 10 s if the endpoint does not answer at all) |
| A card says "Feed unavailable", "Badges unavailable" or similar with a database message | One query failed or took over 5 s. The other pages keep working. It clears on the next refetch |
| "HACK mint not configured" after setup | The server was not restarted after `devnet:setup` wrote `.env`. Restart it |
| Issue or revoke fails with "registry authority … has … SOL" | Fund the authority at <https://faucet.solana.com>, try again |
| Attack attempt shows `expired` | Click Charge again |
| A page shows "This page crashed" | Click another tab and come back, or reload. The other pages are unaffected |
| `npm run web` fails: `Port 5173 is already in use` | Another Vite is running. Stop it; the port is fixed |
| Edited `server/config/badges.json`, `.env` or server code | Restart the server (there is no watch mode) |
| Server log shows `[unhandled] …` | A background task failed and was caught. The server is still up. Note the message and carry on |

DEMO is the safe default for anything unexpected: it needs no server, no database and no network.
