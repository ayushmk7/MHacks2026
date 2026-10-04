// Host test for src/lua_sdk/wallet_decode.cpp against the R6 fuzz set (harness/fuzz-vectors.json).
// sign_proof cases are argument-type checks inside the Lua binding, so they are not covered here.
//
//   c++ -std=c++17 -O1 -fsanitize=address,undefined -I src/lua_sdk \
//       tools/wallet_decode_test.cpp src/lua_sdk/wallet_decode.cpp -o /tmp/wdt
//   node tools/fuzz-vectors-flat.mjs ../../harness/fuzz-vectors.json | /tmp/wdt
#include <stdio.h>
#include <string.h>

#include <string>
#include <vector>

#include "wallet_decode.h"

static std::vector<uint8_t> unhex(const std::string &h) {
  std::vector<uint8_t> out;
  for (size_t i = 0; i + 1 < h.size(); i += 2) out.push_back((uint8_t)std::stoi(h.substr(i, 2), nullptr, 16));
  return out;
}

int main() {
  char payerText[64], mintText[64];
  if (scanf("%63s %63s", payerText, mintText) != 2) return 2;
  uint8_t payer[32], mint[32];
  if (!wallet_decode::base58To32(payerText, payer) || !wallet_decode::base58To32(mintText, mint)) {
    fprintf(stderr, "bad payer/mint base58\n");
    return 2;
  }

  char id[32], fn[32], expect[16];
  static char hex[20000];
  int run = 0, failed = 0, skipped = 0;
  while (scanf("%31s %31s %15s %19999s", id, fn, expect, hex) == 4) {
    const std::vector<uint8_t> bytes = unhex(strcmp(hex, "-") == 0 ? "" : hex);
    // A copy on the heap of exactly the input size, so ASan catches any read past the end.
    uint8_t *buf = new uint8_t[bytes.size() + 1];
    memcpy(buf, bytes.data(), bytes.size());
    const char *why = nullptr;
    if (strcmp(fn, "begin_solana") == 0) why = wallet_decode::solana(buf, bytes.size(), payer, mint, 2, nullptr);
    else if (strcmp(fn, "begin_bank") == 0) why = wallet_decode::bank(buf, bytes.size(), nullptr);
    else if (strcmp(fn, "sign_request") == 0) why = wallet_decode::request(buf, bytes.size(), payer);
    else { ++skipped; delete[] buf; continue; }
    delete[] buf;
    ++run;
    const bool accepted = why == nullptr;
    const bool want = strcmp(expect, "refuse") != 0;
    if (accepted != want) {
      ++failed;
      printf("FAIL %s %s expect %s, decoder %s\n", id, fn, expect, accepted ? "accepted" : why);
    }
  }
  printf("%d cases, %d failed, %d skipped (sign_proof)\n", run, failed, skipped);
  return failed ? 1 : 0;
}
