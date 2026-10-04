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

When SELECT is pressed on a page row, the list copies the page's `id`, `enter`, `update`, `draw` and `refresh_ms` into one static `Screen` and pushes it (only one page is open at a time). On an action row it calls `action()` and repaints.

## Screen names

`shell::screenName()` returns one of these; device tests read it as the `screen` field of `VKSTATE` ([testing](../testing/testing.md#dev-hooks)).

`launcher` · `app_delete` · `settings` · `wifi` · `bluetooth` · `espnow` · `push` · `store` · `identity` · `identity_new` · `display` · `leds` · `info` · `console` · `about` · `app_error` · `offer` · `installing`

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
| barcode | `receipt::barcode(10, 184, 300, 22, vk::wallet::publicKey(), 32)`; not drawn when the badge has no identity |
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
| SELECT | `removed = !app_store::exists(id) \|\| app_store::removeApp(id)`; `app_store::refresh()`. Removed (already gone counts as removed): `badge_log::tagf("os", "deleted app '%s'", id)`, `pulseLedBad(500)`, `home()`. Not removed: `badge_log::tagf("os", "could not delete app '%s'", id)`, then `shell::showError("Could not delete " + name)` |
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

Screen `wifi`, `page_wifi.cpp`. The badge has no keyboard: an open network is joined from here; a secured one is set up from a phone through the hotspot and the web page, then re-joined from here.

Layout: `frame("WI-FI", <left>)`. Status: `receipt::row(X0, X1, 48, "STATUS", wifi_mgr::statusText(), false, connected ? STAMP_OK : 0)`; `receipt::subline(X0, X1, 61, …)` with `<wifi_mgr::ssid()>  <wifi_mgr::ip().toString()>  <wifi_mgr::rssi()> dBm` when `wifi_mgr::connected()`, else `not connected`. `receipt::rule(78)`. A list of 7 visible rows from y = 88 (`listDraw(list, rows, count, 88, 7)`): five actions, then the scan results (none while `wifi_mgr::scanning()`; otherwise `wifi_mgr::scanResultCount()`).

| Row | Value | SELECT |
|---|---|---|
| `Scan for networks` | `scanning..` while `wifi_mgr::scanning()` | `wifi_mgr::startScan()` |
| `Connect saved network` | `settings::wifiSsid()`, with ` (EAP)` appended when `settings::wifiIsEnterprise()`; `-` when none | no saved network: `badge_log::tagf("ui", "no saved network - set one up from the web UI")`. Enterprise: `wifi_mgr::connectEnterprise(saved, settings::enterpriseConfig(), false)`; on false `badge_log::tagf("ui", "enterprise connect refused - see Console")`. Otherwise `wifi_mgr::connect(saved, settings::wifiPassword(), false)` |
| `Start hotspot` | `on` when `wifi_mgr::mode() == wifi_mgr::Mode::AccessPoint` | `wifi_mgr::startAccessPoint(); push_server::begin();` |
| `Disconnect` | | `wifi_mgr::disconnect(); push_server::stop();` |
| `Forget saved network` | the saved SSID or `-` | `settings::forgetWifi()` |
| a scan result | label `wifi_mgr::scanSsid(i)`, prefixed `* ` when `wifi_mgr::scanEncrypted(i)`; value `<wifi_mgr::scanRssi(i)> dBm` | open network: `wifi_mgr::connect(wifi_mgr::scanSsid(i), "", true)`. Secured: `badge_log::tagf("ui", "'%s' is secured - set it up from the web UI", ssid)` |

Footer left: `* needs a password: use the hotspot` when the cursor is on a secured result, otherwise `SELECT choose`. Buttons: UP/DOWN `listMove(list, count, 7)`; SELECT as above, then `repaint()`; CANCEL back. Refresh: 250 ms (the scan and the join finish on their own).

As built: only the STATUS value and the hotspot's `on` are coloured (`STAMP_OK`); every other value is ink (upstream's purple for an enterprise network is gone). The list shows at most 59 scan results (64 rows with the five actions). The number of rows changes without input when a scan ends, so the page clamps the cursor and the scroll position at the start of both `update()` and `draw()`.

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

Layout: `frame("APP PUSH", "SELECT choose")`; `receipt::row(X0, X1, 48, "WEB UI", up ? "ready" : "wi-fi is off", false, onOffColor(up))`; `receipt::subline(X0, X1, 61, …)`: `http://<wifi_mgr::ip().toString()>/` when up, else `Settings > Wi-Fi to connect`; `text(22, 76, …, SUB)`: `http://<settings::deviceName()>.local/` when up, else `or start the hotspot`; `receipt::amount(160, 92, label, settings::pairingCode(), "")` with label `PAIRING CODE`, or `PAIRING CODE (NOT REQUIRED)` when `!settings::pushRequiresPairing()`; `receipt::rule(148)`; three rows from y = 158 (`listDraw(list, rows, 3, 158, 3)`).

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

Screen `display`, `page_display.cpp`. `frame("DISPLAY", "LEFT/RIGHT adjust")`; `receipt::row(X0, X1, 48, "BACKLIGHT", "<settings::brightness() * 100 / 255>%")`; `bar(41, 70, 30, settings::brightness(), 255)`.

LEFT / RIGHT (`buttons::repeated`): value ∓ 8, limited to 8..255 (a floor of 8: a dark screen looks like a crash). On a change: `settings::setBrightness(v); display::setBrightness(v);` and repaint, so the value chosen is the value seen. A press at either limit changes nothing and writes nothing. CANCEL back. Refresh: input only.

### LEDs

Screen `leds`, `page_leds.cpp`. `frame("LEDS", "LEFT/RIGHT adjust  SELECT preview")`; row `RGB BRIGHTNESS` / `<settings::ledBrightness() * 100 / 255>%`; `bar(41, 70, 30, settings::ledBrightness(), 255)`.

LEFT / RIGHT: value ∓ 8 within 0..255: `settings::setLedBrightness(v); leds::setBrightness(v);`. SELECT: `pulseLed(700)` (upstream previewed with its brand boot animation, which BadgeOS does not play). CANCEL: `leds::stopAnimation(); leds::off();` then back. Refresh: input only.

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

Screen `about`, `page_about.cpp`: what the badge runs, and a QR code that opens the project's repository on a phone. The link is the config key `repo_url` ([config](../platform/config.md#keys)), which this file registers; no address is compiled into the firmware. A striped barcode cannot hold a link, so this is a QR code; the launcher's barcode stays as decoration.

Layout, two columns like Home, with no page title: `receipt::page()`, the header, `receipt::perforation(146, 24, 212)`.

- Left stub: `receipt::title("BadgeOS", 36, 73)` (the product's name, in the kit's title font), `receipt::rule(56, 10, 136)`, then four rows from x = 10 to 136 at y = 70, 88, 106, 124: `VERSION` `SOLANA_OS_VERSION` (`0.1.0`); `API` `VK_API_VERSION` (`2`); `KEY` `secure chip` in `STAMP_OK` when `vk::wallet::keyLocation()` is `se050`, otherwise the location (`software`, `none`) in `STAMP_WARN`; `ADDRESS` the first four and the last four characters of `vk::wallet::addressBase58()` joined by `..`, or `none`.
- Body, with a link: `receipt::qr(159, 26, 148, repo_url)` ([the QR code](ui.md#the-receipt-kit): a light patch in both themes), then the link as text in `SUB`, centred on x = 233, in up to three lines of 26 characters at y = 181, 192, 203. A line ends after the last `/` that fits, or at the edge when that would leave less than half a line; what does not fit in three lines ends in `..`.
- Body, with `repo_url` empty: `no link set` at y = 104 and the hint `VKSET repo_url <url>` in `SUB` at y = 122, both centred on x = 233.
- `receipt::footer("SCAN to open the link", "CANCEL back")`; the left text is empty when there is no link.

Row value in the Settings list: the host of the link (the text between `://` and the next `/`, `?` or `#`), or `not set`.

Buttons: CANCEL back. Refresh: on change only. The page reads `repo_url` twice a second and repaints when it differs from what it last drew, so a `VKSET repo_url …` shows while the page is open. For this project: `VKSET repo_url https://github.com/ayushmk7/MHacks2026` (38 characters: QR version 3, 29 modules, 4 px each).

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

While `broker::lastResult()` is empty: `frame("INSTALLING", …)`; `receipt::amount(160, 48, "", "<broker::installProgress()>%", "")`; `bar(41, 112, 30, broker::installProgress(), 100)`; rows y = 136 `OK` / `broker::installedCount()` and y = 154 `FAILED` / `broker::failedCount()`. Footer left: `verifying sha256 before each write`; after 20 s on this screen (`INSTALL_ESCAPE_MS = 20000`): `stuck? CANCEL to give up`; footer right empty.

When it is set: title `INSTALLED` when `broker::failedCount() == 0`, else `FINISHED WITH ERRORS`; `textCentered(160, 70, result)`; `textCentered(160, 90, "The launcher has them now.", SUB)`; footer `SELECT or CANCEL: launcher`.

| Button | Action |
|---|---|
| CANCEL while working | only after 20 s: `home()`. A stalled install must not trap the badge (the force-quit hold only rescues apps). The broker keeps its state; a late result is harmless |
| SELECT or CANCEL when done | `broker::clearResult()`; `home()` |

Refresh: 250 ms.

## App error

Screen `app_error`, in `dialogs.cpp`: where an app lands when it dies, or when a delete fails.

Layout: `frame("APP STOPPED", "SELECT retry", "CANCEL launcher")`; the message wrapped by hand at 50 characters and at every `\n`, lines at x = 10, y = 48 + 12 × i, at most 13 lines; the first line `STAMP_BAD`, the rest `INK`. Tabs in the message (a Lua traceback indents with them) become two spaces; the kit would draw a tab as `?`.

| Button | Action |
|---|---|
| SELECT | `retry = runtime::lastApp()` (the current app is already empty). If it is not empty and `app_store::exists(retry)`: `runtime::clearError(); runtime::requestLaunch(retry);` |
| CANCEL | `runtime::clearError(); home();` |

Refresh: input only. `shell::showError` pulses the LEDs red for 700 ms.

## Approval, notifications, themes

- **Approval.** While `vk::modalActive()`, the main loop calls neither the shell nor an app (hook H4): the shell is paused, its timers simply lapse, and no button reaches it. When the approval closes it calls `vk::ui::requestShellRepaint()`, and the shell repaints its top screen on the next pass. The shell raises approvals itself in one place only (New identity). An offer that arrives during an approval is raised on the first pass after it.
- **Notifications.** The launcher shows the waiting count on the `inbox` cell and the Settings list on the `Inbox` row. Nothing is drawn in the header. The `notify` LED pattern is unchanged.
- **Idle.** `vk::host::idle()` is `!runtime::running()`: the shell is showing. The balance poll, the `notify` pattern and the idle LED animation ask it.
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

No host suite: the shell is drawing and upstream calls. Device tests ([testing](../testing/testing.md#acceptance-tests)) navigate by `VKSTATE.screen`, never by comparing screenshots, and save one screenshot per screen and theme as `os/test/device/shots/shell_<screen>_<theme>.png`:

- `t_shell.py`: boot lands on `launcher` with no app, with the backlight on and the canvas sent to the panel (`VKSTATE` `backlight` and `flushes`: a screenshot reads the canvas and would pass on a black screen); grid navigation; settings and back; an app that exits, an app that errors (`app_error`), the delete confirmation by holding RIGHT. Its two fixture apps are written to a temporary folder at run time.
- `t_pages1.py`, `t_pages2.py`: every settings page reached by name, drawn, and left with CANCEL; Theme changes config key `theme`; Wallet and Inbox launch their apps. `identity_new` is only ever left with CANCEL.
- `t_about.py`: the About page in both themes. The QR code in each screenshot is read back on the laptop (`common.qr_decode`: OpenCV's detector on the screenshot enlarged three times) and must be exactly `repo_url`; its patch is the light paper in both themes; a module is at least 3 px; with the key empty the page holds no code, and the code returns when the key is set again.
- Not scriptable on one badge: the boot screen (a person), the offer and installing screens (a broker), the Wi-Fi actions (they would drop the test network).
