# BadgeOS documentation

The complete specification for the badge firmware. **Build the OS from these documents and nothing else.** If something you need is missing or wrong here, fix this folder first, then the code.

## What BadgeOS is

Firmware for the ESP32-S3 badge, built as a fork of Solana OS. From upstream it keeps the Lua app runtime, the hardware layer, Wi-Fi, ESP-NOW, BLE, app push, the app-store client and the device key. The user interface as a whole is BadgeOS's own, in the Receipt design, and nothing a user can see or a network can hear names upstream. What we add:

- a **shell**: boot screen, launcher, settings and dialogs in the Receipt layout ([ui/shell.md](ui/shell.md));
- a **wallet core**: the only code that can sign with the badge key;
- an **approval engine**: a firmware screen that apps cannot draw over or skip, on which the user approves each payment;
- **verification**: the firmware itself checks who is being paid (an issuer-signed registry record), that they asked for exactly this (a signed request), and that they are here now (a presence proof);
- an **app platform**: Lua apps and native C++ apps, permissions with first-run consent, an ESP-NOW router, notifications;
- **apps**: Home, Pay, Request, History, Contacts, a game with a shop, a duel with a stake, and system apps.

Credit: BadgeOS is built on Solana OS by spacemandev; we added the shell, the wallet, identity and app-platform layers. The credit lives in the repository (`os/README.md` carries the same line) and is never shown on the device.

## The one rule

> An app can ask for a signature. Only the wallet core can produce one. Every signature belongs to one row of the signing-domain table. A row marked "button" signs only after the user presses SELECT on the wallet core's own screen, while the app that asked is not running.

## Easy to add, easy to remove

This is a design requirement, not a hope. It holds because:

- **Every extensible list registers itself.** Signing domains, config keys, Lua functions, permissions, ESP-NOW routes, serial commands, services, LED patterns, themes, settings pages and native apps are each one macro line in the file of the feature that owns them. There is no central table to edit ([how](architecture/overview.md#6-self-registration)).
- **Features are folders.** Delete `src/vk/features/<name>/` and the feature is gone, with nothing left behind to clean up.
- **Trusted mechanisms are generic.** A new thing to sign is a table row and a decoder; a new thing to approve is a filled struct. The signer, the approval engine and the screen are never edited for it.
- **Nothing about a deployment is in the source.** Keys, tokens, URLs and limits are provisioned settings; a new token is one `VKSET` command.
- **Apps keep their own knobs in `config.lua`.**
- **Upstream is touched only by marked one-line hooks, or by files listed as replaced** ([upstream-hooks](architecture/upstream-hooks.md#replaced-upstream-files)), so every difference from upstream is written down.

Recipes for every kind of addition and removal: [guides/extending.md](guides/extending.md).

## Status

As of 2026-10-04, on one development badge (software key, no network, no second badge).

- **BadgeOS runs on the badge with its own shell.** Upstream's launcher, settings screens, dialogs and splash are gone; the boot screen, the launcher, the settings list with its thirteen rows, the delete confirmation and the app-error screen are BadgeOS's, in the Receipt design, light and dark ([shell](ui/shell.md)). Nothing a user or the network can see names upstream; the check is `scripts/check-names.py` plus `strings` on the image ([names](architecture/upstream-hooks.md#checking-the-hooks)). `os/` is upstream plus hooks H1 to H13, H16, H17, H19, H21, H23 and H24 and the files listed as replaced.
- **Two profiles build.** Dev 1,991,895 bytes, release 1,983,131 bytes (the slot is 3,342,336). The release build has none of the dev commands (T-REL2 passed). The badge carries the dev build.
- **Everything that one badge can show passes.** Fourteen host suites and 25 scripted device tests ([testing](testing/testing.md)): boot, config and provisioning, the approval engine and its screen in both themes, the transfer decoder and every refusal vector, the record checks (T-CHK2 to T-CHK9), signing with the badge key (the signature verifies on the laptop), history, permissions and consent, native apps, notifications, contacts on one badge, every shell screen in both themes, and every shipped app up to the point where it needs a peer or the network. Every app returns to the launcher on CANCEL and on the force-quit hold.
- **No payment has been made.** Nothing that needs a second badge or the network has run: presence, a green approval, a transaction on chain, the balance, the registry, the feed, a contact swap, a duel, the game's shop and the evil-game demos. Gate 1 (a transaction confirmed on devnet), Gate 2's green payee, Gate 4 (the demo script) and the release gate are **not met**. The list, with what each item needs, is [Deferred verification](roadmap/implementation-plan.md#deferred-verification).
- **Values a person must fill in before a demo** (two placeholders in the game configs, `dashboard/.env`, the hotspot credentials): [build guide](guides/build-flash-provision.md#before-a-demo).
- **The badge answers at once.** An idle loop pass is about 1 ms on every screen; a key press is on the glass in about 50 ms (one draw and one 34 ms transfer of the canvas). The exception is the Game while playing, 22 frames a second ([measurements](testing/testing.md#responsiveness)).
- **Not built:** the bank rail (WP54, cut).
- The development badge's key is a software key (`5vpmgLuC…`; the full key is in the [tracking notes](roadmap/implementation-plan.md#tracking)). Its SE050 refused the applet select in WP00 and is now never addressed on the I²C bus (hook H21, finding F17), so the badge behaves as one without a secure element. **Its hardware buttons work**: after one power cycle the I²C bus was healthy at the Batch 2 flash (`[btn] TCA9534 init ok`, heartbeat `btn=0`, real key presses in the log) and has stayed so through every flash and test run since, the Batch 5 regression included. The tests still inject buttons over serial.
- Statements about upstream were verified by reading its source at commit `812b8c7`. Statements marked `[UNVERIFIED]` need hardware and always name a fallback.
- The starting code for the pure modules is kept unchanged in [`reference/code/`](reference/code/): the Solana message decoder and builder, base58, SHA-256, their test and vectors. The firmware's copies in `os/src/vk/wallet/pure/` have since been changed as [solana-payments](wallet/solana-payments.md) says. From `docs/os/reference/code/`:
  ```bash
  cc -std=c99 -Wall -Wextra -Wpedantic -O2 -DSOL_HOST_SHA256 \
     sol_b58.c sol_curve.c sol_pda.c sol_tx.c sol_sha256.c test_sol.c -o /tmp/test_sol && /tmp/test_sol
  # all sol tests passed
  ```
- The look is decided: the **Receipt** design in light and dark mode ([ui/ui.md](ui/ui.md#theme)); a live simulation is in `docs/design/os-mockups/`.

## Documents

| Document | Read it for |
|---|---|
| [architecture/overview.md](architecture/overview.md) | layers, trust, source tree, main loop, registries, features, compile switches. **Read first** |
| [architecture/upstream-baseline.md](architecture/upstream-baseline.md) | what Solana OS provides and the exact names to call |
| [architecture/upstream-hooks.md](architecture/upstream-hooks.md) | every edit to an upstream file: the hooks, the replaced files, the names |
| [wallet/signing.md](wallet/signing.md) | the key, the single signing path, the domain table, reason codes |
| [wallet/approval.md](wallet/approval.md) | the approval engine: request struct, state machine, screen |
| [wallet/solana-payments.md](wallet/solana-payments.md) | which transactions are accepted; decoder, builder, token table |
| [wallet/checks.md](wallet/checks.md) | registry record, clock, the check chain, green/amber/red, cap |
| [wallet/stores.md](wallet/stores.md) | history, contacts and consent files |
| [protocol/espnow.md](protocol/espnow.md) | frames between badges, the router, requests, presence |
| [platform/app-host.md](platform/app-host.md) | manifest, permissions, consent, native runtime, notifications |
| [platform/lua-api.md](platform/lua-api.md) | every Lua function we add; `lib/vk.lua` |
| [platform/native-apps.md](platform/native-apps.md) | writing a C++ app |
| [platform/config.md](platform/config.md) | config keys, provisioning, USB serial commands |
| [ui/ui.md](ui/ui.md) | LED patterns, boot bar, balance, the Receipt theme (light and dark), the receipt kit, the header rule |
| [ui/shell.md](ui/shell.md) | the BadgeOS shell: boot, launcher, every settings page, dialogs; the settings-page registry |
| [apps/apps.md](apps/apps.md) | every shipped app |
| [integration/backend.md](integration/backend.md) | what the badge needs from the laptop |
| [guides/build-flash-provision.md](guides/build-flash-provision.md) | toolchain, build profiles, flashing, four badges |
| [guides/extending.md](guides/extending.md) | how to add or remove anything |
| [testing/testing.md](testing/testing.md) | host tests, dev hooks, acceptance tests, measurements |
| [roadmap/implementation-plan.md](roadmap/implementation-plan.md) | the work packages, in order, with gates |
| [reference/reasons.md](reference/reasons.md) | reason codes, headlines, glossary |
| [reference/upstream-readme.md](reference/upstream-readme.md) | upstream's README: the upstream Lua API and push protocol |
| [reference/differences-from-specs.md](reference/differences-from-specs.md) | what changed from `docs/specs/` and what other tracks must do |

## Reading order

**Anyone, first:** this page → [overview](architecture/overview.md) → [implementation plan](roadmap/implementation-plan.md) (find your work package; it lists what else to read).

**Firmware, wallet side:** [upstream-baseline](architecture/upstream-baseline.md) → [upstream-hooks](architecture/upstream-hooks.md) → [signing](wallet/signing.md) → [approval](wallet/approval.md) → [solana-payments](wallet/solana-payments.md) → [checks](wallet/checks.md) → [protocol](protocol/espnow.md) → [testing](testing/testing.md).

**Firmware, platform side:** [app host](platform/app-host.md) → [native apps](platform/native-apps.md) → [config](platform/config.md) → [ui](ui/ui.md) → [shell](ui/shell.md) → [stores](wallet/stores.md).

**App author:** [Lua API](platform/lua-api.md) → [upstream's Lua API](reference/upstream-readme.md) → [apps](apps/apps.md) → [extending](guides/extending.md#add-a-lua-app) → [reasons](reference/reasons.md).

**Operator:** [build, flash, provision](guides/build-flash-provision.md) → [backend](integration/backend.md) → [testing](testing/testing.md#acceptance-tests).

## Rules for agents working from these documents

1. Your work package in the [plan](roadmap/implementation-plan.md) names the files you own. Create and edit only those.
2. Names are contracts. Function names, struct fields, config keys, reason strings, headline strings, frame layouts and Lua names are used exactly as written. If two documents disagree, the document that *owns* the item wins (the table above says which), and you fix the other in the same change.
3. Never edit an upstream file except through a hook or a replacement listed in [upstream-hooks.md](architecture/upstream-hooks.md).
4. Never call `identity::sign` outside `src/vk/wallet/signer.cpp`. Never define a signing domain outside a feature's `domain_*.cpp`.
5. Never write a deployment value (key, mint, URL, name, limit) as a literal. Add a config key.
6. Never assume hardware counts (LEDs, screen size); use upstream's constants.
7. Write the host test first for anything in `src/vk/wallet/pure/` and for every state machine.
8. A package is done when its tests pass on a badge. If hardware contradicts a document, apply the stated fallback, update the document, and say so in the commit.
9. Do not invent requirements. If the documents do not say it, it is not needed; if it seems needed, add it to the owning document first.

## Open items

Everything marked `[UNVERIFIED]`, with the fallback and the package that settles it.

| # | Item | Fallback | Settled by |
|---|---|---|---|
| U1 | ~~The toolchain builds upstream~~ settled: core 3.3.12 builds it | — | WP00, done |
| U2 | ~~Self-registering statics survive linking~~ settled: they do (a service in an unreferenced file ran at boot); no anchor file is needed | — | WP01, done |
| U3 | ~~Ed25519 speed on the ESP32-S3~~ settled with the fallback: TweetNaCl verified in 419 ms (Batch 3), above the 400 ms threshold, so the Monocypher backend is on since Batch 4 (`VK_ED25519_BACKEND 1`) and verifies in 18 ms. Sign is 211 ms (software key, still TweetNaCl) | Monocypher backend: applied | WP51 (M2), done |
| U12 | Upstream's 7-frame ESP-NOW queue loses frames while a signature blocks the loop. Still unverified: needs a second badge | re-challenge; amber when presence is unknown | WP51 (M1), deferred |
| U4 | SE050 signs a 214-byte message with the limit raised to 242. Still unverified: needs an SE050-keyed badge | software key (H18) | WP50, not run; blocked on this badge, whose SE050 refuses select |
| U13 | Which SE050 operation latches the I²C clock low (finding F17) | H21 keeps every path to the SE050 closed ([H21](architecture/upstream-hooks.md#h21--se050-quarantine-provisional)); with it the bus stayed healthy after the power cycle (first evidence, recorded under F17) | one deliberate debugging session with `VK_SE050_QUARANTINE 0`, which will need another power cycle |
| U5 | ~~SNTP sync callback exists in the installed core~~ it does: `sntp_set_time_sync_notification_cb` and `sntp_get_sync_status` are both declared in core 3.3.12 and both paths are compiled in `clock.cpp`. Which one fires on a real network is not yet seen | both paths stay in the code | compiled in WP20; verification on a network deferred (`t_clock_net.py`) |
| U6 | The hotspot lets badges and the laptop reach each other, and passes SNTP. Still unverified: no network test has run | clock floor from records (amber at best) | deferred (`t_clock_net.py`) |
| U7 | Presence round-trip time. Still unverified: needs a second badge; `presence_ms` is its default | `presence_ms` from M1; presence shown amber if unusable | WP51 (M1), deferred |
| U8 | Loop-task stack is sufficient with TLS + signing. Half settled: with signing and verification, and no TLS, the least free stack is 2,272 bytes (M6), above the 1 KB limit, so hook H22 is not applied. With TLS it is unmeasured | raise it with hook H22 | WP51 (M6): signing done; TLS deferred (network) |
| U9 | Authority keypair file is 64 bytes with the public key last. Still unverified: `devnet:setup` has not been run, so no such file exists yet | `--issuer <base58>` | deferred (`vkdev.py provision --env`) |
| U10 | ~~USB auto-reset and serial control~~ settled: auto-reset into the bootloader works; upload needs 460800 baud; opening the port with `vkdev.py` does not reset the badge; its RTS pulse does (ready again after about 8 s) | — | WP00 and WP01, done |
| U11 | Upstream's licence | keep the fork private; ask the author | before publishing |
