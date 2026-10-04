// VKDEMOAPPROVE <green|amber|red>: raises a sample confirmation of that severity, so the approval
// screen, its rules and the LEDs can be tested before any signing domain exists (testing.md,
// "Dev hooks"). Dev profile only: in the release profile this file is empty.
#include "../../vk_build.h"

#if VK_TEST_HOOKS

#include <Arduino.h>

#include <string.h>

#include "../../../badge_log.h"
#include "../../core/serial.h"
#include "../../wallet/approval.h"  // last: it removes the Arduino core's DISABLED macro

namespace {

const char *const SEVERITY_NAMES[] = {"green", "amber", "red"};

// `arg` is the severity's name, so the log says which demo was answered.
void onDemoDone(bool approved, void *arg) {
  badge_log::tagf("vk", "demo approval %s: %s", (const char *)arg, approved ? "approved" : "refused");
}

void cmdDemoApprove(const String &args, const vk::serial::Reply &reply) {
  using namespace vk::wallet;

  String severity = args;
  severity.trim();
  severity.toLowerCase();

  ApprovalRequest request{};
  const char *name = nullptr;
  if (severity == "green") {
    name = SEVERITY_NAMES[0];
    strlcpy(request.headline, "VERIFIED - PRESENT", sizeof request.headline);
    request.severity = Severity::GREEN;
    request.select = SelectRule::PRESS;
  } else if (severity == "amber") {
    name = SEVERITY_NAMES[1];
    strlcpy(request.headline, "VERIFIED - NOT PRESENT", sizeof request.headline);
    request.severity = Severity::AMBER;
    request.select = SelectRule::HOLD;
  } else if (severity == "red") {
    name = SEVERITY_NAMES[2];
    strlcpy(request.headline, "UNVERIFIED RECIPIENT", sizeof request.headline);
    request.severity = Severity::RED;
    request.select = SelectRule::DISABLED;   // what the engine forces for RED in any case
    request.red_reason = VK_UNVERIFIED;
    request.dev_overridable = false;         // a closed red screen, in the dev profile too
  } else {
    reply("ERR usage");
    return;
  }

  strlcpy(request.title, "Pay", sizeof request.title);
  strlcpy(request.big, "10.00 HACK", sizeof request.big);
  strlcpy(request.sub, "to Demo Merchant", sizeof request.sub);
  strlcpy(request.lines[0].label, "Account", sizeof request.lines[0].label);
  strlcpy(request.lines[0].value, "2awX..6wrr", sizeof request.lines[0].value);
  strlcpy(request.lines[1].label, "Kind", sizeof request.lines[1].label);
  strlcpy(request.lines[1].value, "merchant", sizeof request.lines[1].value);
  request.line_count = 2;

  if (!approval::confirm(request, onDemoDone, (void *)name)) {
    reply("ERR busy");   // an approval is already open
    return;
  }
  reply("OK");
}

}  // namespace

VK_SERIAL_COMMAND(vkdemoapprove, "VKDEMOAPPROVE", cmdDemoApprove, "<green|amber|red> raises a sample approval");

#endif  // VK_TEST_HOOKS
