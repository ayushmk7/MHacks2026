// Screen dim and sleep (ui.md, "Screen dim and sleep"). After a while with no activity the backlight
// dims, later it goes off; any key wakes it, and the key that woke it does nothing else. Never while
// an approval is open, while an app keeps the screen awake, or (if configured) on external power.
//
// Included by upstream's src/hal/buttons.cpp (hook H25), so the part above the C++ section is plain C.
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Hook H25: buttons::update() calls this with the edges of the pass, before anything reads them and
// before the dev profile's injected buttons are added (injected buttons wake the screen through the
// service instead, and are delivered: device tests stay valid). Drops the edges of a key that woke
// the screen until that key is up.
void vk_screen_filter_buttons(uint8_t down, uint8_t *pressed, uint8_t *released);

#ifdef __cplusplus
}

namespace vk::ui::screen {

enum class State : uint8_t { AWAKE, DIM, SLEEP };   // the values of vk_screen_state_t (power_core.h)

State state();
const char *stateName();   // "awake", "dim", "sleep"

// For apps (native: call it; Lua: badge.screen.keep_awake(on)). While on, the screen neither dims nor
// sleeps: a game being played, a code being shown. Cleared when the app stops.
void keepAwake(bool on);
bool keepingAwake();

// Firmware that has something to show calls this: the idle timer restarts and the screen is lit.
void wake();

}  // namespace vk::ui::screen
#endif
