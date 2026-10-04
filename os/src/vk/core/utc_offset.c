// src/vk/core/utc_offset.c -- see utc_offset.h. Pure C99.
#include "utc_offset.h"

#include <stdio.h>
#include <string.h>

static int digit(char c) { return (c >= '0' && c <= '9') ? c - '0' : -1; }

bool vk_utc_offset_parse(const char *text, int32_t *minutes) {
  if (text == NULL) return false;
  if (text[0] == '\0') {
    if (minutes != NULL) *minutes = 0;
    return true;
  }
  if (strlen(text) != 6) return false;
  if (text[0] != '+' && text[0] != '-') return false;
  if (text[3] != ':') return false;
  const int h1 = digit(text[1]), h2 = digit(text[2]), m1 = digit(text[4]), m2 = digit(text[5]);
  if (h1 < 0 || h2 < 0 || m1 < 0 || m2 < 0) return false;
  const int hours = h1 * 10 + h2, mins = m1 * 10 + m2;
  if (mins >= 60 || mins % VK_UTC_OFFSET_STEP != 0) return false;
  const int32_t value = (int32_t)((text[0] == '-' ? -1 : 1) * (hours * 60 + mins));
  if (value < VK_UTC_OFFSET_LOWEST || value > VK_UTC_OFFSET_HIGHEST) return false;
  if (minutes != NULL) *minutes = value;
  return true;
}

bool vk_utc_offset_valid(const char *text) { return vk_utc_offset_parse(text, NULL); }

void vk_utc_offset_format(int32_t minutes, char *out, size_t cap) {
  if (out == NULL || cap == 0) return;
  const int32_t a = minutes < 0 ? -minutes : minutes;
  snprintf(out, cap, "%c%02d:%02d", minutes < 0 ? '-' : '+', (int)(a / 60) % 100, (int)(a % 60));
}

void vk_utc_offset_label(int32_t minutes, char *out, size_t cap) {
  if (out == NULL || cap == 0) return;
  if (minutes == 0) {
    snprintf(out, cap, "UTC");
    return;
  }
  const int32_t a = minutes < 0 ? -minutes : minutes;
  if (a % 60 == 0) {
    snprintf(out, cap, "UTC%c%d", minutes < 0 ? '-' : '+', (int)(a / 60));
  } else {
    snprintf(out, cap, "UTC%c%d:%02d", minutes < 0 ? '-' : '+', (int)(a / 60), (int)(a % 60));
  }
}

int32_t vk_utc_offset_step(int32_t minutes, int direction) {
  if (minutes > VK_UTC_OFFSET_HIGHEST) return VK_UTC_OFFSET_HIGHEST;
  if (minutes < VK_UTC_OFFSET_LOWEST) return VK_UTC_OFFSET_LOWEST;
  if (direction == 0) return minutes;
  // The grid point at or below, measured from LOWEST so negative values round the same way.
  const int32_t below = VK_UTC_OFFSET_LOWEST +
                        ((minutes - VK_UTC_OFFSET_LOWEST) / VK_UTC_OFFSET_STEP) * VK_UTC_OFFSET_STEP;
  int32_t next;
  if (direction > 0) {
    next = below + VK_UTC_OFFSET_STEP;
  } else {
    next = below == minutes ? below - VK_UTC_OFFSET_STEP : below;
  }
  if (next > VK_UTC_OFFSET_HIGHEST) next = VK_UTC_OFFSET_HIGHEST;
  if (next < VK_UTC_OFFSET_LOWEST) next = VK_UTC_OFFSET_LOWEST;
  return next;
}

size_t vk_utc_offset_count(void) {
  return (size_t)((VK_UTC_OFFSET_HIGHEST - VK_UTC_OFFSET_LOWEST) / VK_UTC_OFFSET_STEP + 1);
}

int32_t vk_utc_offset_at(size_t index) {
  if (index >= vk_utc_offset_count()) return 0;
  return VK_UTC_OFFSET_LOWEST + (int32_t)index * VK_UTC_OFFSET_STEP;
}

size_t vk_utc_offset_index(int32_t minutes) {
  if (minutes <= VK_UTC_OFFSET_LOWEST) return 0;
  if (minutes >= VK_UTC_OFFSET_HIGHEST) return vk_utc_offset_count() - 1;
  return (size_t)((minutes - VK_UTC_OFFSET_LOWEST) / VK_UTC_OFFSET_STEP);
}

uint32_t vk_utc_offset_apply(uint32_t unix_s, int32_t minutes) {
  const int64_t moved = (int64_t)unix_s + (int64_t)minutes * 60;
  if (moved < 0) return 0;
  if (moved > (int64_t)0xFFFFFFFFu) return 0xFFFFFFFFu;
  return (uint32_t)moved;
}

void vk_utc_offset_clock(uint32_t unix_s, int32_t minutes, char *out, size_t cap) {
  if (out == NULL || cap == 0) return;
  const uint32_t t = vk_utc_offset_apply(unix_s, minutes);
  snprintf(out, cap, "%02u:%02u", (unsigned)((t / 3600) % 24), (unsigned)((t / 60) % 60));
}
