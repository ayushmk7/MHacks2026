/* vk_record.h - the issuer-signed registry record. Pure C99, no heap.
   Spec: docs/os/wallet/checks.md ("Registry record").

   Canonical bytes: UTF-8, key=value lines separated by '\n', exactly this order, no trailing
   newline, at most 512 bytes:
     v=1
     attestation=<base58 address, or "none">
     display_name=<1..32 printable ASCII>
     device_pubkey=<64 lower-case hex>
     kind=merchant|person
     solana_wallet=<64 hex, or empty>
     solana_ata=<64 hex, or empty>
     bank_ref_hash=<64 hex, or empty>
     expiry=<unix seconds>
     status=active|revoked
     issued_at=<unix seconds>
   Signed bytes: "registry:" followed by the canonical bytes. */
#ifndef VK_RECORD_H
#define VK_RECORD_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

#define VK_RECORD_MAX 512                 /* largest canonical record, in bytes */
#define VK_RECORD_PREFIX "registry:"      /* reserved: signed by the issuer, never by a badge */

typedef struct {
  char     attestation[45];
  char     display_name[33];
  uint8_t  device_pubkey[32];
  uint8_t  kind;                 /* 1 merchant, 2 person */
  uint8_t  has_solana;           /* solana_wallet and solana_ata present */
  uint8_t  solana_wallet[32];
  uint8_t  solana_ata[32];       /* the recipient's token account for the payment token */
  uint8_t  has_bank;
  uint8_t  bank_ref_hash[32];
  uint32_t expiry;
  uint8_t  status;               /* 1 active, 2 revoked */
  uint32_t issued_at;
} vk_record_t;

#define VK_KIND_MERCHANT 1
#define VK_KIND_PERSON   2
#define VK_STATUS_ACTIVE  1
#define VK_STATUS_REVOKED 2

/* 0 on success. Strict: wrong key, wrong order, extra line, bad hex, bad number, or non-printable name fails.
   Hex is lower-case only. Numbers are 1..10 decimal digits with no sign and no leading zero, and fit
   a u32. `attestation` is "none" or a base58 string that decodes to 32 bytes. has_solana is set only
   when both solana_wallet and solana_ata are present; when it is 0 both arrays are zero. */
int vk_record_parse(const uint8_t *bytes, size_t len, vk_record_t *out);

/* 1 if sig is a valid signature by issuer_key over "registry:" || bytes. */
int vk_record_verify(const uint8_t *bytes, size_t len, const uint8_t sig[64], const uint8_t issuer_key[32],
                     int (*verify)(const uint8_t *, size_t, const uint8_t *, const uint8_t *));

#ifdef __cplusplus
}
#endif
#endif
