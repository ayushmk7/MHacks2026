// Settings row `theme` (docs/os/ui/shell.md, "Theme"): an action row. SELECT cycles the registered
// themes; the list stays on screen. Deleting this file removes the row and nothing else.
#include "../page.h"

namespace {
using namespace vk::shell;
namespace tk = vk::ui::theme;

// The active theme's name without its "receipt-" prefix ("light", "dark").
void themeValue(char *out, size_t cap) {
  const char *name = tk::activeName();
  if (name == nullptr) name = "";
  if (strncmp(name, "receipt-", 8) == 0) name += 8;
  snprintf(out, cap, "%s", name);
}

// The theme after the active one, wrapping. An active theme that is not in the registry (none
// resolved yet) is followed by the first one.
void themeNext() {
  const size_t count = tk::count();
  if (count == 0) return;
  const char *active = tk::activeName();
  size_t next = 0;
  for (size_t i = 0; i < count; ++i) {
    const tk::Theme *theme = tk::at(i);
    if (theme != nullptr && active != nullptr && strcmp(theme->name, active) == 0) {
      next = (i + 1) % count;
      break;
    }
  }
  const tk::Theme *theme = tk::at(next);
  if (theme != nullptr) tk::setActive(theme->name);
  repaint();
}
}  // namespace

VK_SETTINGS_ACTION(theme, "theme", 10, "Theme", themeValue, themeNext);
