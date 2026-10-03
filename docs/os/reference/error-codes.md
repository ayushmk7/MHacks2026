# Error codes

One table of every error the badge API can return, the decoder's detail names, and where the user sees each one.

- Audience: app authors (Lua and C++) and firmware engineers.
- Status: design, not yet built on hardware.
- Base: Solana OS, directory `firmware/solana-os/` at commit `812b8c7` of the [upstream repository](https://github.com/spacemandev-git/solana-defcon-badge-26/tree/812b8c7aca5c366d18c0b040fafd2999f7204d84/firmware/solana-os). Upstream has no shared error type: its Lua bindings return `nil, "reason"` with free-text reasons [UPSTREAM `README.md:1314-1348`]. Everything on this page is [OURS] and applies to the functions we add.

There is one error type for the whole badge API. `badge_err_t` is an `int32_t`.

- **Lua** receives the string name as the second return value: `nil, "rejected"`. `badge.identity.sign` and `badge.identity.decode` add a third value, the decoder detail: `nil, "unknown_instruction", "ix_data"`. `badge.rpc.send` adds the node's message (its first 96 characters) after `rpc`.
- **C and C++** receive the number. `badge_err_name(e)` returns the Lua string for a number.
- The values are defined in `src/app_host/badge_api.h` ([`code/sdk-headers/app_host/badge_api.h`](code/sdk-headers/app_host/badge_api.h)); `src/wallet/wallet.h` uses the same type.

## badge_err_t

| Value | C name | Lua string | Meaning |
|---|---|---|---|
| 0 | `BADGE_OK` | — | success |
| 1 | `BADGE_ERR_BAD_ARG` | `bad_arg` | malformed argument (wrong length, not base58, …) |
| 2 | `BADGE_ERR_DENIED` | `denied` | the calling app lacks the permission |
| 3 | `BADGE_ERR_NOT_READY` | `not_ready` | identity or wallet config missing |
| 4 | `BADGE_ERR_BUSY` | `busy` | another wallet prompt is active, or (`pay.request`, `pay.receive`) a session is already open |
| 5 | `BADGE_ERR_RATE_LIMITED` | `rate_limited` | a rate limit applies; see [Rate limits](../wallet-core/signing-gate.md#rate-limits) |
| 6 | `BADGE_ERR_NO_NETWORK` | `no_network` | no Wi-Fi and no bridge |
| 7 | `BADGE_ERR_TIMEOUT` | `timeout` | network timeout |
| 8 | `BADGE_ERR_IO` | `io` | transport or storage failure |
| 9 | `BADGE_ERR_RPC` | `rpc` | JSON-RPC returned an `error` object |
| 10 | `BADGE_ERR_PARSE` | `parse` | response did not parse |
| 11 | `BADGE_ERR_NO_ACCOUNT` | `no_account` | account does not exist on chain. Returned only by `rpc.token_owner`; a token account that does not exist reads as a balance of `0` in `rpc.token_balance`, not as an error. |
| 12 | `BADGE_ERR_TOO_LONG` | `too_long` | input exceeds a fixed limit |
| 13 | `BADGE_ERR_NO_MEMORY` | `no_memory` | allocation failed or app heap cap reached |
| 20 | `BADGE_ERR_UNKNOWN_INSTRUCTION` | `unknown_instruction` | decoder rejected the message (detail = `sol_tx_err_name`) |
| 21 | `BADGE_ERR_WRONG_SIGNER` | `wrong_signer` | fee payer/authority is not this badge |
| 22 | `BADGE_ERR_UNKNOWN_MINT` | `unknown_mint` | mint is not the configured mint |
| 23 | `BADGE_ERR_DECIMALS` | `decimals` | instruction decimals differ from config |
| 24 | `BADGE_ERR_BAD_SOURCE` | `bad_source` | source is not this badge's ATA |
| 25 | `BADGE_ERR_OVER_LIMIT` | `over_limit` | amount > `max` |
| 26 | `BADGE_ERR_BLOCKED` | `blocked` | red state and `block_red`: returned when the red approval screen is closed with CANCEL or by the 60 s limit, because signing was never possible |
| 27 | `BADGE_ERR_REJECTED` | `rejected` | CANCEL pressed on a prompt that could have been approved |
| 28 | `BADGE_ERR_APPROVAL_TIMEOUT` | `approval_timeout` | no decision within the call's 60 s budget |
| 29 | `BADGE_ERR_SIGN_FAILED` | `sign_failed` | key backend failed, or the produced signature did not verify |
| 30 | `BADGE_ERR_NO_DISPLAY` | `no_display` | framebuffer or buttons unavailable; the wallet refuses rather than sign blind |
| 31 | `BADGE_ERR_NO_SESSION` | `no_session` | PROOF or RCPT asked for with no matching open session; for `pay.receipt`, no session in state `paid` |

Values 14–19 are unused. Values 1–13 are general (arguments, permissions, network, storage); values 20–31 come from the wallet core.

The same list as C, from `badge_api.h`:

```c
typedef int32_t badge_err_t;
enum { BADGE_OK = 0, BADGE_ERR_BAD_ARG = 1, BADGE_ERR_DENIED = 2, BADGE_ERR_NOT_READY = 3, BADGE_ERR_BUSY = 4,
       BADGE_ERR_RATE_LIMITED = 5, BADGE_ERR_NO_NETWORK = 6, BADGE_ERR_TIMEOUT = 7, BADGE_ERR_IO = 8,
       BADGE_ERR_RPC = 9, BADGE_ERR_PARSE = 10, BADGE_ERR_NO_ACCOUNT = 11, BADGE_ERR_TOO_LONG = 12,
       BADGE_ERR_NO_MEMORY = 13, BADGE_ERR_UNKNOWN_INSTRUCTION = 20, BADGE_ERR_WRONG_SIGNER = 21,
       BADGE_ERR_UNKNOWN_MINT = 22, BADGE_ERR_DECIMALS = 23, BADGE_ERR_BAD_SOURCE = 24, BADGE_ERR_OVER_LIMIT = 25,
       BADGE_ERR_BLOCKED = 26, BADGE_ERR_REJECTED = 27, BADGE_ERR_APPROVAL_TIMEOUT = 28,
       BADGE_ERR_SIGN_FAILED = 29, BADGE_ERR_NO_DISPLAY = 30, BADGE_ERR_NO_SESSION = 31 };
```

## Decoder error names

When the error is `unknown_instruction`, the detail says which decoder rule failed. These are the strings returned by `sol_tx_err_name()` ([`code/sol_tx.c`](code/sol_tx.c)); the rules are explained in [Transaction decoder](../wallet-core/transaction-decoder.md#rules).

| `sol_tx_err_t` | Name | The message … |
|---|---|---|
| `SOL_TX_OK` (0) | `ok` | was accepted |
| `SOL_TX_ERR_TOO_LONG` (1) | `too_long` | is longer than 256 bytes |
| `SOL_TX_ERR_TRUNCATED` (2) | `truncated` | ended before a required field |
| `SOL_TX_ERR_VERSION` (3) | `version` | is a versioned message with a version other than 0 |
| `SOL_TX_ERR_HEADER` (4) | `header` | does not have exactly 1 required signature, or has read-only signers |
| `SOL_TX_ERR_ACCOUNTS` (5) | `accounts` | does not have exactly 5 account keys with 2 read-only unsigned |
| `SOL_TX_ERR_LOOKUPS` (6) | `lookups` | is v0 and uses address lookup tables |
| `SOL_TX_ERR_IX_COUNT` (7) | `ix_count` | does not have exactly one instruction |
| `SOL_TX_ERR_PROGRAM` (8) | `program` | calls a program other than the SPL Token program |
| `SOL_TX_ERR_IX_ACCOUNTS` (9) | `ix_accounts` | does not pass exactly 4 instruction accounts, or an index is out of range |
| `SOL_TX_ERR_IX_DATA` (10) | `ix_data` | has instruction data that is not 10 bytes starting with 12 (`TransferChecked`) |
| `SOL_TX_ERR_AUTHORITY` (11) | `authority` | has an authority that is not account 0 (the signer and fee payer) |
| `SOL_TX_ERR_ROLES` (12) | `roles` | has source/destination that are not the two writable accounts, mint/program that are not the two read-only ones, or one key in two roles |
| `SOL_TX_ERR_AMOUNT_ZERO` (13) | `amount_zero` | transfers an amount of 0 |
| `SOL_TX_ERR_TRAILING` (14) | `trailing` | has bytes left over after the instruction |

Note the two different `too_long` values: the decoder detail `too_long` (message over 256 bytes, reported as `unknown_instruction`) and the wallet error `too_long` (code 12, a message the decoder accepted but the key backend cannot take).

## Where each is shown

"Shown" means a screen drawn by the wallet core before the call returns. Screens are specified in [Screens](../wallet-core/screens.md). For everything else the app receives the error and decides what to tell the user; each app's document lists its messages.

| Error | Wallet screen | Notes |
|---|---|---|
| `bad_arg`, `no_network`, `timeout`, `io`, `rpc`, `parse`, `no_account`, `no_memory` | none | returned by the API call that failed |
| `denied` | none | returned before anything is drawn. Upstream bindings that we guard with a permission check raise a Lua error naming the permission instead ([App platform overview](../app-platform/overview.md)). |
| `not_ready` | none | the mint is not configured or the identity failed; see Settings → Wallet |
| `busy` | none | checked before anything else; not counted by the rate limiter |
| `rate_limited` | none | the per-app lockout also writes an audit line |
| `too_long` | [Screen E](../wallet-core/screens.md#screen-e), headline `Transaction too large`, when it comes from the signing gate | no screen when it comes from another function's length check |
| `unknown_instruction` | Screen E, headline `Unknown instruction`, `reason:` = decoder name | |
| `wrong_signer` | Screen E, headline `Not your account` | |
| `unknown_mint` | Screen E, headline `Unknown token` | |
| `decimals` | Screen E, headline `Wrong token decimals` | |
| `bad_source` | Screen E, headline `Not your token account` | |
| `over_limit` | Screen E, headline `Above the maximum` | |
| `blocked` | [Screen C](../wallet-core/screens.md#screen-c): title `DO NOT PAY`, footer `BLOCKED` / `CANCEL to close` | returned after CANCEL or when the call's 60 s budget ends |
| `rejected` | the screen on which CANCEL was pressed (A, B, D, F, G or H, and C only when `block_red = 0`) | CANCEL on a blocked Screen C returns `blocked` |
| `approval_timeout` | the prompt that was on screen when the call's 60 s budget ended | one budget covers Screens H, A–C and D of a signing call |
| `sign_failed` | Screen E, headline `Signing failed`, body `The key did not produce a valid signature.` | happens after the approval gesture |
| `no_display` | none | never drawn: the call returns at once and logs `[wallet] no_display: refusing to sign` |
| `no_session` | none | |

Shown on Screen E before the call returns: codes 20–25, `too_long` (12) when policy check 11 raises it, and `sign_failed` (29). `blocked` (26) is returned after the red Screen C. `no_display` (30) is never drawn: the call returns at once and logs `[wallet] no_display: refusing to sign`.

Screen E closes on CANCEL or after 15 s; the call then returns the error. Every outcome of a signing call, shown or not, is written to the audit log ([Config, limits and audit](../wallet-core/config-limits-audit.md#audit-log)).

## Requirements covered

- **F1**: the errors `identity.sign` can return.
- **F3**: `unknown_instruction` and the decoder detail names.
- **F14**: `over_limit`.
- Supports every app document's "error states" section.

## Open items

- [UNVERIFIED] None of these codes has been returned by running firmware; the enum has passed a syntax-only compile.
