// Dev tools (testing.md, "Dev hooks"). Dev profile only: everything behind this header is compiled
// only when VK_TEST_HOOKS is 1, and upstream includes it only then (hook H17).
#pragma once

#include <stdint.h>

// Called by buttons::update() on every pass (hook H17) with upstream's three key masks. Applies the
// presses and releases queued by the VKBTN serial command.
extern "C" void vk_dev_apply_injected_buttons(uint8_t *down, uint8_t *pressed, uint8_t *released);
