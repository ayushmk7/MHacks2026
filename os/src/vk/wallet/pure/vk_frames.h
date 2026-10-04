/* vk_frames.h - codec for the Badge OS ESP-NOW frames. Pure C99, no heap.
   Spec: docs/os/protocol/espnow.md ("Frame header", "Frames", "Codec").

   Every frame starts with the 4-byte header 'V' 'K' 1 <type>. Integers are little-endian.
   Parsers are strict: exact length, name_len in range (1..32), printable ASCII names, known rail.
   A build function returns the frame length, or 0 if cap is too small or a field is one the
   matching parser would refuse; so parse(build(x)) always succeeds and build(parse(f)) == f. */
#ifndef VK_FRAMES_H
#define VK_FRAMES_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

/* src/vk/wallet/pure/vk_frames.h — every function is pure; parse returns 0 on success */
enum { VK_T_REQ = 1, VK_T_CHAL = 2, VK_T_PROOF = 3, VK_T_RESULT = 4, VK_T_CONTACT_HELLO = 16, VK_T_CONTACT_CARD = 17 };

int vk_frame_type(const uint8_t *frame, size_t len);          /* type, or -1 if not a VK v1 frame */

typedef struct {
  uint8_t rail; uint8_t payee_pubkey[32]; uint64_t amount; char currency[5];
  uint8_t req_id[8]; uint32_t expiry; uint8_t name_len; char name[33]; uint8_t sig[64];
  size_t signed_len;                                           /* bytes of the frame the signature covers */
} vk_req_t;
int    vk_req_parse(const uint8_t *frame, size_t len, vk_req_t *out);
size_t vk_req_build(const vk_req_t *in, uint8_t *out, size_t cap);     /* writes all fields incl. in->sig */

typedef struct { uint8_t req_id[8]; uint8_t nonce[16]; uint8_t payer_pubkey[32]; } vk_chal_t;
int    vk_chal_parse(const uint8_t *frame, size_t len, vk_chal_t *out);
size_t vk_chal_build(const vk_chal_t *in, uint8_t *out, size_t cap);

typedef struct { uint8_t req_id[8]; uint8_t sig[64]; } vk_proof_t;
int    vk_proof_parse(const uint8_t *frame, size_t len, vk_proof_t *out);
size_t vk_proof_build(const vk_proof_t *in, uint8_t *out, size_t cap);
size_t vk_proof_signed_bytes(const uint8_t req_id[8], const uint8_t nonce[16], const uint8_t payer[32], uint8_t out[56]);

typedef struct { uint8_t req_id[8]; uint8_t status; uint8_t ref[64]; } vk_result_t;
int    vk_result_parse(const uint8_t *frame, size_t len, vk_result_t *out);
size_t vk_result_build(const vk_result_t *in, uint8_t *out, size_t cap);

typedef struct { uint8_t pubkey[32]; uint8_t nonce[16]; uint8_t name_len; char name[33]; } vk_hello_t;
int    vk_hello_parse(const uint8_t *frame, size_t len, vk_hello_t *out);
size_t vk_hello_build(const vk_hello_t *in, uint8_t *out, size_t cap);

typedef struct { uint8_t peer_pubkey[32]; uint8_t peer_nonce[16]; uint8_t pubkey[32]; uint8_t name_len; char name[33]; uint8_t sig[64]; } vk_card_t;
int    vk_card_parse(const uint8_t *frame, size_t len, vk_card_t *out);
size_t vk_card_build(const vk_card_t *in, uint8_t *out, size_t cap);
size_t vk_card_signed_bytes(const vk_card_t *card, uint8_t out[113]);

/* ---- constants (not in the spec block; values are from the frame tables in espnow.md) ---- */
#define VK_FRAME_MAX      240    /* largest ESP-NOW app payload upstream delivers */
#define VK_FRAME_HEADER   4
#define VK_NAME_MAX       32
#define VK_RAIL_SOLANA    1
#define VK_RAIL_BANK      2
#define VK_REQ_MIN_LEN    127    /* name_len 1 */
#define VK_REQ_MAX_LEN    158    /* name_len 32 */
#define VK_REQ_SIGNED_MAX 94     /* 62 + 32: header up to the end of the name */
#define VK_CHAL_LEN       60
#define VK_PROOF_LEN      76
#define VK_PROOF_SIGNED_LEN 56
#define VK_RESULT_LEN     77
#define VK_RESULT_OK       0
#define VK_RESULT_REJECTED 1
#define VK_RESULT_FAILED   2
#define VK_HELLO_MAX_LEN  85     /* 53 + 32 */
#define VK_CARD_MAX_LEN   181    /* 85 + 32 + 64 */
#define VK_CARD_SIGNED_MAX 113

/* Signing-domain prefixes of the frame signatures (signing.md, the domain table). The signed
   message is the prefix followed by the bytes named in the frame table:
     REQ    VK_PREFIX_PAY_REQ   || frame[0 .. signed_len)
     PROOF  VK_PREFIX_PAY_PROOF || vk_proof_signed_bytes(...)
     CARD   VK_PREFIX_CONTACT   || vk_card_signed_bytes(...)            */
#define VK_PREFIX_PAY_REQ   "pay-req:"
#define VK_PREFIX_PAY_PROOF "pay-proof:"
#define VK_PREFIX_CONTACT   "contact:"

#ifdef __cplusplus
}
#endif
#endif
