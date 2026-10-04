// Settings page `leds` (shell.md, "LEDs"): the RGB LED brightness, with a preview pulse.

#include "../page.h"

#include "../../../hal/leds.h"
#include "../../../settings.h"

namespace {
using namespace vk::shell;
namespace receipt = vk::ui::receipt;

constexpr int kStep = 8;

void pageValue(char *out, size_t cap) { snprintf(out, cap, "%d%%", (settings::ledBrightness() * 100) / 255); }

void pageUpdate() {
  // CANCEL stops the preview and turns the LEDs off before leaving, as upstream does.
  if (buttons::pressed(BTN_B)) {
    ::leds::stopAnimation();
    ::leds::off();
  }
  if (back()) return;

  const int before = settings::ledBrightness();
  int after = before;
  if (buttons::repeated(BTN_LEFT)) after -= kStep;
  if (buttons::repeated(BTN_RIGHT)) after += kStep;
  after = constrain(after, 0, 255);
  if (after != before) {
    settings::setLedBrightness((uint8_t)after);
    ::leds::setBrightness((uint8_t)after);
    repaint();
  }

  // Upstream previewed with its brand boot animation, which BadgeOS does not play.
  if (buttons::pressed(BTN_A)) pulseLed(700);
}

void pageDraw() {
  frame("LEDS", "LEFT/RIGHT adjust  SELECT preview");
  char percent[8];
  snprintf(percent, sizeof percent, "%d%%", (settings::ledBrightness() * 100) / 255);
  receipt::row(X0, X1, 48, "RGB BRIGHTNESS", percent);
  bar(41, 70, 30, settings::ledBrightness(), 255);
}
}  // namespace

VK_SETTINGS_PAGE(leds, "leds", 90, "LEDs", pageValue, nullptr, pageUpdate, pageDraw, 0);
