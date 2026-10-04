#include "vk_reason.h"

const char *vk_reason_name(vk_reason_t r) {
  static const char *const N[VK_REASON_COUNT] = {
    "ok", "cancelled", "timeout", "undecodable", "unverified", "revoked", "expired",
    "mismatch", "bad_proof", "over_cap", "no_time", "busy", "denied", "not_provisioned",
    "too_long", "sign_failed", "bad_arg", "unsupported", "idle"};
  return (unsigned)r < VK_REASON_COUNT ? N[(unsigned)r] : "?";
}
