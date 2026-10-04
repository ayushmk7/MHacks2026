// src/vk/shell/setup_core.h
// The logic under the on-badge settings pages (docs/os/ui/shell.md, "Badge", "Setup", "Advanced"):
//   - the number stepper LEFT/RIGHT uses for a U32 config key, inside the key's own range;
//   - the setup checklist, computed from a snapshot of the badge's state: what is done, what is
//     left, what to do next and where SELECT goes; the first-boot rule.
// Pure C99: no Arduino, no drawing, no state. pages/page_setup.cpp fills the snapshot from the real
// badge and draws the rows; test/host/test_setup.c tests this file.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- the number stepper -------------------------------------------------------------------------
 * One step of `value` within lo..hi. A range of 20 or less steps by one. A wider one steps along a
 * ladder of round numbers (1 2 3 5 10 15 20 25 30 45 50 60 90 100 120 ... 3600 ... then 1-2-5 per
 * decade), with lo and hi themselves as stops: 0..3600 is crossed in about 35 presses, and 15 s,
 * 1 min, 5 min, 1 h are all on the way. A value off the ladder (set over USB) goes to its neighbour
 * on that side; a value outside the range comes back to the nearest end. direction 0: unchanged. */
uint32_t vk_step_u32(uint32_t value, uint32_t lo, uint32_t hi, int direction);

/* ---- the setup checklist ----------------------------------------------------------------------- */

#define VK_SETUP_MISSING_MAX 4     /* required config keys the snapshot names */
#define VK_SETUP_NEXT_LINES 3      /* lines of "what to do next" per row */
#define VK_SETUP_LINE_MAX 50       /* the longest line: TEXT_COLS of the shell */

typedef enum { VK_SETUP_CLOCK_NONE = 0, VK_SETUP_CLOCK_FLOOR = 1, VK_SETUP_CLOCK_SNTP = 2 } vk_setup_clock_t;

/* A snapshot of the badge, taken by the caller. Strings are borrowed for the call. */
typedef struct {
  bool identity;                            /* the badge has its key (identity::ready()) */
  unsigned wifi_saved;                      /* networks the badge knows */
  bool wifi_joined;                         /* station mode and connected */
  bool hotspot;                             /* running its own hotspot */
  vk_setup_clock_t clock;                   /* vk::clock::source() */
  bool provisioned;                         /* vk::config::provisioned() */
  unsigned missing_count;                   /* required config keys with no value */
  const char *missing[VK_SETUP_MISSING_MAX];/* their names, from the config registry */
  bool listener_set;                        /* config key listener_url is not empty */
  bool name_set;                            /* config key display_name is not empty */
  const char *name;                         /* that name, or NULL */
  const char *theme;                        /* the active theme, shown as it is ("light") */
} vk_setup_state_t;

/* The rows, in this order. */
typedef enum {
  VK_SETUP_IDENTITY,
  VK_SETUP_WIFI,
  VK_SETUP_CLOCK,
  VK_SETUP_WALLET,
  VK_SETUP_LISTENER,
  VK_SETUP_NAME,
  VK_SETUP_THEME,
  VK_SETUP_ITEMS
} vk_setup_item_t;

typedef enum {
  VK_SETUP_DONE,       /* nothing to do */
  VK_SETUP_TODO,       /* the person has to act */
  VK_SETUP_WAIT,       /* under way by itself (a saved network, the network time) */
  VK_SETUP_OPTIONAL    /* not needed to pay, not set */
} vk_setup_status_t;

/* Where SELECT on the row goes. */
typedef enum {
  VK_SETUP_GO_NONE,    /* nowhere: the text says what to do */
  VK_SETUP_GO_WIFI,    /* the Wi-Fi page */
  VK_SETUP_GO_NAME,    /* the keyboard for display_name */
  VK_SETUP_GO_TIME,    /* the Badge page (time zone) */
  VK_SETUP_GO_THEME,   /* the next theme, in place */
  VK_SETUP_GO_LAPTOP   /* nowhere on the badge: the text gives the laptop step */
} vk_setup_go_t;

typedef struct {
  vk_setup_item_t item;
  const char *label;                        /* "Wi-Fi" */
  const char *value;                        /* "joined", "none", the name... */
  vk_setup_status_t status;
  bool required;                            /* counted by vk_setup_left */
  vk_setup_go_t go;
  const char *next[VK_SETUP_NEXT_LINES];    /* what to do next; unused lines are NULL */
  char detail[VK_SETUP_LINE_MAX + 1];       /* one more line computed from the state, or "" */
} vk_setup_row_t;

/* Fills out[0 .. VK_SETUP_ITEMS-1], one row per item in enum order. */
void vk_setup_rows(const vk_setup_state_t *s, vk_setup_row_t out[VK_SETUP_ITEMS]);
/* Required rows that are not DONE: identity, Wi-Fi joined, clock synced by the network, wallet
 * provisioned. 0 means set up. */
unsigned vk_setup_left(const vk_setup_state_t *s);
/* The first-boot rule: open the setup screen by itself at boot only when it was never dismissed
 * (config key setup_done is 0), the badge is not provisioned (a provisioned badge was set up from a
 * laptop and is not new) and something required is left. */
bool vk_setup_autoopen(const vk_setup_state_t *s, uint32_t setup_done);
/* "missing: issuer_key, rpc_url, tokens", cut with ".." to fit; "" when nothing is missing. */
void vk_setup_missing_text(const vk_setup_state_t *s, char *out, size_t cap);

#ifdef __cplusplus
}
#endif
