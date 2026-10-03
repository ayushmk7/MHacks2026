/* sol.h - Solana primitives the wallet core needs. Pure C99, no heap, no Arduino.
   Host-testable: cc -std=c99 -Wall -Wextra -DSOL_HOST_SHA256 sol_*.c test_sol.c
   On the badge SOL_HOST_SHA256 is NOT defined and sol_sha256() wraps mbedtls. */
#ifndef SOL_H
#define SOL_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

#define SOL_PUBKEY_LEN   32
#define SOL_SIG_LEN      64
#define SOL_B58_PUBKEY_MAX 45   /* 44 chars + NUL */
#define SOL_B58_SIG_MAX    89   /* 88 chars + NUL */
#define SOL_TX_MSG_MAX   256    /* largest message the wallet will look at */

/* ---- base58 (Bitcoin alphabet) ------------------------------------------ */
/* Returns chars written (excluding NUL), or 0 if out is too small. */
size_t sol_b58_encode(const uint8_t *in, size_t in_len, char *out, size_t out_cap);
/* Decodes exactly out_len bytes. Returns 0 on success, -1 on bad char / wrong length. */
int sol_b58_decode(const char *in, uint8_t *out, size_t out_len);

/* ---- sha256 -------------------------------------------------------------- */
typedef struct { const uint8_t *p; size_t n; } sol_slice_t;
void sol_sha256(const sol_slice_t *parts, size_t count, uint8_t out[32]);

/* ---- ed25519 curve membership (for program-derived addresses) ------------ */
/* 1 if the 32 bytes decompress to a point on edwards25519, else 0.
   Same answer as curve25519-dalek CompressedEdwardsY::decompress().is_some(). */
int sol_is_on_curve(const uint8_t p[32]);

/* ---- program-derived addresses ------------------------------------------- */
/* find_program_address: tries bump 255..0, returns 0 and fills out/bump on success. */
int sol_find_pda(const sol_slice_t *seeds, size_t seed_count, const uint8_t program_id[32],
                 uint8_t out[32], uint8_t *bump);
/* Associated token account for (owner, mint) under the classic Token program. */
int sol_ata(const uint8_t owner[32], const uint8_t mint[32], uint8_t out[32]);
/* SAS attestation PDA: seeds "attestation", credential, schema, nonce(subject pubkey). */
int sol_sas_attestation_pda(const uint8_t credential[32], const uint8_t schema[32],
                            const uint8_t subject[32], uint8_t out[32]);

extern const uint8_t SOL_TOKEN_PROGRAM_ID[32];   /* TokenkegQfeZyiNwAJbNbGKPFXCWuBvf9Ss623VQ5DA */
extern const uint8_t SOL_ATA_PROGRAM_ID[32];     /* ATokenGPvbdGVxr1b2hvZbsiqW5xWH25efTNsLJA8knL */
extern const uint8_t SOL_SAS_PROGRAM_ID[32];     /* 22zoJMtdu4tQc2PzL74ZUT7FrwgB1Udec8DdW4yw4BdG */

/* ---- transaction message decoder (structure only, no policy) ------------- */
typedef enum {
  SOL_TX_OK = 0,
  SOL_TX_ERR_TOO_LONG,        /* len > SOL_TX_MSG_MAX */
  SOL_TX_ERR_TRUNCATED,       /* ran out of bytes */
  SOL_TX_ERR_VERSION,         /* versioned message with version != 0 */
  SOL_TX_ERR_HEADER,          /* not exactly 1 required signature / readonly-signed != 0 */
  SOL_TX_ERR_ACCOUNTS,        /* account count != 5, or readonly-unsigned count != 2 */
  SOL_TX_ERR_LOOKUPS,         /* v0 message that uses address lookup tables */
  SOL_TX_ERR_IX_COUNT,        /* not exactly one instruction */
  SOL_TX_ERR_PROGRAM,         /* instruction program is not the SPL Token program */
  SOL_TX_ERR_IX_ACCOUNTS,     /* not exactly 4 instruction accounts, or an index out of range */
  SOL_TX_ERR_IX_DATA,         /* data is not 10 bytes starting with 12 (TransferChecked) */
  SOL_TX_ERR_AUTHORITY,       /* authority is not account 0 (the only signer / fee payer) */
  SOL_TX_ERR_ROLES,           /* source/destination not writable, mint/program not readonly, or aliasing */
  SOL_TX_ERR_AMOUNT_ZERO,     /* amount == 0 */
  SOL_TX_ERR_TRAILING         /* bytes left over after the message */
} sol_tx_err_t;

typedef struct {
  uint8_t  version;           /* 0xFF = legacy, 0 = v0 */
  uint8_t  fee_payer[32];     /* account 0; also the transfer authority */
  uint8_t  source[32];        /* token account debited */
  uint8_t  mint[32];
  uint8_t  destination[32];   /* token account credited */
  uint8_t  blockhash[32];
  uint64_t amount;            /* raw base units */
  uint8_t  decimals;
} sol_transfer_t;

/* Accepts ONLY: legacy or v0-without-lookups message, 1 signer, exactly 5 account keys,
   exactly 1 instruction = SPL Token TransferChecked with 4 accounts. Everything else is an
   error - the caller shows "Unknown instruction" and refuses to sign. */
sol_tx_err_t sol_tx_decode_transfer(const uint8_t *msg, size_t len, sol_transfer_t *out);
const char *sol_tx_err_name(sol_tx_err_t e);

/* Builds a legacy message for one TransferChecked (214 bytes).
   Account order: payer; the two token accounts in ascending raw-byte order; then mint and Token program
   in ascending raw-byte order. Guaranteed: the result is a valid Solana message and
   sol_tx_decode_transfer() accepts it and returns the same fields that were passed in.
   NOT guaranteed: byte equality with a message built by @solana/kit for the same inputs. kit orders
   keys inside each role class with a base58 text collation, which can differ from raw-byte order; the
   two are equal for vector V_LEGACY and differ for V_ALT_LEGACY (test_sol.c checks both). The order
   inside a role class has no effect on chain, and the decoder reads accounts through the instruction's
   indices, so it accepts either order.
   Returns the length, or 0 if cap < 214 or source == destination. */
size_t sol_tx_build_transfer(const uint8_t payer[32], const uint8_t source[32],
                             const uint8_t destination[32], const uint8_t mint[32],
                             const uint8_t blockhash[32], uint64_t amount, uint8_t decimals,
                             uint8_t *out, size_t cap);

/* "12.50" from (1250, 2). Returns chars written, 0 if cap too small. No float. */
size_t sol_format_amount(uint64_t raw, uint8_t decimals, char *out, size_t cap);
/* Parses "12.5" / "12" / "0.05" with at most `decimals` fraction digits. 0 on success. */
int sol_parse_amount(const char *text, uint8_t decimals, uint64_t *raw);

#ifdef __cplusplus
}
#endif
#endif
