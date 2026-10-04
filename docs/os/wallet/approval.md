# Approval engine

The one firmware screen on which the user says yes or no. It is generic: a decoder describes *what* is being approved as data, and the engine handles the screen, the buttons, the timeout, the signature and the result. Files: `src/vk/wallet/approval.{h,cpp}` (state machine), `src/vk/ui/approval_screen.{h,cpp}` (drawing).

Used by every button signing domain and by every non-signing confirmation (first-run app consent, change of a secure setting, wallet reset).

## The request

```cpp
// src/vk/wallet/approval.h
namespace vk::wallet {

enum class Severity : uint8_t { GREEN, AMBER, RED };
enum class SelectRule : uint8_t { PRESS, HOLD, DISABLED };

struct ApprovalLine { char label[10]; char value[36]; };

struct ApprovalRequest {
  // --- filled by the decoder (or by the caller of confirm()) ---
  char title[20];            // title bar: "Pay", "Allow app", "Change setting"
  char headline[28];         // coloured band: "VERIFIED - PRESENT", "MISMATCH", ...
  char big[24];              // large centre text: "10.00 HACK"; may be empty
  char sub[36];              // small text under it: "to MHacks Merch"; may be empty
  ApprovalLine lines[4];     // detail rows
  uint8_t line_count;
  Severity severity;
  SelectRule select;         // ignored when severity is RED (forced DISABLED, see Dev builds)
  Reason red_reason;         // the reason poll() reports if this request is RED
  bool dev_overridable;      // true only for "no record supplied" (red_reason VK_UNVERIFIED from check 4)
  const char *led;           // LED pattern name; nullptr = by severity
  // --- for the history; zero when not a payment ---
  uint8_t recipient[32];
  char recipient_name[33];
  uint64_t amount;
  uint8_t decimals;
  char symbol[9];
  // --- filled by the engine ---
  char app_id[33];           // upstream app ids are up to 32 characters
  char domain[12];
  bool dev_override;
};

struct ApprovalOutcome {
  const ApprovalRequest *request;
  bool approved;
  Reason reason;             // VK_OK when approved and signed
  const uint8_t *sig;        // 64 bytes when a signature was made, else nullptr
};

struct ApprovalListener : Registered<ApprovalListener> {
  void (*fn)(const ApprovalOutcome &);
  explicit ApprovalListener(void (*f)(const ApprovalOutcome &)) : fn(f) {}
};
#define VK_ON_APPROVAL(ident, fn) static vk::wallet::ApprovalListener vk_on_approval_##ident(fn)

namespace approval {
enum class Phase : uint8_t { IDLE, WAIT_RELEASE, ARMED, HOLDING, SIGNING, RESULT };

// Signing approval. Called only by vk::wallet::begin(). Copies request, bytes and app_id.
bool open(const ApprovalRequest &request, const SignDomain *domain, const uint8_t *bytes, size_t len, const char *app_id);

// The result of the last signing approval. takeResult() is what vk::wallet::poll() returns: SIGNED and
// FAILED are handed out once. peekResult() does not consume (used by the VKSTATE dev command).
Poll takeResult(uint8_t sig[64], Reason &reason);
Poll peekResult();

// Non-signing confirmation. `done` is called once, from the main loop, when the screen closes.
using ConfirmDone = void (*)(bool approved, void *arg);
bool confirm(const ApprovalRequest &request, ConfirmDone done, void *arg);

bool active();                         // true from open/confirm until the result screen closes
void update();                         // one pass; called by vk::modalUpdate()
Phase phase();
const ApprovalRequest *current();      // nullptr when IDLE
void appStopping(const char *app_id);  // drop an open approval or an un-polled result owned by that app
}  // namespace approval
}  // namespace vk::wallet
```

`SelectRule::DISABLED` collides with a macro: the Arduino-ESP32 core defines `DISABLED` as `0x00` (an interrupt mode, in `esp32-hal-gpio.h`). `approval.h` includes `<Arduino.h>` and then removes the macro with `#undef DISABLED`, so the enum name works in every file that includes `approval.h`, in any include order (the core's header is guarded and cannot define it again). Nothing in upstream, the core's libraries or LovyanGFX uses the macro. A file that names `SelectRule::DISABLED` must include `approval.h` itself.

`open` and `confirm` return false if an approval is already active. All strings are truncated to fit, always NUL-terminated, and restricted to printable ASCII (`0x20`–`0x7E`); any other byte is replaced by `?` before drawing.

## State machine

```
IDLE --open/confirm--> WAIT_RELEASE --SELECT and CANCEL both up--> ARMED
ARMED --CANCEL pressed--> RESULT(cancelled)
ARMED --SELECT pressed, rule PRESS--> SIGNING
ARMED --SELECT pressed, rule HOLD--> HOLDING
ARMED --SELECT pressed, rule DISABLED--> ARMED (footer blinks once)
HOLDING --SELECT released early--> ARMED
HOLDING --held for hold_ms--> SIGNING
WAIT_RELEASE/ARMED/HOLDING --approval_tmo_s since open--> RESULT(timeout)
SIGNING --signature made--> RESULT(signed)      --signature failed--> RESULT(sign_failed)
RESULT --RESULT_SHOW_MS (800)--> IDLE
```

Rules:

- **Fresh press.** In `WAIT_RELEASE` both keys are ignored until both have been seen up. A key held when the screen appears can never approve.
- **Red is closed.** For a RED request the select rule is DISABLED. Whatever closes it (CANCEL or timeout), the reported reason is `red_reason`, so the app learns *why* it was blocked.
- **`confirm`** skips `SIGNING`: SELECT (or the hold) goes straight to `RESULT` and calls `done(true, arg)`; CANCEL or timeout calls `done(false, arg)`.
- **Signing** draws "Signing..." and flushes the display first, then calls the signer. The loop is blocked for the duration of one signature.
- **Listeners.** On entering `RESULT`, every `VK_ON_APPROVAL` listener is called with the outcome. The history feature and the LED feedback are listeners.
- **Result ownership.** The engine stores the result of a signing approval until `poll()` takes it. It is dropped when the owning app stops (the engine registers a `VK_ON_APP_STOP` listener that calls `appStopping`) or after 60 s. Until then `begin()` returns `busy`.
- **Keys up before closing.** `RESULT` does not end until its 800 ms have passed **and** every key is up. Otherwise the app would receive a release without a press, and a CANCEL held through the approval would count towards upstream's 1.5 s force-quit.
- **Backlight.** An app can set the backlight to zero with `badge.gfx.brightness(0)`. On open the engine sets `display::setBrightness(settings::brightness())` (the user's saved level) and restores the previous value on close.
- **Repaint.** On close the engine calls `vk::ui::requestShellRepaint()` so that the launcher, if it is what lies underneath, redraws (hook H20).
- **Config.** `approval_tmo_s` (default 45) and `hold_ms` (default 3000) are config keys ([config](../platform/config.md#keys)).

`vk::modalActive()` is `approval::active()`, and so is `vk::host::luaPaused()`. While it is true the main loop runs `vk::modalUpdate()` and neither the app nor the shell, and applies no launch or stop request (hook H4); every Lua callback is a no-op (hook H19). Services still run. The router still handles firmware frame types but does not deliver frames to the app.

The engine also serves the config store: at boot it sets `vk::config::confirmChange` ([config](../platform/config.md#provisioning)) to a function that raises the `Change setting` confirmation, so `core/` never includes a wallet header.

## Screen

320×240, drawn into `display::canvas()` every pass while active, with the receipt kit ([ui](../ui/ui.md#the-receipt-kit)). The layout is one fixed function in `src/vk/ui/approval_screen.cpp`; paper and ink follow the active theme (light or dark), nothing else is configurable. It is declared in `src/vk/ui/approval_screen.h`, in namespace `vk::ui` (the types are the `vk::wallet` ones above):

```cpp
// src/vk/ui/approval_screen.h
void drawApproval(const ApprovalRequest &, approval::Phase, float holdProgress, const ApprovalOutcome *outcome, bool footerBlink);
```

`outcome` is non-null only in `RESULT`: it is what the result band and stamp are drawn from. `footerBlink` is true while the footer is blinking after SELECT was pressed under rule DISABLED. The engine passes both; the screen keeps no state of its own.

| Region | Position (px) | Content |
|---|---|---|
| Header | y 0–19 | left at x=10: `<TITLE> · asked by <app_id>` (title upper-cased; "asked by" omitted for confirmations raised by firmware). Right-aligned at x=310: `KEY SE` or `KEY SW`; `DEV BUILD · ` before it when `VK_PROFILE_DEV` is 1 |
| Verdict band | y 20–46, full width | filled with the severity colour; `headline` centred in black, `FreeMonoBold9pt7b` (falls back to `Font0` if wider than 300 px) |
| Perforation | x = 146, y 52–198 | dashed vertical line |
| Left stub | x 0–145 | `receipt::amount(73, 58, "AMOUNT", big, <unit>)`: the label, then `big` in the serif amount font, then the unit. For a payment `big` is the number and the unit is the token symbol; the decoder writes `big` as `"10.00 HACK"` and the renderer splits it at the last space. Below, at y=134, `sub` upper-cased and centred (`TO MHACKS MERCH`), truncated with `..` to 23 characters. When `big` has no space (a confirmation), it is drawn as wrapped text in `FreeSerifBold9pt7b` and no unit |
| Body | x 147–319 | up to four `receipt::row(156, 310, y, LABEL, value)` at y = 58, 76, 94, 112 (labels upper-cased), then a dashed rule 6 px under the last row |
| Stamp | centred at (250, 172) | `VERIFIED` (`STAMP_OK`) for green, `CHECK` (`STAMP_WARN`) for amber, `BLOCKED` (`STAMP_BAD`) for red; confirmations use `CONFIRM` (`STAMP_WARN`) |
| Hold bar | y 204–209, x 40–280 | only in `HOLDING`: filled from the left in proportion to the hold |
| Footer | rule at y=216, text at y=224 | PRESS: `SELECT approve` / `CANCEL reject` · HOLD: `Hold SELECT` / `CANCEL reject` · DISABLED: `Blocked` / `CANCEL close` · dev override: `DEV: hold SELECT to sign anyway` / `CANCEL close` · signing: `Signing...` |

Result (`RESULT` phase): the band's text and the stamp's text both become `SIGNED` or `APPROVED` (band green, stamp `STAMP_OK`), `CANCELLED` or `TIMED OUT` (band `FAINT`, stamp `STAMP_WARN`), `BLOCKED` or `SIGN FAILED` (band red, stamp `STAMP_BAD`); the rest of the screen is unchanged.

**The severity colours are constants in `approval_screen.cpp` and are not theme tokens**: green `#1FBF75`, amber `#FFB020`, red `#FF4545`, always with black text. A theme that could recolour them could make a blocked payment look approved. The layout is likewise fixed: a theme changes paper and ink only.

LEDs: unless `led` names a pattern, GREEN plays `approve_green`, AMBER `approve_amber`, RED `approve_red`; the result plays `signed` or `refused` ([ui](../ui/ui.md#led-patterns)).

## Dev builds

With `VK_DEV_ALLOW_UNVERIFIED` set (dev profile only):

- a RED request with `dev_overridable == true` gets select rule HOLD, `dev_override = true`, and the footer `DEV: hold SELECT to sign anyway`;
- the headline band stays red and the `DEV BUILD` banner is always visible;
- the history records the outcome with the dev flag.

`dev_overridable` is true only for "no record supplied" (check 4, `unverified`). Undecodable bytes, a mismatch against a supplied record or request, a revoked or expired record, and a bad proof are never overridable in any build.

## What a decoder writes

A decoder is a function with the `SignDomain::decode` signature. It never draws. Minimal example, the consent screen raised by the app host:

```cpp
vk::wallet::ApprovalRequest r{};
strlcpy(r.title, "Allow app", sizeof r.title);
strlcpy(r.headline, "NEW PERMISSIONS", sizeof r.headline);
strlcpy(r.big, info.name.c_str(), sizeof r.big);
strlcpy(r.lines[0].label, "May", sizeof r.lines[0].label);
strlcpy(r.lines[0].value, "request payments", sizeof r.lines[0].value);
r.line_count = 1;
r.severity = vk::wallet::Severity::AMBER;
r.select = vk::wallet::SelectRule::HOLD;
vk::wallet::approval::confirm(r, onConsent, nullptr);
```

Adding a new thing to approve is writing a function like this. The engine, the screen, the timeouts and the fresh-press rule come for free and cannot be got wrong by the new code.

## Tests

Host: `test_approval` drives the state machine with a fake clock and fake button states (the engine reads time and buttons through two function pointers so it compiles on the host): fresh-press rule, hold released early, timeout, red closes with `red_reason`, dev override only when `dev_overridable`.

Device: T-APR1 to T-APR6 in [../testing/testing.md](../testing/testing.md#acceptance-tests).
