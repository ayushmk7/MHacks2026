# Documentation

This folder is the source of truth for project documentation. Each subfolder is one documentation set with its own index.

| Set | Folder | What it covers | Owner |
|---|---|---|---|
| BadgeOS | [os/](os/README.md) | the badge firmware: architecture, wallet, ESP-NOW protocol, app platform, UI, apps, backend integration, guides (build, flash, provision, extend), testing, roadmap, reference. Also the host-tested reference code in [os/reference/code/](os/reference/code/) and the [naming and documentation conventions](os/reference/conventions.md) | Ayush |
| Specifications | [specs/](specs/README.md) | the product PRD and the work-package specs for each person and part (P1, P2, P3) | all |
| Backend and dashboard | [dashboard/](dashboard/README.md) | BadgePay: architecture, API, runbook, badge gaps, Tiger Data | Utsav |
| Design | [design/](design/README.md) | the dashboard's frontend design guide and the badge's Receipt mockups | all |
| Logs | [logs/](logs/README.md) | session and debugging logs (the badge-button I²C fault, the harness status) | all |

Where documents disagree: `docs/os/` wins for the firmware, and lists what changed from the specs in [differences-from-specs](os/reference/differences-from-specs.md). The test harness is documented in its own folder, [harness/README.md](../harness/README.md).

For setup and repository navigation, see the [repository README](../README.md).
