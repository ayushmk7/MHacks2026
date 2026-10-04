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
- The launcher and the settings are not apps: they are the BadgeOS shell ([shell](../ui/shell.md)). An app that exits returns to the shell's launcher. Upstream's sample apps (`hello`, `dice`, `gallery`, `radar`, `vumeter`, `whosnear`) are not shipped.

| App | Id | Kind | Permissions |
|---|---|---|---|
| Sign test | `signtest` | Lua, dev only | `sign,net` |
| Home | `home` | Lua | `net` |
| Pay | `pay` | Lua | `sign,net,espnow` |
| Request | `request` | Lua | `request,net,espnow` |
| History | `history` | Lua | `history` |
| Contacts | `contacts` | Lua | `contacts,espnow` |
| Game | `game` | Lua | `sign,net,espnow,storage` |
| Evil game | `evilgame` | Lua, demo only | `sign,net,espnow,storage` |
| Duel | `duel` | Lua | `sign,request,net,espnow` |
| Check test | `checktest` | Lua, dev only (test fixture, WP21): loads its case from `case.lua`, pushed with the app by the test | `sign,net,history,storage` |
| Library test | `vktest` | Lua, dev only (test fixture, WP35) | `sign,net,espnow` |
| Request test | `reqtest` | Lua, dev only (test fixture, WP23): opens a request and logs RESULT frames | `request,espnow` |
| Inbox | `inbox` | native | — |
| Wallet | `wallet_settings` | native | — |
| Hello (C++) | `hello_native` | native | — |

## Sign test

The first app, used to reach gate 1 and by the unattended test loop. Dev profile only; never pushed to a judge badge.

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
- A menu of the other installed apps (`badge.system.apps()` filtered by the list in `config.lua`); SELECT launches with `badge.system.launch(id)`.
- CANCEL exits to the launcher.

## Pay

1. **List.** `wallet.requests()` sorted by `rssi`, strongest first: claimed name, amount, signal bars. Empty: "No requests nearby". Refreshes every 500 ms.
2. **Pay.** SELECT on a request starts `vk.pay.start{request = entry}`. The screen shows the flow's state (`checking presence`, `fetching record`, `building`, `approve on the firmware screen`, `sending`, `confirming`).
3. **Result.** `done`: "Paid", the short signature, green LEDs; a RESULT frame has been sent to the payee. `failed`: the reason in words; for a firmware block (`unverified`, `revoked`, `mismatch`, `bad_proof`, `expired`) the app calls `vk.report{...}` so the dashboard shows the refusal.

The app shows the request's *claimed* name in the list, labelled as a claim. The verified name appears only on the firmware approval.

## Request

1. **Amount.** UP/DOWN change the amount by `config.step` minor units; LEFT/RIGHT by ten steps. SELECT opens the request.
2. **Waiting.** `wallet.request_open{amount = ...}`; the screen shows the amount, "waiting for payment", seconds left, and `request_status().proofs` as "badges checking: n". CANCEL closes the request.
3. **Paid.** On a RESULT frame (`vk.result_parse`) for this `req_id` with status 0: `vk.confirm(ref)` until `confirmed` (poll every 2 s, up to 30 s), then the left stub's label reads `PAID` (drawn in the `STAMP_OK` colour) and the LEDs go green. Status 1 or 2: "Payer cancelled" / "Payment failed". A RESULT is never trusted without the on-chain confirmation.

## History

A scrolling list of `wallet.history(64)`: time, outcome (coloured), amount and symbol, recipient name or short address, app. SELECT on a row shows the full record including the reason and the short signature.

## Contacts

- **List.** `wallet.contacts()`: name, short address, date added. RIGHT removes after a confirm line.
- **Swap** (SELECT on "Swap contacts"): the app broadcasts `wallet.contact_hello()` once a second and listens.
  - On a CONTACT_HELLO from another badge (`vk.hello_parse`): show "Swap with `<name>`?"; SELECT sends `wallet.contact_card(hello)` unicast to it.
  - On a CONTACT_CARD: `wallet.contact_accept(card)`; on success show "Saved `<name>`" and pulse green; on failure show the reason.
  - Both badges do both halves, so each ends up with the other's card.
- Names here are labelled "self-named"; they are not verified identities.

## Game

A single-player arcade game with a shop, to show that an ordinary app can take payments safely.

- **Play.** A dodge game: the player moves LEFT/RIGHT along the bottom, blocks fall, the score counts seconds survived. Speed rises over time. High score in `badge.storage`.
- **Shop.** From the title screen: items from `config.shop.items` (`{name, price}`); SELECT buys with `vk.pay.start{to = config.shop.recipient, amount = item.price, memo = item.name}`. The approval is amber "VERIFIED - NOT PRESENT" (a shop has no badge present), and shows the real amount and the shop's verified name.
- On `done` the item (an extra life, a colour) is unlocked and stored.

`config.lua`: `shop.recipient` (address with a registry record), `shop.items`, `speed`, `colors`.

## Evil game

The same game with a dishonest shop, for the demo. `apps/evilgame/` contains only `app.ini` and `config.lua`; `scripts/push-apps.sh` copies `apps/game/*.lua` except `config.lua` into it before pushing.

`config.lua` adds `evil`:

| `evil` | What the game does | What the firmware shows |
|---|---|---|
| `"amount"` | its own screen says "Buy sword: 5.00", but it builds the transfer for `config.evil_amount` (500.00) | the true amount, `500.00 HACK`, and a hold because it is over the cap |
| `"recipient"` | builds the transfer to `config.evil_recipient` while passing the real shop's record | red, WRONG RECIPIENT; cannot be signed |

The point of the demo: the game's screen lies, the firmware's cannot.

## Duel

Two badges, one stake.

1. **Invite.** Badge A picks a stake (`config.stakes`) and broadcasts INVITE (app frame type 64: stake string, a random 8-byte game id). Badge B shows "Duel `<name>` for `<stake>`?"; SELECT sends ACCEPT (type 65).
2. **Play.** A reaction round: after a random 2–5 s delay chosen by A and sent as GO (type 66), both screens flash; each player presses SELECT; each badge sends its reaction time in TIME (type 67). Best of `config.rounds`.
3. **Settle.** The winner's badge calls `wallet.request_open{amount = stake}`; the loser's badge finds that request in `wallet.requests()` (matching the winner's key) and runs `vk.pay.start{request = entry}`. This is the full payment flow, so the approval can be green.
4. The winner shows PAID after confirming on chain, or "unpaid" if no confirmed RESULT arrives within `config.settle_timeout_s`.

Limits to state honestly: there is no escrow (the loser can press CANCEL), and reaction times are self-reported (a modified app could lie). Frame types 64–71 are reserved for Duel ([protocol](../protocol/espnow.md#type-registry)).

## Inbox (native)

Reached from the launcher (its cell shows the waiting count) and from Settings → Inbox ([shell](../ui/shell.md#wallet-and-inbox)). Lists `vk::host::notify` notes, newest first: title, body, age. SELECT launches the note's `app_id` and removes the note; RIGHT dismisses; CANCEL exits. Empty: "Nothing new".

As built (WP32): a row's label is the note's title in capitals, its value the age (`now`, `3m`, `2h`, `1d`), its subline the body; five rows are visible. SELECT on a note whose `app_id` is empty or not installed removes the note and stays in the Inbox (no launch into an error screen). The footer reads `SELECT open · RIGHT dismiss` / `CANCEL back`, because nothing else tells the user about RIGHT. It redraws on every key and four times a second.

## Wallet (native)

Reached from the launcher and from Settings → Wallet. Read-only pages, LEFT/RIGHT to change page:

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

## Hello (native)

The smallest native app ([native apps](../platform/native-apps.md#the-smallest-app)). Kept as the template to copy.

## Adding an app

Lua: a new folder with `app.ini`, `main.lua`, `config.lua`; push it. Native: a new folder with one `.cpp`; reflash. Recipes: [../guides/extending.md](../guides/extending.md#add-a-lua-app). Add a row to the table at the top of this document.
