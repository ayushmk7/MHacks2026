# Logs

Session and debugging logs: what was tried, what was seen, and what is still unproven. A log records a moment; it is not a specification. When a finding changes how something is built or run, the owning document (in [`docs/os/`](../os/README.md), [`docs/dashboard/`](../dashboard/README.md) or [`harness/`](../../harness/README.md)) is updated and links back here for the evidence.

| Log | Author | What it records |
|---|---|---|
| [BADGE-BUTTONS-I2C.md](BADGE-BUTTONS-I2C.md) | Raiana | why the badge's buttons go dead (the I²C clock line held low after a warm reset), the power-cycle checklist, and the evidence so far. Linked from the flashing guides |
| [raiana-p1-test-harness-changes.md](raiana-p1-test-harness-changes.md) | Raiana | status and change log of the P1-R test harness: what is committed, what was tested and how, what is left |

New logs: name them in lower-case kebab case with the topic first (`<topic>-<what>.md`), and start with the date and a one-line status. `BADGE-BUTTONS-I2C.md` predates that rule and keeps its name because the flashing guides, the harness README and the source link to it.
