# Configuration, limits and audit

The wallet's configuration keys and how to set them, the spending cap and maximum, the audit log, and the payment history store.

- Audience: firmware engineers implementing `wallet_config.cpp`, `audit.cpp` and `history.cpp`; operators configuring badges.
- Status: design, not yet built on hardware.
- Base: Solana OS, directory `firmware/solana-os/` at commit `812b8c7` of the [upstream repository](https://github.com/spacemandev-git/solana-defcon-badge-26/tree/812b8c7aca5c366d18c0b040fafd2999f7204d84/firmware/solana-os). Paths are relative to that directory in our fork.

Tags: **[UPSTREAM]** exists at that commit (path given); **[OURS]** our decision (reason given); **[UNVERIFIED]** needs a badge (fallback given). Everything here is [OURS] unless tagged otherwise, and none of it is written yet.

Where the wallet keeps its state:

| Store | Location | Content | Size |
|---|---|---|---|
| Configuration | NVS namespace `wallet` | the keys below, and the boot counter `boots` used by the history store | about 600 B of the 20 KB NVS partition |
| Audit log | LittleFS `/wallet/audit.log`, `/wallet/audit.1` | one text line per event | ≤ 128 KB |
| Payment history | LittleFS `/wallet/history.bin` | 50 fixed records | 8 KB |
| Known names | LittleFS `/wallet/known.bin` | names seen as verified | 2 KB; see [Attestation, Known names](../identity/attestation.md#known-names) |

None of these is reachable by an app: `badge.storage` is confined to `/apps/<id>/` [UPSTREAM `src/lua_sdk/lua_runtime.cpp:404-410`], the push file API is confined to `/apps/` [UPSTREAM `src/net/push_server.cpp`], and app key/value storage lives in a different NVS namespace, `luakv` [UPSTREAM `src/settings.cpp:19`]. The private key is not in any of them; it is in NVS `badgeid` or in the SE050 ([Keys and the SE050](keys-and-se050.md)).

## Configuration keys

NVS namespace `wallet`. NVS key names are limited to 15 characters [UPSTREAM constraint]; all of these fit.

| Key | Type | Default (`wallet_defaults.h`) | Meaning |
|---|---|---|---|
| `rpc_url` | string | `https://api.devnet.solana.com` | JSON-RPC endpoint |
| `mint` | base58 string | `""` (wallet not ready until set) | HACK mint; must equal the dashboard's `HACK_MINT` |
| `decimals` | u8 | `2` | must equal the mint's decimals |
| `symbol` | string ≤ 7 | `HACK` | display only |
| `cred` | base58 string | `""` | SAS credential PDA (dashboard `/api/status` → `registry.credential`) |
| `schema` | base58 string | `""` | SAS schema PDA (`registry.schema`) |
| `cap` | decimal string, raw | `10000` | second-confirmation threshold |
| `max` | decimal string, raw | `100000` | hard limit |
| `deadline_ms` | u16 | `400` | PROOF deadline |
| `attest_ttl` | u16 seconds | `30` | attestation cache freshness |
| `block_red` | u8 | `1` | red disables signing |
| `dash_url` | string | `""` | dashboard badge listener base, e.g. `http://172.20.10.2:8788`; empty disables checkout polling |
| `rssi_min` | i8 | `-75` | REQs weaker than this are not listed [UNVERIFIED threshold] |

Notes on individual keys:

- **`mint`** gates everything. Until it is set, `wallet_ready()` is false, every signing call returns `not_ready`, and Home has nothing to show. The badge derives its own token account as `ATA(public key, mint)` and never asks the network for it.
- **`decimals`** is checked against the `decimals` byte inside every transaction (policy check 8). A wrong value here makes every payment show the blocked screen `Wrong token decimals`.
- **`symbol`** is display text only. The token is identified by `mint`, never by its symbol.
- **`cred` and `schema`** define what "verified" means on this badge: an attestation counts only if it sits under this credential and this schema. They depend on the authority key of the laptop that runs the dashboard, so they are configuration and not constants. Whoever can set them can redefine "verified"; see [Security model](../security/security-model.md#non-goals-and-residual-risks).
- **`cap` and `max`** are raw base units stored as decimal strings: `10000` is 100.00 HACK at 2 decimals. See [Cap and max](#cap-and-max).
- **`deadline_ms`**: the default 400 ms is [UNVERIFIED]. Use 800 if any badge signs with the SE050 and 2500 if the fast Ed25519 backend is not shipped. Set it from measurement M1 ([Measurements](../testing/measurements.md)).
- **`attest_ttl`** is how stale a cached attestation may be before the signing gate re-fetches it. It is the "one refresh" within which a revocation becomes visible.
- **`block_red`**: `1` means a red approval screen cannot be signed. `0` turns red into hold-SELECT-3-s, for demos that want the judge to be the last line of defence.
- **`rssi_min`**: set to `-100` to disable the filter.

All four badges in a demo must agree on `mint`, `decimals`, `cred`, `schema` and `rpc_url`, and those must match the dashboard ([Dashboard integration](../integration/dashboard.md)). A badge with a different `mint` will not list other badges' payment requests at all, because requests carry a 4-byte mint tag.

## How to set

Three ways:

**1. Compile-time defaults.** `src/wallet/wallet_defaults.h` defines `WALLET_DEFAULT_RPC_URL`, `WALLET_DEFAULT_MINT`, `WALLET_DEFAULT_CRED`, `WALLET_DEFAULT_SCHEMA` and `WALLET_DEFAULT_DASH_URL` (devnet URL, empty, empty, empty, empty). A key that is absent from NVS takes its default. Baking the event's values in here lets four badges be flashed with one image and no per-badge step.

**2. HTTP.** `POST /api/wallet/config` with a JSON object containing any subset of the keys. It requires the pairing code in `X-Badge-Token`, like every state-changing route of the push server [UPSTREAM `README.md:773-817`]. `GET /api/wallet/config` reads the values back. (Routes added by patch P11.)

```bash
B=http://<badge-ip>; T=123456        # T is the 6-digit pairing code from Settings -> Push
curl -s -H "X-Badge-Token: $T" -H "Content-Type: application/json" -X POST "$B/api/wallet/config" -d '{
  "mint":"<HACK_MINT>","decimals":2,"cred":"<registry.credential>","schema":"<registry.schema>",
  "rpc_url":"https://api.devnet.solana.com","dash_url":"http://<laptop-ip>:8788"}'
curl -s -H "X-Badge-Token: $T" "$B/api/wallet/config"       # read back
curl -s http://127.0.0.1:8787/api/status                     # on the laptop: where mint, credential, schema come from
```

**3. Serial or BLE line.** `SETWALLET <key> <value>`, after `AUTH`, over the same line protocol the push tools use [UPSTREAM `README.md:826-872`]. It sets one key at a time with the validation rules below and answers `OK <key>` (as upstream's `SETWIFI` answers `OK <field>`), `ERR unknown key` or `ERR bad value`.

Effects of a change:

- Every changed key writes one `CONFIG` line to the [audit log](#audit-log), naming the key and whether it came from `push` (HTTP), `serial` or `ble`.
- Changing `mint`, `cred`, `schema` or `rpc_url` clears the attestation cache and re-derives the badge's token account.
- Values are read back on Settings → Wallet ([Settings](../apps/settings.md)) and with `GET /api/wallet/config`.

The full operator procedure, including Wi-Fi and the RPC certificate pin, is in [Configure](../guides/configure.md).

### Response and validation

`GET /api/wallet/config` (authenticated) returns one JSON object with every key of the table above plus two read-only fields, `token_account` and `ready`:

```json
{"rpc_url":"https://api.devnet.solana.com","mint":"<base58 or empty>","decimals":2,"symbol":"HACK",
 "cred":"<base58 or empty>","schema":"<base58 or empty>","cap":"10000","max":"100000","deadline_ms":400,
 "attest_ttl":30,"block_red":1,"dash_url":"","rssi_min":-75,
 "token_account":"<base58 or empty>","ready":false}
```

`POST /api/wallet/config` is all-or-nothing. If any key is unknown or any value is invalid, nothing is applied and the answer is HTTP 400:

```json
{"error":"bad_config","key":"<first offending key>","message":"<reason>"}
```

On success the answer is HTTP 200 with the same body as GET, and one `CONFIG` audit line is written per changed key.

| Key | Valid values |
|---|---|
| `mint`, `cred`, `schema` | empty, or base58 of exactly 32 bytes |
| `decimals` | 0–9 |
| `symbol` | 1–7 printable ASCII characters |
| `cap`, `max` | decimal strings or JSON integers, with `cap <= max` after the request is applied |
| `deadline_ms` | 50–5000 |
| `attest_ttl` | 5–3600 |
| `block_red` | 0 or 1 |
| `rpc_url` | starts with `http://` or `https://`, at most 95 characters |
| `dash_url` | empty, or starts with `http://`, at most 63 characters |
| `rssi_min` | −100 to −30 |
| `token_account`, `ready` | read-only: either one in a POST is `bad_config` |

Why all-or-nothing [OURS]: `mint`, `decimals`, `cred` and `schema` only make sense together, and a half-applied request would leave a badge that looks configured and is not.

## Cap and max

Two limits on a single payment (requirement F14):

| Limit | Key | Default | Effect when `amount` exceeds it |
|---|---|---|---|
| Cap | `cap` | `10000` raw = **100.00 HACK** | A second screen ([Screen D](screens.md#screen-d)): hold SELECT for 2 s. The payment can still be made. |
| Maximum | `max` | `100000` raw = **1000.00 HACK** | The wallet refuses. Blocked screen `Above the maximum`, error `over_limit`. Nothing is signed. |

Where each is enforced:

| Place | Rule |
|---|---|
| Signing gate, policy check 10 | `amount <= max`, else blocked ([Policy checks](signing-gate.md#policy-checks)) |
| Signing gate, after the approval gesture | `amount > cap` → Screen D |
| Payer, on receiving a payment request | a request with `amount > max` is not listed ([Payment protocol, Rules](../protocol/payment-protocol.md#rules)) |
| Apps | the amount pickers in Pay and Request stop at `cap` (a convenience; the gate is the enforcement) |

Worked examples with the defaults:

| Amount | Raw | Result |
|---|---|---|
| 10.00 HACK (the demo purchase) | `1000` | approval screen only |
| 150.00 HACK | `15000` | approval screen, then Screen D with a 2 s hold |
| 500.00 HACK (the tampered-transaction attack) | `50000` | above the cap and below the maximum, so it **reaches the approval screen**, which is red because the app claimed 5.00; blocked there |
| 1500.00 HACK | `150000` | blocked screen `Above the maximum` |

Why two limits [OURS]: the cap is a speed bump for honest mistakes (one more deliberate gesture); the maximum is a wall that no gesture gets past. The defaults are chosen so the 10.00 demo purchase is quick and the 500.00 attack is shown to the judge on the approval screen instead of being silently refused by a limit.

Judges cannot change either value on the badge: there is no on-device editor. Both are configuration pushed by the team with the pairing code. To disable the second confirmation, set `cap` equal to `max`.

The comparison is on raw integer units decoded from the transaction bytes. No amount is ever held as a float.

## Audit log

`audit.cpp`. Every wallet decision leaves a line.

- **File:** `/wallet/audit.log` on LittleFS. Append-only text, one line per event.
- **Rotation:** at 64 KB the file is renamed to `/wallet/audit.1` (one generation kept) and a new file is started.
- **Mirror:** each line is also written to `badge_log` with tag `wallet`, so it appears on the serial console, on Settings → Console and at `GET /api/logs` [UPSTREAM `src/badge_log.h`].
- **Flushing:** `wallet_update()` flushes pending lines once per main-loop tick.
- **Read back:** Settings → Wallet → Audit (last 20 lines); `GET /api/wallet/audit` (authenticated, patch P11).
- **Not reachable** from Lua or from the push file API (see the table at the top).

### Line format

Space-separated fields, `-` for an absent field:

```
<seq> <uptime_ms> <event> <app_id> <result> <amount_raw> <subject> <identity> <presence> <sig16>
```

| Field | Content |
|---|---|
| `seq` | line sequence number |
| `uptime_ms` | `millis()` when the event was recorded (the badge has no real-time clock) |
| `event` | `TX`, `REQ`, `PROOF`, `RCPT`, `BROKER` or `CONFIG` |
| `app_id` | the app that was running and asked; for `CONFIG` it is `push`, `serial` or `ble` |
| `result` | `ok`, or a Lua error string from [Error codes](../reference/error-codes.md) (`rejected`, `blocked`, `unknown_instruction`, …) |
| `amount_raw` | raw base units, decimal |
| `subject` | `TX`: the recipient wallet in base58, or `ta:` followed by the token account in base58 when the recipient wallet is unknown. `PROOF` and `RCPT`: the payer's public key. `CONFIG`: the configuration key that changed. Otherwise `-`. |
| `identity` | the identity state shown: `verified`, `unverified`, `mismatch`, `revoked`, `expired` or `unknown`, or `-` |
| `presence` | the presence state shown: `present`, `not_checked` or `not_present`, or `-` |
| `sig16` | the first 16 base58 characters of the signature, or `-` |

Events:

| Event | Written when |
|---|---|
| `TX` | `wallet_sign_transaction` returns, with any result: signed, rejected, timed out, blocked, refused by the decoder |
| `REQ` | a payment request is signed on the payee |
| `PROOF` | a proof of presence is signed |
| `RCPT` | a receipt is signed |
| `BROKER` | the broker registration string is signed (only with `WALLET_ENABLE_BROKER`) |
| `CONFIG` | a configuration key changes |

The per-app lockout described in [Rate limits](signing-gate.md#rate-limits) also writes a line when it starts.

Example lines (a signed payment, a blocked red screen, a signed request, a proof of presence, a configuration change):

```
41 512044 TX pay ok 1000 Gn2GQYmcQkatE2594ov2ELKkYWZHwARG7FiAoPWSEcxq verified present 5VERv8NMvzbJMEkV
42 530911 TX checkout blocked 50000 FZEAS6Nayu6nMX1RVmoNwoPA4tu1KNM5pvfoXsQzei3K mismatch not_checked -
43 541200 REQ request ok 1000 - - - 3kXw9qYt2LmNp8Rs
44 541930 PROOF request ok - 4vJDQvQxRrfLYpjFWXFsVTMsw7cayvWB46pnzdn2BW97 - - 2bFh7Jq1ZxCv9TnW
45 600100 CONFIG push ok - cap - - -
```

The declarations, `src/wallet/audit.h` in full. File: [`../reference/code/sdk-headers/wallet/audit.h`](../reference/code/sdk-headers/wallet/audit.h). Syntax-checked only; `audit.cpp` is not written.

```c
/* src/wallet/audit.h - append-only audit log (/wallet/audit.log, rotated to /wallet/audit.1 at 64 KB).
   Every line is also written to badge_log with tag "wallet". */
#ifndef AUDIT_H
#define AUDIT_H
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif

#define AUDIT_LINE_MAX 160

void audit_begin(void);

/* Writes one line:
     <seq> <uptime_ms> <event> <app_id> <result> <amount_raw> <subject> <identity> <presence> <sig16>
   event:    TX REQ PROOF RCPT BROKER CONFIG
   app_id:   calling app id; "push", "serial" or "ble" for CONFIG; "-" when no app is involved
   result:   "ok" or a badge_err_t name
   subject:  TX: recipient wallet base58, or "ta:" + token account base58 when the wallet is unknown;
             PROOF, RCPT: payer public key base58; CONFIG: the config key that changed; else "-"
   identity: verified unverified mismatch revoked expired unknown, or "-"
   presence: present not_checked not_present, or "-"
   sig16:    first 16 base58 characters of the signature, or "-"
   Pass NULL for any field that is absent; it is written as "-". */
void audit_log(const char *event, const char *app_id, const char *result, const char *amount_raw,
               const char *subject, const char *identity, const char *presence, const char *sig16);

/* Copies the newest max_lines lines (oldest first, each ending in '\n') into out, NUL-terminated.
   Returns the number of lines copied. Used by Settings > Wallet > Audit and GET /api/wallet/audit. */
size_t audit_tail(char *out, size_t cap, size_t max_lines);

#ifdef __cplusplus
}
#endif
#endif
```

`audit_tail()` is what Settings → Wallet → Audit and `GET /api/wallet/audit` read.

Why a log at all [OURS]: the gate signs on behalf of apps that may crash, lie or be removed. The audit log is the wallet's own account of what it was asked and what it did, written by code the app cannot influence. It is also the evidence trail for the attack demos: a blocked attempt is recorded even though nothing reached the chain.

## History store

`history.cpp`. The payment history shown by the History app (requirement F16) is owned by firmware so that it survives app crashes and cannot be forged by one app to mislead another.

- **File:** `/wallet/history.bin`, a ring of **50 records of 160 bytes** (8000 bytes).
- **Record:** 160 bytes, little-endian, fixed offsets. It is written field by field, not as a C struct.

| Offset | Size | Field | Content |
|---|---|---|---|
| 0 | 1 | `dir` | `0` out, `1` in |
| 1 | 1 | `status` | `0` signed, `1` submitted, `2` confirmed, `3` failed |
| 2 | 1 | `flags` | bit 0 `verified` (the counterparty was verified at the time), bit 1 `has_sig` (`sig` is filled), bit 2 `receipt` (a co-signed receipt was verified, F19) |
| 3 | 1 | `name_len` | length of `name`, 0–32 |
| 4 | 32 | `peer` | counterparty public key |
| 36 | 32 | `name` | counterparty name at the time, zero padded |
| 68 | 8 | `amount` | `u64`, raw base units |
| 76 | 64 | `sig` | transaction signature (zero when `has_sig` is clear) |
| 140 | 4 | `uptime_s` | `u32`, seconds since boot when written |
| 144 | 2 | `boot_count` | `u16`, the value of NVS `wallet`/`boots` at that boot |
| 146 | 4 | `seq` | `u32`, starts at 1 and is never reused; `0` marks an empty slot |
| 150 | 10 | reserved | zero |

- **Ring index:** the slot of a record is `(seq − 1) % 50`. The newest record is the one with the highest `seq`, found by scanning the 50 slots at boot.
- **Boot counter:** `boot_count` is the NVS value `wallet`/`boots` (`u16`), incremented by `history_begin()` at every boot.

The declarations, `src/wallet/history.h` in full. File: [`../reference/code/sdk-headers/wallet/history.h`](../reference/code/sdk-headers/wallet/history.h). Syntax-checked only; `history.cpp` is not written.

```c
/* src/wallet/history.h - payment history ring (/wallet/history.bin, 50 records of 160 bytes = 8000 bytes). */
#ifndef HISTORY_H
#define HISTORY_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "wallet.h"
#ifdef __cplusplus
extern "C" {
#endif

#define HISTORY_RECORDS     50
#define HISTORY_RECORD_LEN 160

/* On-disk record, little-endian, fixed offsets (written field by field, not as a C struct):
     0    1  dir         0 out, 1 in
     1    1  status      0 signed, 1 submitted, 2 confirmed, 3 failed
     2    1  flags       bit0 verified, bit1 has_sig, bit2 receipt (F19 co-signed)
     3    1  name_len    0..32
     4   32  peer        counterparty public key
     36  32  name        zero padded
     68   8  amount      u64 LE, raw units
     76  64  sig         transaction signature (zero when has_sig is clear)
     140  4  uptime_s    u32 LE, seconds since boot when written
     144  2  boot_count  u16 LE, value of NVS wallet/boots at that boot
     146  4  seq         u32 LE, 1-based, never reused; 0 marks an empty slot
     150 10  reserved    zero
   Slot of a record = (seq - 1) % HISTORY_RECORDS. The newest record is the one with the highest seq,
   found by scanning all 50 slots at boot. */
#define HISTORY_F_VERIFIED 0x01
#define HISTORY_F_HAS_SIG  0x02
#define HISTORY_F_RECEIPT  0x04

typedef struct {
  uint8_t  dir, status, flags;
  char     name[33];
  uint8_t  peer[32];
  uint64_t amount;
  uint8_t  sig[64];
  uint32_t uptime_s;
  uint16_t boot_count;
  uint32_t seq;
} history_record_t;

void        history_begin(void);                                    /* also increments NVS wallet/boots */
uint16_t    history_boot_count(void);
badge_err_t history_append(const history_record_t *r);              /* seq, uptime_s, boot_count are filled in */
size_t      history_list(history_record_t *out, size_t max);        /* newest first */
badge_err_t history_set_status(const uint8_t sig[64], uint8_t status);   /* bad_arg if no record has that signature */
badge_err_t history_set_receipt(const uint8_t sig[64]);

#ifdef __cplusplus
}
#endif
#endif
```

- **Who writes:**
  - The wallet core appends an `out` / `signed` record for **every transaction it signs**, inside `wallet_sign_transaction`, before returning to the app. An app cannot pay without leaving a record, even if it crashes immediately after signing.
  - Apps holding the `wallet` permission update a record's status with `history.mark(signature, status)` as the transaction is submitted and confirmed, and add incoming payments with `history.add{dir="in", …}` ([API reference](../app-platform/api-reference.md#badgehistory)).
  - When the payer's wallet receives a receipt (RCPT), it verifies it against the payee key and the record's transaction signature and sets the `receipt` flag (`history_set_receipt`). History then shows `co-signed` in place of `confirmed` ([Receipts](../apps/receipts.md)).
- **Time:** the badge has no real-time clock [UPSTREAM `README.md:293-304`], so a record carries `boot_count` and `uptime_s`. The History app shows `<n> s ago` (uptime now − `uptime_s`) only for a record written during the current boot, and `earlier` for any other; `history.list` reports this as `this_boot`.
- **Capacity:** the 51st record overwrites the oldest.

The History app is described in [History](../apps/history.md). Acceptance check T-F16: after several payments and a power cycle, History lists them.

## Requirements covered

- **F14**: spending cap with a second confirmation, and a hard maximum ([Cap and max](#cap-and-max)).
- **F16**: payment history on the badge; this document specifies the store, the History app document specifies the screen.
- **F15** (supporting): `attest_ttl` is the refresh interval within which a revocation shows.
- **F19** (supporting): the `receipt` flag of the history record.
- **F9** (supporting): `deadline_ms`.
- Non-functional "Judge UX: amounts capped".
- Security guarantee "every signature leaves a record" (audit log and history).

## Open items

- [UNVERIFIED] `deadline_ms` default of 400 ms. Fallback: 800 ms with an SE050 signer, 2500 ms with TweetNaCl.
- [UNVERIFIED] `rssi_min` default of −75. Fallback: tune at the table; −100 disables the filter.
- [UNVERIFIED] LittleFS append cost for one audit line per event, and wear from rewriting `history.bin`. Fallback: buffer audit lines and flush every few seconds; the data volumes (≤ 128 KB, 8 KB) are small against the 9.5 MB partition.
