#include "sol.h"
#include <string.h>

const uint8_t SOL_TOKEN_PROGRAM_ID[32] = {
  0x06,0xdd,0xf6,0xe1,0xd7,0x65,0xa1,0x93,0xd9,0xcb,0xe1,0x46,0xce,0xeb,0x79,0xac,
  0x1c,0xb4,0x85,0xed,0x5f,0x5b,0x37,0x91,0x3a,0x8c,0xf5,0x85,0x7e,0xff,0x00,0xa9};
const uint8_t SOL_ATA_PROGRAM_ID[32] = {
  0x8c,0x97,0x25,0x8f,0x4e,0x24,0x89,0xf1,0xbb,0x3d,0x10,0x29,0x14,0x8e,0x0d,0x83,
  0x0b,0x5a,0x13,0x99,0xda,0xff,0x10,0x84,0x04,0x8e,0x7b,0xd8,0xdb,0xe9,0xf8,0x59};
const uint8_t SOL_SAS_PROGRAM_ID[32] = {
  0x0f,0x5e,0x9e,0xd5,0x37,0x1e,0x2c,0x70,0x89,0x8c,0xa9,0xfd,0x0e,0x77,0xc0,0x06,
  0x5c,0xab,0x5d,0xa0,0x2e,0x56,0x67,0x8b,0x27,0x13,0x38,0x2a,0xf3,0x74,0x59,0xb7};

int sol_find_pda(const sol_slice_t *seeds, size_t seed_count, const uint8_t program_id[32],
                 uint8_t out[32], uint8_t *bump) {
  static const char MARKER[] = "ProgramDerivedAddress";
  sol_slice_t parts[8 + 3];
  uint8_t b;
  int i;
  size_t k;
  if (seed_count > 8) return -1;
  for (k = 0; k < seed_count; k++) { if (seeds[k].n > 32) return -1; parts[k] = seeds[k]; }
  parts[seed_count].p = &b;                       parts[seed_count].n = 1;
  parts[seed_count + 1].p = program_id;           parts[seed_count + 1].n = 32;
  parts[seed_count + 2].p = (const uint8_t *)MARKER; parts[seed_count + 2].n = sizeof MARKER - 1;
  for (i = 255; i >= 0; i--) {
    b = (uint8_t)i;
    sol_sha256(parts, seed_count + 3, out);
    if (!sol_is_on_curve(out)) { if (bump) *bump = b; return 0; }
  }
  return -1;
}

int sol_ata(const uint8_t owner[32], const uint8_t mint[32], uint8_t out[32]) {
  sol_slice_t seeds[3];
  seeds[0].p = owner;                seeds[0].n = 32;
  seeds[1].p = SOL_TOKEN_PROGRAM_ID; seeds[1].n = 32;
  seeds[2].p = mint;                 seeds[2].n = 32;
  return sol_find_pda(seeds, 3, SOL_ATA_PROGRAM_ID, out, NULL);
}

int sol_sas_attestation_pda(const uint8_t credential[32], const uint8_t schema[32],
                            const uint8_t subject[32], uint8_t out[32]) {
  sol_slice_t seeds[4];
  seeds[0].p = (const uint8_t *)"attestation"; seeds[0].n = 11;
  seeds[1].p = credential; seeds[1].n = 32;
  seeds[2].p = schema;     seeds[2].n = 32;
  seeds[3].p = subject;    seeds[3].n = 32;
  return sol_find_pda(seeds, 4, SOL_SAS_PROGRAM_ID, out, NULL);
}
