// Signing domain "pay-req": the signature inside a REQ frame (signing.md, "Domain table";
// protocol/espnow.md, "REQ").
//
// An auto domain: it signs with no button press, so its validator is the whole gate. It accepts
// only a REQ frame from its header to the end of the name (63 to 94 bytes) that the strict codec
// parses and whose payee_pubkey is this badge's own key. So the badge can only ever sign "pay
// *me*", and the signed message always begins with the prefix "pay-req:".
//
// No app can hand the wallet core bytes for this domain: the only caller of
// signAuto("pay-req", ...) is vk::requests::openRequest (requests.cpp), which builds the bytes.
#include <Arduino.h>

#include "../../wallet/pure/vk_frames.h"
#include "../../wallet/signer.h"
#include "requests.h"

namespace {

bool validatePayReq(const uint8_t *bytes, size_t len) {
  return vk::requests::validReqBytes(bytes, len, vk::wallet::publicKey());
}

}  // namespace

VK_SIGN_DOMAIN(pay_req, "pay-req", VK_PREFIX_PAY_REQ, false, nullptr, VK_REQ_SIGNED_MAX, nullptr, validatePayReq);
