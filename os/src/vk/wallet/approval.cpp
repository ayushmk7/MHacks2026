// The approval engine: the state machine behind the one firmware screen on which the user says yes
// or no (wallet/approval.md, "State machine", "Rules", "Dev builds").
//
// The first half of this file is the state machine. It touches nothing outside itself except
// through approval::hooks (time, keys, config values, the signer, the screen, the LEDs, the
// backlight), so the host test drives it with fakes. The second half, compiled only for the badge,
// fills the hooks and registers the engine's config keys, its app-stop listener and
// vk::config::confirmChange.
#include "approval.h"

#include <string.h>

#include "../vk_build.h"

#ifndef VK_HOST_TEST
#include "../../badge_log.h"
#include "../../config.h"          // BTN_A (SELECT), BTN_B (CANCEL)
#include "../../hal/buttons.h"
#include "../../hal/display.h"
#include "../../settings.h"
#include "../core/config.h"
#include "../core/service.h"
#include "../host/lifecycle.h"
#include "../ui/approval_screen.h"
#include "../ui/leds.h"
#include "../ui/repaint.h"
#include "signer_internal.h"
#define VK_APPROVAL_LOG(...) badge_log::tagf("vk", __VA_ARGS__)
#else
#define VK_APPROVAL_LOG(...) ((void)0)
#endif

namespace vk::wallet::approval {

Hooks hooks = {};

#ifdef VK_HOST_TEST
bool hostDevAllowUnverified = true;
#endif

namespace {

// The ranges of the two config keys (platform/config.md, Keys). They are used twice: in the
// VK_CONFIG_KEY lines below, and to pull whatever the config hook returns into range, so a missing
// or damaged value can never mean "no hold" or "no timeout". The defaults are in the key lines only.
constexpr uint32_t TMO_MIN_S = 10;
constexpr uint32_t TMO_MAX_S = 120;
constexpr uint32_t HOLD_MIN_MS = 1000;
constexpr uint32_t HOLD_MAX_MS = 10000;

// The largest message the key path signs (signing.md, Key: the software-key cap).
constexpr size_t SIGN_BYTES_MAX = 1248;

enum class Kind : uint8_t { None, Signing, Confirmation };
enum class Stored : uint8_t { Nothing, Signed, Failed };

// --- the approval on screen ---
Phase sPhase = Phase::IDLE;
Kind sKind = Kind::None;
ApprovalRequest sRequest;
SelectRule sRule = SelectRule::DISABLED;   // the rule in force (also written back to sRequest.select)
const SignDomain *sDomain = nullptr;
uint8_t sBytes[SIGN_BYTES_MAX];
size_t sLen = 0;
ConfirmDone sDone = nullptr;
void *sDoneArg = nullptr;
ApprovalOutcome sOutcome = {nullptr, false, VK_OK, nullptr};

uint32_t sOpenedAt = 0;
uint32_t sTimeoutMs = 0;
uint32_t sHoldMs = 0;
uint32_t sHoldStart = 0;
uint32_t sResultAt = 0;
uint32_t sBlinkAt = 0;
bool sBlinking = false;
bool sPrevSelect = false;
bool sFirstDraw = false;
uint8_t sSavedBacklight = 0;
bool sBacklightSaved = false;

// --- the result of the last signing approval, kept until poll() takes it ---
struct StoredResult {
  Stored state;
  Reason reason;
  uint8_t sig[64];
  char app_id[33];
  uint32_t at;      // when its screen closed: RESULT_KEEP_MS run from here
};
StoredResult sResult = {Stored::Nothing, VK_OK, {0}, {0}, 0};

uint32_t nowMs() { return hooks.now ? hooks.now() : 0; }
bool selectDown() { return hooks.selectDown && hooks.selectDown(); }
bool cancelDown() { return hooks.cancelDown && hooks.cancelDown(); }
bool anyKeyDown() { return hooks.anyKeyDown ? hooks.anyKeyDown() : (selectDown() || cancelDown()); }

uint32_t configValue(const char *key, uint32_t low, uint32_t high) {
  const uint32_t value = hooks.configU32 ? hooks.configU32(key) : 0;
  return value < low ? low : (value > high ? high : value);
}

bool devAllowUnverified() {
#if VK_DEV_ALLOW_UNVERIFIED
#ifdef VK_HOST_TEST
  return hostDevAllowUnverified;
#else
  return true;
#endif
#else
  return false;
#endif
}

// NUL-terminates `text`, replaces every byte outside printable ASCII with '?', and zeroes what
// follows the terminator (approval.md: "restricted to printable ASCII ... before drawing").
void cleanText(char *text, size_t size) {
  size_t i = 0;
  for (; i + 1 < size && text[i] != '\0'; ++i) {
    const unsigned char c = (unsigned char)text[i];
    if (c < 0x20 || c > 0x7E) text[i] = '?';
  }
  for (; i < size; ++i) text[i] = '\0';
}
template <size_t N>
void clean(char (&text)[N]) { cleanText(text, N); }

template <size_t N>
void copyText(char (&out)[N], const char *text) {
  strlcpy(out, text ? text : "", N);
  cleanText(out, N);
}

void clearResult() {
  sResult.state = Stored::Nothing;
  sResult.reason = VK_OK;
  memset(sResult.sig, 0, sizeof sResult.sig);
  sResult.app_id[0] = '\0';
  sResult.at = 0;
}

bool signingOnScreen() { return sPhase != Phase::IDLE && sKind == Kind::Signing; }

// Drops a result nobody polled for RESULT_KEEP_MS. Checked whenever the result is looked at, so
// it needs no service of its own.
void expireResult() {
  if (sResult.state == Stored::Nothing || signingOnScreen()) return;
  if ((uint32_t)(nowMs() - sResult.at) >= RESULT_KEEP_MS) clearResult();
}

const char *patternFor(const ApprovalRequest &request) {
  if (request.led != nullptr && request.led[0] != '\0') return request.led;
  switch (request.severity) {
    case Severity::GREEN: return "approve_green";
    case Severity::AMBER: return "approve_amber";
    case Severity::RED:   break;
  }
  return "approve_red";
}

#ifndef VK_HOST_TEST
const char *severityName(Severity severity) {
  switch (severity) {
    case Severity::GREEN: return "green";
    case Severity::AMBER: return "amber";
    case Severity::RED:   break;
  }
  return "red";
}
#endif

// Copies the caller's request into the engine, makes its strings safe to draw, fills the engine's
// own fields and settles the select rule in force.
void adopt(const ApprovalRequest &request, const char *domain, const char *app_id) {
  sRequest = request;
  clean(sRequest.title);
  clean(sRequest.headline);
  clean(sRequest.big);
  clean(sRequest.sub);
  for (ApprovalLine &line : sRequest.lines) {
    clean(line.label);
    clean(line.value);
  }
  const uint8_t maxLines = (uint8_t)(sizeof sRequest.lines / sizeof sRequest.lines[0]);
  if (sRequest.line_count > maxLines) sRequest.line_count = maxLines;
  clean(sRequest.recipient_name);
  clean(sRequest.symbol);
  copyText(sRequest.app_id, app_id);
  copyText(sRequest.domain, domain);
  sRequest.dev_override = false;

  // A value outside either enum is treated as the closed one.
  if ((uint8_t)sRequest.severity > (uint8_t)Severity::RED) sRequest.severity = Severity::RED;
  if ((uint8_t)sRequest.select > (uint8_t)SelectRule::DISABLED) sRequest.select = SelectRule::DISABLED;

  // Red is closed. The one exception is the dev build's hold-to-sign on "no record supplied".
  if (sRequest.severity == Severity::RED) {
    sRequest.select = SelectRule::DISABLED;
    if (devAllowUnverified() && sRequest.dev_overridable) {
      sRequest.select = SelectRule::HOLD;
      sRequest.dev_override = true;
    }
  }
  sRule = sRequest.select;
}

void start(Kind kind) {
  sKind = kind;
  sPhase = Phase::WAIT_RELEASE;
  sOpenedAt = nowMs();
  sTimeoutMs = configValue("approval_tmo_s", TMO_MIN_S, TMO_MAX_S) * 1000;
  sHoldMs = configValue("hold_ms", HOLD_MIN_MS, HOLD_MAX_MS);
  sHoldStart = 0;
  sResultAt = 0;
  sBlinking = false;
  sPrevSelect = false;
  sFirstDraw = true;
  sOutcome = {&sRequest, false, VK_OK, nullptr};

  // An app may have turned the backlight off (badge.gfx.brightness(0)): show the screen at the
  // user's saved level and put the app's value back on close.
  sBacklightSaved = false;
  if (hooks.backlight && hooks.setBacklight) {
    sSavedBacklight = hooks.backlight();
    sBacklightSaved = true;
    hooks.setBacklight(hooks.userBacklight ? hooks.userBacklight() : sSavedBacklight);
  }
  if (hooks.ledPlay) hooks.ledPlay(patternFor(sRequest));

  VK_APPROVAL_LOG("approval open: %s \"%s\" %s%s app=%s at %lu ms", sRequest.domain, sRequest.title,
                  severityName(sRequest.severity), sRequest.dev_override ? " dev-override" : "",
                  sRequest.app_id, (unsigned long)sOpenedAt);
}

void drawFrame() {
  if (!hooks.draw) return;
  float progress = 0.0f;
  if (sPhase == Phase::HOLDING) {
    const uint32_t held = (uint32_t)(nowMs() - sHoldStart);
    progress = (sHoldMs == 0 || held >= sHoldMs) ? 1.0f : (float)held / (float)sHoldMs;
  } else if (sPhase == Phase::SIGNING && sRule == SelectRule::HOLD) {
    progress = 1.0f;
  }
  const bool blink = sPhase == Phase::ARMED && sBlinking;
  hooks.draw(sRequest, sPhase, progress, sPhase == Phase::RESULT ? &sOutcome : nullptr, blink);
  if (sFirstDraw) {
    sFirstDraw = false;
    VK_APPROVAL_LOG("approval first draw at %lu ms", (unsigned long)nowMs());   // measurement M3
  }
}

void enterResult(bool approved, Reason reason, const uint8_t *sig) {
  sPhase = Phase::RESULT;
  sResultAt = nowMs();
  sBlinking = false;
  sOutcome = {&sRequest, approved, reason, sig};
  if (sKind == Kind::Signing) {
    sResult.state = approved ? Stored::Signed : Stored::Failed;
    sResult.reason = reason;
    if (!approved) memset(sResult.sig, 0, sizeof sResult.sig);
    strlcpy(sResult.app_id, sRequest.app_id, sizeof sResult.app_id);
    sResult.at = sResultAt;
  }
  VK_APPROVAL_LOG("approval result: %s %s", approved ? (sig ? "signed" : "approved") : "refused", reasonName(reason));

  for (ApprovalListener *listener = Registered<ApprovalListener>::first(); listener;
       listener = listener->Registered<ApprovalListener>::next()) {
    if (listener->fn) listener->fn(sOutcome);
  }
}

// CANCEL or the timeout. A red request reports why it was red, however it was closed.
void refuse(Reason how) {
  const bool red = sRequest.severity == Severity::RED && sRequest.red_reason != VK_OK;
  enterResult(false, red ? sRequest.red_reason : how, nullptr);
}

// SELECT under rule PRESS, or a completed hold.
void approve() {
  if (sKind == Kind::Confirmation) {
    enterResult(true, VK_OK, nullptr);
    return;
  }
  // The signature blocks the loop (about a second with a software key), so the "Signing..." frame
  // has to reach the panel before it starts.
  sPhase = Phase::SIGNING;
  drawFrame();
  if (hooks.flush) hooks.flush();
  const Reason reason = hooks.sign ? hooks.sign(sDomain, sBytes, sLen, sResult.sig) : VK_SIGN_FAILED;
  if (reason == VK_OK) {
    enterResult(true, VK_OK, sResult.sig);
  } else {
    enterResult(false, reason, nullptr);
  }
}

// Ends the approval. `deliver` is false when the owning app stopped: nothing is kept or reported.
void closeScreen(bool deliver) {
  const Kind kind = sKind;
  const ConfirmDone done = sDone;
  void *const arg = sDoneArg;
  const bool approved = sOutcome.approved;

  sPhase = Phase::IDLE;
  sKind = Kind::None;
  sDomain = nullptr;
  sDone = nullptr;
  sDoneArg = nullptr;
  memset(sBytes, 0, sizeof sBytes);
  sLen = 0;
  sBlinking = false;
  sOutcome = {nullptr, false, VK_OK, nullptr};

  if (sBacklightSaved && hooks.setBacklight) hooks.setBacklight(sSavedBacklight);
  sBacklightSaved = false;
  if (hooks.ledStop) hooks.ledStop();
  if (hooks.repaint) hooks.repaint();

  if (kind == Kind::Signing) {
    if (deliver) sResult.at = nowMs(); else clearResult();
  }
  VK_APPROVAL_LOG("approval closed%s", deliver ? "" : " (app stopped)");

  // Last, with the engine already idle, so `done` may raise the next confirmation.
  if (kind == Kind::Confirmation && deliver && done) done(approved, arg);
}

}  // namespace

bool open(const ApprovalRequest &request, const SignDomain *domain, const uint8_t *bytes, size_t len, const char *app_id) {
  if (active()) return false;
  if (hooks.now == nullptr) return false;            // no clock, no timeout: never open
  if (domain == nullptr || bytes == nullptr || len == 0 || len > SIGN_BYTES_MAX) return false;

  clearResult();   // begin() refuses while a result waits; if one is still here, the new approval replaces it
  adopt(request, domain->name, app_id);
  sDomain = domain;
  memcpy(sBytes, bytes, len);
  sLen = len;
  sDone = nullptr;
  sDoneArg = nullptr;
  start(Kind::Signing);
  return true;
}

bool confirm(const ApprovalRequest &request, ConfirmDone done, void *arg) {
  if (active()) return false;
  if (hooks.now == nullptr) return false;

  // A confirmation is raised by firmware: it carries no app id (the screen then omits "asked by")
  // and the fixed domain name "confirm". A signing result that is waiting to be polled is kept.
  adopt(request, "confirm", "");
  sDomain = nullptr;
  sLen = 0;
  sDone = done;
  sDoneArg = arg;
  start(Kind::Confirmation);
  return true;
}

Poll peekResult() {
  if (signingOnScreen()) return Poll::PENDING;
  expireResult();
  switch (sResult.state) {
    case Stored::Signed: return Poll::SIGNED;
    case Stored::Failed: return Poll::FAILED;
    case Stored::Nothing:   break;
  }
  return Poll::IDLE;
}

Poll takeResult(uint8_t sig[64], Reason &reason) {
  const Poll poll = peekResult();
  switch (poll) {
    case Poll::PENDING:
      reason = VK_OK;
      break;
    case Poll::SIGNED:
      if (sig) memcpy(sig, sResult.sig, 64);
      reason = VK_OK;
      clearResult();
      break;
    case Poll::FAILED:
      reason = sResult.reason;
      clearResult();
      break;
    case Poll::IDLE:
      reason = VK_IDLE;
      break;
  }
  return poll;
}

bool active() { return sPhase != Phase::IDLE; }
Phase phase() { return sPhase; }
const ApprovalRequest *current() { return sPhase == Phase::IDLE ? nullptr : &sRequest; }

void update() {
  if (sPhase == Phase::IDLE) return;

  const uint32_t now = nowMs();
  const bool select = selectDown();
  const bool cancel = cancelDown();

  switch (sPhase) {
    case Phase::WAIT_RELEASE:
    case Phase::ARMED:
    case Phase::HOLDING:
      // The timeout is checked first: a press on the pass it falls on is too late.
      if ((uint32_t)(now - sOpenedAt) >= sTimeoutMs) {
        refuse(VK_TIMEOUT);
        break;
      }
      if (sPhase == Phase::WAIT_RELEASE) {
        // Fresh press: nothing counts until SELECT and CANCEL have both been seen up.
        if (!select && !cancel) sPhase = Phase::ARMED;
        break;
      }
      if (cancel) {   // CANCEL wins over SELECT, also in the middle of a hold
        refuse(VK_CANCELLED);
        break;
      }
      if (sPhase == Phase::ARMED) {
        if (sBlinking && (uint32_t)(now - sBlinkAt) >= FOOTER_BLINK_MS) sBlinking = false;
        if (select && !sPrevSelect) {
          switch (sRule) {
            case SelectRule::PRESS:
              approve();
              break;
            case SelectRule::HOLD:
              sPhase = Phase::HOLDING;
              sHoldStart = now;
              break;
            case SelectRule::DISABLED:
              sBlinking = true;   // the footer blinks once; nothing else happens
              sBlinkAt = now;
              break;
          }
        }
      } else {   // HOLDING
        if (!select) {
          sPhase = Phase::ARMED;   // released early: the hold starts again from zero
        } else if ((uint32_t)(now - sHoldStart) >= sHoldMs) {
          approve();
        }
      }
      break;

    case Phase::RESULT:
      // Keys up before closing: the app must not get a release without a press, and a CANCEL held
      // through the approval must not count towards upstream's force-quit.
      if ((uint32_t)(now - sResultAt) >= RESULT_SHOW_MS && !anyKeyDown()) {
        closeScreen(true);
        return;
      }
      break;

    case Phase::SIGNING:   // never spans a pass: approve() leaves it before returning
    case Phase::IDLE:
      break;
  }

  sPrevSelect = select;
  if (sPhase != Phase::IDLE) drawFrame();
}

void appStopping(const char *app_id) {
  if (app_id == nullptr || app_id[0] == '\0') return;   // upstream calls stop() with no app running
  char id[sizeof sRequest.app_id];
  copyText(id, app_id);                                 // compare with the id as open() stored it

  if (signingOnScreen() && strcmp(sRequest.app_id, id) == 0) closeScreen(false);
  if (sResult.state != Stored::Nothing && strcmp(sResult.app_id, id) == 0) clearResult();
}

#ifdef VK_HOST_TEST
void hostReset() {
  sPhase = Phase::IDLE;
  sKind = Kind::None;
  sDomain = nullptr;
  sDone = nullptr;
  sDoneArg = nullptr;
  sLen = 0;
  sBlinking = false;
  sPrevSelect = false;
  sBacklightSaved = false;
  sOutcome = {nullptr, false, VK_OK, nullptr};
  clearResult();
}
#endif

// =================================================================================================
// Firmware side: the real hooks, the config keys, the listeners.
// =================================================================================================
#ifndef VK_HOST_TEST

namespace {

uint32_t fwNow() { return (uint32_t)millis(); }
bool fwSelectDown() { return buttons::down(BTN_A); }
bool fwCancelDown() { return buttons::down(BTN_B); }
bool fwAnyKeyDown() { return buttons::downMask() != 0; }

// The engine asks for a frame on every pass while the approval is up. A frame costs about 10 ms of
// drawing and a 34 ms display transfer, so the screen is redrawn only when the picture changes:
// the first frame of an approval, a new phase, the next step of the hold bar, the footer blink, the
// result, and once a second for the clock in the header. Between those a pass costs about 1 ms, so
// a key is seen at once.
constexpr int HOLD_BAR_STEPS = 40;
constexpr uint32_t APPROVAL_REDRAW_MS = 1000;

void fwDraw(const ApprovalRequest &request, Phase phase, float holdProgress, const ApprovalOutcome *outcome, bool footerBlink) {
  static Phase drawnPhase = Phase::IDLE;
  static int drawnStep = -1;
  static bool drawnBlink = false;
  static bool drawnOutcome = false;
  static uint32_t drawnAt = 0;

  const int step = (int)(holdProgress * HOLD_BAR_STEPS);
  const uint32_t now = (uint32_t)millis();
  if (!sFirstDraw && phase == drawnPhase && step == drawnStep && footerBlink == drawnBlink &&
      (outcome != nullptr) == drawnOutcome && (uint32_t)(now - drawnAt) < APPROVAL_REDRAW_MS) {
    return;
  }
  drawnPhase = phase;
  drawnStep = step;
  drawnBlink = footerBlink;
  drawnOutcome = outcome != nullptr;
  drawnAt = now;
  vk::ui::drawApproval(request, phase, holdProgress, outcome, footerBlink);
  display::touch();
}
void fwFlush() { display::flush(); }

// --- vk::config::confirmChange (platform/config.md, Provisioning) ---

void (*sChangeDone)(bool approved) = nullptr;   // one slot is enough: one approval at a time

void onChangeDone(bool approved, void *) {
  void (*const done)(bool) = sChangeDone;
  sChangeDone = nullptr;
  if (done) done(approved);
}

bool isKey32(const char *name) {
  using vk::config::ConfigKey;
  for (ConfigKey *key = Registered<ConfigKey>::first(); key; key = key->Registered<ConfigKey>::next()) {
    if (strcmp(key->name, name) == 0) return key->type == vk::config::Type::KEY32;
  }
  return false;
}

// A value as an approval row shows it: at most 35 characters; a key as its first 4 + ".." + last 4.
void shortValue(char (&out)[36], const char *text, bool isKey) {
  if (text == nullptr) text = "";
  const size_t length = strlen(text);
  if (length == 0) {
    strlcpy(out, "(none)", sizeof out);
  } else if (isKey && length > 10) {
    snprintf(out, sizeof out, "%.4s..%.4s", text, text + length - 4);
  } else if (length > sizeof out - 1) {
    snprintf(out, sizeof out, "%.33s..", text);
  } else {
    strlcpy(out, text, sizeof out);
  }
}

// Raises the "Change setting" confirmation for a secure key on a provisioned badge, and the
// wallet-reset confirmation when config.cpp passes the key name "(reset)". `done` is called once
// with the answer when the screen closes. False if a confirmation could not be raised (an approval
// is already open): `done` is then never called.
bool raiseChange(const char *key, const char *oldText, const char *newText, void (*done)(bool approved)) {
  if (key == nullptr || active()) return false;

  ApprovalRequest request{};
  strlcpy(request.title, "Change setting", sizeof request.title);
  request.severity = Severity::AMBER;
  request.select = SelectRule::HOLD;
  if (strcmp(key, "(reset)") == 0) {
    strlcpy(request.headline, "ERASE WALLET CONFIG", sizeof request.headline);
    strlcpy(request.big, "RESET", sizeof request.big);
    strlcpy(request.lines[0].label, "Erases", sizeof request.lines[0].label);
    strlcpy(request.lines[0].value, "all wallet settings", sizeof request.lines[0].value);
    strlcpy(request.lines[1].label, "Keeps", sizeof request.lines[1].label);
    strlcpy(request.lines[1].value, "key, history, contacts", sizeof request.lines[1].value);
  } else {
    const bool isKey = isKey32(key);
    strlcpy(request.headline, "SECURITY SETTING", sizeof request.headline);
    strlcpy(request.big, key, sizeof request.big);
    strlcpy(request.lines[0].label, "Old", sizeof request.lines[0].label);
    shortValue(request.lines[0].value, oldText, isKey);
    strlcpy(request.lines[1].label, "New", sizeof request.lines[1].label);
    shortValue(request.lines[1].value, newText, isKey);
  }
  request.line_count = 2;

  sChangeDone = done;
  if (!confirm(request, onChangeDone, nullptr)) {
    sChangeDone = nullptr;
    return false;
  }
  return true;
}

void onAppStop(const char *appId) {
  if (appId != nullptr && appId[0] != '\0') appStopping(appId);
}

void serviceBegin() {
  hooks.now = fwNow;
  hooks.selectDown = fwSelectDown;
  hooks.cancelDown = fwCancelDown;
  hooks.anyKeyDown = fwAnyKeyDown;
  hooks.configU32 = vk::config::u32;
  hooks.sign = vk::wallet::signForApproval;
  hooks.draw = fwDraw;
  hooks.flush = fwFlush;
  hooks.ledPlay = vk::ui::leds::play;
  hooks.ledStop = vk::ui::leds::stop;
  hooks.backlight = display::brightness;
  hooks.setBacklight = display::setBrightness;
  hooks.userBacklight = settings::brightness;
  hooks.repaint = vk::ui::requestShellRepaint;

  vk::config::confirmChange = raiseChange;
}

}  // namespace

VK_SERVICE(approval, serviceBegin, nullptr);

VK_CONFIG_KEY(approval_tmo_s, "approval_tmo_s", vk::config::Type::U32, "45", vk::config::F_SECURE, TMO_MIN_S, TMO_MAX_S,
              "approval timeout in seconds; keep below the ~60 s blockhash lifetime");
VK_CONFIG_KEY(hold_ms, "hold_ms", vk::config::Type::U32, "3000", vk::config::F_SECURE, HOLD_MIN_MS, HOLD_MAX_MS,
              "hold-SELECT duration in milliseconds");

VK_ON_APP_STOP(approval, onAppStop);

#endif  // VK_HOST_TEST

}  // namespace vk::wallet::approval
