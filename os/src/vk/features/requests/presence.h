// Presence, payer side (protocol/espnow.md, "Payer: challenging and judging"). Private to
// features/requests.
//
// A table of 4 slots: req_id, payee_pubkey (from the REQ), mac, nonce, t0_ms, result. A challenge
// fills a slot and sends CHAL; the PROOF that comes back is judged against the slot. The approval
// reads the result through vk::wallet::presenceLookup, which this feature sets to lookup() at boot.
//
// Time, random bytes, the verifier, the radio and the config store are reached through
// vk::requests::hooks (requests.h), so the table is host-tested with fakes.
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "../../wallet/pure/vk_checks.h"   // vk_presence_t
#include "../../wallet/reason.h"

namespace vk::presence {

constexpr size_t SLOTS = 4;
// PROOFs checked per challenge (each check is one Ed25519 verification, ~18 ms). Within the deadline
// a burst this large overflows upstream's 7-frame receive queue anyway, so the cap adds no cheaper
// way to lose the real PROOF; it bounds what a flood costs the payer's loop.
constexpr uint32_t PROOF_CHECKS_MAX = 16;

// Parses the REQ, fills a slot (the one already holding that req_id, else a free one, else the
// oldest) with a fresh random nonce and result PENDING, sets t0 and sends CHAL to `mac`.
// VK_BAD_ARG: no mac, or the frame is not a REQ. VK_SIGN_FAILED: this badge has no key to name
// as the payer. VK_OK otherwise, whether or not the radio took the frame: a lost CHAL leaves the
// slot PENDING, and the app may challenge again.
vk::wallet::Reason challenge(const uint8_t mac[6], const uint8_t *req_frame, size_t len);

// Route for type 3 (PROOF). Judges the PENDING slot with that req_id whose mac is the sender's:
// valid and rx_ms - t0 <= presence_ms -> PRESENT; valid but slower -> LATE. The first valid PROOF
// decides. A PROOF whose signature does not verify (forged, or a recording of an earlier challenge)
// is ignored and counted: the slot stays PENDING, which the approval shows as amber if no valid
// PROOF comes. Nothing here stores BAD_SIG. Always consumes the frame.
bool onProof(const uint8_t mac[6], const uint8_t *frame, size_t len, uint32_t rx_ms);

// The signature of vk::wallet::presenceLookup. For any result other than NONE it also writes the
// payee key the slot holds and the nonce that was sent (each only when the pointer is not null).
vk_presence_t lookup(const uint8_t req_id[8], uint8_t payee_pubkey_out[32], uint8_t nonce_out[16]);

const char *resultName(vk_presence_t result);    // "none", "pending", "present", "late", "bad_sig"

// The CHAL -> PROOF time of the last PROOF that was judged valid (PRESENT or LATE), in ms.
uint32_t lastProofMs();

// PROOFs for that request's current challenge that were ignored: invalid signatures, and those over
// PROOF_CHECKS_MAX that were not checked. 0 for a request with no slot.
uint32_t badProofs(const uint8_t req_id[8]);

void reset();                                    // empties every slot

}  // namespace vk::presence
