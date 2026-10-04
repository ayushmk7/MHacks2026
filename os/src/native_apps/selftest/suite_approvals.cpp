// src/native_apps/selftest/suite_approvals.cpp
// APPROVAL SCREENS (dev profile only): the green, amber and red looks of the approval screen, for a
// person to eyeball. Each row raises a demo confirmation the way the dev command VKDEMOAPPROVE does
// (approval::confirm: a confirmation, never a signature), then asks whether it looked right.
//
//   SELECT on a row     the demo approval opens (green: SELECT approves, amber: hold SELECT, red:
//                       SELECT is blocked); any way of closing it is fine
//   then                SELECT looked right (OK) · LEFT looked wrong (FAIL) · CANCEL skip (--)
//
// Every demo says DEMO and "nothing is signed" on the screen. Like any confirmation it is written
// to the history (domain `confirm`). Release builds have no demo approvals (VK_TEST_HOOKS is 0):
// this whole suite is compiled out there, and the profile field hides it from the runner too.
#include "../../vk/vk_build.h"
#include "selftest.h"
#include "suites.h"

#if VK_TEST_HOOKS

#include "../../vk/sdk/badge_sdk.hpp"

#include <string.h>

#include "../../vk/ui/receipt.h"
#include "../../vk/wallet/approval.h"   // last: it removes the Arduino core's DISABLED macro
#include "ui.h"

namespace selftest {
namespace {

namespace rc = vk::ui::receipt;
using vk::wallet::ApprovalRequest;
using vk::wallet::SelectRule;
using vk::wallet::Severity;

constexpr uint32_t ANSWER_TIMEOUT_MS = 60000;   // nobody answers "did it look right": "--"

// The answer of the confirmation on screen: -1 while it is open, else 0 refused / 1 approved. A file
// variable, not the app object: `done` is called by the engine, which outlives any one app run.
volatile int sAnswer = -1;
void onDone(bool approved, void *) { sAnswer = approved ? 1 : 0; }

enum : uint8_t { STEP_OPEN = 1, STEP_ASK = 2 };
struct DemoScratch { uint32_t askedAt; };

// Which look a row shows: its index in the table below.
struct Look { const char *name; const char *headline; Severity severity; SelectRule select; };
const Look LOOKS[] = {
    {"green", "VERIFIED - PRESENT", Severity::GREEN, SelectRule::PRESS},
    {"amber", "VERIFIED - NOT PRESENT", Severity::AMBER, SelectRule::HOLD},
    {"red", "UNVERIFIED RECIPIENT", Severity::RED, SelectRule::DISABLED},
};

void demoStart(Ctx &c) {
  const Look &look = LOOKS[c.index < 3 ? c.index : 2];
  ApprovalRequest request{};
  strlcpy(request.title, "Self test", sizeof request.title);
  strlcpy(request.headline, look.headline, sizeof request.headline);
  strlcpy(request.big, "DEMO", sizeof request.big);
  strlcpy(request.sub, "nothing is signed", sizeof request.sub);
  strlcpy(request.lines[0].label, "Look", sizeof request.lines[0].label);
  strlcpy(request.lines[0].value, look.name, sizeof request.lines[0].value);
  strlcpy(request.lines[1].label, "Signs", sizeof request.lines[1].label);
  strlcpy(request.lines[1].value, "nothing (a demo)", sizeof request.lines[1].value);
  request.line_count = 2;
  request.severity = look.severity;
  request.select = look.select;
  request.red_reason = VK_UNVERIFIED;
  request.dev_overridable = false;       // a closed red screen, in the dev profile too

  sAnswer = -1;
  if (!vk::wallet::approval::confirm(request, onDone, nullptr)) {
    c.finish(State::Skip, "busy");
    return;
  }
  c.step = STEP_OPEN;
}

void demoUpdate(Ctx &c) {
  if (c.step == STEP_OPEN && sAnswer >= 0) {
    c.step = STEP_ASK;
    c.scratch<DemoScratch>().askedAt = c.now;
    c.redraw = true;
  } else if (c.step == STEP_ASK && c.now - c.scratch<DemoScratch>().askedAt >= ANSWER_TIMEOUT_MS) {
    c.finish(State::Skip, "no answer");
  }
}

void demoDraw(Ctx &c) {
  const Look &look = LOOKS[c.index < 3 ? c.index : 2];
  ui::frame("APPROVAL SCREENS");
  rc::row(ui::X0, ui::X1, ui::FIRST_ROW_Y, "LOOK", look.name);
  rc::row(ui::X0, ui::X1, ui::FIRST_ROW_Y + ui::ROW_PITCH, "CLOSED",
          c.step != STEP_ASK ? "..." : sAnswer == 1 ? "approved" : "refused");
  display::textCentered("Did the approval look right?", display::width() / 2, ui::NOTE_Y, ui::sub());
  rc::footer("SELECT yes  LEFT no", "CANCEL skip");
}

void demoKey(Ctx &c, uint8_t key) {
  if (c.step != STEP_ASK) {
    if (key == BTN_B) c.finish(State::Skip, "stopped");
    return;
  }
  const char *closed = sAnswer == 1 ? "approved" : "refused";
  if (key == BTN_A) c.finish(State::Ok, "looked right, %s", closed);
  else if (key == BTN_LEFT) c.finish(State::Fail, "looked wrong, %s", closed);
  else if (key == BTN_B) c.finish(State::Skip, "skipped");
}

const ManualOps DEMO = {demoStart, demoUpdate, demoDraw, demoKey, false};

const Check TABLE[] = {
    {"green", "GREEN", Kind::Manual, Profile::Dev, nullptr, &DEMO, 0},
    {"amber", "AMBER", Kind::Manual, Profile::Dev, nullptr, &DEMO, 0},
    {"red", "RED", Kind::Manual, Profile::Dev, nullptr, &DEMO, 0},
};

}  // namespace

const Suite APPROVALS = {
    "approvals", "APPROVAL SCREENS", Profile::Dev, TABLE, sizeof TABLE / sizeof TABLE[0],
    nullptr, nullptr, nullptr, false,
};

}  // namespace selftest

#else  // release: an empty suite the runner leaves out (its profile is Dev and it has no rows)

namespace selftest {
const Suite APPROVALS = {"approvals", "APPROVAL SCREENS", Profile::Dev, nullptr, 0, nullptr, nullptr, nullptr, false};
}

#endif  // VK_TEST_HOOKS
