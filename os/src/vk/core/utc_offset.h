// src/vk/core/utc_offset.h
// The badge's UTC offset (config key `utc_offset`; docs/os/platform/config.md, wallet/checks.md
// "Clock"): how far the local time at the venue is from UTC. **For display only**: it moves the
// time the header and the settings pages print, never the clock the wallet checks read
// (vk::clock::now() stays UTC, and its source stays NONE, FLOOR or SNTP whatever this says).
//
// Text form: "" (UTC), or a sign, two digits of hours, ':' and two digits of minutes ("+05:30",
// "-04:00"), a whole quarter hour from -12:00 to +14:00. Pure C99: no Arduino, no state.
// Host-tested in test/host/test_setup.c.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define VK_UTC_OFFSET_LOWEST (-720)      /* -12:00, in minutes */
#define VK_UTC_OFFSET_HIGHEST 840        /* +14:00 */
#define VK_UTC_OFFSET_STEP 15            /* every offset in use is a whole quarter hour */
#define VK_UTC_OFFSET_TEXT_CAP 7         /* "+05:30" and its terminator */
#define VK_UTC_OFFSET_LABEL_CAP 10       /* "UTC+5:30" and its terminator */

/* True when `text` is a valid offset; *minutes (may be NULL) is written only then. "" is 0. */
bool vk_utc_offset_parse(const char *text, int32_t *minutes);
/* The rule of the config key: vk_utc_offset_parse without the output. */
bool vk_utc_offset_valid(const char *text);
/* The text form of `minutes` ("+05:30"; 0 is "+00:00"). Always terminated when cap > 0. */
void vk_utc_offset_format(int32_t minutes, char *out, size_t cap);
/* For people: "UTC", "UTC+5:30", "UTC-4". */
void vk_utc_offset_label(int32_t minutes, char *out, size_t cap);
/* One quarter hour up (direction > 0) or down (< 0), within LOWEST..HIGHEST. A value off the grid
 * goes to its neighbour on that side; a value out of range comes back to the nearest end. */
int32_t vk_utc_offset_step(int32_t minutes, int direction);

/* Every offset, ascending: for a picker. */
size_t vk_utc_offset_count(void);
int32_t vk_utc_offset_at(size_t index);          /* 0 past the end */
size_t vk_utc_offset_index(int32_t minutes);     /* the index of the nearest offset at or below */

/* unix_s moved by the offset, kept inside 0..UINT32_MAX: the "local unix time" a display prints. */
uint32_t vk_utc_offset_apply(uint32_t unix_s, int32_t minutes);
/* "HH:MM" of unix_s at the offset (cap >= 6). */
void vk_utc_offset_clock(uint32_t unix_s, int32_t minutes, char *out, size_t cap);

#ifdef __cplusplus
}
#endif
