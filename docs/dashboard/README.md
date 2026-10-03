# Dashboard documentation

Documentation for BadgePay, the local laptop dashboard used to observe payments, manage badge attestations, and run the compromised-checkout demo.

| Document | Contents |
|---|---|
| [Architecture](ARCHITECTURE.md) | Components, data flow, database model, modes, design decisions, and known shortcuts. |
| [API](API.md) | HTTP routes, request and response shapes, errors, SSE events, and badge listener. |
| [Runbook](RUNBOOK.md) | Setup, demo-day sequence, verification state, and recovery steps. |
| [Badge gaps](BADGE-GAPS.md) | Hardware-dependent work and integration inputs. |
| [Tiger Data](TIGER-DATA.md) | TimescaleDB/Tiger Data features and measurements. |

For installation and commands, see the [dashboard README](../../dashboard/README.md). For the project-wide index, see [documentation](../README.md).
