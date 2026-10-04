/* vk_record.c - parser and signature check for the registry record. Spec: docs/os/wallet/checks.md. */
#include "vk_record.h"
#include <string.h>
#include "sol.h"

typedef struct { const uint8_t *p; size_t n, at; } cur_t;

/* Reads one "key=value" line at the cursor. The value runs to the next '\n', or to the end of the
   record when `last` is set (then no '\n' may follow at all). Returns 0 and the value's span. */
static int line(cur_t *c, const char *key, int last, const uint8_t **val, size_t *val_len) {
  const size_t key_len = strlen(key);
  size_t end;
  if (c->n - c->at < key_len + 1 || memcmp(c->p + c->at, key, key_len) != 0 || c->p[c->at + key_len] != '=') return -1;
  c->at += key_len + 1;
  end = c->at;
  while (end < c->n && c->p[end] != '\n') end++;
  if (last ? end != c->n : end == c->n) return -1;        /* the last line ends the record; the others end in '\n' */
  *val = c->p + c->at;
  *val_len = end - c->at;
  c->at = last ? end : end + 1;
  return 0;
}

static int is(const uint8_t *val, size_t val_len, const char *text) {
  return val_len == strlen(text) && memcmp(val, text, val_len) == 0;
}

/* 64 lower-case hex digits -> 32 bytes */
static int hex32(const uint8_t *val, size_t val_len, uint8_t out[32]) {
  size_t i; int k;
  if (val_len != 64) return -1;
  for (i = 0; i < 32; i++) {
    uint8_t b = 0;
    for (k = 0; k < 2; k++) {
      const uint8_t ch = val[2 * i + (size_t)k];
      if (ch >= '0' && ch <= '9') b = (uint8_t)(b << 4 | (ch - '0'));
      else if (ch >= 'a' && ch <= 'f') b = (uint8_t)(b << 4 | (ch - 'a' + 10));
      else return -1;
    }
    out[i] = b;
  }
  return 0;
}

/* 64 hex digits, or empty. *present says which. */
static int hex32_or_empty(const uint8_t *val, size_t val_len, uint8_t out[32], uint8_t *present) {
  if (val_len == 0) { *present = 0; memset(out, 0, 32); return 0; }
  *present = 1;
  return hex32(val, val_len, out);
}

/* unix seconds: 1..10 decimal digits, no sign, no leading zero, at most 4294967295 */
static int number(const uint8_t *val, size_t val_len, uint32_t *out) {
  uint64_t v = 0; size_t i;
  if (val_len < 1 || val_len > 10 || (val_len > 1 && val[0] == '0')) return -1;
  for (i = 0; i < val_len; i++) {
    if (val[i] < '0' || val[i] > '9') return -1;
    v = v * 10 + (uint64_t)(val[i] - '0');
  }
  if (v > 0xFFFFFFFFu) return -1;
  *out = (uint32_t)v;
  return 0;
}

int vk_record_parse(const uint8_t *bytes, size_t len, vk_record_t *out) {
  cur_t c; const uint8_t *v; size_t n, i; uint8_t has_wallet, has_ata, key[32];
  if (!bytes || !out || len == 0 || len > VK_RECORD_MAX) return -1;
  c.p = bytes; c.n = len; c.at = 0;
  memset(out, 0, sizeof *out);

  if (line(&c, "v", 0, &v, &n) || !is(v, n, "1")) return -1;

  if (line(&c, "attestation", 0, &v, &n)) return -1;
  if (n < 1 || n >= sizeof out->attestation) return -1;
  memcpy(out->attestation, v, n);                               /* NUL-terminated by the memset */
  if (!is(v, n, "none") && (memchr(v, 0, n) != NULL || sol_b58_decode(out->attestation, key, 32) != 0)) return -1;

  if (line(&c, "display_name", 0, &v, &n)) return -1;
  if (n < 1 || n >= sizeof out->display_name) return -1;
  for (i = 0; i < n; i++) if (v[i] < 0x20 || v[i] > 0x7E) return -1;
  memcpy(out->display_name, v, n);

  if (line(&c, "device_pubkey", 0, &v, &n) || hex32(v, n, out->device_pubkey)) return -1;

  if (line(&c, "kind", 0, &v, &n)) return -1;
  if (is(v, n, "merchant")) out->kind = VK_KIND_MERCHANT;
  else if (is(v, n, "person")) out->kind = VK_KIND_PERSON;
  else return -1;

  if (line(&c, "solana_wallet", 0, &v, &n) || hex32_or_empty(v, n, out->solana_wallet, &has_wallet)) return -1;
  if (line(&c, "solana_ata", 0, &v, &n) || hex32_or_empty(v, n, out->solana_ata, &has_ata)) return -1;
  out->has_solana = (uint8_t)(has_wallet && has_ata);
  if (!out->has_solana) { memset(out->solana_wallet, 0, 32); memset(out->solana_ata, 0, 32); }

  if (line(&c, "bank_ref_hash", 0, &v, &n) || hex32_or_empty(v, n, out->bank_ref_hash, &out->has_bank)) return -1;

  if (line(&c, "expiry", 0, &v, &n) || number(v, n, &out->expiry)) return -1;

  if (line(&c, "status", 0, &v, &n)) return -1;
  if (is(v, n, "active")) out->status = VK_STATUS_ACTIVE;
  else if (is(v, n, "revoked")) out->status = VK_STATUS_REVOKED;
  else return -1;

  if (line(&c, "issued_at", 1, &v, &n) || number(v, n, &out->issued_at)) return -1;
  return 0;
}

int vk_record_verify(const uint8_t *bytes, size_t len, const uint8_t sig[64], const uint8_t issuer_key[32],
                     int (*verify)(const uint8_t *, size_t, const uint8_t *, const uint8_t *)) {
  static const char PREFIX[] = VK_RECORD_PREFIX;
  uint8_t buf[sizeof PREFIX - 1 + VK_RECORD_MAX];
  if (!bytes || !sig || !issuer_key || !verify || len == 0 || len > VK_RECORD_MAX) return 0;
  memcpy(buf, PREFIX, sizeof PREFIX - 1);
  memcpy(buf + sizeof PREFIX - 1, bytes, len);
  return verify(buf, sizeof PREFIX - 1 + len, sig, issuer_key) == 1;
}
