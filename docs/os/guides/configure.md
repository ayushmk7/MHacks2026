# Configure a badge

Put a flashed badge on the hotspot, give the wallet its settings, install the apps, pin the RPC certificate, and check the result on the badge.

Audience: operators (whoever prepares the four badges).

Status: design, not yet built on hardware.

Nothing in this guide has been run on a badge. The Wi-Fi and app-push steps use upstream tooling as documented in `firmware/solana-os/README.md` at commit `812b8c7` of the [upstream repository](https://github.com/spacemandev-git/solana-defcon-badge-26/tree/812b8c7aca5c366d18c0b040fafd2999f7204d84/firmware/solana-os). The wallet routes (`/api/wallet/config`, `/api/wallet/audit`) are ours and exist only in the fork (patch P11).

## Before you start

| You need | Where it comes from |
|---|---|
| A badge flashed with the fork | [build-and-flash.md](build-and-flash.md) |
| A phone hotspot that the laptop and all badges join | all badges must share one Wi-Fi channel for ESP-NOW [UPSTREAM `README.md:1248-1262`] |
| The dashboard set up, with a mint and a registry | inside `dashboard/`: `npm run devnet:setup`, then `npm run server`; see the [dashboard runbook](../../dashboard/RUNBOOK.md) |
| The badge's pairing code | Settings → Push on the badge: six digits, created on first boot [UPSTREAM] |
| A shell in `firmware/solana-os/` | for `tools/badge-push.py` |

`tools/badge-push.py` accepts the pairing code as `--token` or in the environment variable `BADGE_TOKEN` [UPSTREAM `README.md:750-764`]. After five wrong codes the badge locks the write path out with a growing backoff (30 s, up to 5 min) [UPSTREAM].

With four badges on one network, address them by IP. The mDNS name `solana-badge.local` is the same default on every badge and is unreliable on some networks [UPSTREAM `README.md:1290-1291`].

## Wi-Fi

Skip this section if the hotspot credentials were baked into the image (`WALLET_DEFAULT_WIFI_SSID` in `local_config.h`, see [build-and-flash.md](build-and-flash.md#compile-time-switches)): the badge joins on first boot [OURS]. An image built without `local_config.h` has empty defaults and skips that first-boot step, so this section applies.

The badge has no keyboard, so a WPA passphrase cannot be typed on it. An open network joins from Settings → Wi-Fi. A secured one is provisioned through the badge's own hotspot [UPSTREAM `README.md:215-218`]:

1. On the badge: Settings → Wi-Fi → Start hotspot.
2. On the laptop: join the network `badge-XXXX`, password `solanabadge`. The badge is at `192.168.4.1`.
3. On the badge: read the pairing code on Settings → Push.
4. On the laptop:

   ```bash
   cd firmware/solana-os
   tools/badge-push.py --host 192.168.4.1 --token 123456 join "<phone hotspot SSID>" --password "<password>"
   ```

5. Put the laptop back on the phone hotspot.
6. On the badge: Settings → Push now shows the badge's IP address on the hotspot. Use it as `<badge-ip>` below.

The badge saves one network and reconnects to it at boot. The credentials are stored unencrypted in NVS [UPSTREAM `README.md:316-318`].

Check that the laptop can reach the badge. `/api/status` is the only route that answers without the pairing code [UPSTREAM `README.md:773-790`]:

```bash
curl -s "http://<badge-ip>/api/status"
```

[UNVERIFIED: that the phone hotspot lets two clients reach each other (no client isolation). Fallback: if the badge and the laptop cannot reach each other, configure over the serial console (see [Without Wi-Fi](#without-wi-fi)) and use the "Mark rejected" button on the dashboard for the attack demo; payments and identity checks do not need the laptop.]

## Wallet config

The wallet is not ready until `mint` is set: `identity.sign` returns `not_ready` and Home shows "Wallet not configured" [OURS].

1. On the laptop, read the values the badge must be configured with:

   ```bash
   curl -s http://127.0.0.1:8787/api/status      # token.mint, token.decimals, registry.credential, registry.schema
   ipconfig getifaddr en0                        # the laptop's IP on the hotspot (macOS, Wi-Fi on en0)
   ```

2. Push them to the badge (badge on the hotspot; IP on Settings → Push):

   ```bash
   B=http://<badge-ip>; T=123456
   curl -s -H "X-Badge-Token: $T" -H "Content-Type: application/json" -X POST "$B/api/wallet/config" -d '{
     "mint":"<HACK_MINT>","decimals":2,"cred":"<registry.credential>","schema":"<registry.schema>",
     "rpc_url":"https://api.devnet.solana.com","dash_url":"http://<laptop-ip>:8788"}'
   ```

3. Read it back:

   ```bash
   curl -s -H "X-Badge-Token: $T" "$B/api/wallet/config"       # read back
   ```

4. Repeat for each badge, or bake the values into the image once (`WALLET_DEFAULT_*`, see [build-and-flash.md](build-and-flash.md#compile-time-switches)).

`GET /api/wallet/config` requires the `X-Badge-Token` header and returns one JSON object with every key of the table below plus two read-only fields, `token_account` and `ready` [OURS]:

```json
{"rpc_url":"https://api.devnet.solana.com","mint":"<base58 or empty>","decimals":2,"symbol":"HACK",
 "cred":"<base58 or empty>","schema":"<base58 or empty>","cap":"10000","max":"100000","deadline_ms":400,
 "attest_ttl":30,"block_red":1,"dash_url":"","rssi_min":-75,
 "token_account":"<base58 or empty>","ready":false}
```

`POST /api/wallet/config` takes a JSON object with any subset of the keys and requires the same header. It is all-or-nothing: if any key is unknown or any value invalid, nothing is applied and the answer is HTTP 400:

```json
{"error":"bad_config","key":"<first offending key>","message":"<reason>"}
```

On success the answer is HTTP 200 with the same body as the GET, and one `CONFIG` line per changed key is written to the audit log. Changing `mint`, `cred`, `schema` or `rpc_url` clears the attestation cache and re-derives the badge's token account [OURS].

### Keys

Stored in NVS namespace `wallet`. Defaults come from `src/wallet/wallet_defaults.h` [OURS]. The reference for each key is [../wallet-core/config-limits-audit.md](../wallet-core/config-limits-audit.md).

| Key | Type | Default | Meaning |
|---|---|---|---|
| `rpc_url` | string | `https://api.devnet.solana.com` | JSON-RPC endpoint |
| `mint` | base58 string | `""` (wallet not ready until set) | HACK mint; must equal the dashboard's `HACK_MINT` |
| `decimals` | u8 | `2` | must equal the mint's decimals |
| `symbol` | string ≤ 7 | `HACK` | display only |
| `cred` | base58 string | `""` | SAS credential PDA (`/api/status` → `registry.credential`) |
| `schema` | base58 string | `""` | SAS schema PDA (`registry.schema`) |
| `cap` | decimal string, raw | `10000` | second-confirmation threshold (100.00 HACK) |
| `max` | decimal string, raw | `100000` | hard limit (1000.00 HACK) |
| `deadline_ms` | u16 | `400` | PROOF deadline |
| `attest_ttl` | u16 seconds | `30` | attestation cache freshness |
| `block_red` | u8 | `1` | red disables signing |
| `dash_url` | string | `""` | dashboard badge listener base, for example `http://172.20.10.2:8788`; empty disables checkout polling |
| `rssi_min` | i8 | `-75` | REQs weaker than this are not listed [UNVERIFIED threshold; fallback: set to -100 to disable the filter] |

Amounts are decimal strings of raw base units: `"10000"` is 100.00 HACK at 2 decimals.

What a POST accepts [OURS]:

| Key | Valid values |
|---|---|
| `mint`, `cred`, `schema` | empty, or base58 of exactly 32 bytes |
| `decimals` | 0 to 9 |
| `symbol` | 1 to 7 printable ASCII characters |
| `cap`, `max` | a decimal string or a JSON integer; `cap <= max` must hold after the request is applied |
| `deadline_ms` | 50 to 5000 |
| `attest_ttl` | 5 to 3600 |
| `block_red` | 0 or 1 |
| `rpc_url` | starts with `http://` or `https://`, at most 95 characters |
| `dash_url` | empty, or starts with `http://`, at most 63 characters |
| `rssi_min` | -100 to -30 |
| `token_account`, `ready` | read-only; sending either is `bad_config` |

### Values that must match the dashboard

| Dashboard | Badge config |
|---|---|
| `HACK_MINT` (`/api/status` `token.mint`) | `mint` |
| `token.decimals` | `decimals` |
| `registry.credential` | `cred` |
| `registry.schema` | `schema` |
| `RPC_URL` cluster | `rpc_url` (same cluster) |
| `BADGE_LISTEN_HOST:BADGE_LISTEN_PORT` | `dash_url` |
| `server/config/badges.json` `pubkey` | the badge's key (must be listed or `/badge/pending` returns 404) |

The credential and schema addresses depend on the registry authority keypair of the laptop that runs the dashboard. They are configuration, not constants: a different laptop gives different addresses.

On the laptop, in `dashboard/.env`, then restart the server:

```
BADGE_LISTEN_HOST=<laptop hotspot IP>
BADGE_LISTEN_PORT=8788
ATTACK_TX_VERSION=legacy
```

`BADGE_LISTEN_HOST` opens the listener that the Checkout flow polls. `/api/status` → `badgeListener.url` shows the URL a badge must poll; `dash_url` is that URL without `/badge/pending`. With `BADGE_LISTEN_HOST=0.0.0.0` the dashboard builds the URL from the laptop's first non-internal IPv4 address, which can be the wrong interface, so prefer the hotspot IP. `ATTACK_TX_VERSION=legacy` matches what the badge builds itself; the decoder also accepts v0 without address lookup tables [OURS, host-tested]. Background: [../integration/dashboard.md](../integration/dashboard.md) and [BADGE-GAPS.md](../../dashboard/BADGE-GAPS.md).

### Later changes

| Goal | Send |
|---|---|
| Set the PROOF deadline after measuring [M1](../testing/measurements.md#m1) | `{"deadline_ms": <n>}` to every badge; in a mixed fleet set it for the slowest signer |
| Disable the second confirmation | set `cap` equal to `max` |
| Let a judge sign through a red screen (hold SELECT 3 s) | `{"block_red": 0}`; the default `1` blocks signing on red |
| Stop filtering requests by signal strength | `{"rssi_min": -100}` |
| Stop checkout polling | `{"dash_url": ""}` |

Example:

```bash
curl -s -H "X-Badge-Token: $T" -H "Content-Type: application/json" -X POST "$B/api/wallet/config" -d '{"deadline_ms":520}'
```

Judges cannot change `cap` or `max` on the badge; both are config pushed by the team [OURS]. Whoever holds a badge's pairing code can change that badge's `cred`, `schema` and `rpc_url`, and so its notion of "verified". The change is audited, not prevented.

### Without Wi-Fi

The same keys can be set over the USB serial console or BLE with the line protocol [UPSTREAM protocol `README.md:826-872`; `SETWALLET` is OURS]:

```
AUTH 123456
SETWALLET deadline_ms 400
```

`AUTH` answers `OK authed`. Every command of this protocol answers with exactly one `OK` or `ERR` line [UPSTREAM]. `SETWALLET <key> <value>` sets one key and answers [OURS]:

| Reply | When |
|---|---|
| `OK <key>` | the value was stored (the same form as `SETWIFI`, which answers `OK <field>` [UPSTREAM `src/net/push_protocol.cpp:261`]) |
| `ERR unknown key` | `<key>` is not a config key |
| `ERR bad value` | the value fails the validation in the table under [Keys](#keys), applied to that one key |

The audit line for a change made this way carries `serial` or `ble` in its app field; a change over HTTP carries `push`.

## Apps

Push the Lua apps from the fork [UPSTREAM tool, `README.md:745-764`]:

```bash
cd firmware/solana-os
T=123456
for a in home pay request history checkout tipjar; do
  tools/badge-push.py --host <badge-ip> --token $T push apps/$a
done
tools/badge-push.py --host <badge-ip> --token $T list
```

Pushing a directory sends every `.lua`, `.ini`, `.txt`, `.json`, `.csv`, `.png` and `.jpg` in it. One file is capped at 96 KB [UPSTREAM].

What to expect on the badge:

- The launcher lists the apps, then Settings. An app that holds the `sign` permission has `$` after its name [OURS].
- Home starts by itself at boot: on first boot the fork sets `autostart` to `home` if it is unset [OURS], and upstream launches the autostart app when it is installed [UPSTREAM `solana-os.ino:230`].
- C++ apps are not pushed. They are compiled into the image; changing one means reflashing [OURS].

Alternative for many badges: flash the whole `apps/` directory as a LittleFS image at `0x670000` [UPSTREAM `app/scripts/build-firmware.ts`]:

```bash
mklittlefs -c <dir with apps/> -b 4096 -p 256 -s 0x980000 apps.bin
esptool.py --chip esp32s3 write_flash 0x670000 apps.bin
```

This replaces the entire filesystem partition, not only `/apps/`: installed CA certificates (`/certs/`) and the wallet's files (`/wallet/`: audit log, history, known names) are gone afterwards. Do it before the CA pin below, and not on a badge whose history you want to keep. Never do it on a judge badge between the seeding step and the demo: it erases the known-names store that makes the impostor and revocation screens red ([pre-event checklist, item 10](pre-event-checklist.md#10-seed-known-names-on-each-judge-badge)).

## CA pin

Without a pinned CA the badge's HTTPS calls to the RPC endpoint are not validated: upstream's HTTP client connects without a CA [UPSTREAM `src/net/net_route.cpp:34-40`]. An attacker on the hotspot could then forge "verified" or hide a revocation. Our RPC client validates TLS when a CA named `rpc-ca` is installed in the badge's certificate store, and shows the detail line `RPC TLS not pinned` on the approval screen when it is not [OURS]. See [../identity/attestation.md](../identity/attestation.md#transport-trust).

1. Find the root that signs the RPC host's certificate. [UNVERIFIED: which root `api.devnet.solana.com` chains to on the day. Fetch it then.]

   ```bash
   openssl s_client -showcerts -connect api.devnet.solana.com:443 </dev/null
   ```

   The output lists the chain the server sends. Save the root certificate that signs that chain as `rpc-ca.pem`, in PEM form. If you have it in DER form, convert it [UPSTREAM `README.md` troubleshooting]:

   ```bash
   openssl x509 -inform der -in rpc-ca.crt -out rpc-ca.pem
   ```

2. Install it, once per badge:

   ```bash
   tools/badge-push.py --host <badge-ip> --token $T cert rpc-ca.pem --name rpc-ca
   tools/badge-push.py --host <badge-ip> --token $T certs
   ```

   The RPC client looks for a CA named `rpc-ca` [OURS]. Without `--name` the tool stores the certificate under its lower-cased file name, here `rpc-ca.pem` [UPSTREAM `tools/badge-push.py:389`], and the wallet would not find it, so pass the name explicitly. The second command lists the installed certificates; `rpc-ca` must be among them.

3. On the badge: Settings → Wallet, row `RPC`, must read `pinned`.

If RPC calls fail only when `rpc-ca` is installed: whether a pinned TLS handshake checks certificate dates while the badge clock is unset is not known [UNVERIFIED]. The RPC client waits up to 5 s after Wi-Fi connects for the first SNTP sync before its first pinned request. If pinned handshakes still fail with a date error, remove the certificate and run unpinned; the screen then says `RPC TLS not pinned`:

```bash
tools/badge-push.py --host <badge-ip> --token $T rmcert rpc-ca
```

Fallback: leave it unpinned. Everything works; the approval screen says `RPC TLS not pinned`, and the limitation is stated in [../security/security-model.md](../security/security-model.md). Even with the pin, the RPC operator is trusted for attestation data.

If the badge uses a different RPC URL (for example a private endpoint to avoid devnet rate limits), pin the root for that host instead.

## Verifying on the badge

1. Settings → Identity [UPSTREAM screen, one line OURS]: the badge ID, `key lives in  secure element` or `software`, the line `token account  <short address>` directly under that status line, and the full public key.
2. Settings → Wallet [OURS]. The screen has 12 rows, in this order. Compare each with what you pushed:

   | Row | Expect |
   |---|---|
   | `Key  secure element` (green) or `Key  software` (amber) | the same key location as Settings → Identity and `/api/identity` |
   | `Token  HACK  <mint short>` | the first and last four characters of the dashboard's mint; `Token  not configured` when `mint` is unset |
   | `Token account  <short>` | same as on Settings → Identity and as the dashboard's Badges page; `-` when `mint` is unset |
   | `Second confirm above  100.00` | `cap` |
   | `Maximum  1000.00` | `max` |
   | `Registry  <credential short>` | the dashboard's `registry.credential`; `Registry  not configured` when `cred` or `schema` is unset |
   | `RPC  api.devnet.solana.com  pinned` or `not pinned` | the host of `rpc_url`, and the CA pin state |
   | `Presence deadline  400 ms` | `deadline_ms` |
   | `Red blocks signing  on` | `block_red = 1` |
   | `Clock  synced` or `not synced` | SNTP result (see below) |
   | `Audit log >` | the audit view, newest record first; a `CONFIG` line for each change you made |
   | `Forget known names` | SELECT, then confirm, clears the known-names store. Do not use it on a judge badge between the seeding step and the demo |

3. Home: HACK and SOL balances appear within 10 s of boot on Wi-Fi; the merchant badge shows its attested name with `verified` once the attestation is issued on the dashboard's Registry page.
4. From the laptop, the same facts over HTTP:

   ```bash
   curl -s -H "X-Badge-Token: $T" "$B/api/identity"        # pubkey, key_location, token_account
   curl -s -H "X-Badge-Token: $T" "$B/api/wallet/config"   # the stored configuration
   curl -s -H "X-Badge-Token: $T" "$B/api/wallet/audit"    # audit lines
   tools/badge-push.py --host <badge-ip> --token $T logs   # recent log lines
   ```

   Audit line format: `<seq> <uptime_ms> <event> <app_id> <result> <amount_raw> <subject> <identity> <presence> <sig16>`, with `-` for an absent field. For a `CONFIG` line the app field is `push`, `serial` or `ble` and the subject is the key that changed:

   ```
   45 600100 CONFIG push ok - cap - - -
   ```

5. Check that the badge can reach the dashboard listener. From another device on the hotspot:

   ```bash
   curl -i "http://<laptop-ip>:8788/badge/pending?badge=<badge public key>"
   ```

   `204` means reachable and nothing pending. `404` means the key is not in `badges.json`: add it and restart the server.

Clock: the badge has no real-time clock [UPSTREAM `README.md:293-304`]. The fork starts SNTP once when Wi-Fi first connects. `Clock  synced` means the SNTP sync callback has fired and the time is past the threshold; it gates the attestation expiry check, and the RPC client waits up to 5 s for it before its first pinned request [OURS; details in [attestation, Clock](../identity/attestation.md#clock)]. [UNVERIFIED: SNTP through the hotspot. Fallback: with `Clock  not synced` the expiry of an attestation is not evaluated; the dashboard also treats an expired attestation as verified.]

After all four badges are configured, register them with the dashboard, fund them and issue the merchant's attestation: [build-and-flash.md](build-and-flash.md#flashing-four-badges), steps 2 to 5. Then run the [pre-event checklist](pre-event-checklist.md).

## Requirements covered

Configuration that F4 (balances), F10 and F15 (registry addresses, `attest_ttl`), F9 (`deadline_ms`), F12 (`dash_url`) and F14 (`cap`, `max`) depend on. No requirement is implemented here.

## Open items

- [UNVERIFIED] The hotspot lets the badge and the laptop reach each other. Fallback: "Mark rejected" on the dashboard for the attack demo.
- [UNVERIFIED] SNTP works through the hotspot. Fallback: expiry is not evaluated.
- [UNVERIFIED] Which CA root to pin for the RPC host. Fallback: unpinned, shown on screen.
- [UNVERIFIED] `rssi_min = -75` is a usable threshold at table distance. Fallback: tune at the table; -100 disables it.
- [UNVERIFIED] Devnet rate limits with four badges polling. Fallback: Home makes one RPC call per 3 s and backs off on HTTP 429; use another `rpc_url`.
- [UNVERIFIED] Whether a pinned TLS handshake checks certificate validity dates while the badge clock is unset. Fallback: the client waits up to 5 s for SNTP before the first pinned request; if it still fails, `rmcert rpc-ca` and run unpinned (shown on screen).
- [UNVERIFIED] `__has_include("local_config.h")` under the installed core's compiler, which makes the baked-in Wi-Fi optional. Fallback: commit an empty `local_config.h`.
