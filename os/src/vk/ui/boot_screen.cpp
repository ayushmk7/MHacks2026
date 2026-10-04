// The Receipt boot screen (docs/os/ui/shell.md, "Boot"). boot::progress() in src/ui/boot.cpp calls
// bootScreen() as each stage of setup() begins; boot stages block, so the screen changes once per
// stage. It runs before vk::begin(): it uses only theme::color (which resolves the theme on first
// use), the receipt kit and the shell's drawing helpers, which need nothing started.
#include <stdio.h>
#include <string.h>

#include "../../hal/display.h"
#include "../shell/page.h"
#include "leds.h"
#include "receipt.h"

namespace vk::ui {

namespace {

// The seven stages of setup() and the percent each one begins at (os.ino; 75 is hook H2).
struct Stage { const char *name; uint8_t percent; };
const Stage STAGES[] = {
    {"STORAGE", 20}, {"PERIPHERALS", 45}, {"IDENTITY", 55}, {"RUNTIME", 65},
    {"WALLET", 75},  {"RADIOS", 85},      {"READY", 100},
};

constexpr int STUB_CX = 73;                  // centre of the stub, left of the perforation
constexpr int BODY_CX = 233;                 // centre of the body column
constexpr int BRAND_Y = 36;                  // top of the brand line's capitals
constexpr int BRAND_CAP_H = 16;              // capital height of FreeSerifBoldItalic12pt7b
constexpr int DETAIL_COLS = 22;

}  // namespace

bool bootScreen(const char *step, const char *detail, uint8_t percent) {
  (void)step;                                // the checklist shows the stage; the name is not printed twice
  if (percent > 100) percent = 100;
  LGFX_Sprite &c = display::canvas();

  receipt::page();
  receipt::header("BADGEOS", "*** STARTING UP ***");
  receipt::perforation(146, 24, 232);

  // Brand line. The kit leaves the canvas on Font0, size 1, top-left: put that back afterwards.
  c.setFont(&fonts::FreeSerifBoldItalic12pt7b);
  c.setTextSize(1);
  c.setTextColor(theme::color(theme::INK));
  c.setTextDatum(textdatum_t::baseline_center);
  c.drawString("BadgeOS", STUB_CX, BRAND_Y + BRAND_CAP_H - 1);
  c.setFont(&fonts::Font0);
  c.setTextSize(1);
  c.setTextDatum(textdatum_t::top_left);

  char value[8];
  snprintf(value, sizeof value, "%u%%", (unsigned)percent);
  receipt::amount(STUB_CX, 62, "", value, "");
  vk::shell::bar(18, 132, 14, percent, 100);

  if (detail != nullptr && detail[0] != '\0') {
    const int length = (int)strlen(detail);
    const int cols = length > DETAIL_COLS ? DETAIL_COLS : length;
    vk::shell::text(STUB_CX - (cols * 6 - 1) / 2, 150, detail, theme::INK, DETAIL_COLS);
  }

  receipt::title("CHECKLIST", 28, BODY_CX);
  for (size_t i = 0; i < sizeof STAGES / sizeof STAGES[0]; ++i) {
    // OK when the stage is behind the current percent, ".." when it is the one running, blank when
    // it is still to come. At 100 every row is OK; at 0 (the frame boot::run() draws) every row is blank.
    const char *mark = "";
    if (percent >= 100 || STAGES[i].percent < percent) mark = "OK";
    else if (STAGES[i].percent == percent) mark = "..";
    receipt::row(157, 310, 50 + 18 * (int)i, STAGES[i].name, mark);
  }

  display::touch();
  display::flush();
  leds::bootProgress(percent);               // one frame of the LED boot bar
  return true;
}

}  // namespace vk::ui
