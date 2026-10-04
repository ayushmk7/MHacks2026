/* sol.h - Solana primitives the wallet core needs. Pure C99, no heap, no Arduino.
   Spec: docs/os/wallet/solana-payments.md.
   Host-testable: test/host/run.sh test_sol (compiled with -DSOL_HOST_SHA256).
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
#define SOL_TX_MSG_MAX 1232     /* largest message the wallet will look at */

/* ---- base58 (Bitcoin alphabet) ------------------------------------------ */
/* Returns chars written (excluding NUL), or 0 if out is too small or in_len > 64. */
size_t sol_b58_encode(const uint8_t *in, size_t in_len, char *out, size_t out_cap);
/* Decodes exactly out_len bytes (out_len <= 64). Returns 0 on success, -1 on bad char / wrong length. */
int sol_b58_decode(const char *in, uint8_t *out, size_t out_len);

/* ---- sha256 -------------------------------------------------------------- */
typedef struct { const uint8_t *p; size_t n; } sol_slice_t;
void sol_sha256(const sol_slice_t *parts, size_t count, uint8_t out[32]);

extern const uint8_t SOL_TOKEN_PROGRAM_ID[32];   /* TokenkegQfeZyiNwAJbNbGKPFXCWuBvf9Ss623VQ5DA */
extern const uint8_t SOL_MEMO_PROGRAM_ID[32];    /* MemoSq4gqABAXKb96qnH8TysNcWxMyWCqXgDLGmfcHr */

/* ---- transaction message decoder (structure only, no policy) ------------- */
typedef enum {
  SOL_TX_OK = 0, SOL_TX_ERR_TOO_LONG, SOL_TX_ERR_TRUNCATED, SOL_TX_ERR_VERSION, SOL_TX_ERR_HEADER,
  SOL_TX_ERR_ACCOUNTS, SOL_TX_ERR_IX_COUNT, SOL_TX_ERR_PROGRAM, SOL_TX_ERR_IX_ACCOUNTS,
  SOL_TX_ERR_IX_DATA, SOL_TX_ERR_AUTHORITY, SOL_TX_ERR_ROLES, SOL_TX_ERR_AMOUNT_ZERO,
  SOL_TX_ERR_MEMO, SOL_TX_ERR_TRAILING
} sol_tx_err_t;                       /* SOL_TX_ERR_LOOKUPS is gone: every versioned message is ERR_VERSION */

typedef struct {
  uint8_t  fee_payer[32];             /* account 0; also the transfer authority */
  uint8_t  source[32];
  uint8_t  mint[32];
  uint8_t  destination[32];           /* the token account credited */
  uint8_t  blockhash[32];
  uint64_t amount;                    /* raw base units */
  uint8_t  decimals;
  const uint8_t *memo;                /* points into msg; NULL when there is no Memo */
  size_t   memo_len;
} sol_transfer_t;                     /* the `version` field is removed */

/* Accepts ONLY a legacy message of 1..SOL_TX_MSG_MAX bytes with one signer and either
     - 5 account keys and 1 instruction: SPL Token TransferChecked, or
     - 6 account keys and 2 instructions: that transfer and one Memo (either order).
   The twelve rules are tabulated in solana-payments.md ("Decoder rules"). Everything else is an
   error: the caller shows "CANNOT READ PAYMENT" and refuses to sign.
   On any error the contents of *out are unspecified. */
sol_tx_err_t sol_tx_decode_transfer(const uint8_t *msg, size_t len, sol_transfer_t *out);
const char *sol_tx_err_name(sol_tx_err_t e);

/* Legacy message for one TransferChecked, optionally followed by one Memo.
   Key order: payer; source and destination in ascending raw-byte order; then the readonly keys
   (mint, Token program, and the Memo program if memo_len > 0) in ascending raw-byte order.
   Returns the length, or 0 if cap is too small, source == destination, or memo is not valid UTF-8.
   Guarantee: sol_tx_decode_transfer() accepts the result and returns the same fields.
   To keep that guarantee it also returns 0 when amount == 0 or the message would exceed
   SOL_TX_MSG_MAX. Without a memo the result is 214 bytes; with one it is 248 + the length prefix
   (1 or 2 bytes) + memo_len. */
size_t sol_tx_build_transfer(const uint8_t payer[32], const uint8_t source[32],
                             const uint8_t destination[32], const uint8_t mint[32],
                             const uint8_t blockhash[32], uint64_t amount, uint8_t decimals,
                             const uint8_t *memo, size_t memo_len,
                             uint8_t *out, size_t cap);

/* "12.50" from (1250, 2). Returns chars written, 0 if cap too small. No float. */
size_t sol_format_amount(uint64_t raw, uint8_t decimals, char *out, size_t cap);   /* 1250, 2 -> "12.50" */
/* Parses "12.5" / "12" / "0.05" with at most `decimals` fraction digits. 0 on success. */
int    sol_parse_amount(const char *text, uint8_t decimals, uint64_t *raw);        /* "12.5", 2 -> 1250; 0 on success */

#ifdef __cplusplus
}
#endif
#endif
