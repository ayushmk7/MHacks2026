# Troubleshooting

Where to read logs, a table from symptom to likely cause to fix, how to read an SE050 failure, and pointers into the upstream manual.

Audience: everyone.

Status: design, not yet built on hardware.

No symptom in this document has been observed on a badge. The table is derived from the design and from reading upstream source (`firmware/solana-os/` at commit `812b8c7` of the [upstream repository](https://github.com/spacemandev-git/solana-defcon-badge-26/tree/812b8c7aca5c366d18c0b040fafd2999f7204d84/firmware/solana-os)). Add rows as real failures are found.

## Log tags

A log line is `[tag] message` [UPSTREAM `src/badge_log.h`].

Tags added by us [OURS]:

| Tag | Covers |
|---|---|
| `wallet` | gate decisions, audit lines |
| `pay` | frames, sessions, timings |
| `attest` | attestation fetch and parse |
| `rpc` | JSON-RPC calls |
| `native` | the native (C++) app runtime |

Upstream tags you will meet [UPSTREAM]: `os` (boot, heartbeat, force quit), `lcd` (display), `id` (identity, key source), `se050` (secure element transport), `btn` (button expander), `bridge` (phone bridge).

Three ways to read them:

| Transport | How | Notes |
|---|---|---|
| Serial | `arduino-cli monitor -p /dev/cu.usbserial-XXXX -c baudrate=115200` | everything since you connected; logs go to both UART0 and USB CDC [UPSTREAM] |
| On the badge | Settings → Console | the log ring, errors in red [UPSTREAM] |
| Over Wi-Fi | `tools/badge-push.py --host <ip> --token <code> logs` (or `GET /api/logs`) | the log ring; HTTP only [UPSTREAM] |

The log ring holds 64 lines of 120 characters [UPSTREAM `src/badge_log.h`]. For anything longer than a few seconds of activity, capture serial to a file: `arduino-cli monitor … | tee serial.log`.

While a wallet screen is open the main loop does not run, so the push server and the serial console are not serviced [OURS]. `badge-push.py` and `curl` time out until the user decides or the prompt closes after 60 s. That is expected behaviour, not a hang.

Lines worth knowing:

| Line | Meaning |
|---|---|
| `[os] ready` | boot finished [UPSTREAM `solana-os.ino:234`] |
| `[os] up <n>s  heap <n>KB  psram <n>KB  batt <n>%  btn=<xx> int=<c>  …` | heartbeat every 30 s; `btn=--` means the button expander is not answering [UPSTREAM `solana-os.ino:103-112`] |
| `[id] <badgeId>, secure element (<n> ms)` or `…, software (<n> ms)` | key source chosen at boot [UPSTREAM `src/identity/identity.cpp:265`] |
| `[id] SE050 refused at <stage>, sw <xxxx>` | the secure element refused; see [SE050 status words](#se050-status-words) |
| `[lcd] FATAL: could not allocate 320x240 framebuffer (PSRAM missing?)` | built without `PSRAM=opi` [UPSTREAM `src/hal/display.cpp:103`] |
| `[btn] P4 SELECT down (raw=0x2F)` | one line per debounced key press [UPSTREAM `src/hal/buttons.cpp:172`] |
| `[wallet] ata <n>ms` | the badge derived its own token account at boot ([M7](../testing/measurements.md#m7)) |
| `[wallet] sign <n>ms verify <n>ms` | timing of each signature ([M2](../testing/measurements.md#m2)) |
| `[pay] proof ok elapsed=<n>ms` | a PROOF arrived and verified inside the deadline ([M1](../testing/measurements.md#m1)); `[pay] proof late elapsed=<n>ms` and `[pay] proof bad_sig` are the two failures |
| `[pay] req bad_sig from <mac>` | a REQ failed its signature check and was dropped; `<mac>` is the sender, lower case, `aa:bb:cc:dd:ee:ff` |
| `[pay] req first id=<16 hex> t=<ms>` | first receipt of a request id ([M3](../testing/measurements.md#m3)) |
| `[wallet] modal open screen=<A\|B\|C> id=<16 hex or -> t=<ms>` | an approval screen opened ([M3](../testing/measurements.md#m3)) |
| `[wallet] select sig=<sig16> t=<ms>` | SELECT was accepted and a signature produced ([M4](../testing/measurements.md#m4)) |
| `[rpc] status sig=<sig16> confirmed t=<ms>` | first `confirmed` or `finalized` answer for that signature ([M4](../testing/measurements.md#m4)) |
| `[wallet] mem heap=<bytes> stack=<bytes> at=modal` (or `at=sent`) | free heap and stack headroom ([M6](../testing/measurements.md#m6)) |
| `[wallet] no_display: refusing to sign` | a signing call returned `no_display` at once; nothing was drawn |
| `[app] <id>: <function> needs permission <name>` | a native app called a guarded function without the permission; logged once per launch |
| `[rpc] <method> <n>ms status=<code>` | one line per RPC call ([M5](../testing/measurements.md#m5)) |
| `[attest] malformed` | an attestation account failed a layout check and is treated as unverified |

## Symptoms

### Wallet, payments and identity

| Symptom | Likely cause | Fix |
|---|---|---|
| `identity.sign` returns `not_ready` | `mint` not configured, or identity failed | set config ([configure.md](configure.md#wallet-config)); check `[id]` at boot |
| `denied` | `app.ini` lacks `permissions=sign` | add it, push again |
| Blocked screen "Not your token account" | wrong mint configured, or the builder used another source | check `mint`; compare `sol.ata()` with the dashboard's token account |
| Home shows "stale" | RPC unreachable or rate-limited | check hotspot; lower polling; own RPC URL |
| Every payee is `NOT PRESENT` | `deadline_ms` too low for the key backend; payee app blocking in HTTP; badges on different channels | measure ([M1](../testing/measurements.md#m1)), raise deadline; check Settings → ESP-NOW channel on both |
| REQ never appears on the payer | different mint config (mint_tag), RSSI below `rssi_min`, or the `on_espnow` upstream bug: patch P1 not applied | compare config; move closer; verify P1 |
| `verified` never appears | `cred`/`schema` do not match the dashboard's authority; attestation not issued; RPC error | compare with `/api/status`; check Registry page; read `[attest]` lines |
| Badge falls back to software key | SE050 refused; see `[id] SE050 refused at <stage>, sw <xxxx>` | [SE050 status words](#se050-status-words); accept software and say so |
| `/badge/pending` returns 404 | badge key not in `badges.json` | add it, restart the server |
| App killed with "exceeded its time budget" | Lua loop, or more than 12 s of blocking calls in one callback | spread calls across frames |
| No keys respond | TCA9534 not answering | serial `[btn]` lines; Settings → Device info [UPSTREAM] |

Further symptoms that follow from the design [OURS]. Error strings are listed in [../reference/error-codes.md](../reference/error-codes.md#badge_err_t).

| Symptom | Likely cause | Fix |
|---|---|---|
| `identity.sign` returns `rate_limited` | 2 s cooldown per app after any prompt that ended without a signature (rejected, timed out, blocked, or a blocked screen); or three of those from one app within 60 s, which locks that app out for 30 s | wait; see [rate limits](../wallet-core/signing-gate.md#rate-limits) |
| `busy` | another wallet prompt or payment session is active; one session at a time | close the other session (`pay.cancel()`, or stop the app that opened it) |
| `no_display` | framebuffer missing or button expander absent; the wallet refuses rather than sign blind | check `[lcd]` and `[btn]` lines; the board needs PSRAM and a working TCA9534 |
| `approval_timeout` (Pay, Request and Checkout show `No answer in 60 s, nothing was signed`) | the 60 s budget of the signing call ran out before a decision | sign again; a fresh blockhash is needed because it lives roughly 60 to 90 s |
| Approval screen line `RPC TLS not pinned` | no CA named `rpc-ca` installed | [configure.md](configure.md#ca-pin) |
| Identity line `NOT CHECKED` (amber) | RPC unreachable, HTTP or JSON error, or the route is the phone bridge (then also `checked via phone`) | check Wi-Fi; identity is never shown as verified over the bridge |
| Identity line `EXPIRED` | the attestation's expiry has passed and the clock is synced | re-issue on the Registry page |
| RPC fails only when `rpc-ca` is installed | the pinned TLS handshake may check certificate dates while the badge clock is unset [UNVERIFIED]. The RPC client waits up to 5 s after Wi-Fi connects for the first SNTP sync before its first pinned request | if pinned handshakes still fail with a date error, remove the certificate (`tools/badge-push.py --host <ip> --token <code> rmcert rpc-ca`) and run unpinned; the screen then says `RPC TLS not pinned` |
| Impostor or revoked merchant shows amber `UNVERIFIED` where red was expected | the payer's badge has never seen the real merchant verified, or its known-names store was erased (Forget known names, or a filesystem image flashed at `0x670000`) | repeat the seeding step ([pre-event checklist, item 10](pre-event-checklist.md#10-seed-known-names-on-each-judge-badge)) |
| Verified merchant shows `REVOKED` | its attestation was closed after this badge had seen it verified | re-issue; it shows `verified` again within 30 s. Use Settings → Wallet → Forget known names only if the badge's memory is stale after a registry reset, and never between the seeding step and the demo |
| Honest badge shows `NAME MISMATCH` | it is not attested itself and claims a name that this badge has seen verified for a different key (for example after the merchant's identity was reset) | issue the attestation to the new key on the Registry page; or Settings → Wallet → Forget known names on the payer |
| Blocked screen "Unknown token" | the transaction's mint is not the configured `mint` | check `mint` on both sides ([values that must match](configure.md#values-that-must-match-the-dashboard)) |
| Blocked screen "Wrong token decimals" | `decimals` config differs from the mint's | set `decimals` to `token.decimals` from `/api/status` |
| Blocked screen "Above the maximum" | amount above `max` | intended; raise `max` only if the demo needs it |
| Blocked screen "Signing failed" (`sign_failed`) | the key did not produce a signature that verifies against the badge's own public key; nothing was signed | read the `[id]` and `[se050]` lines; on an SE050 badge see [SE050 status words](#se050-status-words) |
| Blocked screen "Transaction too large" | message longer than the key backend's limit (242 bytes for the SE050 with patch P6) | check P6; a transfer message is 214 or 216 bytes |
| Blocked screen "Unknown instruction" on a normal payment | the message has anything besides one `TransferChecked` (a memo, a compute-budget or token-account-create instruction, Token-2022) | build with `sol.transfer_message`; the `reason:` line names the decoder rule |
| Pay app: `Expired, try again` | blockhash expired before send (the node's message contained `lockhash`) | retry; nothing moved |
| Pay app: `Send failed, nothing moved` | the node refused the transaction for another reason | read the `[rpc]` lines; check the payer's HACK and SOL balances |
| Pay app: `Sent?, checking`, then `Not confirmed. Check History before paying again` | `rpc.send` timed out or failed in transport, so the transaction may have reached the node; Pay polled for 30 s without a confirmation | check History and the dashboard feed before paying again |
| Pay app: `Payee has no HACK account` | the payee's token account does not exist | `npm run devnet:setup` in `dashboard/` after adding the badge to `badges.json` |
| Home: "No Wi-Fi: Settings > Wi-Fi" | no route | [configure.md](configure.md#wi-fi) |
| Home: "Wallet not configured" | `mint` is empty | [configure.md](configure.md#wallet-config) |
| Checkout never opens during the attack demo | `dash_url` empty or wrong; listener closed; attempt older than 90 s; receive mode is on (Home does not poll then) | check `dash_url` against `/api/status` → `badgeListener.url`; `BADGE_LISTEN_HOST` set and server restarted; click Charge again |
| Attack page does not show `rejected` | the badge cannot reach the listener | use **Mark rejected** on the Attack page; it is offered on pending and on expired attempts |
| Push of a Lua app refused with HTTP 409 or `ERR id reserved` | its id is the id of a compiled-in app | rename the Lua app |
| Error screen `needs API <n>` | the app's `min_api` is higher than the firmware's API version (2) | flash a newer build or lower `min_api` |
| `badge-push.py` or `curl` times out | a wallet screen is open on the badge | decide on the badge, then retry |

### Build and flash

| Symptom | Likely cause | Fix |
|---|---|---|
| Upload cannot connect | board not in download mode | hold `BOOT1`, tap `RST1`, release `BOOT1` [UPSTREAM root `README.md`] |
| Nothing on the screen, badge otherwise alive | built without `PSRAM=opi`; log says `FATAL: could not allocate framebuffer` | use the full board name from [build-and-flash.md](build-and-flash.md#toolchain) [UPSTREAM] |
| Serial console prints but does not respond | it speaks the push protocol, not free text | try `PING`, then `AUTH <code>` [UPSTREAM] |
| A pre-flash `grep` prints a line | a file outside the gate includes `identity_private.h` or calls a signing primitive | move the call behind the wallet API; see [build-and-flash.md](build-and-flash.md#pre-flash-checks) |
| `ctest` fails | decoder, derivation, codec or attestation-parser behaviour changed | do not flash; see [../testing/host-tests.md](../testing/host-tests.md) |
| Build fails at `#if __has_include("local_config.h")` | the installed core's compiler does not support it [UNVERIFIED] | commit an empty `local_config.h` and drop the `#if` ([build-and-flash.md](build-and-flash.md#compile-time-switches)) |
| Upload or monitor says the serial port is busy | another program holds the port | close the other serial monitor, then retry |

The laptop panel has its own recovery table: [dashboard runbook](../../dashboard/RUNBOOK.md).

## SE050 status words

The secure-element path has never run against a real SE050 [UPSTREAM `src/hal/se050_apdu.cpp:1-4`]. It fails closed: any wrong answer drops the badge to a software key, and each failure logs the stage it stopped at and the status word [UPSTREAM `README.md:1054-1072`].

How to read a failure:

1. Open serial at 115200 and reboot the badge.
2. Find the `[id]` lines. A fallback looks like:

   ```
   [id] SE050 refused at <stage>, sw <xxxx>
   [id] falling back to a software key
   [id] <badgeId>, software (<n> ms)
   ```

3. Look up the stage and status word below. Stages are `select`, `exists`, `generate`, `read`, `sign`, `delete` and `t1` [UPSTREAM `src/hal/se050_apdu.cpp`].

| Stage | Status word | Meaning | What to do |
|---|---|---|---|
| `t1` | `0000` | The block layer never got a well-formed answer. A bus or reset problem, not an applet one. | Check that the I²C bus answers at `0x48` (Settings → Device info, or the upstream test kit). If the open sequence misbehaves, upstream names the first thing to try: open with R-Sync `0xC0` then Get ATR `0xC7` instead of the soft reset `0xCF` (`src/hal/se050_t1.h`, "ONE KNOWN DEVIATION"). |
| `select` | none on the `[id]` line, which reads `[id] SE050 unavailable (select)`; the `se050` tag has the detail: `[se050] link/select failed (…)`, and `[se050] applet select refused: SW=6A82` when the applet answered | The link did not come up, or (with `6A82`) the applet identifier is wrong or the applet is not installed. | Software key. |
| `exists` | `6982` after a clean applet SELECT | Security status not satisfied: this part requires an SCP03 secure channel, which upstream deliberately does not implement. | Software key. |
| `generate` | `6A80` or `6A81` | Wrong data, or function not supported: this SE050 variant has no Ed25519. | Software key. |
| any | any other non-zero value | The applet answered and refused. | Look the status word up in NXP AN12413, section 4.4. |

Other `[id]` lines [UPSTREAM `src/identity/identity.cpp`]:

| Line | Meaning |
|---|---|
| `[id] SE050 unavailable (<stage>)` | the link or the applet SELECT failed before any command was sent |
| `[se050] <stage> refused, sw <xxxx>` | the same refusal as the `[id]` line, logged by the driver (`src/hal/se050_apdu.cpp`) |
| `[se050] <stage>: transport failed (<reason>)` | the block layer failed during that command; the `[id]` line then shows stage `t1`, `sw 0000` |
| `[id] SE050 signature failed local verification, not trusting it` | the probe signature made inside the SE050 did not verify in software; most likely the byte-order assumption is wrong |
| `[id] SE050 did not answer; identity present but cannot sign this boot` | a secure-element identity is stored, the part is silent this boot; signing fails until it answers |
| `[id] SE050 public key does not match stored identity; refusing to sign with it` | the part's key differs from the stored one; signing is refused |
| `[id] SE050 refused to sign at <stage>, sw <xxxx>` | a signature request failed after boot |

What is specific to our build [OURS]:

- A payment message is 214 bytes (216 for v0). Upstream caps SE050 signing at 180 message bytes; patch P6 raises it to 242, which needs T=1 chaining of a roughly 230-byte APDU. [UNVERIFIED on silicon; fallback below.]
- Every signature is verified against the badge's own public key before it leaves the wallet core. A failure returns `sign_failed`, so a byte-order or transport fault cannot produce a bad signature on the wire.
- The test that settles it is [T-SE1](../testing/acceptance.md#t-se1).

Fallback when any badge fails: build with `WALLET_FORCE_SOFTWARE_KEY 1` (patch P14), create a new identity on that badge, and report `software` honestly. Steps: [build-and-flash.md](build-and-flash.md#resetting-identity). Background and the full bring-up procedure: [../wallet-core/keys-and-se050.md](../wallet-core/keys-and-se050.md).

## Upstream troubleshooting

The upstream manual has its own troubleshooting section (`firmware/solana-os/README.md`, "Troubleshooting") [UPSTREAM]. The entries most likely to matter here:

| Symptom | Upstream's answer |
|---|---|
| Launcher is empty after pushing | The app id must be `[a-z0-9._-]`. Check `GET /api/apps`; a rejected id returns 400. |
| App dies with "not enough memory" well under 1 MB free | That is the app's own 1 MiB cap, not the system heap. `system.lua_memory()` reports used and limit. |
| Cannot see other badges on the radar | Same channel? Wi-Fi joined to something else? ESP-NOW follows the Wi-Fi channel ("Radio notes"). |
| Web UI unreachable at `solana-badge.local` | mDNS is unreliable on some networks; use the IP from Settings → Push. |
| Pushed an app and the badge is stuck in it | Hold CANCEL for 1.5 s. The main loop force-quits the app. |
| `not a PEM certificate` when uploading | The file is DER. Convert: `openssl x509 -inform der -in x.crt -out x.pem`. |
| Keys arrive as the wrong action | The button mapping is per board revision. Watch `[btn] P4 SELECT down` lines and compare with `src/config.h` ("Buttons"). |

Other upstream sections worth knowing: "Pushing apps" (HTTP API and the line protocol), "Phone bridge" (the phone terminates TLS, which is why identity is not shown as verified over it), "Identity" (honest status of the secure-element path), and "Working with a fresh board" in the root README (test kit, I²C scan).

Known upstream issues that our patches address or that you may still hit [UPSTREAM]:

| Issue | Status in the fork |
|---|---|
| The ESP-NOW receive handler is installed once at boot and cleared when any app stops (`solana-os.ino:134-137`, `src/lua_sdk/lua_runtime.cpp:290`), so `on_espnow` works only in the first app launched | fixed by patch P1; checked by [T-APP2](../testing/acceptance.md#t-app2) |
| The app-store broker signs a registration message without a button press and can raise install prompts while the shell is in front | compiled out by default (`WALLET_ENABLE_BROKER 0`) |
| While the badge runs its own hotspot, it believes it has Wi-Fi and never uses the phone bridge | unchanged; join the phone hotspot instead |
| The software seed is plaintext in NVS; flash encryption and secure boot are off | unchanged; reported honestly as `software` |

## Requirements covered

F17 (diagnosing the SE050 path and its fallback). Supports the PRD's honesty and judge-UX requirements by naming what each state on screen means.

## Open items

- [UNVERIFIED] No symptom here has been seen on hardware; the table is derived from the design.
- [UNVERIFIED] SE050C2: Ed25519 support, no SCP03, byte order, T=1 chaining, the 242-byte limit. Fallback: software key, and say so.
- [UNVERIFIED] `rssi_min = -75`. Fallback: tune at the table; -100 disables it.
- [UNVERIFIED] PROOF deadline against the real signing time. Fallback: raise `deadline_ms` after measuring M1.
- [UNVERIFIED] Whether a pinned TLS handshake checks certificate validity dates while the badge clock is unset. Fallback: wait up to 5 s for SNTP before the first pinned request; if it still fails, remove `rpc-ca` and run unpinned (shown on screen).
- [UNVERIFIED] Exact wording of the node's preflight error for an expired blockhash. Pay matches `lockhash`; otherwise it shows `Send failed, nothing moved`.
- [UNVERIFIED] `__has_include("local_config.h")` under the installed core's compiler. Fallback: commit an empty `local_config.h` and drop the `#if`.
