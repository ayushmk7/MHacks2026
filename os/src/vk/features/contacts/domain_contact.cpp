// Signing domain "contact": the signature inside a CONTACT_CARD frame (signing.md, "Domain table";
// protocol/espnow.md, "CONTACT_CARD").
//
// An auto domain: it signs with no button press, so its validator is the whole gate. It accepts
// only a well-formed card body, peer_nonce[16] ‖ peer_pubkey[32] ‖ own_pubkey[32] ‖ name_len[1] ‖
// name (82 to 113 bytes, the name 1..32 printable ASCII characters), whose own_pubkey is this
// badge's own key. So the badge can only ever sign "this is *my* card, for that badge and that
// swap", and the signed message always begins with the prefix "contact:".
//
// No app can hand the wallet core bytes for this domain: the only caller of
// signAuto("contact", ...) is vk::contacts::card (contacts.cpp), which builds the bytes from a
// HELLO frame and this badge's key and name.
#include <Arduino.h>

#include "../../wallet/pure/vk_frames.h"
#include "../../wallet/signer.h"
#include "contacts.h"

namespace {

bool validateContact(const uint8_t *bytes, size_t len) {
  return vk::contacts::validCardBytes(bytes, len, vk::wallet::publicKey());
}

}  // namespace

VK_SIGN_DOMAIN(contact, "contact", VK_PREFIX_CONTACT, false, nullptr, VK_CARD_SIGNED_MAX, nullptr, validateContact);
