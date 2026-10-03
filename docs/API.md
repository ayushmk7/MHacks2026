# API

Everything here is implemented in `server/src/http.js`. Response examples were captured from the running server on 2026-10-03 (database seeded, `HACK_MINT` empty, authority unfunded). Examples for the write routes that need devnet SOL are marked "from code" because they have not run for real yet (see [RUNBOOK.md](RUNBOOK.md)).

- Base URL: `http://127.0.0.1:8787` (`HOST`, `PORT` in `.env`). The browser reaches it same-origin through the Vite proxy at `http://127.0.0.1:5173/api`.
- All bodies are JSON. Responses carry `Cache-Control: no-store`. There are no CORS headers.
- Amounts: `amount` is a UI number (raw / 10^decimals). `amountRaw` is the raw integer as a string.

Start a server to try the examples:

```bash
npm run db:up
node --env-file-if-exists=.env server/src/index.js
```

## Routes

| Method | Path | Purpose |
|---|---|---|
| GET | `/api/status` | Health, config, and the `gaps` list that drives every degraded UI state |
| GET | `/api/payments` | Payment feed, newest first |
| GET | `/api/stats` | 24 h totals, ingest lag, 60 one-minute buckets |
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

Applied in this order on the main listener:

1. `Host` hostname must be `localhost` or `127.0.0.1`, else 403 `forbidden_origin` ("Host not allowed"). Blocks DNS rebinding.
2. Unknown method + path: 404 `not_found`.
3. POST only:
   1. An `Origin` header, if present, must be in `WEB_ORIGINS`, else 403 `forbidden_origin` ("Origin not allowed").
   2. `Content-Type` must start with `application/json`, else 415.
   3. Body at most 16 KB, else 413.
   4. Body must parse as a JSON object, else 400 `bad_json`.

Field rules:

| Field | Rule |
|---|---|
| `pubkey`, `victim` | base58 32-byte address (`isAddress`) |
| `name` | trimmed, 1 to 32 printable ASCII characters (`/^[\x20-\x7E]+$/`) |
| `displayAmount`, `actualAmount` | JSON number, `> 0`, `<= 1000000`, at most `decimals` decimal places |
| `limit` | integer 1 to 200 |
| `before` | anything `Date.parse` accepts |
| `id` | UUID |

## Errors

Every non-2xx response has this shape. `gap` appears only on `not_configured` and sits inside `error`:

```json
{"error":{"code":"not_configured","message":"HACK mint is not configured or not found on chain","gap":"hack_mint_missing"}}
```

| Code | HTTP | When |
|---|---|---|
| `bad_json` | 400 | Body is not valid JSON, or not a JSON object |
| `invalid_pubkey` | 400 | `pubkey` / `victim` / `badge` is not an address |
| `invalid_name` | 400 | `name` fails the rule above |
| `invalid_amount` | 400 | Amount fails the rule above |
| `invalid_param` | 400 | Bad `limit`, `before`, `id`, or `outcome` other than `"rejected"` |
| `forbidden_origin` | 403 | Bad `Host` or bad `Origin` |
| `unknown_badge` | 404 | `victim` / `badge` is not a pubkey in `server/config/badges.json` |
| `not_found` | 404 | No such route, or no such attack attempt |
| `conflict` | 409 | Attempt is not pending; or revoke with no on-chain attestation |
| `too_large` | 413 | Body over 16 KB |
| `unsupported_media_type` | 415 | POST without `Content-Type: application/json` |
| `internal` | 500 | Unexpected error. Message is always `internal error`; details go to the server log |
| `chain_error` | 502 | A Solana RPC call or transaction failed. Message is the RPC error text |
| `not_configured` | 503 | `error.gap` is `authority_unfunded` or `hack_mint_missing` |
| `db_error` | 503 | A database query failed |

Captured:

```bash
curl -s -H 'Host: evil.com' http://127.0.0.1:8787/api/status
# 403 {"error":{"code":"forbidden_origin","message":"Host not allowed"}}

curl -s -X POST -H 'Content-Type: text/plain' -d '{}' http://127.0.0.1:8787/api/attestations
# 415 {"error":{"code":"unsupported_media_type","message":"Content-Type must be application/json"}}

curl -s -X POST -H 'Origin: http://evil.example' -H 'Content-Type: application/json' -d '{}' http://127.0.0.1:8787/api/attestations
# 403 {"error":{"code":"forbidden_origin","message":"Origin not allowed"}}

curl -s 'http://127.0.0.1:8787/api/payments?limit=0'
# 400 {"error":{"code":"invalid_param","message":"limit must be an integer from 1 to 200"}}
```

## GET /api/status

Always 200 while the process is up, including when the database or RPC is down.

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

(`gaps` is trimmed to one entry here. The same call returned six.)

- `ingest.state`: `idle` (no mint), `backfilling`, `live`, `reconnecting`.
- `rpc.url` is the origin only, so an API key in `RPC_URL` is never echoed.
- `token.mint` is `null` when `HACK_MINT` is unset. `token.decimals` is read from the chain once the mint is found; before that it is `HACK_DECIMALS`.
- `authority.sol` is refreshed every 30 s and on every issue/revoke.
- `registry.ready` is true once both the credential and schema accounts exist on chain.
- `badgeListener.url` is the URL the badge polls, or `null` when closed.

### Gaps

`gaps` is computed by `computeGaps()` in `server/src/config.js`. Each entry has `code`, `severity` (`blocker` | `warn` | `info`), `badgeGap` (a `BADGE-GAP` id or `null`), `title`, `fix`, `pages`.

| Code | Severity | badgeGap | Condition | Pages |
|---|---|---|---|---|
| `db_offline` | blocker | | Pool cannot connect | all four |
| `db_schema_missing` | blocker | | `payments` table absent | all four |
| `rpc_unreachable` | blocker | | Last RPC call failed | all four |
| `hack_mint_missing` | blocker | `hack-mint` | `HACK_MINT` empty/invalid, or mint account not found | feed, badges, attack |
| `badges_unconfigured` | blocker | `badge-pubkeys` | No badge has a pubkey | badges, registry, attack |
| `badges_partial` | warn | `badge-pubkeys` | Some slots have no pubkey | badges, registry, attack |
| `badges_stand_in` | info | `badge-pubkeys` | Any badge has `standIn: true` | badges |
| `key_location_unknown` | warn | `key-location` | A configured badge has `keyLocation: "unknown"` | badges |
| `config_invalid` | warn | | `badges.json` has invalid entries (title lists them) | badges |
| `authority_unfunded` | warn | | Authority balance below 0.02 SOL | registry, attack |
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
    "slot": 399999999, "blockTime": "2026-10-03T04:26:18.520Z",
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
- `attackId` is set when the transfer matched a pending attack attempt (the tampered transfer was signed).
- `lagMs` is block time to database insert. `null` for seed rows and for rows picked up by the backfill poll.
- `prevSignature` is the next-older row in this page, `null` on the page's last row.
- Paging caveat: `before` is a strict timestamp cursor. Rows that share the exact block time of the page's last row are skipped.

## GET /api/stats

```bash
curl -s http://127.0.0.1:8787/api/stats
```

```json
{ "window": "24h", "payments": 17128, "volume": 179840, "verifiedShare": 0.7, "seedRows": 17128,
  "lagMs": { "p50": null, "p95": null },
  "series": [ { "t": "2026-10-03T03:38:00.000Z", "n": 0, "verifiedN": 0, "volume": 0 } ],
  "queryMs": 7.8 }
```

- Totals come from the `payments_1m` continuous aggregate over the last 24 hours. They include seed rows; `seedRows` says how many.
- `verifiedShare` is rounded to 3 places and is `0` when there are no payments.
- `lagMs` is the p50/p95 of `ingest_lag_ms` over chain rows in the last hour. Both are `null` when there are none.
- `series` has 61 one-minute buckets, oldest first, zero-filled.
- `queryMs` is how long the series query took.

## GET /api/stats/db

```bash
curl -s http://127.0.0.1:8787/api/stats/db
```

```json
{ "timescaledb": "2.30.2",
  "payments": { "rows": 51840, "chunks": 73, "columnstoreChunks": 70,
                "bytesBefore": 29343744, "bytesAfter": 9732096, "compressionRatio": 0.668, "totalBytes": 10133504 },
  "caggs": [
    { "name": "payments_1m", "realtime": true, "scheduleSec": 60, "lastRefresh": "2026-10-03T04:38:23.740Z", "lastStatus": "Success" },
    { "name": "payee_volume_1h", "realtime": true, "scheduleSec": 300, "lastRefresh": "2026-10-03T04:36:23.695Z", "lastStatus": "Success" }
  ],
  "queryMs": { "feed": 16.8, "series": 1.4 } }
```

- `rows` is an exact `count(*)`.
- `compressionRatio = 1 - bytesAfter / bytesBefore`, rounded to 3 places. `null` when no chunk has been converted.
- `lastRefresh` is `null` for a refresh job that has never finished.
- `queryMs`: this route re-runs the feed query (limit 40) and the series query one after the other and reports each one's time in ms.

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
- `sol` and `hack` come from one `getMultipleAccounts` call, cached 5 s. Both are `null` if the RPC call fails. `hack` is `null` while the mint is missing.
- `tokenAccount` is the value from `badges.json`, else the derived associated token account, else `null` (no mint).
- `attestation.status`: `verified` | `unverified` | `revoked`.
- `received` is the all-time total of chain rows from the `payee_volume_1h` continuous aggregate. Seed rows are excluded (`source = 'chain'`).
- A slot with `"pubkey": null` is returned with `sol`, `hack`, `tokenAccount` all `null`.

## GET /api/attestations

```bash
curl -s http://127.0.0.1:8787/api/attestations
# {"attestations":[]}
```

One entry per subject, most recently updated first (shape from code, `toAttestation` in `server/src/db.js`):

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

Errors: `invalid_pubkey`, `invalid_name`, `not_configured` (`gap: authority_unfunded`, authority below 0.02 SOL), `chain_error`.

Validation errors, captured:

```bash
curl -s -X POST -H 'Content-Type: application/json' \
  -d '{"pubkey":"nope","name":"MHacks Merch"}' http://127.0.0.1:8787/api/attestations
# 400 {"error":{"code":"invalid_pubkey","message":"pubkey must be a base58 32-byte address"}}

curl -s -X POST -H 'Content-Type: application/json' \
  -d '{"pubkey":"11111111111111111111111111111111","name":"café"}' http://127.0.0.1:8787/api/attestations
# 400 {"error":{"code":"invalid_name","message":"name must be 1 to 32 printable ASCII characters"}}
```

## POST /api/attestations/revoke

Closes the attestation account. `{ "pubkey": "<base58>" }`. Response 200, same shape as issue, with `status: "revoked"` and `signature` = the closing transaction.

Check order: `invalid_pubkey`, then authority funding, then existence. So an unfunded authority returns 503 even when there is nothing to revoke:

```bash
curl -s -X POST -H 'Content-Type: application/json' \
  -d '{"pubkey":"11111111111111111111111111111111"}' http://127.0.0.1:8787/api/attestations/revoke
# 503 {"error":{"code":"not_configured","message":"registry authority FZEAS6Nayu6nMX1RVmoNwoPA4tu1KNM5pvfoXsQzei3K has 0 SOL, fund it at https://faucet.solana.com","gap":"authority_unfunded"}}
```

With a funded authority and no on-chain attestation: 409 `conflict`.

## GET /api/attacks

Query: `limit` (1 to 200, default 20). Newest first.

```bash
curl -s 'http://127.0.0.1:8787/api/attacks?limit=5'
# {"attempts":[]}
```

Attempt shape (from code, `toAttack` in `server/src/db.js`):

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

- `outcome`: `pending` | `signed` | `rejected` | `expired`. `expired` is derived (pending and older than 90 s), never stored.
- `expiresAt` is an estimate of the blockhash lifetime (`createdAt` + 90 s). It is not the same clock as `expired`.
- `txBytes` is the wire size of `txBase64` (signature slots + message), not the message size.
- `txVersion`: `legacy` or `0`, from `ATTACK_TX_VERSION`.
- `signature` is set when `outcome` is `signed`: the ingest saw a transfer on chain with the same victim, attacker and raw amount.
- `deliveredAt` is set the first time the badge listener hands the attempt to a badge.
- `warnings` is only populated in the POST response below. In this list it is always `[]`.

## POST /api/attacks

Builds an ordinary, unsigned SPL `transferChecked` from the victim's token account to the attacker's, for `actualAmount`, and stores it with the `displayAmount` the fake checkout claims. The server never signs or submits it.

```json
{ "victim": "<pubkey of a configured badge>", "displayAmount": 5, "actualAmount": 500 }
```

Amounts are optional (defaults 5 and 500). Response 201: one attempt object.

Check order: `invalid_pubkey`, `unknown_badge`, `invalid_amount`, `not_configured` (`gap: hack_mint_missing`), then the build (`chain_error`).

- Attacker = `ATTACKER_PUBKEY`, or the authority address when empty.
- The server tries to create the attacker's token account first. If that fails (for example the authority has no SOL), the attempt is still built and `warnings` gets an entry.
- `warnings` also gets an entry when the victim holds less HACK than `actualAmount`.

Captured:

```bash
curl -s -X POST -H 'Content-Type: application/json' \
  -d '{"victim":"11111111111111111111111111111111"}' http://127.0.0.1:8787/api/attacks
# 404 {"error":{"code":"unknown_badge","message":"victim must be the pubkey of a configured badge"}}

curl -s -X POST -H 'Content-Type: application/json' \
  -d '{"victim":"4vJDQvQxRrfLYpjFWXFsVTMsw7cayvWB46pnzdn2BW97","actualAmount":0.001}' http://127.0.0.1:8787/api/attacks
# 400 {"error":{"code":"invalid_amount","message":"amount must be a number > 0 and <= 1000000 with at most 2 decimal places"}}

curl -s -X POST -H 'Content-Type: application/json' \
  -d '{"victim":"4vJDQvQxRrfLYpjFWXFsVTMsw7cayvWB46pnzdn2BW97"}' http://127.0.0.1:8787/api/attacks
# 503 {"error":{"code":"not_configured","message":"HACK mint is not configured or not found on chain","gap":"hack_mint_missing"}}
```

## POST /api/attacks/outcome

```json
{ "id": "<uuid>", "outcome": "rejected" }
```

Response 200: the attempt. Only `rejected` is accepted, and only for a `pending` attempt. `signed` is never accepted from a client.

```bash
curl -s -X POST -H 'Content-Type: application/json' \
  -d '{"id":"0b7e2f44-91c3-4d1a-8e57-6a3c9f2d5b18","outcome":"signed"}' http://127.0.0.1:8787/api/attacks/outcome
# 400 {"error":{"code":"invalid_param","message":"outcome must be \"rejected\""}}

curl -s -X POST -H 'Content-Type: application/json' \
  -d '{"id":"0b7e2f44-91c3-4d1a-8e57-6a3c9f2d5b18","outcome":"rejected"}' http://127.0.0.1:8787/api/attacks/outcome
# 404 {"error":{"code":"not_found","message":"no such attempt"}}
```

An attempt that is already `signed` or `rejected` returns 409 `conflict`. An attempt shown as `expired` is still stored as `pending`, so it can be rejected.

## GET /api/events (SSE)

```bash
curl -s -N http://127.0.0.1:8787/api/events
```

```
: connected

data: {"type":"attack","data":{"id":"00000000-0000-4000-8000-000000000000"}}

```

Headers: `Content-Type: text/event-stream`, `Cache-Control: no-cache`, `X-Accel-Buffering: no`, `Connection: keep-alive`. The server writes `: connected` on open and `: ping` every 15 s. Only the default event is used; each message is `data: {"type": …, "data": …}`.

| `type` | `data` | Sent when |
|---|---|---|
| `payment` | A full payment object (same shape as `/api/payments`, `prevSignature` looked up in the database) | A non-seed row is inserted into `payments` |
| `attestation` | `{ "id": "<subject pubkey>" }` | An `attestations` row is inserted or updated |
| `attack` | `{ "id": "<attempt uuid>" }` | An `attack_attempts` row is inserted or updated |
| `status` | `{}` | Database, ingest, gap or badge-listener state changed. Refetch `/api/status` |

The first three originate in Postgres: an insert/update trigger calls `pg_notify('feed', …)` and the server's `LISTEN` client fans it out. Seed rows never notify.

## Badge listener

A second HTTP server, started only when `BADGE_LISTEN_HOST` is set, on `BADGE_LISTEN_PORT` (default 8788). It serves two routes and nothing else. It has no Host, Origin or Content-Type checks, because the caller is badge firmware, not a browser. It is the only thing reachable from the hotspot.

Captured on a second instance started with `PORT=8797 BADGE_LISTEN_HOST=127.0.0.1 BADGE_LISTEN_PORT=8798`:

### GET /badge/pending?badge=\<pubkey\>

Returns the newest pending attempt for that badge created in the last 90 s and marks it delivered.

```bash
curl -s -i 'http://127.0.0.1:8798/badge/pending?badge=4vJDQvQxRrfLYpjFWXFsVTMsw7cayvWB46pnzdn2BW97'
# 204 (nothing pending)

curl -s 'http://127.0.0.1:8798/badge/pending'
# 400 {"error":{"code":"invalid_pubkey","message":"pubkey must be a base58 32-byte address"}}

curl -s 'http://127.0.0.1:8798/badge/pending?badge=11111111111111111111111111111111'
# 404 {"error":{"code":"unknown_badge","message":"not a configured badge"}}
```

200 body (from code):

```json
{ "id": "<uuid>",
  "claimed": { "merchant": "MHacks Merch", "amount": 5, "symbol": "HACK" },
  "txBase64": "<unsigned wire transaction>", "messageBase64": "<message bytes>",
  "txVersion": "legacy", "txBytes": 0 }
```

`claimed.merchant` is fixed to `"MHacks Merch"`. `claimed.amount` is the attempt's `displayAmount`.

### POST /badge/outcome

`{ "id": "<uuid>", "outcome": "rejected" }` returns `{ "ok": true }`. Same rules and errors as `/api/attacks/outcome`.

```bash
curl -s -X POST -d '{"id":"0b7e2f44-91c3-4d1a-8e57-6a3c9f2d5b18","outcome":"rejected"}' http://127.0.0.1:8798/badge/outcome
# 404 {"error":{"code":"not_found","message":"no such attempt"}}
```

Anything else on this port is 404 `not_found`.
