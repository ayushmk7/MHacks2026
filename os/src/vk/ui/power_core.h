/* power_core.h - the pure logic of the screen dim/sleep service and of the low-battery warning.
   C99, no Arduino: host-tested by test/host/test_power.c. Spec: docs/os/ui/ui.md, "Screen dim and
   sleep" and "Low battery". The services that drive it are screen_power.cpp and battery.cpp. */
#ifndef VK_POWER_CORE_H
#define VK_POWER_CORE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- screen: awake -> dim -> asleep --------------------------------------------------------- */

typedef enum { VK_SCREEN_AWAKE = 0, VK_SCREEN_DIM = 1, VK_SCREEN_SLEEP = 2 } vk_screen_state_t;

typedef struct {
  uint32_t last_activity_ms; /* the idle timer runs from here */
  uint8_t state;             /* vk_screen_state_t */
  uint8_t swallow;           /* keys whose press woke the screen: their edges are dropped until they are up */
} vk_idle_t;

void vk_idle_init(vk_idle_t *s, uint32_t now_ms);

/* Something the wearer will want to see happened: the timer restarts and the screen is awake.
   Returns 1 when the screen was dim or asleep. */
int vk_idle_activity(vk_idle_t *s, uint32_t now_ms);

/* The key edges of one pass, given before anything reads them. `down` is the keys held now. A press
   while the screen is dim or asleep wakes it and is removed, with every later edge of the keys that
   were down at that moment, until each of them is up. With an approval open (`modal`), which lit the
   screen itself, a press wakes and is delivered. A key held down counts as activity. */
void vk_idle_keys(vk_idle_t *s, uint32_t now_ms, uint8_t down, uint8_t *pressed, uint8_t *released, int modal);

/* One pass of the timers. dim_s, sleep_s: seconds of no activity before the screen dims or sleeps;
   0 = never. A dim time that is not below a non-zero sleep time means no dim phase. `hold`: something
   keeps the screen awake this pass (an approval, an app's keep-awake, external power if configured).
   By itself the state only gets deeper; only activity or hold wakes it. Returns the state. */
uint8_t vk_idle_step(vk_idle_t *s, uint32_t now_ms, uint32_t dim_s, uint32_t sleep_s, int hold);

/* The backlight level while dim: `pct` percent of the awake level (100 and above: the awake level).
   Never 0 when the awake level is not 0: a dark screen looks like a crash. */
uint8_t vk_dim_level(uint8_t awake, uint32_t pct);

/* ---- battery: ok -> low -> critical, with hysteresis ----------------------------------------- */

typedef enum { VK_BATT_OK = 0, VK_BATT_LOW = 1, VK_BATT_CRITICAL = 2 } vk_batt_level_t;

/* The level for a measured percentage. `measured` 0: there is no cell reading (on external power the
   ADC reads the charger), and the level is OK. A level is entered at or below its threshold and left
   only above threshold + hyst_pct. crit_pct 0 turns the critical level off; low_pct 0 the low one. */
uint8_t vk_batt_level(uint8_t prev, int measured, int percent, uint32_t low_pct, uint32_t crit_pct,
                      uint32_t hyst_pct);

#ifdef __cplusplus
}
#endif
#endif
