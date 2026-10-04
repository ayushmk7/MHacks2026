// src/vk/vk.h
// Badge OS entry points, called from os.ino through hooks H2, H4 and H5
// (architecture/overview.md, sections 4, 5 and 7).
#pragma once

#include "vk_build.h"      // the build switches
#include "core/serial.h"   // vk::serial::handleLine (hook H6)
#include "host/router.h"   // vk::host::router::install (hook H3)

namespace vk {
void begin();         // hook H2: loads the config, then runs every service's begin. Never fails the boot.
void update();        // hook H5: runs every service's update, once per loop pass
bool modalActive();   // hook H4: true while the approval owns the screen and the buttons
void modalUpdate();   // hook H4: one pass of the approval engine
}  // namespace vk
