#include "wallet_decode.h"

#include <string.h>

namespace wallet_decode {
namespace {

const char *const PREFIXES[] = {"pay-req:", "pay-proof:", "bank-auth:", "registry:"};

// SPL Token and Memo program ids (00 §2), raw.
const uint8_t TOKEN_PROGRAM[32] = {
    0x06, 0xdd, 0xf6, 0xe1, 0xd7, 0x65, 0xa1, 0x93, 0xd9, 0xcb, 0xe1, 0x46, 0xce, 0xeb, 0x79, 0xac,
    0x1c, 0xb4, 0x85, 0xed, 0x5f, 0x5b, 0x37, 0x91, 0x3a, 0x8c, 0xf5, 0x85, 0x7e, 0xff, 0x00, 0xa9};
const uint8_t MEMO_PROGRAM[32] = {
    0x05, 0x4a, 0x53, 0x5a, 0x99, 0x29, 0x21, 0x06, 0x4d, 0x24, 0xe8, 0x71, 0x60, 0xda, 0x38, 0x7c,
    0x7c, 0x35, 0xb5, 0xdd, 0xbc, 0x92, 0xbb, 0x81, 0xe4, 0x1f, 0xa8, 0x40, 0x41, 0x05, 0x44, 0x8d};

constexpr uint8_t TRANSFER_CHECKED = 12;
constexpr size_t MAX_KEYS = 64;  // a 1232 B message cannot hold more than 37
constexpr size_t MAX_IXS = 16;

// Strict UTF-8, as TextDecoder({fatal: true}): no overlongs, surrogates or > U+10FFFF.
bool validUtf8(const uint8_t *s, size_t n) {
  size_t i = 0;
  while (i < n) {
    const uint8_t c = s[i];
    size_t extra;
    uint32_t cp;
    if (c < 0x80) { ++i; continue; }
    if (c >= 0xc2 && c <= 0xdf) { extra = 1; cp = c & 0x1f; }
    else if (c >= 0xe0 && c <= 0xef) { extra = 2; cp = c & 0x0f; }
    else if (c >= 0xf0 && c <= 0xf4) { extra = 3; cp = c & 0x07; }
    else return false;
    if (i + extra >= n) return false;
    for (size_t k = 1; k <= extra; ++k) {
      if ((s[i + k] & 0xc0) != 0x80) return false;
      cp = (cp << 6) | (s[i + k] & 0x3f);
    }
    if ((extra == 2 && cp < 0x800) || (extra == 3 && cp < 0x10000)) return false;
    if (cp >= 0xd800 && cp <= 0xdfff) return false;
    if (cp > 0x10ffff) return false;
    i += extra + 1;
  }
  return true;
}

struct Reader {
  const uint8_t *p;
  size_t n, pos;
  const char *err;

  bool need(size_t k) {
    if (err) return false;
    if (pos + k > n) { err = "truncated"; return false; }
    return true;
  }
  uint8_t byte() { return need(1) ? p[pos++] : 0; }
  const uint8_t *take(size_t k) {
    if (!need(k)) return nullptr;
    const uint8_t *at = p + pos;
    pos += k;
    return at;
  }
  // Canonical short_vec, as Solana's sanitizer requires.
  uint16_t cu16() {
    uint32_t v = 0;
    for (int i = 0; i < 3; ++i) {
      const uint8_t b = byte();
      if (err) return 0;
      v |= (uint32_t)(b & 0x7f) << (7 * i);
      if (!(b & 0x80)) {
        if ((i > 0 && b == 0) || v > 0xffff) { err = "compact_u16"; return 0; }
        return (uint16_t)v;
      }
    }
    err = "compact_u16";
    return 0;
  }
};

bool printable(const char *v, size_t n, size_t max) {
  if (n == 0 || n > max || v[0] == ' ' || v[n - 1] == ' ') return false;
  for (size_t i = 0; i < n; ++i) {
    if (v[i] < 0x20 || v[i] > 0x7e) return false;
  }
  return true;
}

// ^[1-9][0-9]{0,max-1}$
bool decimal(const char *v, size_t n, size_t max) {
  if (n == 0 || n > max || v[0] < '1' || v[0] > '9') return false;
  for (size_t i = 1; i < n; ++i) {
    if (v[i] < '0' || v[i] > '9') return false;
  }
  return true;
}

bool lowerHex(const char *v, size_t n, size_t want) {
  if (n != want) return false;
  for (size_t i = 0; i < n; ++i) {
    if (!((v[i] >= '0' && v[i] <= '9') || (v[i] >= 'a' && v[i] <= 'f'))) return false;
  }
  return true;
}

}  // namespace

bool reservedPrefix(const uint8_t *data, size_t length) {
  for (const char *p : PREFIXES) {
    const size_t k = strlen(p);
    if (length >= k && memcmp(data, p, k) == 0) return true;
  }
  return false;
}

const char *solana(const uint8_t *msg, size_t length, const uint8_t payer[32],
                   const uint8_t mint[32], uint8_t decimals, Solana *out) {
  if (length > MAX_MESSAGE) return "oversized";
  if (reservedPrefix(msg, length)) return "reserved_prefix";
  if (length == 0) return "truncated";
  if (msg[0] & 0x80) return "versioned";

  Reader r{msg, length, 0, nullptr};
  const uint8_t required = r.byte(), roSigned = r.byte(), roUnsigned = r.byte();
  if (r.err) return r.err;
  if (required != 1) return "signer_count";
  if (roSigned != 0) return "header";  // the fee payer must be writable
  const uint16_t nKeys = r.cu16();
  if (r.err) return r.err;
  if (nKeys < 1 || required + roUnsigned > nKeys) return "header";
  if (nKeys > MAX_KEYS) return "truncated";  // cannot fit; the JS reader runs out of bytes too
  const uint8_t *keys[MAX_KEYS];
  for (uint16_t i = 0; i < nKeys; ++i) keys[i] = r.take(32);
  if (r.err) return r.err;
  if (memcmp(keys[0], payer, 32) != 0) return "payer";
  r.take(32);  // recent blockhash
  const uint16_t nIx = r.cu16();
  if (r.err) return r.err;

  struct Ix {
    const uint8_t *program;
    const uint8_t *accts;
    uint16_t nAcc;
    const uint8_t *data;
    uint16_t dataLength;
  } ixs[MAX_IXS];
  size_t kept = 0;
  for (uint16_t i = 0; i < nIx; ++i) {
    const uint8_t prog = r.byte();
    const uint16_t nAcc = r.cu16();
    const uint8_t *accts = r.take(nAcc);
    const uint16_t dataLength = r.cu16();
    const uint8_t *data = r.take(dataLength);
    if (r.err) return r.err;
    if (prog >= nKeys) return "index";
    for (uint16_t a = 0; a < nAcc; ++a) {
      if (accts[a] >= nKeys) return "index";
    }
    // More than MAX_IXS can never pass the one-transfer-one-memo rule below; keep counting.
    if (kept < MAX_IXS) ixs[kept] = {keys[prog], accts, nAcc, data, dataLength};
    ++kept;
  }
  if (r.pos != length) return "trailing";

  const Ix *token = nullptr;
  const Ix *memo = nullptr;
  size_t nToken = 0, nMemo = 0;
  for (size_t i = 0; i < kept && i < MAX_IXS; ++i) {
    if (memcmp(ixs[i].program, TOKEN_PROGRAM, 32) == 0) { token = &ixs[i]; ++nToken; }
    else if (memcmp(ixs[i].program, MEMO_PROGRAM, 32) == 0) { memo = &ixs[i]; ++nMemo; }
  }
  if (nToken != 1 || nMemo > 1 || nToken + nMemo != kept) return "instructions";
  if (token->dataLength != 10 || token->data[0] != TRANSFER_CHECKED) return "not_transfer_checked";
  if (token->nAcc != 4) return "accounts";
  if (token->accts[3] != 0) return "owner";
  if (memcmp(keys[token->accts[1]], mint, 32) != 0) return "mint";
  if (token->data[9] != decimals) return "decimals";
  if (memo) {
    if (memo->nAcc != 0) return "memo_accounts";
    if (!validUtf8(memo->data, memo->dataLength)) return "memo_utf8";
  }

  if (out) {
    uint64_t amount = 0;
    for (int b = 8; b >= 1; --b) amount = (amount << 8) | token->data[b];
    out->amount = amount;
    out->source = keys[token->accts[0]];
    out->destination = keys[token->accts[2]];
    out->memo = memo ? memo->data : nullptr;
    out->memoLength = memo ? memo->dataLength : 0;
  }
  return nullptr;
}

const char *request(const uint8_t *f, size_t length, const uint8_t self[32]) {
  if (length < REQ_FIXED) return "truncated";
  if (f[0] != 'V' || f[1] != 'K') return "magic";
  if (f[2] != 1) return "version";
  if (f[3] != 1) return "type";
  const uint8_t nameLength = f[61];
  if (nameLength > 32) return "name_len";
  if (length != REQ_FIXED + nameLength) return length < REQ_FIXED + nameLength ? "truncated" : "trailing";
  const uint8_t rail = f[4];
  const char *currency = rail == 1 ? "HACK" : rail == 2 ? "USD\0" : nullptr;
  if (!currency || memcmp(f + 45, currency, 4) != 0) return "rail_currency";
  bool zero = true;
  for (int i = 37; i < 45; ++i) zero = zero && f[i] == 0;
  if (zero) return "amount";
  if (memcmp(f + 5, self, 32) != 0) return "payee_not_self";
  for (size_t i = REQ_FIXED; i < length; ++i) {
    if (f[i] < 0x20 || f[i] > 0x7e) return "name";
  }
  return nullptr;
}

const char *bank(const uint8_t *payload, size_t length, Bank *out) {
  static const char *const KEYS[] = {"v", "rail", "action", "amount_cents", "currency", "from_acct",
                                     "payee_name", "payee_ref", "attestation", "req_id",
                                     "proof_nonce", "issued_at"};
  constexpr size_t N = sizeof(KEYS) / sizeof(KEYS[0]);
  if (length > MAX_MESSAGE) return "oversized";
  if (reservedPrefix(payload, length)) return "reserved_prefix";
  if (!validUtf8(payload, length)) return "utf8";

  const char *text = (const char *)payload;
  const char *value[N];
  size_t valueLength[N];
  size_t line = 0, start = 0;
  for (size_t i = 0; i <= length; ++i) {
    if (i < length && text[i] != '\n') continue;
    if (line >= N) return "line_count";
    const size_t k = strlen(KEYS[line]);
    const size_t n = i - start;
    if (n < k + 1 || memcmp(text + start, KEYS[line], k) != 0 || text[start + k] != '=') return "key_order";
    value[line] = text + start + k + 1;
    valueLength[line] = n - k - 1;
    ++line;
    start = i + 1;
  }
  if (line != N) return "line_count";

  auto is = [&](size_t i, const char *want) {
    return valueLength[i] == strlen(want) && memcmp(value[i], want, valueLength[i]) == 0;
  };
  const bool ok = is(0, "1") && is(1, "nessie") && (is(2, "purchase") || is(2, "transfer")) &&
                  decimal(value[3], valueLength[3], 12) && is(4, "USD") &&
                  printable(value[5], valueLength[5], 64) && printable(value[6], valueLength[6], 32) &&
                  lowerHex(value[7], valueLength[7], 64) && printable(value[8], valueLength[8], 64) &&
                  lowerHex(value[9], valueLength[9], 16) && lowerHex(value[10], valueLength[10], 32) &&
                  decimal(value[11], valueLength[11], 11);
  if (!ok) return "field";

  if (out) {
    *out = {value[2], valueLength[2], value[3], valueLength[3], value[6], valueLength[6]};
  }
  return nullptr;
}

bool base58To32(const char *text, uint8_t out[32]) {
  static const char ALPHABET[] = "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz";
  uint8_t bytes[32] = {0};
  size_t zeros = 0;
  const size_t n = strlen(text);
  if (n == 0 || n > 44) return false;
  while (zeros < n && text[zeros] == '1') ++zeros;
  for (size_t i = 0; i < n; ++i) {
    const char *at = strchr(ALPHABET, text[i]);
    if (!at || !*at) return false;
    uint32_t carry = (uint32_t)(at - ALPHABET);
    for (int j = 31; j >= 0; --j) {
      carry += 58u * bytes[j];
      bytes[j] = carry & 0xff;
      carry >>= 8;
    }
    if (carry) return false;  // more than 32 bytes
  }
  // Leading '1's are leading zero bytes; the number itself must fill the rest exactly.
  size_t lead = 0;
  while (lead < 32 && bytes[lead] == 0) ++lead;
  if (lead != zeros) return false;
  memcpy(out, bytes, 32);
  return true;
}

}  // namespace wallet_decode
