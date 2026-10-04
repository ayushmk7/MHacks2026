# Reason codes and glossary

## Reason codes

One list for every refusal in BadgeOS. In C it is `vk_reason_t` (`src/vk/wallet/pure/vk_reason.h`); in Lua it is the lower-case string returned as the second value of `nil, reason`; in the history store it is the numeric value. The order is fixed: new codes are added at the end.

| # | C | Lua string | Meaning | Where it appears |
|---|---|---|---|---|
| 0 | `VK_OK` | `ok` | no error | — |
| 1 | `VK_CANCELLED` | `cancelled` | the user pressed CANCEL on a green or amber approval | `poll` |
| 2 | `VK_TIMEOUT` | `timeout` | a green or amber approval was not answered in `approval_tmo_s`; a balance fetch had no route to the node or did not complete | `poll`; `refresh_balance` |
| 3 | `VK_UNDECODABLE` | `undecodable` | the bytes are not a payment the badge can read, the token is unknown, or account 0 is not this badge; a fetched transaction is not a one-signature payment the decoder accepts | `poll` (red approval); `verify_payment`, `record_received` |
| 4 | `VK_UNVERIFIED` | `unverified` | no record, a record whose issuer signature fails, or a request that does not verify | `poll` (red); `vk.record` on 404 |
| 5 | `VK_REVOKED` | `revoked` | the record says revoked | `poll` (red) |
| 6 | `VK_EXPIRED` | `expired` | the record is past its expiry or older than `record_ttl_s`; a contact nonce is no longer current | `poll` (red); `contact_accept` |
| 7 | `VK_MISMATCH` | `mismatch` | the decoded recipient or amount differs from the record or the request; a request payment without that request's memo; a contact card made for another badge; a fetched transaction that is not the expected payment (signature, payer, token, account, amount or memo) | `poll` (red); `contact_accept`; `verify_payment`, `record_received` |
| 8 | `VK_BAD_PROOF` | `bad_proof` | a valid presence proof was made by a key other than the record's (an invalid presence proof is ignored, never this reason: [protocol](../protocol/espnow.md#payer-challenging-and-judging)); a contact card signature is invalid; a fetched transaction's signature does not verify over its message | `poll` (red); `contact_accept`; `verify_payment`, `record_received` |
| 9 | `VK_OVER_CAP` | `over_cap` | the amount is above the token's `max` | `poll` (red) |
| 10 | `VK_NO_TIME` | `no_time` | the clock has no trusted source | `request_open` |
| 11 | `VK_BUSY` | `busy` | an approval is open or a result is waiting to be polled; two requests are already open | `begin`; `request_open` |
| 12 | `VK_DENIED` | `denied` | the app lacks the permission | `begin` (native apps; Lua apps get a Lua error instead) |
| 13 | `VK_NOT_PROVISIONED` | `not_provisioned` | the badge has not been provisioned; a balance fetch has no token table, `rpc_url` or key | `begin`; `request_open`; `refresh_balance` |
| 14 | `VK_TOO_LONG` | `too_long` | the bytes exceed the domain's limit or what the key can sign (242 bytes with an SE050 key) | `begin` |
| 15 | `VK_SIGN_FAILED` | `sign_failed` | the key refused, the badge has no key, or the domain table failed its self-check | `poll`; `request_open`; `challenge`; `contact_hello`; `contact_card` |
| 16 | `VK_BAD_ARG` | `bad_arg` | an argument has the right type but an invalid value | any function |
| 17 | `VK_UNSUPPORTED` | `unsupported` | the domain or feature is not in this firmware; a default could not be resolved; the node's reply to a balance fetch held no usable account or was not status 200 | `begin`; `build_transfer`; `refresh_balance`; `contact_accept` (the card is valid but the contacts file could not be written); `verify_payment`, `record_received` (no token table, or this badge's token account not known yet; `record_received` also when the history file could not be written) |
| 18 | `VK_IDLE` | `idle` | `poll` was called with nothing begun | `poll` |
| 19 | `VK_OVER_DAILY` | `over_daily` | the payment would take the token's total signed in the last 24 hours over `day_limit`, or that total could not be read, or `day_limit` does not parse ([checks](../wallet/checks.md#daily-limit)) | `poll` (red) |
| 20 | `VK_LOW_BATTERY` | `low_battery` | the measured battery is at the critical level (`batt_crit_pct`, [shell](../ui/shell.md)); no new approval is opened until it recovers or the badge is on USB power | `begin` |

Adding a code: append it to `vk_reason_t` and to the name table in `vk_reason.c` (same position), bump `VK_REASON_COUNT`, add the row here, the name to `test_reason_names` in `test/host/test_checks.c`, and the code to every place listed under "Where it appears" (for a red approval: the `poll` reasons in [lua-api](../platform/lua-api.md#badgewallet-payments) and the headline table below). Apps that show a reason keep their own text tables (Pay, Request, Duel, Game, Evil game, History, Contacts); a code an app does not know is shown raw until it is added there.

Rules: a red approval always reports its own cause (`red_reason`), however it was closed. An app decides what to tell the user from the reason; `vk.report` forwards `unverified`, `revoked`, `expired`, `mismatch` and `bad_proof` to the dashboard feed.

Decoder errors (`sol_tx_err_t`, [solana-payments](../wallet/solana-payments.md#decoder-rules)) are logged as `[pay] undecodable: <name>` and all surface to apps as `undecodable`. The two `undecodable` causes that are not decoder errors (account 0 is not this badge; UNKNOWN TOKEN) log nothing.

## Approval headlines

| Headline | Colour | Reason if red |
|---|---|---|
| `VERIFIED - PRESENT` | green | — |
| `VERIFIED - NOT PRESENT` | amber | — |
| `CLOCK UNSYNCED` | amber | — |
| `CANNOT READ PAYMENT` | red | `undecodable` |
| `UNKNOWN TOKEN` | red | `undecodable` |
| `OVER LIMIT` | red | `over_cap` |
| `DAILY LIMIT` | red | `over_daily` |
| `UNVERIFIED RECIPIENT` | red | `unverified` |
| `REVOKED` | red | `revoked` |
| `EXPIRED`, `STALE RECORD` | red | `expired` |
| `WRONG RECIPIENT`, `WRONG AMOUNT`, `WRONG MEMO` | red | `mismatch` |
| `BAD REQUEST` | red | `unverified` |
| `BAD PROOF` | red | `bad_proof` |
| `NEW PERMISSIONS`, `SECURITY SETTING`, `ERASE WALLET CONFIG` | amber | — (confirmations) |

## Glossary

| Term | Meaning |
|---|---|
| Approval | the firmware's own full-screen prompt on which the user accepts or rejects; the only place a button-domain signature can come from |
| App host | the layer that launches apps, enforces permissions, routes frames and holds notifications |
| ATA | associated token account: the account that holds one owner's balance of one token |
| Auto domain | a signing domain that signs without a button press; its bytes are always built by firmware |
| Button domain | a signing domain that signs only after SELECT on the approval |
| Check chain | the pure function that turns message + record + request + presence + clock into a verdict |
| Config key | a provisioned setting stored in NVS |
| Consent | the user's one-time approval of an app's sensitive permissions |
| Dev profile | the build with test hooks and the hold-to-sign override; never flashed on a judge badge |
| Feature | a self-contained folder under `src/vk/features/` |
| BadgeOS | this firmware: a fork of Solana OS with its own shell, wallet core and app platform. One word; `BADGEOS` in a screen header |
| Hook | a single marked line in an upstream file that calls into `src/vk/` or changes one upstream value |
| Issuer | the key that signs registry records; the backend's registry authority |
| Listener | the backend's badge-facing HTTP port on the hotspot |
| Presence | proof, by a fresh signed answer within a deadline, that the payee's key holder is in range now |
| Provisioned | all required config keys are set and committed |
| Record | the issuer-signed statement of who a device key belongs to and which token account is theirs |
| Registry (code) | a self-registering list of rows of one kind (domains, routes, services, settings pages, ...) |
| Replaced file | an upstream file the fork deletes, rewrites or edits as a whole; listed in [upstream-hooks](../architecture/upstream-hooks.md#replaced-upstream-files) |
| Request (REQ) | a payee's signed "pay me" frame |
| Screen name | the shell's current screen as `VKSTATE.screen` reports it (`launcher`, `settings`, `wifi`, ...); empty while an app runs |
| Settings page | one row of the Settings list and, usually, the screen it opens; one file under `src/vk/shell/pages/`, registered with `VK_SETTINGS_PAGE` or `VK_SETTINGS_ACTION` |
| Severity | green, amber or red |
| Shell | BadgeOS's own user interface outside apps and the approval: boot screen, launcher, settings, dialogs (`src/vk/shell/`, [shell](../ui/shell.md)). It replaces upstream's shell |
| Upstream | Solana OS at commit `812b8c7`, by spacemandev: the firmware BadgeOS is forked from |
| Verdict | the output of the check chain: severity, select rule, headline, reason |
