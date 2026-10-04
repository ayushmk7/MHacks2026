# UI: LEDs, balance, theme

The look and the parts shared by every BadgeOS screen: LED patterns, the balance, the Receipt theme, the fonts and the receipt drawing kit. The screens themselves are specified elsewhere: the shell (boot, launcher, settings, dialogs) in [shell.md](shell.md), the approval screen in [../wallet/approval.md](../wallet/approval.md#screen) (deliberately not customisable), each app in [../apps/apps.md](../apps/apps.md). Files: `src/vk/ui/`.

The whole user interface is BadgeOS's own. Upstream's shell, splash and status bar are gone ([what was removed](shell.md#what-was-removed)).

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
void bootProgress(uint8_t percent); // called by vk::ui::bootScreen for every boot stage
}
```

A service calls the current pattern's `frame` and then `::leds::show()`, at most once every 16 ms (as upstream does: `::leds::show()` is a blocking transfer); the first frame of a pattern is drawn at once. Upstream's own `leds::update()` also runs every loop and redraws whenever one of *its* animations is set (the idle animation restarts whenever the launcher comes back), so on **every frame it draws** the service first calls `::leds::stopAnimation()`. When a pattern finishes or is stopped, the LEDs are turned off and, if the badge is idle (`vk::host::idle()`), upstream's idle animation resumes (`::leds::playIdle()`). While an app runs and no pattern is playing, the LEDs belong to the app.

| Name | When | Look | Ends |
|---|---|---|---|
| `approve_green` | approval open, green | slow green pulse, 1.5 s period | when the approval closes |
| `approve_amber` | approval open, amber | amber pulse, 1 s period | when the approval closes |
| `approve_red` | approval open, red | solid red | when the approval closes |
| `signed` | approval result: signed or approved | three quick green flashes (100 ms on, 100 ms off) | after 600 ms |
| `refused` | approval result: cancelled, timeout, blocked, failed | one red blink (on for 250 ms) | after 400 ms |
| `notify` | a notification is waiting and the badge is idle (`vk::host::idle()`) | dim breathe in the active theme's `LED` colour, 3 s period | when the inbox is empty or an app starts |
| `low_battery` | the battery enters the low or the critical level ([Low battery](#low-battery)), when no approval is open and no pattern is playing | two short red blinks at half strength (150 ms on, 150 ms off) | after 600 ms |

`leds.h` has no way to ask which pattern is playing, so `notify` ends itself: its frame returns false when no note waits, the badge is not idle, or an approval is open, and its service (in `host/notify.cpp`) starts it only when nothing is playing. It never calls `leds::stop()`, so it cannot stop an approval's pattern. "When an app starts" is therefore one LED frame after the app's `on_start`: an app that sets its LEDs once in `on_start` while a note is waiting loses them on that frame (accepted; apps set LEDs from `on_update` or `on_draw`).

Colours are the approval's fixed severity colours ([approval](../wallet/approval.md#screen)) and the active theme's `LED` token. No pattern uses upstream's brand purple or green. `low_battery` is defined in `src/vk/ui/leds.cpp` next to `refused`, with the same red.

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

Boot stages block, so the bar is drawn only inside `bootProgress()`, once per stage, from `boot::progress` through `vk::ui::bootScreen` ([shell](shell.md#boot)): no service runs during `setup()`, and upstream's `tick()` is its own `leds::update()`, which never reaches BadgeOS code. A stage therefore holds one flash state until the next stage begins, which is acceptable. The boot bar is not a registered pattern. There is no splash and upstream's boot animation (`leds::playBoot()`) is never played: the bar starts with the first boot frame, at 0 %, with every LED off. The colour is the active theme's accent ([Theme](#theme)).

### Adding a pattern

```cpp
static bool rainbow(uint32_t t) {
  for (uint8_t i = 0; i < RGB_LED_COUNT; ++i) { /* ::leds::set(i, r, g, b) */ }
  return t < 2000;
}
VK_LED_PATTERN(rainbow, "rainbow", rainbow);
```

Then `vk::ui::leds::play("rainbow")` from firmware, or name it in an `ApprovalRequest::led`. Add the row to the table above.

## Status bar (removed)

BadgeOS has no status bar and no status items. Upstream's 22 px bar belonged to upstream's shell, which is deleted; hook H14 and the `VK_STATUS_ITEM` registry (`src/vk/ui/statusbar.{h,cpp}`) went with it. Every screen has the receipt [header](#header) instead, whose right side is the time and the battery and nothing else. What the four items used to show now lives here:

| Former item | Now |
|---|---|
| `setup` (`SETUP` while unprovisioned) | the launcher's balance row reads `SETUP NEEDED` ([launcher](shell.md#launcher)) |
| `dev` (`DEV` in the dev profile) | the dev build is marked on the approval screen, where it matters ([approval](../wallet/approval.md#dev-builds)) |
| `inbox` (`[n]`) | the value of the launcher's `inbox` cell and of the Settings list's Inbox row |
| `balance` | the launcher's `BALANCE` row |

The one piece that stays is the repaint request, now in `src/vk/ui/repaint.{h,cpp}`: `vk::ui::requestShellRepaint()` asks the shell to redraw its top screen on its next pass, and the shell calls `vk::ui::consumeShellRepaint()` itself ([framework](shell.md#framework)). Whoever changes something the shell shows calls it: the balance feature on a new balance, the notification inbox on a change, the config store on provisioning or reset, the approval engine when it closes.

## Balance


`src/vk/features/balance/`. A service that, every `balance_poll_s` seconds, **only while the badge is idle (`vk::host::idle()`), no approval is open and the badge is joined to a network** (`wifi_mgr::mode() == wifi_mgr::Mode::Station && wifi_mgr::connected()`; `connected()` alone is also true in hotspot mode), makes one JSON-RPC call to `rpc_url`:

```json
{"jsonrpc":"2.0","id":1,"method":"getTokenAccountsByOwner",
 "params":["<this badge's address>",{"mint":"<default token mint>"},{"encoding":"jsonParsed"}]}
```

From the first account in the reply it keeps the account's address (`pubkey`) and `tokenAmount.amount`. The reply is scanned for those two fields with a small string search, not a JSON library, and any reply that does not contain both is ignored. Timeout 3 s.

**The poll does not block the loop.** The service builds the request in the loop and hands it to a background task (`vk_balance`, 12 KB stack, priority 1, on core 0, the radios' core; created at the first poll), which makes the one HTTP request the way `net_route`'s Wi-Fi route does (HTTPClient; for `https`, TLS without a pinned certificate, as before) and touches nothing else. The loop picks the answer up on its next pass and does everything else itself: the log line, storing the balance, the repaint request. A pass costs the same whether the node answers in 200 ms or never; before this the launcher froze for up to 3 s on every poll while the node was unreachable (gap audit B7). It is the one task BadgeOS starts, and the exception to "one task" in [overview §5](../architecture/overview.md#5-main-loop): the request is the one long thing that cannot be cut into loop-sized steps (a TLS handshake). The task never uses the phone bridge, whose pump belongs to the loop; the service polls only while joined to Wi-Fi anyway. If the task cannot be created, the service polls in the loop as before, with the back-off below.

**Back-off.** After a failed poll (no answer, an HTTP error, a reply without the two fields) the next one waits twice as long, then four times, and so on, up to `balance_max_s` (default 600 s, never below `balance_poll_s`); `retryDelayMs` in `balance.h`. A successful fetch, by the service or by an app's `wallet.refresh_balance()`, resets it. From the second failure in a row the log says `[bal] <n> polls failed in a row: next in <s> s`.

**Freshness.** The last balance stays known while it is stale (`tokenInfoLookup` is unchanged, so the launcher keeps showing the last measured figure); `vk::balance::status()` tells a screen how far to trust it: `freshness` is `offline` (not joined to Wi-Fi), `none` (no balance yet), `stale` (the last poll failed, or the balance is older than three poll periods) or `fresh`, with the balance's age, the number of failed polls, the time to the next poll and whether one is in flight. The rule is `freshness()` in `balance.h`, covered by the host suite `test_balance`. The launcher does not show it yet (its owner may add, for example, the value in `SUB` with `(stale)` or `(offline)` after it). While an app runs the service does not poll: an app that needs the balance or the token account calls `wallet.refresh_balance()` (Home does so on start and every `balance_poll_s`; `vk.pay` does so when `wallet.token_account()` is nil). Only the default (first) token is tracked; `wallet.balance(symbol)` and `wallet.token_account(symbol)` return nil for any other symbol. The token account address is what `wallet.token_account()` returns and what `build_transfer` uses as the default source; this is how the badge learns its own token account without deriving it.

```cpp
// src/vk/features/balance/balance.h
namespace vk::balance {
bool known();
uint64_t raw();
bool tokenAccount(uint8_t out[32]);
bool refresh(uint32_t timeoutMs);      // one blocking fetch
Status status();                       // freshness, age, failures, next poll (below)
}
```

As built, the header also has `Reason fetch(uint32_t timeoutMs)` (what `refresh` and `wallet.refresh_balance` call; its reasons are in the [Lua API](../platform/lua-api.md#badgewallet-balance)), `FETCH_TIMEOUT_MS` (3000) and the reply scanner `scanReply` as an inline pure function, tested in the host suite `test_stores`.

- The balance and the token account are always known together: a reply must hold both.
- A stored balance belongs to the mint it was fetched for. If provisioning makes another mint the default token, the balance counts as unknown until the next fetch.
- A fetch logs `[bal] fetch <ms> ms` (measurement M5), or `[bal] fetch failed after <ms> ms: …`; for the service's background poll `<ms>` is the request's own time, measured on the task. With Wi-Fi down the poll is skipped silently. `balance_poll_s 0` turns the poll off.
- The balance is shown on the launcher's `BALANCE` row ([shell](shell.md#launcher)); the feature calls `vk::ui::requestShellRepaint()` when the value changes.
- Device tests run with Wi-Fi off, so the poll never fires under the test provisioning. On a badge that is joined to a network, the test `rpc_url` (`http://127.0.0.1:8899`) makes every idle poll fail, now in the background and less and less often.
- `wallet.refresh_balance()` (an app asking) still makes one blocking fetch in the loop: the app is waiting for the answer anyway.

The feature publishes these to the rest of the firmware through `vk::wallet::tokenInfoLookup` ([signing](../wallet/signing.md#cross-feature-interfaces)), so `solana_pay` and the Wallet app never include a `balance` header.

## Screen dim and sleep

`src/vk/ui/screen_power.{h,cpp}` (service `screen_power`), logic in `src/vk/ui/power_core.c` (host suite `test_power`), hook H25. After `dim_s` seconds with no activity the backlight dims to `dim_pct` percent of its level; after `sleep_s` seconds it goes off. Any key wakes it, at the level it had, and **the key that woke it does nothing else**: its press and its release are dropped, and so is every key that was down at that moment, until each is up again. A key pressed after the wake, while the first is still held, is delivered.

| Counts as activity (restarts the timer, wakes the screen) | Holds the screen awake (no dim, no sleep) |
|---|---|
| a key pressed or held (a held key never lets the screen dim) | an approval is open (`vk::modalActive()`) |
| a new notification (the count grew) | an app called `badge.screen.keep_awake(true)` (Lua) or `vk::ui::screen::keepAwake(true)` (native); cleared when the app stops |
| an app starting or stopping | external power (`power::charging()`) when `awake_usb` is 1 |
| the app-store offer appearing | |
| a button injected by the dev profile's `VKBTN` | |
| firmware calling `vk::ui::screen::wake()` | |

- **The approval is never dimmed or slept through.** It holds the screen awake for as long as it is open; one that opens over a dimmed or dark screen is lit by the approval engine itself (it sets the user's level on open), and a key pressed on it is not swallowed. When it closes, the engine puts back the level it found, which was the dim or dark level: the service then restores the awake level.
- **A notification is not slept through:** a new one wakes the screen and restarts the timer. A note that has been waiting does not keep the screen on; the `notify` LED pattern keeps breathing while the screen is off.
- **What is changed:** the backlight only, and only on a transition. The service remembers the level it replaced and restores it on waking only if nobody changed it meanwhile (an app's `badge.gfx.brightness`). Asleep in the shell, the full-colour idle LED animation is stopped too and comes back on waking; a registered pattern (`notify`, an approval's) and an app's own LEDs are left alone.
- **Backlight only, by decision.** The panel is not put to sleep and the CPU does not light-sleep. In light sleep the loop stops: ESP-NOW frames (payment requests, presence proofs) and the USB console would be lost, and the expander's interrupt line is only a hint to a button poll that would stop with the loop. The backlight is the load worth removing. The shell and apps keep drawing as usual.
- **Wake key, exactly:** hook H25 calls `vk_screen_filter_buttons(down, &pressed, &released)` in `buttons::update()` after the debounce and before key repeat, hold timing and the dev profile's injected buttons ([hook](../architecture/upstream-hooks.md#h25--the-wake-key)). `buttons::down()` still reports a swallowed key that is held (a Lua app polling `badge.input.down` sees it); its press edge is gone, so its repeat and `heldMs` never start, and a CANCEL held to wake the screen is not counted towards the 1.5 s force-quit. Buttons injected with `VKBTN` are added after the hook and are never swallowed; they count as activity, so a device test that taps a dimmed badge behaves as before.
- **Critical battery:** at the critical level the screen sleeps after `crit_sleep_s` seconds when that is sooner than `sleep_s` ([Low battery](#low-battery)).
- **Seen from outside:** `VKINFO` has `display=awake|dim|sleep`; `VKSTATE`'s `backlight` is the level the panel is driven at, so it reads the dim level, and `0` while asleep. The log has `[vk] screen awake|dim|sleep` at each change. Settings → Display shows and sets `Sleep after` ([shell](shell.md#display)).

```cpp
// src/vk/ui/screen_power.h
namespace vk::ui::screen {
enum class State : uint8_t { AWAKE, DIM, SLEEP };
State state();  const char *stateName();
void keepAwake(bool on);  bool keepingAwake();   // for apps; cleared when the app stops
void wake();                                     // firmware with something to show
}
```

## Low battery

`src/vk/ui/battery.{h,cpp}` (service `battery`), thresholds in `power_core.c` (host suite `test_power`). Once a second it takes the same measured figure the header shows (`power::percent()`, rounded) and puts the battery in one of three levels: ok, low (at or below `batt_low_pct`, default 20 %) or critical (at or below `batt_crit_pct`, default 8 %). A level is left only `batt_hyst_pct` (default 3) above its threshold, because the cell sags under the radio's load and would otherwise cross the line again and again. On external power there is no cell reading and no level (the header says `USB`).

When the battery **enters** the low or the critical level, and only then:

- one notification through the notification inbox: `Battery low` / `17% left: charge it soon`, or `Battery critical` / `6% left: charge it now` (no app: SELECT in the Inbox dismisses it). It counts the way every note does: the launcher's inbox cell, the `notify` LED breath, and it wakes the screen;
- the LED pattern `low_battery` once, unless an approval is open or another pattern is playing;
- `[vk] battery low (17%)` in the log.

While the level lasts, the header's battery figure reads `LOW 17%` or `CRIT 6%`. Nothing is repeated while the level lasts, and nothing is said when it gets better.

At the **critical** level, beyond the warning: the screen sleeps after `crit_sleep_s` seconds (default 30) of no activity when that is sooner than `sleep_s`. Refusing to start a new signing approval at critical level (a new reason code, `low_battery`) is specified but not built: the check belongs in `vk::wallet::begin()` (`src/vk/wallet/signer.cpp`), which is the wallet core's file. `vk::ui::battery::critical()` is what it would ask.

`VKINFO` has `battery=ok|low|critical`.

```cpp
// src/vk/ui/battery.h
namespace vk::ui::battery {
uint8_t level();            // VK_BATT_OK, VK_BATT_LOW, VK_BATT_CRITICAL (power_core.h)
bool critical();
const char *levelName();    // "ok", "low", "critical"
}
```

## Theme

**Decided: the Receipt design, in a light and a dark mode, on every screen of the OS.** Reference: `docs/design/os-mockups/index.html`, a live simulation of every screen (serve the folder with `python3 -m http.server 8765`, open `http://127.0.0.1:8765`; keys 1 and 2 switch mode). When this document and the simulation disagree about a pixel, the simulation wins; when they disagree about behaviour, this document wins.

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
const Theme *find(const char *name);                // the registered theme with that name, or nullptr
bool colorByName(const char *name, uint16_t &out);  // the Lua names: a token ("paper", "stamp_ok", ...) or "green", "amber", "red"
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
| `LED` | boot bar, the `notify` pattern and the shell's LED pulses | `#FFE2AA` | `#FFC478` |

The three `STAMP_*` tokens keep the names of the first design; nothing draws a stamp. They colour text (through `row`'s `valueColor`, for example), never a band.

The config key `theme` holds the active theme's name; empty or an unknown name means `receipt-light`. `color()` works before `vk::begin()` (the config accessors start the store themselves) and re-reads the key every 500 ms, so `VKSET theme receipt-dark` shows on the next frame without a reboot. If `setActive`'s write is refused, the stored name wins again at the next read. A selected row is drawn inverted: `INK` fill, `PAPER` text. The three **severity colours are not tokens**: green `#1FBF75`, amber `#FFB020`, red `#FF4545` in both modes, with black text on them ([approval](../wallet/approval.md#screen)).

Adding a theme is one `VK_THEME(...)` line with eight colours; it then appears in Settings → Theme ([shell](shell.md#theme)), which cycles through the registered themes. Removing one is deleting that line.

### Fonts

All from the graphics library (LovyanGFX), no font files to ship:

| Role | Font | Size on screen |
|---|---|---|
| body, labels, header, footer | `fonts::Font0` | 6×8 px per character, 53 columns |
| band headline, section titles (`MENU`, `PAY`) | `fonts::FreeMonoBold9pt7b` | about 11 px per character |
| amounts | `fonts::FreeSerifBold24pt7b`; `FreeSerifBold18pt7b` when the text is wider than 136 px; then `FreeSerifBold9pt7b`, then `Font0`, so that an amount is never cut and never crosses the perforation | |
| brand line "BadgeOS" on the boot screen | `fonts::FreeSerifBoldItalic12pt7b` | |

All the names exist in the installed LovyanGFX 1.2.32 (`lgfx_fonts.hpp`), as does `textWidth`. `FreeSerifBold9pt7b` is also used, for wrapped text in the approval's left stub and as the third amount size. If a later library version drops one, fall back to the numbered fonts `Font4` (amounts) and `Font2` (titles).

### The receipt kit

Every BadgeOS screen is drawn with one small set of functions, so that screens stay consistent and a new screen is a few calls. `src/vk/ui/receipt.{h,cpp}`:

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
bool barcodeText(int x, int y, int w, int h, const char *text);                     // scannable Code 128 (ui/code128.c); false if it does not fit
void holdBar(float progress);                             // y 204..209, x 40..280
void footer(const char *left, const char *right);         // dashed rule at y=216, text at y=224
void headerText(const char *left, const char *right);     // header() without its rule: the approval's band sits directly under it
void title(const char *text, int y, int cx);              // title() centred on cx (the body column of a two-column screen is centred on 233)
bool qr(int x, int y, int size, const char *text);        // QR code of `text` centred in a light size x size square; false when nothing was drawn
}
```

Conventions every kit function follows (also in the comment at the top of `receipt.h`):

- A `y` is the top of the capital letters. A row at `y` owns y-5 to y+12; a selected row fills x0-10 to x1+10. A subline goes at y+13 and owns the 13 px below it.
- Text that does not fit its space is cut and ends in `..`.
- Body text is `Font0`. Three characters outside ASCII are drawn by hand, one column wide, when written as UTF-8: the middle dot `·` (U+00B7) and the triangles `◂` (U+25C2) and `▸` (U+25B8); LovyanGFX's own string drawing cannot reach them. Any other byte outside `0x20`–`0x7E` is drawn as `?`. `display::text` does not do this: text with a middle dot must go through the kit.
- `statusRight` separates its parts with that middle dot (`14:32 · 87%`, or `14:32 · USB` on external power). Below a battery threshold the battery part carries one word before the figure: `14:32 · LOW 17%`, `14:32 · CRIT 6%` ([Low battery](#low-battery)). The time is UTC (there is no timezone key) and is left out when the clock has no source, leaving `87%` or `USB` alone. Seen on the badge: `03:00 · USB` and `USB` (`os/test/device/shots/header_clock_usb.png`, `header_noclock_usb.png`).
- Upstream never sets a font on the canvas, so every kit function leaves it on `Font0`, size 1, top-left datum. Code that sets a font on the canvas must put it back.
- The footer clears its strip to `PAPER` first and its rule runs edge to edge (x 0..319), as in the simulation. The hold bar is 240 px wide (x 40..279) over a half-tone `FAINT` track. Sublines and `sub` are not bold (`Font0` has no bold).

**The QR code.** `qr(x, y, size, text)` fills the `size` × `size` square at (x, y) and draws the QR code of `text` (a link, at most 154 bytes) centred in it. The square is always the **light theme's** paper and the modules its ink, whatever the active theme: a phone reads dark modules on a light ground only, so in the dark theme the code sits on a light patch. The encoder is the one inside the graphics library (`lgfx_qrcode`, what LovyanGFX's own `canvas.qrcode()` uses; that function is not called, because it draws pure white and black). It takes the smallest QR version that holds the text, at the lowest error correction, up to version 7. A module is as many whole pixels as leave the standard quiet zone of 4 modules inside the square, or of 2 modules when that makes the modules larger; the rest of the square is margin. It returns false and draws nothing for an empty text, a text over 154 bytes, or a square too small for 1 px modules. The last code is kept, so a screen that redraws the same link does not encode it again. Sizes in use: 148 px on [About](shell.md#about) (a link of up to 53 characters is 29 modules: 4 px each with a 4-module quiet zone) and 102 px in Home's stub (3 px each, 7 px of margin). Both were read back from a screenshot with OpenCV's QR detector in both themes (`t_about.py`, `t_app_home.py`).

Layout constants (pixels): margins 10; list row pitch 18; subline 13 below its row; two-column split at x = 146 (left stub 0..145, body 147..319); content starts at y = 24 under the header. The barcode's bars come from the badge's public key, so each badge prints its own.

**Text entry.** The kit has one input component, the on-screen keyboard (`src/vk/ui/keyboard.{h,cpp}`): a whole screen on which a text is typed with the six buttons, opened by a shell page (`keyboard::open`) or driven by a native app (`begin`, `update`, `draw`). Its layout, its buttons and its API are in [text-entry.md](text-entry.md).

### Header

The left side of every header is `BADGEOS`. The right side of every header is `receipt::statusRight`: the time and the battery, and nothing else. No notification count, no setup or dev marker (waiting notifications show on the launcher's Inbox row; an unprovisioned badge says so on the launcher's balance row; the dev build is marked on the approval screen, where it matters).

The battery figure is the measured one. The badge has no fuel gauge, so the only real measurement is the cell voltage, which upstream's `power::percent()` maps to a percentage. When the badge is on external power (`power::charging()`, cell line above 4.25 V) the ADC is reading the charger, not the cell, and any percentage would be invented: the header shows `USB` instead. Nothing is predicted (no "time left"). While the battery is at the low or critical level the figure is marked `LOW` or `CRIT` ([Low battery](#low-battery)); the mark is part of the battery figure, and the right side still holds only the time and the battery.

### Screens

| Screen | Layout |
|---|---|
| Boot | [shell: Boot](shell.md#boot). Header `BADGEOS` / `*** STARTING UP ***`; left stub: brand line `BadgeOS`, percent as an amount, a 14-cell block bar, the stage's detail text; body: title `CHECKLIST`, one row per stage with `OK`, `..` or blank. No splash images come before it |
| Launcher | [shell: Launcher](shell.md#launcher). Header; title `MENU`; apps in a 2-column grid of rows `NN NAME`, selected row inverted with `◂` (the `inbox` row shows the number of waiting notifications as its value when there are any); rule; row `BALANCE … 142.50 HACK`, or `SETUP NEEDED` while the badge is unprovisioned; barcode; footer `SELECT open` / `CANCEL settings` |
| Settings list, every settings page, delete confirmation, app-store offer, installing, app error | [shell](shell.md#settings-list): each is a receipt list or ticket drawn with the kit |
| About | [shell: About](shell.md#about). Left stub: the name `BadgeOS` as a title, a rule, rows VERSION, API, KEY, ADDRESS; body: the QR code of config key `repo_url` (148 px) and the link as text under it, or `no link set` |
| Home | left stub: `BALANCE` amount, then the QR code of `repo_url` (102 px), or the barcode when that key is empty; body: rows NAME, ADDRESS, KEY, CLOCK; a rule; the app menu (four rows, scrolling); a rule; `THANK YOU FOR HACKING`. No INBOX row: the count is not readable from Lua, and the launcher and Settings show it ([apps](../apps/apps.md#home)) |
| Any list (Pay, History, Contacts, Inbox, Wallet, shop) | header; title at y = 26; rows from y = 46 with optional sublines: pitch 18 without sublines (9 rows, the last at y = 190) or 31 with them (5 rows, ending at y = 196); the line shown when the list is empty is centred at y = 112 in `SUB`; footer with the action and `CANCEL back`. The native Inbox and Wallet and `vk.ui.list` all use these positions |
| Approval | [approval](../wallet/approval.md#screen) |
| Request | left stub: amount with label `PAY ME`, `WAITING` or `RECEIVED`, barcode; body: title `REQUEST`, three rows. When the payment is confirmed the left stub's label reads `PAID`, drawn in the `STAMP_OK` colour |

**Lua apps render through the same kit.** `lib/vk.lua`'s `vk.ui` helpers (`vk.ui.header`, `row`, `title`, `amount`, `footer`, `rule`, `qr` and the rest) each call the firmware function of the same name through the Lua module `badge.receipt` ([Lua API](../platform/lua-api.md#badgereceipt), `src/vk/ui/lua_receipt.cpp`; no permission). A Lua app's screen is therefore drawn by `receipt.cpp` itself: the same title font as the shell (the glyphs of `HISTORY` in the History app and of `SETTINGS` in the shell are pixel-identical), the same serif amounts, and one call into C per element instead of one per text run, rectangle and leader dot. On a firmware without `badge.receipt`, and in the host test, `vk.ui` falls back to its own drawing with `badge.gfx` and colours from `badge.theme.color(name)`, where a title and an amount are the built-in font scaled up; the geometry is the same. `badge.theme.color` and `badge.theme.name()` are registered Lua functions with no permission.

### Launcher and settings

The launcher, the settings list and every settings page are BadgeOS's own shell, specified in [shell.md](shell.md). There is no launcher app, no settings app and no home service: when no app runs, the shell is on screen, and it starts on the launcher. An app that exits, crashes or is force-quit lands on the launcher (or on the shell's app-error screen, then the launcher).

`vk::host::idle()` is what "the badge is idle" means everywhere in these documents: no app is running (`!runtime::running()`), so the shell is showing. The balance poll, the `notify` LED pattern and the idle LED animation ask it.

```cpp
// src/vk/host/home.h
namespace vk::host { bool idle(); }
```

Upstream's compile-time palette (`src/ui/theme.h`) is set to the Receipt-light values ([replaced upstream files](../architecture/upstream-hooks.md#replaced-upstream-files)) so that anything upstream still draws matches; no BadgeOS screen uses it, and Lua has no `gfx.SOLANA_*` constants: apps take their colours from `badge.theme.color`.
