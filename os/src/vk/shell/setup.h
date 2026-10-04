// src/vk/shell/setup.h
// The setup checklist (docs/os/ui/shell.md, "Setup"), for the launcher and any other screen that
// wants to send a person to it. Implemented in pages/page_setup.cpp: deleting that file removes
// the page, so a caller that cannot do without these must be deleted with it.
#pragma once

#include <stddef.h>

namespace vk::shell::setup {

bool needed();                      // something required is not done: identity, Wi-Fi, clock, wallet
unsigned left();                    // how many of those four are left (0 = set up)
void summary(char *out, size_t cap);   // "3 steps left", "1 step left", or "done"
void open();                        // push screen `setup` on top of the current one

}  // namespace vk::shell::setup
