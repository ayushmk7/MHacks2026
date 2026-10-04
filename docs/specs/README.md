# Product and build specifications

| File | Owner | Part | What it covers |
| --- | --- | --- | --- |
| [00-Interfaces.md](00-Interfaces.md) | All | — | Decisions, shared contracts: Lua wallet API, signing prefixes, ESP-NOW messages, bank payload, registry record, backend endpoints, 🆕 **§9 Routing (relay network)**. **Read first.** |
| [P1-a-firmware-wallet-core.md](P1-a-firmware-wallet-core.md) | A | 1 | Firmware wallet core: async approval screen, Solana decoder, signing, REQ/presence/registry checks, SNTP, 🆕 routing-ready decoder and presence slots |
| [P1-r-test-harness.md](P1-r-test-harness.md) | R | 1 | SE050 survey, network probe, signing harness, ESP-NOW RTT, verifier (JS + Python), 🆕 multi-hop fragmentation test, routing vectors |
| [P1-u-economies-registry-app-spec.md](P1-u-economies-registry-app-spec.md) | U | 1 | HACK mint, Nessie seeding, issuer key, SAS registry, badge routes on the existing server, 🆕 relay attestations, **app decision doc (unblocks Part 2)** |
| [P2-a-payer-flow.md](P2-a-payer-flow.md) | A | 2 | Pay app, Solana and bank signing, approval states, spending cap, 🆕 routed payments (route verification, routed screen) |
| [P2-r-payee-presence-attacks.md](P2-r-payee-presence-attacks.md) | R | 2 | Request/merchant app, handshake responder, impostor mode, attack console, 🆕 relay and gateway apps, routing attacks |
| [P2-u-backend-dashboard-home.md](P2-u-backend-dashboard-home.md) | U | 2 | `/bank/authorize`, audit memos, revocation, dashboard, Home/History, demo, 🆕 route reporting, routes view, relay leaderboard, route metrics |
| **[U6-app-decisions.md](U6-app-decisions.md)** | U | — | **Final demo scope (2026-10-04): direct payments, Capital One as identity/settlement/funding, no routing, no OS changes. Wins over older specs.** |
| [P3-demo-apps-tier0.md](P3-demo-apps-tier0.md) | A | — | ~~App-level routing~~ — cancelled, kept for history |
| 🆕 [CHANGELOG-routing.md](CHANGELOG-routing.md) | U | — | What the relay-network update changed in each file and why; decisions made; open decisions |

Part 2 specs are drafts until U finalizes the app decision doc (P1-U §5, target hour 6).
Gates: hour 8 — one badge-signed HACK payment on devnet · hour 14 — impostor caught · hour 20 — both rails or two scenes, ✏️ and a 2-hop Solana route or the relay beat is dropped.
Routing is additive: it lands only after the direct-payment gates, and direct payment stays the fallback demo. Sections changed for routing are tagged **🆕 Routing** (new) or **✏️ Changed (routing)** (edited).
Master product PRD: [Prd-verified-payment-key.md](Prd-verified-payment-key.md). Existing backend and dashboard: [repository overview](../../README.md) and [dashboard docs](../dashboard/).

**Start here:** [`../PROJECT-OVERVIEW.md`](../PROJECT-OVERVIEW.md) (plain-language guide) and [`U6-app-decisions.md`](U6-app-decisions.md) (final scope). Routing (§9 and the 🆕 Routing sections) is **not being built** for the demo.
