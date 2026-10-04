// TEMPORARY (integrator I3): removed before the commit.
//   VKHDR             draws a page with the receipt header and replies with statusRight's text
//   VKHISTFILL <n>    appends n synthetic history records; replies with each append's milliseconds
//   VKHISTRM          deletes /vk/history.bin
#include "../../vk_build.h"

#if VK_TEST_HOOKS

#include <Arduino.h>

#include "../../../hal/display.h"
#include "../../../hal/power.h"
#include "../../core/fileio.h"
#include "../../core/serial.h"
#include "../../ui/receipt.h"
#include "../history/history.h"

namespace {

void cmdHdr(const String &, const vk::serial::Reply &reply) {
  char right[40];
  vk::ui::receipt::statusRight(right, sizeof right);
  vk::ui::receipt::page();
  vk::ui::receipt::header("BADGE OS", right);
  vk::ui::receipt::title("HEADER CHECK", 40);
  vk::ui::receipt::footer("SELECT open", "CANCEL back");
  char volts[24];
  snprintf(volts, sizeof volts, "%.3f", (double)power::volts());
  reply(String("OK ") + right + " | volts=" + volts + " charging=" + (power::charging() ? "1" : "0"));
}

void cmdHistFill(const String &args, const vk::serial::Reply &reply) {
  const int n = args.toInt();
  if (n <= 0 || n > 300) { reply("ERR usage"); return; }
  String line;
  for (int i = 0; i < n; ++i) {
    vk::history::Entry entry{};
    entry.time = 1790000000u + (uint32_t)i;
    strlcpy(entry.domain, "confirm", sizeof entry.domain);
    entry.outcome = vk::history::OUTCOME_APPROVED;
    strlcpy(entry.app_id, "i3fill", sizeof entry.app_id);
    const uint32_t startedAt = millis();
    const bool ok = vk::history::append(entry);
    const uint32_t ms = millis() - startedAt;
    line += String(ok ? "" : "!") + String(ms) + " ";
    if ((i + 1) % 16 == 0 || i + 1 == n) {
      reply(String("+ count=") + String((unsigned)vk::history::count()) + " ms: " + line);
      line = "";
    }
  }
  reply("OK");
}

void cmdHistRm(const String &, const vk::serial::Reply &reply) {
  reply(vk::fileio::ops->removeFile(vk::history::FILE_PATH) ? "OK" : "ERR remove");
}

}  // namespace

VK_SERIAL_COMMAND(tmp_vkhdr, "VKHDR", cmdHdr, "TEMP header check");
VK_SERIAL_COMMAND(tmp_vkhistfill, "VKHISTFILL", cmdHistFill, "TEMP history fill");
VK_SERIAL_COMMAND(tmp_vkhistrm, "VKHISTRM", cmdHistRm, "TEMP history remove");

#endif
