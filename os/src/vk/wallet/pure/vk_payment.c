/* vk_payment.c - the request memo and the payee's check of a fetched transaction.
   Spec: docs/os/wallet/solana-payments.md ("Request memo", "Checking a received payment"). */
#include "vk_payment.h"
#include <string.h>

static const char HEX_DIGITS[] = "0123456789abcdef";

void vk_req_memo(const uint8_t req_id[8], char out[VK_REQ_MEMO_LEN + 1]) {
  size_t i;
  for (i = 0; i < 8; i++) {
    out[2 * i] = HEX_DIGITS[req_id[i] >> 4];
    out[2 * i + 1] = HEX_DIGITS[req_id[i] & 0x0F];
  }
  out[VK_REQ_MEMO_LEN] = '\0';
}

int vk_memo_is_req(const uint8_t *memo, size_t len, const uint8_t req_id[8]) {
  char want[VK_REQ_MEMO_LEN + 1];
  if (!memo || !req_id || len != VK_REQ_MEMO_LEN) return 0;
  vk_req_memo(req_id, want);
  return memcmp(memo, want, VK_REQ_MEMO_LEN) == 0;
}

static int nibble(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

int vk_req_id_parse(const char *text, size_t len, uint8_t out[8]) {
  uint8_t id[8]; size_t i;
  if (!text || !out || len != VK_REQ_MEMO_LEN) return -1;
  for (i = 0; i < 8; i++) {
    const int hi = nibble(text[2 * i]), lo = nibble(text[2 * i + 1]);
    if (hi < 0 || lo < 0) return -1;
    id[i] = (uint8_t)(hi << 4 | lo);
  }
  memcpy(out, id, 8);
  return 0;
}

int vk_wire_split(const uint8_t *tx, size_t len, const uint8_t **sig, const uint8_t **msg, size_t *msg_len) {
  /* 0x01 is the only (shortest) encoding of the count 1; 0x81 0x00 and the like are refused. */
  if (!tx || len < 1 + SOL_SIG_LEN + 1 || tx[0] != 0x01) return -1;
  if (sig) *sig = tx + 1;
  if (msg) *msg = tx + 1 + SOL_SIG_LEN;
  if (msg_len) *msg_len = len - 1 - SOL_SIG_LEN;
  return 0;
}

vk_pay_err_t vk_payment_verify(const uint8_t *tx, size_t len, const vk_pay_expect_t *expect, vk_pay_seen_t *seen) {
  const uint8_t *sig = NULL, *msg = NULL; size_t msg_len = 0;
  sol_transfer_t t;

  if (!expect || !expect->to || !expect->mint || !expect->req_id || expect->amount == 0) return VK_PAY_ERR_ARG;
  if (vk_wire_split(tx, len, &sig, &msg, &msg_len) != 0) return VK_PAY_ERR_WIRE;
  if (sol_tx_decode_transfer(msg, msg_len, &t) != SOL_TX_OK) return VK_PAY_ERR_SHAPE;
  if (seen) { seen->transfer = t; memcpy(seen->sig, sig, SOL_SIG_LEN); }

  if (expect->verify && expect->verify(msg, msg_len, sig, t.fee_payer) != 1) return VK_PAY_ERR_SIGNATURE;
  if (expect->sig && memcmp(expect->sig, sig, SOL_SIG_LEN) != 0) return VK_PAY_ERR_SIG_MISMATCH;
  if (expect->payer && memcmp(expect->payer, t.fee_payer, SOL_PUBKEY_LEN) != 0) return VK_PAY_ERR_PAYER;
  if (memcmp(expect->mint, t.mint, SOL_PUBKEY_LEN) != 0 || expect->decimals != t.decimals) return VK_PAY_ERR_MINT;
  if (memcmp(expect->to, t.destination, SOL_PUBKEY_LEN) != 0) return VK_PAY_ERR_RECIPIENT;
  if (expect->amount != t.amount) return VK_PAY_ERR_AMOUNT;
  if (!vk_memo_is_req(t.memo, t.memo_len, expect->req_id)) return VK_PAY_ERR_MEMO;
  return VK_PAY_OK;
}

const char *vk_pay_err_name(vk_pay_err_t e) {
  static const char *const N[] = {"ok", "arg", "wire", "shape", "signature", "sig", "payer", "mint",
                                  "recipient", "amount", "memo"};
  return (unsigned)e < sizeof N / sizeof N[0] ? N[(unsigned)e] : "?";
}

vk_reason_t vk_pay_reason(vk_pay_err_t e) {
  switch (e) {
    case VK_PAY_OK:            return VK_OK;
    case VK_PAY_ERR_ARG:       return VK_BAD_ARG;
    case VK_PAY_ERR_WIRE:
    case VK_PAY_ERR_SHAPE:     return VK_UNDECODABLE;
    case VK_PAY_ERR_SIGNATURE: return VK_BAD_PROOF;
    default:                   return VK_MISMATCH;
  }
}
