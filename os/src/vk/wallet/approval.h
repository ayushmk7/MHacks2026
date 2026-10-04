// The approval engine: the one firmware screen on which the user says yes or no
// (approval.md, "The request").
#pragma once

#include <Arduino.h>

#include "../core/registry.h"
#include "reason.h"
#include "signer.h"   // SignDomain, Poll

// The Arduino-ESP32 core defines DISABLED as a macro (esp32-hal-gpio.h, an interrupt mode), which
// would turn SelectRule::DISABLED below into `0x00`. Nothing under src/vk/ uses the macro, so it is
// removed for every file that includes this header. Use SelectRule::DISABLED only after including it.
#ifdef DISABLED
#undef DISABLED
#endif

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

// --- Added with the engine (WP12) --------------------------------------------------------------

constexpr uint32_t RESULT_SHOW_MS = 800;     // how long the RESULT screen stays (and until every key is up)
constexpr uint32_t RESULT_KEEP_MS = 60000;   // an un-polled result is dropped this long after its screen closed
constexpr uint32_t FOOTER_BLINK_MS = 200;    // the footer blink after SELECT under rule DISABLED

// Everything the state machine reads or drives outside itself. The firmware fills these from the
// engine's VK_SERVICE begin function; the host test fills them with fakes. A null pointer is safe:
// keys read as up, the signer as VK_SIGN_FAILED, the rest is skipped. Without `now`, open() and
// confirm() return false.
struct Hooks {
  uint32_t (*now)();                           // millis()
  bool (*selectDown)();                        // SELECT held on this pass
  bool (*cancelDown)();                        // CANCEL held on this pass
  bool (*anyKeyDown)();                        // any of the six keys held on this pass
  uint32_t (*configU32)(const char *key);      // vk::config::u32: "approval_tmo_s", "hold_ms"; read once per approval
  Reason (*sign)(const SignDomain *domain, const uint8_t *bytes, size_t len, uint8_t sig[64]);   // signForApproval
  void (*draw)(const ApprovalRequest &, Phase, float holdProgress, const ApprovalOutcome *outcome, bool footerBlink);
  void (*flush)();                             // push the frame to the panel now (before the blocking signature)
  void (*ledPlay)(const char *name);
  void (*ledStop)();
  uint8_t (*backlight)();                      // the backlight level now
  void (*setBacklight)(uint8_t value);
  uint8_t (*userBacklight)();                  // the user's saved level
  void (*repaint)();                           // vk::ui::requestShellRepaint
};
extern Hooks hooks;

#ifdef VK_HOST_TEST
// Host tests only. The dev override is a compile-time switch in the firmware (VK_DEV_ALLOW_UNVERIFIED);
// the host build has it on, and this flag lets a test see the release behaviour as well.
extern bool hostDevAllowUnverified;
void hostReset();                              // back to IDLE with no stored result; hooks are kept
#endif
}  // namespace approval
}  // namespace vk::wallet
