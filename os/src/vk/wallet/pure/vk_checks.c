/* vk_checks.c - the check chain: message + record + request + presence + clock -> verdict.
   Spec: docs/os/wallet/checks.md ("The check chain"). No side effects, no hardware. */
#include "vk_checks.h"
#include <string.h>

const char *vk_headline_text(vk_headline_t h) {
  static const char *const T[] = {
    "VERIFIED - PRESENT", "VERIFIED - NOT PRESENT", "CLOCK UNSYNCED",
    "CANNOT READ PAYMENT", "UNKNOWN TOKEN", "UNVERIFIED RECIPIENT", "REVOKED", "EXPIRED", "STALE RECORD",
    "WRONG RECIPIENT", "WRONG AMOUNT", "BAD REQUEST", "BAD PROOF",
    "OVER LIMIT"};
  return (unsigned)h < sizeof T / sizeof T[0] ? T[(unsigned)h] : "?";
}

/* A failed check: red, SELECT disabled. */
static void fail(vk_verdict_t *out, vk_headline_t headline, vk_reason_t reason, int dev_overridable) {
  out->severity = VK_SEV_RED;
  out->select = VK_SEL_DISABLED;
  out->headline = headline;
  out->reason = reason;
  out->dev_overridable = dev_overridable;
}

/* Check 11: the request parses, is signed (domain pay-req) by the record's device key, names that
   key as the payee, is for the Solana rail and has not expired. */
static int request_ok(const vk_check_input_t *in, vk_verdict_t *out) {
  static const char PREFIX[] = VK_PREFIX_PAY_REQ;
  uint8_t signed_bytes[sizeof PREFIX - 1 + VK_REQ_SIGNED_MAX];
  if (vk_req_parse(in->req, in->req_len, &out->req) != 0) return 0;
  if (!in->verify || out->req.signed_len > VK_REQ_SIGNED_MAX) return 0;
  memcpy(signed_bytes, PREFIX, sizeof PREFIX - 1);
  memcpy(signed_bytes + sizeof PREFIX - 1, in->req, out->req.signed_len);
  if (in->verify(signed_bytes, sizeof PREFIX - 1 + out->req.signed_len, out->req.sig, out->record.device_pubkey) != 1) return 0;
  if (memcmp(out->req.payee_pubkey, out->record.device_pubkey, 32) != 0) return 0;
  if (out->req.rail != VK_RAIL_SOLANA) return 0;
  if (!(out->req.expiry > in->now)) return 0;
  return 1;
}

void vk_check_solana(const vk_check_input_t *in, vk_verdict_t *out) {
  const vk_token_t *token = NULL;
  int has_request;
  size_t i;

  if (!out) return;
  memset(out, 0, sizeof *out);
  out->presence = VK_PRESENCE_NONE;
  out->tx_err = SOL_TX_ERR_TRUNCATED;
  fail(out, VK_HL_CANNOT_READ, VK_UNDECODABLE, 0);
  if (!in) return;

  /* 1: the message decodes and account 0 is this badge */
  out->tx_err = sol_tx_decode_transfer(in->msg, in->msg_len, &out->transfer);
  if (out->tx_err != SOL_TX_OK) return;
  if (!in->own_pubkey || memcmp(out->transfer.fee_payer, in->own_pubkey, 32) != 0) return;

  /* 2: the mint is in the token table and the decimals match */
  for (i = 0; in->tokens && i < in->token_count; i++)
    if (memcmp(in->tokens[i].mint, out->transfer.mint, 32) == 0) { token = &in->tokens[i]; break; }
  if (!token || token->decimals != out->transfer.decimals) { fail(out, VK_HL_UNKNOWN_TOKEN, VK_UNDECODABLE, 0); return; }
  out->decoded = 1;
  out->token = token;

  /* 3: amount <= max */
  if (token->max != 0 && out->transfer.amount > token->max) { fail(out, VK_HL_OVER_LIMIT, VK_OVER_CAP, 0); return; }

  /* 4: a record and its signature were supplied (the one failure a dev build may override) */
  if (!in->record || in->record_len == 0 || !in->record_sig) { fail(out, VK_HL_UNVERIFIED, VK_UNVERIFIED, 1); return; }

  /* 5: the record parses and the issuer signature is valid */
  if (vk_record_parse(in->record, in->record_len, &out->record) != 0 ||
      vk_record_verify(in->record, in->record_len, in->record_sig, in->issuer_key, in->verify) != 1) {
    fail(out, VK_HL_UNVERIFIED, VK_UNVERIFIED, 0); return;
  }
  out->record_ok = 1;

  /* 6: status is active */
  if (out->record.status != VK_STATUS_ACTIVE) { fail(out, VK_HL_REVOKED, VK_REVOKED, 0); return; }

  /* (there is no check 7) */

  /* 8: expiry > now */
  if (!(out->record.expiry > in->now)) { fail(out, VK_HL_EXPIRED, VK_EXPIRED, 0); return; }

  /* 9: under SNTP only: now - issued_at <= record_ttl_s, and issued_at <= now + 60 */
  if (in->time_source == VK_TIME_SNTP) {
    const uint32_t issued = out->record.issued_at;
    const int stale = issued > in->now ? (issued - in->now > 60) : (in->now - issued > in->record_ttl_s);
    if (stale) { fail(out, VK_HL_STALE, VK_EXPIRED, 0); return; }
  }

  /* 10: the record has solana_ata and it equals the decoded destination */
  if (!out->record.has_solana || memcmp(out->record.solana_ata, out->transfer.destination, 32) != 0) {
    fail(out, VK_HL_WRONG_RECIPIENT, VK_MISMATCH, 0); return;
  }

  has_request = in->req != NULL && in->req_len != 0;
  if (has_request) {
    /* 11 */
    if (!request_ok(in, out)) { fail(out, VK_HL_BAD_REQUEST, VK_UNVERIFIED, 0); return; }
    out->req_ok = 1;

    /* 12: the request's currency is the token's symbol and its amount is the decoded amount */
    if (strcmp(out->req.currency, token->symbol) != 0 || out->req.amount != out->transfer.amount) {
      fail(out, VK_HL_WRONG_AMOUNT, VK_MISMATCH, 0); return;
    }

    /* 13: a judged proof (PRESENT, LATE, BAD_SIG) must be good and made for this record's key */
    if (in->presence) {
      uint8_t payee[32], nonce[16];
      memset(payee, 0, sizeof payee);
      memset(nonce, 0, sizeof nonce);
      out->presence = in->presence(out->req.req_id, payee, nonce);
      if (out->presence == VK_PRESENCE_PRESENT || out->presence == VK_PRESENCE_LATE || out->presence == VK_PRESENCE_BAD_SIG) {
        if (out->presence == VK_PRESENCE_BAD_SIG || memcmp(payee, out->record.device_pubkey, 32) != 0) {
          fail(out, VK_HL_BAD_PROOF, VK_BAD_PROOF, 0); return;
        }
      }
    }
  }

  /* Nothing failed: the first row that applies. */
  out->reason = VK_OK;
  out->dev_overridable = 0;
  if (in->time_source != VK_TIME_SNTP) {
    out->severity = VK_SEV_AMBER; out->headline = VK_HL_CLOCK_UNSYNCED; out->select = VK_SEL_HOLD;
  } else if (!has_request || out->presence != VK_PRESENCE_PRESENT) {
    out->severity = VK_SEV_AMBER; out->headline = VK_HL_NOT_PRESENT; out->select = VK_SEL_HOLD;
  } else {
    out->severity = VK_SEV_GREEN; out->headline = VK_HL_VERIFIED_PRESENT; out->select = VK_SEL_PRESS;
  }

  /* The cap: above it SELECT is a hold, whatever the colour. */
  if (token->cap != 0 && out->transfer.amount > token->cap) out->select = VK_SEL_HOLD;
}
