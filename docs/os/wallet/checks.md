# Checks: who is being paid, and can we tell

How the wallet core decides between green, amber and red for a payment. Files: `src/vk/wallet/pure/vk_record.{h,c}`, `vk_checks.{h,c}` (C99, host-tested), `src/vk/core/clock.{h,cpp}`, and the glue in `src/vk/features/solana_pay/domain_solana.cpp`.

Principle: **no trust decision comes from the app.** The app supplies bytes (the message, a registry record, a payment request). The firmware verifies every one of them itself, and shows the amount and token only from the decoded message and the recipient's name only from a record whose issuer signature it checked.

## Inputs

| Input | From | Verified by |
|---|---|---|
| Message bytes | the app | the decoder ([solana-payments.md](solana-payments.md)) |
| Registry record + issuer signature (`ctx.record`, `ctx.record_sig`) | the app, which fetched it from the backend | Ed25519 against the provisioned `issuer_key` |
| Payment request (`ctx.req`), the full REQ frame | the app, which got it from `wallet.requests()` or `on_espnow` | Ed25519 against `record.device_pubkey` |
| Presence result | the firmware's own presence table, keyed by `req_id` | made by the firmware ([protocol](../protocol/espnow.md#presence)) |
| Clock | the clock service | SNTP, or the floor (below) |
| Token table, caps | provisioned config | — |

## Registry record

Issued by the backend for one device key, signed by the issuer key. Canonical bytes: UTF-8, `key=value` lines separated by `\n`, **exactly this order**, no trailing newline, at most 512 bytes:

```
v=1
attestation=<base58 address, or "none">
display_name=<1..32 printable ASCII>
device_pubkey=<64 lower-case hex>
kind=merchant|person
solana_wallet=<64 hex, or empty>
solana_ata=<64 hex, or empty>
bank_ref_hash=<64 hex, or empty>
expiry=<unix seconds>
status=active|revoked
issued_at=<unix seconds>
```

Signed bytes: `registry:` followed by the canonical bytes. `registry:` is reserved: the badge never signs it, and the domain self-check rejects any domain that tries to use it.

```c
/* src/vk/wallet/pure/vk_record.h */
typedef struct {
  char     attestation[45];
  char     display_name[33];
  uint8_t  device_pubkey[32];
  uint8_t  kind;                 /* 1 merchant, 2 person */
  uint8_t  has_solana;           /* solana_wallet and solana_ata present */
  uint8_t  solana_wallet[32];
  uint8_t  solana_ata[32];       /* the recipient's token account for the payment token */
  uint8_t  has_bank;
  uint8_t  bank_ref_hash[32];
  uint32_t expiry;
  uint8_t  status;               /* 1 active, 2 revoked */
  uint32_t issued_at;
} vk_record_t;

/* 0 on success. Strict: wrong key, wrong order, extra line, bad hex, bad number, or non-printable name fails. */
int vk_record_parse(const uint8_t *bytes, size_t len, vk_record_t *out);

/* 1 if sig is a valid signature by issuer_key over "registry:" || bytes. */
int vk_record_verify(const uint8_t *bytes, size_t len, const uint8_t sig[64], const uint8_t issuer_key[32],
                     int (*verify)(const uint8_t *, size_t, const uint8_t *, const uint8_t *));
```

What "strict" means where the format above does not say (fixed by `vk_record.c` and `test_record`):

- A number is 1 to 10 decimal digits with no sign and no leading zero, and must fit in 32 bits.
- `attestation` is `none` or a base58 string that decodes to 32 bytes.
- `solana_wallet` and `solana_ata` come as a pair. A record with only one of them parses with `has_solana = 0`, so check 10 gives WRONG RECIPIENT.
- The parser alone cannot catch a record cut short inside its last number (`issued_at` with digits removed is still well formed); the issuer signature does.

`vk_record.h` also defines `VK_RECORD_MAX` (512), `VK_RECORD_PREFIX` (`"registry:"`), `VK_KIND_MERCHANT` / `VK_KIND_PERSON` and `VK_STATUS_ACTIVE` / `VK_STATUS_REVOKED`.

The record carries `solana_ata` so the badge compares 32 bytes instead of deriving an address. One record covers one token (the first row of the token table); a second token needs the backend to issue a record per token, which is out of scope until a second token exists.

## Clock

The badge has no real-time clock. Until something sets it, the system time is 1970 plus uptime (upstream seeds it from the build date only on its WPA2-Enterprise path, finding F3). The clock service never trusts the raw system time; it tracks where the time came from:

```cpp
// src/vk/core/clock.h
namespace vk::clock {
enum class Source : uint8_t { NONE, FLOOR, SNTP };
Source source();
uint32_t now();                 // unix seconds; meaningful only when source() != NONE
bool ok();                      // source() != NONE
void raiseTo(uint32_t unix_s);  // from a verified record's issued_at: if unix_s > now(), set the clock; NONE becomes FLOOR
#if VK_TEST_HOOKS
void devSet(uint32_t unix_s);   // dev profile only, for the VKTIME command: stores the time and sets the source to SNTP
#endif
}
```

- **SNTP**: when Wi-Fi first connects, the service calls `configTime(0, 0, <ntp_server>)` and marks `SNTP` from the sync callback (`sntp_set_time_sync_notification_cb`). [UNVERIFIED] that the callback exists in the installed Arduino core; fallback: poll `sntp_get_sync_status() == SNTP_SYNC_STATUS_COMPLETED` once a second.
- **FLOOR**: with no SNTP, a record whose issuer signature verified raises the clock to its `issued_at`. The clock never goes backwards.
- Time is never taken from an unsigned source (an HTTP header, an app).

What each source allows:

| Source | Expiry check | Freshness check (`record_ttl_s`) | Best possible state |
|---|---|---|---|
| `SNTP` | yes | yes | green |
| `FLOOR` | yes | no (the clock was just set from this record) | amber, "CLOCK UNSYNCED" |
| `NONE` | — | — | cannot occur once a record has verified (the record raises the clock to `FLOOR`). With `NONE`, `request_open` refuses with `no_time` |

Limit to state honestly: under `FLOOR`, a record captured before a revocation and replayed to a badge that has never synced is accepted as amber. Revocation needs SNTP to be reliable.

## Presence lookup

```c
/* vk_checks.h */
typedef enum { VK_PRESENCE_NONE, VK_PRESENCE_PENDING, VK_PRESENCE_PRESENT, VK_PRESENCE_LATE, VK_PRESENCE_BAD_SIG } vk_presence_t;
```

```cpp
// src/vk/wallet/signer.h
namespace vk::wallet {
// Set by the requests feature at boot; null when that feature is absent. Returns the stored result for
// req_id; for any result other than NONE it also writes the payee key the slot holds and the nonce sent.
extern vk_presence_t (*presenceLookup)(const uint8_t req_id[8], uint8_t payee_pubkey_out[32], uint8_t nonce_out[16]);
}
```

## The check chain

One pure function. It has no side effects and no access to hardware, so every branch is host-tested.

```c
/* vk_checks.h */
typedef enum { VK_TIME_NONE, VK_TIME_FLOOR, VK_TIME_SNTP } vk_time_source_t;
typedef enum { VK_SEV_GREEN, VK_SEV_AMBER, VK_SEV_RED } vk_severity_t;
typedef enum { VK_SEL_PRESS, VK_SEL_HOLD, VK_SEL_DISABLED } vk_select_t;
typedef enum {
  VK_HL_VERIFIED_PRESENT, VK_HL_NOT_PRESENT, VK_HL_CLOCK_UNSYNCED,
  VK_HL_CANNOT_READ, VK_HL_UNKNOWN_TOKEN, VK_HL_UNVERIFIED, VK_HL_REVOKED, VK_HL_EXPIRED, VK_HL_STALE,
  VK_HL_WRONG_RECIPIENT, VK_HL_WRONG_AMOUNT, VK_HL_BAD_REQUEST, VK_HL_BAD_PROOF,
  VK_HL_OVER_LIMIT
} vk_headline_t;
const char *vk_headline_text(vk_headline_t h);

typedef struct {
  const uint8_t *msg;        size_t msg_len;
  const uint8_t *record;     size_t record_len;   const uint8_t *record_sig;   /* NULL when absent */
  const uint8_t *req;        size_t req_len;                                   /* NULL when absent */
  const uint8_t *own_pubkey; const uint8_t *issuer_key;
  const vk_token_t *tokens;  size_t token_count;
  uint32_t now;              vk_time_source_t time_source;   uint32_t record_ttl_s;
  int (*verify)(const uint8_t *msg, size_t len, const uint8_t *sig, const uint8_t *pubkey);   /* 1 = valid */
  vk_presence_t (*presence)(const uint8_t req_id[8], uint8_t payee_pubkey_out[32], uint8_t nonce_out[16]);   /* may be NULL: presence is then NONE */
} vk_check_input_t;

typedef struct {
  vk_severity_t severity;  vk_select_t select;  vk_reason_t reason;  vk_headline_t headline;
  int dev_overridable;
  int decoded;       sol_transfer_t transfer;   const vk_token_t *token;   /* valid when decoded */
  sol_tx_err_t tx_err;                                                     /* the decoder's result, for the log */
  int record_ok;     vk_record_t record;                                   /* valid when record_ok */
  int req_ok;        vk_req_t req;                                         /* vk_frames.h; valid when req_ok */
  vk_presence_t presence;
} vk_verdict_t;

void vk_check_solana(const vk_check_input_t *in, vk_verdict_t *out);
```

Checks run in this order. The first one that fails decides the verdict; a red verdict always has `select = VK_SEL_DISABLED`.

| # | Check | On failure: headline (text) | Reason | Dev-overridable |
|---|---|---|---|---|
| 1 | message decodes ([rules](solana-payments.md#decoder-rules)) and account 0 is `own_pubkey` | `CANNOT_READ` ("CANNOT READ PAYMENT") | `undecodable` | no |
| 2 | the mint is in the token table and the decimals match | `UNKNOWN_TOKEN` ("UNKNOWN TOKEN") | `undecodable` | no |
| 3 | amount ≤ token `max` (when `max` ≠ 0) | `OVER_LIMIT` ("OVER LIMIT") | `over_cap` | no |
| 4 | a record and its signature were supplied | `UNVERIFIED` ("UNVERIFIED RECIPIENT") | `unverified` | **yes** |
| 5 | the record parses and the issuer signature is valid | `UNVERIFIED` | `unverified` | no |
| 6 | `status` is active | `REVOKED` ("REVOKED") | `revoked` | no |
| 8 | `expiry` > `now` | `EXPIRED` ("EXPIRED") | `expired` | no |
| 9 | under SNTP only: `now − issued_at` ≤ `record_ttl_s` (and `issued_at` ≤ `now` + 60) | `STALE` ("STALE RECORD") | `expired` | no |
| 10 | the record has `solana_ata` and it equals the decoded destination | `WRONG_RECIPIENT` ("WRONG RECIPIENT") | `mismatch` | no |
| 11 | if a request was supplied (whether or not the `requests` feature is present): it parses, its `pay-req:` signature verifies with `record.device_pubkey`, `req.payee_pubkey == record.device_pubkey`, `req.rail` is Solana, `req.expiry` > `now` | `BAD_REQUEST` ("BAD REQUEST") | `unverified` | no |
| 12 | if a request was supplied: `req.currency` equals the token symbol and `req.amount` equals the decoded amount | `WRONG_AMOUNT` ("WRONG AMOUNT") | `mismatch` | no |
| 13 | if a request was supplied and the presence result is `PRESENT`, `LATE` or `BAD_SIG`: the result is not `BAD_SIG`, and the payee key stored with it equals `record.device_pubkey`. (`NONE` has no slot and `PENDING` has no proof to judge; both pass this check and become amber below. The stored key is compared only for `PRESENT`, `LATE` and `BAD_SIG`, although the lookup writes it for `PENDING` too) | `BAD_PROOF` ("BAD PROOF") | `bad_proof` | no |

Terms in the table, as `vk_checks.c` implements them:

- A record counts as **supplied** only if `record`, `record_len` and `record_sig` are all non-zero; a request only if `req` and `req_len` are.
- `decoded` is set to 1 only after checks 1 and 2 have both passed, because "valid when decoded" covers `transfer` and `token` together. On UNKNOWN TOKEN `decoded` is 0 and the `big` line stays empty.
- `VK_TIME_NONE` is treated like `VK_TIME_FLOOR`.
- The REQ signature is verified over `"pay-req:"` followed by `frame[0..signed_len)`, so the requests feature must sign exactly those bytes ([protocol](../protocol/espnow.md#codec)).

Check numbers are kept stable; there is no check 7. The record's `time_source` can never be NONE here, because the caller raised the clock from the verified record first ([Verdict to screen](#verdict-to-screen)).

If nothing failed, the verdict is the **first** row of this table that applies:

| Order | Condition | Severity | Headline | Select |
|---|---|---|---|---|
| 1 | `time_source` is FLOOR | amber | `CLOCK_UNSYNCED` ("CLOCK UNSYNCED") | hold |
| 2 | no request supplied; or presence is `NONE`, `PENDING` or `LATE`; or `presence` is NULL | amber | `NOT_PRESENT` ("VERIFIED - NOT PRESENT") | hold |
| 3 | otherwise | green | `VERIFIED_PRESENT` ("VERIFIED - PRESENT") | press |

Then the cap: if the amount is above the token's `cap` (when `cap` ≠ 0), `select` becomes hold regardless of colour. Amber is always hold. Green under the cap is the only single-press state.

Why record-only payments are allowed (amber, not red): a shop inside a game has no payee badge in the room. The recipient is still issuer-verified and the amount still comes from the bytes; what is missing is proof that the merchant is physically here, and the screen says exactly that.

## Verdict to screen

`decodeSolana` (the `solana` domain's decoder) does, in order:

1. If a record and signature were supplied and `vk_record_verify` passes, call `clock::raiseTo(record.issued_at)`.
2. Fill `vk_check_input_t` from the config (issuer key, token table, `record_ttl_s`), the clock, `crypto::verify`, `presenceLookup`; call `vk_check_solana`.
3. Build the `ApprovalRequest` ([approval](approval.md#the-request)):

| Field | Value |
|---|---|
| `title` | `Pay` |
| `headline` | `vk_headline_text(verdict.headline)` |
| `big` | `<amount> <symbol>` from the decoded bytes and the token table; empty if not decoded |
| `sub` | `to <record.display_name>` when the record is valid; else `to unverified recipient`; empty if not decoded |
| line `Account` | the destination token account, base58, shortened to first 4 + `..` + last 4 |
| line `Kind` | `merchant` or `person` (only with a valid record) |
| line `Requested` | the request's amount and currency (only for `WRONG_AMOUNT`) |
| line `Expected` | the record's token account, shortened (only for `WRONG_RECIPIENT`) |
| line `Memo` | first 35 characters of the memo (only when present) |
| line `Limit` | `over <cap> <symbol>` (only when the cap forced a hold). The verdict has no flag for this; `decodeSolana` recomputes it as `token->cap != 0 && transfer.amount > token->cap` |
| `severity`, `select`, `red_reason`, `dev_overridable` | from the verdict |
| `recipient`, `recipient_name` | `record.device_pubkey`, `record.display_name` when valid; else the destination and empty |
| `amount`, `decimals`, `symbol` | from the decoded bytes and the token table |

At most four lines are shown; when more apply, the order of priority is `Requested`, `Expected`, `Limit`, `Account`, `Kind`, `Memo`.

4. Return `VK_OK`, which opens the approval (a red one included). The only immediate refusals are those made by `begin()` itself ([signing](signing.md#the-signing-path)).

## Bank rail

Optional, built last (work package WP54). Domain `bank`, prefix `bank-auth:`, button, permission `sign`. The bytes are UTF-8 `key=value` lines separated by `\n`, exactly this order, no trailing newline:

```
v=1
rail=nessie
action=purchase|transfer
amount_cents=<decimal>
currency=USD
from_acct=<payer account id>
payee_name=<record.display_name>
payee_ref=<64 hex, record.bank_ref_hash>
attestation=<record.attestation>
req_id=<16 hex>
proof_nonce=<32 hex>
issued_at=<unix seconds>
```

With its prefix the payload is about 360 bytes, so **the bank rail cannot be signed by an SE050-held key** (242-byte limit); `begin` returns `too_long` on such a badge. Its decoder runs checks 4–9 and 11 as above (with `req.rail` bank), then: `payee_ref == record.bank_ref_hash`; `payee_name == record.display_name`; `action` is `purchase` for a merchant and `transfer` for a person; `req_id` and `amount_cents` equal the request's; `proof_nonce` equals the nonce the firmware stored for that `req_id` (the third output of `presenceLookup`). Amber (not present) is **red** on this rail, because the backend requires a proof. The amount is shown as dollars and cents from `amount_cents`.

## Tests

Host suite `test_checks`: a fixed issuer key pair and device key pair (test vectors, generated by `vectors.mjs`), a valid record, a valid request, and one test per row of both tables above: each failure produces exactly its headline and reason; each amber condition; the cap; green. Plus `test_record`: the parser accepts the canonical record and refuses each mutated copy (reordered key, extra line, upper-case hex, trailing newline, 33-character name, non-numeric expiry).

Device: T-CHK1 to T-CHK9 in [../testing/testing.md](../testing/testing.md#acceptance-tests).
