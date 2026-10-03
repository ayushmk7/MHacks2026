#include "sol.h"
#include <string.h>
static const char A58[] = "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz";

size_t sol_b58_encode(const uint8_t *in, size_t in_len, char *out, size_t out_cap) {
  uint8_t digits[90];                       /* enough for 64 input bytes (88 digits) */
  size_t used = 0, zeros = 0, i, j;
  if (in_len > 64) return 0;
  while (zeros < in_len && in[zeros] == 0) zeros++;
  for (i = zeros; i < in_len; i++) {
    uint32_t carry = in[i];
    for (j = 0; j < used; j++) {
      carry += (uint32_t)digits[j] << 8;
      digits[j] = (uint8_t)(carry % 58);
      carry /= 58;
    }
    while (carry) { digits[used++] = (uint8_t)(carry % 58); carry /= 58; }
  }
  if (zeros + used + 1 > out_cap) return 0;
  for (i = 0; i < zeros; i++) out[i] = '1';
  for (j = 0; j < used; j++) out[zeros + j] = A58[digits[used - 1 - j]];
  out[zeros + used] = '\0';
  return zeros + used;
}

int sol_b58_decode(const char *in, uint8_t *out, size_t out_len) {
  uint8_t bytes[64];                        /* little-endian big number */
  size_t used = 0, zeros = 0, i, j, n = strlen(in);
  if (out_len > 64 || n == 0 || n > 88) return -1;
  while (zeros < n && in[zeros] == '1') zeros++;
  for (i = zeros; i < n; i++) {
    const char *p = strchr(A58, in[i]);
    uint32_t carry;
    if (!p || !in[i]) return -1;
    carry = (uint32_t)(p - A58);
    for (j = 0; j < used; j++) {
      carry += (uint32_t)bytes[j] * 58;
      bytes[j] = (uint8_t)carry;
      carry >>= 8;
    }
    while (carry) { if (used >= sizeof bytes) return -1; bytes[used++] = (uint8_t)carry; carry >>= 8; }
  }
  if (zeros + used != out_len) return -1;
  memset(out, 0, zeros);
  for (j = 0; j < used; j++) out[zeros + j] = bytes[used - 1 - j];
  return 0;
}
