/* power_core.c - see power_core.h. */
#include "power_core.h"

void vk_idle_init(vk_idle_t *s, uint32_t now_ms) {
  s->last_activity_ms = now_ms;
  s->state = VK_SCREEN_AWAKE;
  s->swallow = 0;
}

int vk_idle_activity(vk_idle_t *s, uint32_t now_ms) {
  const int woke = s->state != VK_SCREEN_AWAKE;
  s->last_activity_ms = now_ms;
  s->state = VK_SCREEN_AWAKE;
  return woke;
}

void vk_idle_keys(vk_idle_t *s, uint32_t now_ms, uint8_t down, uint8_t *pressed, uint8_t *released, int modal) {
  if (*pressed != 0 && s->state != VK_SCREEN_AWAKE && !modal) {
    /* Every key down at the wake belongs to it, not only the one whose edge is on this pass. */
    s->swallow = (uint8_t)(s->swallow | *pressed | down);
  }
  if (*pressed != 0 || down != 0) (void)vk_idle_activity(s, now_ms);
  *pressed = (uint8_t)(*pressed & (uint8_t)~s->swallow);
  *released = (uint8_t)(*released & (uint8_t)~s->swallow);
  s->swallow = (uint8_t)(s->swallow & down); /* a key that is up is free again */
}

uint8_t vk_idle_step(vk_idle_t *s, uint32_t now_ms, uint32_t dim_s, uint32_t sleep_s, int hold) {
  if (hold) {
    (void)vk_idle_activity(s, now_ms);
    return s->state;
  }
  const uint64_t idle = (uint32_t)(now_ms - s->last_activity_ms);
  uint8_t want = VK_SCREEN_AWAKE;
  if (sleep_s != 0 && idle >= (uint64_t)sleep_s * 1000u) {
    want = VK_SCREEN_SLEEP;
  } else if (dim_s != 0 && (sleep_s == 0 || dim_s < sleep_s) && idle >= (uint64_t)dim_s * 1000u) {
    want = VK_SCREEN_DIM;
  }
  if (want > s->state) s->state = want;
  return s->state;
}

uint8_t vk_dim_level(uint8_t awake, uint32_t pct) {
  if (pct >= 100u) return awake;
  uint32_t level = ((uint32_t)awake * pct) / 100u;
  if (level == 0 && awake != 0) level = 1;
  return (uint8_t)level;
}

uint8_t vk_batt_level(uint8_t prev, int measured, int percent, uint32_t low_pct, uint32_t crit_pct,
                      uint32_t hyst_pct) {
  if (!measured) return VK_BATT_OK;
  const int64_t p = percent;
  uint8_t level = VK_BATT_OK;
  if (crit_pct != 0 && p <= (int64_t)crit_pct) {
    level = VK_BATT_CRITICAL;
  } else if (low_pct != 0 && p <= (int64_t)low_pct) {
    level = VK_BATT_LOW;
  }
  /* On the way back up a level is kept until the reading is hyst_pct above its threshold, so a cell
     hovering at a threshold (it sags under the radio's load) does not warn again and again. */
  if (prev == VK_BATT_CRITICAL && crit_pct != 0 && p <= (int64_t)crit_pct + (int64_t)hyst_pct) {
    level = VK_BATT_CRITICAL;
  }
  if (prev >= VK_BATT_LOW && level == VK_BATT_OK && low_pct != 0 && p <= (int64_t)low_pct + (int64_t)hyst_pct) {
    level = VK_BATT_LOW;
  }
  return level;
}
