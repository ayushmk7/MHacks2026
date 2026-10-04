// LINK: src/vk/wallet/approval.cpp
// test_approval - the approval state machine driven with a fake clock, fake buttons, a fake signer
// and a fake screen. Spec: docs/os/wallet/approval.md ("State machine", "Rules", "Dev builds").
#include "../../src/vk/wallet/approval.h"

#include <stdio.h>
#include <string.h>

#include <string>
#include <vector>

using namespace vk::wallet;
using approval::Phase;

static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

// ---- fakes ------------------------------------------------------------------------------------

static uint32_t t_now = 0;
static bool k_select = false, k_cancel = false, k_other = false;
static uint32_t cfg_tmo_s = 45, cfg_hold_ms = 3000;
static int cfg_reads = 0;

static int draws = 0, flushes = 0;
static Phase drawn_phase = Phase::IDLE;
static float drawn_progress = 0;
static bool drawn_blink = false, drawn_outcome = false;
static ApprovalOutcome drawn_outcome_value;
static std::string drawn_title;

static int sign_calls = 0, sign_draws_seen = 0, sign_flushes_seen = 0;
static Reason sign_result = VK_OK;
static const SignDomain *sign_domain = nullptr;
static std::vector<uint8_t> sign_bytes;
static Phase sign_phase = Phase::IDLE, sign_drawn_phase = Phase::IDLE;

static std::vector<std::string> led_log;
static uint8_t backlight_now = 0, backlight_user = 180;
static int repaints = 0;

static int listener_calls = 0;
static ApprovalOutcome listener_outcome;
static bool listener_had_sig = false;
static uint8_t listener_sig[64];
static Phase listener_phase = Phase::IDLE;
static std::string listener_domain, listener_app;

static int done_calls = 0;
static bool done_approved = false;
static void *done_arg = nullptr;
static bool done_active = true;

static uint32_t f_now() { return t_now; }
static bool f_select() { return k_select; }
static bool f_cancel() { return k_cancel; }
static bool f_any() { return k_select || k_cancel || k_other; }
static uint32_t f_config(const char *key) {
  cfg_reads++;
  if (strcmp(key, "approval_tmo_s") == 0) return cfg_tmo_s;
  if (strcmp(key, "hold_ms") == 0) return cfg_hold_ms;
  CHECK(!"unexpected config key");
  return 0;
}
static Reason f_sign(const SignDomain *domain, const uint8_t *bytes, size_t len, uint8_t sig[64]) {
  sign_calls++;
  sign_domain = domain;
  sign_bytes.assign(bytes, bytes + len);
  sign_phase = approval::phase();
  sign_drawn_phase = drawn_phase;
  sign_draws_seen = draws;
  sign_flushes_seen = flushes;
  if (sign_result == VK_OK) for (int i = 0; i < 64; i++) sig[i] = (uint8_t)(i ^ 0x5A);
  return sign_result;
}
static void f_draw(const ApprovalRequest &request, Phase phase, float progress, const ApprovalOutcome *outcome, bool blink) {
  draws++;
  drawn_phase = phase;
  drawn_progress = progress;
  drawn_blink = blink;
  drawn_outcome = outcome != nullptr;
  if (outcome) drawn_outcome_value = *outcome;
  drawn_title = request.title;
  CHECK(&request == approval::current());
  CHECK(phase == approval::phase());
}
static void f_flush() { flushes++; }
static void f_led_play(const char *name) { led_log.push_back(name ? name : "(null)"); }
static void f_led_stop() { led_log.push_back("stop"); }
static uint8_t f_backlight() { return backlight_now; }
static void f_set_backlight(uint8_t value) { backlight_now = value; }
static uint8_t f_user_backlight() { return backlight_user; }
static void f_repaint() { repaints++; }

static void on_outcome(const ApprovalOutcome &outcome) {
  listener_calls++;
  listener_outcome = outcome;
  listener_had_sig = outcome.sig != nullptr;
  if (outcome.sig) memcpy(listener_sig, outcome.sig, 64);
  listener_phase = approval::phase();
  listener_domain = outcome.request ? outcome.request->domain : "(null)";
  listener_app = outcome.request ? outcome.request->app_id : "(null)";
}
VK_ON_APPROVAL(test, on_outcome);

static void on_done(bool approved, void *arg) {
  done_calls++;
  done_approved = approved;
  done_arg = arg;
  done_active = approval::active();
}

static SignDomain SOL_DOMAIN("solana", "", true, "sign", 1232, nullptr, nullptr);
static const uint8_t BYTES[5] = {1, 2, 3, 4, 5};

static void reset() {
  approval::hostReset();
  approval::hostDevAllowUnverified = true;
  approval::hooks = approval::Hooks{f_now, f_select, f_cancel, f_any, f_config, f_sign, f_draw, f_flush,
                                    f_led_play, f_led_stop, f_backlight, f_set_backlight, f_user_backlight, f_repaint};
  t_now = 1000;
  k_select = k_cancel = k_other = false;
  cfg_tmo_s = 45; cfg_hold_ms = 3000; cfg_reads = 0;
  draws = flushes = 0; drawn_phase = Phase::IDLE; drawn_progress = 0; drawn_blink = drawn_outcome = false; drawn_title.clear();
  sign_calls = 0; sign_result = VK_OK; sign_domain = nullptr; sign_bytes.clear();
  sign_phase = sign_drawn_phase = Phase::IDLE; sign_draws_seen = sign_flushes_seen = 0;
  led_log.clear(); backlight_now = 0; backlight_user = 180; repaints = 0;
  listener_calls = 0; listener_had_sig = false; listener_phase = Phase::IDLE; listener_domain.clear(); listener_app.clear();
  memset(&listener_outcome, 0, sizeof listener_outcome);
  done_calls = 0; done_approved = false; done_arg = nullptr; done_active = true;
}

static ApprovalRequest request(Severity severity, SelectRule select) {
  ApprovalRequest r{};
  strlcpy(r.title, "Pay", sizeof r.title);
  strlcpy(r.headline, "VERIFIED - PRESENT", sizeof r.headline);
  strlcpy(r.big, "10.00 HACK", sizeof r.big);
  strlcpy(r.sub, "to Demo Merchant", sizeof r.sub);
  strlcpy(r.lines[0].label, "Account", sizeof r.lines[0].label);
  strlcpy(r.lines[0].value, "2awX..6wrr", sizeof r.lines[0].value);
  r.line_count = 1;
  r.severity = severity;
  r.select = select;
  return r;
}

static void step(uint32_t ms) { t_now += ms; approval::update(); }
static bool openSign(Severity severity, SelectRule select, const char *app = "pay") {
  return approval::open(request(severity, select), &SOL_DOMAIN, BYTES, sizeof BYTES, app);
}
// Opens a signing approval and takes it to ARMED with both keys up.
static void armed(Severity severity, SelectRule select, const char *app = "pay") {
  CHECK(openSign(severity, select, app));
  step(10);
  CHECK(approval::phase() == Phase::ARMED);
}
// Lets a RESULT screen run out with every key up.
static void closeResult() {
  k_select = k_cancel = k_other = false;
  CHECK(approval::phase() == Phase::RESULT);
  step(approval::RESULT_SHOW_MS);
  CHECK(approval::phase() == Phase::IDLE);
  CHECK(!approval::active());
}
static Poll take(Reason &reason, uint8_t sig[64]) {
  reason = VK_BUSY;   // a value takeResult never reports
  return approval::takeResult(sig, reason);
}

// ---- tests ------------------------------------------------------------------------------------

static void test_idle() {
  reset();
  CHECK(!approval::active());
  CHECK(approval::phase() == Phase::IDLE);
  CHECK(approval::current() == nullptr);
  CHECK(approval::peekResult() == Poll::IDLE);
  uint8_t sig[64]; Reason reason;
  CHECK(take(reason, sig) == Poll::IDLE && reason == VK_IDLE);
  approval::update();   // a pass with nothing open does nothing
  CHECK(draws == 0 && led_log.empty() && repaints == 0);
}

static void test_open_copies_and_fills() {
  reset();
  uint8_t bytes[5]; memcpy(bytes, BYTES, 5);
  char app[8] = "pay";
  ApprovalRequest r = request(Severity::GREEN, SelectRule::PRESS);
  strlcpy(r.app_id, "forged", sizeof r.app_id);     // the engine overwrites its own fields
  strlcpy(r.domain, "forged", sizeof r.domain);
  r.dev_override = true;
  CHECK(approval::open(r, &SOL_DOMAIN, bytes, sizeof bytes, app));
  memset(bytes, 0xEE, sizeof bytes);                // the caller's buffers may go away
  strcpy(app, "zzz");
  memset(&r, 0x41, sizeof r);

  CHECK(approval::active());
  CHECK(approval::phase() == Phase::WAIT_RELEASE);
  const ApprovalRequest *c = approval::current();
  CHECK(c != nullptr);
  CHECK(c && strcmp(c->title, "Pay") == 0 && strcmp(c->big, "10.00 HACK") == 0);
  CHECK(c && strcmp(c->app_id, "pay") == 0);
  CHECK(c && strcmp(c->domain, "solana") == 0);
  CHECK(c && !c->dev_override);
  CHECK(approval::peekResult() == Poll::PENDING);
  CHECK(cfg_reads == 2);                            // the two config values are read once, at open

  step(10); k_select = true; step(10);
  CHECK(sign_calls == 1 && sign_domain == &SOL_DOMAIN);
  CHECK(sign_bytes == std::vector<uint8_t>(BYTES, BYTES + 5));
  CHECK(cfg_reads == 2);
  closeResult();
}

static void test_open_refusals() {
  reset();
  CHECK(!approval::open(request(Severity::GREEN, SelectRule::PRESS), nullptr, BYTES, sizeof BYTES, "pay"));
  CHECK(!approval::open(request(Severity::GREEN, SelectRule::PRESS), &SOL_DOMAIN, nullptr, 5, "pay"));
  CHECK(!approval::open(request(Severity::GREEN, SelectRule::PRESS), &SOL_DOMAIN, BYTES, 0, "pay"));
  static uint8_t huge[1249];
  CHECK(!approval::open(request(Severity::GREEN, SelectRule::PRESS), &SOL_DOMAIN, huge, sizeof huge, "pay"));
  CHECK(!approval::active() && led_log.empty());
  CHECK(approval::open(request(Severity::GREEN, SelectRule::PRESS), &SOL_DOMAIN, huge, 1248, "pay"));
  approval::hostReset();

  // Without a clock the timeout could not work, so nothing opens.
  approval::hooks.now = nullptr;
  CHECK(!openSign(Severity::GREEN, SelectRule::PRESS));
  CHECK(!approval::confirm(request(Severity::GREEN, SelectRule::PRESS), on_done, nullptr));
  CHECK(!approval::active());
}

static void test_open_false_while_active() {
  reset();
  CHECK(openSign(Severity::GREEN, SelectRule::PRESS));
  CHECK(!openSign(Severity::GREEN, SelectRule::PRESS, "other"));                          // WAIT_RELEASE
  CHECK(!approval::confirm(request(Severity::AMBER, SelectRule::HOLD), on_done, nullptr));
  step(10);
  CHECK(!openSign(Severity::GREEN, SelectRule::PRESS, "other"));                          // ARMED
  k_select = true; step(10);
  CHECK(approval::phase() == Phase::RESULT);
  CHECK(!openSign(Severity::GREEN, SelectRule::PRESS, "other"));                          // RESULT
  CHECK(!approval::confirm(request(Severity::AMBER, SelectRule::HOLD), on_done, nullptr));
  CHECK(strcmp(approval::current()->app_id, "pay") == 0);
  closeResult();
  CHECK(done_calls == 0);

  CHECK(approval::confirm(request(Severity::AMBER, SelectRule::HOLD), on_done, nullptr));
  CHECK(!openSign(Severity::GREEN, SelectRule::PRESS));
  CHECK(!approval::confirm(request(Severity::AMBER, SelectRule::HOLD), on_done, nullptr));
}

static void test_fresh_press() {
  // SELECT held when the screen appears can never approve.
  reset();
  k_select = true;
  CHECK(openSign(Severity::GREEN, SelectRule::PRESS));
  for (int i = 0; i < 200; i++) { step(10); CHECK(approval::phase() == Phase::WAIT_RELEASE); }
  CHECK(sign_calls == 0);
  k_select = false; step(10);
  CHECK(approval::phase() == Phase::ARMED && sign_calls == 0);
  k_select = true; step(10);                         // a press made after the release does approve
  CHECK(sign_calls == 1 && approval::phase() == Phase::RESULT);
  closeResult();

  // Held for the whole approval: it times out, unsigned.
  reset();
  cfg_tmo_s = 10;
  k_select = true;
  CHECK(openSign(Severity::GREEN, SelectRule::PRESS));
  step(9999);
  CHECK(approval::phase() == Phase::WAIT_RELEASE);
  step(1);
  CHECK(approval::phase() == Phase::RESULT && sign_calls == 0);
  CHECK(listener_calls == 1 && !listener_outcome.approved && listener_outcome.reason == VK_TIMEOUT);

  // CANCEL held at open does not cancel either, and it keeps SELECT ignored.
  reset();
  k_cancel = true;
  CHECK(openSign(Severity::GREEN, SelectRule::PRESS));
  step(10); step(10);
  CHECK(approval::phase() == Phase::WAIT_RELEASE && listener_calls == 0);
  k_select = true; step(10);
  CHECK(approval::phase() == Phase::WAIT_RELEASE && sign_calls == 0);
  k_cancel = false; step(10);                        // SELECT still down: still waiting
  CHECK(approval::phase() == Phase::WAIT_RELEASE && sign_calls == 0);
  k_select = false; step(10);
  CHECK(approval::phase() == Phase::ARMED);
  k_cancel = true; step(10);
  CHECK(approval::phase() == Phase::RESULT && listener_outcome.reason == VK_CANCELLED);

  // The same rule for a hold: a key held at open does not start the hold.
  reset();
  k_select = true;
  CHECK(openSign(Severity::AMBER, SelectRule::HOLD));
  step(5000);
  CHECK(approval::phase() == Phase::WAIT_RELEASE && sign_calls == 0);
}

static void test_press() {
  reset();
  backlight_now = 0;                                 // the app turned the backlight off
  CHECK(openSign(Severity::GREEN, SelectRule::PRESS));
  CHECK(backlight_now == 180);                       // the user's saved level while the screen is up
  CHECK(led_log.size() == 1 && led_log[0] == "approve_green");
  CHECK(draws == 0);                                 // open() does not draw: it may be inside an app callback
  step(10);
  CHECK(approval::phase() == Phase::ARMED);
  CHECK(draws == 1 && drawn_phase == Phase::ARMED && !drawn_outcome && !drawn_blink && drawn_title == "Pay");
  step(10); step(10);
  CHECK(draws == 3);                                 // one frame per pass
  CHECK(sign_calls == 0 && listener_calls == 0);

  k_select = true; step(10);
  CHECK(sign_calls == 1);
  CHECK(sign_phase == Phase::SIGNING);               // the signer runs in SIGNING...
  CHECK(sign_drawn_phase == Phase::SIGNING);         // ...after a "Signing..." frame was drawn...
  CHECK(sign_draws_seen == 4 && sign_flushes_seen == 1 && flushes == 1);   // ...and pushed to the panel
  CHECK(approval::phase() == Phase::RESULT);
  CHECK(draws == 5 && drawn_phase == Phase::RESULT && drawn_outcome);
  CHECK(drawn_outcome_value.approved && drawn_outcome_value.reason == VK_OK && drawn_outcome_value.sig != nullptr);
  CHECK(drawn_outcome_value.request == approval::current());

  CHECK(listener_calls == 1 && listener_phase == Phase::RESULT);
  CHECK(listener_outcome.approved && listener_outcome.reason == VK_OK && listener_had_sig);
  CHECK(listener_outcome.request == approval::current());
  CHECK(listener_domain == "solana" && listener_app == "pay");
  for (int i = 0; i < 64; i++) CHECK(listener_sig[i] == (uint8_t)(i ^ 0x5A));

  // Still active, so the result is not out yet and nothing was repainted.
  CHECK(approval::active() && approval::peekResult() == Poll::PENDING);
  uint8_t sig[64]; Reason reason;
  CHECK(take(reason, sig) == Poll::PENDING && reason == VK_OK);
  CHECK(repaints == 0 && backlight_now == 180);

  closeResult();
  CHECK(approval::current() == nullptr);
  CHECK(backlight_now == 0);                         // restored
  CHECK(repaints == 1);
  CHECK(led_log.size() == 2 && led_log[1] == "stop");
  CHECK(sign_calls == 1 && listener_calls == 1);
  const int drawn = draws;
  step(10);
  CHECK(draws == drawn);                             // nothing is drawn once closed

  CHECK(approval::peekResult() == Poll::SIGNED);
  memset(sig, 0, sizeof sig);
  CHECK(take(reason, sig) == Poll::SIGNED && reason == VK_OK);
  for (int i = 0; i < 64; i++) CHECK(sig[i] == (uint8_t)(i ^ 0x5A));
}

static void test_hold_released_early() {
  reset();
  armed(Severity::AMBER, SelectRule::HOLD);
  CHECK(led_log[0] == "approve_amber");
  k_select = true; step(10);
  CHECK(approval::phase() == Phase::HOLDING && drawn_phase == Phase::HOLDING);
  CHECK(drawn_progress >= 0.0f && drawn_progress < 0.01f);
  step(1500);
  CHECK(approval::phase() == Phase::HOLDING);
  CHECK(drawn_progress > 0.49f && drawn_progress < 0.51f);
  step(1499);
  CHECK(approval::phase() == Phase::HOLDING && sign_calls == 0);   // 2999 ms: one short
  k_select = false; step(1);
  CHECK(approval::phase() == Phase::ARMED && sign_calls == 0 && listener_calls == 0);
  CHECK(drawn_progress == 0.0f);

  // A second hold starts from zero: the earlier 2999 ms do not count.
  k_select = true; step(10);
  CHECK(approval::phase() == Phase::HOLDING);
  step(2999);
  CHECK(approval::phase() == Phase::HOLDING && sign_calls == 0);
  k_select = false; step(10);
  CHECK(approval::phase() == Phase::ARMED && sign_calls == 0);
}

static void test_hold_completes() {
  reset();
  armed(Severity::AMBER, SelectRule::HOLD);
  k_select = true; step(10);
  CHECK(approval::phase() == Phase::HOLDING);
  step(2999);
  CHECK(approval::phase() == Phase::HOLDING && sign_calls == 0);
  step(1);
  CHECK(sign_calls == 1 && sign_phase == Phase::SIGNING && sign_drawn_phase == Phase::SIGNING && flushes == 1);
  CHECK(approval::phase() == Phase::RESULT);
  CHECK(listener_calls == 1 && listener_outcome.approved && listener_had_sig);
  step(5000);                                        // SELECT is still down from the hold
  CHECK(approval::phase() == Phase::RESULT);
  closeResult();
  CHECK(approval::peekResult() == Poll::SIGNED && sign_calls == 1);

  // CANCEL during the hold cancels.
  reset();
  armed(Severity::AMBER, SelectRule::HOLD);
  k_select = true; step(10);
  step(1000);
  k_cancel = true; step(10);
  CHECK(approval::phase() == Phase::RESULT && sign_calls == 0);
  CHECK(listener_calls == 1 && !listener_outcome.approved && listener_outcome.reason == VK_CANCELLED);

  // hold_ms comes from config, read at open.
  reset();
  cfg_hold_ms = 1000;
  armed(Severity::AMBER, SelectRule::HOLD);
  cfg_hold_ms = 9000;                                // a change after open does not move this approval
  k_select = true; step(10);
  step(999);
  CHECK(approval::phase() == Phase::HOLDING);
  step(1);
  CHECK(approval::phase() == Phase::RESULT && sign_calls == 1);
}

static void test_disabled() {
  reset();
  armed(Severity::AMBER, SelectRule::DISABLED);
  k_select = true; step(10);
  CHECK(approval::phase() == Phase::ARMED && sign_calls == 0);
  CHECK(drawn_blink);                                // the footer blinks...
  step(approval::FOOTER_BLINK_MS - 1);
  CHECK(drawn_blink);
  step(1);
  CHECK(!drawn_blink);                               // ...once, however long SELECT stays down
  step(20000);
  CHECK(approval::phase() == Phase::ARMED && sign_calls == 0 && !drawn_blink);
  k_select = false; step(10);
  CHECK(!drawn_blink);
  k_select = true; step(10);                         // a new press blinks again
  CHECK(drawn_blink && approval::phase() == Phase::ARMED && sign_calls == 0);
  k_select = false; step(10);
  CHECK(listener_calls == 0);

  k_cancel = true; step(10);
  CHECK(approval::phase() == Phase::RESULT && !drawn_blink);
  CHECK(listener_calls == 1 && !listener_outcome.approved && listener_outcome.reason == VK_CANCELLED);
  closeResult();
  uint8_t sig[64]; Reason reason;
  CHECK(take(reason, sig) == Poll::FAILED && reason == VK_CANCELLED);
  CHECK(sign_calls == 0);
}

static void test_timeout() {
  // In ARMED.
  reset();
  cfg_tmo_s = 10;
  armed(Severity::GREEN, SelectRule::PRESS);
  step(9989);                                        // 9999 ms since open
  CHECK(approval::phase() == Phase::ARMED);
  step(1);
  CHECK(approval::phase() == Phase::RESULT && sign_calls == 0);
  CHECK(listener_calls == 1 && !listener_outcome.approved && listener_outcome.reason == VK_TIMEOUT && !listener_had_sig);
  CHECK(drawn_outcome && drawn_outcome_value.reason == VK_TIMEOUT && drawn_outcome_value.sig == nullptr);
  closeResult();
  uint8_t sig[64]; Reason reason;
  CHECK(take(reason, sig) == Poll::FAILED && reason == VK_TIMEOUT);

  // In HOLDING: the timeout is counted from open, and a hold that has not completed does not sign.
  reset();
  cfg_tmo_s = 10;
  armed(Severity::AMBER, SelectRule::HOLD);
  step(8000);
  k_select = true; step(10);
  CHECK(approval::phase() == Phase::HOLDING);
  step(1979);                                        // 9999 ms since open, 1979 ms into a 3000 ms hold
  CHECK(approval::phase() == Phase::HOLDING);
  step(1);
  CHECK(approval::phase() == Phase::RESULT && sign_calls == 0 && listener_outcome.reason == VK_TIMEOUT);

  // A press on the pass the timeout falls on is too late.
  reset();
  cfg_tmo_s = 10;
  armed(Severity::GREEN, SelectRule::PRESS);
  k_select = true; step(9990);
  CHECK(approval::phase() == Phase::RESULT && sign_calls == 0 && listener_outcome.reason == VK_TIMEOUT);

  // The clock may wrap during an approval.
  reset();
  t_now = 0xFFFFFFFFu - 2000;
  cfg_tmo_s = 10;
  CHECK(openSign(Severity::GREEN, SelectRule::PRESS));
  step(10);
  step(5000);                                        // past the wrap
  CHECK(approval::phase() == Phase::ARMED);
  step(4989);
  CHECK(approval::phase() == Phase::ARMED);
  step(1);
  CHECK(approval::phase() == Phase::RESULT && listener_outcome.reason == VK_TIMEOUT);

  // Out-of-range config values are pulled into the documented ranges (10-120 s, 1000-10000 ms).
  reset();
  cfg_tmo_s = 0; cfg_hold_ms = 0;
  armed(Severity::AMBER, SelectRule::HOLD);
  k_select = true; step(10);
  step(999);
  CHECK(approval::phase() == Phase::HOLDING && sign_calls == 0);   // never an instant approval
  k_select = false; step(10);
  step(8970);                                        // 9999 ms since open
  CHECK(approval::phase() == Phase::ARMED);
  step(1);
  CHECK(approval::phase() == Phase::RESULT && listener_outcome.reason == VK_TIMEOUT);
  reset();
  cfg_tmo_s = 100000;
  armed(Severity::GREEN, SelectRule::PRESS);
  step(119989);
  CHECK(approval::phase() == Phase::ARMED);
  step(1);
  CHECK(approval::phase() == Phase::RESULT);
}

static void test_red() {
  // CANCEL: the reason is red_reason, not "cancelled".
  reset();
  approval::hostDevAllowUnverified = false;
  ApprovalRequest r = request(Severity::RED, SelectRule::PRESS);   // the select field is ignored for RED
  r.red_reason = VK_MISMATCH;
  CHECK(approval::open(r, &SOL_DOMAIN, BYTES, sizeof BYTES, "pay"));
  CHECK(led_log[0] == "approve_red");
  CHECK(approval::current()->select == SelectRule::DISABLED && !approval::current()->dev_override);
  step(10);
  k_select = true; step(10);
  CHECK(approval::phase() == Phase::ARMED && sign_calls == 0 && drawn_blink);
  step(20000);
  CHECK(approval::phase() == Phase::ARMED && sign_calls == 0);
  k_select = false; step(10);
  k_cancel = true; step(10);
  CHECK(approval::phase() == Phase::RESULT);
  CHECK(listener_calls == 1 && !listener_outcome.approved && listener_outcome.reason == VK_MISMATCH);
  closeResult();
  uint8_t sig[64]; Reason reason;
  CHECK(take(reason, sig) == Poll::FAILED && reason == VK_MISMATCH);

  // Timeout: red_reason again.
  reset();
  approval::hostDevAllowUnverified = false;
  cfg_tmo_s = 10;
  r = request(Severity::RED, SelectRule::HOLD);
  r.red_reason = VK_UNDECODABLE;
  CHECK(approval::open(r, &SOL_DOMAIN, BYTES, sizeof BYTES, "pay"));
  step(10);
  step(9990);
  CHECK(approval::phase() == Phase::RESULT && listener_outcome.reason == VK_UNDECODABLE);
  closeResult();
  CHECK(take(reason, sig) == Poll::FAILED && reason == VK_UNDECODABLE);
  CHECK(sign_calls == 0);

  // A severity outside the enum is treated as RED: closed.
  reset();
  r = request((Severity)7, SelectRule::PRESS);
  CHECK(approval::open(r, &SOL_DOMAIN, BYTES, sizeof BYTES, "pay"));
  CHECK(approval::current()->severity == Severity::RED && approval::current()->select == SelectRule::DISABLED);
  step(10); k_select = true; step(10);
  CHECK(sign_calls == 0 && approval::phase() == Phase::ARMED);
  // And a select rule outside the enum is DISABLED.
  reset();
  CHECK(approval::open(request(Severity::GREEN, (SelectRule)9), &SOL_DOMAIN, BYTES, sizeof BYTES, "pay"));
  CHECK(approval::current()->select == SelectRule::DISABLED);
  step(10); k_select = true; step(10);
  CHECK(sign_calls == 0 && approval::phase() == Phase::ARMED);
}

static void test_dev_override() {
  uint8_t sig[64]; Reason reason;

  // Dev build, dev_overridable: a hold signs, and the request says so.
  reset();
  ApprovalRequest r = request(Severity::RED, SelectRule::PRESS);
  r.red_reason = VK_UNVERIFIED;
  r.dev_overridable = true;
  CHECK(approval::open(r, &SOL_DOMAIN, BYTES, sizeof BYTES, "pay"));
  CHECK(approval::current()->dev_override);
  CHECK(approval::current()->select == SelectRule::HOLD);          // a hold, never a press
  CHECK(approval::current()->severity == Severity::RED);
  CHECK(led_log[0] == "approve_red");
  step(10);
  k_select = true; step(10);
  CHECK(approval::phase() == Phase::HOLDING && sign_calls == 0);
  step(3000);
  CHECK(sign_calls == 1 && approval::phase() == Phase::RESULT);
  CHECK(listener_outcome.approved && listener_outcome.reason == VK_OK && listener_outcome.request->dev_override);
  closeResult();
  CHECK(take(reason, sig) == Poll::SIGNED && reason == VK_OK);

  // Overridable but cancelled: still reports why it was red.
  reset();
  CHECK(approval::open(r, &SOL_DOMAIN, BYTES, sizeof BYTES, "pay"));
  step(10);
  k_cancel = true; step(10);
  CHECK(approval::phase() == Phase::RESULT && listener_outcome.reason == VK_UNVERIFIED && !listener_outcome.approved);
  closeResult();
  CHECK(take(reason, sig) == Poll::FAILED && reason == VK_UNVERIFIED);

  // Dev build, not overridable: closed.
  reset();
  r.dev_overridable = false;
  CHECK(approval::open(r, &SOL_DOMAIN, BYTES, sizeof BYTES, "pay"));
  CHECK(!approval::current()->dev_override && approval::current()->select == SelectRule::DISABLED);
  step(10);
  k_select = true; step(10); step(20000);
  CHECK(approval::phase() == Phase::ARMED && sign_calls == 0);

  // Release behaviour: overridable makes no difference.
  reset();
  approval::hostDevAllowUnverified = false;
  r.dev_overridable = true;
  CHECK(approval::open(r, &SOL_DOMAIN, BYTES, sizeof BYTES, "pay"));
  CHECK(!approval::current()->dev_override && approval::current()->select == SelectRule::DISABLED);
  step(10);
  k_select = true; step(10); step(20000);
  CHECK(approval::phase() == Phase::ARMED && sign_calls == 0);

  // dev_overridable means nothing on a request that is not RED.
  reset();
  r = request(Severity::AMBER, SelectRule::DISABLED);
  r.dev_overridable = true;
  CHECK(approval::open(r, &SOL_DOMAIN, BYTES, sizeof BYTES, "pay"));
  CHECK(!approval::current()->dev_override && approval::current()->select == SelectRule::DISABLED);
}

static void test_sign_failed() {
  reset();
  sign_result = VK_SIGN_FAILED;
  armed(Severity::GREEN, SelectRule::PRESS);
  k_select = true; step(10);
  CHECK(sign_calls == 1 && approval::phase() == Phase::RESULT);
  CHECK(listener_calls == 1 && !listener_outcome.approved && listener_outcome.reason == VK_SIGN_FAILED && !listener_had_sig);
  CHECK(drawn_outcome && !drawn_outcome_value.approved && drawn_outcome_value.sig == nullptr);
  closeResult();
  uint8_t sig[64]; Reason reason;
  memset(sig, 0x77, sizeof sig);
  CHECK(approval::peekResult() == Poll::FAILED);
  CHECK(take(reason, sig) == Poll::FAILED && reason == VK_SIGN_FAILED);
  for (int i = 0; i < 64; i++) CHECK(sig[i] == 0x77);              // no signature is handed out

  // No signer at all is a sign failure, not a crash.
  reset();
  approval::hooks.sign = nullptr;
  armed(Severity::GREEN, SelectRule::PRESS);
  k_select = true; step(10);
  CHECK(approval::phase() == Phase::RESULT && listener_outcome.reason == VK_SIGN_FAILED && !listener_outcome.approved);
}

static void test_confirm() {
  int marker = 0;
  uint8_t sig[64]; Reason reason;

  // PRESS -> done(true), with no SIGNING and no signer.
  reset();
  ApprovalRequest r = request(Severity::GREEN, SelectRule::PRESS);
  strlcpy(r.app_id, "forged", sizeof r.app_id);
  CHECK(approval::confirm(r, on_done, &marker));
  CHECK(approval::active() && approval::phase() == Phase::WAIT_RELEASE);
  CHECK(strcmp(approval::current()->domain, "confirm") == 0);
  CHECK(approval::current()->app_id[0] == '\0');                   // raised by firmware: no "asked by"
  CHECK(approval::peekResult() == Poll::IDLE);                     // a confirmation is not a signing approval
  CHECK(led_log[0] == "approve_green" && backlight_now == 180);
  step(10);
  k_select = true; step(10);
  CHECK(approval::phase() == Phase::RESULT && sign_calls == 0 && flushes == 0);
  CHECK(listener_calls == 1 && listener_outcome.approved && listener_outcome.reason == VK_OK && !listener_had_sig);
  CHECK(listener_domain == "confirm");
  CHECK(done_calls == 0);                                          // done is called when the screen closes
  closeResult();
  CHECK(done_calls == 1 && done_approved && done_arg == &marker);
  CHECK(!done_active);                                             // already closed when done runs
  CHECK(repaints == 1 && backlight_now == 0 && led_log.back() == "stop");
  CHECK(take(reason, sig) == Poll::IDLE && reason == VK_IDLE);     // and it leaves no result
  step(1000);
  CHECK(done_calls == 1);

  // HOLD -> done(true) after hold_ms.
  reset();
  CHECK(approval::confirm(request(Severity::AMBER, SelectRule::HOLD), on_done, &marker));
  step(10);
  k_select = true; step(10);
  step(2999);
  CHECK(approval::phase() == Phase::HOLDING);
  step(1);
  CHECK(approval::phase() == Phase::RESULT && sign_calls == 0);
  closeResult();
  CHECK(done_calls == 1 && done_approved);

  // CANCEL -> done(false).
  reset();
  CHECK(approval::confirm(request(Severity::AMBER, SelectRule::HOLD), on_done, &marker));
  step(10);
  k_cancel = true; step(10);
  CHECK(approval::phase() == Phase::RESULT && listener_outcome.reason == VK_CANCELLED);
  closeResult();
  CHECK(done_calls == 1 && !done_approved && done_arg == &marker);

  // Timeout -> done(false).
  reset();
  cfg_tmo_s = 10;
  CHECK(approval::confirm(request(Severity::AMBER, SelectRule::HOLD), on_done, nullptr));
  step(10000);
  CHECK(approval::phase() == Phase::RESULT && listener_outcome.reason == VK_TIMEOUT);
  closeResult();
  CHECK(done_calls == 1 && !done_approved && done_arg == nullptr);

  // RED confirmation: closed; done(false); the outcome carries red_reason.
  reset();
  r = request(Severity::RED, SelectRule::PRESS);
  r.red_reason = VK_UNVERIFIED;
  CHECK(approval::confirm(r, on_done, nullptr));
  step(10);
  k_select = true; step(10); step(5000);
  CHECK(approval::phase() == Phase::ARMED);
  k_select = false; step(10);
  k_cancel = true; step(10);
  CHECK(approval::phase() == Phase::RESULT && listener_outcome.reason == VK_UNVERIFIED);
  closeResult();
  CHECK(done_calls == 1 && !done_approved);

  // A null done is allowed.
  reset();
  CHECK(approval::confirm(request(Severity::GREEN, SelectRule::PRESS), nullptr, nullptr));
  step(10); k_select = true; step(10);
  closeResult();
  CHECK(done_calls == 0);

  // A confirmation does not disturb a signing result that is waiting to be polled.
  reset();
  armed(Severity::GREEN, SelectRule::PRESS);
  k_select = true; step(10);
  closeResult();
  CHECK(approval::peekResult() == Poll::SIGNED);
  CHECK(approval::confirm(request(Severity::AMBER, SelectRule::HOLD), on_done, nullptr));
  CHECK(approval::peekResult() == Poll::SIGNED);
  step(10); k_cancel = true; step(10);
  closeResult();
  CHECK(done_calls == 1 && !done_approved);
  CHECK(take(reason, sig) == Poll::SIGNED && reason == VK_OK);
  for (int i = 0; i < 64; i++) CHECK(sig[i] == (uint8_t)(i ^ 0x5A));
}

static int chain_calls = 0;
static bool chain_opened = false;
static void on_done_chain(bool approved, void *) {
  chain_calls++;
  if (approved) chain_opened = approval::confirm(request(Severity::AMBER, SelectRule::HOLD), on_done, nullptr);
}
static void test_confirm_done_may_open_another() {
  reset();
  chain_calls = 0; chain_opened = false;
  CHECK(approval::confirm(request(Severity::GREEN, SelectRule::PRESS), on_done_chain, nullptr));
  step(10); k_select = true; step(10);
  k_select = false;
  step(approval::RESULT_SHOW_MS);                    // the first screen closes; its done raises the next
  CHECK(chain_calls == 1 && chain_opened);
  CHECK(approval::active() && approval::phase() == Phase::WAIT_RELEASE);
  CHECK(approval::current() != nullptr && approval::current()->severity == Severity::AMBER);
  step(10); k_cancel = true; step(10);
  if (approval::active()) closeResult();
  CHECK(chain_calls == 1 && done_calls == 1 && !done_approved);
}

static void test_listeners_once() {
  reset();
  // signed
  armed(Severity::GREEN, SelectRule::PRESS);
  k_select = true; step(10);
  for (int i = 0; i < 50; i++) step(10);              // SELECT held through RESULT
  CHECK(listener_calls == 1);
  closeResult();
  step(100);
  CHECK(listener_calls == 1);
  // cancelled
  armed(Severity::GREEN, SelectRule::PRESS);
  k_cancel = true; step(10);
  for (int i = 0; i < 50; i++) step(10);
  CHECK(listener_calls == 2);
  closeResult();
  // timeout
  armed(Severity::GREEN, SelectRule::PRESS);
  step(45000);
  for (int i = 0; i < 50; i++) step(10);
  CHECK(listener_calls == 3);
  closeResult();
  // sign failed
  sign_result = VK_SIGN_FAILED;
  armed(Severity::GREEN, SelectRule::PRESS);
  k_select = true; step(10);
  CHECK(listener_calls == 4);
  closeResult();
  // confirmation approved and refused
  CHECK(approval::confirm(request(Severity::GREEN, SelectRule::PRESS), on_done, nullptr));
  step(10); k_select = true; step(10);
  CHECK(listener_calls == 5);
  closeResult();
  CHECK(approval::confirm(request(Severity::GREEN, SelectRule::PRESS), on_done, nullptr));
  step(10); k_cancel = true; step(10);
  CHECK(listener_calls == 6);
  closeResult();
  step(100000);
  CHECK(listener_calls == 6 && done_calls == 2);
}

static void test_result_waits() {
  // 800 ms, and every key up.
  reset();
  armed(Severity::GREEN, SelectRule::PRESS);
  k_select = true; step(10);
  CHECK(approval::phase() == Phase::RESULT);
  k_select = false;
  step(approval::RESULT_SHOW_MS - 1);
  CHECK(approval::phase() == Phase::RESULT && approval::active());
  step(1);
  CHECK(approval::phase() == Phase::IDLE);

  // SELECT still down after 800 ms: stays until it is released.
  reset();
  armed(Severity::GREEN, SelectRule::PRESS);
  k_select = true; step(10);
  step(5000);
  CHECK(approval::phase() == Phase::RESULT && repaints == 0);
  CHECK(approval::peekResult() == Poll::PENDING);
  CHECK(drawn_phase == Phase::RESULT && drawn_outcome);            // and keeps drawing the result
  k_select = false; step(10);
  CHECK(approval::phase() == Phase::IDLE && repaints == 1);

  // CANCEL held through the approval's end (it would count towards upstream's force-quit).
  reset();
  armed(Severity::GREEN, SelectRule::PRESS);
  k_cancel = true; step(10);
  step(3000);
  CHECK(approval::phase() == Phase::RESULT);
  k_cancel = false; step(10);
  CHECK(approval::phase() == Phase::IDLE);

  // Any other key counts too.
  reset();
  armed(Severity::GREEN, SelectRule::PRESS);
  k_cancel = true; step(10);
  k_cancel = false; k_other = true;
  step(3000);
  CHECK(approval::phase() == Phase::RESULT);
  k_other = false; step(10);
  CHECK(approval::phase() == Phase::IDLE);

  // Keys other than SELECT and CANCEL do nothing before the result.
  reset();
  k_other = true;
  CHECK(openSign(Severity::GREEN, SelectRule::PRESS));
  step(10);
  CHECK(approval::phase() == Phase::ARMED);           // only SELECT and CANCEL must be up to arm
  step(1000);
  CHECK(approval::phase() == Phase::ARMED && sign_calls == 0 && listener_calls == 0);
}

static void test_app_stopping() {
  uint8_t sig[64]; Reason reason;

  // WAIT_RELEASE
  reset();
  k_select = true;
  CHECK(openSign(Severity::GREEN, SelectRule::PRESS, "pay"));
  step(10);
  CHECK(approval::phase() == Phase::WAIT_RELEASE);
  approval::appStopping("other");                    // another app: nothing happens
  approval::appStopping("");                         // upstream stop() with nothing running
  approval::appStopping(nullptr);
  CHECK(approval::active());
  approval::appStopping("pay");
  CHECK(!approval::active() && approval::phase() == Phase::IDLE && approval::current() == nullptr);
  CHECK(approval::peekResult() == Poll::IDLE);
  CHECK(take(reason, sig) == Poll::IDLE && reason == VK_IDLE);
  CHECK(backlight_now == 0 && repaints == 1 && led_log.back() == "stop");
  CHECK(listener_calls == 0 && sign_calls == 0);
  CHECK(openSign(Severity::GREEN, SelectRule::PRESS, "other"));     // the next begin is not busy
  approval::hostReset();

  // ARMED
  reset();
  armed(Severity::GREEN, SelectRule::PRESS, "pay");
  approval::appStopping("pay");
  CHECK(!approval::active() && approval::peekResult() == Poll::IDLE);
  k_select = true; step(10);                         // a press after the drop signs nothing
  CHECK(sign_calls == 0 && listener_calls == 0);
  k_select = false;
  CHECK(openSign(Severity::GREEN, SelectRule::PRESS, "other"));
  approval::hostReset();

  // HOLDING
  reset();
  armed(Severity::AMBER, SelectRule::HOLD, "pay");
  k_select = true; step(10);
  step(1500);
  CHECK(approval::phase() == Phase::HOLDING);
  approval::appStopping("pay");
  CHECK(!approval::active() && approval::peekResult() == Poll::IDLE);
  step(5000);                                        // SELECT still held: the hold does not complete
  CHECK(sign_calls == 0 && listener_calls == 0 && !approval::active());
  k_select = false;
  CHECK(openSign(Severity::GREEN, SelectRule::PRESS, "other"));
  approval::hostReset();

  // RESULT: the screen and the result both go.
  reset();
  armed(Severity::GREEN, SelectRule::PRESS, "pay");
  k_select = true; step(10);
  CHECK(approval::phase() == Phase::RESULT);
  approval::appStopping("pay");
  CHECK(!approval::active() && approval::peekResult() == Poll::IDLE && repaints == 1);

  // An un-taken result, after the screen closed.
  reset();
  armed(Severity::GREEN, SelectRule::PRESS, "pay");
  k_select = true; step(10);
  closeResult();
  CHECK(approval::peekResult() == Poll::SIGNED);
  approval::appStopping("other");
  approval::appStopping("");
  CHECK(approval::peekResult() == Poll::SIGNED);     // not its owner: kept
  approval::appStopping("pay");
  CHECK(approval::peekResult() == Poll::IDLE);
  memset(sig, 0x33, sizeof sig);
  CHECK(take(reason, sig) == Poll::IDLE && reason == VK_IDLE);
  for (int i = 0; i < 64; i++) CHECK(sig[i] == 0x33);
  CHECK(openSign(Severity::GREEN, SelectRule::PRESS, "other"));
  approval::hostReset();

  // A failed result is dropped the same way.
  reset();
  armed(Severity::GREEN, SelectRule::PRESS, "pay");
  k_cancel = true; step(10);
  closeResult();
  CHECK(approval::peekResult() == Poll::FAILED);
  approval::appStopping("pay");
  CHECK(approval::peekResult() == Poll::IDLE);

  // A confirmation belongs to no app: an app stopping never closes it.
  reset();
  CHECK(approval::confirm(request(Severity::AMBER, SelectRule::HOLD), on_done, nullptr));
  step(10);
  approval::appStopping("pay");
  approval::appStopping("confirm");
  approval::appStopping("");
  CHECK(approval::active() && approval::phase() == Phase::ARMED && done_calls == 0);

  // A confirmation is open while app A's result waits; A stops: the result goes, the screen stays.
  reset();
  armed(Severity::GREEN, SelectRule::PRESS, "pay");
  k_select = true; step(10);
  closeResult();
  CHECK(approval::confirm(request(Severity::AMBER, SelectRule::HOLD), on_done, nullptr));
  approval::appStopping("pay");
  CHECK(approval::active() && approval::peekResult() == Poll::IDLE);

  // The owner is matched on the id as stored (32 characters).
  reset();
  const char *longId = "abcdefghijklmnopqrstuvwxyz0123456789";     // 36 characters
  armed(Severity::GREEN, SelectRule::PRESS, longId);
  CHECK(strcmp(approval::current()->app_id, "abcdefghijklmnopqrstuvwxyz012345") == 0);
  approval::appStopping(longId);
  CHECK(!approval::active());
}

static void test_result_dropped_after_60s() {
  uint8_t sig[64]; Reason reason;
  reset();
  armed(Severity::GREEN, SelectRule::PRESS);
  k_select = true; step(10);
  closeResult();                                     // the 60 s run from here
  t_now += approval::RESULT_KEEP_MS - 1;
  CHECK(approval::peekResult() == Poll::SIGNED);     // still busy for begin()
  t_now += 1;
  CHECK(approval::peekResult() == Poll::IDLE);
  CHECK(take(reason, sig) == Poll::IDLE && reason == VK_IDLE);

  // The time spent on the screen (SELECT held through RESULT) is not counted.
  reset();
  armed(Severity::GREEN, SelectRule::PRESS);
  k_select = true; step(10);
  step(100000);
  CHECK(approval::phase() == Phase::RESULT);
  closeResult();
  t_now += approval::RESULT_KEEP_MS - 1;
  CHECK(take(reason, sig) == Poll::SIGNED);

  // A failed result expires too.
  reset();
  armed(Severity::GREEN, SelectRule::PRESS);
  k_cancel = true; step(10);
  closeResult();
  t_now += approval::RESULT_KEEP_MS;
  CHECK(take(reason, sig) == Poll::IDLE && reason == VK_IDLE);

  // Across a wrap of the clock.
  reset();
  t_now = 0xFFFFFFFFu - 30000;
  CHECK(openSign(Severity::GREEN, SelectRule::PRESS));
  step(10); k_select = true; step(10);
  closeResult();
  t_now += 59000;
  CHECK(approval::peekResult() == Poll::SIGNED);
  t_now += 1000;
  CHECK(approval::peekResult() == Poll::IDLE);
}

static void test_take_once() {
  uint8_t sig[64]; Reason reason;
  reset();
  armed(Severity::GREEN, SelectRule::PRESS);
  k_select = true; step(10);
  closeResult();
  CHECK(approval::peekResult() == Poll::SIGNED);
  CHECK(approval::peekResult() == Poll::SIGNED);     // peeking does not consume
  CHECK(take(reason, sig) == Poll::SIGNED && reason == VK_OK);
  CHECK(take(reason, sig) == Poll::IDLE && reason == VK_IDLE);
  CHECK(approval::peekResult() == Poll::IDLE);

  armed(Severity::GREEN, SelectRule::PRESS);
  k_cancel = true; step(10);
  closeResult();
  CHECK(approval::peekResult() == Poll::FAILED);
  CHECK(approval::peekResult() == Poll::FAILED);
  CHECK(take(reason, sig) == Poll::FAILED && reason == VK_CANCELLED);
  CHECK(take(reason, sig) == Poll::IDLE && reason == VK_IDLE);

  // takeResult accepts a null sig pointer.
  armed(Severity::GREEN, SelectRule::PRESS);
  k_select = true; step(10);
  closeResult();
  reason = VK_BUSY;
  CHECK(approval::takeResult(nullptr, reason) == Poll::SIGNED && reason == VK_OK);
  CHECK(approval::peekResult() == Poll::IDLE);
}

static void test_strings() {
  reset();
  ApprovalRequest r{};
  memset(r.title, 'T', sizeof r.title);              // not NUL-terminated
  memset(r.headline, 'H', sizeof r.headline);
  strcpy(r.big, "1\x01" "0\t.\x7F\xC3\xA9 ~");       // control bytes, DEL, UTF-8
  memset(r.sub, 0xFF, sizeof r.sub);
  memset(r.lines[3].label, 'L', sizeof r.lines[3].label);
  memset(r.lines[3].value, '\n', sizeof r.lines[3].value);
  memset(r.recipient_name, 'N', sizeof r.recipient_name);
  memset(r.symbol, 'S', sizeof r.symbol);
  r.line_count = 200;
  r.severity = Severity::GREEN;
  r.select = SelectRule::PRESS;
  CHECK(approval::open(r, &SOL_DOMAIN, BYTES, sizeof BYTES, "p\x02y"));
  const ApprovalRequest *c = approval::current();
  CHECK(strlen(c->title) == sizeof c->title - 1 && c->title[0] == 'T');
  CHECK(strlen(c->headline) == sizeof c->headline - 1);
  CHECK(strcmp(c->big, "1?0?.??? ~") == 0);
  CHECK(strlen(c->sub) == sizeof c->sub - 1 && c->sub[0] == '?' && c->sub[sizeof c->sub - 2] == '?');
  CHECK(strlen(c->lines[3].label) == sizeof c->lines[3].label - 1);
  CHECK(strlen(c->lines[3].value) == sizeof c->lines[3].value - 1 && c->lines[3].value[0] == '?');
  CHECK(strlen(c->recipient_name) == sizeof c->recipient_name - 1);
  CHECK(strlen(c->symbol) == sizeof c->symbol - 1);
  CHECK(c->line_count == 4);
  CHECK(strcmp(c->app_id, "p?y") == 0);
  // Every byte of every text field is printable ASCII or NUL.
  const struct { const char *p; size_t n; } fields[] = {
      {c->title, sizeof c->title}, {c->headline, sizeof c->headline}, {c->big, sizeof c->big}, {c->sub, sizeof c->sub},
      {c->lines[0].label, sizeof c->lines[0].label}, {c->lines[0].value, sizeof c->lines[0].value},
      {c->lines[3].label, sizeof c->lines[3].label}, {c->lines[3].value, sizeof c->lines[3].value},
      {c->recipient_name, sizeof c->recipient_name}, {c->symbol, sizeof c->symbol},
      {c->app_id, sizeof c->app_id}, {c->domain, sizeof c->domain}};
  for (const auto &f : fields) {
    CHECK(f.p[f.n - 1] == '\0');
    for (size_t i = 0; i < f.n; i++) CHECK(f.p[i] == '\0' || (f.p[i] >= 0x20 && f.p[i] <= 0x7E));
  }

  // A null app id and a long domain name are handled.
  reset();
  static SignDomain LONG("a-very-long-domain-name", "", true, "sign", 64, nullptr, nullptr);
  CHECK(approval::open(request(Severity::GREEN, SelectRule::PRESS), &LONG, BYTES, sizeof BYTES, nullptr));
  CHECK(approval::current()->app_id[0] == '\0');
  CHECK(strcmp(approval::current()->domain, "a-very-long") == 0);   // char[12]
}

static void test_leds_and_backlight() {
  // A named pattern wins over the severity's.
  reset();
  ApprovalRequest r = request(Severity::GREEN, SelectRule::PRESS);
  r.led = "rainbow";
  CHECK(approval::open(r, &SOL_DOMAIN, BYTES, sizeof BYTES, "pay"));
  CHECK(led_log.size() == 1 && led_log[0] == "rainbow");
  approval::hostReset();
  reset();
  r.led = "";                                        // empty means "by severity"
  r.severity = Severity::AMBER;
  CHECK(approval::open(r, &SOL_DOMAIN, BYTES, sizeof BYTES, "pay"));
  CHECK(led_log.size() == 1 && led_log[0] == "approve_amber");

  // The backlight the app left is restored, whatever it was.
  reset();
  backlight_now = 37; backlight_user = 200;
  armed(Severity::GREEN, SelectRule::PRESS);
  CHECK(backlight_now == 200);
  k_cancel = true; step(10);
  CHECK(backlight_now == 200);
  closeResult();
  CHECK(backlight_now == 37);

  // Missing hooks are skipped.
  reset();
  approval::hooks.draw = nullptr; approval::hooks.flush = nullptr; approval::hooks.ledPlay = nullptr;
  approval::hooks.ledStop = nullptr; approval::hooks.backlight = nullptr; approval::hooks.setBacklight = nullptr;
  approval::hooks.userBacklight = nullptr; approval::hooks.repaint = nullptr; approval::hooks.anyKeyDown = nullptr;
  approval::hooks.configU32 = nullptr;
  armed(Severity::GREEN, SelectRule::PRESS);
  k_select = true; step(10);
  CHECK(sign_calls == 1 && approval::phase() == Phase::RESULT);
  step(approval::RESULT_SHOW_MS);
  CHECK(approval::phase() == Phase::RESULT);         // without anyKeyDown, SELECT and CANCEL still count
  k_select = false; step(10);
  CHECK(approval::phase() == Phase::IDLE && approval::peekResult() == Poll::SIGNED);
}

int main() {
  setvbuf(stdout, nullptr, _IONBF, 0);   // a FAIL line is not lost if a later check crashes
  test_idle();
  test_open_copies_and_fills();
  test_open_refusals();
  test_open_false_while_active();
  test_fresh_press();
  test_press();
  test_hold_released_early();
  test_hold_completes();
  test_disabled();
  test_timeout();
  test_red();
  test_dev_override();
  test_sign_failed();
  test_confirm();
  test_confirm_done_may_open_another();
  test_listeners_once();
  test_result_waits();
  test_app_stopping();
  test_result_dropped_after_60s();
  test_take_once();
  test_strings();
  test_leds_and_backlight();
  printf(fails ? "%d FAILED\n" : "all approval tests passed\n", fails);
  return fails != 0;
}
