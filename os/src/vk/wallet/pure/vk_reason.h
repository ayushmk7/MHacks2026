/* vk_reason.h - one enum for every refusal in Badge OS. Pure C99.
   Spec: docs/os/wallet/signing.md ("Reason codes"), docs/os/reference/reasons.md.
   The order is fixed: the numeric value is stored in the history file. New codes go at the end. */
#ifndef VK_REASON_H
#define VK_REASON_H
#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
  VK_OK = 0, VK_CANCELLED, VK_TIMEOUT, VK_UNDECODABLE, VK_UNVERIFIED, VK_REVOKED, VK_EXPIRED,
  VK_MISMATCH, VK_BAD_PROOF, VK_OVER_CAP, VK_NO_TIME, VK_BUSY, VK_DENIED, VK_NOT_PROVISIONED,
  VK_TOO_LONG, VK_SIGN_FAILED, VK_BAD_ARG, VK_UNSUPPORTED, VK_IDLE
} vk_reason_t;
const char *vk_reason_name(vk_reason_t r);   /* "ok", "cancelled", "timeout", ... lower-case, same order */

#define VK_REASON_COUNT 19                   /* number of codes; an out-of-range value is named "?" */

#ifdef __cplusplus
}
#endif
#endif
