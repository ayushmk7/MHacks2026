// Status-bar items (ui.md, "Status bar") and the shell repaint request (hook H20).
#pragma once

#include <Arduino.h>

#include "../core/registry.h"

namespace vk::ui::statusbar {
struct StatusItem : Registered<StatusItem> {
  const char *name;
  int order;                           // lower = further right
  int (*draw)(int rightX, int y);      // draw right-aligned ending at rightX; return the width used, 0 for nothing
  StatusItem(const char *n, int o, int (*d)(int, int)) : name(n), order(o), draw(d) {}
};
#define VK_STATUS_ITEM(ident, name, order, draw_fn) static vk::ui::statusbar::StatusItem vk_status_##ident(name, order, draw_fn)
void draw(int rightEdgeX);             // hook H14
}

namespace vk::ui {
void requestShellRepaint();            // ask upstream's shell to redraw on its next pass
bool consumeShellRepaint();            // hook H20: true once per request
}
