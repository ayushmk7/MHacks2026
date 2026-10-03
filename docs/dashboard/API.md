# API

Everything here is implemented in `server/src/http.js`. Paths in this document are relative to `dashboard/`, and commands run from `dashboard/`.

Response examples were captured from the running server on 2026-10-03 (database seeded, `HACK_MINT` empty, authority unfunded). Examples marked "from code" belong to write routes that need devnet SOL; they have not run on real devnet (see [RUNBOOK.md](RUNBOOK.md#what-is-verified)).

- Base URL: `http://127.0.0.1:8787` (`HOST`, `PORT` in `.env`). The browser reaches it same-origin through the Vite proxy at `http://127.0.0.1:5173/api`. `API_URL=<url> npm run web` points the proxy at another backend.
- All bodies are JSON. JSON responses carry `Cache-Control: no-store`. There are no CORS headers.
- Amounts: `amount` is a UI number (raw / 10^decimals). `amountRaw` is the raw integer as a string.

Start a server to try the examples:

```bash
cd dashboard
npm run db:up
node --env-file-if-exists=.env server/src/index.js
```

## Routes

| Method | Path | Purpose |
|---|---|---|
| GET | `/api/status` | Health, config, and the `gaps` list that drives every degraded UI state |
| GET | `/api/payments` | Payment feed, newest first |
| GET | `/api/stats` | 24 h totals, ingest lag, 61 one-minute buckets |
| GET | `/api/stats/db` | TimescaleDB numbers for the "Powered by Tiger Data" card |
| GET | `/api/badges` | The badge slots with balances, attestation and received totals |
| GET | `/api/attestations` | Mirror of the on-chain registry |
| POST | `/api/attestations` | Issue (or re-issue / rename) an attestation |
| POST | `/api/attestations/revoke` | Revoke (close) an attestation |
| GET | `/api/attacks` | Attack-console attempts |
| POST | `/api/attacks` | Build an unsigned tampered transfer |
| POST | `/api/attacks/outcome` | Mark an attempt rejected |
| GET | `/api/events` | Server-sent events |
| GET | `/badge/pending` | Badge listener only (separate port, off by default) |
| POST | `/badge/outcome` | Badge listener only |

## Request checks

Applied in this order on the main listener (`handle`, `server/src/http.js:213`):

1. `Host` hostname must be `localhost` or `127.0.0.1`, else 403 `forbidden_origin` ("Host not allowed"). Blocks DNS rebinding.
2. A request target that does not parse as a URL (for example `GET //`): 404 `not_found`.
3. Unknown method + path: 404 `not_found`. A POST to a GET route is also 404.
4. POST only (`readJson`, `server/src/http.js:69`):
   1. An `Origin` header, if present, must be in `WEB_ORIGINS`, else 403 `forbidden_origin` ("Origin not allowed"). Entries in `WEB_ORIGINS` have trailing slashes stripped, so `http://127.0.0.1:5173/` in `.env` still matches.
   2. `Content-Type` must start with `application/json`, else 415.
   3. Body at most 16 KB, else 413 (sent with `Connection: close`).
   4. Body must parse as a JSON object, else 400 `bad_json`.

Two cases never reach these checks. Node answers them itself, with no JSON body:

| Request | Response |
|---|---|
| HTTP/1.1 without a `Host` header | `400 Bad Request`, empty body |
| Headers over 16 KB | `431 Request Header Fields Too Large`, empty body |

Field rules (`validate`, `server/src/http.js:24`):

| Field | Rule |
|---|---|
| `pubkey`, `victim` | base58 32-byte address (`isAddress`) |
| `name` | trimmed, 1 to 32 printable ASCII characters (`/^[\x20-\x7E]+$/`) |
| `displayAmount`, `actualAmount` | JSON number, `<= 1000000`, at most `decimals` decimal places, and at least 1 raw unit after rounding (so `0`, negatives, `0.001` and `1e-9` are all rejected at 2 decimals) |
| `limit` | integer 1 to 200 |
| `before` | anything `Date.parse` accepts |
| `id` | UUID |

## Errors

Every non-2xx response produced by the server code has this shape. `gap` appears only on `not_configured` and sits inside `error`:

```json
{"error":{"code":"not_configured","message":"HACK mint is not configured or not found on chain","gap":"hack_mint_missing"}}
```

| Code | HTTP | When |
|---|---|---|
| `bad_json` | 400 | Body is not valid JSON, or not a JSON object |
| `invalid_pubkey` | 400 | `pubkey` / `victim` / `badge` is not an address |
| `invalid_name` | 400 | `name` fails the rule above |
| `invalid_amount` | 400 | Amount fails the rule above, including amounts that round to 0 raw units |
| `invalid_param` | 400 | Bad `limit`, `before`, `id`, or `outcome` other than `"rejected"` |
| `forbidden_origin` | 403 | Bad `Host` or bad `Origin` |
| `unknown_badge` | 404 | `victim` / `badge` is not a pubkey in `server/config/badges.json` |
| `not_found` | 404 | No such route, unparseable request target, or no such attack attempt |
| `conflict` | 409 | Attempt is not pending; or revoke with no on-chain attestation |
| `too_large` | 413 | Body over 16 KB |
| `unsupported_media_type` | 415 | POST without `Content-Type: application/json` |
| `internal` | 500 | Unexpected error. Message is always `internal error`; details go to the server log |
| `chain_error` | 502 | A Solana RPC call or transaction failed. Message is the RPC error text. An RPC request that gets no answer within 10 s fails this way ("The operation was aborted due to timeout") |
| `not_configured` | 503 | `error.gap` is `authority_unfunded` or `hack_mint_missing` |
| `db_error` | 503 | A database query failed. Message is the driver's error text. A query that takes longer than 5 s fails this way |

Captured:

```bash
curl -s -H 'Host: evil.com' http://127.0.0.1:8787/api/status
# 403 {"error":{"code":"forbidden_origin","message":"Host not allowed"}}

curl -s --path-as-is 'http://127.0.0.1:8787//'
# 404 {"error":{"code":"not_found","message":"no such route"}}

curl -s -X POST -H 'Content-Type: text/plain' -d '{}' http://127.0.0.1:8787/api/attestations
# 415 {"error":{"code":"unsupported_media_type","message":"Content-Type must be application/json"}}

curl -s -X POST -H 'Origin: http://evil.example' -H 'Content-Type: application/json' -d '{}' http://127.0.0.1:8787/api/attestations
# 403 {"error":{"code":"forbidden_origin","message":"Origin not allowed"}}

curl -s -X POST -H 'Content-Type: application/json' -d '[1]' http://127.0.0.1:8787/api/attestations
# 400 {"error":{"code":"bad_json","message":"body must be a JSON object"}}

curl -s 'http://127.0.0.1:8787/api/payments?limit=0'
# 400 {"error":{"code":"invalid_param","message":"limit must be an integer from 1 to 200"}}

curl -s 'http://127.0.0.1:8787/api/payments?before=nope'
# 400 {"error":{"code":"invalid_param","message":"before must be an ISO timestamp"}}
```

Captured on a second instance started with `DATABASE_URL` pointing at a closed port:

```bash
curl -s http://127.0.0.1:8797/api/payments
# 503 {"error":{"code":"db_error","message":"connect ECONNREFUSED 127.0.0.1:5999"}}
```

## GET /api/status

Always 200 while the process is listening, including when the database or RPC is down. It reads state held in memory and makes no database or RPC call.

```bash
curl -s http://127.0.0.1:8787/api/status
```

```json
{
  "ok": true, "app": "badgepay", "cluster": "devnet", "explorer": "https://explorer.solana.com",
  "db": { "ok": true, "schema": true, "error": null },
  "rpc": { "ok": true, "url": "https://api.devnet.solana.com" },
  "ingest": { "state": "idle", "lastSignature": null, "lastEventAt": null },
  "token": { "mint": null, "symbol": "HACK", "decimals": 2 },
  "authority": { "pubkey": "FZEAS6Nayu6nMX1RVmoNwoPA4tu1KNM5pvfoXsQzei3K", "sol": 0 },
  "registry": {
    "program": "22zoJMtdu4tQc2PzL74ZUT7FrwgB1Udec8DdW4yw4BdG",
    "credential": "GvGHuhBZMp3v7bjtFFsog7L8jKKpDP54tKcvg1RHC3UP",
    "schema": "GE5gFbZ3Cgqx8boTotq8oDAXoxMK3UtDy3jaWX2P7gD2",
    "credentialName": "MHacks Verified", "schemaName": "badge-identity", "schemaVersion": 1, "ready": false
  },
  "badgeListener": { "open": false, "url": null },
  "gaps": [
    { "code": "hack_mint_missing", "severity": "blocker", "badgeGap": "hack-mint",
      "title": "HACK mint not configured",
      "fix": "Run `npm run devnet:setup` or set HACK_MINT in .env, then restart `npm run server`.",
      "pages": ["feed", "badges", "attack"] }
  ]
}
```

(`gaps` is trimmed to one entry here. The same call returned six: `hack_mint_missing`, `badges_stand_in`, `authority_unfunded`, `registry_uninitialised`, `attack_delivery_unconfigured`, `attack_outcome_manual`.)

- `db.error` is the driver's message while the database is unreachable (for example `connect ECONNREFUSED 127.0.0.1:5999`), else `null`.
- `rpc.ok` is `false` only after a balance read has failed. Right after boot it is `true` until the first read finishes, so a dead RPC shows up after at most one 10 s timeout.
- `rpc.url` is the origin only, so an API key in `RPC_URL` is never echoed.
- `ingest.state`: `idle` (no mint), `backfilling`, `live`, `reconnecting`.
- `token.mint` is `null` when `HACK_MINT` is unset. `token.decimals` is read from the chain once the mint is found; before that it is `HACK_DECIMALS`.
- `authority.sol` is `null` until the first balance read succeeds. It is refreshed every 30 s and on every issue/revoke.
- `registry.ready` is true once both the credential and schema accounts exist on chain.
- `badgeListener.url` is the URL the badge polls, or `null` when closed. With `BADGE_LISTEN_HOST=0.0.0.0` the URL shows this machine's first non-internal IPv4 address, not `0.0.0.0`.

### Gaps

`gaps` is computed by `computeGaps()` (`server/src/config.js:67`). Each entry has `code`, `severity` (`blocker` | `warn` | `info`), `badgeGap` (a `BADGE-GAP` id or `null`), `title`, `fix`, `pages`.

| Code | Severity | badgeGap | Condition | Pages |
|---|---|---|---|---|
| `db_offline` | blocker | | Pool cannot connect | all four |
| `db_schema_missing` | blocker | | `payments` table absent | all four |
| `rpc_unreachable` | blocker | | The last balance read failed (30 s tick, or `/api/badges`) | all four |
| `hack_mint_missing` | blocker | `hack-mint` | `HACK_MINT` empty/invalid, or mint account not found | feed, badges, attack |
| `badges_unconfigured` | blocker | `badge-pubkeys` | No badge has a pubkey | badges, registry, attack |
| `badges_partial` | warn | `badge-pubkeys` | Some slots have no pubkey | badges, registry, attack |
| `badges_stand_in` | info | `badge-pubkeys` | Any badge has `standIn: true` | badges |
| `key_location_unknown` | warn | `key-location` | A configured badge has `keyLocation: "unknown"` | badges |
| `config_invalid` | warn | | `badges.json` has invalid entries (title lists them) | badges |
| `authority_unfunded` | warn | | Authority balance is known and below 0.02 SOL | registry, attack |
| `registry_uninitialised` | info | | Credential or schema not on chain yet | registry |
| `attack_delivery_unconfigured` | warn | `attack-delivery` | Badge listener is closed | attack |
| `attack_outcome_manual` | info | `attack-outcome` | Always | attack |

## GET /api/payments

Query: `limit` (1 to 200, default 40), `before` (ISO timestamp; returns rows with `blockTime` strictly older).

```bash
curl -s 'http://127.0.0.1:8787/api/payments?limit=2'
```

```json
{ "payments": [
  { "signature": "SEED6b86b273ff34fce19d6b804eff5a3f5747ada4eaa22f1d49c01e52ddb7875b4b",
    "slot": 399999999, "blockTime": "2026-10-03T16:56:39.087Z",
    "payer": { "pubkey": "4vJDQvQxRrfLYpjFWXFsVTMsw7cayvWB46pnzdn2BW97", "badgeId": 3, "label": "Judge A" },
    "payee": { "pubkey": "Gn2GQYmcQkatE2594ov2ELKkYWZHwARG7FiAoPWSEcxq", "badgeId": 1, "label": "Merchant (team)",
               "name": "MHacks Merch", "status": "verified" },
    "amount": 8, "amountRaw": "800", "decimals": 2, "symbol": "HACK",
    "mint": "SeedMint11111111111111111111111111111111111",
    "payerTokenAccount": "SeedTokenAcct3", "payeeTokenAccount": "SeedTokenAcct1",
    "source": "seed", "attackId": null, "lagMs": null,
    "prevSignature": "SEEDd4735e3a265e16eee03f59718b9b5d03019c07d8b6c51f90da3a666eec13ab35" },
  { "signature": "SEEDd4735e3a265e16eee03f59718b9b5d03019c07d8b6c51f90da3a666eec13ab35", "…": "…", "prevSignature": null }
] }
```

- `payee.status`: `verified` | `unverified` | `revoked`. It is a snapshot taken when the row was ingested, so a later revoke does not rewrite history. `payee.name` is the attested name at that moment, or `null`.
- `badgeId` and `label` are `null` for wallets that are not in `badges.json`.
- `source`: `chain` (ingested from devnet) or `seed` (`npm run db:seed`). DEMO fixtures in the browser use `demo`; the server never returns it.
- `attackId` is set when the transfer matched an attack attempt (the tampered transfer was signed).
- `lagMs` is block time to database insert. `null` for seed rows and for rows picked up by the safety poll (backfill and retries).
- `prevSignature` is the next-older row in this page, `null` on the page's last row.

### Paging caveat

`before` is a strict timestamp cursor (`block_time < before`, `server/src/http.js:124`). Rows that share the block time of a page's last row are skipped if the next request passes that block time as `before`. Chain block times are whole seconds, so two transfers in one slot share one. This is a known limit and is not fixed in the server.

The web client works around it (`loadOlder`, `web/src/pages/Feed.jsx:182`): it asks for `before = <last blockTime> + 1 ms` and drops rows it already shows. A client that pages by hand should do the same. Checked on the seeded data:

```bash
curl -s 'http://127.0.0.1:8787/api/payments?limit=3&before=2026-10-03T16:56:34.087Z'   # first row is 16:56:29.087Z
curl -s 'http://127.0.0.1:8787/api/payments?limit=3&before=2026-10-03T16:56:34.088Z'   # first row is 16:56:34.087Z again
```

The workaround has its own ceiling: 40 or more rows inside one millisecond would never page past it.

## GET /api/stats

```bash
curl -s http://127.0.0.1:8787/api/stats
```

```json
{ "window": "24h", "payments": 16448, "volume": 172700, "verifiedShare": 0.7, "seedRows": 16448,
  "lagMs": { "p50": null, "p95": null },
  "series": [ { "t": "2026-10-03T17:05:00.000Z", "n": 0, "verifiedN": 0, "volume": 0 } ],
  "queryMs": 25.8 }
```

- Totals come from the `payments_1m` continuous aggregate over the last 24 hours. They include seed rows; `seedRows` says how many.
- `verifiedShare` is rounded to 3 places and is `0` when there are no payments.
- `lagMs` is the p50/p95 of `ingest_lag_ms` over chain rows in the last hour. Both are `null` when there are none.
- `series` has 61 one-minute buckets, oldest first, zero-filled. `series[0]` is the minute that contains "now minus 60 minutes" and holds the real count for that whole minute. (Before 2026-10-03 it was always 0. The zeros in the capture above are real: the newest seed row was 68 minutes old.) Checked in SQL against a fixed past window: the first bucket and a direct `count(*)` for that minute both returned 12.
- `queryMs` is how long the series query took in this request.

## GET /api/stats/db

```bash
curl -s http://127.0.0.1:8787/api/stats/db
```

```json
{ "timescaledb": "2.30.2",
  "payments": { "rows": 51840, "chunks": 74, "columnstoreChunks": 71,
                "bytesBefore": 29638656, "bytesAfter": 9822208, "compressionRatio": 0.669, "totalBytes": 10117120 },
  "caggs": [
    { "name": "payments_1m", "realtime": true, "scheduleSec": 60, "lastRefresh": "2026-10-03T18:04:53.643Z", "lastStatus": "Success" },
    { "name": "payee_volume_1h", "realtime": true, "scheduleSec": 300, "lastRefresh": "2026-10-03T18:01:44.588Z", "lastStatus": "Success" }
  ],
  "queryMs": { "feed": 57, "series": 6.8 } }
```

(This capture was the first call after a server start. Five warm calls reported `feed` 12.4 to 15.6 and `series` 2.4 to 4.8.)

- `rows` is an exact `count(*)`.
- `compressionRatio = 1 - bytesAfter / bytesBefore`, rounded to 3 places. `null` when no chunk has been converted.
- `lastRefresh` is `null` for a refresh job that has never finished.
- `lastStatus` is the job's last run status from TimescaleDB. Known limit, not fixed: `payments_1m` can report `"Failed"` for a few minutes right after `npm run db:seed`, until its next scheduled refresh succeeds. This was reported by the debugging pass on 2026-10-03 and not reproduced for this document (it needs a re-seed).
- `queryMs`: this route re-runs the feed query (limit 40) and the series query one after the other and reports each one's time in ms. A call that lands on a fresh database session is slower (50 to 57 ms for the feed query; see [TIGER-DATA.md](TIGER-DATA.md#measured-numbers-2026-10-03-local-docker)).

## GET /api/badges

```bash
curl -s http://127.0.0.1:8787/api/badges
```

```json
{ "badges": [
  { "id": 1, "label": "Merchant (team)", "pubkey": "Gn2GQYmcQkatE2594ov2ELKkYWZHwARG7FiAoPWSEcxq",
    "tokenAccount": null, "keyLocation": "software", "standIn": true,
    "sol": 0, "hack": null,
    "attestation": { "status": "unverified", "name": null, "pda": null, "issuedSig": null, "revokedSig": null },
    "received": { "count": 0, "amount": 0 } }
] }
```

(Three more entries of the same shape.)

- `keyLocation`: `se050` | `software` | `unknown`.
- `sol` and `hack` come from one `getMultipleAccounts` call, cached 5 s. Both are `null` if the RPC call fails (captured with an unreachable `RPC_URL`). `hack` is `null` while the mint is missing.
- `tokenAccount` is the value from `badges.json`, else the derived associated token account, else `null` (no mint).
- `attestation.status`: `verified` | `unverified` | `revoked`.
- `received` is the all-time total of chain rows from the `payee_volume_1h` continuous aggregate. Seed rows are excluded (`WHERE source = 'chain'`, `server/src/db.js:82`).
- A slot with `"pubkey": null` is returned with `sol`, `hack`, `tokenAccount` all `null`.

## GET /api/attestations

```bash
curl -s http://127.0.0.1:8787/api/attestations
# {"attestations":[]}
```

One entry per subject, most recently updated first (shape from code, `toAttestation`, `server/src/db.js:255`):

```json
{ "attestations": [
  { "subject": "<badge pubkey>", "badgeId": 1, "label": "Merchant (team)", "name": "MHacks Merch",
    "status": "verified", "pda": "<attestation PDA>",
    "issuedSig": "<tx signature>", "issuedAt": "<ISO>", "revokedSig": null, "revokedAt": null, "expiresAt": "<ISO>" }
] }
```

- `status`: `verified` | `revoked`. A subject that was never attested has no row.
- `issuedSig` / `revokedSig` are `null` when the change was discovered by the 30 s chain sync rather than made through this API.
- Registry metadata (program, credential, schema, authority) is in `/api/status`.

## POST /api/attestations

Issues an attestation. If one already exists for the pubkey, it is closed and re-created in the same transaction (rename / re-issue). Creates the credential and schema first if they are not on chain yet. Expiry is 30 days from now.

```json
{ "pubkey": "<base58>", "name": "MHacks Merch" }
```

Response 201 (from code):

```json
{ "attestation": { "…same shape as above…": "" },
  "signature": "<tx signature>",
  "explorerUrl": "https://explorer.solana.com/tx/<signature>?cluster=devnet" }
```

The response does not depend on the database. After the transaction lands, the server looks for the mirror row that carries this signature. If the row is missing or stale, or the database is down, `attestation` is built from what the chain call returned (`attestationResult`, `server/src/http.js:107`) and the status is still 201. The 30 s sync writes the mirror row later.

Errors: `invalid_pubkey`, `invalid_name`, `chain_error` (balance read or transaction failed), `not_configured` (`gap: authority_unfunded`, authority below 0.02 SOL).

Captured:

```bash
curl -s -X POST -H 'Content-Type: application/json' \
  -d '{"pubkey":"nope","name":"MHacks Merch"}' http://127.0.0.1:8787/api/attestations
# 400 {"error":{"code":"invalid_pubkey","message":"pubkey must be a base58 32-byte address"}}

curl -s -X POST -H 'Content-Type: application/json' \
  -d '{"pubkey":"11111111111111111111111111111111","name":"café"}' http://127.0.0.1:8787/api/attestations
# 400 {"error":{"code":"invalid_name","message":"name must be 1 to 32 printable ASCII characters"}}

curl -s -X POST -H 'Content-Type: application/json' \
  -d '{"pubkey":"11111111111111111111111111111111","name":"MHacks Merch"}' http://127.0.0.1:8787/api/attestations
# 503 {"error":{"code":"not_configured","message":"registry authority FZEAS6Nayu6nMX1RVmoNwoPA4tu1KNM5pvfoXsQzei3K has 0 SOL, fund it at https://faucet.solana.com","gap":"authority_unfunded"}}
```

With an `RPC_URL` that never answers, the same valid request returned after 10.0 s:

```
502 {"error":{"code":"chain_error","message":"The operation was aborted due to timeout"}}
```

## POST /api/attestations/revoke

Closes the attestation account. `{ "pubkey": "<base58>" }`. Response 200 (from code), same shape as issue, with `status: "revoked"` and `signature` = the closing transaction.

As with issue, the response does not depend on the database. In the fallback (mirror row unavailable) the attestation is built from the chain call: `name` and `expiresAt` are read from the on-chain account before it is closed, `revokedSig` and `revokedAt` are set, and `issuedSig` and `issuedAt` are `null`.

Check order: `invalid_pubkey`, then authority funding, then existence. So an unfunded authority returns 503 even when there is nothing to revoke:

```bash
curl -s -X POST -H 'Content-Type: application/json' \
  -d '{"pubkey":"11111111111111111111111111111111"}' http://127.0.0.1:8787/api/attestations/revoke
# 503 {"error":{"code":"not_configured","message":"registry authority FZEAS6Nayu6nMX1RVmoNwoPA4tu1KNM5pvfoXsQzei3K has 0 SOL, fund it at https://faucet.solana.com","gap":"authority_unfunded"}}
```

With a funded authority and no on-chain attestation: 409 `conflict` (from code).

## GET /api/attacks

Query: `limit` (1 to 200, default 20). Newest first.

```bash
curl -s 'http://127.0.0.1:8787/api/attacks?limit=5'
# {"attempts":[]}
```

Attempt shape (`toAttack`, `server/src/db.js:260`). The field set was captured from a row inserted by hand with dummy transaction fields, then deleted; the values below are placeholders:

```json
{ "attempts": [
  { "id": "<uuid>", "createdAt": "<ISO>", "expiresAt": "<createdAt + 90 s>",
    "victim": { "pubkey": "<badge pubkey>", "badgeId": 3, "label": "Judge A" },
    "attacker": "<pubkey>",
    "displayAmount": 5, "actualAmount": 500, "symbol": "HACK", "decimals": 2,
    "txBase64": "<unsigned wire transaction>", "messageBase64": "<message bytes>",
    "txBytes": 0, "txVersion": "legacy",
    "blockhash": "<base58>", "lastValidBlockHeight": 0,
    "decoded": { "program": "spl-token", "instruction": "transferChecked",
                 "source": "<victim token account>", "destination": "<attacker token account>",
                 "mint": "<mint>", "authority": "<victim pubkey>",
                 "amount": 500, "amountRaw": "50000", "decimals": 2 },
    "outcome": "pending", "signature": null, "deliveredAt": null, "resolvedAt": null, "warnings": [] }
] }
```

- `outcome`: `pending` | `signed` | `rejected` | `expired`. `expired` is derived (stored as `pending` and older than 90 s), never stored.
- `expiresAt` is `createdAt` + 90 s. It is an estimate of the blockhash lifetime, not a value read from the chain. The same 90 s is used for `expired` and for how long the badge listener serves the attempt.
- `txBytes` is the wire size of `txBase64` (signature slots + message), not the message size. The Attack page labels it "Transaction".
- `txVersion`: `legacy` or `0`, from `ATTACK_TX_VERSION`.
- `signature` is set when `outcome` is `signed`: the ingest saw a transfer on chain with the same victim, attacker and raw amount. The match is idempotent per signature: if the payment insert fails and the transfer is handled again, the same attempt is returned and no second pending attempt is marked `signed`.
- `deliveredAt` is set the first time the badge listener hands the attempt to a badge.
- `warnings` is only populated in the POST response below. In this list it is always `[]`.

## POST /api/attacks

Builds an ordinary, unsigned SPL `transferChecked` from the victim's token account to the attacker's, for `actualAmount`, and stores it with the `displayAmount` the fake checkout claims. The server never signs or submits it.

```json
{ "victim": "<pubkey of a configured badge>", "displayAmount": 5, "actualAmount": 500 }
```

Amounts are optional (defaults 5 and 500). Response 201 (from code): one attempt object.

Check order: `invalid_pubkey`, `unknown_badge`, `invalid_amount`, `not_configured` (`gap: hack_mint_missing`), then the build (`chain_error`).

- Attacker = `ATTACKER_PUBKEY`, or the authority address when empty.
- The server tries to create the attacker's token account first. If that fails (for example the authority has no SOL), the attempt is still built and `warnings` gets an entry.
- `warnings` also gets an entry when the victim holds less HACK than `actualAmount`.
- `warnings` is returned only here. The Attack page keeps the POST response and shows its warnings under the attempt it just built.

Captured:

```bash
curl -s -X POST -H 'Content-Type: application/json' \
  -d '{"victim":"11111111111111111111111111111111"}' http://127.0.0.1:8787/api/attacks
# 404 {"error":{"code":"unknown_badge","message":"victim must be the pubkey of a configured badge"}}

curl -s -X POST -H 'Content-Type: application/json' \
  -d '{"victim":"4vJDQvQxRrfLYpjFWXFsVTMsw7cayvWB46pnzdn2BW97","actualAmount":0.001}' http://127.0.0.1:8787/api/attacks
# 400 {"error":{"code":"invalid_amount","message":"amount must be a number > 0 and <= 1000000 with at most 2 decimal places"}}

curl -s -X POST -H 'Content-Type: application/json' \
  -d '{"victim":"4vJDQvQxRrfLYpjFWXFsVTMsw7cayvWB46pnzdn2BW97","actualAmount":1e-9}' http://127.0.0.1:8787/api/attacks
# 400 {"error":{"code":"invalid_amount","message":"amount must be a number > 0 and <= 1000000 with at most 2 decimal places"}}

curl -s -X POST -H 'Content-Type: application/json' \
  -d '{"victim":"4vJDQvQxRrfLYpjFWXFsVTMsw7cayvWB46pnzdn2BW97"}' http://127.0.0.1:8787/api/attacks
# 503 {"error":{"code":"not_configured","message":"HACK mint is not configured or not found on chain","gap":"hack_mint_missing"}}
```

## POST /api/attacks/outcome

```json
{ "id": "<uuid>", "outcome": "rejected" }
```

Response 200: the attempt. Only `rejected` is accepted, and only for an attempt stored as `pending`. `signed` is never accepted from a client.

```bash
curl -s -X POST -H 'Content-Type: application/json' \
  -d '{"id":"x","outcome":"rejected"}' http://127.0.0.1:8787/api/attacks/outcome
# 400 {"error":{"code":"invalid_param","message":"id must be a UUID"}}

curl -s -X POST -H 'Content-Type: application/json' \
  -d '{"id":"0b7e2f44-91c3-4d1a-8e57-6a3c9f2d5b18","outcome":"signed"}' http://127.0.0.1:8787/api/attacks/outcome
# 400 {"error":{"code":"invalid_param","message":"outcome must be \"rejected\""}}

curl -s -X POST -H 'Content-Type: application/json' \
  -d '{"id":"0b7e2f44-91c3-4d1a-8e57-6a3c9f2d5b18","outcome":"rejected"}' http://127.0.0.1:8787/api/attacks/outcome
# 404 {"error":{"code":"not_found","message":"no such attempt"}}
```

An attempt that is already `signed` or `rejected` returns 409:

```
409 {"error":{"code":"conflict","message":"attempt is not pending"}}
```

An attempt shown as `expired` is still stored as `pending`, so it can be rejected. Checked with a hand-inserted row two minutes old: `GET /api/attacks` showed `expired`, this route returned 200 with `outcome: "rejected"`. The Attack page offers "Mark rejected" on both pending and expired attempts.

## GET /api/events (SSE)

```bash
curl -s -N http://127.0.0.1:8787/api/events
```

```
: connected

: ping

data: {"type":"attack","data":{"id":"00000000-0000-4000-8000-000000000000"}}

```

Headers: `Content-Type: text/event-stream`, `Cache-Control: no-cache`, `X-Accel-Buffering: no`, `Connection: keep-alive`. The server writes `: connected` on open and `: ping` every 15 s. Only the default event is used; each message is `data: {"type": …, "data": …}`.

| `type` | `data` | Sent when |
|---|---|---|
| `payment` | A full payment object (same shape as `/api/payments`, `prevSignature` looked up in the database) | A non-seed row is inserted into `payments` |
| `attestation` | `{ "id": "<subject pubkey>" }` | An `attestations` row is inserted or updated |
| `attack` | `{ "id": "<attempt uuid>" }` | An `attack_attempts` row is inserted or updated |
| `status` | `{}` | Database, ingest, gap or badge-listener state changed, or the server's `LISTEN` session reconnected. Refetch everything |

Semantics:

- The first three originate in Postgres: an insert/update trigger calls `pg_notify('feed', …)` and the server's `LISTEN` session fans it out. Seed rows never notify. The example above is a manual `SELECT pg_notify('feed', '{"type":"attack","id":"…"}')`.
- There is no replay and no event id. Notifications sent while the `LISTEN` session was down are lost. When the session reconnects, the server sends one `status` frame; the web client answers a `status` frame by refetching every mounted resource.
- A NOTIFY payload that is not valid JSON is logged (`[db] notify handler: …`) and dropped. The server keeps running (checked with `SELECT pg_notify('feed', 'not json')`).
- An attempt produces one `attack` frame when it is built, one when the badge listener first delivers it, and one when its outcome changes. Later polls of `/badge/pending` produce none (checked: three polls, one frame).
- Through the Vite proxy the stream ends when the server stops: a proxied `curl -N` exited the moment the server was killed. The browser's `EventSource` then reconnects; see [ARCHITECTURE.md](ARCHITECTURE.md#stream-recovery).

## Badge listener

A second HTTP server, started only when `BADGE_LISTEN_HOST` is set, on `BADGE_LISTEN_PORT` (default 8788). It serves two routes and nothing else. It is the only thing reachable from the hotspot.

It is unauthenticated: no Host, Origin or Content-Type checks, and no shared secret. Anyone on the same network who knows a badge's public key can fetch its pending attempt or mark an attempt rejected (with the attempt id). This is the `BADGE-GAP(attack-delivery)` stub, a known limit that is not fixed. Leave `BADGE_LISTEN_HOST` empty when no badge needs it.

Boot log line when it opens:

```
[http] badge listener OPEN on 0.0.0.0:8798, badges poll http://<LAN IP>:8798/badge/pending (reachable from the LAN)
```

Captured on a second instance started with `PORT=8797 BADGE_LISTEN_HOST=127.0.0.1 BADGE_LISTEN_PORT=8798`:

### GET /badge/pending?badge=\<pubkey\>

Returns the newest pending attempt for that badge created in the last 90 s. The first poll that returns an attempt sets its `deliveredAt`; later polls return the same body and change nothing.

```bash
curl -s -i 'http://127.0.0.1:8798/badge/pending?badge=4vJDQvQxRrfLYpjFWXFsVTMsw7cayvWB46pnzdn2BW97'
# 204 (nothing pending)

curl -s 'http://127.0.0.1:8798/badge/pending'
# 400 {"error":{"code":"invalid_pubkey","message":"pubkey must be a base58 32-byte address"}}

curl -s 'http://127.0.0.1:8798/badge/pending?badge=11111111111111111111111111111111'
# 404 {"error":{"code":"unknown_badge","message":"not a configured badge"}}
```

200 body, captured from a hand-inserted attempt whose transaction fields were dummies (`AA==`, 1 byte), deleted afterwards:

```json
{ "id": "036b69a1-f7e2-4aa5-a6b6-1b2979c3e34a",
  "claimed": { "merchant": "MHacks Merch", "amount": 5, "symbol": "HACK" },
  "txBase64": "AA==", "messageBase64": "AA==",
  "txVersion": "legacy", "txBytes": 1 }
```

`claimed.merchant` is fixed to `"MHacks Merch"`. `claimed.amount` is the attempt's `displayAmount`. In a real attempt `txBase64` is the unsigned wire transaction and `messageBase64` the message bytes to sign. A real attempt has not been served yet: building one needs the mint.

### POST /badge/outcome

`{ "id": "<uuid>", "outcome": "rejected" }` returns `{"ok":true}`. Same rules and errors as `/api/attacks/outcome`, except that no `Content-Type` is required. The 16 KB body limit applies.

```bash
curl -s -X POST -d '{"id":"0b7e2f44-91c3-4d1a-8e57-6a3c9f2d5b18","outcome":"rejected"}' http://127.0.0.1:8798/badge/outcome
# 404 {"error":{"code":"not_found","message":"no such attempt"}}
```

With the id of the hand-inserted pending attempt the same call returned `200 {"ok":true}`, and a second call returned 409 `conflict`.

Anything else on this port is 404 `not_found`.
