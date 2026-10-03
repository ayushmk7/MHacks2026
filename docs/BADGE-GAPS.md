# Badge gaps

The panel was built before any badge was flashed. Every place that depends on badge hardware or firmware is marked in the code with `BADGE-GAP(<id>)`. This file is the single list of them.

Find the markers:

```bash
grep -rn "BADGE-GAP" . --exclude-dir=node_modules --exclude-dir=dist --exclude-dir=docs
```

There are seven ids. In LIVE mode the server reports the open ones in `/api/status.gaps` (field `badgeGap`), and the page shows a `BADGE-GAP(<id>)` chip next to the fix. DEMO mode always shows three of them (`key-location`, `attack-delivery`, `attack-outcome`) from fixtures in `web/src/demo.js`, so the placeholders can be seen without a server.

## The gaps

| Id | What is unknown | What to fill in | What the UI shows meanwhile |
|---|---|---|---|
| `badge-pubkeys` | Each real badge's Ed25519 public key | `server/config/badges.json`: set `badges[n].pubkey` to the base58 key from the badge (Settings > Identity) and `standIn` to `false` | Stand-in keypairs: each Badges card has a "stand-in keypair" chip, plus the info line "Stand-in software badges in use". A slot with `pubkey: null`: a dashed "Waiting for badge" card, and the warn strip "N of 4 badges not configured" on Badges, Registry and Attack. No slot configured: the blocker "No badges configured" |
| `key-location` | Whether each badge's key lives in the SE050 secure element or in software (PRD "honesty" requirement) | `server/config/badges.json`: set `badges[n].keyLocation` to `"se050"` or `"software"` | Key location chip on the Badges card: `SECURE ELEMENT` (green), `SOFTWARE` (amber) or `UNKNOWN`. A configured badge with `"unknown"` adds the warn strip "Key location unknown for a configured badge". Stand-ins are `software` |
| `token-accounts` | Whether a badge keeps HACK in its standard associated token account | `server/config/badges.json`: leave `badges[n].tokenAccount` as `null` to derive the associated token account, or set it to the base58 token account if the badge uses another one | Nothing special. Balances and the attack transaction use the derived account. A badge whose tokens sit elsewhere shows 0 HACK |
| `hack-mint` | Which SPL mint is HACK: the stand-in mint from `npm run devnet:setup`, or one the badge team mints | `.env`: `HACK_MINT=<mint address>`, then restart the server. `devnet:setup` writes it when it creates the stand-in mint. Decimals are read from the chain | Blocker card "HACK mint not configured" (or "HACK mint account not found on chain") in place of the chain on the Feed, and on Badges and Attack. Pill reads `IDLE`. HACK balances show "—". The Charge button is disabled |
| `attack-delivery` | How the tampered transaction reaches the badge. The stub is Wi-Fi HTTP polling; BLE is the PRD's alternative (a transaction does not fit in a 240-byte ESP-NOW frame) | `.env`: `BADGE_LISTEN_HOST=<laptop hotspot IP>` (and `BADGE_LISTEN_PORT`, default 8788), then restart the server. Firmware polls `GET http://<ip>:8788/badge/pending?badge=<its pubkey>` | Attack page, Delivery card: "listener closed" and the warn card "Badge delivery listener is closed". When open: "listener open" and the exact poll URL with a Copy button |
| `attack-payload` | Which transaction message version the badge decoder parses | `.env`: `ATTACK_TX_VERSION=legacy` or `ATTACK_TX_VERSION=0`, then restart the server | Attack page, under the decoded panel: "Built as a legacy message…" with the chip, next to the byte size |
| `attack-outcome` | Whether the badge reports that the holder pressed CANCEL | Firmware: `POST http://<ip>:8788/badge/outcome` with `{"id":"<attempt id>","outcome":"rejected"}`. No laptop config. `signed` is never reported by the badge; the server detects it on chain | Attack page: info line "Rejected outcomes are marked manually" and a **Mark rejected** button on each pending attempt |

Notes:

- The badge listener contract (request, response, status codes) is in [API.md](API.md#badge-listener). `/badge/pending` returns both `txBase64` (the unsigned wire transaction, signature slot zeroed) and `messageBase64` (the bytes to sign), so firmware can take either.
- `BADGE_LISTEN_HOST=0.0.0.0` also opens the listener, but the URL shown on the Attack page is built from that value. Use the real hotspot IP so the URL is usable.
- If delivery ends up on BLE, the listener is not used and a BLE sender has to be written. Nothing for BLE exists.
- The `attack_outcome_manual` info line is unconditional (`server/src/config.js:93`). Remove that entry once firmware reports outcomes.
- A badge-team mint: `devnet:setup` mints HACK with the laptop authority as mint authority. With a mint the authority does not control, its funding step fails for any badge holding less than 100 HACK (derived from the code, not run). Fund the badges from the real mint authority instead; the registry is still created by the first issue on the Registry page.

## Where the markers are

Line numbers as of 2026-10-03.

| Id | Markers |
|---|---|
| `badge-pubkeys` | `server/config/badges.json:2`, `server/src/config.js:38`, `db/schema.sql:11`, `scripts/devnet-setup.mjs:24`, `web/src/pages/Badges.jsx:19`, `web/src/pages/Badges.jsx:25`, `web/src/pages/admin.css:15` |
| `key-location` | `server/config/badges.json:2`, `server/src/config.js:38`, `db/schema.sql:11`, `scripts/devnet-setup.mjs:24`, `web/src/pages/Badges.jsx:6` |
| `token-accounts` | `server/config/badges.json:2` |
| `hack-mint` | `.env.example:11`, `server/src/config.js:14`, `server/src/index.js:56`, `scripts/devnet-setup.mjs:55`, `web/src/pages/Feed.jsx:209` |
| `attack-delivery` | `.env.example:34`, `server/src/config.js:28`, `server/src/http.js:216`, `web/src/pages/Attack.jsx:149`, `web/src/pages/Attack.jsx:160` |
| `attack-payload` | `.env.example:32`, `server/src/config.js:26`, `server/src/solana.js:133`, `web/src/pages/Attack.jsx:53`, `web/src/pages/Attack.jsx:55` |
| `attack-outcome` | `server/src/http.js:217`, `web/src/demo.js:151`, `web/src/pages/Attack.jsx:42`, `web/src/pages/Attack.jsx:166` |

One more line mentions the marker without a fixed id: `web/src/ui.jsx:114` renders the `BADGE-GAP(<id>)` chip for any gap that has one. Your local `.env` repeats the three `.env.example` markers.

## When a real badge arrives

Per badge:

1. On the badge, open Settings > Identity. Note the public key and whether the key is in the secure element or in software.
2. Edit `server/config/badges.json`, in the slot this badge replaces:

   ```json
   { "id": 3, "label": "Judge A", "pubkey": "<badge public key>", "keyLocation": "se050", "tokenAccount": null, "standIn": false }
   ```

   `keyLocation` is `"se050"` or `"software"`. Keep `id` and `label`. Public keys must be unique across slots. Restart `npm run server`; it resyncs the `badges` table at boot. A bad entry shows up as an "Invalid badges.json: …" strip on the Badges page.
3. If the slot's stand-in had an attestation (badge 1 gets "MHacks Merch" from `devnet:setup`), revoke it on the Registry page. It stays listed after the swap, as "Unknown wallet", until revoked.
4. Fund the badge: `npm run devnet:setup`. For every configured badge it sends 0.05 SOL if the badge has less than 0.02, creates the token account, and mints 1000 HACK if it holds less than 100. It never overwrites a slot that already has a public key. (This needs the stand-in mint; see the note on a badge-team mint above.)
5. Verify it: Registry page, Issue card, pick the badge, type the name, **Issue attestation**. Or re-run `npm run devnet:setup`, which issues "MHacks Merch" to badge 1 if it has no attestation.
6. Check the Badges page: the card shows the right key location chip, no "stand-in keypair" chip, a HACK balance and SOL for fees.
7. Optional: `npm run db:seed` again so the seeded history uses the new public keys. Otherwise old seed rows point at the retired stand-in key and show as an unlabelled wallet.

Once, for the attack demo:

8. Put the laptop's hotspot IP in `.env` as `BADGE_LISTEN_HOST` (`ipconfig getifaddr en0` prints it on macOS when Wi-Fi is `en0`). Restart the server. The Attack page should read "listener open".
9. Set `ATTACK_TX_VERSION` to what the badge decoder parses and restart.
10. From another device on the hotspot, check the listener:

    ```bash
    curl -i "http://<hotspot ip>:8788/badge/pending?badge=<badge public key>"
    ```

    204 means reachable and nothing pending. Build a charge on the Attack page and repeat: 200 with the transaction.

Then confirm the gaps are gone:

```bash
curl -s http://127.0.0.1:8787/api/status
```

With four real badges, the listener open, a mint set and the authority funded, `gaps` should contain only `attack_outcome_manual` (plus `key_location_unknown` if any badge was left at `"unknown"`).

What changes for scripts once a slot is a real badge: `npm run pay -- --from <id>` refuses ("its key never leaves the badge"); the badge has to send the payment itself. `--to <id>` still works.
