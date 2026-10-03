# Tiger Data usage

BadgePay stores every HACK payment in TimescaleDB (Tiger Data). The database is not only storage: an insert trigger is what pushes a new block to the browser, and the feed's stat tiles and chart are continuous aggregates queried in real time.

Tested against `timescale/timescaledb:2.30.2-pg17` in Docker (TimescaleDB 2.30.2, PostgreSQL 17.11). Tiger Cloud was not tested; see [Pointing at Tiger Cloud](#pointing-at-tiger-cloud).

Paths in this document are relative to `dashboard/` (so `db/schema.sql:38-62` is `dashboard/db/schema.sql`, lines 38 to 62). Commands run from `dashboard/`. Line numbers are as of 2026-10-03.

## Features used, and where

| Feature | Why | Where |
|---|---|---|
| Hypertable (`payments`) | One row per payment, partitioned by `block_time` in 1-hour chunks | `db/schema.sql:38-62` |
| Hypertable (`attack_attempts`) | Attack-console attempts by `created_at`, 1-day chunks, rowstore only because rows are updated in place | `db/schema.sql:69-96` |
| Columnstore with `segmentby` / `orderby` | `segmentby = payee` (4 badges, low cardinality, the per-badge filter), `orderby = block_time DESC` (the feed's sort order) | `db/schema.sql:60-61` |
| Columnstore policy | Converts chunks older than 2 hours, checks every 10 minutes (replaces the default daily policy) | `db/schema.sql:65-66` |
| Manual `convert_to_columnstore` | The seed converts its historical chunks immediately so the measured ratio is visible without waiting for the policy | `db/seed.sql:34-41` |
| Continuous aggregate `payments_1m` | Per-minute count, verified count and volume, grouped by `source`. Feeds the stat tiles and the minute chart | `db/schema.sql:101-112` |
| Continuous aggregate `payee_volume_1h` | Per-payee hourly totals, grouped by `source` so seed rows stay out of badge totals. Feeds "received" on the Badges page | `db/schema.sql:121-132` |
| Real-time aggregation | `materialized_only = false` on both, so the newest minute is computed from raw rows at query time and the chart never lags the feed | `db/schema.sql:102`, `112`, `122`, `132` |
| Refresh policies | `payments_1m` every 1 minute (window 1 day to 1 minute ago); `payee_volume_1h` every 5 minutes (3 days to 1 hour ago) | `db/schema.sql:114-118`, `134-138` |
| `time_bucket` | Bucketing in both aggregates | `db/schema.sql:103`, `123` |
| `time_bucket_gapfill` | 61 zero-filled one-minute buckets for the chart, straight from the aggregate. The lower bound is bucket-aligned so the first bucket is a real count | `server/src/db.js:58-65` |
| `hypertable_columnstore_stats`, `hypertable_size` | Measured compression for the on-screen card | `server/src/db.js:145-147` |
| `timescaledb_information.continuous_aggregates` / `jobs` / `job_stats` | Real-time flag, schedule, last refresh and last run status of each aggregate | `server/src/db.js:148-156` |
| Relational tables joined to the hypertable | `badges` joined twice to `payments` (payer and payee labels) | `server/src/db.js:4-10` |
| Relational + aggregate in one query | `badges` + `attestations` + `payee_volume_1h` (chain rows only) | `server/src/db.js:75-84` |
| Verification snapshot in SQL | The insert joins `attestations` so each payment records the payee's status at that moment | `server/src/db.js:27-34` |
| Ordered-set aggregate on the hypertable | p50/p95 chain-to-database lag (`percentile_cont`) over the last hour | `server/src/db.js:68-72` |
| `LISTEN` / `NOTIFY` realtime | Triggers on `payments`, `attack_attempts`, `attestations` call `pg_notify('feed', …)`; the server listens and forwards to the browser as SSE | `db/schema.sql:142-159`, `server/src/db.js:199-218`, `server/src/index.js:71-75` |

Plain relational tables: `badges` (`db/schema.sql:12-20`) mirrors `server/config/badges.json`; `attestations` (`db/schema.sql:23-34`) mirrors the on-chain registry.

## How realtime works

```
INSERT INTO payments            (server/src/ingest.js:31, one row per confirmed transfer)
  -> trigger payments_notify    (db/schema.sql:150-151, skipped for source = 'seed')
  -> pg_notify('feed', {type, id})
  -> LISTEN feed                (server/src/db.js:199-218, a dedicated session)
  -> re-read the row            (server/src/index.js:71-75, so SSE and REST shapes are identical)
  -> SSE /api/events            (server/src/http.js:44-54)
  -> new block in the browser
```

The server does not poll the database for changes. The NOTIFY payload is only `{type, id}`; the server re-reads the row with the same query the REST route uses.

The `LISTEN` session is kept honest (`server/src/db.js:199-218`):

- It has a 3 s connect timeout and a 5 s query timeout.
- An idle `LISTEN` session never writes, so a silently dead connection would look connected. A `SELECT 1` heartbeat every 10 s catches that.
- On error, end or a failed heartbeat it reconnects after 2 s. Notifications sent in between are lost, so after a reconnect the server sends an SSE `status` frame and clients refetch.
- A payload that is not valid JSON is logged and dropped. Checked with `SELECT pg_notify('feed', 'not json')`: the server logged `[db] notify handler: …` and kept serving.

The query pool (`server/src/db.js:170`) has a 5 s `query_timeout`: a query on a dead session fails as `db_error` instead of hanging and using up the pool.

## On screen and over HTTP

- `GET /api/stats/db` (`server/src/http.js:142-162`) returns the TimescaleDB version, hypertable rows, chunk counts, before/after bytes, the computed ratio, each aggregate's real-time flag, last refresh and last run status, and the timing of the feed and series queries.
- `GET /api/stats` (`server/src/http.js:129-140`) returns the 24 h totals and the gapfilled series from `payments_1m`.
- `GET /api/badges` reads "received" from `payee_volume_1h` with `source = 'chain'` (`server/src/db.js:82`).
- The Feed page renders a card labelled "Powered by Tiger Data" from `/api/stats/db` (`TigerCard`, `web/src/pages/Feed.jsx:129`). It shows the measured ratio, before and after sizes, rows, chunks in columnstore, query times, and both aggregates. In DEMO mode the card shows fixture numbers (87.9%), not measurements; switch to LIVE for real ones.

## Measured numbers (2026-10-03, local Docker)

Re-read on 2026-10-03 at 18:10 UTC from the existing seeded database (51,840 seed rows: three days at one payment every 5 seconds; no chain rows). The database was not reset or re-seeded for this measurement.

| Metric | Value |
|---|---|
| Rows in `payments` | 51,840 |
| Chunks | 74 (72 in columnstore) |
| Size before columnstore | 30,023,680 bytes (table 19,210,240, index 10,223,616, toast 589,824) |
| Size after columnstore | 9,961,472 bytes (table 1,179,648, index 1,179,648, toast 7,602,176) |
| Compression ratio | **0.668** (66.8% smaller) |
| `hypertable_size('payments')` | 9,895,936 bytes |
| `payments_1m` rows | 4,321 |
| `payee_volume_1h` rows | 292 |
| Feed query (40 newest rows, two joins) | 12.4 to 20.3 ms over 10 warm calls; 50 to 57 ms on 4 calls that used a fresh database session |
| Series query (aggregate + gapfill) | 2.4 to 6.8 ms over 14 calls |

These numbers move while the columnstore policy works through the newest chunks. Six minutes earlier the same route reported 71 of 74 chunks, 29,638,656 to 9,822,208 bytes, ratio 0.669 (that capture is the example in [API.md](API.md#get-apistatsdb)). Of the two chunks still in rowstore at 18:10, one holds the newest hour of seed rows (16:00 to 17:00 UTC) and one holds no rows (17:00 to 18:00 UTC).

Other processes were running on the laptop during this measurement. The previous version of this document reported 4.4 to 16.8 ms (feed) and 0.9 to 1.4 ms (series) over six calls.

Reproduce:

```bash
curl -s http://127.0.0.1:8787/api/stats/db
npm run db:psql -- -c "SELECT * FROM hypertable_columnstore_stats('payments')"
```

### Why 67% and not 90%

The ratio is measured, not targeted. Each row carries a transaction signature, and signatures do not compress: they are unique, high-entropy strings (a base58 Ed25519 signature on chain, a SHA-256 hex digest in the seed).

Measured on one columnstore chunk (`_hyper_1_8_chunk`, 720 rows, 4 batches, one per payee):

| | Raw | Compressed |
|---|---|---|
| `signature` column | 51,837 bytes | 45,873 bytes (11.5% smaller) |
| Every other data column together | about 188,000 bytes | 6,587 bytes (96.5% smaller) |

The signature is 87% of the compressed column data. The low-cardinality columns that `segmentby = payee` and `orderby = block_time DESC` were chosen for compress as expected. Chunks are also small (720 rows, 417,792 bytes before, 139,264 after), so the fixed 16 kB table + 16 kB index per chunk is a visible share of the "after" size.

Query used (chunk names differ per install; list them with `SELECT chunk_name FROM chunk_columnstore_stats('payments')`, then append `_compressed`):

```sql
SELECT count(*) AS batches, sum(_ts_meta_count) AS rows,
       sum(pg_column_size(signature)) AS signature,
       sum(pg_column_size(block_time)) AS block_time,
       sum(pg_column_size(payer)) AS payer
FROM _timescaledb_internal._hyper_1_8_chunk_compressed;
```

The 6,587 figure sums `pg_column_size` the same way over every other data column (not the `_ts_meta_*` columns). The raw figures are `sum(pg_column_size(signature))` and `sum(pg_column_size(p.*))` over the chunk itself (`_timescaledb_internal._hyper_1_8_chunk p`): 51,837 and 240,208 bytes.

Real chain data should land in the same range, since real signatures are just as incompressible. That has not been measured: no chain rows of our own mint exist yet.

## Known limit: `lastStatus` after a seed

`/api/stats/db` reports each aggregate's last refresh-job status. `payments_1m` can show `"Failed"` for a few minutes right after `npm run db:seed`, until its next scheduled refresh succeeds. This was reported by the debugging pass on 2026-10-03 and is deliberately not fixed. It was not reproduced for this document, because that needs a re-seed. Both aggregates reported `"Success"` in today's captures.

## Pointing at Tiger Cloud

Not tested. The steps below are what the code expects.

1. Create a Tiger Cloud service and copy its connection string.
2. Use the **direct** connection, not the pooler. `LISTEN`/`NOTIFY` needs a session that stays open; a transaction pooler drops the subscription.
3. Put it in `dashboard/.env`:

   ```bash
   DATABASE_URL=postgres://tsdbadmin:<password>@<host>.tsdb.cloud.timescale.com:<port>/tsdb?sslmode=require
   ```

4. Create the schema and (optionally) the seed, then restart the server:

   ```bash
   npm run db:migrate
   npm run db:seed
   npm run server
   ```

Known unknowns:

- TLS: `pg` with `sslmode=require` against Tiger Cloud was never run. If certificate verification fails, try `sslmode=no-verify`.
- The schema uses `CREATE TABLE … WITH (tsdb.hypertable, …)` and `add_columnstore_policy`, which need a recent TimescaleDB. It was only run on 2.30.2.
- The 5 s query timeout and the 3 s connect timeout were chosen for a local database. They were not checked against a remote one.
- `npm run db:psql` only talks to the local Docker container. Use `psql "$DATABASE_URL"` for a cloud service.
- `db/seed.sql` ends with `VACUUM FULL payments`, which takes an exclusive lock for a few seconds.
