// src/vk/shell/setup_core.c -- see setup_core.h. Pure C99.
#include "setup_core.h"

#include <stdio.h>
#include <string.h>

// ---- the number stepper -------------------------------------------------------------------------

#define SMALL_RANGE 20u

// Round numbers up to 10000, chosen so that seconds (15 s, 1 min, 5 min, 1 h) and milliseconds
// (250, 500, 1000) are both on the way. Above, 1-2-5 per decade.
static const uint32_t LADDER[] = {1,    2,    3,    5,    10,   15,   20,   25,   30,   45,   50,   60,
                                  90,   100,  120,  150,  180,  200,  250,  300,  400,  500,  600,  750,
                                  900,  1000, 1200, 1500, 1800, 2000, 2500, 3000, 3600, 4000, 5000, 6000,
                                  7200, 7500, 10000};
#define LADDER_COUNT (sizeof LADDER / sizeof LADDER[0])

// The ladder value at position i (0, 1, ...), continuing as 1-2-5 per decade; 0 once past 2^32.
static uint32_t rung(size_t i) {
  if (i < LADDER_COUNT) return LADDER[i];
  size_t k = i - LADDER_COUNT;       // 20000, 50000, 100000, 200000, 500000, ...
  uint64_t decade = 10000;
  static const uint32_t mult[3] = {2, 5, 10};
  for (size_t d = 0; d < k / 3; ++d) decade *= 10;
  const uint64_t v = decade * mult[k % 3];
  return v > 0xFFFFFFFFull ? 0 : (uint32_t)v;
}

uint32_t vk_step_u32(uint32_t value, uint32_t lo, uint32_t hi, int direction) {
  if (lo > hi) return value;
  if (value > hi) return hi;
  if (value < lo) return lo;
  if (direction == 0) return value;
  if (hi - lo <= SMALL_RANGE) {
    if (direction > 0) return value < hi ? value + 1 : hi;
    return value > lo ? value - 1 : lo;
  }
  if (direction > 0) {
    // The smallest rung above value and above lo; else hi.
    for (size_t i = 0;; ++i) {
      const uint32_t r = rung(i);
      if (r == 0 || r >= hi) return hi;
      if (r > value) return r;
    }
  }
  // The largest rung below value and below hi; else lo.
  uint32_t best = lo;
  for (size_t i = 0;; ++i) {
    const uint32_t r = rung(i);
    if (r == 0 || r >= value) break;
    if (r > lo) best = r;
  }
  return best;
}

// ---- the setup checklist -------------------------------------------------------------------------

static void lines(vk_setup_row_t *row, const char *a, const char *b, const char *c) {
  row->next[0] = a;
  row->next[1] = b;
  row->next[2] = c;
}

static void identityRow(const vk_setup_state_t *s, vk_setup_row_t *row) {
  row->label = "Identity";
  row->required = true;
  row->go = VK_SETUP_GO_NONE;   // never towards New identity: it would wipe the wallet key
  if (s->identity) {
    row->value = "ready";
    row->status = VK_SETUP_DONE;
    lines(row, "The badge has its own key.", NULL, NULL);
  } else {
    row->value = "no key";
    row->status = VK_SETUP_TODO;
    lines(row, "The badge has no key: payments cannot be signed.", "Restart it (Settings > Restart). If it stays,",
          "Settings > Console says why.");
  }
}

static void wifiRow(const vk_setup_state_t *s, vk_setup_row_t *row) {
  row->label = "Wi-Fi";
  row->required = true;
  row->go = VK_SETUP_GO_WIFI;
  if (s->wifi_joined) {
    row->value = "joined";
    row->status = VK_SETUP_DONE;
    lines(row, "On a network. SELECT to change it.", NULL, NULL);
  } else if (s->hotspot) {
    row->value = "hotspot";
    row->status = VK_SETUP_TODO;
    lines(row, "The badge runs its own hotspot. Join a network", "for the time, the balance and payments:",
          "SELECT, scan, pick it, type its password.");
  } else if (s->wifi_saved > 0) {
    row->value = "looking";
    row->status = VK_SETUP_WAIT;
    lines(row, "A saved network has not answered yet.", "SELECT to scan and join another one.", NULL);
  } else {
    row->value = "none";
    row->status = VK_SETUP_TODO;
    lines(row, "Needed for the time, the balance and payments.", "SELECT, scan, pick a network, type its",
          "password with the on-screen keyboard.");
  }
}

static void clockRow(const vk_setup_state_t *s, vk_setup_row_t *row) {
  row->label = "Clock";
  row->required = true;
  if (s->clock == VK_SETUP_CLOCK_SNTP) {
    row->value = "synced";
    row->status = VK_SETUP_DONE;
    row->go = VK_SETUP_GO_TIME;
    lines(row, "The network gave the time. SELECT to set the", "time zone the header shows.", NULL);
    return;
  }
  // A FLOOR clock (raised by a verified record) is not synced: the wallet still says CLOCK UNSYNCED.
  row->value = s->clock == VK_SETUP_CLOCK_FLOOR ? "unsynced" : "not set";
  if (s->wifi_joined) {
    row->status = VK_SETUP_WAIT;
    row->go = VK_SETUP_GO_TIME;
    lines(row, "Waiting for the network time (up to a minute).", "A network with no internet cannot give it.",
          "Payments show CLOCK UNSYNCED until then.");
  } else {
    row->status = VK_SETUP_TODO;
    row->go = VK_SETUP_GO_WIFI;
    lines(row, "The time comes from the network: join Wi-Fi.", "Payments show CLOCK UNSYNCED until then.",
          "It is never typed in: payments check it.");
  }
}

static void walletRow(const vk_setup_state_t *s, vk_setup_row_t *row) {
  row->label = "Wallet";
  row->required = true;
  if (s->provisioned) {
    row->value = "provisioned";
    row->status = VK_SETUP_DONE;
    row->go = VK_SETUP_GO_NONE;
    lines(row, "The payment settings are on the badge.", "They change only from a laptop.", NULL);
    return;
  }
  // The root of trust comes from the laptop tool, never from the buttons. Two lines and the detail.
  row->value = "needs USB";
  row->status = VK_SETUP_TODO;
  row->go = VK_SETUP_GO_LAPTOP;
  lines(row, "From a laptop: connect USB, and in the repo run", "python3 os/scripts/vkdev.py provision", NULL);
  if (s->missing_count > 0) {
    vk_setup_missing_text(s, row->detail, sizeof row->detail);
  } else {
    snprintf(row->detail, sizeof row->detail, "all values set: finish with VKCOMMIT");
  }
}

static void listenerRow(const vk_setup_state_t *s, vk_setup_row_t *row) {
  row->label = "Listener";
  row->required = false;
  if (s->listener_set) {
    row->value = "set";
    row->status = VK_SETUP_DONE;
    row->go = VK_SETUP_GO_NONE;
    lines(row, "The venue's backend address is set.", NULL, NULL);
  } else {
    row->value = "not set";
    row->status = VK_SETUP_OPTIONAL;
    row->go = VK_SETUP_GO_LAPTOP;
    lines(row, "The venue's backend. Without it every payee", "shows UNVERIFIED RECIPIENT. It comes with the",
          "provision command (--listener), or Advanced.");
  }
}

static void nameRow(const vk_setup_state_t *s, vk_setup_row_t *row) {
  row->label = "Name";
  row->required = false;
  row->go = VK_SETUP_GO_NAME;
  if (s->name_set && s->name != NULL && s->name[0] != '\0') {
    row->value = s->name;
    row->status = VK_SETUP_DONE;
    lines(row, "Others see this name in requests and contacts.", "SELECT to change it.", NULL);
  } else {
    row->value = "device name";
    row->status = VK_SETUP_OPTIONAL;
    lines(row, "Others see the device name in requests and", "contacts. SELECT to type your own.", NULL);
  }
}

static void themeRow(const vk_setup_state_t *s, vk_setup_row_t *row) {
  row->label = "Theme";
  row->required = false;
  row->go = VK_SETUP_GO_THEME;
  row->value = s->theme != NULL ? s->theme : "";
  row->status = VK_SETUP_OPTIONAL;
  lines(row, "SELECT switches between light and dark.", NULL, NULL);
}

void vk_setup_rows(const vk_setup_state_t *s, vk_setup_row_t out[VK_SETUP_ITEMS]) {
  for (int i = 0; i < VK_SETUP_ITEMS; ++i) {
    memset(&out[i], 0, sizeof out[i]);
    out[i].item = (vk_setup_item_t)i;
    out[i].value = "";
  }
  if (s == NULL) return;
  identityRow(s, &out[VK_SETUP_IDENTITY]);
  wifiRow(s, &out[VK_SETUP_WIFI]);
  clockRow(s, &out[VK_SETUP_CLOCK]);
  walletRow(s, &out[VK_SETUP_WALLET]);
  listenerRow(s, &out[VK_SETUP_LISTENER]);
  nameRow(s, &out[VK_SETUP_NAME]);
  themeRow(s, &out[VK_SETUP_THEME]);
}

unsigned vk_setup_left(const vk_setup_state_t *s) {
  if (s == NULL) return 0;
  vk_setup_row_t rows[VK_SETUP_ITEMS];
  vk_setup_rows(s, rows);
  unsigned left = 0;
  for (int i = 0; i < VK_SETUP_ITEMS; ++i) {
    if (rows[i].required && rows[i].status != VK_SETUP_DONE) ++left;
  }
  return left;
}

bool vk_setup_autoopen(const vk_setup_state_t *s, uint32_t setup_done) {
  if (s == NULL || setup_done != 0 || s->provisioned) return false;
  return vk_setup_left(s) > 0;
}

void vk_setup_missing_text(const vk_setup_state_t *s, char *out, size_t cap) {
  if (out == NULL || cap == 0) return;
  out[0] = '\0';
  if (s == NULL || s->missing_count == 0) return;
  char full[256] = "missing: ";
  const unsigned count = s->missing_count < VK_SETUP_MISSING_MAX ? s->missing_count : VK_SETUP_MISSING_MAX;
  for (unsigned i = 0; i < count; ++i) {
    if (i > 0) strncat(full, ", ", sizeof full - strlen(full) - 1);
    strncat(full, s->missing[i] != NULL ? s->missing[i] : "?", sizeof full - strlen(full) - 1);
  }
  size_t limit = cap - 1 < VK_SETUP_LINE_MAX ? cap - 1 : VK_SETUP_LINE_MAX;
  const size_t length = strlen(full);
  if (length <= limit) {
    memcpy(out, full, length + 1);
    return;
  }
  if (limit < 2) {
    memcpy(out, full, limit);
    out[limit] = '\0';
    return;
  }
  memcpy(out, full, limit - 2);
  out[limit - 2] = '.';
  out[limit - 1] = '.';
  out[limit] = '\0';
}
