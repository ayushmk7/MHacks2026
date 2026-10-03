# Product and build specifications

| File | Owner | Part | What it covers |
| --- | --- | --- | --- |
| [00-Interfaces.md](00-Interfaces.md) | All | — | Decisions, shared contracts: Lua wallet API, signing prefixes, ESP-NOW messages, bank payload, registry record, backend endpoints. **Read first.** |
| [P1-a-firmware-wallet-core.md](P1-a-firmware-wallet-core.md) | A | 1 | Firmware wallet core: async approval screen, Solana decoder, signing, REQ/presence/registry checks, SNTP |
| [P1-r-test-harness.md](P1-r-test-harness.md) | R | 1 | SE050 survey, network probe, signing harness, ESP-NOW RTT, verifier (JS + Python) |
| [P1-u-economies-registry-app-spec.md](P1-u-economies-registry-app-spec.md) | U | 1 | HACK mint, Nessie seeding, issuer key, SAS registry, badge routes on the existing server, **app decision doc (unblocks Part 2)** |
| [P2-a-payer-flow.md](P2-a-payer-flow.md) | A | 2 | Pay app, Solana and bank signing, approval states, spending cap |
| [P2-r-payee-presence-attacks.md](P2-r-payee-presence-attacks.md) | R | 2 | Request/merchant app, handshake responder, impostor mode, attack console |
| [P2-u-backend-dashboard-home.md](P2-u-backend-dashboard-home.md) | U | 2 | `/bank/authorize`, audit memos, revocation, dashboard, Home/History, demo |

Part 2 specs are drafts until U finalizes the app decision doc (P1-U §5, target hour 6).
Gates: hour 8 — one badge-signed HACK payment on devnet · hour 14 — impostor caught · hour 20 — both rails or two scenes.
Master product PRD: [Prd-verified-payment-key.md](Prd-verified-payment-key.md). Existing backend and dashboard: [repository overview](../../README.md) and [dashboard docs](../dashboard/).
