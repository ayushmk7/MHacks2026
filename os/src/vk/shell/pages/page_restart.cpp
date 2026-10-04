// Settings page `restart` (shell.md, "Restart"): restarts the badge after a second press. It is the
// page itself that confirms: the row opens it, SELECT on it restarts, CANCEL goes back. It erases
// nothing: this is not Identity's "New identity" and not the wallet reset.

#include "../page.h"

#include "../../../badge_log.h"

namespace {
using namespace vk::shell;
namespace tk = vk::ui::theme;

// Long enough for the log line and the "Restarting" frame to leave before the chip resets.
constexpr uint32_t kLastWordsMs = 150;

void pageUpdate() {
  if (back()) return;  // CANCEL
  if (!buttons::pressed(BTN_A)) return;
  badge_log::tagf("os", "restart from Settings");
  frame("RESTART", "", "");
  textCentered(160, 112, "Restarting...");
  display::flush();
  delay(kLastWordsMs);
  ESP.restart();
}

void pageDraw() {
  frame("RESTART", "SELECT restart", "CANCEL back");
  text(X0, 48, "Restart the badge now?");
  text(X0, 70, "Kept: the key, wallet settings, apps and their", tk::SUB);
  text(X0, 84, "data, history, contacts, Wi-Fi.", tk::SUB);
  text(X0, 98, "Lost: waiting notifications, an open request.", tk::SUB);
}
}  // namespace

VK_SETTINGS_PAGE(restart, "restart", 150, "Restart", nullptr, nullptr, pageUpdate, pageDraw, 0);
