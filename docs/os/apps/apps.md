# Apps

Every app BadgeOS ships: what it is for, its permissions, its screens and its flow. Lua apps live in `os/apps/<id>/`; native apps in `os/src/native_apps/<id>/`.

## Rules for every Lua app

- `app.ini` lists `permissions=` and `min_api=2` ([app host](../platform/app-host.md#manifest)).
- Anything a person might want to change (a price, a recipient, a step size, a colour) is in the app's own `config.lua`, which returns a table. `main.lua` contains no such literals. Customising an app is editing `config.lua` and pushing again.
- Shared code comes from `require("vk")` ([Lua API](../platform/lua-api.md#libvklua)); an app does not carry its own base64, JSON or RPC code.
- CANCEL (`"b"`) always goes back or exits. No app traps it.
- Amounts are strings. Amount stepping uses the app's own integer count of minor units, formatted with a helper; never a float.
- An app never draws anything that imitates the firmware approval screen.
- Apps are independent: deleting one app's folder breaks no other app.
- Every app draws with `vk.ui` (Lua) or the receipt kit (native), in colours from the active theme. No app hard-codes a colour, draws a stamp, or uses a brand colour of upstream's.
- Every app draws through `vk.ui.frame(draw)`, which calls `draw` only when the screen can have changed ([Lua API](../platform/lua-api.md#drawing-only-when-something-changed)). An app that draws on every pass holds the badge at 20 loop passes a second. Only an animation (the Game's playfield) passes period 0.
- Every app passes `header = "BADGEOS"` from its `config.lua`.
- The launcher and the settings are not apps: they are the BadgeOS shell ([shell](../ui/shell.md)). An app that exits returns to the shell's launcher. Upstream's sample apps (`hello`, `dice`, `gallery`, `radar`, `vumeter`, `whosnear`) are not shipped; the Dice app below is BadgeOS's own.

Where an app is listed comes from its own manifest (`category`, `hidden`; for a native app the last `BADGE_APP` argument), and whether `push-apps.sh release` installs it from `profile` ([app host](../platform/app-host.md#launcher-and-install-keys)). The launcher's top level is Contacts, History, Home, Pay, Request and Inbox, then the folders GAMES (Dice, Duel, Game) and TESTS (Self test, its only app, so SELECT on TESTS opens it).

| App | Id | Kind | Launcher | Permissions |
|---|---|---|---|---|
| Home | `home` | Lua | top level | `net` |
| Pay | `pay` | Lua | top level | `sign,net,espnow` |
| Request | `request` | Lua | top level | `request,net,espnow,history` |
| History | `history` | Lua | top level | `history` |
| Contacts | `contacts` | Lua | top level | `contacts,espnow` |
| Inbox | `inbox` | native | top level, with the waiting count (`count=notes`) | — |
| Dice | `dice` | Lua | GAMES | — |
| Duel | `duel` | Lua | GAMES | `sign,request,net,espnow` |
| Game | `game` | Lua | GAMES | `sign,net,espnow,storage` |
| Self test | `selftest` | native | TESTS | — |
| Wallet | `wallet_settings` | native | hidden: Settings → Wallet opens it | — |
| Evil game | `evilgame` | Lua, dev profile only (`profile=dev`, `include=game`): the attack demo | hidden | `sign,net,espnow,storage` |
| Sign test | `signtest` | Lua, dev profile only: test fixture, needs the laptop's listener | hidden | `sign,net` |
| Check test | `checktest` | Lua, dev profile only (test fixture, WP21): loads its case from `case.lua`, pushed with the app by the test | hidden | `sign,net,history,storage` |
| Library test | `vktest` | Lua, dev profile only (test fixture, WP35) | hidden | `sign,net,espnow` |
| Request test | `reqtest` | Lua, dev profile only (test fixture, WP23): opens a request and logs RESULT frames | hidden | `request,espnow` |
| Native test | `nativetest` | native, dev profile only (test fixture) | hidden | — |

## Audit (2026-10-04)

Every app on the badge was launched with the dev build of that day, its main keys pressed (injected), the screen captured and its log read; one badge, provisioned with the test values, no network, no clock source, no second badge. The rule: nothing visible on the launcher may dead-end, show a placeholder, error, or be unable to do its job on a provisioned badge. An app that needs a second badge or the network is not broken for that, but must say on its screen what it waits for.

| App | Evidence | Decision |
|---|---|---|
| Home | starts (`HOME addr 5vpm..edvj`); name, address, key, `clock not set` in the warning ink, the repository QR code, `no network` under it; the menu lists the other apps | keep, top level. Its menu was a list of ids in `config.lua`; it is now `badge.system.launcher_apps()` |
| Pay | `PAY list 0`, the screen says "No requests nearby" | keep, top level (waits for a badge that asks) |
| Request | SELECT on the amount: `REQ err no_time`, the screen says "The clock is not set (no_time). Join Wi-Fi to set it" | keep, top level |
| History | `HIST n 64`; SELECT shows a record's nine rows | keep, top level |
| Contacts | `CON list 0`; SELECT starts the swap: "Looking for badges nearby / open Contacts, Swap on the other badge" | keep, top level |
| Inbox | "Nothing new"; the count appears on its launcher cell with a note (`t_notify.py`) | keep, top level |
| Dice | SELECT: `DICE roll 1 1 total 2` | keep, GAMES |
| Duel | `DUEL title`; SELECT on a stake: `DUEL not ready no_time`, the screen says "Cannot duel: clock not set." Its header read `DUEL`, not `BADGEOS` | keep, GAMES; fixed: the header comes from `config.lua` |
| Game | play runs (`GAME play`, `GAME score 1`, `GAME over 2`). Its shop paid `shop.recipient = "REPLACE_WITH_SHOP_ADDRESS"`: every purchase ended in "the shop is not set up" | keep, GAMES; fixed: the shop's address is the config key `shop_address`, and while it is empty the title is Play only |
| Self test | runs its 14 checks (`t_selftest.py`) | keep, TESTS |
| Wallet | its five pages open; Settings → Wallet opens it | keep, hidden: it is the wallet's settings and lives in Settings ([shell](../ui/shell.md#wallet-and-inbox)) |
| Evil game | the game's code with a lying config; `evil_recipient = "REPLACE_WITH_ATTACKER_TOKEN_ACCOUNT"` | fixed and hidden: the shop comes from `shop_address`, the "recipient" lie pays the badge's own address, so it works from configuration alone; dev profile only, never on a launcher |
| Sign test | "listener_url not set": does nothing without the laptop's listener | hidden dev fixture |
| Check test | `CT nocase`: idles without the `case.lua` a test pushes | hidden dev fixture |
| Library test | runs its checks (`VT done`); not for people | hidden dev fixture |
| Request test | `RT err no_time`; a fixture of `t_req.py` | hidden dev fixture |
| Hello (C++) `hello_native` | a sample that printed "gm from C++" in upstream's palette | deleted; the native template (`os/templates/native_app/`) replaced it, and the tests' native fixture is `nativetest` (hidden, dev only) |

## Dice

A small game with no payments and no permissions. SELECT rolls `config.count` dice (default 2) with a short animation; UP and DOWN change the number of dice between 1 and `config.max` (default 5); the total is printed as the amount, the last rolls as rows; the LEDs pulse in the theme's LED colour on a roll. It logs `DICE roll <d1> <d2> ... total <n>` when the dice settle and `DICE count <n>` when the number changes. Every tunable is in `config.lua`. Test: `t_app_dice.py`.

## Self test

The badge's one test suite: a native app (id `selftest`, folder `src/native_apps/selftest/`) titled **TESTS**, in the spirit of the factory test firmware. It opens on a menu of suites; each suite is a checklist whose rows end `OK`, `FAIL` or `--` (not tested: it needs a person, or the thing is off or absent); the last menu row is RUN ALL. The Lua fixtures (`signtest`, `checktest`, `vktest`, `reqtest`) stay for the scripted tests and are not part of it.

Keys. Menu: UP/DOWN, SELECT opens a suite (its automatic checks run the first time) or starts RUN ALL, CANCEL exits. Checklist: SELECT on a manual row starts that test, SELECT on an automatic row runs the suite again, CANCEL back to the menu. A manual test has its own keys and CANCEL always ends it. Holding CANCEL is the firmware's force-quit. Every automatic check has a hard timeout (20 s unless its row says otherwise) and one step per frame; the screen is drawn on a change or once a second.

| Suite | Rows (automatic unless marked) |
|---|---|
| HARDWARE (runs when the app opens) | display, buttons, LEDs (manual: colour screens; each key in turn, 20 s each, a short CANCEL gives the key up; each of `RGB_LED_COUNT`), battery, microphone (2 s of samples), I²C bus (line levels, `0x20`, `0x14`, `0x5D`, `0x4A`; **`0x48` is never probed**, see [H21](../architecture/upstream-hooks.md)), storage (a write, read and delete of `/vk/selftest.tmp`), settings store, key present, crypto (RFC 8032 test 2 verifies, corrupted copies do not), clock, Wi-Fi, ESP-NOW, memory |
| WALLET | key location (a software key is reported as "software, in flash"), address is the base58 of `publicKey()`, `maxSignBytes()` matches the key location, the domain self-check (`selfCheckOk()`), RFC 8032 tests 1–3 verify and a one-bit change of each does not, provisioned, issuer key set, token table; **signature** (manual): a harmless test message goes through `vk::wallet::begin`, a person holds SELECT on the approval, and the app verifies the returned signature against the badge's public key. A refusal, a cancel or a timeout is `--` |
| CHECKS | the check chain (`vk_check_solana`) on the host suite's test vectors, one row per case: green, not present, clock unsynced, cannot read, not this badge, unknown token, over limit, no record, forged record, expired, stale record, bad proof. A verdict other than the expected severity, headline, reason and select rule is FAIL. The cases never read the badge's config, key or clock |
| APPROVAL SCREENS (dev profile only) | green, amber, red (manual): a demo confirmation of that look opens (title `Self test`, `DEMO`, "nothing is signed", raised as `VKDEMOAPPROVE` does); after it closes, SELECT "looked right" (OK), LEFT "looked wrong" (FAIL), CANCEL skip. Each demo is written to the history like any confirmation (domain `confirm`) |
| STORES | filesystem free space, the write/read/delete probe, and the history, contacts and consent files: magic, version, entry count within capacity, file size; the consent store's own count agrees. A missing file is OK (`none yet`); a file the store would treat as empty is FAIL |
| RADIO | ESP-NOW on and its channel, presence beacon, one broadcast (`BADGEOS SELFTEST PING`, plain text, not a VK frame) queued by the router, badges heard in 5 s, the Wi-Fi radio's state, ESP-NOW and the joined network on the same channel. The presence exchange is not tested: it needs a second badge |
| NETWORK | `--` unless joined as a station. Address and signal; `GET <listener_url>/health` (404 is `--`: the listener answered without that route); JSON-RPC `getHealth` to `rpc_url`; the time source (SNTP is OK). Each request has its own frame and a 3 s timeout |
| APPS | one row per installed app, Lua and native, in launcher order: every permission is a registered name, a Lua app's `app.ini` parses, its `min_api` is at most the firmware's API version and its entry file exists. The value says what is wrong |

The signature row needs a signing domain for test messages, `selftest` (prefix `selftest:`, button, no permission, at most 34 bytes; its decoder accepts exactly `badgeos self test ` followed by 16 lower-case hex digits and shows an amber hold approval titled `Self test`). It is not in the [domain table](../wallet/signing.md#domain-table) yet: the app looks it up at run time with `findDomain`, and until a feature registers it the row ends `-- needs domain`. STORES reads only the 8-byte header of each store, read-only, through `vk::fileio`, with the paths and formats of [stores](../wallet/stores.md): the stores' headers belong to their features, which a native app may not include.

Serial, each line `[selftest] …`: `start`; for HARDWARE, as before, `<name>=<OK|FAIL|--> <value>` and `done ok=<n> fail=<n> manual=<n>`; for every other suite `<suite>.<name>=<OK|FAIL|--> <value>` and `suite <suite> done ok=<n> fail=<n> manual=<n>`; at the end of RUN ALL `all done ok=<n> fail=<n> manual=<n>`; and the summary line `batt=… mic=… btn=0x.. i2c=… heap=…` every 5 s. `manual` counts every row that is not OK or FAIL. A done line comes when a suite's automatic run ends and again after each of its manual tests; a manual row is logged as `-- manual` the first time its suite runs. Nothing secret is logged.

Adding a check is one entry in its suite's table (`suite_<name>.cpp`: name, label, automatic or manual, profile, function); adding a suite is one file and one line in `suites.cpp`. The runner (`core.cpp`) and the CHECKS cases (`checks_cases.cpp`) are plain C++ with no Arduino include. Test: `t_selftest.py` (every suite, RUN ALL; the signature test's approval, if it opens, is cancelled, never approved). Not yet run on a badge.

## Sign test

The first app, used to reach gate 1 and by the unattended test loop. Dev profile only (`profile=dev`) and hidden (`hidden=1`): without the laptop's listener it can only say `listener_url not set`, so no person is offered it; never pushed to a judge badge.

1. `GET <listener_url>/badge/pending?badge=<wallet.address()>` every 2 s. The reply holds `id` and `messageBase64` ([backend](../integration/backend.md#existing-routes)).
2. `wallet.begin_solana(codec.b64dec(messageBase64))` with no `ctx`. In the dev profile the approval is red "UNVERIFIED RECIPIENT" with the hold-to-sign override.
3. On a signature: `vk.send_tx(wallet.wire_tx(sig, msg))`. The dashboard detects a signed transaction from the chain; nothing is posted. On refusal: `POST <listener_url>/badge/outcome` with `{id, outcome="rejected"}`.
4. Screen: one status line (`waiting`, `approving`, `sent <short sig>`, `refused: <reason>`).

As built (WP13): the listener serves a pending attempt for 90 s, so `signtest` remembers every attempt id it has taken and never opens the same attempt twice. Since WP35 it uses `require("vk")` for JSON and RPC (its own temporary code is gone), draws with `vk.ui`, and sends with `vk.commitment` as the preflight commitment; its log lines are unchanged. Whoever pushes it must add `lib/vk.lua` as `vk.lua` (`push-apps.sh` does; `t_sign_net.py` does). A network call that returns nil shows a status line and retries. Its on-chain path has not run yet (`t_sign_net.py`, deferred: network).

## Check test

A test fixture, not an app for people. On start it requires `case` (a file `case.lua` that the test pushes into the app folder; without it the app logs `CT nocase` and idles). Case fields: `msg_hex` or `transfer` (a table passed to `wallet.build_transfer`), `record_hex`, `sig_hex`, `req_hex`, `delay_ms` (default 1500 in the tests: `common.launch()` must first see the app running with no approval open), `flush_draw` (draw and call `badge.gfx.flush()` every frame), `history` (log the newest `wallet.history` entry), `storage_probe` (try `badge.storage.read` on `"../../vk/history.bin"` and `"/vk/history.bin"`, each inside `pcall`).

It logs with `badge.log`, one line each: `CT tick <n>` once a second from `on_update`; `CT call <ms>` just before `begin_solana` (for measurement M3); `CT begin ok` or `CT begin err <reason>`; `CT poll sig <hex>` or `CT poll err <reason>`; `CT hist <outcome> <amount> <symbol>` or `CT hist none`; `CT storage <ok|fail> <ok|fail>`; `CT case error: …` when `case.lua` exists but does not load. CANCEL exits.

## Library test

`vktest`, a test fixture for `lib/vk.lua` on the badge's own Lua (`t_vk.py`). It runs the JSON table of its `config.lua` (the same table `test_vk.lua` runs on the laptop), the frame helpers and `badge.theme`, then every network helper and the payer flow with no network. It logs `VT json ok`, `VT frames ok`, `VT theme ok <name>`, `VT <helper> nil <message>` for rpc, blockhash, confirm, record, send_tx, report and feed, `VT pay failed <reason>`, and `VT done`. If the badge has a route it logs `VT net up` and skips the posting steps and the payment, and the test fails with a message saying to turn Wi-Fi off.

## Request test

A test fixture. It turns ESP-NOW on, then opens one request per frame from `on_update` (not all inside `on_start`: each open is one signature, and this keeps the badge answering serial between them) for the amounts in its `config.lua`. It logs `RT open <req_id>` or `RT err <reason>`, `RT status <state> <proofs>` every second, and `RT result <status>` for RESULT frames.

## Home

The landing app (set as upstream's autostart app by provisioning, `--autostart home`; without it the badge starts on the shell's launcher).

- Calls `wallet.refresh_balance()` on start and every `balance_poll_s` seconds (the firmware does not poll while an app runs). Shows the badge's display name, short address (first 4 + `..` + last 4), key location, and `wallet.balance()` for the default token; "SETUP NEEDED" when unprovisioned; "clock not set" when `time_ok()` is false.
- A menu of the other apps: every app the launcher lists (`badge.system.launcher_apps()`, folders flattened, hidden apps left out) except Home; SELECT launches with `badge.system.launch(id)`.
- CANCEL exits to the launcher.

As built (WP40):

- The screen is the two-column receipt. Left stub: `BALANCE`, the balance and the symbol, then the barcode; on an unprovisioned badge the label reads `SETUP NEEDED`. Body: rows NAME, ADDRESS, KEY (`secure chip` or `software`, worded in `config.lua`), CLOCK (`clock not set` in the warning ink when `time_ok()` is false), a rule, a four-row scrolling menu, a rule, `THANK YOU FOR HACKING`. There is no INBOX row (the count is not readable from Lua; the launcher and Settings show it). The footer's left text is `SELECT open` when the menu is not empty.
- The menu is `badge.system.launcher_apps()` minus Home, in the launcher's order (no app id is named in Home's files; until 2026-10-04 it was a list in `config.lua`). UP/DOWN move, SELECT launches.
- The balance is fetched after the first frame is on screen and then every `balance_poll_s` seconds (0: once only). It is skipped when unprovisioned or when Wi-Fi is not connected; a failure only changes one line under the barcode, and the next attempt is a whole period later. `wallet.refresh_balance()` blocks: with a route but a node that does not answer it holds the frame for up to about 6 s per attempt (the badge's own hotspot counts as connected).
- Log lines: `HOME addr <short>` once, `HOME balance <ok|reason>` per attempted fetch.
- The repository QR code. When `wallet.config("repo_url")` is not empty, the stub shows `vk.ui.qr(22, 98, 102, link)` in place of the barcode: the amount moves up (label at y = 28 instead of 40) and the line a failed fetch writes goes under the code (y = 204). With the project's link a module is 3 px. The key is read on every frame, so a `VKSET repo_url …` shows without restarting the app. With the key empty, or on a firmware whose kit has no QR code (`vk.ui.qr` returns false), the stub is as before: amount, barcode, line. `t_app_home.py` reads the code back from its screenshot.

## Pay

1. **List.** `wallet.requests()` sorted by `rssi`, strongest first: claimed name, amount, signal bars. Empty: "No requests nearby: open Request on the payee" (`config.empty`). Refreshes every 500 ms. While Wi-Fi is not joined (`badge.wifi.connected()` false) the footer says `No Wi-Fi: Settings > Wi-Fi` (`config.offline_hint`), before any payment is tried.
2. **Pay.** SELECT on a request starts `vk.pay.start{request = entry}`. The screen shows the flow's state (`checking presence`, `fetching record`, `building`, `approve on the firmware screen`, `sending`, `confirming`).
3. **Result.** `done`: "Paid", the short signature, green LEDs; a RESULT frame has been sent to the payee. `failed`: the reason in words from the shared table (`vk.reason_text(flow:reason())`, [lua-api](../platform/lua-api.md#reason-texts)); with no network the words say what to do next (join Wi-Fi in Settings > Wi-Fi, start the dashboard, ...). For a firmware block (`unverified`, `revoked`, `mismatch`, `bad_proof`, `expired`) the app calls `vk.report{...}` so the dashboard shows the refusal; not when the red UNVERIFIED came from a missing registry route, which is shown as `registry_missing` ("the listener has no registry"), not as an impostor.

The payment carries the request's id as its memo: `vk.pay` passes `req_id` to `wallet.build_transfer` ([request memo](../wallet/solana-payments.md#request-memo)). Without it the approval would be red WRONG MEMO and the payee could not match the payment.

The app shows the request's *claimed* name in the list, labelled as a claim. The verified name appears only on the firmware approval.

As built (WP41):

- **CANCEL during a payment.** Before the approval opens (presence, record, blockhash) CANCEL abandons the payment and returns to the list. From `approve` on it returns to the list while the flow finishes in the background (the payee must get its RESULT), and the result screen appears when it ends.
- Signal strength is text in the row's subline (`signal |||.`), thresholds in `config.signal_dbm`.
- The progress and result screen is a single column with no amount stub and no coloured band, so it cannot be taken for the firmware approval. `PAID` is text in the `stamp_ok` colour, `NOT PAID` in `stamp_bad`.
- Log lines: `PAY list <n>`.

## Request

1. **Amount.** UP/DOWN change the amount by `config.step` minor units; LEFT/RIGHT by ten steps. SELECT opens the request.
2. **Waiting.** `wallet.request_open{amount = ...}`; the screen shows the amount, "waiting for payment", seconds left, and `request_status().proofs` as "badges checking: n". The screen is kept awake (`vk.keep_awake`) while a request is open. CANCEL closes the request.
3. **Checking.** A RESULT frame (`vk.result_parse`) for this `req_id` with status 0 goes to a `vk.receive` watch ([lua-api](../platform/lua-api.md#vkreceive)): the transaction it names is fetched (`getTransaction`, every `config.confirm_every_ms` for up to `config.confirm_for_ms` while it is not visible) and the firmware checks that it pays this request (`wallet.verify_payment`: this badge's account, the amount, the token, the request memo, the payer's signature). The stub's label reads `CHECKING`, never "received" or "paid", until that check passes.
4. **Paid.** Only after the check: the stub's label reads `PAID` (in the `stamp_ok` colour), the LEDs go green, the request is closed, and the payment is written to the history (`wallet.record_received`, a `received` row).

As built (WP41, then the payment check):

- **A RESULT never ends the request by itself.** Status 1 or 2 puts "The payer says they cancelled / the payment failed. Still waiting." on the waiting screen; the request stays open. A status-0 RESULT whose transaction is not this payment, failed on chain, or was not found in time puts the reason on the waiting screen in the payee's words (`vk.reason_text(reason, "receive")`), and the request stays open.
- **Several RESULTs.** RESULTs are taken while waiting and while checking; up to three reported transactions are checked in turn and the first that verifies is PAID, so a forged RESULT with a junk signature cannot make the app ignore the real one.
- **No network.** A check that failed for want of network or time leaves the footer `SELECT check again`, which checks the same transactions again.
- **CANCEL while checking asks first** (a real payment may be being checked): the note reads "A payment is being checked. Close the request anyway? ..." and the footer `SELECT keep checking` / `CANCEL close anyway`. A second CANCEL closes the request; the check goes on while the question is up, and a payment that verifies meanwhile still shows PAID. CANCEL while only waiting closes at once.
- Body rows. Amount: `UP / DOWN`, `LEFT / RIGHT`, `SELECT open`. Waiting: `EXPIRES IN`, `BADGES CHECKING n`, `STATUS on air`. Checking: `REPORTED paid`, `REF` (the signature being checked), `VERIFIED not yet`. Paid: `VERIFIED on chain`, `FROM` (the payer's key, from the verified transaction), `HISTORY saved` / `not saved`.
- Permission `history` is for `record_received`. A failure to write the row does not undo PAID (`REQ record failed <why>`).
- Defaults in `config.lua`: `start = 1000` (10.00) and `step = 50` (0.50). Decimals come from `wallet.tokens()`.
- With no clock it logs `REQ err no_time` and stays open. Log lines: `REQ amount <text>`, `REQ open <req_id>`, `REQ err <reason>`, `REQ result <status>`, `REQ confirm pending` / `REQ confirm failed <reason>`, `REQ paid <signature>`, `REQ payer <address>`, `REQ recorded` / `REQ record failed <why>`, `REQ expired`, `REQ closed`.

## History

A scrolling list of `wallet.history(64)`: time, outcome (coloured), amount and symbol, recipient name or short address, app. SELECT on a row shows the full record including the reason and the short signature.

As built (WP42, then the three row kinds):

- **Approval rows** (`kind` `approval`): the name (or the short address, or the domain's label) and the amount with its symbol, coloured by outcome; the subline is `HH:MM · app · outcome`. A record with no amount (a confirmation, which is what `VKDEMOAPPROVE` writes) shows the label `Confirmation` and the outcome word as the coloured value. Detail: nine rows (Time in UTC, Outcome, Reason, Amount, To, Address, App, Domain, Signature short); a payment that answered a request shows `Request <req_id>` in place of Domain.
- **Automatic rows** (`auto`): the domain's label (`Payment request`, `Presence proof`, `Contact card`, `Store registration`, from `config.domain_label`) and `<n> signed` in the faint ink; the subline is `HH:MM · app · automatic`. Detail: Time, Kind, Domain, What, Signatures, Outcome, App, then one row per signature, newest first, as many as fit (`HH:MM:SS` and the first 16 hex characters of its digest).
- **Received rows** (`received`): `From <short payer>` and `+<amount> <symbol>` in the `stamp_ok` ink; the subline is `HH:MM · app · received`. Detail: Time, Kind, Amount, From, Request, App, Signature.
- Reasons are in the shared short words (`vk.reason_text(reason, "short")`).
- **Refresh.** The list is read again every `config.refresh_ms` (10 s) while it shows, when a detail screen is closed, and on SELECT when the list is empty (footer `SELECT reload`). The cursor stays on its record when new ones arrive on top.
- UP/DOWN step between records; CANCEL goes detail → list → launcher. Cancelled and timed-out records use the warning ink (`tone` in `config.lua`; the simulation uses the faint ink). Log line: `HIST n <count>`, on start and whenever a reload finds another count.

## Contacts

- **List.** `wallet.contacts()`: name, short address, date added. RIGHT removes after a confirm line (SELECT removes, CANCEL keeps, other keys do nothing). With no contact yet, the "Swap contacts" row's subline says `no contacts yet: SELECT to swap with a badge`.
- **Swap** (SELECT on "Swap contacts"): the app broadcasts `wallet.contact_hello()` once a second and listens.
  - On a CONTACT_HELLO from another badge (`vk.hello_parse`): show "Swap with `<name>`?"; SELECT sends `wallet.contact_card(hello)` unicast to it.
  - On a CONTACT_CARD: `wallet.contact_accept(card)`; on success show "Saved `<name>`" and pulse green; on failure show the reason.
  - Both badges do both halves, so each ends up with the other's card.
- Names here are labelled "self-named"; they are not verified identities.

As built (WP43): `wallet.contacts()` is oldest first; the list shows newest first with the date as `Oct 3` (blank when `added` is 0). The card frame is matched with `vk.T_CONTACT_CARD`. ESP-NOW is left on when swap mode ends. Refusals are in the shared words (`vk.reason_text(reason, "contact")`). Log lines: `CON list <n>`, `CON removed <short>`, `CON swap on`, `CON swap off`, `CON hello <short>`, `CON card sent <short>`, `CON saved <name>`, `CON accept failed <reason>`.

The badges heard in swap mode are a [`vk.peers`](../platform/lua-api.md#vkpeers) list of `config.max_peers` (4):

- A new badge is never kept waiting for room: the one heard longest ago leaves, except the selected one and one a card was just sent to. Made-up HELLOs from many MACs therefore cannot lock an honest badge out; it is listed again on its next HELLO.
- A HELLO from a listed MAC under another key is ignored: it cannot rename the badge or change the key a card is made for.
- After a card is sent to a badge its HELLO frame is held for `config.card_hold_ms` (10 s), so a HELLO sent as its MAC during the swap cannot change what the card answered. The same key's new nonce is taken after that.

There is no "Pay" action on a contact. A contact's address is a device key that may have no registry record, so a payment to it would be red UNVERIFIED RECIPIENT; it would also need an amount screen and the `sign` and `net` permissions (a new consent). Paying a person goes through their Request.

## Game

A single-player arcade game with a shop, to show that an ordinary app can take payments safely.

- **Play.** A dodge game: the player moves LEFT/RIGHT along the bottom, blocks fall, the score counts seconds survived. Speed rises over time. High score in `badge.storage`.
- **Shop.** From the title screen: items from `config.shop.items` (`{name, price}`); SELECT buys with `vk.pay.start{to = wallet.config("shop_address"), amount = item.price, memo = item.name}`. The approval is amber "VERIFIED - NOT PRESENT" (a shop has no badge present), and shows the real amount and the shop's verified name.
- On `done` the item (an extra life, a colour) is unlocked and stored.

`config.lua`: `shop.address_key` (the name of the config key that holds the shop's address, `shop_address`), `shop.items`, `speed`, `colors`. The address itself is a deployment value, provisioned with `VKSET shop_address <base58>` (an address with a registry record); it is never in the app.

As built (WP44):

- **The title screen is the shop**, as in the simulation: one list with `Play` and one `Buy <item>` row per item, and a separate status screen while a purchase runs. Items: shield and sword (each a life) and green paint (a colour); an owned item cannot be bought again.
- High score and unlocks are in `badge.storage.kv` (keys `best` and `o<id>`, so an item id is at most 6 characters).
- **CANCEL during a purchase:** before the approval opens it drops the purchase; after signing it returns to the title and the flow finishes in the background.
- **The shop is open only on a badge provisioned with `shop_address`** (since 2026-10-04; before, `config.lua` held the placeholder `REPLACE_WITH_SHOP_ADDRESS` and every purchase ended in "the shop is not set up"). With the key empty the title is the Play row alone, so nothing on the screen leads nowhere; `VKSET shop_address` with no value closes the shop again. The app reads the key when it starts.
- Log lines: `GAME shop open` or `GAME shop closed` (on start), `GAME title`, `GAME play`, `GAME score <n>`, `GAME over <n>`, `GAME shop <id> <price>`, `GAME buy failed <reason>`.

## Evil game

The same game with a dishonest shop, for the demo. `apps/evilgame/` contains only `app.ini` and `config.lua`; its `app.ini` says `include=game`, so `scripts/push-apps.sh` copies `apps/game/`'s files except `config.lua` and `app.ini` into it before pushing. It also says `hidden=1` and `profile=dev`: it is never on a launcher and a release push leaves it out; a demo starts it over serial (`vkdev.py run evilgame`) on a badge that has the dev apps.

`config.lua` adds `evil`:

| `evil` | What the game does | What the firmware shows |
|---|---|---|
| `"amount"` | its own screen says "Buy sword: 5.00", but it builds the transfer for `config.evil_amount` (500.00) | the true amount, `500.00 HACK`, and a hold because it is over the cap |
| `"recipient"` | builds the transfer to the badge's own address while passing the real shop's record | red, WRONG RECIPIENT; cannot be signed |

The point of the demo: the game's screen lies, the firmware's cannot.

As built (WP44): both evil cases go through `vk.pay.start`, the same code path as an honest purchase, with one option changed: `amount = config.evil_amount`, or `destination = wallet.address()`. The lie applies to every item. Until 2026-10-04 the destination was `config.evil_recipient`, shipped as the placeholder `REPLACE_WITH_ATTACKER_TOKEN_ACCOUNT`; any account the shop's record does not name gives WRONG RECIPIENT, so the badge's own address (whoever runs the evil game keeps the money) needs no value at all, and the shop comes from `shop_address` as in the Game. Neither demo has run: both need the network and a registry record for the shop.

## Duel

Two badges, one stake.

1. **Invite.** Badge A picks a stake (`config.stakes`) and broadcasts INVITE (app frame type 64: stake string, a random 8-byte game id). Badge B shows "Duel `<name>` for `<stake>`?"; SELECT sends ACCEPT (type 65).
2. **Play.** A reaction round: after a random 2–5 s delay chosen by A and sent as GO (type 66), both screens flash; each player presses SELECT; each badge sends its reaction time in TIME (type 67). Best of `config.rounds`.
3. **Settle.** The winner's badge calls `wallet.request_open{amount = stake}`; the loser's badge finds that request in `wallet.requests()` (matching the winner's key) and runs `vk.pay.start{request = entry}`. This is the full payment flow, so the approval can be green.
4. The winner shows PAID after confirming on chain, or "unpaid" if no confirmed RESULT arrives within `config.settle_timeout_s`.

Limits to state honestly: there is no escrow (the loser can press CANCEL), and reaction times are self-reported (a modified app could lie). Frame types 64–71 are reserved for Duel ([protocol](../protocol/espnow.md#type-registry)).

As built (WP45); the frames are specified in [protocol](../protocol/espnow.md#type-registry):

- INVITE and ACCEPT each carry the sender's public key, because the loser must find "the winner's request" and no other frame names the winner's key. A fifth frame, LEAVE (68), ends the other badge's wait at once when a player quits.
- The opponent's name in "Duel `<name>` for `<stake>`?" comes from `badge.espnow.peers()` by MAC, or is the short address; the screen says it is self-named.
- GO carries the milliseconds still to wait and is resent until the flash; each badge times itself from its own flash. A press before the flash is sent as 65535 and loses the round. A tied round is replayed; after `config.max_rounds` (9) the duel is a draw with no settlement.
- The app refuses to invite or accept on a badge that is unprovisioned or has no clock (`DUEL not ready <reason>`), because `request_open` would fail only after the game.
- The flash is the one screen not drawn with `vk.ui`: the whole screen in the ink colour with `PRESS` in the paper colour (`gfx.clear`, `gfx.text`).
- If the loser's payment fails before the approval (no network, no record), the app itself sends RESULT failed, so the winner does not wait out `settle_timeout_s`; it reads `flow.result_sent` and `flow.sig` for that ([Lua API](../platform/lua-api.md#vkpay)). A forged RESULT can only make the winner show "unpaid" early; PAID needs the on-chain confirmation.
- Two badges that invite each other at the same moment both ignore the other's INVITE and time out to the title.
- The header's left text is `config.header` (`BADGEOS`); it was the literal `DUEL` until 2026-10-04. The rest of Duel's words are still literals in `main.lua`, not in `config.lua`.
- Log lines: `DUEL title`, `DUEL stake <text>`, `DUEL invite <id>`, `DUEL invite timeout`, `DUEL request <req_id>`, `DUEL paying <req_id>`, `DUEL paid`, `DUEL unpaid`.

## Inbox (native)

Reached from the launcher (its cell shows the waiting count) and from Settings → Inbox ([shell](../ui/shell.md#wallet-and-inbox)). Lists `vk::host::notify` notes, newest first: title, body, age. SELECT launches the note's `app_id` and removes the note; RIGHT dismisses; CANCEL exits. Empty: "Nothing new".

As built (WP32): a row's label is the note's title in capitals, its value the age (`now`, `3m`, `2h`, `1d`), its subline the body; five rows are visible. SELECT on a note whose `app_id` is empty or not installed removes the note and stays in the Inbox (no launch into an error screen). The footer reads `SELECT open · RIGHT dismiss` / `CANCEL back`, because nothing else tells the user about RIGHT. It redraws on every key and four times a second.

## Wallet (native)

Reached from Settings → Wallet. It is the wallet's settings, so since 2026-10-04 it is not on the launcher (`hidden=1` in its `BADGE_APP` line); it was a launcher cell before. Read-only pages, LEFT/RIGHT to change page:

1. **Status**: provisioned or not, public key (full, wrapped), key location, clock source, build profile, domain self-check result.
2. **Tokens**: each token's symbol, mint (short), cap, max, balance.
3. **Config**: every config key and value (`VKKEYS` in screen form).
4. **Apps**: native apps and their declared permissions; Lua apps with stored consent.
5. **Reset**: "Reset wallet config" → `vk::config::requestReset()`.

Balances come from `vk::wallet::tokenInfoLookup`; the page shows `--` when the balance feature is absent or the balance is not known (the kit draws an em dash as `?`, so the two hyphens the launcher's balance row uses stand in for it). The lookup answers only for the default token, so every other token's balance shows `--`.

As built (WP36):

- The title is the page's name (`STATUS`, `TOKENS`, `CONFIG`, `APPS`, `RESET`) and the footer's left text is `◂ WALLET n/5 ▸` plus a key hint; LEFT and RIGHT wrap; UP and DOWN scroll one row per press (nine rows per page, no auto-repeat); SELECT acts only on the Reset page.
- Status: `sntp, synced` / `floor, unsynced` / `none` for the clock; key location `se050` reads `secure element`.
- Tokens: four rows per token (`<SYM> MINT` short, `CAP`, `MAX`, `BALANCE`); a cap or max of 0 reads `none`.
- Config: every registered key sorted by name; an empty value reads `(none)`; a value that does not fit beside its key is written in full on sublines. Key names and app ids are not put in capitals: they are identifiers a person types.
- Apps: native apps sorted by id with their declared permissions, then the Lua app ids in the consent store, each with `allowed` (the store keeps a hash, not the list, so the permissions themselves cannot be shown).
- Reset: `requestReset()` returns nothing, so the app looks at `vk::wallet::approval::active()` straight after the call; if no confirmation opened, a row `CONFIRMATION … unavailable` appears.
- It repaints after a key, once a second (every 5 s on Apps), and when a frame arrives more than 0.25 s late: a native app gets no callback when an approval closes over it, and the late frame (hook H8d's `dt`) is how it knows to redraw.

## Native test (fixture)

`src/native_apps/nativetest/nativetest.cpp`, compiled in the dev profile only (`#if VK_PROFILE_DEV`) and hidden. The native app the device tests launch, stop and draw over (`t_native`, `t_shell`, `t_apr`, `t_notify`, `t_pages2`): it fills the screen with two colours a test counts, and SELECT does nothing. It replaced the sample `hello_native`, whose other job, the example to copy, is now `os/templates/native_app/` ([native apps](../platform/native-apps.md#the-smallest-app)).

## Adding an app

`scripts/new-app.sh <id> "<Name>" [--native] [--category <name>]`, then push it (Lua) or reflash (native); removing it is deleting its folder ([extending](../guides/extending.md#add-an-app)). Add a row to the table at the top of this document.
