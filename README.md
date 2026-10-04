# Verified Hardware Payments on the Solana Badge

MHacks 2026. A wallet on the Solana DEF CON badge that proves who you are paying and shows exactly what you are signing, so impostors, spoofed requests and compromised apps cannot trick you into a payment. Payments are SPL transfers of a demo token (HACK) on Solana devnet. Before a payment the badge's own firmware checks three things: who is being paid (a registry record signed by the issuer), that they asked for exactly this (a signed request), and that their badge is here now (a presence proof). A laptop dashboard shows every payment live, runs the registry, and plays the attacker in the demo.

Built on Solana OS by spacemandev; we added the shell, the wallet, identity and app-platform layers.

## The four parts

| Part | Folder | Owner | Start here |
|---|---|---|---|
| **BadgeOS**, the badge firmware: shell, wallet core, approval screen, app platform, Lua and native apps | [`os/`](os/README.md) | Ayush | [os/README.md](os/README.md), then [docs/os/](docs/os/README.md) |
| **Backend and dashboard** ("BadgePay"): API server, badge listener, registry, database, web app | [`dashboard/`](dashboard/README.md) | Utsav | [dashboard/README.md](dashboard/README.md), then [docs/dashboard/](docs/dashboard/README.md) |
| **Test harness**: probes, signing and presence timing, verifiers, fuzz set, attack console | [`harness/`](harness/README.md) | Raiana | [harness/README.md](harness/README.md) |
| **Documentation**: specs, firmware docs, backend docs, design, logs | [`docs/`](docs/README.md) | all | [docs/README.md](docs/README.md) |

`temp_firmware/solana-os-settings-only/` is a teammate's separate, stripped fork of the upstream firmware that boots into Settings; the harness apps also run on it ([its README](temp_firmware/solana-os-settings-only/README.md), [harness](harness/README.md)).

## Layout

```
MHacks2026/
├─ os/                 BadgeOS firmware (Arduino sketch os.ino), apps/, lib/, scripts/, test/, templates/
├─ dashboard/          backend and dashboard: server/, web/, scripts/, db/ (run every npm script from here)
├─ harness/            P1-R test harness: laptop scripts and four badge apps
├─ docs/
│  ├─ os/              BadgeOS documentation, the source of truth for the firmware
│  ├─ specs/           product PRD and the work-package specs
│  ├─ dashboard/       backend docs: architecture, API, runbook, badge gaps, Tiger Data
│  ├─ design/          dashboard design guide and the badge's Receipt mockups
│  └─ logs/            session and debugging logs
├─ temp_firmware/      a teammate's settings-only firmware fork
└─ README.md           this file
```

## Quick start: four badges

From the repository root, once: `python3 -m venv .venv && .venv/bin/pip install pyserial esptool`. Building needs `arduino-cli` with the ESP32 core ([toolchain](docs/os/guides/build-flash-provision.md#toolchain)).

```bash
cd os
# 1. one build, firmware and apps flashed to every badge in parallel; fails if two launchers differ (dev profile)
scripts/fleet.sh release /dev/cu.usbserial-10 /dev/cu.usbserial-210 /dev/cu.usbserial-310 /dev/cu.usbserial-410

# 2. power-cycle every badge once: USB out, switch off, switch on, USB in (otherwise the buttons may stay dead)

# 3. provision each badge: pinned public values come from os/provision.public.env; nothing secret is needed
../.venv/bin/python scripts/vkdev.py --port /dev/cu.usbserial-10 provision \
    --listener http://<backend laptop address>:8788 --wifi "<SSID>" "<password>" \
    --badge-id 1 --label "<label>" --badges-json badges.provisioned.json

# 4. check: launcher -> TESTS -> RUN ALL on the badge, or from the laptop
../.venv/bin/python scripts/vkdev.py --port /dev/cu.usbserial-10 info
```

The full procedure, with what to do when something is wrong: [flash another badge](docs/os/guides/flash-another-badge.md). The backend: [dashboard/README.md](dashboard/README.md) (Node 22.12 or newer and Docker; the badge listener is on port 8788).

## Where the documentation is

| To | Read |
|---|---|
| Understand the product | [PRD](docs/specs/Prd-verified-payment-key.md), [specs index](docs/specs/README.md) |
| Understand, build or extend the firmware | [docs/os/README.md](docs/os/README.md): architecture, wallet, protocol, platform, UI, apps, guides, testing |
| Write a badge app | [os/apps/README.md](os/apps/README.md), [Lua API](docs/os/platform/lua-api.md), [extending](docs/os/guides/extending.md#add-an-app) |
| Run or change the backend | [dashboard/README.md](dashboard/README.md), [docs/dashboard/](docs/dashboard/README.md) |
| Run the harness | [harness/README.md](harness/README.md) |
| Name and document things | [conventions](docs/os/reference/conventions.md) |

## Status (2026-10-04)

- **BadgeOS runs on the badges** with its own shell, wallet core, approval screen and apps. The host suites and 26 scripted device tests pass on the development badge ([docs/os status](docs/os/README.md#status)).
- **Two badges:** requests, the green approval (registry record verified, payee present), the contact swap and the duel passed between two badges on 2026-10-04 (commit `0be6486`). The status section of `docs/os/README.md` and parts of the flashing guide were written before that run and still describe one badge.
- **Provisioning values are pinned:** issuer `2SXh6Xng9b1qQBCEn3pt2Ucwcb4ibBWwBKuuTwNgEzJy` and HACK mint `3VmWnzfWTfS5UGkwEjbMZnpjd1DPtKyMcwKeJc4QM9wP` (2 decimals) on devnet, in [`os/provision.public.env`](os/provision.public.env).
- **Not recorded as verified in the repository:** a badge-signed payment confirmed on devnet (Gate 1), the full demo script, and the network paths (balance, registry fetch, feed). Their tests exist (`t_sign_net.py`, `t_pay_2.py`); the list of what each needs is [deferred verification](docs/os/roadmap/implementation-plan.md#deferred-verification).
- **Keys:** every badge signs with a software key; the secure element (SE050) is never addressed ([hook H21](docs/os/architecture/upstream-hooks.md)).
- **Backend:** its own status table is in [dashboard/README.md](dashboard/README.md#status-2026-10-03) (dated 2026-10-03).
