// The shell repaint request (docs/os/ui/shell.md, "Framework").
#pragma once
namespace vk::ui {
void requestShellRepaint();   // ask the shell to redraw its top screen on its next pass
bool consumeShellRepaint();   // the shell: true once per request
}
