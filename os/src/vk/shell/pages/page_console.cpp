// Settings page `console` (shell.md, "Console"): the log ring, newest at the bottom. No title, to
// fit more lines; the background is paper.

#include "../page.h"

#include "../../../badge_log.h"

namespace {
using namespace vk::shell;
namespace receipt = vk::ui::receipt;
namespace th = vk::ui::theme;

constexpr int VISIBLE = 17;      // lines at y = 24 + 11 * i
constexpr int LINE_PITCH = 11;
constexpr int LINE_COLS = 51;    // from x = 6

int sScroll = 0;   // lines scrolled back from the newest

int maxScroll() {
  const int total = (int)badge_log::lineCount();
  return total > VISIBLE ? total - VISIBLE : 0;
}

void pageEnter() { sScroll = 0; }

void pageUpdate() {
  if (back()) return;  // CANCEL
  const int limit = maxScroll();
  if (buttons::repeated(BTN_UP) && sScroll < limit) {
    ++sScroll;
    repaint();
  }
  if (buttons::repeated(BTN_DOWN) && sScroll > 0) {
    --sScroll;
    repaint();
  }
}

void pageDraw() {
  receipt::page();
  char status[32];
  receipt::statusRight(status, sizeof status);
  receipt::header("BADGEOS", status);
  receipt::footer("UP/DOWN scroll", "CANCEL back");

  const int total = (int)badge_log::lineCount();
  const int limit = total > VISIBLE ? total - VISIBLE : 0;
  if (sScroll > limit) sScroll = limit;
  const int first = total > VISIBLE ? total - VISIBLE - sScroll : 0;

  for (int i = 0; i < VISIBLE; ++i) {
    const int index = first + i;
    if (index < 0 || index >= total) continue;
    const char *line = badge_log::line((size_t)index);
    if (line == nullptr) continue;
    // Errors in the blocked ink, so a failure is found without reading every line.
    const bool bad = strstr(line, "error") || strstr(line, "failed") || strstr(line, "FATAL");
    text(6, 24 + LINE_PITCH * i, line, bad ? th::STAMP_BAD : th::INK, LINE_COLS);
  }
}
}  // namespace

VK_SETTINGS_PAGE(console, "console", 130, "Console", nullptr, pageEnter, pageUpdate, pageDraw, 250);
