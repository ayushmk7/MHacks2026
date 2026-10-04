// The shell repaint flag: features set it, the shell (src/vk/shell/shell.cpp) consumes it.
#include "repaint.h"

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
