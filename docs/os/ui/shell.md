# The BadgeOS shell

Everything the badge shows when no app is running: the boot screen, the launcher, the settings list and every settings page, the delete confirmation, the app-store offer and installing screens, and the app-error screen. Files: `src/vk/shell/`, `src/ui/boot.cpp`, `src/vk/ui/boot_screen.cpp`.

**The UI is ours as a whole.** Upstream's shell (`src/ui/shell.cpp`) is deleted and rewritten here in the Receipt layout ([ui](ui.md#theme)). It is not an app on top of upstream's launcher: the launcher *is* the shell. Nothing on any screen, and no network identifier, says "Solana" or "SKYRIZZ" or carries upstream's purple and green ([names](../architecture/upstream-hooks.md#replaced-upstream-files)).

Design reference: `docs/design/os-mockups/index.html` (boot, launcher/MENU, lists, the settings list). When this document and the simulation disagree about a pixel, the simulation wins; about behaviour, this document wins. The simulation was drawn before the rename and still prints `Badge OS` and `BADGE OS` (two words): the name on the device is `BadgeOS`, and `BADGEOS` in headers. Upstream's shell at the last commit that has it (`git show 89eadec:os/src/ui/shell.cpp`) is the reference for behaviour: every call it makes is listed below, so none is lost.

## Source layout

```
src/ui/shell.h                 upstream, untouched: the interface os.ino and the runtime call
                               (shell::begin, update, onAppStopped, showError)
src/ui/boot.h                  upstream, untouched (boot::run, boot::progress)
src/ui/boot.cpp                rewritten: no splash (see Boot)
src/vk/ui/boot_screen.cpp      vk::ui::bootScreen(): draws the Receipt boot screen
src/vk/ui/repaint.h .cpp       vk::ui::requestShellRepaint(), consumeShellRepaint()
src/vk/shell/
  screens.h                    framework API: Screen, push, pop, home, repaint; ::shell::screenName()
  shell.cpp                    the framework: screen stack, timers, the four upstream functions, screenName()
  page.h  page.cpp             the settings-page registry and the helpers every page draws with
  launcher.cpp                 screen `launcher`
  settings_list.cpp            screen `settings`: lists the registered pages
  dialogs.cpp                  screens `app_delete`, `app_error`, `offer`, `installing`
  edit.h  edit.cpp             changing a config key from the buttons, screen `pick`, the jump to a page
  setup.h                      the setup checklist for the launcher (implemented in pages/page_setup.cpp)
  setup_core.h  setup_core.c   pure C: the checklist and the number stepper (host suite test_setup)
  pages/page_<id>.cpp          one file per settings page; each registers itself
```

A settings page is added by adding one file under `pages/` and removed by deleting it. No other file names a page.

Namespaces: upstream's four functions and `screenName()` are in the global namespace `shell`; everything else is `vk::shell`. From code inside `namespace vk`, write `::shell::screenName()`.

## Framework

`src/vk/shell/screens.h`, complete:

```cpp
#pragma once

#include <Arduino.h>

namespace shell {
const char *screenName();          // name of the screen on top ("launcher", "wifi", ...); "" while an app runs
}

namespace vk::shell {

struct Screen {
  const char *name;                // what screenName() and VKSTATE report: [a-z_], unique
  void (*enter)();                 // called each time the screen is pushed; may be nullptr
  void (*update)();                // called every loop pass while the screen is on top: reads buttons
  void (*draw)();                  // full repaint of the canvas (the main loop flushes it)
  uint32_t refresh_ms;             // 0 = repaint only on request; otherwise also every refresh_ms
};

void push(const Screen *screen);   // show `screen` on top. The object must be static. Calls enter(), requests a repaint
void pop();                        // back one screen (not re-entered: it keeps its cursor); does nothing on the launcher
void home();                       // back to the launcher
void repaint();                    // redraw the top screen on this pass
const Screen *top();

// The screens the framework itself opens.
extern const Screen kLauncher;     // launcher.cpp
extern const Screen kSettings;     // settings_list.cpp
extern const Screen kAppDelete;    // dialogs.cpp
extern const Screen kAppError;     // dialogs.cpp
extern const Screen kOffer;        // dialogs.cpp
extern const Screen kInstalling;   // dialogs.cpp
void appDeleteOpen(const String &id, const String &name);   // remembers the app, then push(&kAppDelete)
void appErrorSet(const String &message);                    // the text kAppError shows

}  // namespace vk::shell
```

The stack holds at most 6 screens (`launcher` → `settings` → a page → a sub-screen → a dialog); the launcher is always at the bottom.

The four functions upstream's `shell.h` declares, as `shell.cpp` implements them:

| Function | Does |
|---|---|
| `shell::begin()` | `app_store::refresh()`; the stack becomes `[launcher]`; repaint |
| `shell::update()` | one pass, below |
| `shell::onAppStopped()` | If `runtime::lastError().length()`: `showError(runtime::lastError())`. Otherwise `home()` and `::leds::playIdle()`. The launcher keeps its cursor (clamped to the app count); upstream reset it to the first row. **No `app_store::refresh()`**, which upstream's shell called here: the scan walks every app folder (measured 1.2 s with seventeen Lua apps installed) and froze the badge each time an app exited. The list cannot be stale, because every path that installs or removes an app rescans by itself (serial and BLE push, the web page, the store client, `app_store::removeApp`). What can be stale is an app's `SIZE` in the delete confirmation, by the data the app saved since the last scan |
| `shell::showError(message)` | `appErrorSet(message)`; the stack becomes `[launcher, app_error]`; `pulseLedBad(700)` |
| `shell::screenName()` | `runtime::running() ? "" : top()->name` |

One pass of `shell::update()` (the main loop calls it only when no app runs and no approval is up):

```
0. if millis() - lastPassEnd >= 300: dirty      (the shell was paused: an app or an approval drew on the canvas)
1. if broker::hasOffer() and top is neither kOffer nor kInstalling:
       push(&kOffer); pulseLed(900); skip step 2 on this pass
2. top->update()                      (may push, pop or home; input first, so a switch is drawn on this pass)
3. if top->refresh_ms and millis() - lastDraw >= top->refresh_ms: dirty
   every 500 ms: if the header text (receipt::statusRight) or theme::color(PAPER) differs from the last draw: dirty
4. if vk::ui::consumeShellRepaint(): dirty
5. if dirty: top->draw(); lastDraw = millis()
6. lastPassEnd = millis()
```

**The shell draws only when something changed.** A draw is about 10 ms and the transfer of the canvas to the panel that follows it is 34 ms; an idle screen costs neither, and a loop pass is then about 1 ms, which is what makes a key press feel immediate ([measurements](../testing/testing.md#responsiveness)). Step 0 is a safety net: every known pause already asks for a repaint (`home()` when an app stops, `requestShellRepaint()` when an approval closes). Its gap is measured from the **end** of the last pass, not its start: measured from the start, a page whose own draw took longer than 300 ms looked like a pause and was redrawn on every pass (Device info did exactly that at three passes a second, before its slow read was moved out of `draw()`).

Step 1 skips input on the pass the offer appears: the button edges of that pass were computed before the prompt existed, and a SELECT meant for the previous screen must not count as consent to install code. Upstream did the same with `sOfferJustRaised`. The offer is not raised over `installing`, so the result of the last install is read before the next offer replaces it; and never while an app runs, because the main loop does not call the shell then: the offer is still waiting when the app exits.

Step 3's 500 ms check is what keeps the clock, the battery figure and a theme change (`VKSET theme …`) current on every screen without a blind repaint.

`src/vk/ui/repaint.h`, complete (the pair moved unchanged from the former `statusbar.h`):

```cpp
#pragma once
namespace vk::ui {
void requestShellRepaint();   // ask the shell to redraw its top screen on its next pass
bool consumeShellRepaint();   // the shell: true once per request
}
```

Callers of `requestShellRepaint()`: the approval engine when it closes, the config store on provisioning or reset, the balance feature on a new balance, the notification inbox on a change.

## page.h

`src/vk/shell/page.h`, complete. It is the only shell header a page file includes. `page.cpp` implements the functions.

```cpp
#pragma once

#include <Arduino.h>

#include "../../config.h"          // BTN_UP, BTN_DOWN, BTN_LEFT, BTN_RIGHT, BTN_A (SELECT), BTN_B (CANCEL)
#include "../../hal/buttons.h"     // buttons::pressed, repeated, released, down
#include "../../hal/display.h"     // display::canvas, width, height
#include "../core/registry.h"
#include "../ui/receipt.h"         // vk::ui::receipt::*, vk::ui::theme::*
#include "screens.h"

namespace vk::shell {

// ---- layout, in pixels (ui.md, "The receipt kit") ----
constexpr int X0 = 10;             // left edge of a full-width row
constexpr int X1 = 310;            // right edge
constexpr int TITLE_Y = 26;        // page title
constexpr int LIST_Y = 48;         // first row: a row at y owns y-5 .. y+12
constexpr int ROW_PITCH = 18;
constexpr int LIST_ROWS = 9;       // rows at 48 .. 192; the last one ends at y = 204
constexpr int TEXT_COLS = 50;      // Font0 characters between X0 and X1

// ---- the registry ----
struct SettingsPage : Registered<SettingsPage> {
  const char *id;                          // the page's screen name ("wifi"): [a-z_], unique
  int order;                               // position in the Settings list, ascending
  const char *title;                       // the row's label in the Settings list ("Wi-Fi")
  void (*value)(char *out, size_t cap);    // the row's right-hand text; nullptr = none. Called on every repaint of the list
  void (*action)();                        // an action row: SELECT calls this and the list stays. nullptr for a page
  void (*enter)();                         // a page: called each time it opens; may be nullptr
  void (*update)();                        // a page: every loop pass while on top
  void (*draw)();                          // a page: full repaint
  uint32_t refresh_ms;                     // a page: 0, or the repaint period
  SettingsPage(const char *i, int o, const char *t, void (*v)(char *, size_t), void (*a)(),
               void (*e)(), void (*u)(), void (*d)(), uint32_t r)
      : id(i), order(o), title(t), value(v), action(a), enter(e), update(u), draw(d), refresh_ms(r) {}
};
// A row that opens a page.
#define VK_SETTINGS_PAGE(ident, id, order, title, value_fn, enter_fn, update_fn, draw_fn, refresh_ms) \
  static vk::shell::SettingsPage vk_page_##ident(id, order, title, value_fn, nullptr, enter_fn, update_fn, draw_fn, refresh_ms)
// A row that acts in place (cycle a value, launch an app).
#define VK_SETTINGS_ACTION(ident, id, order, title, value_fn, action_fn) \
  static vk::shell::SettingsPage vk_page_##ident(id, order, title, value_fn, action_fn, nullptr, nullptr, nullptr, 0)

// ---- input ----
bool back();                       // CANCEL pressed on this pass: pop() and return true

// ---- drawing ----
// page(); header("BADGEOS", statusRight); title(title, TITLE_Y); footer(footLeft, footRight).
void frame(const char *title, const char *footLeft, const char *footRight = "CANCEL back");
// Font0 ASCII text in a theme colour, cut to maxCols with "..". y is the top of the capitals.
void text(int x, int y, const char *s, vk::ui::theme::Token token = vk::ui::theme::INK, int maxCols = TEXT_COLS);
void textCentered(int cx, int y, const char *s, vk::ui::theme::Token token = vk::ui::theme::INK);
// A block bar: `cells` cells of 6x8 px, 2 px apart, from x. round(value * cells / max) cells are filled INK; the rest are outlined FAINT.
void bar(int x, int y, int cells, uint32_t value, uint32_t max);
void pulseLed(uint16_t ms);        // ::leds::pulse in the active theme's LED colour
void pulseLedBad(uint16_t ms);     // ::leds::pulse(0xFF, 0x45, 0x45, ms): the fixed severity red
const char *onOff(bool on);        // "on" / "off"
uint16_t onOffColor(bool on);      // theme STAMP_OK when on, SUB when off

// ---- lists ----
struct ListRow { const char *label; const char *value; uint16_t valueColor; };   // valueColor 0 = ink
struct List { int cursor = 0; int scroll = 0; };
// UP/DOWN with key repeat (buttons::repeated), wrapping at both ends, keeping the cursor inside the
// `visible` window. Returns true when the cursor moved (it has already called repaint()).
bool listMove(List &list, int count, int visible = LIST_ROWS);
// Draws rows scroll .. scroll+visible-1 with receipt::row(X0, X1, y0 + i * ROW_PITCH, ...); the row under
// the cursor is selected (inverted). When count > visible it also draws "n/N" (selected row / rows)
// right-aligned at (X1, TITLE_Y + 2) in FAINT. Pass a List with cursor -1 for rows that cannot be
// selected: such a list has no mark.
void listDraw(const List &list, const ListRow *rows, int count, int y0 = LIST_Y, int visible = LIST_ROWS);

// ---- additions (the shell's owner may add declarations below this line; nothing above changes) ----

// Font0 text whose right edge is at xRight (the "n/N" scroll mark of a list or of the launcher).
void textRight(int xRight, int y, const char *s, vk::ui::theme::Token token = vk::ui::theme::INK);
// Swaps the top screen for `screen` and calls its enter() (offer -> installing). On the launcher it pushes.
void replaceTop(const Screen *screen);
// The stack becomes [launcher, screen]; calls enter(). The launcher is not re-entered: it keeps its cursor.
void showOver(const Screen *screen);

}  // namespace vk::shell
```

As built:

- `pulseLed(ms)` is `vk::ui::leds::pulseTheme(ms)` (`src/vk/ui/leds.h`), the same function the push code calls through hook H23.
- `listDraw` draws the value of the selected row in the paper colour, like its label: a status ink on the ink ground would not be readable. The `n/N` mark is always the selected row and the number of rows (`11/14` on the Settings list, as on the launcher and the offer). With cursor −1 (nothing selectable) no mark is drawn: it used to show the number of the last visible row (`9/12` on Device info), which read as a selection that does not exist. Such a page says in its footer that it scrolls.
- `text()` cuts at `maxCols` characters; two pages pass their own value where a line of the specified text is longer than 50 columns from its x (New identity, 52) or starts at x = 22 (App push, 48).

Rules for every screen and page:

- Draw only with the receipt kit and the helpers above, in theme colours. No literal colour, no upstream `::theme::` constant, no `display::statusBar`.
- The header's left text is `BADGEOS`; its right side is `receipt::statusRight` ([header rule](ui.md#header)). Titles are upper case.
- `update()` handles CANCEL itself: `if (back()) return;`. CANCEL always goes back; no screen traps it.
- State lives in file-static variables; `enter()` resets it. A `ListRow` holds pointers: keep the strings in named locals that outlive the `listDraw` call (upstream's `savedLabel` note applies).
- A page blocks the loop no longer than the upstream call it makes.
- `pages/page_identity.cpp` is the one shell file that includes `src/identity/identity.h` (to show the badge ID, the key location and the public key, and for `identity::regenerate()`). It never signs: `identity::sign` stays behind the wallet core, and pre-flash check 2 covers the shell like any other folder.

## Settings page registry

`settings_list.cpp` collects the registered `SettingsPage` objects at `enter()`, sorted by `order` (ties by `id`), at most 24. The shipped rows:

| Order | Id | Label | Kind | Value shown in the list | File | Batch 5 agent |
|---|---|---|---|---|---|---|
| 10 | `theme` | Theme | action | active theme name without its `receipt-` prefix (`light`, `dark`) | `pages/page_theme.cpp` | 5F |
| 20 | `wifi` | Wi-Fi | page | `wifi_mgr::statusText()` | `pages/page_wifi.cpp` | 5F |
| 30 | `bluetooth` | Bluetooth | page | `on` / `off` | `pages/page_bluetooth.cpp` | 5F |
| 40 | `espnow` | ESP-NOW | page | `off`, or `<n> peer` / `<n> peers` | `pages/page_espnow.cpp` | 5F |
| 50 | `push` | App push | page | `ready` when `push_server::running()` (so also on the badge's own hotspot), else `needs wi-fi` | `pages/page_push.cpp` | 5F |
| 60 | `store` | App store | page | `off` when disabled or no address is set; otherwise `broker::stateText()` | `pages/page_store.cpp` | 5G |
| 70 | `identity` | Identity | page | `identity::badgeId()`, or `not ready` | `pages/page_identity.cpp` | 5G |
| 80 | `display` | Display | page | backlight as `NN%` | `pages/page_display.cpp` | 5G |
| 90 | `leds` | LEDs | page | LED brightness as `NN%` | `pages/page_leds.cpp` | 5G |
| 100 | `wallet` | Wallet | action | `provisioned` / `setup needed` | `pages/page_wallet.cpp` | 5G |
| 110 | `inbox` | Inbox | action | the number of waiting notifications, or nothing | `pages/page_inbox.cpp` | 5G |
| 120 | `info` | Device info | page | `SOLANA_OS_VERSION` (`0.1.0`) | `pages/page_info.cpp` | 5G |
| 130 | `console` | Console | page | — | `pages/page_console.cpp` | 5G |
| 140 | `about` | About | page | the host of config key `repo_url` (`github.com`), or `not set` | `pages/page_about.cpp` | added after the close-out |
| 142 | `badge` | Badge | page | config key `display_name`, or `name, time` when it is empty | `pages/page_badge.cpp` | on-badge settings |
| 144 | `setup` | Setup | page | `<n> steps left` / `1 step left` / `done` (`setup::summary`) | `pages/page_setup.cpp` | on-badge settings |
| 146 | `advanced` | Advanced | page | `all settings` | `pages/page_advanced.cpp` | on-badge settings |
| 150 | `restart` | Restart | page | — | `pages/page_restart.cpp` | added with screen sleep |

Badge, Setup and Advanced sit after About on purpose: the device tests' `common.SETTINGS_ROWS` finds a page by its index in the list, so a row inserted above About would move every page after it. A page list read from the badge (`VKPAGES`, gap audit item 22) would free the order.

When SELECT is pressed on a page row, the list copies the page's `id`, `enter`, `update`, `draw` and `refresh_ms` into one static `Screen` and pushes it (only one page is open at a time). On an action row it calls `action()` and repaints.

## Screen names

`shell::screenName()` returns one of these; device tests read it as the `screen` field of `VKSTATE` ([testing](../testing/testing.md#dev-hooks)).

`launcher` · `app_delete` · `settings` · `wifi` · `bluetooth` · `espnow` · `push` · `store` · `identity` · `identity_new` · `display` · `leds` · `info` · `console` · `about` · `badge` · `setup` · `advanced` · `pick` · `restart` · `app_error` · `offer` · `installing`

It is empty while an app runs. While an approval is open over the shell it keeps the name of the screen underneath (`VKSTATE.modal` tells). The boot screen has no name: nothing can ask during `setup()`.

## Boot

No splash images. Power-on goes straight to the Receipt boot screen, with the LED boot bar ([rule](ui.md#boot-bar)).

`src/ui/boot.cpp` is ours now (a [replaced upstream file](../architecture/upstream-hooks.md#replaced-upstream-files)); `os.ino` still calls `boot::run()` once and `boot::progress(step, detail, percent)` as each stage begins. The whole file:

```cpp
#include "boot.h"

#include "../hal/display.h"
#include "../settings.h"
#include "../vk/ui/leds.h"

namespace boot {

void progress(const char *step, const char *detail, uint8_t percent) {
  vk::ui::bootScreen(step, detail, percent);   // draws the screen, flushes it, advances the LED boot bar
}

void run() {
  display::setBrightness(settings::brightness());   // upstream's splash used to fade the backlight in
  progress("", "power on", 0);
}

}  // namespace boot
```

`leds::playBoot()` is no longer called and `splash_images.h` is deleted. `vk::ui::bootScreen` (declared in `src/vk/ui/leds.h`, defined in `src/vk/ui/boot_screen.cpp`) always returns true. It runs before `vk::begin()`: it may use only `theme::color` (which starts the config store itself) and the receipt kit.

Layout:

| Element | Call and position |
|---|---|
| paper | `receipt::page()` |
| header | `receipt::header("BADGEOS", "*** STARTING UP ***")` |
| perforation | `receipt::perforation(146, 24, 232)` |
| brand line | `BadgeOS` in `fonts::FreeSerifBoldItalic12pt7b`, centred on x = 73, top at y = 36, `INK` |
| percent | `receipt::amount(73, 62, "", "<n>%", "")` |
| block bar | `bar(18, 132, 14, percent, 100)`: 14 cells, x 18..127 |
| detail | `detail`, Font0, centred on x = 73 at y = 150, cut to 22 characters |
| checklist title | `receipt::title("CHECKLIST", 28, 233)` |
| stage rows | `receipt::row(157, 310, 50 + 18 * i, NAME, mark)` for the seven stages |

The seven stages and the percent each one begins at: `STORAGE` 20, `PERIPHERALS` 45, `IDENTITY` 55, `RUNTIME` 65, `WALLET` 75, `RADIOS` 85, `READY` 100. A row's mark is `OK` when its percent is below the current percent, `..` when equal, blank when above; at 100 every row is `OK`. At 0 (the frame `boot::run()` draws) every row is blank. There is no footer and no version text, as in the simulation. (The simulation puts `..` on the stage after the current one; the rule here is the one built: `..` marks the stage that is running.)

The brand line is drawn on a baseline at y = 51, which assumes a capital height of 16 px for `FreeSerifBoldItalic12pt7b`; that figure was not measured. `boot_screen.cpp` includes `../shell/page.h` for `bar()` and `text()`, which need only `theme::color` and the canvas and so are safe before `vk::begin()`.

After drawing: `display::touch()`, `display::flush()`, then `vk::ui::leds::bootProgress(percent)` (one frame of the LED boot bar). Boot stages block, so the screen changes once per stage.

Buttons: none. The screen cannot be captured over serial (no command is read during `setup()`); a person checks it (T-LED1).

## Launcher

Screen `launcher`: the simulation's MENU screen. It lists every installed app, Lua and native, through `app_store::count()` and `app_store::at(i, info)` (native apps follow the Lua apps, hook H11).

| Element | Call and position |
|---|---|
| frame | `receipt::page()`, `receipt::header("BADGEOS", statusRight)`, `receipt::title("MENU", 26)` |
| grid | two columns: left `x0 = 10, x1 = 150`, right `x0 = 170, x1 = 310`; six rows at y = 48, 66, 84, 102, 120, 138. Cell (row r, column c) shows app index `(scrollRow + r) * 2 + c` |
| cell | `receipt::row(x0, x1, y, "NN NAME", value, selected)`: `NN` is the index + 1 with two digits, `NAME` is `info.name` in upper case, cut to 16 characters so the value always fits the 23-column cell. Value: `◂` on the selected cell; for the app whose id is `inbox`, the waiting-notification count when it is above 0, and `<count> ◂` when that cell is the selected one (the count must not disappear under the cursor); otherwise empty |
| scroll mark | when there are more than 12 apps: `n/N` (selected index + 1 / count) right-aligned at (310, 28), `FAINT` |
| rule | `receipt::rule(156)` |
| balance | `receipt::row(10, 310, 166, "BALANCE", "<amount> <symbol>")`. Unprovisioned (`!vk::config::provisioned()`): `receipt::row(10, 310, 166, "SETUP NEEDED", "provision over USB", false, STAMP_WARN)` |
| barcode | `receipt::barcodeText(10, 184, 300, 22, id)`: a Code 128 barcode a scanner app reads back as the badge ID (the first 8 characters of the address), dark on a light patch in both themes, 2 px per module. If it does not fit, the decorative `receipt::barcode(…, vk::wallet::publicKey(), 32)`. Not drawn when the badge has no identity |
| footer | `receipt::footer("SELECT open", "CANCEL settings")` |

Balance text: the default token is the first entry of `vk::config::tokens()`; `vk::wallet::tokenInfoLookup(mint, info)` gives its balance; the amount is formatted with `sol_format_amount` and followed by the symbol. `--` and the symbol when the pointer is null (the balance feature is absent) or the balance is not known yet.

With no app at all (not possible while the native apps are compiled in): `NO APPS INSTALLED` centred at y = 84 in `INK` and `push one: Settings > App push` at y = 102 in `SUB`.

Buttons:

| Button | Action |
|---|---|
| UP / DOWN | one row up or down (index ∓ 2), with key repeat; wraps between the first and last row, clamped to the last app |
| LEFT | to the left column |
| RIGHT, released within 600 ms | to the right column, when an app is there. A release between 600 and 800 ms does nothing |
| RIGHT, held 800 ms | **delete**: if the selected app is a Lua app (`!vk::host::native::exists(info.id)`): `appDeleteOpen(info.id, info.name.length() ? info.name : info.id)`. A native app cannot be deleted; nothing happens. The release that follows is ignored |
| SELECT | `::leds::stopAnimation(); runtime::requestLaunch(info.id);` |
| CANCEL | `push(&kSettings)` |

RIGHT is read from `buttons::pressed`, `buttons::down` and `buttons::released` with the press time kept by the launcher. Upstream's launcher was a single column with RIGHT = delete; the grid needs RIGHT for the column, so delete moved to the hold.

Refresh: on input and on a repaint request; `refresh_ms` is 0. In `update()` the launcher also compares `app_store::count()` and `vk::host::notify::count()` with the values it last drew and repaints when either changed (an install or delete arriving over the network; a new notification). The battery, the clock and the theme are covered by the framework's 500 ms check; the balance by `requestShellRepaint()`.

Differences from upstream's launcher, on purpose: no `Settings` row (CANCEL opens settings), no version text beside an app (the delete confirmation shows it), the cursor survives an app run.

## Delete confirmation

Screen `app_delete`. Reached only by the launcher's RIGHT hold. The app is remembered **by id**, not by index: a push can install or remove an app while the prompt is up.

Layout: `frame("DELETE APP", "SELECT delete", "CANCEL keep")`; rows from y = 48: `APP` / name, `ID` / id, `VERSION` / `info.version` or `-`, `SIZE` / `<n> KB` (`(info.sizeBytes + 1023) / 1024`, from `app_store::byId(id, info)`; the last three rows are left out when `byId` fails); `text(10, 126, "Its files and saved data go with it.", SUB)`; `text(10, 140, "This cannot be undone.", STAMP_WARN)`.

| Button | Action |
|---|---|
| SELECT | `removed = !app_store::exists(id) \|\| app_store::removeApp(id)`; `app_store::refresh()`. Removed (already gone counts as removed): `badge_log::tagf("os", "deleted app '%s'", id)`, `pulseLed(500)` (the theme's pulse: it was done as asked; it was the red `pulseLedBad` until the gap audit), `home()`. Not removed: `badge_log::tagf("os", "could not delete app '%s'", id)`, then `shell::showError("Could not delete " + name)`, after which the error screen offers no retry ([App error](#app-error)) |
| CANCEL | forget the app, `pop()` |

Refresh: input only.

## Settings list

Screen `settings`, opened by CANCEL on the launcher. `frame("SETTINGS", "SELECT open")`; the [registered rows](#settings-page-registry) through `listDraw` (9 visible, scrolling; `n/N` beside the title). Each row's value comes from its `value` function.

| Button | Action |
|---|---|
| UP / DOWN | `listMove` |
| SELECT | a page: push its screen. An action: call it, repaint |
| CANCEL | `pop()` (the launcher) |

Refresh: 1000 ms, because the row values are live (Wi-Fi state, peers, notifications).

## Settings pages

Each subsection names every upstream function the page reads or calls. Texts in `code` are drawn exactly.

### Theme

Action row, `page_theme.cpp`. Value: `vk::ui::theme::activeName()` with a leading `receipt-` removed. SELECT: find the active theme among `vk::ui::theme::at(0 .. count()-1)` and call `vk::ui::theme::setActive(next->name)` for the one after it (wrapping). The framework repaints within 500 ms because the paper colour changed; the action also calls `repaint()`.

### Wi-Fi

Screens `wifi`, `wifi_scan`, `wifi_saved` and `wifi_join`, all in `page_wifi.cpp`; the page also opens screen `keyboard` ([text entry](text-entry.md)). The whole way onto a network is on the badge: nothing needs a laptop or a phone, except a network that asks for a user name (WPA2-Enterprise), which is still set up from the web page.

**Status:** built; the logic under the screens is host-tested (`test_wifi_net`, `test_keyboard`). **Not yet run on a badge:** no screen below was seen on the glass and no network was joined with it. `t_wifi_setup.py` is written and has not been run. What the first run must settle is listed under [To check on a badge](#to-check-on-a-badge).

```
wifi (status, switches, saved networks)
 ├─ Scan for networks ─> wifi_scan ─┬─ an open network, or a saved one ────────────────> wifi_join
 │                                  ├─ a network with a password ─> keyboard (PASSWORD) ─> wifi_join
 │                                  ├─ a network with a login ──────────────────────────> wifi_join (notice)
 │                                  └─ OTHER... ─> keyboard (NETWORK NAME) ─> keyboard (PASSWORD) ─> wifi_join
 └─ a saved network ─> wifi_saved ──── Join ────────────────────────────────────────────> wifi_join
                                  └─── Forget

wifi_join: CONNECTING ─> joined · wrong password · not found · no answer      (each says what to do next)
```

CANCEL goes back one screen everywhere (on the keyboard: [one character, then out](text-entry.md#buttons)). The stack is at most five deep (`launcher`, `settings`, `wifi`, `wifi_scan`, then `keyboard` or `wifi_join`).

The files, and who does what:

| File | Does |
|---|---|
| `src/vk/shell/pages/page_wifi.cpp` | the four screens. Holds a typed password only between the keyboard and the join, and for "type it again"; wipes it when the flow ends |
| `src/vk/core/wifi_net.{h,cpp}` | `vk::wifi`: the saved networks, the join state machine with its timeout, auto-join. Host-tested |
| `src/vk/ui/keyboard.{h,cpp}`, `keyboard_core.{h,c}` | the keyboard |
| `src/net/wifi_mgr`, `src/settings` | upstream, untouched: the radio, the scan, one saved network |

#### wifi: status

`frame("WI-FI", <left>)`; footer left `SELECT join or forget` on a saved network's row, otherwise `SELECT choose`.

Status block: `receipt::row(X0, X1, 48, "STATUS", wifi_mgr::statusText(), false, connected ? STAMP_OK : 0)`; `receipt::subline(X0, X1 - 24, 61, …)`; `receipt::rule(78)`. The subline:

| State | Text |
|---|---|
| on a network | `<ssid, 16 characters>  <ip>  <rssi> dBm`, with the signal bars at its right end |
| hotspot | `<the badge's name>  <ip>` |
| joining, or scanning for a saved network | `looking for a saved network`; with none saved `not connected yet` |
| off | `not connected` |

A list of 7 visible rows from y = 88 (`listDraw(list, rows, count, 88, 7)`):

| Row | Value | SELECT |
|---|---|---|
| `Scan for networks` | | `push(&kScan)` |
| `Wi-Fi` | `on` in station mode, else `off` | on: `vk::wifi::joinCancel(); wifi_mgr::disconnect(); push_server::stop();`. Off, with a saved network: `vk::wifi::joinBest()` (the strongest saved network in range; no result screen: the status block shows it). Off, with only upstream's enterprise profile: `wifi_mgr::connectEnterprise(…, false)`. Off, with nothing saved: `push(&kScan)` |
| `Hotspot` | `on` / `off` | on: `wifi_mgr::disconnect(); push_server::stop();`. Off: `wifi_mgr::startAccessPoint(); push_server::begin();` |
| each saved network, newest first | `joined` (`STAMP_OK`) when the badge is on it, else `saved` (`SUB`) | opens `wifi_saved` for it |
| upstream's enterprise profile, if it has one | `joined`, else `saved login` | opens `wifi_saved` for it |

Refresh: 250 ms. The row count changes without input, so the page clamps the cursor and the scroll position at the start of both `update()` and `draw()`.

#### wifi_scan: the networks in range

Entering the screen starts a scan (`wifi_mgr::startScan()`, asynchronous: the loop keeps running). A radio that is busy joining refuses to scan; then the stalled join is stopped (`wifi_mgr::disconnect()`) and the scan started again. A network the badge is on, and the hotspot, are never dropped for a scan.

While it runs: `frame("NETWORKS", "")`, `Looking for networks..` centred at y = 104 and `a few seconds` at y = 122 in `SUB`. A scan that has not ended after 12 s (`SCAN_TIMEOUT_MS`) counts as failed.

When it ends the results are read once into a list: one row per name (of several access points with one name, the strongest), no nameless network, **strongest first**, at most 24 (`MAX_LISTED`). Then `frame("NETWORKS", <left>)` and 9 visible rows from y = 48, with `n/N` beside the title when there are more:

| Row | Drawn | SELECT |
|---|---|---|
| a network | `receipt::row`: label = the name, two columns in from the left; in those columns a padlock (7 × 8 px) when the network is not open; value `joined`, `saved`, `login` or nothing, then the **signal bars**: four rising bars, 3 px wide, in the last 18 px of the row. A bar the signal does not reach is `FAINT`; the others are ink (paper on the selected row). One bar from −85 dBm, two from −75, three from −65, four from −55 (`BAR_LEVELS`) | the network the badge is on: back to `wifi`. Open, or saved: the join starts (`vk::wifi::join(ssid, "")`, `vk::wifi::joinSaved(ssid)`). With a password: the keyboard, title `PASSWORD`, hint `8 to 63 characters, for <ssid>`, secret, 8 to 63. With a login (any enterprise mode): `wifi_join` with a notice |
| `OTHER...` (always the last row) | value `type a name` | the keyboard, title `NETWORK NAME`, 1 to 32 characters, not secret. Then, unless that name is saved, the keyboard for the password with hint `none if it is open, or 8 to 63, for <ssid>`: an empty password is accepted and means an open network |

Footer left, by the row under the cursor: `SELECT join  RIGHT rescan` · `SELECT password  RIGHT rescan` · `needs a login  RIGHT rescan` · `SELECT type the name  RIGHT rescan`. With no network in range: `No network in range.` at y = 104. After a failed scan: `The scan did not finish.` (`STAMP_WARN`) and `RIGHT tries again`.

| Button | Action |
|---|---|
| UP / DOWN | `listMove` (wraps: UP from the first row is `OTHER...`) |
| SELECT | as above |
| RIGHT | scan again |
| CANCEL | back to `wifi`. If the scan had to stop a join that was under way, the badge goes back to its saved networks (`vk::wifi::joinBest()`) |

Leaving the keyboard without a text returns here with nothing changed.

#### wifi_join: connecting, then the result

`vk::wifi::join()` or `joinSaved()` has started the attempt; the screen polls `vk::wifi::joinState()` and repaints every 250 ms. When the result arrives the LEDs pulse once: the theme's colour for joined, red otherwise.

| State | Screen | SELECT | CANCEL |
|---|---|---|---|
| connecting | `frame("CONNECTING", "", "CANCEL stop")`; rows `NETWORK` / the name and `STATE` / `wifi_mgr::statusText()`; `bar(41, 96, 30, elapsed, limit)`; `<n> s of <limit> s at most` centred at y = 114 | | `vk::wifi::joinCancel()` (Wi-Fi off), back |
| joined | `frame("CONNECTED", "SELECT or CANCEL: done", "")`; rows `NETWORK`, `RESULT` / `joined` (`STAMP_OK`), `ADDRESS` / the IP address, `SIGNAL` / `<rssi> dBm` and the bars, `SAVED` / `yes (<n> of 4)`; `The badge joins it by itself from now on.` If NVS is full: `SAVED` / `no: storage is full` (`STAMP_WARN`) | back to `wifi` | the same |
| wrong password | title `NOT CONNECTED`; `RESULT` / `wrong password` (`STAMP_BAD`); `The network refused the password.` / `Capitals and small letters are different. The` / `show key lets you read what you type.` Footer `SELECT type it again` | the keyboard again, **with the password that was typed** (for a saved network whose stored password failed: empty) | back |
| (the same, when no password was given) | `RESULT` / `needs a password`; `This network is not open.` / `SELECT to type its password.` | the keyboard | back |
| not found | `RESULT` / `not found` (`STAMP_BAD`); `No network with this name answered.` / `Move closer, or check the name: capitals and` / `small letters are different.` Footer `SELECT try again` | the same join again, with the same password | back |
| no answer (the hard timeout) | `RESULT` / `no answer` (`STAMP_WARN`); `The network did not let the badge in within <limit> s.` / `It may be busy or far away. The password was` / `not refused.` Footer `SELECT try again` | the same join again | back |
| notice: needs a login | `RESULT` / `needs a login` (`STAMP_WARN`); `This network asks for a user name.` / `The badge cannot type one in. Set the network up` / `from the web page (Settings > App push).` | back | back |
| notice: cannot join | `RESULT` / `cannot join`: a name or a password of a length no network accepts (not reachable through the keyboard, whose fields have these limits) | back | back |

Every result screen has rows `NETWORK` and `RESULT` at y = 48 and 66, `receipt::rule(82)`, and its three lines at y = 94 (`INK`), 108 and 122 (`SUB`).

"Back" from a result that is not a success wipes the typed password and, if Wi-Fi was on or trying before the flow began, sends the badge back to its saved networks (`vk::wifi::joinBest()`): a failed attempt on a new network must not leave a badge that was online offline.

#### wifi_saved: one saved network

`frame("SAVED NETWORK", "SELECT choose")`; rows `NETWORK` / the name, `KEPT` / `its password`, `open: no password` or `a login, set on the web page`, `STATE` / `joined` or `not joined`; `receipt::rule(100)`; two rows from y = 110:

| Row | SELECT |
|---|---|
| `Join` | `wifi_join` takes this screen's place, joining with the stored password. Upstream's enterprise profile: `wifi_mgr::connectEnterprise(ssid, settings::enterpriseConfig(), false)` and back (on false, `badge_log::tagf("ui", "enterprise connect refused - see Console")`) |
| `Forget this network` | `vk::wifi::forget(ssid)` (the enterprise profile: `settings::forgetWifi()`). If the badge is on that network, or the radio is still trying to reach one: `wifi_mgr::disconnect(); push_server::stop(); vk::wifi::joinBest();` (what is left of the list is tried). Back |

No screen shows a stored password, and "type it again" after a stored password failed starts from an empty field: a borrowed badge does not give its owner's passwords away.

#### Saved networks

Upstream remembers **one** network (`sysconf`: what `VKWIFI`, `JOINWIFI` and the web page write, and what it joins at boot). `vk::wifi` keeps **four** (`MAX_SAVED`), newest first, in its own NVS namespace `vkwifi` (keys `s0`..`s3` for the names, `p0`..`p3` for the passwords), and keeps the two in step with no edit to upstream:

- A network that lands in upstream's slot is copied to the front of the list: at boot, and within 2 s (`SYNC_MS`) while running. So `VKWIFI <ssid>|<password>` over USB, `JOINWIFI` and the web page all end in the same list as a network typed on the badge.
- The front of the list is written to upstream's slot (`settings::setWifiCredentials`), so upstream's join at boot is the network joined last. Forgetting the front moves the slot to the next network, or empties it.
- A network joined **from the badge** is saved when, and only when, the join succeeded: a wrong password is never stored. (`VKWIFI` saves before it joins, as before.)
- A fifth network pushes the oldest out. A name is 1 to 32 bytes; a password is empty, or 8 to 63 characters (or the 64 hex digits of a raw key, which upstream's slot may hold).
- An enterprise profile stays upstream's alone: it is not copied into the list, and the status page shows it as its own row. Joining a network from the badge takes upstream's slot, as `JOINWIFI` always did, so the enterprise profile then has to be set again from the web page.
- A stored password never leaves `wifi_net.cpp`: no function returns it, nothing logs it, and it is not a config key (`VKGET` prints config values). It is stored as upstream stores its one password: as plain text in NVS.

#### Joining

```cpp
// src/vk/core/wifi_net.h (the part the screens use)
namespace vk::wifi {
constexpr size_t MAX_SAVED = 4;
size_t savedCount();
const char *savedSsid(size_t index);                      // newest first; "" past the end
bool isSaved(const char *ssid);
bool savedIsOpen(const char *ssid);
bool remember(const char *ssid, const char *password);    // to the front, and into upstream's slot
bool forget(const char *ssid);

enum class Join : uint8_t { IDLE, CONNECTING, JOINED, WRONG_PASSWORD, NOT_FOUND, TIMEOUT };
bool join(const char *ssid, const char *password);        // typed credentials; "" = an open network
bool joinSaved(const char *ssid);                         // with the stored password
void joinBest();                                          // quiet: the strongest saved network in range, else the newest
Join joinState();
const char *joinStateName();                              // "idle", "connecting", "joined", "wrong_password", "not_found", "timeout"
const char *joinSsid();
uint32_t joinElapsedMs();
uint32_t joinTimeoutMs();                                 // config key wifi_join_s
void joinCancel();                                        // stop; Wi-Fi off; IDLE
void joinDismiss();                                       // the result was read: IDLE
void hold();                                              // the user is choosing: no automatic join for 5 s
}
```

One attempt: `wifi_mgr::connect(ssid, password, false)` from a clean radio (a link that is up, or another join that is under way, is stopped first), then a service watches it on every loop pass:

| Seen | Result |
|---|---|
| station mode, connected, and the network's name is the one asked for | `JOINED`; `remember()` |
| two disconnect events that mean the password (4-way handshake timeout 15, handshake timeout 204, MIC failure 14), or one the driver does not retry (authentication failed 202; no access point with compatible security 210 or in the allowed modes 211: a password for an open network, or none for a secured one) | `WRONG_PASSWORD` |
| two events "no access point found" (201), or one "none strong enough" (212) | `NOT_FOUND` |
| `wifi_join_s` seconds (default 15) with neither | what a single event said, if there was one; otherwise `TIMEOUT` |

Every result but `JOINED` turns the radio off (`wifi_mgr::disconnect()`), or the driver would retry a join that failed for ever. The reason codes come from the Wi-Fi driver's disconnect event (`WiFi.onEvent`, registered once at boot); upstream's `statusText()` is too coarse to tell a wrong password from a slow network. `classify()` is the table above as a pure function.

`joinBest()` is for "Wi-Fi on" and for auto-join. With one saved network it joins it. With more it scans, then joins the strongest saved network in range, or the newest when none is (which the driver then keeps retrying, as upstream does with its one network). It reports nothing and does not reorder the list.

**Auto-join.** At boot upstream joins the newest network, as before. With **two or more** saved networks, when the badge is in station mode and has had no network for 15 s (`AUTOJOIN_AFTER_MS`), the service does what `joinBest()` does, and again every 60 s (`AUTOJOIN_RETRY_MS`) while that lasts: that covers a boot where the newest network is out of range, and a drop. It does so only while no app runs and no approval is open (a scan takes the radio off the ESP-NOW channel for a few seconds, and a join can move it for good: [the shared radio](../protocol/espnow.md)), not while a Wi-Fi screen is open, and never when Wi-Fi is off or the hotspot is on. With one saved network there is nothing to choose and nothing changes: the driver retries that network by itself. `VKSET wifi_autojoin 0` turns it off.

Config keys registered in `wifi_net.cpp` ([config](../platform/config.md#keys)): `wifi_join_s` (U32, default 15, 5 to 60) and `wifi_autojoin` (U32, default 1, 0 or 1).

#### Secrets

A password typed on the badge goes: keyboard buffer → the page's buffer → `vk::wifi::join` → the radio, and on success into NVS. Each buffer is overwritten with zeros when its part is over. None of it is logged: the `[wifi]` log lines name the network and the result (`connecting to '<ssid>'`, `join '<ssid>': not_found`, `joined '<ssid>'`), never the password. Nothing prints it over serial, `VKSTATE` has no field for it, and the two dev commands below give names and states only.

#### Dev hooks

Dev profile only, registered in the files they describe:

- `VKWIFIUI` (`page_wifi.cpp`): `{"scanning":false,"scan_failed":false,"networks":3,"rows":4,"cursor":3,"row":"OTHER...","join":"not_found","ssid":"zz T9~x","saved":1,"mode":"off"}`. `row` is the name under the scan list's cursor, `join` is `joinStateName()` (or `needs_login`, `refused` for the two notices), `mode` is `wifi_mgr::statusText()`.
- `VKKBD` (`keyboard.cpp`): [text entry](text-entry.md#dev-hook).

#### Tests

- Host: `test_wifi_net` (the list: order, four at most, forget, a reboot, a failed NVS write; the slot in both directions; every join result, the timeout, cancel; `joinBest`; every auto-join condition) and `test_keyboard`.
- Device: `t_wifi_setup.py`, one badge, no hands, **never joins a real network**: `wifi` → `wifi_scan` (a scan runs and ends) → `OTHER...` → the name `zz T9~x` typed through all four layers, with a held SELECT and a backspace → the password field (secret: no text in any reply; DONE refused under 8; show and hide) → `wifi_join`: `connecting`, then `not_found` → SELECT tries again → CANCEL. It checks that nothing was saved and that the password is in no `VKSTATE` reply and no log line, and saves `shots/wifi_setup_<status|scan|name|password|connecting|not_found>_<theme>.png` in both themes. The join to nothing turns the radio off, so a badge that was on a network leaves it and comes back through `joinBest()`; the test waits for that.
- `t_pages1.py` still opens `wifi` and leaves it with CANCEL.

#### To check on a badge

In this order; none of it has been seen:

1. The image builds in both profiles and the boot line counts one more service and two more config keys; `VKHELP` in the release build lists neither `VKKBD` nor `VKWIFIUI`.
2. `t_pages1.py` and `t_wifi_setup.py` pass; look at the twelve pictures in both themes (the lock and the bars on the selected row, the keyboard's cells).
3. By hand, an open network and a WPA2 network: joined, the address shown, `SAVED yes`; after a reboot the badge is back on it.
4. A wrong password: which disconnect reason the driver really gives (`[wifi] join '<ssid>': wrong_password` within a few seconds, not `timeout` at 15 s). If it is `timeout`, the reason is missing from `classify()`: read it from the driver's log at debug level and add it.
5. A name that does not exist: `not_found` within about 6 s.
6. `VKWIFI a|b` over USB appears on the status page within 2 s; Forget removes it and it does not come back after a reboot.
7. Two saved networks, the newer one switched off: within 15 s of boot the badge scans and joins the other. Then check that ESP-NOW still works afterwards on the new channel.
8. The hotspot row, and Wi-Fi off and on.

### Bluetooth

Screen `bluetooth`, `page_bluetooth.cpp`. `frame("BLUETOOTH", "SELECT toggle")`; three rows from y = 48; `receipt::rule(106)`; `text(10, 116, "Nordic UART service", SUB)`; `text(10, 130, …, SUB)` with `ble_mgr::address()`, or `6e400001-b5a3-f393-e0a9-e50e24dcca9e` when it is empty.

| Row | Value | SELECT |
|---|---|---|
| `Bluetooth LE` | `onOff(ble_mgr::enabled())` | enabled: `ble_mgr::end()`; else `ble_mgr::begin(settings::deviceName())` |
| `Connected` | `onOff(ble_mgr::connected())` | nothing |
| `Enable at boot` | `onOff(settings::bleEnabledAtBoot())` | `settings::setBleEnabledAtBoot(!settings::bleEnabledAtBoot())` |

Values are coloured with `onOffColor`. Refresh: 1000 ms (the connection state changes without input).

### ESP-NOW

Screen `espnow`, `page_espnow.cpp`: the radar. `frame("ESP-NOW", "SELECT on/off  LEFT/RIGHT channel")`. Three rows that are not selectable: y = 48 `STATE` / `onOff(espnow_mgr::enabled())`; y = 66 `CHANNEL` / `espnow_mgr::channel()`; y = 84 `PEERS` / `espnow_mgr::peerCount()`. `receipt::rule(100)`. Peers from y = 110, six visible: for `i < espnow_mgr::peerCount()`, `espnow_mgr::peerAt(i)` gives `name`, `rssi`, `lastSeenMs`; row label = name, value = `<rssi>dBm <seconds since lastSeenMs>s`. When off: `textCentered(160, 130, "Press SELECT to turn ESP-NOW on", SUB)` and no peer rows. When on with no peer: `Listening for other badges...` at y = 130 and `They must be on the same channel` at y = 144.

| Button | Action |
|---|---|
| SELECT | enabled: `espnow_mgr::end(); settings::setEspnowEnabledAtBoot(false);` else `espnow_mgr::begin(settings::espnowChannel()); settings::setEspnowEnabledAtBoot(true);` |
| LEFT / RIGHT (`buttons::pressed`) | channel ∓ 1 within 1..13: `settings::setEspnowChannel(channel)`; if enabled, `espnow_mgr::end(); espnow_mgr::begin(channel);` (a peer set cannot move across channels) |
| CANCEL | back |

Refresh: 250 ms. Only badges running BadgeOS appear: the frame magic is `BDOS` ([hook H23](../architecture/upstream-hooks.md#h23--badgeos-names)). The peer rows are drawn with `receipt::row` directly, not `listDraw`: at most six, no scrolling and no `n/N` mark, as upstream.

### App push

Screen `push`, `page_push.cpp`. `up = push_server::running() && wifi_mgr::connected()`.

Layout: `frame("APP PUSH", "SELECT choose")`; `receipt::row(X0, X1, 48, "WEB UI", up ? "ready" : "wi-fi is off", false, onOffColor(up))`; `receipt::subline(X0, X1, 61, …)`: `http://<wifi_mgr::ip().toString()>/` when up, else `Settings > Wi-Fi to connect`; `text(22, 76, …, SUB)`: on the badge's own hotspot (`wifi_mgr::mode() == AccessPoint`) `hotspot <ssid>  password <password>` as the radio has them (`esp_wifi_get_config`: a Lua app may start the hotspot with another password), otherwise when up `http://<settings::deviceName()>.local/`, else `or start the hotspot: password <DEFAULT_AP_PASSWORD>` (the password Settings → Wi-Fi's `Start hotspot` uses, `badgeos-setup`, hook H23). A phone must join the hotspot before it can open the web page, and the password was in the source only; `receipt::amount(160, 92, label, settings::pairingCode(), "")` with label `PAIRING CODE`, or `PAIRING CODE (NOT REQUIRED)` when `!settings::pushRequiresPairing()`; `receipt::rule(148)`; three rows from y = 158 (`listDraw(list, rows, 3, 158, 3)`).

| Row | Value | SELECT |
|---|---|---|
| `Require pairing code` | `onOff(settings::pushRequiresPairing())` | `settings::setPushRequiresPairing(!settings::pushRequiresPairing())` |
| `New pairing code` | | `settings::regeneratePairingCode(); pulseLed(400);` |
| `BLE push` | `advertising` when `ble_mgr::enabled()`, else `off` | enabled: `ble_mgr::end()`; else `ble_mgr::begin(settings::deviceName())` |

Refresh: 1000 ms.

### App store

Screen `store`, `page_store.cpp`. Upstream's store client (`broker`) stays compiled in, but `DEFAULT_BROKER_URL` is empty, so it is off until someone sets an address from the web page. The page shows that and keeps upstream's two actions.

Layout: `frame("APP STORE", "SELECT choose")`; y = 48 `STATE` / `off` when `!broker::enabled()` or `broker::url()` is empty, otherwise `broker::stateText()` (`STAMP_BAD` when `broker::state() == broker::State::Error`; `STAMP_OK` when the state is `Idle`, `Offered` or `Installing`); y = 66 `REGISTERED` / `yes` or `no` (`broker::registered()`); y = 84 `ADDRESS` / `broker::url()` or `none`; `text(10, 102, broker::lastError(), STAMP_BAD)` (50 characters); `text(10, 118, "The address is set from the web page,", SUB)`, `text(10, 130, "not from here: no keyboard.", SUB)`; `text(10, 142, …)`: `http://<wifi_mgr::ip().toString()>/` when `wifi_mgr::connected()`, else `Settings > Wi-Fi, then browse to the badge`; `receipt::rule(158)`; two rows from y = 168.

| Row | Value | SELECT |
|---|---|---|
| `App store` | `onOff(broker::enabled())` | `broker::setEnabled(!broker::enabled())` |
| `Forget registration` | `re-register` when `broker::registered()` | `broker::forget()` |

Refresh: 1000 ms. Removing this file removes the row only; the offer screens stay, because the client can still raise them. Colours the text above does not give: `STATE` `off` is `SUB`; `REGISTERED` is `STAMP_OK` for `yes` and `SUB` for `no`; any other state is ink.

### Identity

Screen `identity`, `page_identity.cpp`. The badge ID is the one string a stranger reads off this screen, so it is printed large.

Layout: `frame("IDENTITY", "SELECT new identity")`; `receipt::amount(160, 55, "BADGE ID", identity::ready() ? identity::badgeId() : "--------", "")` (the label starts one row pitch, 18 px, under the title's capitals, which end at y = 36; at y = 44, as first built, it sat directly under the title); `receipt::row(X0, X1, 112, "KEY LIVES IN", identity::sourceName(), false, secure ? STAMP_OK : STAMP_WARN)` where `secure = identity::source() == identity::Source::SecureElement` (the two key paths promise different things; the colour must not hide a dead SE050); `text(10, 127, identity::status(), SUB)`; `text(10, 141, "PUBLIC KEY", SUB)`; `identity::publicKeyBase58()` in lines of 32 characters at y = 153 and 165 (a half-shown key is useless for checking against a wallet); `receipt::row(X0, X1, 190, "New identity", "changes the badge ID", true)`.

Buttons: SELECT pushes the sub-screen `identity_new`; CANCEL back. Refresh: input only. On a badge with no identity the New identity confirmation still opens, with an empty `big`, as upstream allowed.

### New identity

Screen `identity_new`, a static `Screen` in `page_identity.cpp`. `frame("NEW IDENTITY", "SELECT continue")`; text lines at x = 10:

| y | Text | Colour |
|---|---|---|
| 48 | `This throws the keypair away and` | INK |
| 62 | `makes a new one. It cannot be undone.` | INK |
| 84 | `The badge ID and wallet address change, so:` | SUB |
| 100 | `- funds stay with the old address: move them first` | SUB |
| 114 | `- the registry record and contacts name the old key` | SUB |
| 128 | `- a configured app store loses this badge` | SUB |
| 142 | `- installed apps are left alone` | SUB |
| 166 | `<badgeId>  ->  ?`, or `no identity yet` | STAMP_WARN |

| Button | Action |
|---|---|
| SELECT | raises a firmware confirmation ([approval](../wallet/approval.md)) through `vk::wallet::approval::confirm`: title `New identity`, headline `ERASE BADGE KEY`, big = the badge ID, lines `Erases` = `the badge key`, `Keeps` = `apps, history, contacts`, amber, hold. Approved: `ok = identity::regenerate()` (blocks a second or two); `broker::forget()` (the stored token was issued to the old key); `badge_log::tagf("ui", "identity regenerated: %s", ok ? "ok" : "failed")`; `pulseLed(700)` or `pulseLedBad(700)`; `pop()`; `vk::ui::requestShellRepaint()`. Rejected or timed out: stay on this screen. If `confirm` returns false (another approval is open): nothing |
| CANCEL | back to `identity` |

Upstream regenerated on a single press. BadgeOS holds the wallet key in the same identity, so the destructive step goes through the same hold-SELECT confirmation as `VKRESET`.

### Display

Screen `display`, `page_display.cpp`. `frame("DISPLAY", "UP/DOWN choose  LEFT/RIGHT adjust")`; two rows through `listDraw(list, rows, 2, 48, 2)`: `Backlight` / `<settings::brightness() * 100 / 255>%` and `Sleep after` / config key `sleep_s` as `30 s`, `2 min` or `never`; `bar(41, 90, 30, settings::brightness(), 255)`; at y = 112 in `SUB` what the timers do (`Idle: dims after 30 s, off after 2 min.`, or `… never turns off.`, `Idle: turns off after …`, `Idle: stays on.`); at y = 126 `Any key wakes it; that key does nothing else.`; at y = 140, only when `awake_usb` is 1, `Stays on while on USB power.` ([screen dim and sleep](ui.md#screen-dim-and-sleep)).

UP / DOWN: `listMove`; the cursor starts on `Backlight` each time the page opens. On `Backlight`, LEFT / RIGHT (`buttons::repeated`): value ∓ 8, limited to 8..255 (a floor of 8: a dark screen looks like a crash). On a change: `settings::setBrightness(v); display::setBrightness(v);` and repaint, so the value chosen is the value seen. A press at either limit changes nothing and writes nothing. On `Sleep after`, LEFT / RIGHT (`buttons::pressed`) step through 30 s, 1 min, 2 min, 5 min, 10 min, never (shorter / longer; no wrap) and write `sleep_s` with `vk::config::set`; a value set over USB that is not a choice steps to its neighbour. The dim time (`dim_s`), the dim level (`dim_pct`) and `awake_usb` are set over USB only. CANCEL back. Refresh: input only.

### LEDs

Screen `leds`, `page_leds.cpp`. `frame("LEDS", "LEFT/RIGHT adjust  SELECT preview")`; row `RGB BRIGHTNESS` / `<settings::ledBrightness() * 100 / 255>%`; `bar(41, 70, 30, settings::ledBrightness(), 255)`.

LEFT / RIGHT: value ∓ 8 within 0..255: `settings::setLedBrightness(v); leds::setBrightness(v);`. SELECT: `pulseLed(700)` (upstream previewed with its brand boot animation, which BadgeOS does not play). CANCEL: back, leaving the LEDs alone. (Upstream stopped its preview animation here with `leds::stopAnimation(); leds::off();`, as this page first did; the preview is now a pulse that hands back to the idle animation by itself, and stopping it on the way out also killed the idle animation until an app was opened and closed.) Refresh: input only.

### Wallet and Inbox

Action rows, `page_wallet.cpp` and `page_inbox.cpp`. They launch the native apps that already exist ([apps](../apps/apps.md#wallet-native)).

| Row | Value | SELECT |
|---|---|---|
| `Wallet` | `provisioned` or `setup needed` (`vk::config::provisioned()`) | if `app_store::exists("wallet_settings")`: `::leds::stopAnimation(); runtime::requestLaunch("wallet_settings");` |
| `Inbox` | `vk::host::notify::count()` when above 0 | the same with `"inbox"` |

When the app is not compiled in, the value is `absent` (for the Inbox row too, whatever the count) and SELECT does nothing. The app exits to the launcher, like every app.

### Device info

Screen `info`, `page_info.cpp`. `frame("DEVICE INFO", "UP/DOWN scroll  SELECT re-scan I2C")`; twelve rows that are not selectable, 9 visible, UP/DOWN scroll by one row. Nothing is selected, so there is no `n/N` mark beside the title; the footer says that the list scrolls:

| Label | Value |
|---|---|
| `FIRMWARE` | `SOLANA_OS_NAME " " SOLANA_OS_VERSION` (reads `BadgeOS 0.1.0`) |
| `DEVICE` | `settings::deviceName()` |
| `CHIP` | `<ESP.getChipModel()> rev<ESP.getChipRevision()> @<ESP.getCpuFreqMHz()>MHz` |
| `HEAP` | `<ESP.getFreeHeap() / 1024> KB free` |
| `PSRAM FREE` | `<ESP.getFreePsram() / 1024> / <ESP.getPsramSize() / 1024> KB` |
| `STORAGE` | `<app_store::usedBytes() / 1024> / <app_store::totalBytes() / 1024> KB` |
| `BATTERY` | `<power::volts(), 2 decimals>V  <power::percent()>%` plus ` chg` when `power::charging()`; `STAMP_BAD` under 15 % |
| `I2C BUS` | `held low` (`STAMP_BAD`) when `badge_i2c::down()`, else `ok` (`STAMP_OK`) |
| `BUTTONS` | `TCA9534 ok` (`STAMP_OK`) when `buttons::present()`, else `not found` (`STAMP_BAD`) |
| `SE050` | `ATR ok` (`STAMP_OK`) when `se050::present()`, else `no answer` (`STAMP_WARN`) |
| `WIFI MAC` | `wifi_mgr::macAddress()` |
| `UPTIME` | `<millis() / 1000>s` |

SELECT: `badge_i2c::retry(); buttons::retry(); se050::test(); badge_i2c::scan();` then repaint: the one place a wedged bus can be retried by hand (with hook H21 the last two do nothing). CANCEL back. Refresh: 1000 ms.

The `STORAGE` value is read when the page opens and again on SELECT, not on every repaint: `app_store::usedBytes()` walks the filesystem and takes about 290 ms on the badge. Read in `draw()`, as first built, it held the loop at three passes a second for as long as the page was open.

### Console

Screen `console`, `page_console.cpp`: the log ring, newest at the bottom. `receipt::page()`, the header, `receipt::footer("UP/DOWN scroll", "CANCEL back")`; no title, to fit more lines. 17 lines at x = 6, y = 24 + 11 × i, 51 characters each: `badge_log::line(index)` for `index` from `first`, where `total = badge_log::lineCount()` and `first = total > 17 ? total - 17 - scroll : 0`. A line containing `error`, `failed` or `FATAL` is `STAMP_BAD`; the others `INK`. The background is paper (upstream's was black).

UP (`buttons::repeated`): `scroll + 1` up to `total - 17`; DOWN: `scroll - 1` down to 0; CANCEL back. Refresh: 250 ms.

### About

Screen `about`, `page_about.cpp`: what the badge runs, and a QR code that opens the project's repository on a phone. The link is the config key `repo_url` ([config](../platform/config.md#keys)), which this file registers; no address is compiled into the firmware. A 1-D barcode wide enough for a link does not fit 320 px, so this is a QR code; the launcher's barcode holds the badge ID.

Layout, two columns like Home, with no page title: `receipt::page()`, the header, `receipt::perforation(146, 24, 212)`.

- Left stub: `receipt::title("BadgeOS", 36, 73)` (the product's name, in the kit's title font), `receipt::rule(56, 10, 136)`, then four rows from x = 10 to 136 at y = 70, 88, 106, 124: `VERSION` `SOLANA_OS_VERSION` (`0.1.0`); `API` `VK_API_VERSION` (`2`); `KEY` `secure chip` in `STAMP_OK` when `vk::wallet::keyLocation()` is `se050`, otherwise the location (`software`, `none`) in `STAMP_WARN`; `ADDRESS` the first four and the last four characters of `vk::wallet::addressBase58()` joined by `..`, or `none`.
- Body, with a link: `receipt::qr(159, 26, 148, repo_url)` ([the QR code](ui.md#the-receipt-kit): a light patch in both themes), then the link as text in `SUB`, centred on x = 233, in up to three lines of 26 characters at y = 181, 192, 203. A line ends after the last `/` that fits, or at the edge when that would leave less than half a line; what does not fit in three lines ends in `..`.
- Body, with `repo_url` empty: `no link set` at y = 104 and the hint `VKSET repo_url <url>` in `SUB` at y = 122, both centred on x = 233.
- `receipt::footer("SCAN to open the link", "CANCEL back")`; the left text is empty when there is no link.

Row value in the Settings list: the host of the link (the text between `://` and the next `/`, `?` or `#`), or `not set`.

Buttons: CANCEL back. Refresh: on change only. The page reads `repo_url` twice a second and repaints when it differs from what it last drew, so a `VKSET repo_url …` shows while the page is open. For this project: `VKSET repo_url https://github.com/ayushmk7/MHacks2026` (38 characters: QR version 3, 29 modules, 4 px each).

### Badge

Screen `badge`, `page_badge.cpp`: what a person sets on the badge itself, with no laptop. `frame("BADGE", <left>)`; five rows through `listDraw(list, rows, 5, 48, 5)`; `receipt::rule(140)`; `receipt::row(X0, X1, 150, "LOCAL TIME", "<HH:MM>  (UTC <HH:MM>)")`, or `no time yet` while the clock has no source; two lines at y = 170 and 182 in `SUB` that say what the row under the cursor does; for 3 s after a change, its [note](#editing-a-config-key) at y = 196 (`STAMP_OK`, or `STAMP_WARN` for a refusal).

| Row | Value | Keys | Footer left |
|---|---|---|---|
| `Name` | config key `display_name`, or `device name` in `SUB` | SELECT: the keyboard, title `NAME`, limits from the key (0 to 32 printable characters; empty means upstream's device name, which the help line names) | `SELECT type` |
| `Time zone` | `UTC`, `UTC+5:30`, `UTC-4` (config key `utc_offset`) | LEFT / RIGHT (`buttons::repeated`): a quarter hour west / east, from −12:00 to +14:00, written at once (UTC is stored as the empty text). SELECT: the picker over every offset | `LEFT/RIGHT change  SELECT list` |
| `Clock` | `synced` (`STAMP_OK`) for SNTP, `unsynced` (`STAMP_WARN`) for FLOOR, `not set` (`STAMP_WARN`) for NONE | none: there is nothing to set. The help line says where the time comes from (the SNTP server's name, or "join it in Settings > Wi-Fi") and that it is never typed in | — |
| `Balance check` | config key `balance_poll_s` as `every 15 s`, `every 1 min`, `every 1 h`, or `off` | LEFT / RIGHT (`buttons::pressed`): one [step](#editing-a-config-key) in the key's range; SELECT: type the number | `LEFT/RIGHT change  SELECT type` |
| `Start at boot` | upstream's `settings::autostartApp()` as the app's name, `none`, or `<id> (not installed)` | SELECT: the picker: `none: the launcher`, then every installed app (`app_store::count()` / `at()`); choosing writes `settings::setAutostartApp` (what `VKAUTOSTART` does) and logs `[os] autostart app set on the badge: '<id>'` | `SELECT choose` |

**The clock is never set by hand**, here or anywhere on the badge. The wallet trusts the network's time (SNTP) and the floor of a verified record ([checks](../wallet/checks.md#clock)); a time typed with the buttons would let anyone holding the badge move record expiry. `utc_offset` changes what is printed and nothing else: `vk::clock::now()` stays UTC and `vk::clock::source()` is untouched by it. The header's time ([ui](ui.md#header)) and this page's `LOCAL TIME` add the offset; the History app's times are still UTC (they come through `wallet.time`, which this work does not change).

The page also registers the choices (`VK_KEY_CHOICES`) of `utc_offset` (the 105 quarter hours) and of `pay_app` (the installed apps), which Advanced then offers as pickers. Refresh: on change only; twice a second the page compares what it shows (the values, the clock's source, Wi-Fi joined or not, the minute, whether a note is up) with its last draw and repaints when they differ.

### Setup

Screen `setup`, `page_setup.cpp`; the logic is `setup_core.c` (pure C, host suite `test_setup`). A checklist of what this badge still needs, read from the badge each time it is drawn. `frame("SETUP", <left>, "CANCEL close")`; beside the title, right-aligned, `<n> left` in `FAINT` or `ready` in `STAMP_OK`; seven rows through `listDraw(list, rows, 7, 48, 7)`, the value coloured by state (done `STAMP_OK`, to do `STAMP_WARN`, waiting ink, optional `SUB`); `receipt::rule(172)`; up to three lines at y = 178, 189 and 200 that say what to do next for the row under the cursor (`SUB`; the wallet's missing keys in `STAMP_WARN`; for 3 s after a change the note takes the last line).

| Row | Required | State, from | SELECT |
|---|---|---|---|
| `Identity` | yes | `ready` / `no key` (`vk::wallet::publicKey()`) | nothing. The text says to restart and read the Console. It never leads to New identity, which would wipe the wallet key |
| `Wi-Fi` | yes | `joined` (station and connected) / `hotspot` / `looking` (a saved network, not joined: waiting) / `none` | the Wi-Fi page |
| `Clock` | yes | `synced` (SNTP) / `unsynced` (FLOOR: still not done, payments say CLOCK UNSYNCED) / `not set`; waiting while joined | joined: the Badge page (time zone); otherwise the Wi-Fi page |
| `Wallet` | yes | `provisioned` / `needs USB` | nothing: the lines say `From a laptop: connect USB, and in the repo run` / `python3 os/scripts/vkdev.py provision`, then `missing: issuer_key, rpc_url, tokens` (the `F_REQUIRED` keys with no value, from the config registry, sorted), or `all values set: finish with VKCOMMIT` |
| `Listener` | no | `set` / `not set` (`listener_url`) | nothing; the text says it comes with the provision command (`--listener`) or Advanced |
| `Name` | no | the name, or `device name` | the keyboard for `display_name` |
| `Theme` | no | `light` / `dark` | the next theme, in place (as the Theme row) |

The footer's left side says what SELECT does on the row (`SELECT open Wi-Fi`, `SELECT time zone`, `SELECT type a name`, `SELECT switch theme`, `needs a laptop: see below`, or nothing). No value, key or address is printed: the wallet's values are the root of trust and come from the laptop tool only ([build, flash, provision](../guides/build-flash-provision.md#provisioning)). When `VKCOMMIT` lands the rows turn done within half a second: the screen compares its rows with its last draw twice a second and repaints only when they differ.

**Set up** means the four required rows are done: a key, a joined network, a clock synced by the network, a provisioned wallet. `setup::needed()` is that test.

**First boot.** The screen opens by itself, once, over the launcher (stack `[launcher, setup]`) on the first loop pass after boot on which the launcher is on top, when all of these hold: no approval is open, no autostart app is set, no app is running, config key `setup_done` is 0, the badge is not provisioned, and a required row is not done (`vk_setup_autoopen`). "First" needs no flag of its own: a badge that is not provisioned is new, or was reset, which is when it should be shown again. The one flag is the dismissal: CANCEL leaves the screen (to the launcher when it opened by itself) and sets `setup_done` to 1 (`[os] setup dismissed`), so it never opens by itself again. A provisioned badge never gets it at boot. `VKRESET` erases `setup_done` with the rest of the config, so a reset badge shows it once more. CANCEL always leaves: the screen never traps.

**For the launcher** (`src/vk/shell/setup.h`):

```cpp
namespace vk::shell::setup {
bool needed();                         // a required row is not done
unsigned left();                       // how many required rows are left
void summary(char *out, size_t cap);   // "3 steps left", "1 step left", "done"
void open();                           // push(&kSetup)
}
```

Config key `setup_done` (U32, 0, 0 to 1) is registered in `page_setup.cpp`. Dev hook `VKSETUP` (dev profile only): `{"left":3,"autoopen":true,"setup_done":0,"cursor":0,"rows":"identity=done,wi-fi=todo,clock=todo,wallet=todo,listener=optional,name=optional,theme=optional"}`.

### Advanced

Screen `advanced`, `page_advanced.cpp`: every config key that is not secure, from the config registry, sorted by name, with its value; so a key added tomorrow is shown and editable here with no new page. `frame("ADVANCED", <left>)`; eight visible rows from y = 48 (`listDraw(list, rows, count, 48, 8)`, `n/N` beside the title); at y = 194 the selected key's `help` text in `SUB`, or the [note](#editing-a-config-key) for 3 s after a change. A value is the key's text (`(empty)` in `SUB` when it is empty); a read-only key's value is `SUB`.

| Key | Footer left | Keys |
|---|---|---|
| U32 | `LEFT/RIGHT change  SELECT type` | LEFT / RIGHT one step; SELECT the keyboard for a number in the key's range |
| with registered choices (`theme`, `utc_offset`, `pay_app`) | `LEFT/RIGHT change  SELECT list` | LEFT / RIGHT the previous / next choice; SELECT the picker |
| STR | `SELECT type` | SELECT the keyboard, limits from the key |
| required (`rpc_url`), or a stored text longer than the keyboard holds | `set from a laptop (USB)` | none |

Secure keys are not listed: they change over USB with a hold on the badge, as before. This file registers the choices of `theme` (the registered themes). Refresh: on change only (a hash of every value and of the note, compared twice a second).

Why a generic page and not one page per key: it is about 160 lines (and `edit.cpp`, which Badge and Setup need anyway), it reads ranges, lengths, rules and help from the keys themselves, and every future key becomes editable on the badge with no code. The cost is that a key's raw name and value are shown as they are; the keys a person is likely to want (name, time zone, balance poll, autostart) have the Badge page with words around them.

### Editing a config key

`src/vk/shell/edit.{h,cpp}`, shared by Badge, Setup and Advanced. Every change goes through `vk::config::set`, so the key's own type, range and [rule](../platform/config.md#key-rules) decide; no range, length or default is copied into the shell.

```cpp
namespace vk::shell::edit {
struct Choices : Registered<Choices> { const char *key; size_t (*count)(); At at; };   // VK_KEY_CHOICES(ident, key, count, at)
const Choices *choicesFor(const char *key);
enum class How : uint8_t { STEP, CHOOSE, TYPE, LAPTOP };
How how(const vk::config::ConfigKey &key);
bool step(const char *key, int direction);      // U32: one step inside the key's range
bool cycle(const char *key, int direction);     // the previous / next registered choice
void type(const char *key, const char *title);  // the keyboard; written on DONE
void choose(const char *key, const char *title);// the picker; written on SELECT
void pick(const char *title, size_t count, At at, const char *current, Picked picked);   // screen `pick`
bool openPage(const char *id);                  // push a registered settings page by id
const char *note(bool *bad = nullptr);          // what the last change did, for 3 s
}
```

- **What can be changed here:** a key that is neither `F_SECURE` nor `F_REQUIRED`, of type U32 or STR (`How::LAPTOP` otherwise). The rule is enforced in `edit.cpp` itself, whatever page asks: the issuer key, the token table, the limits and the RPC address never come from the buttons.
- **The step** (`vk_step_u32`): a range of 20 or less moves by one; a wider range moves along round numbers (1 2 3 5 10 15 20 25 30 45 50 60 90 100 120 150 180 200 250 300 400 500 600 750 900 1000 … 3600 … 10000, then 1-2-5 per decade), with the range's ends as stops; 0 to 3600 takes about 35 presses. A value set over USB that is not on the ladder steps to its neighbour. At a limit nothing is written.
- **The keyboard** gets the key's limits: STR `minLen` = the key's minimum, `maxLen` = its maximum but at most 64 (`keyboard::TEXT_MAX`), an empty text allowed when the minimum is 0, hint `up to <n> characters; empty: the default` or `<a> to <b> characters`; U32 1 to 10 characters, hint `a number from <a> to <b>`. It starts with the current value.
- **The note**: `saved` (and `[os] setting <key> changed on the badge` in the log, the name only), `refused: a number from <a> to <b>`, `refused: up to <n> printable characters`, `refused: not a valid value (see its help)` (a key rule), `not saved: storage is full`. A step or a cycle leaves a note only when it fails.
- **Screen `pick`**: `frame(<title>, "SELECT choose")`; the choices through `listDraw` (9 visible, `n/N` when more), only the rows on screen read through `at`; the current value's row has the value `current` in `STAMP_OK` and the cursor starts on it. SELECT pops the picker and then calls `picked(value)`; CANCEL pops it and changes nothing. With no choice: `Nothing to choose from.`
- **`openPage(id)`** pushes the registered page with that id from one of four static `Screen` slots (a jumped-to page can itself jump), or runs its action; no page with that id: nothing happens. Setup uses it for `wifi` and `badge`.

### Restart

Screen `restart`, `page_restart.cpp`: restarts the badge, with the page as the confirmation (the row opens it; a second press restarts). It erases nothing; it is neither Identity's New identity nor the wallet reset.

Layout: `frame("RESTART", "SELECT restart", "CANCEL back")`; `text(10, 48, "Restart the badge now?")`; in `SUB` at y = 70, 84, 98: `Kept: the key, wallet settings, apps and their` / `data, history, contacts, Wi-Fi.` / `Lost: waiting notifications, an open request.`

| Button | Action |
|---|---|
| SELECT | `badge_log::tagf("os", "restart from Settings")`; draws `RESTART` with `Restarting...` centred at y = 112 and flushes it; waits 150 ms (the log line and the frame leave); `ESP.restart()` |
| CANCEL | back |

Refresh: input only. The shell never runs while an approval is open, so a restart can never cut one short. The web page's reboot (`push_server`) is unchanged.

## App-store offer

Screen `offer`, in `dialogs.cpp`. The one screen that appears without being asked for, and the only thing between "anyone who knows a badge ID" and code on this badge's flash. It is raised by the framework ([step 1](#framework)) and is reachable from no menu. It exists as long as the store client is compiled in, whatever the App store page says.

Layout, from `broker::offer()` (`repo`, `ref`, `sender`, `count`, `scripts[i].name`, `.bytes`, `.description`, `.file`): `frame("APP OFFER", "SELECT install", "CANCEL decline")`; `receipt::row(X0, X1, 48, repo or "unknown source", "<count> script" / "<count> scripts")`; `receipt::subline(X0, X1, 61, "<ref or -> via <sender>")` (` via …` only when there is a sender); `receipt::rule(78)`; scripts from y = 88 at a pitch of 31, four visible: `receipt::row(…, script.name, size, selected)` with size `<n> KB` from 1024 bytes up, else `<n> B`, and `receipt::subline` with the description (the author's own first comment line), or the file name when there is none. `n/N` beside the title when there are more than four.

| Button | Action |
|---|---|
| (every pass) | if `!broker::hasOffer()` (it expired, or the broker withdrew it): `broker::installing()` → remember `millis()`, replace this screen with `installing`; otherwise `home()` |
| UP / DOWN | `listMove(list, offer.count, 4)` |
| SELECT | `broker::accept()`; remember `millis()`; replace this screen with `installing` |
| CANCEL | `broker::decline()`; `home()` |

Refresh: input only. The LEDs pulse in the theme's LED colour for 900 ms when it appears (the wearer is looking at whoever sent it, not at the screen).

## Installing

Screen `installing`, in `dialogs.cpp`.

While `broker::lastResult()` is empty: `frame("INSTALLING", …)`; `receipt::amount(160, 48, "", "<broker::installProgress()>%", "")`; `bar(41, 112, 30, broker::installProgress(), 100)`; rows y = 136 `OK` / `broker::installedCount()` and y = 154 `FAILED` / `broker::failedCount()`. Footer left: `verifying sha256 before each write`, with the footer right counting down to the moment CANCEL works (`CANCEL in 20s` … `CANCEL in 1s`; a screen that ignores CANCEL and says nothing looks hung); after 20 s on this screen (`INSTALL_ESCAPE_MS = 20000`): left `stuck? CANCEL to give up`, right empty.

When it is set: title `INSTALLED` when `broker::failedCount() == 0`, else `FINISHED WITH ERRORS`; `textCentered(160, 70, result)`; `textCentered(160, 90, "The launcher has them now.", SUB)`; footer `SELECT or CANCEL: launcher`.

| Button | Action |
|---|---|
| CANCEL while working | only after 20 s: `home()`. A stalled install must not trap the badge (the force-quit hold only rescues apps). The broker keeps its state; a late result is harmless |
| SELECT or CANCEL when done | `broker::clearResult()`; `home()` |

Refresh: 250 ms.

## App error

Screen `app_error`, in `dialogs.cpp`: where an app lands when it dies, or when a delete fails.

Layout: `frame("APP STOPPED", "SELECT retry", "CANCEL launcher")`, with the footer's left empty when there is nothing to retry (below); the message wrapped by hand at 50 characters and at every `\n`, lines at x = 10, y = 48 + 12 × i, at most 13 lines; the first line `STAMP_BAD`, the rest `INK`. Tabs in the message (a Lua traceback indents with them) become two spaces; the kit would draw a tab as `?`.

| Button | Action |
|---|---|
| SELECT | Only when the screen shows an app that stopped: `retry = runtime::lastApp()` (the current app is already empty). If it is not empty and `app_store::exists(retry)`: `runtime::clearError(); runtime::requestLaunch(retry);`. After a failed delete SELECT does nothing: `runtime::lastApp()` is then whatever app ran last, not the one that could not be deleted, and retrying it launched an unrelated app. `appErrorSet()` marks the screen retryable; the delete path clears the mark after `showError` |
| CANCEL | `runtime::clearError(); home();` |

Refresh: input only. `shell::showError` pulses the LEDs red for 700 ms.

## Approval, notifications, themes

- **Approval.** While `vk::modalActive()`, the main loop calls neither the shell nor an app (hook H4): the shell is paused, its timers simply lapse, and no button reaches it. When the approval closes it calls `vk::ui::requestShellRepaint()`, and the shell repaints its top screen on the next pass. The shell raises approvals itself in one place only (New identity). An offer that arrives during an approval is raised on the first pass after it.
- **Notifications.** The launcher shows the waiting count on the `inbox` cell and the Settings list on the `Inbox` row. Nothing is drawn in the header. The `notify` LED pattern is unchanged.
- **Idle.** `vk::host::idle()` is `!runtime::running()`: the shell is showing. The balance poll, the `notify` pattern and the idle LED animation ask it.
- **Screen sleep.** The shell keeps drawing and flushing as usual while the backlight is dimmed or off ([screen dim and sleep](ui.md#screen-dim-and-sleep)), so the picture is current the moment it wakes. The key that wakes the screen never reaches a screen (hook H25).
- **Themes.** Every colour comes from `vk::ui::theme::color()`; a theme change repaints within 500 ms. Upstream's compile-time palette (`src/ui/theme.h`) is set to the Receipt-light values for whatever upstream code still draws with it; no shell screen uses it.
- **Autostart.** Upstream's autostart app (`settings::autostartApp()`, set by `VKAUTOSTART`) still launches after boot; CANCEL or the force-quit hold returns to the launcher.

## What was removed

| Removed | Instead |
|---|---|
| upstream's `src/ui/shell.cpp` (launcher, ten settings screens, offer, installing, error, delete) | this shell |
| the two splash images, `splash_images.h`, the splash fade | the boot screen, at once |
| the native apps `launcher` and `settings` | the shell's `launcher` and `settings` screens |
| the home service, `vk::host::showShell()`, config key `home_app` | nothing is needed: when no app runs, the shell is the launcher |
| "System settings" (the door to upstream's screens) | every settings screen is a BadgeOS page |
| upstream's status bar, `VK_STATUS_ITEM`, `statusbar.{h,cpp}`, the items `setup`, `dev`, `inbox`, `balance` | the launcher's balance row (`SETUP NEEDED` when unprovisioned), the `inbox` cell; the dev build is marked on the approval screen |
| hooks H14, H15, H20 | [retired](../architecture/upstream-hooks.md#retired-hooks) |
| upstream's sample apps, brand colours in Lua (`gfx.SOLANA_*`), the brand names | [replaced upstream files](../architecture/upstream-hooks.md#replaced-upstream-files) |

## Add a settings page

One file, `src/vk/shell/pages/page_<id>.cpp`; nothing else is edited.

```cpp
#include "../page.h"

namespace {
using namespace vk::shell;
namespace receipt = vk::ui::receipt;               // not `using namespace vk::ui`: see below
namespace th = vk::ui::theme;

List sList;

void pageValue(char *out, size_t cap) { snprintf(out, cap, "%s", onOff(true)); }   // shown in the Settings list
void pageEnter() { sList = List(); }
void pageUpdate() {
  if (back()) return;                              // CANCEL
  const int count = 2;
  listMove(sList, count);
  if (buttons::pressed(BTN_A)) { /* act on sList.cursor */ repaint(); }
}
void pageDraw() {
  frame("MY PAGE", "SELECT choose");
  const ListRow rows[] = {{"First", "on", onOffColor(true)}, {"Second", "", 0}};
  listDraw(sList, rows, 2);
}
}  // namespace

VK_SETTINGS_PAGE(mypage, "mypage", 95, "My page", pageValue, pageEnter, pageUpdate, pageDraw, 0);
```

Two rules the shipped pages follow. Do not write `using namespace vk::ui;` in a page: `theme::` and `leds::` would then be ambiguous with upstream's global `::theme` and `::leds` as soon as an upstream header enters the file; use the two aliases above and write `::leds::` in full. Do not name the callbacks `enter`, `update`, `draw` or `value`: a later addition to `vk::shell` with that name would collide; prefix them (`pageDraw`, `wifiDraw`). And keep anything slow out of `draw()` and `value()`: both run on every repaint (the Settings list repaints once a second), so a filesystem walk or a network call there stalls the badge. Read it in `enter()` or on a key.

A row that only acts (no screen) is `VK_SETTINGS_ACTION(ident, "id", order, "Label", value, action);`. Choose an `order` between the neighbours you want, add the row to the [table](#settings-page-registry) and the id to [screen names](#screen-names). To remove a page, delete its file.

## Tests

Host: `test_setup` covers the shell's only logic that is not drawing: the setup checklist from a state struct (every row's state, where SELECT goes, the steps left, the first-boot rule, the missing-keys line), the number stepper, and the UTC offset (`src/vk/core/utc_offset.c`: parse, format, step, apply); `test_config_rule` covers a key's rule. The rest of the shell is drawing and upstream calls. Device tests ([testing](../testing/testing.md#acceptance-tests)) navigate by `VKSTATE.screen`, never by comparing screenshots, and save one screenshot per screen and theme as `os/test/device/shots/shell_<screen>_<theme>.png`:

- `t_shell.py`: boot lands on `launcher` with no app, with the backlight on and the canvas sent to the panel (`VKSTATE` `backlight` and `flushes`: a screenshot reads the canvas and would pass on a black screen); grid navigation; settings and back; an app that exits, an app that errors (`app_error`), the delete confirmation by holding RIGHT. Its two fixture apps are written to a temporary folder at run time.
- `t_pages1.py`, `t_pages2.py`: every settings page reached by name, drawn, and left with CANCEL; Theme changes config key `theme`; Wallet and Inbox launch their apps. `identity_new` is only ever left with CANCEL.
- `t_setup.py` (written, **not yet run**): VKSETUP against VKINFO (the first-boot rule); Setup's rows, a name typed on the keyboard into `display_name`, CANCEL as the dismissal (`setup_done`); Badge's time zone (RIGHT RIGHT is `+00:30`, LEFT LEFT is back), the key's rule refusing `+05:31` over `VKSET`, the two pickers left with CANCEL, a balance-poll step each way; Advanced opened and scrolled. Screenshots `shots/setup_<setup|keyboard|badge|pick|advanced>_<theme>.png`. It puts back `display_name`, `utc_offset`, `balance_poll_s`, `setup_done` and `theme`, and never changes the autostart app.
- `t_about.py`: the About page in both themes. The QR code in each screenshot is read back on the laptop (`common.qr_decode`: OpenCV's detector on the screenshot enlarged three times) and must be exactly `repo_url`; its patch is the light paper in both themes; a module is at least 3 px; with the key empty the page holds no code, and the code returns when the key is set again.
- Not scriptable on one badge: the boot screen (a person), the offer and installing screens (a broker), the Wi-Fi actions (they would drop the test network).
