/* src/wallet/wallet_internal.h - shared inside src/wallet/ only. Implemented in wallet.cpp, where
   wallet::Gate is defined. Gate's methods are public static members of a class whose definition exists
   only in wallet.cpp, so no other file can name them; these two functions are the only way out. */
#ifndef WALLET_INTERNAL_H
#define WALLET_INTERNAL_H
#include <stdint.h>
#include "wallet.h"
#ifdef __cplusplus
extern "C" {
#endif

/* Each checks the open session (pay_session_get) before asking the gate to sign, verifies the signature
   it produced, and writes one audit line.
   BADGE_OK, BADGE_ERR_NO_SESSION (no session, wrong request id, expired), BADGE_ERR_RATE_LIMITED,
   BADGE_ERR_SIGN_FAILED. */
badge_err_t wallet_internal_sign_proof(const uint8_t id[8], const uint8_t nonce[16], const uint8_t payer[32],
                                       uint8_t sig_out[64]);
badge_err_t wallet_internal_sign_receipt(const uint8_t id[8], const uint8_t tx_sig[64], uint8_t sig_out[64]);

#ifdef __cplusplus
}
#endif
#endif
