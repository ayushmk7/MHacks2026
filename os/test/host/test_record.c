/* test_record - the registry record parser (canonical record accepted, every mutated copy refused)
   and the issuer-signature check. Spec: docs/os/wallet/checks.md ("Registry record", "Tests"). */
#include "../../src/vk/wallet/pure/vk_record.h"
#include <stdio.h>
#include <string.h>
#include "host_ed25519.h"
#include "vectors.h"

static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

/* The 11 lines of the vector record, in order. LINES[i] is "key=value" without the newline. */
enum { L_V, L_ATTESTATION, L_NAME, L_DEVICE, L_KIND, L_WALLET, L_ATA, L_BANK, L_EXPIRY, L_STATUS, L_ISSUED, L_COUNT };
static char LINES[L_COUNT][128];
static const char HEX_A[] = "00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff";

static void split_vector(void) {
  size_t i, line = 0, col = 0;
  memset(LINES, 0, sizeof LINES);
  for (i = 0; i < sizeof V_RECORD; i++) {
    if (V_RECORD[i] == '\n') { line++; col = 0; continue; }
    if (line < L_COUNT && col < sizeof LINES[0] - 1) LINES[line][col++] = (char)V_RECORD[i];
  }
  CHECK(line == L_COUNT - 1);
}

/* Joins 11 lines with '\n' (no trailing newline) into buf; returns the length. */
static size_t join(const char *const lines[L_COUNT], size_t count, char *buf, size_t cap) {
  size_t i, n = 0;
  for (i = 0; i < count; i++) {
    const size_t k = strlen(lines[i]);
    if (n + k + 2 > cap) return 0;
    if (i) buf[n++] = '\n';
    memcpy(buf + n, lines[i], k); n += k;
  }
  buf[n] = 0;
  return n;
}

/* Parses the vector record with line `which` replaced by `text`. Returns the parser's result. */
static int with_line(int which, const char *text, vk_record_t *out) {
  const char *lines[L_COUNT]; char buf[1024]; size_t n; int i;
  for (i = 0; i < L_COUNT; i++) lines[i] = (i == which) ? text : LINES[i];
  n = join(lines, L_COUNT, buf, sizeof buf);
  return vk_record_parse((const uint8_t *)buf, n, out);
}
static int parse_text(const char *text, vk_record_t *out) { return vk_record_parse((const uint8_t *)text, strlen(text), out); }

static void test_canonical(void) {
  vk_record_t r; const char *lines[L_COUNT]; char buf[1024]; size_t n; int i; uint8_t zero[32];
  memset(zero, 0, sizeof zero);
  for (i = 0; i < L_COUNT; i++) lines[i] = LINES[i];
  n = join(lines, L_COUNT, buf, sizeof buf);
  CHECK(n == sizeof V_RECORD && memcmp(buf, V_RECORD, n) == 0 && n <= VK_RECORD_MAX);   /* the helper reproduces the vector */

  memset(&r, 0xEE, sizeof r);
  CHECK(vk_record_parse(V_RECORD, sizeof V_RECORD, &r) == 0);
  CHECK(strcmp(r.attestation, LINES[L_ATTESTATION] + strlen("attestation=")) == 0 && strlen(r.attestation) >= 32);
  CHECK(strcmp(r.display_name, "MHacks Merch") == 0);
  CHECK(memcmp(r.device_pubkey, V_DEVICE_PUB, 32) == 0);
  CHECK(r.kind == 1 && r.kind == VK_KIND_MERCHANT);
  CHECK(r.has_solana == 1 && memcmp(r.solana_wallet, V_DST_OWNER, 32) == 0 && memcmp(r.solana_ata, V_DST, 32) == 0);
  CHECK(r.has_bank == 0 && memcmp(r.bank_ref_hash, zero, 32) == 0);
  CHECK(r.expiry == V_RECORD_EXPIRY && r.status == 1 && r.status == VK_STATUS_ACTIVE && r.issued_at == V_RECORD_ISSUED_AT);
}

static void test_accepted_variants(void) {
  vk_record_t r; char text[160]; uint8_t zero[32], want[32]; int i;
  memset(zero, 0, sizeof zero);
  for (i = 0; i < 32; i++) want[i] = (uint8_t)((i % 16) * 0x11);
  CHECK(with_line(L_ATTESTATION, "attestation=none", &r) == 0 && strcmp(r.attestation, "none") == 0);
  CHECK(with_line(L_ATTESTATION, "attestation=11111111111111111111111111111111", &r) == 0);
  CHECK(with_line(L_KIND, "kind=person", &r) == 0 && r.kind == 2 && r.kind == VK_KIND_PERSON);
  CHECK(with_line(L_STATUS, "status=revoked", &r) == 0 && r.status == 2 && r.status == VK_STATUS_REVOKED);
  CHECK(with_line(L_NAME, "display_name=A", &r) == 0 && strcmp(r.display_name, "A") == 0);
  CHECK(with_line(L_NAME, "display_name=12345678901234567890123456789012", &r) == 0 && strlen(r.display_name) == 32);
  CHECK(with_line(L_NAME, "display_name= a=b ~!", &r) == 0 && strcmp(r.display_name, " a=b ~!") == 0);     /* any printable ASCII, '=' included */
  snprintf(text, sizeof text, "bank_ref_hash=%s", HEX_A);
  CHECK(with_line(L_BANK, text, &r) == 0 && r.has_bank == 1 && memcmp(r.bank_ref_hash, want, 32) == 0 && r.has_solana == 1);
  CHECK(with_line(L_EXPIRY, "expiry=0", &r) == 0 && r.expiry == 0);
  CHECK(with_line(L_EXPIRY, "expiry=4294967295", &r) == 0 && r.expiry == 4294967295u);
  CHECK(with_line(L_ISSUED, "issued_at=1", &r) == 0 && r.issued_at == 1);
  /* a record with no Solana account: has_solana 0 and both arrays zero */
  { const char *lines[L_COUNT]; char buf[1024]; size_t n;
    for (i = 0; i < L_COUNT; i++) lines[i] = LINES[i];
    lines[L_WALLET] = "solana_wallet="; lines[L_ATA] = "solana_ata=";
    n = join(lines, L_COUNT, buf, sizeof buf);
    CHECK(vk_record_parse((const uint8_t *)buf, n, &r) == 0 && r.has_solana == 0 && !memcmp(r.solana_wallet, zero, 32) && !memcmp(r.solana_ata, zero, 32));
    CHECK(memcmp(r.device_pubkey, V_DEVICE_PUB, 32) == 0 && r.expiry == V_RECORD_EXPIRY); }
  /* only one of the two present: it parses, but has_solana is 0 (so check 10 reports WRONG RECIPIENT) */
  CHECK(with_line(L_ATA, "solana_ata=", &r) == 0 && r.has_solana == 0 && !memcmp(r.solana_wallet, zero, 32));
  CHECK(with_line(L_WALLET, "solana_wallet=", &r) == 0 && r.has_solana == 0 && !memcmp(r.solana_ata, zero, 32));
}

static void test_refused(void) {
  vk_record_t r; const char *lines[L_COUNT + 1]; char buf[1024], text[200]; size_t n; int i;

  /* --- the six mutations named in the spec --- */
  /* reordered key */
  for (i = 0; i < L_COUNT; i++) lines[i] = LINES[i];
  lines[L_KIND] = LINES[L_DEVICE]; lines[L_DEVICE] = LINES[L_KIND];
  n = join(lines, L_COUNT, buf, sizeof buf); CHECK(n == sizeof V_RECORD && vk_record_parse((const uint8_t *)buf, n, &r) != 0);
  for (i = 0; i < L_COUNT; i++) lines[i] = LINES[i];
  lines[L_EXPIRY] = LINES[L_ISSUED]; lines[L_ISSUED] = LINES[L_EXPIRY];
  n = join(lines, L_COUNT, buf, sizeof buf); CHECK(vk_record_parse((const uint8_t *)buf, n, &r) != 0);
  /* extra line: at the end, in the middle, at the start */
  for (i = 0; i < L_COUNT; i++) lines[i] = LINES[i];
  lines[L_COUNT] = "note=hello";
  n = join(lines, L_COUNT + 1, buf, sizeof buf); CHECK(vk_record_parse((const uint8_t *)buf, n, &r) != 0);
  lines[L_COUNT] = LINES[L_ISSUED];
  n = join(lines, L_COUNT + 1, buf, sizeof buf); CHECK(vk_record_parse((const uint8_t *)buf, n, &r) != 0);
  CHECK(with_line(L_KIND, "kind=merchant\nkind=merchant", &r) != 0);
  CHECK(with_line(L_V, "note=x\nv=1", &r) != 0);
  CHECK(with_line(L_STATUS, "status=active\n", &r) != 0);                   /* an empty line */
  /* upper-case hex */
  snprintf(text, sizeof text, "%s", LINES[L_DEVICE]); text[strlen("device_pubkey=")] = 'A';
  CHECK(with_line(L_DEVICE, text, &r) != 0);
  CHECK(with_line(L_ATA, "solana_ata=00112233445566778899AABBCCDDEEFF00112233445566778899aabbccddeeff", &r) != 0);
  CHECK(with_line(L_WALLET, "solana_wallet=00112233445566778899aabbccddeeff00112233445566778899aabbccddeefF", &r) != 0);
  CHECK(with_line(L_BANK, "bank_ref_hash=00112233445566778899aabbccddeeff00112233445566778899aabbccddeeFf", &r) != 0);
  /* trailing newline */
  memcpy(buf, V_RECORD, sizeof V_RECORD); buf[sizeof V_RECORD] = '\n';
  CHECK(vk_record_parse((const uint8_t *)buf, sizeof V_RECORD + 1, &r) != 0);
  /* 33-character name */
  CHECK(with_line(L_NAME, "display_name=123456789012345678901234567890123", &r) != 0);
  /* non-numeric expiry */
  CHECK(with_line(L_EXPIRY, "expiry=soon", &r) != 0);
  CHECK(with_line(L_EXPIRY, "expiry=18000000x0", &r) != 0);

  /* --- wrong key, missing line, wrong separators --- */
  CHECK(with_line(L_NAME, "name=MHacks Merch", &r) != 0);
  CHECK(with_line(L_NAME, "Display_name=MHacks Merch", &r) != 0);
  CHECK(with_line(L_NAME, "display_name MHacks Merch", &r) != 0);
  CHECK(with_line(L_NAME, "display_name =MHacks Merch", &r) != 0);
  CHECK(with_line(L_V, "v=2", &r) != 0);
  CHECK(with_line(L_V, "v=01", &r) != 0);
  CHECK(with_line(L_V, "v=", &r) != 0);
  CHECK(with_line(L_V, "\nv=1", &r) != 0);                                  /* leading newline */
  CHECK(with_line(L_KIND, "kind=merchant\r", &r) != 0);                     /* CRLF line ending */
  for (i = 0; i < L_COUNT; i++) lines[i] = LINES[i];
  n = join(lines, L_COUNT - 1, buf, sizeof buf); CHECK(vk_record_parse((const uint8_t *)buf, n, &r) != 0);   /* issued_at missing */
  for (i = L_BANK; i < L_COUNT - 1; i++) lines[i] = LINES[i + 1];
  n = join(lines, L_COUNT - 1, buf, sizeof buf); CHECK(vk_record_parse((const uint8_t *)buf, n, &r) != 0);   /* bank_ref_hash missing */
  CHECK(parse_text("", &r) != 0 && parse_text("v=1", &r) != 0 && parse_text("v=1\n", &r) != 0);
  CHECK(vk_record_parse(NULL, 10, &r) != 0 && vk_record_parse(V_RECORD, 0, &r) != 0);
  /* every truncated copy, up to and including the one that ends in "issued_at=" (cutting digits off
     the last number leaves a well-formed record with another value; the signature catches that) */
  { const size_t digits = strlen(LINES[L_ISSUED]) - strlen("issued_at=");
    for (n = 1; n <= sizeof V_RECORD - digits; n++) if (vk_record_parse(V_RECORD, n, &r) == 0) break;
    CHECK(n == sizeof V_RECORD - digits + 1);
    CHECK(vk_record_parse(V_RECORD, sizeof V_RECORD - 1, &r) == 0 && r.issued_at == V_RECORD_ISSUED_AT / 10);
    CHECK(vk_record_verify(V_RECORD, sizeof V_RECORD - 1, V_RECORD_SIG, V_ISSUER_PUB, host_ed25519_verify) == 0); }
  memset(buf, 0, sizeof buf); memcpy(buf, V_RECORD, sizeof V_RECORD);
  CHECK(vk_record_parse((const uint8_t *)buf, VK_RECORD_MAX + 1, &r) != 0);                 /* over 512 bytes */
  CHECK(vk_record_parse((const uint8_t *)buf, sizeof V_RECORD + 1, &r) != 0);               /* a NUL after the record */

  /* --- field values --- */
  CHECK(with_line(L_ATTESTATION, "attestation=", &r) != 0);
  CHECK(with_line(L_ATTESTATION, "attestation=None", &r) != 0);
  CHECK(with_line(L_ATTESTATION, "attestation=abc", &r) != 0);                               /* base58, but not 32 bytes */
  CHECK(with_line(L_ATTESTATION, "attestation=0pKipmyKAyJYMdymUMbhjXX91rDxrLENh6YTY64UuPQg", &r) != 0);   /* '0' is not base58 */
  CHECK(with_line(L_ATTESTATION, "attestation=GpKipmyKAyJYMdymUMbhjXX91rDxrLENh6YTY64UuPQgg", &r) != 0);  /* 45 characters */
  CHECK(with_line(L_NAME, "display_name=", &r) != 0);
  CHECK(with_line(L_NAME, "display_name=Tab\there", &r) != 0);
  CHECK(with_line(L_NAME, "display_name=Del\x7Fhere", &r) != 0);
  CHECK(with_line(L_NAME, "display_name=Caf\xC3\xA9", &r) != 0);                             /* not ASCII */
  CHECK(with_line(L_DEVICE, "device_pubkey=", &r) != 0);
  snprintf(text, sizeof text, "%s", LINES[L_DEVICE]); text[strlen(text) - 1] = 0;
  CHECK(with_line(L_DEVICE, text, &r) != 0);                                                 /* 63 hex digits */
  snprintf(text, sizeof text, "%s0", LINES[L_DEVICE]);
  CHECK(with_line(L_DEVICE, text, &r) != 0);                                                 /* 65 hex digits */
  snprintf(text, sizeof text, "%s", LINES[L_DEVICE]); text[strlen(text) - 1] = 'g';
  CHECK(with_line(L_DEVICE, text, &r) != 0);                                                 /* not a hex digit */
  CHECK(with_line(L_ATA, "solana_ata=abcd", &r) != 0);
  CHECK(with_line(L_BANK, "bank_ref_hash=none", &r) != 0);
  CHECK(with_line(L_KIND, "kind=shop", &r) != 0 && with_line(L_KIND, "kind=", &r) != 0 && with_line(L_KIND, "kind=Merchant", &r) != 0 && with_line(L_KIND, "kind=merchants", &r) != 0);
  CHECK(with_line(L_STATUS, "status=ACTIVE", &r) != 0 && with_line(L_STATUS, "status=", &r) != 0 && with_line(L_STATUS, "status=expired", &r) != 0);
  /* bad numbers */
  CHECK(with_line(L_EXPIRY, "expiry=", &r) != 0);
  CHECK(with_line(L_EXPIRY, "expiry=-5", &r) != 0);
  CHECK(with_line(L_EXPIRY, "expiry=+5", &r) != 0);
  CHECK(with_line(L_EXPIRY, "expiry= 5", &r) != 0);
  CHECK(with_line(L_EXPIRY, "expiry=1.5", &r) != 0);
  CHECK(with_line(L_EXPIRY, "expiry=0x10", &r) != 0);
  CHECK(with_line(L_EXPIRY, "expiry=0180000000", &r) != 0);                                  /* leading zero */
  CHECK(with_line(L_EXPIRY, "expiry=00", &r) != 0);
  CHECK(with_line(L_ISSUED, "issued_at=01", &r) != 0);
  CHECK(with_line(L_EXPIRY, "expiry=4294967296", &r) != 0);                                  /* does not fit a u32 */
  CHECK(with_line(L_EXPIRY, "expiry=99999999999", &r) != 0);
  CHECK(with_line(L_ISSUED, "issued_at=now", &r) != 0);
  CHECK(with_line(L_ISSUED, "issued_at=", &r) != 0);
  CHECK(with_line(L_ISSUED, "issued_at=1790000000 ", &r) != 0);
}

/* Captures what the pure code hands to the verifier. */
static uint8_t seen[1024]; static size_t seen_len; static const uint8_t *seen_sig, *seen_key; static int stub_result;
static int stub_verify(const uint8_t *msg, size_t len, const uint8_t *sig, const uint8_t *key) {
  seen_len = len; memcpy(seen, msg, len < sizeof seen ? len : sizeof seen); seen_sig = sig; seen_key = key;
  return stub_result;
}

static void test_verify(void) {
  uint8_t copy[sizeof V_RECORD], sig[64], big[VK_RECORD_MAX + 1]; size_t i;
  CHECK(vk_record_verify(V_RECORD, sizeof V_RECORD, V_RECORD_SIG, V_ISSUER_PUB, host_ed25519_verify) == 1);
  /* one flipped bit anywhere in the record fails */
  for (i = 0; i < sizeof V_RECORD; i++) {
    memcpy(copy, V_RECORD, sizeof V_RECORD); copy[i] ^= (uint8_t)(1u << (i % 8));
    if (vk_record_verify(copy, sizeof V_RECORD, V_RECORD_SIG, V_ISSUER_PUB, host_ed25519_verify) != 0) break;
  }
  CHECK(i == sizeof V_RECORD);
  /* one flipped bit in the signature fails */
  memcpy(sig, V_RECORD_SIG, 64); sig[0] ^= 1;
  CHECK(vk_record_verify(V_RECORD, sizeof V_RECORD, sig, V_ISSUER_PUB, host_ed25519_verify) == 0);
  memcpy(sig, V_RECORD_SIG, 64); sig[63] ^= 0x10;
  CHECK(vk_record_verify(V_RECORD, sizeof V_RECORD, sig, V_ISSUER_PUB, host_ed25519_verify) == 0);
  /* another key, a shorter record */
  CHECK(vk_record_verify(V_RECORD, sizeof V_RECORD, V_RECORD_SIG, V_DEVICE_PUB, host_ed25519_verify) == 0);
  CHECK(vk_record_verify(V_RECORD, sizeof V_RECORD - 1, V_RECORD_SIG, V_ISSUER_PUB, host_ed25519_verify) == 0);
  /* the signed bytes are "registry:" || record: a signature over the bare record is not accepted */
  host_ed25519_sign(V_ISSUER_SEED, V_RECORD, sizeof V_RECORD, sig);
  CHECK(host_ed25519_verify(V_RECORD, sizeof V_RECORD, sig, V_ISSUER_PUB) == 1);
  CHECK(vk_record_verify(V_RECORD, sizeof V_RECORD, sig, V_ISSUER_PUB, host_ed25519_verify) == 0);
  /* a record signed by the device key is not an issuer record */
  { uint8_t pre[9 + sizeof V_RECORD];
    memcpy(pre, "registry:", 9); memcpy(pre + 9, V_RECORD, sizeof V_RECORD);
    host_ed25519_sign(V_DEVICE_SEED, pre, sizeof pre, sig);
    CHECK(vk_record_verify(V_RECORD, sizeof V_RECORD, sig, V_ISSUER_PUB, host_ed25519_verify) == 0);
    CHECK(vk_record_verify(V_RECORD, sizeof V_RECORD, sig, V_DEVICE_PUB, host_ed25519_verify) == 1); }
  /* exactly what reaches the verifier */
  stub_result = 1; seen_len = 0;
  CHECK(vk_record_verify(V_RECORD, sizeof V_RECORD, V_RECORD_SIG, V_ISSUER_PUB, stub_verify) == 1);
  CHECK(seen_len == 9 + sizeof V_RECORD && memcmp(seen, VK_RECORD_PREFIX, 9) == 0 && memcmp(seen + 9, V_RECORD, sizeof V_RECORD) == 0);
  CHECK(seen_sig == V_RECORD_SIG && seen_key == V_ISSUER_PUB && strcmp(VK_RECORD_PREFIX, "registry:") == 0);
  stub_result = 0;  CHECK(vk_record_verify(V_RECORD, sizeof V_RECORD, V_RECORD_SIG, V_ISSUER_PUB, stub_verify) == 0);
  stub_result = -1; CHECK(vk_record_verify(V_RECORD, sizeof V_RECORD, V_RECORD_SIG, V_ISSUER_PUB, stub_verify) == 0);
  stub_result = 2;  CHECK(vk_record_verify(V_RECORD, sizeof V_RECORD, V_RECORD_SIG, V_ISSUER_PUB, stub_verify) == 0);   /* only 1 means valid */
  /* missing inputs and oversize records never reach the verifier */
  stub_result = 1; seen_len = 0; memset(big, 'a', sizeof big);
  CHECK(vk_record_verify(NULL, 10, V_RECORD_SIG, V_ISSUER_PUB, stub_verify) == 0);
  CHECK(vk_record_verify(V_RECORD, 0, V_RECORD_SIG, V_ISSUER_PUB, stub_verify) == 0);
  CHECK(vk_record_verify(V_RECORD, sizeof V_RECORD, NULL, V_ISSUER_PUB, stub_verify) == 0);
  CHECK(vk_record_verify(V_RECORD, sizeof V_RECORD, V_RECORD_SIG, NULL, stub_verify) == 0);
  CHECK(vk_record_verify(V_RECORD, sizeof V_RECORD, V_RECORD_SIG, V_ISSUER_PUB, NULL) == 0);
  CHECK(vk_record_verify(big, sizeof big, V_RECORD_SIG, V_ISSUER_PUB, stub_verify) == 0);
  CHECK(seen_len == 0);
  CHECK(vk_record_verify(big, VK_RECORD_MAX, V_RECORD_SIG, V_ISSUER_PUB, stub_verify) == 1 && seen_len == 9 + VK_RECORD_MAX);
}

int main(void) {
  split_vector();
  test_canonical();
  test_accepted_variants();
  test_refused();
  test_verify();
  printf(fails ? "%d FAILED\n" : "all record tests passed\n", fails);
  return fails != 0;
}
