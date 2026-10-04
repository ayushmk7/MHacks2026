// Settings page `display` (shell.md, "Display"): the backlight level, applied live.

#include "../page.h"

#include "../../../settings.h"

namespace {
using namespace vk::shell;
namespace receipt = vk::ui::receipt;

constexpr int kStep = 8;
constexpr int kFloor = 8;   // a dark screen looks exactly like a crash

void pageValue(char *out, size_t cap) { snprintf(out, cap, "%d%%", (settings::brightness() * 100) / 255); }

void pageUpdate() {
  if (back()) return;  // CANCEL
  const int before = settings::brightness();
  int after = before;
  if (buttons::repeated(BTN_LEFT)) after -= kStep;
  if (buttons::repeated(BTN_RIGHT)) after += kStep;
  if (after == before) return;
  after = constrain(after, kFloor, 255);
  if (after == before) return;
  // Applied live, so the value being chosen is the value being seen.
  settings::setBrightness((uint8_t)after);
  display::setBrightness((uint8_t)after);
  repaint();
}

void pageDraw() {
  frame("DISPLAY", "LEFT/RIGHT adjust");
  char percent[8];
  snprintf(percent, sizeof percent, "%d%%", (settings::brightness() * 100) / 255);
  receipt::row(X0, X1, 48, "BACKLIGHT", percent);
  bar(41, 70, 30, settings::brightness(), 255);
}
}  // namespace

VK_SETTINGS_PAGE(display, "display", 80, "Display", pageValue, nullptr, pageUpdate, pageDraw, 0);
