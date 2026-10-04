// Presence, payer side: the slot table, the challenge, and the judging of a PROOF
// (protocol/espnow.md, "Payer: challenging and judging").
//
// The table logic reaches the clock, the random source, the verifier, the radio and the config
// store only through vk::requests::hooks, so test/host/test_requests.cpp drives it with fakes.
// The last part, compiled only for the badge, registers the PROOF route and sets
// vk::wallet::presenceLookup at boot.
#include "presence.h"

#include <string.h>

#include "requests.h"   // vk::requests::hooks

#ifndef VK_HOST_TEST
#include <Arduino.h>

#include "../../../badge_log.h"
#include "../../core/service.h"
#include "../../host/router.h"
#include "../../wallet/signer.h"   // vk::wallet::presenceLookup
#define VK_PRESENCE_LOG(...) badge_log::tagf("req", __VA_ARGS__)
#else
#define VK_PRESENCE_LOG(...) ((void)0)
#endif

namespace vk::presence {

namespace {

using vk::requests::hooks;

struct Slot {
  bool used;
  uint8_t req_id[8];
  uint8_t payee_pubkey[32];     // from the REQ: the key a PROOF must be signed with
  uint8_t mac[6];               // who was challenged
  uint8_t nonce[16];
  uint32_t t0_ms;               // when the CHAL was sent
  vk_presence_t result;
  uint32_t order;               // higher = challenged later; the lowest is "the oldest slot"
};
Slot sSlots[SLOTS];
uint32_t sOrder = 0;
uint32_t sLastProofMs = 0;

uint32_t nowMs() { return hooks.nowMs ? hooks.nowMs() : 0; }

Slot *findSlot(const uint8_t req_id[8]) {
  for (Slot &s : sSlots) {
    if (s.used && memcmp(s.req_id, req_id, 8) == 0) return &s;
  }
  return nullptr;
}

}  // namespace

vk::wallet::Reason challenge(const uint8_t mac[6], const uint8_t *req_frame, size_t len) {
  vk_req_t req;
  if (mac == nullptr || vk_req_parse(req_frame, len, &req) != 0) return VK_BAD_ARG;
  const uint8_t *own = hooks.ownKey ? hooks.ownKey() : nullptr;
  if (own == nullptr || hooks.randomBytes == nullptr) return VK_SIGN_FAILED;

  // The slot already holding this request (a fresh nonce replaces it), else a free one, else the oldest.
  Slot *slot = findSlot(req.req_id);
  if (slot == nullptr) {
    for (Slot &s : sSlots) {
      if (!s.used) { slot = &s; break; }
      if (slot == nullptr || s.order < slot->order) slot = &s;
    }
  }

  memset(slot, 0, sizeof *slot);
  slot->used = true;
  memcpy(slot->req_id, req.req_id, 8);
  memcpy(slot->payee_pubkey, req.payee_pubkey, 32);
  memcpy(slot->mac, mac, 6);
  hooks.randomBytes(slot->nonce, sizeof slot->nonce);
  slot->result = VK_PRESENCE_PENDING;
  slot->order = ++sOrder;

  vk_chal_t chal;
  memcpy(chal.req_id, slot->req_id, 8);
  memcpy(chal.nonce, slot->nonce, 16);
  memcpy(chal.payer_pubkey, own, 32);
  uint8_t out[VK_CHAL_LEN];
  const size_t n = vk_chal_build(&chal, out, sizeof out);

  slot->t0_ms = nowMs();
  if (n == 0 || hooks.sendFrame == nullptr || !hooks.sendFrame(mac, out, n)) VK_PRESENCE_LOG("chal not sent");
  return VK_OK;
}

bool onProof(const uint8_t mac[6], const uint8_t *frame, size_t len, uint32_t rx_ms) {
  vk_proof_t proof;
  if (mac == nullptr || vk_proof_parse(frame, len, &proof) != 0) return true;

  // The PENDING slot with that req_id whose mac is the sender's. A slot that has its result keeps
  // it: the first PROOF decides.
  Slot *slot = findSlot(proof.req_id);
  if (slot == nullptr || slot->result != VK_PRESENCE_PENDING) return true;
  if (memcmp(slot->mac, mac, 6) != 0) return true;

  // A frame the radio received before this challenge was sent cannot be its answer (it answers an
  // earlier challenge whose nonce a re-challenge has replaced). It is not judged.
  const uint32_t elapsed = rx_ms - slot->t0_ms;
  if ((int32_t)elapsed < 0) return true;

  const uint8_t *own = hooks.ownKey ? hooks.ownKey() : nullptr;
  if (own == nullptr) return true;

  // The signed message: "pay-proof:" ‖ req_id ‖ nonce ‖ own_pubkey.
  static const char kPrefix[] = VK_PREFIX_PAY_PROOF;
  const size_t prefixLen = sizeof kPrefix - 1;
  uint8_t message[sizeof kPrefix - 1 + VK_PROOF_SIGNED_LEN];
  memcpy(message, kPrefix, prefixLen);
  vk_proof_signed_bytes(slot->req_id, slot->nonce, own, message + prefixLen);

  const bool valid = hooks.verify != nullptr &&
                     hooks.verify(message, sizeof message, proof.sig, slot->payee_pubkey) == 1;
  if (!valid) {
    slot->result = VK_PRESENCE_BAD_SIG;
    VK_PRESENCE_LOG("proof bad signature");
    return true;
  }

  const uint32_t deadline = hooks.configU32 ? hooks.configU32("presence_ms") : 0;
  slot->result = elapsed <= deadline ? VK_PRESENCE_PRESENT : VK_PRESENCE_LATE;
  sLastProofMs = elapsed;
  VK_PRESENCE_LOG("proof %lu ms", (unsigned long)elapsed);     // measurement M1 reads this line
  return true;
}

vk_presence_t lookup(const uint8_t req_id[8], uint8_t payee_pubkey_out[32], uint8_t nonce_out[16]) {
  if (req_id == nullptr) return VK_PRESENCE_NONE;
  const Slot *slot = findSlot(req_id);
  if (slot == nullptr || slot->result == VK_PRESENCE_NONE) return VK_PRESENCE_NONE;
  if (payee_pubkey_out != nullptr) memcpy(payee_pubkey_out, slot->payee_pubkey, 32);
  if (nonce_out != nullptr) memcpy(nonce_out, slot->nonce, 16);
  return slot->result;
}

const char *resultName(vk_presence_t result) {
  switch (result) {
    case VK_PRESENCE_NONE:    return "none";
    case VK_PRESENCE_PENDING: return "pending";
    case VK_PRESENCE_PRESENT: return "present";
    case VK_PRESENCE_LATE:    return "late";
    case VK_PRESENCE_BAD_SIG: return "bad_sig";
  }
  return "none";
}

uint32_t lastProofMs() { return sLastProofMs; }

void reset() {
  memset(sSlots, 0, sizeof sSlots);
  sOrder = 0;
  sLastProofMs = 0;
}

// ---------------------------------------------------------------------------
// Firmware glue: the PROOF route, and the wallet core's presence lookup
// ---------------------------------------------------------------------------

#ifndef VK_HOST_TEST
namespace {

bool routeProof(const uint8_t mac[6], const uint8_t *frame, size_t len, int8_t rssi, uint32_t rx_ms) {
  (void)rssi;
  return onProof(mac, frame, len, rx_ms);
}

// Runs from vk::begin(), before the radios start and before any approval can be opened.
void presenceBegin() { vk::wallet::presenceLookup = lookup; }

VK_SERVICE(presence, presenceBegin, nullptr);
VK_ESPNOW_ROUTE(pay_proof, VK_T_PROOF, VK_T_PROOF, routeProof);

}  // namespace
#endif

}  // namespace vk::presence
