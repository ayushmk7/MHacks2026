// Status-bar items (hook H14) and the shell repaint flag (hook H20).
#include "statusbar.h"

#ifndef VK_HOST_TEST
#include "../../hal/display.h"
#endif

namespace vk::ui::statusbar {

namespace {
constexpr int ITEM_Y = 7;          // upstream draws the bar's text at y = 7
constexpr int ITEM_GAP = 8;        // pixels between items
constexpr int LEFT_LIMIT = 110;    // nothing is drawn left of this, so the title is never covered
constexpr size_t MAX_ITEMS = 16;
}  // namespace

void draw(int rightEdgeX) {
  // Registration order is undefined: collect, then sort by `order` (lower = further right).
  StatusItem *items[MAX_ITEMS];
  size_t n = 0;
  for (StatusItem *it = StatusItem::first(); it && n < MAX_ITEMS; it = it->next()) {
    if (!it->draw) continue;
    size_t i = n++;
    while (i > 0 && items[i - 1]->order > it->order) {
      items[i] = items[i - 1];
      --i;
    }
    items[i] = it;
  }
  if (n == 0 || rightEdgeX <= LEFT_LIMIT) return;

#ifndef VK_HOST_TEST
  // An item cannot know how much room is left, so clip: whatever it draws stays right of the title.
  auto &canvas = display::canvas();
  int32_t cx, cy, cw, ch;
  canvas.getClipRect(&cx, &cy, &cw, &ch);
  canvas.setClipRect(LEFT_LIMIT, 0, rightEdgeX - LEFT_LIMIT, display::STATUS_BAR_HEIGHT);
#endif

  int x = rightEdgeX;
  for (size_t i = 0; i < n && x > LEFT_LIMIT; ++i) {
    const int used = items[i]->draw(x, ITEM_Y);
    if (used > 0) x -= used + ITEM_GAP;
  }

#ifndef VK_HOST_TEST
  canvas.setClipRect(cx, cy, cw, ch);
#endif
}

}  // namespace vk::ui::statusbar

namespace vk::ui {

namespace {
bool sShellRepaint = false;
}

void requestShellRepaint() { sShellRepaint = true; }

bool consumeShellRepaint() {
  const bool wanted = sShellRepaint;
  sShellRepaint = false;
  return wanted;
}

}  // namespace vk::ui
