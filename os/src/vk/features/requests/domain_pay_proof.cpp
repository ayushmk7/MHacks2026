// Signing domain "pay-proof": the signature inside a PROOF frame (signing.md, "Domain table";
// protocol/espnow.md, "PROOF", "Payee: answering CHAL").
//
// An auto domain. Its validator accepts exactly 56 bytes, req_id[8] ‖ nonce[16] ‖
// payer_pubkey[32], whose req_id is a request this badge has open right now. So the badge proves
// presence only for its own live requests, and the signed message always begins with the prefix
// "pay-proof:".
//
// No app can hand the wallet core bytes for this domain: the only caller of
// signAuto("pay-proof", ...) is vk::requests::onChal (requests.cpp), the route for CHAL frames,
// which builds the bytes from the challenge and applies the rate limits first.
#include <Arduino.h>

#include "../../wallet/pure/vk_frames.h"
#include "../../wallet/signer.h"
#include "requests.h"

namespace {

bool validatePayProof(const uint8_t *bytes, size_t len) {
  return vk::requests::validProofBytes(bytes, len);
}

}  // namespace

VK_SIGN_DOMAIN(pay_proof, "pay-proof", VK_PREFIX_PAY_PROOF, false, nullptr, VK_PROOF_SIGNED_LEN, nullptr, validatePayProof);
