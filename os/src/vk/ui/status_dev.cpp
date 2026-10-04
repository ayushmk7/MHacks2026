// Status item `dev` (ui.md, "Status bar"): DEV in amber in upstream's bar, in the dev profile only.
// The release profile compiles this file to nothing, so a judge badge has no such item.
#include "statusbar.h"

#include "../vk_build.h"

#if VK_PROFILE_DEV

#ifndef VK_HOST_TEST
#include "../../hal/display.h"
#include "../../ui/theme.h"      // upstream's palette: the bar is upstream's
#endif

namespace vk::ui::statusbar {

namespace {

int drawDev(int rightX, int y) {
#ifndef VK_HOST_TEST
  static const char LABEL[] = "DEV";
  ::display::textRight(LABEL, rightX, y, ::theme::WARN, 1);       // upstream's amber, #FFB020
  return (int)::display::canvas().textWidth(LABEL);
#else
  (void)rightX;
  (void)y;
  return 18;                                         // three characters of the 6-pixel font
#endif
}

VK_STATUS_ITEM(dev, "dev", 20, drawDev);

}  // namespace

}  // namespace vk::ui::statusbar

#endif  // VK_PROFILE_DEV
