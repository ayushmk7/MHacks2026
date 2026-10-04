// LINK: src/vk/ui/power_core.c
// Host test of the screen dim/sleep state machine and the battery thresholds (src/vk/ui/power_core.h;
// docs/os/ui/ui.md, "Screen dim and sleep" and "Low battery"). Run: test/host/run.sh test_power
//
// What is checked:
//   1. the timers: awake, then dim after dim_s, then asleep after sleep_s; 0 turns each off; a dim
//      time that is not below the sleep time skips the dim; the state only goes deeper by itself;
//   2. the wake key: a press while dim or asleep wakes the screen and is swallowed, with its release
//      and any re-press while it is still held; other keys pressed later are delivered; a press
//      while awake is delivered and restarts the timer; a key held down keeps the screen awake;
//   3. an approval: a press while it is open wakes without swallowing (the approval lit the screen);
//   4. hold (approval open, app keep-awake, USB): the timer restarts and the screen is awake;
//   5. the dim level: a share of the awake level, never 0 from a lit screen;
//   6. the battery: thresholds, hysteresis on the way back up, no level without a cell reading,
//      0 turns a threshold off;
//   7. millis() wrapping round.
#include <stdio.h>
#include <string.h>

#include "../../src/vk/ui/power_core.h"

static int failures = 0;

#define CHECK(cond, ...)                              \
  do {                                                \
    if (!(cond)) {                                    \
      ++failures;                                     \
      printf("FAIL %s:%d: ", __FILE__, __LINE__);     \
      printf(__VA_ARGS__);                            \
      printf("\n");                                   \
    }                                                 \
  } while (0)

enum { UP = 1, DOWN = 2, SEL = 16, CAN = 32 };

static void test_timers(void) {
  vk_idle_t s;
  vk_idle_init(&s, 1000);
  CHECK(s.state == VK_SCREEN_AWAKE, "starts awake");
  CHECK(vk_idle_step(&s, 1000 + 29999, 30, 120, 0) == VK_SCREEN_AWAKE, "awake before dim_s");
  CHECK(vk_idle_step(&s, 1000 + 30000, 30, 120, 0) == VK_SCREEN_DIM, "dim at dim_s");
  CHECK(vk_idle_step(&s, 1000 + 119999, 30, 120, 0) == VK_SCREEN_DIM, "still dim");
  CHECK(vk_idle_step(&s, 1000 + 120000, 30, 120, 0) == VK_SCREEN_SLEEP, "asleep at sleep_s");

  // Only deeper by itself: a longer timeout set while asleep does not wake the screen.
  CHECK(vk_idle_step(&s, 1000 + 130000, 600, 900, 0) == VK_SCREEN_SLEEP, "stays asleep");

  // 0 turns each off.
  vk_idle_init(&s, 0);
  CHECK(vk_idle_step(&s, 10000000, 0, 0, 0) == VK_SCREEN_AWAKE, "both off: never");
  CHECK(vk_idle_step(&s, 10000000, 30, 0, 0) == VK_SCREEN_DIM, "sleep off: dims anyway");
  vk_idle_init(&s, 0);
  CHECK(vk_idle_step(&s, 59999, 0, 60, 0) == VK_SCREEN_AWAKE, "dim off: awake before sleep");
  CHECK(vk_idle_step(&s, 60000, 0, 60, 0) == VK_SCREEN_SLEEP, "dim off: straight to sleep");

  // A dim time not below the sleep time: no dim phase.
  vk_idle_init(&s, 0);
  CHECK(vk_idle_step(&s, 30000, 60, 30, 0) == VK_SCREEN_SLEEP, "dim_s >= sleep_s: straight to sleep");
  vk_idle_init(&s, 0);
  CHECK(vk_idle_step(&s, 20000, 30, 30, 0) == VK_SCREEN_AWAKE, "dim_s == sleep_s: no dim before");
  CHECK(vk_idle_step(&s, 30000, 30, 30, 0) == VK_SCREEN_SLEEP, "dim_s == sleep_s: sleep");
}

static void test_wake_key(void) {
  vk_idle_t s;
  vk_idle_init(&s, 0);
  (void)vk_idle_step(&s, 200000, 30, 120, 0);
  CHECK(s.state == VK_SCREEN_SLEEP, "asleep");

  // SELECT goes down: the screen wakes, the press is gone.
  uint8_t pressed = SEL, released = 0;
  vk_idle_keys(&s, 200100, SEL, &pressed, &released, 0);
  CHECK(s.state == VK_SCREEN_AWAKE, "a press wakes");
  CHECK(pressed == 0, "the wake press is swallowed (pressed=%u)", pressed);
  CHECK(s.swallow == SEL, "SELECT is swallowed until it is up");
  CHECK(vk_idle_step(&s, 200100, 30, 120, 0) == VK_SCREEN_AWAKE, "the timer restarted");

  // Held: no edge. Another key goes down while SELECT is held: delivered.
  pressed = 0; released = 0;
  vk_idle_keys(&s, 200200, SEL, &pressed, &released, 0);
  pressed = UP; released = 0;
  vk_idle_keys(&s, 200300, SEL | UP, &pressed, &released, 0);
  CHECK(pressed == UP, "a second key, pressed after the wake, is delivered (pressed=%u)", pressed);

  // The expander re-reports SELECT as a new press while it is still held: still swallowed.
  pressed = SEL; released = 0;
  vk_idle_keys(&s, 200400, SEL | UP, &pressed, &released, 0);
  CHECK(pressed == 0, "a repeated edge of the held wake key is swallowed");

  // SELECT comes up: the release is swallowed too, and the key is free again.
  pressed = 0; released = SEL;
  vk_idle_keys(&s, 200500, UP, &pressed, &released, 0);
  CHECK(released == 0, "the wake key's release is swallowed");
  CHECK(s.swallow == 0, "nothing swallowed once the key is up (swallow=%u)", s.swallow);
  pressed = 0; released = UP;
  vk_idle_keys(&s, 200600, 0, &pressed, &released, 0);
  CHECK(released == UP, "the other key's release is delivered");

  // Next press, screen awake: delivered.
  pressed = SEL; released = 0;
  vk_idle_keys(&s, 200700, SEL, &pressed, &released, 0);
  CHECK(pressed == SEL, "a press while awake is delivered");
  pressed = 0; released = SEL;
  vk_idle_keys(&s, 200800, 0, &pressed, &released, 0);
  CHECK(released == SEL, "and its release");

  // Dim also swallows.
  (void)vk_idle_step(&s, 200800 + 30000, 30, 120, 0);
  CHECK(s.state == VK_SCREEN_DIM, "dim");
  pressed = CAN; released = 0;
  vk_idle_keys(&s, 231000, CAN, &pressed, &released, 0);
  CHECK(pressed == 0 && s.state == VK_SCREEN_AWAKE, "a press while dim wakes and is swallowed");
  pressed = 0; released = CAN;
  vk_idle_keys(&s, 231100, 0, &pressed, &released, 0);
  CHECK(released == 0 && s.swallow == 0, "its release too");

  // Two keys down on the same pass both belong to the wake.
  (void)vk_idle_step(&s, 231100 + 200000, 30, 120, 0);
  pressed = UP | DOWN; released = 0;
  vk_idle_keys(&s, 431200, UP | DOWN, &pressed, &released, 0);
  CHECK(pressed == 0 && s.swallow == (UP | DOWN), "both keys of the wake are swallowed");

  // A key held down keeps the screen awake.
  vk_idle_init(&s, 0);
  for (uint32_t t = 0; t <= 300000; t += 1000) {
    pressed = 0; released = 0;
    vk_idle_keys(&s, t, DOWN, &pressed, &released, 0);
    (void)vk_idle_step(&s, t, 30, 120, 0);
  }
  CHECK(s.state == VK_SCREEN_AWAKE, "a held key keeps the screen awake");
}

static void test_modal(void) {
  vk_idle_t s;
  vk_idle_init(&s, 0);
  (void)vk_idle_step(&s, 200000, 30, 120, 0);
  uint8_t pressed = SEL, released = 0;
  vk_idle_keys(&s, 200001, SEL, &pressed, &released, 1);
  CHECK(s.state == VK_SCREEN_AWAKE, "a press with an approval open wakes");
  CHECK(pressed == SEL && s.swallow == 0, "and is not swallowed: the approval lit the screen itself");
}

static void test_hold(void) {
  vk_idle_t s;
  vk_idle_init(&s, 0);
  (void)vk_idle_step(&s, 200000, 30, 120, 0);
  CHECK(vk_idle_step(&s, 200001, 30, 120, 1) == VK_SCREEN_AWAKE, "hold wakes");
  CHECK(vk_idle_step(&s, 400000, 30, 120, 1) == VK_SCREEN_AWAKE, "hold keeps awake");
  CHECK(vk_idle_step(&s, 400000 + 29999, 30, 120, 0) == VK_SCREEN_AWAKE, "the timer restarts at the end of the hold");
  CHECK(vk_idle_step(&s, 400000 + 30000, 30, 120, 0) == VK_SCREEN_DIM, "then dims");
  CHECK(vk_idle_activity(&s, 430001) == 1, "activity from dim reports a wake");
  CHECK(vk_idle_activity(&s, 430002) == 0, "activity while awake does not");
}

static void test_dim_level(void) {
  CHECK(vk_dim_level(190, 25) == 47, "25%% of 190 = %u", vk_dim_level(190, 25));
  CHECK(vk_dim_level(8, 5) == 1, "never 0 from a lit screen");
  CHECK(vk_dim_level(0, 25) == 0, "an unlit screen stays unlit");
  CHECK(vk_dim_level(200, 100) == 200, "100%%");
  CHECK(vk_dim_level(200, 250) == 200, "over 100%% is the awake level");
}

static void test_battery(void) {
  // low 20, critical 8, hysteresis 3.
  uint8_t l = VK_BATT_OK;
  l = vk_batt_level(l, 1, 50, 20, 8, 3);  CHECK(l == VK_BATT_OK, "50%%: ok");
  l = vk_batt_level(l, 1, 21, 20, 8, 3);  CHECK(l == VK_BATT_OK, "21%%: ok");
  l = vk_batt_level(l, 1, 20, 20, 8, 3);  CHECK(l == VK_BATT_LOW, "20%%: low");
  l = vk_batt_level(l, 1, 22, 20, 8, 3);  CHECK(l == VK_BATT_LOW, "22%%: still low (hysteresis)");
  l = vk_batt_level(l, 1, 23, 20, 8, 3);  CHECK(l == VK_BATT_LOW, "23%%: still low");
  l = vk_batt_level(l, 1, 24, 20, 8, 3);  CHECK(l == VK_BATT_OK, "24%%: ok again");
  l = vk_batt_level(l, 1, 8, 20, 8, 3);   CHECK(l == VK_BATT_CRITICAL, "8%%: critical (straight from ok)");
  l = vk_batt_level(l, 1, 11, 20, 8, 3);  CHECK(l == VK_BATT_CRITICAL, "11%%: still critical");
  l = vk_batt_level(l, 1, 12, 20, 8, 3);  CHECK(l == VK_BATT_LOW, "12%%: back to low, not ok");
  l = vk_batt_level(l, 1, 22, 20, 8, 3);  CHECK(l == VK_BATT_LOW, "22%%: low");
  l = vk_batt_level(l, 1, 30, 20, 8, 3);  CHECK(l == VK_BATT_OK, "30%%: ok");
  // No cell reading (external power): no level at all.
  l = vk_batt_level(VK_BATT_CRITICAL, 0, 3, 20, 8, 3);
  CHECK(l == VK_BATT_OK, "on USB the reading is the charger's: no warning");
  // 0 turns the critical level off; low still works.
  l = vk_batt_level(VK_BATT_OK, 1, 0, 20, 0, 3);
  CHECK(l == VK_BATT_LOW, "critical off: 0%% is low");
  // Hysteresis 0: leaves at once.
  l = vk_batt_level(VK_BATT_LOW, 1, 21, 20, 8, 0);
  CHECK(l == VK_BATT_OK, "no hysteresis");
  // A critical threshold above the low one: critical wins.
  l = vk_batt_level(VK_BATT_OK, 1, 15, 10, 20, 2);
  CHECK(l == VK_BATT_CRITICAL, "critical above low: critical");
}

static void test_wrap(void) {
  vk_idle_t s;
  const uint32_t start = 0xFFFFFFFFu - 10000u;
  vk_idle_init(&s, start);
  CHECK(vk_idle_step(&s, start + 20000u, 30, 120, 0) == VK_SCREEN_AWAKE, "20 s across the wrap: awake");
  CHECK(vk_idle_step(&s, start + 30000u, 30, 120, 0) == VK_SCREEN_DIM, "30 s across the wrap: dim");
}

int main(void) {
  test_timers();
  test_wake_key();
  test_modal();
  test_hold();
  test_dim_level();
  test_battery();
  test_wrap();
  if (failures) {
    printf("%d failure(s)\n", failures);
    return 1;
  }
  printf("all power tests passed\n");
  return 0;
}
