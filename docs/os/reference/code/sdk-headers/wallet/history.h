/* src/wallet/history.h - payment history ring (/wallet/history.bin, 50 records of 160 bytes = 8000 bytes). */
#ifndef HISTORY_H
#define HISTORY_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "wallet.h"
#ifdef __cplusplus
extern "C" {
#endif

#define HISTORY_RECORDS     50
#define HISTORY_RECORD_LEN 160

/* On-disk record, little-endian, fixed offsets (written field by field, not as a C struct):
     0    1  dir         0 out, 1 in
     1    1  status      0 signed, 1 submitted, 2 confirmed, 3 failed
     2    1  flags       bit0 verified, bit1 has_sig, bit2 receipt (F19 co-signed)
     3    1  name_len    0..32
     4   32  peer        counterparty public key
     36  32  name        zero padded
     68   8  amount      u64 LE, raw units
     76  64  sig         transaction signature (zero when has_sig is clear)
     140  4  uptime_s    u32 LE, seconds since boot when written
     144  2  boot_count  u16 LE, value of NVS wallet/boots at that boot
     146  4  seq         u32 LE, 1-based, never reused; 0 marks an empty slot
     150 10  reserved    zero
   Slot of a record = (seq - 1) % HISTORY_RECORDS. The newest record is the one with the highest seq,
   found by scanning all 50 slots at boot. */
#define HISTORY_F_VERIFIED 0x01
#define HISTORY_F_HAS_SIG  0x02
#define HISTORY_F_RECEIPT  0x04

typedef struct {
  uint8_t  dir, status, flags;
  char     name[33];
  uint8_t  peer[32];
  uint64_t amount;
  uint8_t  sig[64];
  uint32_t uptime_s;
  uint16_t boot_count;
  uint32_t seq;
} history_record_t;

void        history_begin(void);                                    /* also increments NVS wallet/boots */
uint16_t    history_boot_count(void);
badge_err_t history_append(const history_record_t *r);              /* seq, uptime_s, boot_count are filled in */
size_t      history_list(history_record_t *out, size_t max);        /* newest first */
badge_err_t history_set_status(const uint8_t sig[64], uint8_t status);   /* bad_arg if no record has that signature */
badge_err_t history_set_receipt(const uint8_t sig[64]);

#ifdef __cplusplus
}
#endif
#endif
