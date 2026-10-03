#include "sol.h"
#include <string.h>

typedef struct { const uint8_t *p; size_t n, at; } cur_t;
static int take(cur_t *c, size_t k, const uint8_t **out) {
  if (c->n - c->at < k) return -1;
  *out = c->p + c->at; c->at += k; return 0;
}
static int byte(cur_t *c, uint8_t *b) { const uint8_t *p; if (take(c, 1, &p)) return -1; *b = *p; return 0; }

sol_tx_err_t sol_tx_decode_transfer(const uint8_t *msg, size_t len, sol_transfer_t *out) {
  cur_t c; const uint8_t *keys, *bh, *idx, *data; uint8_t b, nro_unsigned, prog, i;
  uint8_t src, mint, dst, auth;
  if (len > SOL_TX_MSG_MAX) return SOL_TX_ERR_TOO_LONG;
  if (len < 1) return SOL_TX_ERR_TRUNCATED;
  c.p = msg; c.n = len; c.at = 0;
  memset(out, 0, sizeof *out);
  out->version = 0xFF;
  if (msg[0] & 0x80) {                                  /* versioned message prefix */
    if ((msg[0] & 0x7F) != 0) return SOL_TX_ERR_VERSION;
    out->version = 0; c.at = 1;
  }
  /* header: num_required_signatures, num_readonly_signed, num_readonly_unsigned */
  if (byte(&c, &b)) return SOL_TX_ERR_TRUNCATED;
  if (b != 1) return SOL_TX_ERR_HEADER;
  if (byte(&c, &b)) return SOL_TX_ERR_TRUNCATED;
  if (b != 0) return SOL_TX_ERR_HEADER;
  if (byte(&c, &nro_unsigned)) return SOL_TX_ERR_TRUNCATED;
  /* account keys: compact-u16 count. Only the single-byte encoding of 5 is accepted. */
  if (byte(&c, &b)) return SOL_TX_ERR_TRUNCATED;
  if (b != 5 || nro_unsigned != 2) return SOL_TX_ERR_ACCOUNTS;
  if (take(&c, 5 * 32, &keys)) return SOL_TX_ERR_TRUNCATED;
  if (take(&c, 32, &bh)) return SOL_TX_ERR_TRUNCATED;
  /* instructions */
  if (byte(&c, &b)) return SOL_TX_ERR_TRUNCATED;
  if (b != 1) return SOL_TX_ERR_IX_COUNT;
  if (byte(&c, &prog)) return SOL_TX_ERR_TRUNCATED;
  if (prog >= 5 || memcmp(keys + 32 * prog, SOL_TOKEN_PROGRAM_ID, 32) != 0) return SOL_TX_ERR_PROGRAM;
  if (byte(&c, &b)) return SOL_TX_ERR_TRUNCATED;
  if (b != 4) return SOL_TX_ERR_IX_ACCOUNTS;
  if (take(&c, 4, &idx)) return SOL_TX_ERR_TRUNCATED;
  for (i = 0; i < 4; i++) if (idx[i] >= 5) return SOL_TX_ERR_IX_ACCOUNTS;
  if (byte(&c, &b)) return SOL_TX_ERR_TRUNCATED;
  if (b != 10) return SOL_TX_ERR_IX_DATA;
  if (take(&c, 10, &data)) return SOL_TX_ERR_TRUNCATED;
  if (data[0] != 12) return SOL_TX_ERR_IX_DATA;         /* 12 = TransferChecked */
  if (out->version == 0) {                              /* v0: address table lookups must be empty */
    if (byte(&c, &b)) return SOL_TX_ERR_TRUNCATED;
    if (b != 0) return SOL_TX_ERR_LOOKUPS;
  }
  if (c.at != c.n) return SOL_TX_ERR_TRAILING;
  /* TransferChecked accounts: 0 source (w), 1 mint (r), 2 destination (w), 3 authority (signer) */
  src = idx[0]; mint = idx[1]; dst = idx[2]; auth = idx[3];
  if (auth != 0) return SOL_TX_ERR_AUTHORITY;
  /* With 5 keys, 1 signer and 2 readonly-unsigned: index 0 signer, 1-2 writable, 3-4 readonly. */
  if (src < 1 || src > 2 || dst < 1 || dst > 2 || src == dst) return SOL_TX_ERR_ROLES;
  if (mint < 3 || prog < 3 || mint == prog) return SOL_TX_ERR_ROLES;
  memcpy(out->fee_payer, keys, 32);
  memcpy(out->source, keys + 32 * src, 32);
  memcpy(out->mint, keys + 32 * mint, 32);
  memcpy(out->destination, keys + 32 * dst, 32);
  memcpy(out->blockhash, bh, 32);
  out->amount = 0;
  for (i = 0; i < 8; i++) out->amount |= (uint64_t)data[1 + i] << (8 * i);
  out->decimals = data[9];
  if (out->amount == 0) return SOL_TX_ERR_AMOUNT_ZERO;
  return SOL_TX_OK;
}

const char *sol_tx_err_name(sol_tx_err_t e) {
  static const char *const N[] = {"ok", "too_long", "truncated", "version", "header", "accounts", "lookups",
    "ix_count", "program", "ix_accounts", "ix_data", "authority", "roles", "amount_zero", "trailing"};
  return (unsigned)e < sizeof N / sizeof N[0] ? N[e] : "?";
}

size_t sol_tx_build_transfer(const uint8_t payer[32], const uint8_t source[32],
                             const uint8_t destination[32], const uint8_t mint[32],
                             const uint8_t blockhash[32], uint64_t amount, uint8_t decimals,
                             uint8_t *out, size_t cap) {
  const int src_first = memcmp(source, destination, 32) < 0;
  const int mint_first = memcmp(mint, SOL_TOKEN_PROGRAM_ID, 32) < 0;
  uint8_t *p = out; int i;
  if (cap < 214 || memcmp(source, destination, 32) == 0) return 0;
  *p++ = 1; *p++ = 0; *p++ = 2;                         /* header */
  *p++ = 5;                                             /* account keys */
  memcpy(p, payer, 32); p += 32;
  memcpy(p, src_first ? source : destination, 32); p += 32;
  memcpy(p, src_first ? destination : source, 32); p += 32;
  memcpy(p, mint_first ? mint : SOL_TOKEN_PROGRAM_ID, 32); p += 32;
  memcpy(p, mint_first ? SOL_TOKEN_PROGRAM_ID : mint, 32); p += 32;
  memcpy(p, blockhash, 32); p += 32;
  *p++ = 1;                                             /* one instruction */
  *p++ = (uint8_t)(mint_first ? 4 : 3);                 /* program id index */
  *p++ = 4;
  *p++ = (uint8_t)(src_first ? 1 : 2);                  /* source */
  *p++ = (uint8_t)(mint_first ? 3 : 4);                 /* mint */
  *p++ = (uint8_t)(src_first ? 2 : 1);                  /* destination */
  *p++ = 0;                                             /* authority = payer */
  *p++ = 10;
  *p++ = 12;                                            /* TransferChecked */
  for (i = 0; i < 8; i++) *p++ = (uint8_t)(amount >> (8 * i));
  *p++ = decimals;
  return (size_t)(p - out);
}

size_t sol_format_amount(uint64_t raw, uint8_t decimals, char *out, size_t cap) {
  char digits[21]; size_t n = 0, i, w = 0, int_len;
  if (decimals > 19) return 0;
  do { digits[n++] = (char)('0' + raw % 10); raw /= 10; } while (raw);
  while (n <= decimals) digits[n++] = '0';              /* at least one integer digit */
  int_len = n - decimals;
  if (int_len + (decimals ? 1 + decimals : 0) + 1 > cap) return 0;
  for (i = 0; i < int_len; i++) out[w++] = digits[n - 1 - i];
  if (decimals) { out[w++] = '.'; for (i = 0; i < decimals; i++) out[w++] = digits[decimals - 1 - i]; }
  out[w] = '\0';
  return w;
}

int sol_parse_amount(const char *text, uint8_t decimals, uint64_t *raw) {
  uint64_t v = 0; int seen_digit = 0, frac = -1; const char *s;
  for (s = text; *s; s++) {
    if (*s == '.') { if (frac >= 0) return -1; frac = 0; continue; }
    if (*s < '0' || *s > '9') return -1;
    if (frac >= 0 && ++frac > (int)decimals) return -1;
    if (v > (UINT64_MAX - (uint64_t)(*s - '0')) / 10) return -1;
    v = v * 10 + (uint64_t)(*s - '0'); seen_digit = 1;
  }
  if (!seen_digit) return -1;
  for (frac = frac < 0 ? 0 : frac; frac < (int)decimals; frac++) {
    if (v > UINT64_MAX / 10) return -1;
    v *= 10;
  }
  *raw = v;
  return 0;
}
