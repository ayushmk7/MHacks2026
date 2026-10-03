-- BadgePay schema. Idempotent: safe to run repeatedly.
-- Tiger Data features used: hypertables, columnstore (segmentby/orderby + policy),
-- continuous aggregates with real-time aggregation + refresh policies, time_bucket,
-- relational tables joined to hypertables, LISTEN/NOTIFY driven realtime.
-- Authoring rules (scripts/db.mjs splits on semicolons outside dollar quotes): no semicolon inside
-- string literals or comments on code lines, no trailing comments on code lines, bare $$ quoting only.

CREATE EXTENSION IF NOT EXISTS timescaledb;

-- Relational: the four badges. Mirrors server/config/badges.json (file is the source of truth).
-- BADGE-GAP(badge-pubkeys, key-location): pubkey is NULL and key_location is 'unknown' until flashed.
CREATE TABLE IF NOT EXISTS badges (
  id            smallint PRIMARY KEY,
  label         text NOT NULL,
  pubkey        text UNIQUE,
  token_account text,
  key_location  text NOT NULL DEFAULT 'unknown' CHECK (key_location IN ('se050', 'software', 'unknown')),
  stand_in      boolean NOT NULL DEFAULT false,
  updated_at    timestamptz NOT NULL DEFAULT now()
);

-- Relational: mirror of SAS attestations (chain is the source of truth, this is for joins).
CREATE TABLE IF NOT EXISTS attestations (
  subject         text PRIMARY KEY,
  name            text NOT NULL,
  status          text NOT NULL CHECK (status IN ('verified', 'revoked')),
  attestation_pda text NOT NULL,
  issued_sig      text,
  issued_at       timestamptz,
  revoked_sig     text,
  revoked_at      timestamptz,
  expires_at      timestamptz,
  updated_at      timestamptz NOT NULL DEFAULT now()
);

-- Time-series: one row per HACK transferChecked. One row = one block in the UI chain.
-- Hypertable unique keys must include the partition column, hence (signature, block_time).
CREATE TABLE IF NOT EXISTS payments (
  block_time          timestamptz NOT NULL,
  signature           text        NOT NULL,
  slot                bigint      NOT NULL,
  payer               text        NOT NULL,
  payee               text        NOT NULL,
  payer_token_account text        NOT NULL,
  payee_token_account text        NOT NULL,
  mint                text        NOT NULL,
  amount_raw          bigint      NOT NULL CHECK (amount_raw >= 0),
  decimals            smallint    NOT NULL,
  payee_status        text        NOT NULL CHECK (payee_status IN ('verified', 'unverified', 'revoked')),
  payee_name          text,
  source              text        NOT NULL DEFAULT 'chain' CHECK (source IN ('chain', 'seed')),
  attack_id           uuid,
  ingest_lag_ms       integer,
  ingested_at         timestamptz NOT NULL DEFAULT now(),
  UNIQUE (signature, block_time)
) WITH (
  tsdb.hypertable,
  tsdb.partition_column = 'block_time',
  tsdb.chunk_interval   = '1 hour',
  tsdb.segmentby        = 'payee',
  tsdb.orderby          = 'block_time DESC'
);

-- Columnstore policy: CREATE TABLE auto-adds one that runs daily. Replace it with a 10 minute schedule.
CALL remove_columnstore_policy('payments', if_exists => true);
CALL add_columnstore_policy('payments', after => INTERVAL '2 hours', schedule_interval => INTERVAL '10 minutes');

-- Time-series: attack console attempts. Tiny and updated in place, so rowstore only.
CREATE TABLE IF NOT EXISTS attack_attempts (
  created_at              timestamptz NOT NULL DEFAULT now(),
  id                      uuid        NOT NULL DEFAULT gen_random_uuid(),
  victim                  text        NOT NULL,
  attacker                text        NOT NULL,
  display_amount_raw      bigint      NOT NULL,
  actual_amount_raw       bigint      NOT NULL,
  decimals                smallint    NOT NULL,
  mint                    text        NOT NULL,
  source_token_account    text        NOT NULL,
  dest_token_account      text        NOT NULL,
  tx_base64               text        NOT NULL,
  message_base64          text        NOT NULL,
  tx_bytes                integer     NOT NULL,
  tx_version              text        NOT NULL,
  blockhash               text        NOT NULL,
  last_valid_block_height bigint      NOT NULL,
  outcome                 text        NOT NULL DEFAULT 'pending' CHECK (outcome IN ('pending', 'signed', 'rejected')),
  signature               text,
  delivered_at            timestamptz,
  resolved_at             timestamptz,
  PRIMARY KEY (id, created_at)
) WITH (
  tsdb.hypertable,
  tsdb.partition_column = 'created_at',
  tsdb.chunk_interval   = '1 day',
  tsdb.columnstore      = false
);

-- Continuous aggregate 1: per-minute counts and volume for the feed chart and stat tiles.
-- materialized_only = false turns on real-time aggregation: the newest minute is computed
-- from raw rows at query time, so the chart never lags the feed.
CREATE MATERIALIZED VIEW IF NOT EXISTS payments_1m
WITH (timescaledb.continuous, timescaledb.materialized_only = false) AS
SELECT time_bucket(INTERVAL '1 minute', block_time)        AS bucket,
       source,
       count(*)                                            AS n,
       count(*) FILTER (WHERE payee_status = 'verified')   AS verified_n,
       sum(amount_raw)                                     AS volume_raw
FROM payments
GROUP BY bucket, source
WITH NO DATA;

ALTER MATERIALIZED VIEW payments_1m SET (timescaledb.materialized_only = false);

SELECT add_continuous_aggregate_policy('payments_1m',
  start_offset      => INTERVAL '1 day',
  end_offset        => INTERVAL '1 minute',
  schedule_interval => INTERVAL '1 minute',
  if_not_exists     => true);

-- Continuous aggregate 2: per-payee hourly totals, joined to the relational badges table.
CREATE MATERIALIZED VIEW IF NOT EXISTS payee_volume_1h
WITH (timescaledb.continuous, timescaledb.materialized_only = false) AS
SELECT time_bucket(INTERVAL '1 hour', block_time) AS bucket,
       payee,
       source,
       count(*)        AS n,
       sum(amount_raw) AS volume_raw
FROM payments
GROUP BY bucket, payee, source
WITH NO DATA;

ALTER MATERIALIZED VIEW payee_volume_1h SET (timescaledb.materialized_only = false);

SELECT add_continuous_aggregate_policy('payee_volume_1h',
  start_offset      => INTERVAL '3 days',
  end_offset        => INTERVAL '1 hour',
  schedule_interval => INTERVAL '5 minutes',
  if_not_exists     => true);

-- Realtime: the database drives the browser. Row change -> NOTIFY -> server LISTEN -> SSE.
-- Payload is only {type, id}. The server re-reads the row so SSE and REST shapes are identical.
CREATE OR REPLACE FUNCTION notify_feed() RETURNS trigger LANGUAGE plpgsql AS $$
BEGIN
  PERFORM pg_notify('feed', json_build_object('type', TG_ARGV[0], 'id', to_jsonb(NEW) ->> TG_ARGV[1])::text);
  RETURN NULL;
END
$$;

DROP TRIGGER IF EXISTS payments_notify ON payments;
CREATE TRIGGER payments_notify AFTER INSERT ON payments
  FOR EACH ROW WHEN (NEW.source <> 'seed') EXECUTE FUNCTION notify_feed('payment', 'signature');

DROP TRIGGER IF EXISTS attack_attempts_notify ON attack_attempts;
CREATE TRIGGER attack_attempts_notify AFTER INSERT OR UPDATE ON attack_attempts
  FOR EACH ROW EXECUTE FUNCTION notify_feed('attack', 'id');

DROP TRIGGER IF EXISTS attestations_notify ON attestations;
CREATE TRIGGER attestations_notify AFTER INSERT OR UPDATE ON attestations
  FOR EACH ROW EXECUTE FUNCTION notify_feed('attestation', 'subject');
