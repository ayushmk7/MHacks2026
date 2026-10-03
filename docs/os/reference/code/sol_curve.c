/* Curve membership for PDA derivation. Field arithmetic is TweetNaCl's (public domain),
   copied here because tweetnacl.c keeps it file-static and must not be edited. */
#include "sol.h"
typedef int64_t gf[16];
static const gf GF1 = {1};
static const gf GF_D = {0x78a3, 0x1359, 0x4dca, 0x75eb, 0xd8ab, 0x4141, 0x0a4d, 0x0070,
                        0xe898, 0x7779, 0x4079, 0x8cc7, 0xfe73, 0x2b6f, 0x6cee, 0x5203};
static const gf GF_I = {0xa0b0, 0x4a0e, 0x1b27, 0xc4ee, 0xe478, 0xad2f, 0x1806, 0x2f43,
                        0xd7a7, 0x3dfb, 0x0099, 0x2b4d, 0xdf0b, 0x4fc1, 0x2480, 0x2b83};

static void car(gf o) {
  int i; int64_t c;
  for (i = 0; i < 16; i++) {
    o[i] += (1LL << 16);
    c = o[i] >> 16;
    o[(i + 1) * (i < 15)] += c - 1 + 37 * (c - 1) * (i == 15);
    o[i] -= c * 65536;
  }
}
static void sel(gf p, gf q, int b) {
  int64_t t, c = ~(int64_t)(b - 1); int i;
  for (i = 0; i < 16; i++) { t = c & (p[i] ^ q[i]); p[i] ^= t; q[i] ^= t; }
}
static void pack(uint8_t *o, const gf n) {
  int i, j, b; gf m, t;
  for (i = 0; i < 16; i++) t[i] = n[i];
  car(t); car(t); car(t);
  for (j = 0; j < 2; j++) {
    m[0] = t[0] - 0xffed;
    for (i = 1; i < 15; i++) { m[i] = t[i] - 0xffff - ((m[i - 1] >> 16) & 1); m[i - 1] &= 0xffff; }
    m[15] = t[15] - 0x7fff - ((m[14] >> 16) & 1);
    b = (int)((m[15] >> 16) & 1);
    m[14] &= 0xffff;
    sel(t, m, 1 - b);
  }
  for (i = 0; i < 16; i++) { o[2 * i] = (uint8_t)(t[i] & 0xff); o[2 * i + 1] = (uint8_t)(t[i] >> 8); }
}
static int neq(const gf a, const gf b) {
  uint8_t c[32], d[32]; unsigned x = 0; int i;
  pack(c, a); pack(d, b);
  for (i = 0; i < 32; i++) x |= (unsigned)(c[i] ^ d[i]);
  return x != 0;
}
static void unpack(gf o, const uint8_t *n) {
  int i;
  for (i = 0; i < 16; i++) o[i] = n[2 * i] + ((int64_t)n[2 * i + 1] << 8);
  o[15] &= 0x7fff;
}
static void add(gf o, const gf a, const gf b) { int i; for (i = 0; i < 16; i++) o[i] = a[i] + b[i]; }
static void sub(gf o, const gf a, const gf b) { int i; for (i = 0; i < 16; i++) o[i] = a[i] - b[i]; }
static void mul(gf o, const gf a, const gf b) {
  int64_t t[31]; int i, j;
  for (i = 0; i < 31; i++) t[i] = 0;
  for (i = 0; i < 16; i++) for (j = 0; j < 16; j++) t[i + j] += a[i] * b[j];
  for (i = 0; i < 15; i++) t[i] += 38 * t[i + 16];
  for (i = 0; i < 16; i++) o[i] = t[i];
  car(o); car(o);
}
static void pow2523(gf o, const gf in) {
  gf c; int a;
  for (a = 0; a < 16; a++) c[a] = in[a];
  for (a = 250; a >= 0; a--) { mul(c, c, c); if (a != 1) mul(c, c, in); }
  for (a = 0; a < 16; a++) o[a] = c[a];
}

int sol_is_on_curve(const uint8_t p[32]) {
  gf y, num, den, den2, den4, den6, t, x, chk;
  unpack(y, p);                       /* y, sign bit dropped */
  mul(num, y, y);                     /* y^2 */
  mul(den, num, GF_D);                /* d*y^2 */
  sub(num, num, GF1);                 /* u = y^2 - 1 */
  add(den, GF1, den);                 /* v = d*y^2 + 1 */
  mul(den2, den, den); mul(den4, den2, den2); mul(den6, den4, den2);
  mul(t, den6, num); mul(t, t, den);
  pow2523(t, t);
  mul(t, t, num); mul(t, t, den); mul(t, t, den); mul(x, t, den);   /* candidate x */
  mul(chk, x, x); mul(chk, chk, den);
  if (neq(chk, num)) mul(x, x, GF_I);
  mul(chk, x, x); mul(chk, chk, den);
  return neq(chk, num) ? 0 : 1;       /* v*x^2 == u  <=>  point exists */
}
