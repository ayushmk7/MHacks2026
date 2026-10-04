// src/vk/vk.h
// BadgeOS entry points, called from os.ino through hooks H2, H4, H5 and H24
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
void flush();         // hook H24: display::flush(), counted. The one transfer of the canvas to the panel per pass

// What flush() has seen since boot. A canvas nobody marked changed is not sent, so `transfers`
// stands still while the screen is idle; a screen that is drawn but never sent would be black.
struct FlushStats {
  uint32_t transfers;   // display::flush() calls that sent the canvas
  uint32_t micros;      // time spent in those transfers
};
FlushStats flushStats();
}  // namespace vk
