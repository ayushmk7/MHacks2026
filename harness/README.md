# Harness

Spec: [docs/specs/P1-r-test-harness.md](../docs/specs/P1-r-test-harness.md).

Current plan: the harness is validated end-to-end on one badge (5mG1mVHD) first, then the same firmware is flashed to the other three badges and the multi-badge items run. See [docs/logs/2026-10-03-session-changes.md](../docs/logs/2026-10-03-session-changes.md#plan-validate-on-one-badge-then-roll-out).

## R0 setup

Python 3.11 (`brew install python@3.11` on macOS).

```
python3.11 -m venv harness/.venv
harness/.venv/bin/pip install -r harness/requirements.txt
```

`requirements.txt` pins `pynacl`, `solders`, `solana`, `requests` (the spec's attack-script set) plus `pyserial` for serial capture. JS work lives in `dashboard/`.

## R1 SE050 survey

```
harness/.venv/bin/python harness/se050_survey.py --port /dev/cu.usbserial-10
```

The script first asks **how many badges you are surveying now** (or pass `--badges N`). Survey whatever you have in hand; the spec's R1 table covers all 4 badges, so run it again later for the rest - each run is its own folder. Ctrl+C at any point saves the badges already finished.

For each badge:

1. **Power the badge properly**: power switch ON. If the buttons or I2C bus are dead, unplug USB, switch off, switch on, replug (see [docs/logs/BADGE-BUTTONS-I2C.md](../docs/logs/BADGE-BUTTONS-I2C.md)).
2. With `--port`, press Enter, then **unplug USB, switch off, switch on, plug USB back in**. The script waits for the port (CH340C, S1 on UART) and captures 15 s of that cold boot into the run folder. It never resets the badge itself, since a warm reset can wedge the I2C bus. Without `--port`, give the path to a log you captured yourself, or Enter for none.
3. The script **parses the log**: key location, whether the SE050 probe passed, the failing step, and whether the I2C bus was up. A log taken with the bus down is flagged and its result is marked incomplete; power-cycle and redo that badge.
4. On the badge, select the **Identity** row in Settings to open the Identity screen. Type the full **public key** shown under the small grey "public key" label: two lines, 43–44 base58 characters, starting with the badge ID. The 8-character badge ID alone is not enough. The badge ID is derived from it; check it matches the screen. Then enter what "key lives in" says: `secure element`, `software` or `unknown`.

Never enter a private key or seed phrase.

Each run writes `harness/results/<UTC timestamp>/`:

| File | Contents |
| --- | --- |
| `results.csv` | One row per badge: badge ID, pubkey, screen and log key location, log state, failing step and detail, I2C bus state, log path and SHA-256, result |
| `table.md` | The R1 table (badge → pubkey → key location → failing step) |
| `pubkeys-for-U.json` | `pubkey` + `keyLocation` per badge, in the vocabulary `dashboard/server/config/badges.json` accepts (`se050` / `software` / `unknown`) |
| `badgeN-boot.log` | The captured boot log (with `--port`) |

An interrupted run still saves the badges it finished.

### How the log is judged

The firmware only creates the identity on a badge's **first boot**; later boots load the stored key and run no probe. So:

- `probe_passed` only when the log shows the key being created on the SE050 (`generating Ed25519 key`) and the identity ending up on the secure element. Creation runs key generation, public-key read, a probe signature and local verification.
- `failed` with the step (`se050_presence_or_atr`, `key_generation`, `public_key_read`, `eddsa_sign`, `probe_signature_verification`, `identity_selection`, `other_documented_step`) when an `[id]`/`[se050]` failure line is present.
- `unknown` when the log only shows a stored key being loaded. That is the normal case for an already-provisioned badge, and it says where the key is but not why.
- `unavailable` (no log) / `unreadable` (log path could not be read).

`--parse LOGFILE` prints the assessment of one saved log. `--check` runs the parser and key-validation self-tests without a badge.
