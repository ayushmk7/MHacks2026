-- Fake history for when the devnet faucet is rate-limited (PRD risk). Rows are marked source='seed',
-- carry SEED-prefixed signatures, never fire NOTIFY, and are drawn dashed in the UI.
DELETE FROM payments WHERE source = 'seed';

WITH pool AS (
  SELECT (array_agg(pk ORDER BY ord))[1:4] AS pk
  FROM (
    SELECT pubkey AS pk, id::int AS ord FROM badges WHERE pubkey IS NOT NULL
    UNION ALL
    SELECT 'SeedBadge' || i || repeat('1', 34), 100 + i FROM generate_series(1, 4) AS i
  ) s
)
INSERT INTO payments (block_time, signature, slot, payer, payee, payer_token_account, payee_token_account,
                      mint, amount_raw, decimals, payee_status, payee_name, source)
SELECT
  now() - INTERVAL '10 minutes' - (g * INTERVAL '5 seconds'),
  'SEED' || encode(sha256(convert_to(g::text, 'UTF8')), 'hex'),
  400000000 - g,
  pool.pk[2 + (g % 3)],
  CASE WHEN g % 10 < 7 THEN pool.pk[1] ELSE pool.pk[2 + ((g + 1) % 3)] END,
  'SeedTokenAcct' || (2 + (g % 3)),
  'SeedTokenAcct' || (CASE WHEN g % 10 < 7 THEN 1 ELSE 2 + ((g + 1) % 3) END),
  'SeedMint11111111111111111111111111111111111',
  (1 + (g * 7) % 20) * 100,
  2,
  CASE WHEN g % 10 < 7 THEN 'verified' WHEN g % 97 = 0 THEN 'revoked' ELSE 'unverified' END,
  CASE WHEN g % 10 < 7 THEN 'MHacks Merch' WHEN g % 97 = 0 THEN 'Old Merch' END,
  'seed'
FROM generate_series(1, 51840) AS g CROSS JOIN pool;

CALL refresh_continuous_aggregate('payments_1m', NULL, now());
CALL refresh_continuous_aggregate('payee_volume_1h', NULL, now());

DO $$
DECLARE c regclass;
BEGIN
  FOR c IN SELECT show_chunks('payments', older_than => INTERVAL '2 hours') LOOP
    CALL convert_to_columnstore(c);
  END LOOP;
END
$$;

-- Re-seeding deletes rows out of columnstore chunks, which leaves dead index and toast pages behind
-- (measured: 13 MB grew to 47 MB after two re-seeds). Rewrite the chunks so hypertable_size stays honest.
VACUUM FULL payments;
