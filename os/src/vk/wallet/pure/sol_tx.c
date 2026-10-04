/* sol_tx.c - Solana legacy message decoder and builder for one SPL Token TransferChecked with an
   optional Memo, plus the amount helpers. Spec: docs/os/wallet/solana-payments.md. */
#include "sol.h"
#include <string.h>

const uint8_t SOL_TOKEN_PROGRAM_ID[32] = {
  0x06,0xdd,0xf6,0xe1,0xd7,0x65,0xa1,0x93,0xd9,0xcb,0xe1,0x46,0xce,0xeb,0x79,0xac,
  0x1c,0xb4,0x85,0xed,0x5f,0x5b,0x37,0x91,0x3a,0x8c,0xf5,0x85,0x7e,0xff,0x00,0xa9};
const uint8_t SOL_MEMO_PROGRAM_ID[32] = {
  0x05,0x4a,0x53,0x5a,0x99,0x29,0x21,0x06,0x4d,0x24,0xe8,0x71,0x60,0xda,0x38,0x7c,
  0x7c,0x35,0xb5,0xdd,0xbc,0x92,0xbb,0x81,0xe4,0x1f,0xa8,0x40,0x41,0x05,0x44,0x8d};

#define TRANSFER_CHECKED 12        /* SPL Token instruction tag */
#define MSG_LEN_NO_MEMO  214       /* 3 header + 1 + 5*32 keys + 32 blockhash + 1 + 17 instruction */

typedef struct { const uint8_t *p; size_t n, at; } cur_t;
static int take(cur_t *c, size_t k, const uint8_t **out) {
  if (c->n - c->at < k) return -1;
  *out = c->p + c->at; c->at += k; return 0;
}
static int byte(cur_t *c, uint8_t *b) { const uint8_t *p; if (take(c, 1, &p)) return -1; *b = *p; return 0; }

/* compact-u16: 1 to 3 bytes, 7 bits each, low bits first, high bit set on every byte but the last.
   Returns 0 on success, -1 if the bytes run out, -2 if the encoding is not the shortest one for
   its value or does not fit 16 bits. */
static int cu16(cur_t *c, uint16_t *v) {
  uint32_t r = 0; uint8_t b; int i;
  for (i = 0; i < 3; i++) {
    if (byte(c, &b)) return -1;
    if (i == 2 && b > 0x03) return -2;                  /* third byte: two value bits, no continuation */
    r |= (uint32_t)(b & 0x7F) << (7 * i);
    if (!(b & 0x80)) {
      if (i > 0 && b == 0) return -2;                   /* a shorter encoding exists */
      *v = (uint16_t)r;
      return 0;
    }
  }
  return -2;
}

/* 1 if s[0..n) is well-formed UTF-8: no overlong forms, no surrogates, nothing above U+10FFFF. */
static int utf8_ok(const uint8_t *s, size_t n) {
  size_t i = 0, k, need; uint32_t cp, min;
  while (i < n) {
    const uint8_t b = s[i];
    if (b < 0x80) { i++; continue; }
    if ((b & 0xE0) == 0xC0)      { need = 1; cp = b & 0x1Fu; min = 0x80; }
    else if ((b & 0xF0) == 0xE0) { need = 2; cp = b & 0x0Fu; min = 0x800; }
    else if ((b & 0xF8) == 0xF0) { need = 3; cp = b & 0x07u; min = 0x10000; }
    else return 0;
    if (n - i - 1 < need) return 0;
    for (k = 1; k <= need; k++) {
      if ((s[i + k] & 0xC0) != 0x80) return 0;
      cp = (cp << 6) | (s[i + k] & 0x3Fu);
    }
    if (cp < min || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return 0;
    i += need + 1;
  }
  return 1;
}

sol_tx_err_t sol_tx_decode_transfer(const uint8_t *msg, size_t len, sol_transfer_t *out) {
  cur_t c; const uint8_t *keys, *bh, *idx = NULL, *data = NULL, *memo = NULL;
  uint8_t b, nro_unsigned, prog, token_prog = 0, memo_prog = 0, src, mint, dst, auth;
  uint16_t nkeys, nix, count, memo_len = 0; uint64_t amount = 0;
  int r, have_token = 0, have_memo = 0; unsigned i, k;

  /* rule 1 */
  if (len > SOL_TX_MSG_MAX) return SOL_TX_ERR_TOO_LONG;
  if (len < 1 || !msg) return SOL_TX_ERR_TRUNCATED;
  c.p = msg; c.n = len; c.at = 0;
  memset(out, 0, sizeof *out);
  /* rule 2: a versioned message starts with a byte whose high bit is set */
  if (msg[0] & 0x80) return SOL_TX_ERR_VERSION;
  /* rule 3: header num_required_signatures, num_readonly_signed, num_readonly_unsigned; then the keys */
  if (byte(&c, &b)) return SOL_TX_ERR_TRUNCATED;
  if (b != 1) return SOL_TX_ERR_HEADER;
  if (byte(&c, &b)) return SOL_TX_ERR_TRUNCATED;
  if (b != 0) return SOL_TX_ERR_HEADER;
  if (byte(&c, &nro_unsigned)) return SOL_TX_ERR_TRUNCATED;
  if (nro_unsigned != 2 && nro_unsigned != 3) return SOL_TX_ERR_HEADER;
  r = cu16(&c, &nkeys);
  if (r == -1) return SOL_TX_ERR_TRUNCATED;
  if (r != 0 || nkeys != (uint16_t)(nro_unsigned + 3)) return SOL_TX_ERR_ACCOUNTS;      /* 5 keys, or 6 with a Memo */
  if (take(&c, (size_t)nkeys * 32, &keys)) return SOL_TX_ERR_TRUNCATED;
  if (take(&c, 32, &bh)) return SOL_TX_ERR_TRUNCATED;
  /* rule 4 */
  r = cu16(&c, &nix);
  if (r == -1) return SOL_TX_ERR_TRUNCATED;
  if (r != 0 || nix != (uint16_t)(nkeys - 4)) return SOL_TX_ERR_IX_COUNT;
  for (i = 0; i < nix; i++) {
    /* rule 5: one Token instruction; with 6 keys the other one is a Memo */
    if (byte(&c, &prog)) return SOL_TX_ERR_TRUNCATED;
    if (prog >= nkeys) return SOL_TX_ERR_PROGRAM;
    if (memcmp(keys + 32 * (size_t)prog, SOL_TOKEN_PROGRAM_ID, 32) == 0) {
      if (have_token) return SOL_TX_ERR_PROGRAM;
      have_token = 1; token_prog = prog;
      /* rule 6 */
      r = cu16(&c, &count);
      if (r == -1) return SOL_TX_ERR_TRUNCATED;
      if (r != 0 || count != 4) return SOL_TX_ERR_IX_ACCOUNTS;
      if (take(&c, 4, &idx)) return SOL_TX_ERR_TRUNCATED;
      for (k = 0; k < 4; k++) if (idx[k] >= nkeys) return SOL_TX_ERR_IX_ACCOUNTS;
      /* rule 7 */
      r = cu16(&c, &count);
      if (r == -1) return SOL_TX_ERR_TRUNCATED;
      if (r != 0 || count != 10) return SOL_TX_ERR_IX_DATA;
      if (take(&c, 10, &data)) return SOL_TX_ERR_TRUNCATED;
      if (data[0] != TRANSFER_CHECKED) return SOL_TX_ERR_IX_DATA;
    } else if (nkeys == 6 && memcmp(keys + 32 * (size_t)prog, SOL_MEMO_PROGRAM_ID, 32) == 0) {
      if (have_memo) return SOL_TX_ERR_PROGRAM;
      have_memo = 1; memo_prog = prog;
      /* rule 11 */
      r = cu16(&c, &count);
      if (r == -1) return SOL_TX_ERR_TRUNCATED;
      if (r != 0 || count != 0) return SOL_TX_ERR_MEMO;
      r = cu16(&c, &memo_len);
      if (r == -1) return SOL_TX_ERR_TRUNCATED;
      if (r != 0 || memo_len == 0) return SOL_TX_ERR_MEMO;
      if (take(&c, memo_len, &memo)) return SOL_TX_ERR_TRUNCATED;
      if (!utf8_ok(memo, memo_len)) return SOL_TX_ERR_MEMO;
    } else {
      return SOL_TX_ERR_PROGRAM;
    }
  }
  /* The loop ran once (5 keys: it accepted only Token) or twice (6 keys: no program twice), so
     there is exactly one Token instruction and, with 6 keys, exactly one Memo. */
  if (!have_token) return SOL_TX_ERR_PROGRAM;
  /* rule 8: TransferChecked accounts are 0 source (w), 1 mint (r), 2 destination (w), 3 authority (signer) */
  src = idx[0]; mint = idx[1]; dst = idx[2]; auth = idx[3];
  if (auth != 0) return SOL_TX_ERR_AUTHORITY;
  /* rule 9: index 0 is the signer, 1-2 are writable, 3 and up are readonly */
  if (src < 1 || src > 2 || dst < 1 || dst > 2 || src == dst) return SOL_TX_ERR_ROLES;
  if (mint < 3 || token_prog < 3 || mint == token_prog) return SOL_TX_ERR_ROLES;
  if (have_memo && (memo_prog < 3 || memo_prog == mint || memo_prog == token_prog)) return SOL_TX_ERR_ROLES;
  /* rule 10 */
  for (k = 0; k < 8; k++) amount |= (uint64_t)data[1 + k] << (8 * k);
  if (amount == 0) return SOL_TX_ERR_AMOUNT_ZERO;
  /* rule 12 */
  if (c.at != c.n) return SOL_TX_ERR_TRAILING;

  memcpy(out->fee_payer, keys, 32);
  memcpy(out->source, keys + 32 * (size_t)src, 32);
  memcpy(out->mint, keys + 32 * (size_t)mint, 32);
  memcpy(out->destination, keys + 32 * (size_t)dst, 32);
  memcpy(out->blockhash, bh, 32);
  out->amount = amount;
  out->decimals = data[9];
  out->memo = have_memo ? memo : NULL;
  out->memo_len = have_memo ? memo_len : 0;
  return SOL_TX_OK;
}

const char *sol_tx_err_name(sol_tx_err_t e) {
  static const char *const N[] = {"ok", "too_long", "truncated", "version", "header", "accounts",
    "ix_count", "program", "ix_accounts", "ix_data", "authority", "roles", "amount_zero", "memo", "trailing"};
  return (unsigned)e < sizeof N / sizeof N[0] ? N[(unsigned)e] : "?";
}

size_t sol_tx_build_transfer(const uint8_t payer[32], const uint8_t source[32],
                             const uint8_t destination[32], const uint8_t mint[32],
                             const uint8_t blockhash[32], uint64_t amount, uint8_t decimals,
                             const uint8_t *memo, size_t memo_len,
                             uint8_t *out, size_t cap) {
  /* readonly keys: 0 mint, 1 Token program, 2 Memo program (only with a memo) */
  const uint8_t *ro[3]; uint8_t order[3] = {0, 1, 2}, index_of[3] = {0, 0, 0};
  const size_t nro = memo_len ? 3 : 2;
  const int src_first = memcmp(source, destination, 32) < 0;
  size_t total = MSG_LEN_NO_MEMO, i, j; uint8_t *p = out; int k;

  if (memcmp(source, destination, 32) == 0 || amount == 0) return 0;
  if (memo_len) {
    if (!memo || memo_len > SOL_TX_MSG_MAX || !utf8_ok(memo, memo_len)) return 0;
    total += 32 + 2 + (memo_len < 0x80 ? 1 : 2) + memo_len;      /* one more key; program index, 0 accounts, length, data */
  }
  if (total > SOL_TX_MSG_MAX || total > cap) return 0;

  ro[0] = mint; ro[1] = SOL_TOKEN_PROGRAM_ID; ro[2] = SOL_MEMO_PROGRAM_ID;
  for (i = 1; i < nro; i++)                                      /* insertion sort, ascending raw bytes */
    for (j = i; j > 0 && memcmp(ro[order[j]], ro[order[j - 1]], 32) < 0; j--) {
      const uint8_t t = order[j]; order[j] = order[j - 1]; order[j - 1] = t;
    }
  for (i = 0; i < nro; i++) index_of[order[i]] = (uint8_t)(3 + i);

  *p++ = 1; *p++ = 0; *p++ = (uint8_t)nro;                       /* header */
  *p++ = (uint8_t)(3 + nro);                                     /* account keys */
  memcpy(p, payer, 32); p += 32;
  memcpy(p, src_first ? source : destination, 32); p += 32;
  memcpy(p, src_first ? destination : source, 32); p += 32;
  for (i = 0; i < nro; i++) { memcpy(p, ro[order[i]], 32); p += 32; }
  memcpy(p, blockhash, 32); p += 32;
  *p++ = (uint8_t)(memo_len ? 2 : 1);                            /* instruction count */
  *p++ = index_of[1];                                            /* Token program */
  *p++ = 4;
  *p++ = (uint8_t)(src_first ? 1 : 2);                           /* source */
  *p++ = index_of[0];                                            /* mint */
  *p++ = (uint8_t)(src_first ? 2 : 1);                           /* destination */
  *p++ = 0;                                                      /* authority = payer */
  *p++ = 10;
  *p++ = TRANSFER_CHECKED;
  for (k = 0; k < 8; k++) *p++ = (uint8_t)(amount >> (8 * k));
  *p++ = decimals;
  if (memo_len) {
    *p++ = index_of[2];                                          /* Memo program */
    *p++ = 0;                                                    /* no accounts */
    if (memo_len < 0x80) *p++ = (uint8_t)memo_len;
    else { *p++ = (uint8_t)(0x80 | (memo_len & 0x7F)); *p++ = (uint8_t)(memo_len >> 7); }
    memcpy(p, memo, memo_len); p += memo_len;
  }
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
