# UI: LEDs, status bar, theme

The parts of the user interface Badge OS owns outside the approval screen (which is specified in [../wallet/approval.md](../wallet/approval.md#screen) and is deliberately not customisable). Files: `src/vk/ui/`.

**Status of this document:** final. The look is the Receipt design in light and dark mode ([Theme](#theme)).

## LED patterns

The badge has `RGB_LED_COUNT` LEDs (2 on this board). No pattern may assume a number: every pattern loops `for (i = 0; i < RGB_LED_COUNT; ++i)`, so a board with 3 or 5 LEDs needs no code change.

```cpp
// src/vk/ui/leds.h
namespace vk::ui::leds {
struct LedPattern : Registered<LedPattern> {
  const char *name;
  bool (*frame)(uint32_t t_ms);     // t_ms since the pattern started. Sets LEDs with ::leds::set(). Return false when finished
  LedPattern(const char *n, bool (*f)(uint32_t)) : name(n), frame(f) {}
};
#define VK_LED_PATTERN(ident, name, frame_fn) static vk::ui::leds::LedPattern vk_led_##ident(name, frame_fn)

void play(const char *name);        // replaces whatever is playing; unknown name: logs and does nothing
void stop();
bool playing();
void bootProgress(uint8_t percent); // hook H15
}
```

A service calls the current pattern's `frame` and then `::leds::show()`, at most once every 16 ms (as upstream does: `::leds::show()` is a blocking transfer); the first frame of a pattern is drawn at once. Upstream's own `leds::update()` also runs every loop and redraws whenever one of *its* animations is set (the idle breath restarts whenever the launcher comes back), so on **every frame it draws** the service first calls `::leds::stopAnimation()`. When a pattern finishes or is stopped, the LEDs are turned off and, if the badge is idle (`vk::host::idle()`), upstream's idle animation resumes (`::leds::playIdle()`). While an app runs and no pattern is playing, the LEDs belong to the app.

| Name | When | Look | Ends |
|---|---|---|---|
| `approve_green` | approval open, green | slow green pulse, 1.5 s period | when the approval closes |
| `approve_amber` | approval open, amber | amber pulse, 1 s period | when the approval closes |
| `approve_red` | approval open, red | solid red | when the approval closes |
| `signed` | approval result: signed or approved | three quick green flashes (100 ms on, 100 ms off) | after 600 ms |
| `refused` | approval result: cancelled, timeout, blocked, failed | one red blink (on for 250 ms) | after 400 ms |
| `notify` | a notification is waiting and the badge is idle (`vk::host::idle()`) | dim purple breathe, 3 s period | when the inbox is empty or an app starts |

Colours are the approval's fixed severity colours ([approval](../wallet/approval.md#screen)) and upstream's brand purple.

### Boot bar

`bootProgress(percent)` turns the LEDs into a progress bar driven by the real boot stages. Upstream calls `boot::progress` as each stage begins (storage 20, peripherals 45, identity 55, Lua 65, radios 85, ready 100), and hook H2 adds the wallet stage at 75. `bootProgress` stores the percentage; a boot-bar pattern draws it.

Each LED owns an equal share of the boot (`share = 100 / RGB_LED_COUNT`). For LED `i`:

| Boot percent | LED `i` |
|---|---|
| below `i × share` | off |
| from `i × share` up to, but not including, `(i + 1) × share` | **flashing** (on 170 ms, off 170 ms) |
| `(i + 1) × share` or more | **solid** |

At 100 % every LED is solid. With this board's 2 LEDs: below 50 % the first LED flashes and the second is off; from 50 % the first is solid and the second flashes; at 100 % both are solid. A board with 3 LEDs gets 33 % each and one with 5 gets 20 % each, with no code change.

```
on every frame while booting: ::leds::stopAnimation()
share = 100 / RGB_LED_COUNT
blink = (millis() / 170) % 2 == 0
for each LED i:
    if percent >= 100 or percent >= (i + 1) * share:  on = true
    else if percent >= i * share:                     on = blink
    else:                                             on = false
    ::leds::set(i, on ? theme accent colour : black)
::leds::show()
```

Boot stages block, so the bar is drawn only inside `bootProgress()`, once per stage, from `boot::progress` (hook H15): no service runs during `setup()`, and upstream's `tick()` is its own `leds::update()`, which never reaches Badge OS code. A stage therefore holds one flash state until the next stage begins, which is acceptable. The boot bar is not a registered pattern. The upstream splash animation still plays during the two splash screens, before the first stage. The colour is the active theme's accent ([Theme](#theme)).

### Adding a pattern

```cpp
static bool rainbow(uint32_t t) {
  for (uint8_t i = 0; i < RGB_LED_COUNT; ++i) { /* ::leds::set(i, r, g, b) */ }
  return t < 2000;
}
VK_LED_PATTERN(rainbow, "rainbow", rainbow);
```

Then `vk::ui::leds::play("rainbow")` from firmware, or name it in an `ApprovalRequest::led`. Add the row to the table above.

## Status bar

Upstream draws a 22 px bar on the launcher and settings screens: title on the left, radios and battery on the right. Hook H14 lets Badge OS add items to the left of the radios text.

```cpp
// src/vk/ui/statusbar.h
namespace vk::ui::statusbar {
struct StatusItem : Registered<StatusItem> {
  const char *name;
  int order;                           // lower = further right
  int (*draw)(int rightX, int y);      // draw right-aligned ending at rightX; return the width used, 0 for nothing
  StatusItem(const char *n, int o, int (*d)(int, int)) : name(n), order(o), draw(d) {}
};
#define VK_STATUS_ITEM(ident, name, order, draw_fn) static vk::ui::statusbar::StatusItem vk_status_##ident(name, order, draw_fn)
void draw(int rightEdgeX);             // hook H14
}
```

Items are drawn right to left in `order`, 8 px apart, and drawing stops before x = 110 so the title is never covered.

| Item | Order | Shows | Registered by |
|---|---|---|---|
| `setup` | 10 | `SETUP` in amber while unprovisioned | `core/config` |
| `dev` | 20 | `DEV` in amber in the dev profile | `ui/status_dev.cpp` |
| `inbox` | 30 | `[n]` when n notifications are waiting | `host/notify` |
| `balance` | 40 | `12.50 HACK`, the default token's last known balance | `features/balance` |

### Balance

`src/vk/features/balance/`. A service that, every `balance_poll_s` seconds, **only while the badge is idle (`vk::host::idle()`), no approval is open and the badge is joined to a network** (`wifi_mgr::mode() == wifi_mgr::Mode::Station && wifi_mgr::connected()`; `connected()` alone is also true in hotspot mode), makes one JSON-RPC call to `rpc_url`:

```json
{"jsonrpc":"2.0","id":1,"method":"getTokenAccountsByOwner",
 "params":["<this badge's address>",{"mint":"<default token mint>"},{"encoding":"jsonParsed"}]}
```

From the first account in the reply it keeps the account's address (`pubkey`) and `tokenAmount.amount`. The reply is scanned for those two fields with a small string search, not a JSON library, and any reply that does not contain both is ignored. Timeout 3 s; the launcher stalls for that long at worst. While an app runs the service does not poll: an app that needs the balance or the token account calls `wallet.refresh_balance()` (Home does so on start and every `balance_poll_s`; `vk.pay` does so when `wallet.token_account()` is nil). Only the default (first) token is tracked; `wallet.balance(symbol)` and `wallet.token_account(symbol)` return nil for any other symbol. The token account address is what `wallet.token_account()` returns and what `build_transfer` uses as the default source; this is how the badge learns its own token account without deriving it.

```cpp
// src/vk/features/balance/balance.h
namespace vk::balance {
bool known();
uint64_t raw();
bool tokenAccount(uint8_t out[32]);
bool refresh(uint32_t timeoutMs);      // one blocking fetch
}
```

As built, the header also has `Reason fetch(uint32_t timeoutMs)` (what `refresh` and `wallet.refresh_balance` call; its reasons are in the [Lua API](../platform/lua-api.md#badgewallet-balance)), `FETCH_TIMEOUT_MS` (3000) and the reply scanner `scanReply` as an inline pure function, tested in the host suite `test_stores`.

- The balance and the token account are always known together: a reply must hold both.
- A stored balance belongs to the mint it was fetched for. If provisioning makes another mint the default token, the balance counts as unknown until the next fetch.
- A fetch logs `[bal] fetch <ms> ms` (measurement M5), or `[bal] fetch failed after <ms> ms: …`. With Wi-Fi down the poll is skipped silently. `balance_poll_s 0` turns the poll off.
- The status item is drawn in upstream's `::theme::TEXT`, since the bar is upstream's.
- Device tests run with Wi-Fi off, so the poll never fires under the test provisioning. On a badge that is joined to a network, the test `rpc_url` (`http://127.0.0.1:8899`) makes every idle poll fail after up to 3 s; add `balance_poll_s 0` to `test_config()` in `test/device/common.py` if that disturbs a run.

The feature publishes these to the rest of the firmware through `vk::wallet::tokenInfoLookup` ([signing](../wallet/signing.md#cross-feature-interfaces)), so `solana_pay` and the Wallet app never include a `balance` header.

Upstream's shell redraws only when its own dirty flag is set (finding F12). When a status item's value changes (a new balance, a notification count, provisioning), its owner calls `vk::ui::requestShellRepaint()`; hook H20 makes the shell redraw on its next pass. The approval engine calls it when it closes.

### Adding a status item

One `VK_STATUS_ITEM` line and a draw function that returns the width it used. Add the row above.

## Theme

**Decided: the Receipt design, in a light and a dark mode.** Reference: `docs/design/os-mockups/index.html`, a live simulation of every screen (serve the folder with `python3 -m http.server 8765`, open `http://127.0.0.1:8765`; keys 1 and 2 switch mode). When this document and the simulation disagree about a pixel, the simulation wins; when they disagree about behaviour, this document wins.

The idea: every screen is a printed ticket. Monospace text, dotted leaders between a label and its value, dashed tear rules, amounts in a bold serif, and a barcode. The verdict is carried by a coloured band alone. Screens that show an amount are split into a left stub (the amount) and a right body (the details) by a dashed vertical perforation. No owner name appears in any header.

### Tokens and modes

```cpp
// src/vk/ui/theme.h
namespace vk::ui::theme {
enum Token : uint8_t { PAPER, INK, FAINT, SUB, STAMP_OK, STAMP_WARN, STAMP_BAD, LED, TOKEN_COUNT };
struct Theme : Registered<Theme> {
  const char *name;
  uint16_t colors[TOKEN_COUNT];      // RGB565
  Theme(const char *n, std::initializer_list<uint32_t> rgb888);   // converts to RGB565
};
#define VK_THEME(ident, name, ...) static vk::ui::theme::Theme vk_theme_##ident(name, {__VA_ARGS__})
uint16_t color(Token token);         // from the active theme
uint16_t blend(Token a, Token b, uint8_t amount);   // a towards b, 0..255
const char *activeName();
void setActive(const char *name);    // writes config key `theme`; unknown name: no change
size_t count();  const Theme *at(size_t i);
}
```

| Token | Use | `receipt-light` | `receipt-dark` |
|---|---|---|---|
| `PAPER` | background | `#F3EFE4` | `#15140F` |
| `INK` | text, rules, barcode, selected-row fill | `#1B1A17` | `#ECE6D6` |
| `FAINT` | dotted leaders, hold-bar track | `#8A8474` | `#7D7868` |
| `SUB` | second line of a list row | `#6D6759` | `#A39C8A` |
| `STAMP_OK` | status ink: "signed", "paid" and other good-outcome text in lists | `#17804F` | `#4FD69A` |
| `STAMP_WARN` | status ink: warning text in lists (cancelled, timed out, unsynced) | `#B56A00` | `#FFC35A` |
| `STAMP_BAD` | status ink: "blocked" and "failed" text in lists | `#C8321E` | `#FF7B6E` |
| `LED` | boot bar and idle LED colour | `#FFE2AA` | `#FFC478` |

The three `STAMP_*` tokens keep the names of the first design; nothing draws a stamp. They colour text (through `row`'s `valueColor`, for example), never a band.

The config key `theme` holds the active theme's name; empty or an unknown name means `receipt-light`. `color()` works before `vk::begin()` (the config accessors start the store themselves) and re-reads the key every 500 ms, so `VKSET theme receipt-dark` shows on the next frame without a reboot. If `setActive`'s write is refused, the stored name wins again at the next read. A selected row is drawn inverted: `INK` fill, `PAPER` text. The three **severity colours are not tokens**: green `#1FBF75`, amber `#FFB020`, red `#FF4545` in both modes, with black text on them ([approval](../wallet/approval.md#screen)).

Adding a theme is one `VK_THEME(...)` line with eight colours; it then appears in the Theme setting. Removing one is deleting that line.

### Fonts

All from the graphics library (LovyanGFX), no font files to ship:

| Role | Font | Size on screen |
|---|---|---|
| body, labels, header, footer | `fonts::Font0` | 6×8 px per character, 53 columns |
| band headline, section titles (`MENU`, `PAY`) | `fonts::FreeMonoBold9pt7b` | about 11 px per character |
| amounts | `fonts::FreeSerifBold24pt7b`; `FreeSerifBold18pt7b` when the text is wider than 136 px; then `FreeSerifBold9pt7b`, then `Font0`, so that an amount is never cut and never crosses the perforation | |
| brand line "Badge OS" on the boot screen | `fonts::FreeSerifBoldItalic12pt7b` | |

All the names exist in the installed LovyanGFX 1.2.32 (`lgfx_fonts.hpp`), as does `textWidth`. `FreeSerifBold9pt7b` is also used, for wrapped text in the approval's left stub and as the third amount size. If a later library version drops one, fall back to the numbered fonts `Font4` (amounts) and `Font2` (titles).

### The receipt kit

Every Badge OS screen is drawn with one small set of functions, so that screens stay consistent and a new screen is a few calls. `src/vk/ui/receipt.{h,cpp}`:

```cpp
namespace vk::ui::receipt {
void page();                                              // fill PAPER
void header(const char *left, const char *right);         // y 0..19: text at y=7, then a dashed rule at y=19
void statusRight(char *out, size_t cap);                  // "14:32 · 87%": the time (when the clock has a source) and the battery, nothing else
void title(const char *text, int y);                      // centred, letter-spaced, FreeMonoBold9pt7b
void rule(int y, int x0 = 10, int x1 = 310);              // dashed: 3 px on, 2 px off
void perforation(int x, int y0, int y1);                  // dashed vertical line
void row(int x0, int x1, int y, const char *label, const char *value, bool selected = false, uint16_t valueColor = 0);
                                                          // label left, value right, dotted leader (one dot every 3 px) between
void subline(int x0, int x1, int y, const char *text, bool selected = false);   // SUB colour, indented 12 px
void amount(int cx, int y, const char *label, const char *value, const char *unit);   // label, big serif value, unit; centred on cx
void barcode(int x, int y, int w, int h, const uint8_t *seed, size_t seedLen);       // bars derived from the bytes
void holdBar(float progress);                             // y 204..209, x 40..280
void footer(const char *left, const char *right);         // dashed rule at y=216, text at y=224
void headerText(const char *left, const char *right);     // header() without its rule: the approval's band sits directly under it
void title(const char *text, int y, int cx);              // title() centred on cx (the body column of a two-column screen is centred on 233)
}
```

Conventions every kit function follows (also in the comment at the top of `receipt.h`):

- A `y` is the top of the capital letters. A row at `y` owns y-5 to y+12; a selected row fills x0-10 to x1+10. A subline goes at y+13 and owns the 13 px below it.
- Text that does not fit its space is cut and ends in `..`.
- Body text is `Font0`. Three characters outside ASCII are drawn by hand, one column wide, when written as UTF-8: the middle dot `·` (U+00B7) and the triangles `◂` (U+25C2) and `▸` (U+25B8); LovyanGFX's own string drawing cannot reach them. Any other byte outside `0x20`–`0x7E` is drawn as `?`. `display::text` does not do this: text with a middle dot must go through the kit.
- `statusRight` separates its parts with that middle dot (`14:32 · 87%`, or `14:32 · USB` on external power). The time is UTC (there is no timezone key) and is left out when the clock has no source, leaving `87%` or `USB` alone. Seen on the badge: `03:00 · USB` and `USB` (`os/test/device/shots/header_clock_usb.png`, `header_noclock_usb.png`).
- Upstream never sets a font on the canvas, so every kit function leaves it on `Font0`, size 1, top-left datum. Code that sets a font on the canvas must put it back.
- The footer clears its strip to `PAPER` first and its rule runs edge to edge (x 0..319), as in the simulation. The hold bar is 240 px wide (x 40..279) over a half-tone `FAINT` track. Sublines and `sub` are not bold (`Font0` has no bold).

Layout constants (pixels): margins 10; list row pitch 18; subline 13 below its row; two-column split at x = 146 (left stub 0..145, body 147..319); content starts at y = 24 under the header. The barcode's bars come from the badge's public key, so each badge prints its own.

### Header

The right side of every header is `receipt::statusRight`: the time and the battery, and nothing else. No notification count, no setup or dev marker (waiting notifications show on the launcher's Inbox row; an unprovisioned badge says so on the launcher's balance row; the dev build is marked on the approval screen, where it matters).

The battery figure is the measured one. The badge has no fuel gauge, so the only real measurement is the cell voltage, which upstream's `power::percent()` maps to a percentage. When the badge is on external power (`power::charging()`, cell line above 4.25 V) the ADC is reading the charger, not the cell, and any percentage would be invented: the header shows `USB` instead.

### Screens

| Screen | Layout |
|---|---|
| Boot | header `BADGE OS` / `*** STARTING UP ***`; left stub: brand line, percent as an amount, a 14-cell block bar, the stage's detail text; body: title `CHECKLIST`, one row per stage with `OK`, `..` or blank. Replaces upstream's progress screen (hook H15 draws it; upstream's two splash images are kept) |
| Launcher | header; title `MENU`; apps in a 2-column grid of rows `NN NAME`, selected row inverted with `◂` (the `inbox` row shows the number of waiting notifications as its value when there are any); rule; row `BALANCE … 142.50 HACK`, or `SETUP NEEDED` while the badge is unprovisioned; barcode; footer `SELECT open` / `CANCEL settings` |
| Home | left stub: `BALANCE` amount, barcode; body: rows ADDRESS, KEY, CLOCK, INBOX; a rule; `THANK YOU FOR HACKING` |
| Any list (Pay, History, Contacts, Inbox, Wallet, settings, shop) | header; title; rows with optional sublines (5 rows with sublines or 9 without); footer with the action and `CANCEL back` |
| Approval | [approval](../wallet/approval.md#screen) |
| Request | left stub: amount with label `PAY ME`, `WAITING` or `RECEIVED`, barcode; body: title `REQUEST`, three rows. When the payment is confirmed the left stub's label reads `PAID`, drawn in the `STAMP_OK` colour |

Lua apps get the same look through `lib/vk.lua`'s `vk.ui` helpers (`vk.ui.header`, `row`, `title`, `amount`, `footer`, `rule`), which draw with `badge.gfx` using colours from `badge.theme.color(name)`; `badge.theme.color` and `badge.theme.name()` are registered Lua functions with no permission.

### Launcher and settings

Upstream's launcher and settings screens use upstream's compile-time colours. Rather than edit `shell.cpp`, Badge OS ships its own launcher as a native app and keeps upstream's shell as the fallback behind it:

- Native app `launcher` draws the MENU screen above. It lists every installed app (Lua and native, through `app_store::count()`/`at()`), except itself. SELECT launches. CANCEL opens the native app `settings`.
- Native app `settings`: rows **Theme** (SELECT cycles through the registered themes), **Wallet** (opens `wallet_settings`), **Inbox**, and **System settings** (leaves to upstream's own settings screens, which keep upstream's look: Wi-Fi, Bluetooth, ESP-NOW, push, app store, identity, display, LEDs).
- The **home service** (`src/vk/host/home.{h,cpp}`) keeps the launcher in front: when no app is running, no approval is open, upstream has no error to show (`runtime::lastError()` is empty), and the shell was not asked for, it calls `runtime::requestLaunch(<home_app>)`. Config key `home_app` (default `launcher`; empty disables the service and leaves upstream's launcher in charge).
- `vk::host::showShell()` is how the settings app reaches upstream's screens: it sets "shell asked for" and exits the app. The flag clears the next time any app starts. From upstream's launcher, launching any app or holding CANCEL returns to ours.
- `vk::host::idle()` is what "the badge is idle" means everywhere in these documents: true when no app is running or the running app is config `home_app`. The launcher is itself a native app that runs at all times (hook H8c makes `runtime::running()` true for it), so "no app is running" would never hold; the balance poll, the `notify` LED pattern and the idle LED animation ask `idle()` instead.

```cpp
// src/vk/host/home.h
namespace vk::host { void showShell(); bool idle(); }
```

So an app that exits, crashes or is force-quit lands on the Receipt launcher, and the upstream screens are reachable only through **System settings**.

When upstream's shell is showing, hook H14 still adds our status items to its bar.
