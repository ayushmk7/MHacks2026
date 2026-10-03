# Verified Hardware Payments on the Solana Badge

MHacks 2026. A wallet on the Solana DEF CON badge that proves who you are paying and shows exactly what you are signing, so impostors, spoofed requests and compromised apps cannot trick you into a payment. Payments are SPL transfers of a demo token (HACK) on Solana devnet, verified identities are Solana Attestation Service attestations bound to each badge's key, and a laptop dashboard shows every payment live, runs the registry, and plays the attacker in the demo.

Built on Solana OS by spacemandev; we added the wallet and identity layer.

## Layout

```
MHacks2026/
├─ Prd-verified-payment-key.md                             product spec: problem, requirements, protocol, demo plan
├─ PRD_GUIDE.md                                            build-spec index (start with 00-Interfaces.md)
├─ distinctive-frontend.md                                  frontend design guide (typography, colour, motion, backgrounds)
├─ dashboard/                                               the laptop dev panel ("BadgePay"): API server, database schema, scripts, web app
├─ docs/
│  ├─ dashboard/                                            dashboard docs: API, ARCHITECTURE, BADGE-GAPS, RUNBOOK, TIGER-DATA
│  └─ os/                                                   badge OS build documentation and host-tested reference code
└─ README.md                                                this file
```

## Start here

| To | Read |
|---|---|
| Understand the product | [PRD](Prd-verified-payment-key.md) |
| Implement the badge workflow | [Build specs](PRD_GUIDE.md), starting with [Interfaces](00-Interfaces.md) |
| Run the dashboard | [dashboard/README.md](dashboard/README.md). All its npm scripts run from inside `dashboard/` |
| Look up how the dashboard works | [docs/dashboard/](docs/dashboard/): [ARCHITECTURE](docs/dashboard/ARCHITECTURE.md), [API](docs/dashboard/API.md), [TIGER-DATA](docs/dashboard/TIGER-DATA.md), [BADGE-GAPS](docs/dashboard/BADGE-GAPS.md), [RUNBOOK](docs/dashboard/RUNBOOK.md) |
| Build the badge OS | [docs/os/README.md](docs/os/README.md). The firmware itself is not in this repository yet; these documents describe how to build it on top of Solana OS |

## Status (2026-10-03)

- Dashboard: built and verified locally. Tests and build pass, every read route was exercised, and the read path was verified against live devnet traffic.
- On-chain path: pending devnet SOL. The faucet is rate-limiting this laptop, so nothing has been sent on real devnet with our own mint. Mint creation, issue, revoke, payments and attack detection have only run against a stand-in chain and `simulateTransaction`.
- Unblock: fund the authority `FZEAS6Nayu6nMX1RVmoNwoPA4tu1KNM5pvfoXsQzei3K` at <https://faucet.solana.com>, run `npm run devnet:setup` in `dashboard/`, restart the server. Steps in [RUNBOOK](docs/dashboard/RUNBOOK.md#first-real-devnet-run).
- Badge OS: documentation plus host-tested reference code under [`docs/os/reference/code/`](docs/os/reference/code/). Nothing has run on hardware.
- No real badge is connected to the dashboard. It uses stand-in software keypairs; what each badge still has to supply is in [BADGE-GAPS](docs/dashboard/BADGE-GAPS.md).
