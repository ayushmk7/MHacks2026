# Wallet screens

The exact layout and text of every screen the wallet core draws: the approval screen in its three colours, the large-payment confirmation, the blocked screen, and the payee-side prompts.

- Audience: firmware engineers implementing `wallet_ui.cpp`; demo operators who need to know what a judge will see.
- Status: design, not yet built on hardware.
- Base: Solana OS, directory `firmware/solana-os/` at commit `812b8c7` of the [upstream repository](https://github.com/spacemandev-git/solana-defcon-badge-26/tree/812b8c7aca5c366d18c0b040fafd2999f7204d84/firmware/solana-os).

Tags: **[UPSTREAM]** exists at that commit (path given); **[OURS]** our decision; **[UNVERIFIED]** needs a badge. Every screen here is [OURS] and none has been drawn on a badge yet.

These screens are drawn by firmware inside a blocking modal. No app can draw them, draw over them, or dismiss them; see [Approval modal](signing-gate.md#approval-modal). Which screen appears, and in which colour, is decided by the [policy checks](signing-gate.md#policy-checks).

| Screen | Title | Band | Shown when | Ends with |
|---|---|---|---|---|
| [A](#screen-a) | `APPROVE PAYMENT` | green | verified and present | SELECT press signs; CANCEL rejects |
| [B](#screen-b) | `APPROVE PAYMENT` | amber | anything not fully verified, nothing red | hold SELECT 2 s signs; CANCEL rejects |
| [C](#screen-c) | `DO NOT PAY` | red | mismatch, revoked, not present, or the app lied about the amount | blocked (default), or hold SELECT 3 s when `block_red = 0` |
| [D](#screen-d) | `LARGE PAYMENT` | amber | after A/B/C, when `amount > cap` | hold SELECT 2 s confirms; CANCEL rejects |
| [E](#screen-e) | `BLOCKED` | red | the decoder or a local policy check refused the bytes, or signing failed after the gesture | CANCEL, or closes after 15 s |
| [F](#screen-f) | `REQUEST PAYMENT` | purple | payee side: `pay.request()` | SELECT broadcasts; CANCEL goes back |
| [G](#screen-g) | `RECEIVE MODE` | purple | payee side: `pay.receive()` | SELECT allows; CANCEL goes back |
| [H](#screen-h) | `APPROVE PAYMENT` | grey | before A–C while a stale attestation is refreshed | RPC answer or 4 s; CANCEL rejects |

## Font facts

- The display is 320×240 pixels, landscape. [UPSTREAM `src/hal/display.cpp:13-66`]
- The only font is LovyanGFX's default bitmap font, 6×8 px at size 1, scaled by an integer: size 2 is 12×16, size 3 is 18×24. [UPSTREAM `src/hal/display.cpp:160-185`]
- A full line holds 53 characters at size 1, 26 at size 2 and 17 at size 3.
- The font is **ASCII only**. There is no tick, cross or ellipsis glyph. [UPSTREAM `README.md:465-466`]
- The tick and the cross are therefore drawn with `drawLine` as 12×12 px glyphs: tick = (0,6)-(4,10)-(11,1), cross = two diagonals. [OURS]
- Elision is two dots, `..`. Addresses are shown as the first 4 + `..` + the last 4 base58 characters, for example `Gn2G..Ecxq`. [OURS]

In the mockups below, `[v]` stands for the tick glyph, `[x]` for the cross glyph and `[!]` for an amber exclamation mark. Each mockup is 40 characters wide, so one character cell is about 8 px; it shows content and order, not exact positions. Exact positions come from the pixel table. The keys in the mockups are the stand-in keys from `dashboard/server/config/badges.json`: the merchant `Gn2G..Ecxq` ("MHacks Merch", verified), the impostor `DhEb..W511` (unverified, device name `badge-51A0`) and the attacker `FZEA..ei3K`. Real badges have their own keys.

## Pixel layout

Shared by all wallet screens.

| y | Element |
|---|---|
| 0–21 | `display::statusBar(title)` (title left, radios and battery right), 2 px gradient rule at 22–23 [UPSTREAM `src/hal/display.cpp:160-222`] |
| 24–31 | severity band, full width: `theme::OK` green, `theme::WARN` amber, `theme::ERR` red, `theme::ACCENT` purple for payee-side prompts, `theme::BORDER` grey on Screen H |
| 40 | row 1: label size 2 at x=8 (`theme::MUTED`), value size 3 at x=56 (`theme::WHITE`), unit size 2 after the value |
| 74 | row 2: label size 2 at x=8, value size 2 at x=56 (size 1 if longer than 21 characters) |
| 96 | row 3: short address, size 2 at x=56, `theme::MUTED` |
| 122 | identity line: glyph at x=8, text size 2 at x=28 |
| 144 | presence line: glyph at x=8, text size 2 at x=28 |
| 168, 181, 194 | up to three detail lines, size 1 at x=8 |
| 208 | context line, size 1, `theme::MUTED`: `app: <id>   tx: legacy|v0` |
| 218–221 | hold progress bar (fills left to right while SELECT is held) |
| 222–239 | footer bar `theme::HEADER`; left text green, right text red, size 1 |

The theme colours are upstream's (`src/ui/theme.h`): `OK` = `GREEN` = `0x14F195`, `WARN` = `0xFFB020`, `ERR` = `0xFF4545`, `ACCENT` = `PURPLE` = `0x9945FF`, `MUTED` = `0x9393A8`, `WHITE` = `0xFFFFFF`, `TEXT` = `0xE8E8F0`, `BORDER` = `0x3A3A4E`, `HEADER` = `0x161224`.

The whole canvas is redrawn on every iteration of the modal loop, so nothing an app drew before the prompt survives.

## Screen A

**Approve, green.** The recipient key holds a valid attestation and answered a fresh challenge in time. SELECT (one press) signs.

```
+----------------------------------------+
| APPROVE PAYMENT            WiFi NOW 87%|
|########################################|  green band
| PAY   10.00 HACK                       |
|                                        |
| TO    MHacks Merch                     |
|       Gn2G..Ecxq                       |
|                                        |
| [v] verified                           |
| [v] present                            |
|                                        |
|                                        |
| app: pay   tx: legacy                  |
|                                        |
| SELECT sign              CANCEL reject |
+----------------------------------------+
```

- Row 1: the amount decoded from the transaction bytes, formatted with the configured decimals, and the configured symbol.
- Row 2: the **attested** name, fetched and parsed by firmware.
- Row 3: the short address of the recipient wallet.
- Returns `BADGE_OK` and the signature after SELECT (or goes on to [Screen D](#screen-d) when the amount is above `cap`); `rejected` on CANCEL; `approval_timeout` when the call's 60 s budget ends. The budget starts when the app calls the wallet and is shared by Screens H, A–C and D ([State machine](signing-gate.md#state-machine)).

## Screen B

**Approve, amber.** Something could not be verified, and nothing is known to be wrong. The example is an unverified payee whose presence was proven. Hold SELECT for 2 s to sign.

```
+----------------------------------------+
| APPROVE PAYMENT            WiFi NOW 87%|
|########################################|  amber band
| PAY   10.00 HACK                       |
|                                        |
| TO    unverified badge                 |
|       DhEb..W511                       |
|                                        |
| [!] UNVERIFIED                         |
| [v] present                            |
| calls itself: badge-51A0               |
|                                        |
| app: pay   tx: legacy                  |
| [=========>                          ] |
| hold SELECT 2s to sign   CANCEL reject |
+----------------------------------------+
```

- Row 2 never shows the name the other badge gave itself. That name appears only in a detail line, labelled as a claim.
- The progress bar at y=218–221 fills while SELECT is held and empties when it is released.
- Returns as Screen A.

## Screen C

**Approve, red.** Impostor, revoked, not present, or an amount mismatch. With `block_red = 1` (the default) signing is not possible from this screen.

```
+----------------------------------------+
| DO NOT PAY                 WiFi NOW 87%|
|########################################|  red band
| PAY   500.00 HACK                      |
|                                        |
| TO    unverified badge                 |
|       FZEA..ei3K                       |
|                                        |
| [x] NAME MISMATCH                      |
| [!] presence not checked               |
| APP SAID 5.00 HACK                     |
| claims: MHacks Merch                   |
| MHacks Merch is verified for Gn2G..Ecxq|
| app: checkout   tx: legacy             |
|                                        |
| BLOCKED                CANCEL to close |
+----------------------------------------+
```

- The mockup is the compromised-laptop case: the Checkout app showed 5.00 HACK to "MHacks Merch", the bytes say 500.00 HACK to a key that is not the merchant's.
- The three detail lines follow the ordering rule in [Text strings](#text-strings): `APP SAID` first, then the claim, then who the claimed name is verified for.
- With `block_red = 1`: the footer is `BLOCKED` / `CANCEL to close`; the call returns `blocked` after CANCEL or when the call's 60 s budget ends. It returns `blocked` and not `rejected` because signing was never possible; `rejected` means the user declined something they could have approved.
- With `block_red = 0`: the footer is `hold SELECT 3s to sign anyway` / `CANCEL reject`, and the progress bar is shown.
- Red `NAME MISMATCH` and `REVOKED` need the payer's badge to have seen the real merchant verified once ([Known names](../identity/attestation.md#known-names)). A badge that has never seen the merchant shows the amber Screen B with `UNVERIFIED`.

## Screen D

**Large payment.** The second confirmation for requirement F14, shown after the gesture on A, B or C succeeds and `amount > cap`. Hold SELECT for 2 s.

```
+----------------------------------------+
| LARGE PAYMENT              WiFi NOW 87%|
|########################################|  amber band
|                                        |
|          500.00 HACK                   |  size 3, centred, y=56
|                                        |
|   is above your limit of 100.00 HACK   |  size 1, centred, y=100
|   to MHacks Merch  Gn2G..Ecxq          |  size 1, centred, y=116
|                                        |
|                                        |
| [=========>                          ] |
| hold SELECT 2s to confirm CANCEL reject|
+----------------------------------------+
```

- The limit shown is the configured `cap` (default 100.00 HACK).
- The second line follows the same rule as row 2 of Screens A–C and never shows a claimed name:

| Recipient | Second line |
|---|---|
| identity `VERIFIED` | `to <attested name>  <short addr>` |
| any other identity state | `to unverified badge  <short addr>` |
| recipient wallet unknown | `to token account  <short token account>` |

- Returns to the gate, which then signs; `rejected` on CANCEL; `approval_timeout` when the call's 60 s budget ends. Screen D does not get a new 60 s: it ends at the deadline set when the call started.
- An amount above `max` never reaches this screen: it is refused on [Screen E](#screen-e) with `Above the maximum`.

## Screen E

**Blocked.** The decoder or a local policy check refused the bytes (requirement F3), or the key did not produce a valid signature after the gesture. Nothing was signed and nothing can be signed from here.

```
+----------------------------------------+
| BLOCKED                    WiFi NOW 87%|
|########################################|  red band
|                                        |
|  Unknown instruction                   |  size 2, y=56
|                                        |
|  This badge only signs HACK transfers. |  size 1, y=96
|  Nothing was signed.                   |  size 1, y=110
|  reason: ix_data                       |  size 1, y=134, MUTED
|  app: checkout                         |  size 1, y=148, MUTED
|                                        |
| CANCEL to close                        |
+----------------------------------------+
```

- The headline depends on the error; see [Blocked headlines](#blocked-headlines).
- `reason:` is `sol_tx_err_name()` for a decoder rejection, otherwise the Lua error string ([Error codes](../reference/error-codes.md)).
- Closes on CANCEL or after 15 s (`WALLET_BLOCKED_AUTOCLOSE_MS`). The call then returns the error that caused the screen.
- Shown on Screen E before the call returns: codes 20–25, `too_long` (12) when policy check 11 raises it, and `sign_failed` (29). `blocked` (26) is returned after the red Screen C. `no_display` (30) is never drawn: the call returns at once and logs `[wallet] no_display: refusing to sign`.
- For `sign_failed` the first body line is `The key did not produce a valid signature.` in place of `This badge only signs HACK transfers.`; the other three lines are the same, with `reason: sign_failed`.

## Screen F

**Request (payee side).** Shown by `wallet_pay_request()` before a payment request is signed and broadcast (requirements F6, F8).

```
+----------------------------------------+
| REQUEST PAYMENT            WiFi NOW 87%|
|########################################|  purple band
| ASK   10.00 HACK                       |
|                                        |
| AS    MHacks Merch                     |
|       Gn2G..Ecxq                       |
|                                        |
| [v] verified                           |  own attestation status
| valid for 120 s                        |
| nearby badges can check you are here   |
|                                        |
| app: request                           |
|                                        |
| SELECT broadcast           CANCEL back |
+----------------------------------------+
```

- Row 2 (`AS`) and the identity line describe this badge itself. They are read from the cache without a network call (`attest_cached(own key)`).
- Row 2 is the badge's own attested name when its cached attestation is `VERIFIED`, and `settings::deviceName()` otherwise. It is the same name the request carries.
- `valid for <ttl> s` is the request's time to live (default 120 s). A `ttl_s` above 120 is clamped to 120, and the screen shows the clamped value.
- `nearby badges can check you are here` tells the user what the SELECT press authorises beyond the request itself: answering presence challenges for this request without further presses, within the limits in [Rate limits](signing-gate.md#rate-limits).
- Returns `BADGE_OK` and the request id after SELECT; `rejected` on CANCEL; `approval_timeout` after 60 s.

A badge with no valid attestation shows its device name in row 2, its short address in row 3, and its own cached state on the identity line in the usual strings: `[!] UNVERIFIED`, `[x] REVOKED`, `[!] EXPIRED`, or `[!] NOT CHECKED` when nothing is cached.

```
+----------------------------------------+
| REQUEST PAYMENT            WiFi NOW 87%|
|########################################|  purple band
| ASK   10.00 HACK                       |
|                                        |
| AS    badge-51A0                       |
|       DhEb..W511                       |
|                                        |
| [!] UNVERIFIED                         |
| valid for 120 s                        |
| nearby badges can check you are here   |
|                                        |
| app: request                           |
|                                        |
| SELECT broadcast           CANCEL back |
+----------------------------------------+
```

## Screen G

**Receive mode (payee side).** Shown by `wallet_pay_receive()`. The user allows the badge to answer presence challenges for a period without a specific request.

```
+----------------------------------------+
| RECEIVE MODE               WiFi NOW 87%|
|########################################|  purple band
| Allow presence checks                  |
|                                        |
| for 10 min                             |
|                                        |
|                                        |
| [v] verified                           |
|                                        |
| Nearby badges can check you are here.  |
| No payment is made by this.            |
| app: home                              |
|                                        |
| SELECT allow               CANCEL back |
+----------------------------------------+
```

Screen G does not use the label/value rows of the [pixel layout](#pixel-layout). Its own layout:

| y | Element |
|---|---|
| 40 | `Allow presence checks`, size 2 at x=8, `theme::WHITE` (21 characters, 252 px) |
| 74 | `for 10 min`, size 2 at x=8, `theme::MUTED`: `for <n> min` with n = `ttl_s` / 60, or `for <n> s` below 60 s |
| 122 | the badge's own identity line, as on [Screen F](#screen-f) |
| 168 | detail line `Nearby badges can check you are here.` |
| 181 | detail line `No payment is made by this.` |
| 208 | context line `app: <id>` |
| 222–239 | footer `SELECT allow` / `CANCEL back` |

- `10 min` is the session's time to live (default 600 s). A `ttl_s` above 600 is clamped to 600.
- Returns `BADGE_OK` after SELECT; `rejected` on CANCEL; `approval_timeout` after 60 s.

## Screen H

**Checking identity.** An interstitial shown before A–C when the cached attestation for the recipient is older than `attest_ttl` and is being re-fetched.

Title `APPROVE PAYMENT`. The band is `theme::BORDER` (grey, `0x3A3A4E`). `Checking identity...` is drawn with `display::textCentered` at x=160, y=104, size 2, `theme::TEXT`; there is nothing else in the body. The footer has `CANCEL reject` (red) on the right and nothing on the left. The LEDs are off. Ends on the RPC answer or after 4 s.

```
+----------------------------------------+
| APPROVE PAYMENT            WiFi NOW 87%|
|########################################|  grey band
|                                        |
|                                        |
|                                        |
|                                        |
|          Checking identity...          |
|                                        |
|                                        |
|                                        |
|                                        |
|                                        |
|                                        |
|                           CANCEL reject|
+----------------------------------------+
```

- CANCEL returns `rejected` without showing the approval screen.
- On an RPC answer the gate continues to A, B or C with the fresh result. On timeout the identity is `NOT CHECKED` (amber at best).
- The time spent here comes out of the call's 60 s budget.

## Text strings

Exact strings. Do not reword them: demo scripts and acceptance checks quote them.

**Title** (status bar): `APPROVE PAYMENT` for green, amber and the checking screen (H), `DO NOT PAY` for red, `LARGE PAYMENT`, `BLOCKED`, `REQUEST PAYMENT`, `RECEIVE MODE`.

**Identity line** (y=122):

| `wallet_identity_t` | Text | Glyph |
|---|---|---|
| `VERIFIED` | `verified` | green tick |
| `UNVERIFIED` | `UNVERIFIED` | amber `!` |
| `MISMATCH` | `NAME MISMATCH` | red cross |
| `REVOKED` | `REVOKED` | red cross |
| `EXPIRED` | `EXPIRED` | amber `!` |
| `UNKNOWN` | `NOT CHECKED` | amber `!` |

**Presence line** (y=144):

| `wallet_presence_t` | Text | Glyph |
|---|---|---|
| `PRESENT` | `present` | green tick |
| `NOT_CHECKED` | `presence not checked` | amber `!` |
| `NOT_PRESENT` | `NOT PRESENT` | red cross |

**Row 2 value** (the name row): the **attested** name when identity is VERIFIED; otherwise the literal `unverified badge`, or `token account` when the recipient wallet is unknown (row 3 then shows the token account). **A claimed name is never drawn in row 2.** [OURS: product goal 2, never present an impostor as verified. The name row is the one place a user reads as "who"; only firmware-verified data may appear there.]

**Detail lines** (y=168, 181, 194): the first three that apply, in this order:

| Order | Text | Colour | Meaning |
|---|---|---|---|
| 1 | `APP SAID <amount> <SYM>` | red | the app's claimed amount differs from the decoded amount |
| 2 | `does not match the request` | red | a request id was given and the request does not match this transaction |
| 3 | `claims: <name>` or `calls itself: <name>` | — | the requester or the app supplied a name; which wording is used depends on the identity state (below) |
| 4 | `<name> is verified for <short addr>` | red | the claimed name is attested to a different key |
| 5 | `proof late: <n> ms` | red | a valid proof arrived after the deadline |
| 6 | `identity cached <n>s ago` | — | the attestation shown comes from the cache |
| 7 | `recipient is a token account` | — | the recipient wallet is unknown |
| 8 | `RPC TLS not pinned` | — | the RPC connection is not certificate-pinned |
| 9 | `checked via phone` | — | the route is the phone bridge |

The wording of line 3 depends on the identity state:

| Identity state | Line 3 |
|---|---|
| `MISMATCH` | `claims: <name>` |
| `UNVERIFIED`, `REVOKED`, `EXPIRED` or `UNKNOWN`, and a claim exists | `calls itself: <name>` |
| `VERIFIED` | no claim line; the attested name is in row 2 |

**Context line** (y=208): `app: <id>   tx: legacy|v0`. The app id is the caller's, so the user sees who asked.

**Footer** (left text green, right text red):

| Screen | Left | Right |
|---|---|---|
| A (green) | `SELECT sign` | `CANCEL reject` |
| B (amber) | `hold SELECT 2s to sign` | `CANCEL reject` |
| C (red, `block_red = 1`) | `BLOCKED` | `CANCEL to close` |
| C (red, `block_red = 0`) | `hold SELECT 3s to sign anyway` | `CANCEL reject` |
| D | `hold SELECT 2s to confirm` | `CANCEL reject` |
| E | `CANCEL to close` | — |
| F | `SELECT broadcast` | `CANCEL back` |
| G | `SELECT allow` | `CANCEL back` |
| H | — | `CANCEL reject` |

Every footer names the silkscreen keys, as the product's judge-UX requirement asks.

**Screen D body**: `is above your limit of <cap> <SYM>`, then `to <attested name>  <short addr>`, `to unverified badge  <short addr>` or `to token account  <short token account>` ([Screen D](#screen-d)).

**Screen E body**: `This badge only signs HACK transfers.`, `Nothing was signed.`, `reason: <reason>`, `app: <id>`. For `sign_failed`: `The key did not produce a valid signature.`, `Nothing was signed.`, `reason: sign_failed`, `app: <id>`.

**Screen F body**: `valid for <ttl> s`, `nearby badges can check you are here`.

**Screen G body**: `Allow presence checks`, `for <n> min` (or `for <n> s`), `Nearby badges can check you are here.`, `No payment is made by this.`

**Screen H body**: `Checking identity...`

A detail line longer than 52 characters at size 1 (possible for `<name> is verified for <short addr>` with a long name) does not fit from x=8; shorten the name with `..`.

## LED colours

While a prompt is on screen both LEDs are solid in the colour of the band: green, amber, red, or purple for payee-side prompts (`setLeds(p.severity)` in the [modal](signing-gate.md#approval-modal)). Any running LED animation is stopped first, and the LEDs are switched off when the prompt closes.

| Prompt | LEDs | RGB | Theme colour |
|---|---|---|---|
| A | green | (20, 241, 149) | `theme::OK` |
| B, D | amber | (255, 176, 32) | `theme::WARN` |
| C, E | red | (255, 69, 69) | `theme::ERR` |
| F, G | purple | (153, 69, 255) | `theme::ACCENT` |
| H | off | — | — |

LED brightness follows the user's setting (default 72/255 [UPSTREAM `src/config.h:124-125`]). Whether the four colours can be told apart at that brightness is [UNVERIFIED]; the fallback is to raise the LED brightness inside the modal only.

## Blocked headlines

The headline on [Screen E](#screen-e) for each error:

| Error (`badge_err_t`) | Headline |
|---|---|
| `unknown_instruction` | `Unknown instruction` |
| `wrong_signer` | `Not your account` |
| `unknown_mint` | `Unknown token` |
| `decimals` | `Wrong token decimals` |
| `bad_source` | `Not your token account` |
| `over_limit` | `Above the maximum` |
| `too_long` | `Transaction too large` |
| `sign_failed` | `Signing failed` |

`reason:` is `sol_tx_err_name()` (for example `ix_data`, `program`, `trailing`) when the decoder rejected the message, or the Lua error string (for example `over_limit`) otherwise.

`no_display` has no headline and no screen: the call returns at once and logs `[wallet] no_display: refusing to sign`. `blocked` is not shown here either; it is returned after the red [Screen C](#screen-c).

## Requirements covered

- **F2**: the approval screen shows decoded amount, token and recipient; SELECT signs, CANCEL rejects (Screens A–C).
- **F3**: "Unknown instruction" and blocked (Screen E).
- **F11**: verified (tick), unverified (amber), mismatch (red) (identity line, Screens A–C).
- **F14**: second confirmation above the cap (Screen D).
- **F6, F8, F9**: payee-side consent for a signed request and for presence proofs (Screens F, G).
- **F15**: `REVOKED` state (identity line, Screen C).
- Non-functional "Judge UX": every screen names the silkscreen button; CANCEL always leaves.

## Open items

- [UNVERIFIED] None of these layouts has been drawn on the panel. Check text fit at each size, especially row 1 with large amounts (`1000.00` at size 3 plus ` HACK` at size 2) and 32-character names in row 2 at size 1. Fallback: drop row 1's value to size 2 when it would pass x=320.
- [UNVERIFIED] Legibility of the amber and red bands under event lighting. Fallback: widen the band (y=24–35).
- [UNVERIFIED] Whether the LED colours can be told apart at brightness 72 (item U22 of the [register](../README.md#open-items)). Fallback: raise the LED brightness in the modal only.
