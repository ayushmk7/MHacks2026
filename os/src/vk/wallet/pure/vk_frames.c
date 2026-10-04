/* vk_frames.c - codec for the Badge OS ESP-NOW frames. Spec: docs/os/protocol/espnow.md. */
#include "vk_frames.h"
#include <string.h>

/* ---- helpers ---------------------------------------------------------------------------- */
static void put_header(uint8_t *out, uint8_t type) { out[0] = 'V'; out[1] = 'K'; out[2] = 1; out[3] = type; }

static int printable(const uint8_t *s, size_t n) {
  size_t i;
  for (i = 0; i < n; i++) if (s[i] < 0x20 || s[i] > 0x7E) return 0;
  return 1;
}
/* name_len 1..32 and every byte printable ASCII */
static int name_ok(const uint8_t *name, size_t name_len) {
  return name_len >= 1 && name_len <= VK_NAME_MAX && printable(name, name_len);
}
/* The 4-byte currency field: 1..4 printable ASCII characters, then NULs only. Returns the number of
   characters, or 0 if the field is not in that form. */
static size_t currency_len(const uint8_t field[4]) {
  size_t n = 0, i;
  while (n < 4 && field[n] != 0) n++;
  for (i = n; i < 4; i++) if (field[i] != 0) return 0;
  return printable(field, n) ? n : 0;
}
static void put_u32(uint8_t *p, uint32_t v) { int i; for (i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i)); }
static void put_u64(uint8_t *p, uint64_t v) { int i; for (i = 0; i < 8; i++) p[i] = (uint8_t)(v >> (8 * i)); }
static uint32_t get_u32(const uint8_t *p) { uint32_t v = 0; int i; for (i = 0; i < 4; i++) v |= (uint32_t)p[i] << (8 * i); return v; }
static uint64_t get_u64(const uint8_t *p) { uint64_t v = 0; int i; for (i = 0; i < 8; i++) v |= (uint64_t)p[i] << (8 * i); return v; }

int vk_frame_type(const uint8_t *frame, size_t len) {
  if (!frame || len < VK_FRAME_HEADER || frame[0] != 'V' || frame[1] != 'K' || frame[2] != 1) return -1;
  return frame[3];
}

/* ---- REQ: header, rail@4, payee_pubkey@5, amount@37, currency@45, req_id@49, expiry@57,
        name_len@61, name@62, sig@62+name_len ------------------------------------------------ */
int vk_req_parse(const uint8_t *frame, size_t len, vk_req_t *out) {
  size_t name_len, cur_len;
  if (!out || vk_frame_type(frame, len) != VK_T_REQ || len < VK_REQ_MIN_LEN || len > VK_REQ_MAX_LEN) return -1;
  name_len = frame[61];
  if (len != 62 + name_len + 64 || !name_ok(frame + 62, name_len)) return -1;
  if (frame[4] != VK_RAIL_SOLANA && frame[4] != VK_RAIL_BANK) return -1;
  cur_len = currency_len(frame + 45);
  if (cur_len == 0) return -1;
  memset(out, 0, sizeof *out);
  out->rail = frame[4];
  memcpy(out->payee_pubkey, frame + 5, 32);
  out->amount = get_u64(frame + 37);
  memcpy(out->currency, frame + 45, cur_len);
  memcpy(out->req_id, frame + 49, 8);
  out->expiry = get_u32(frame + 57);
  out->name_len = (uint8_t)name_len;
  memcpy(out->name, frame + 62, name_len);
  memcpy(out->sig, frame + 62 + name_len, 64);
  out->signed_len = 62 + name_len;
  return 0;
}

size_t vk_req_build(const vk_req_t *in, uint8_t *out, size_t cap) {
  size_t cur_len = 0, total;
  if (!in || !out) return 0;
  if (in->rail != VK_RAIL_SOLANA && in->rail != VK_RAIL_BANK) return 0;
  if (!name_ok((const uint8_t *)in->name, in->name_len)) return 0;
  while (cur_len < 5 && in->currency[cur_len] != 0) cur_len++;
  if (cur_len < 1 || cur_len > 4 || !printable((const uint8_t *)in->currency, cur_len)) return 0;
  total = 62 + (size_t)in->name_len + 64;
  if (cap < total) return 0;
  put_header(out, VK_T_REQ);
  out[4] = in->rail;
  memcpy(out + 5, in->payee_pubkey, 32);
  put_u64(out + 37, in->amount);
  memset(out + 45, 0, 4);
  memcpy(out + 45, in->currency, cur_len);
  memcpy(out + 49, in->req_id, 8);
  put_u32(out + 57, in->expiry);
  out[61] = in->name_len;
  memcpy(out + 62, in->name, in->name_len);
  memcpy(out + 62 + in->name_len, in->sig, 64);
  return total;
}

/* ---- CHAL: header, req_id@4, nonce@12, payer_pubkey@28 ---------------------------------- */
int vk_chal_parse(const uint8_t *frame, size_t len, vk_chal_t *out) {
  if (!out || vk_frame_type(frame, len) != VK_T_CHAL || len != VK_CHAL_LEN) return -1;
  memcpy(out->req_id, frame + 4, 8);
  memcpy(out->nonce, frame + 12, 16);
  memcpy(out->payer_pubkey, frame + 28, 32);
  return 0;
}

size_t vk_chal_build(const vk_chal_t *in, uint8_t *out, size_t cap) {
  if (!in || !out || cap < VK_CHAL_LEN) return 0;
  put_header(out, VK_T_CHAL);
  memcpy(out + 4, in->req_id, 8);
  memcpy(out + 12, in->nonce, 16);
  memcpy(out + 28, in->payer_pubkey, 32);
  return VK_CHAL_LEN;
}

/* ---- PROOF: header, req_id@4, sig@12 ---------------------------------------------------- */
int vk_proof_parse(const uint8_t *frame, size_t len, vk_proof_t *out) {
  if (!out || vk_frame_type(frame, len) != VK_T_PROOF || len != VK_PROOF_LEN) return -1;
  memcpy(out->req_id, frame + 4, 8);
  memcpy(out->sig, frame + 12, 64);
  return 0;
}

size_t vk_proof_build(const vk_proof_t *in, uint8_t *out, size_t cap) {
  if (!in || !out || cap < VK_PROOF_LEN) return 0;
  put_header(out, VK_T_PROOF);
  memcpy(out + 4, in->req_id, 8);
  memcpy(out + 12, in->sig, 64);
  return VK_PROOF_LEN;
}

size_t vk_proof_signed_bytes(const uint8_t req_id[8], const uint8_t nonce[16], const uint8_t payer[32], uint8_t out[56]) {
  memcpy(out, req_id, 8);
  memcpy(out + 8, nonce, 16);
  memcpy(out + 24, payer, 32);
  return VK_PROOF_SIGNED_LEN;
}

/* ---- RESULT: header, req_id@4, status@12, ref@13 ---------------------------------------- */
int vk_result_parse(const uint8_t *frame, size_t len, vk_result_t *out) {
  if (!out || vk_frame_type(frame, len) != VK_T_RESULT || len != VK_RESULT_LEN) return -1;
  if (frame[12] > VK_RESULT_FAILED) return -1;
  memcpy(out->req_id, frame + 4, 8);
  out->status = frame[12];
  memcpy(out->ref, frame + 13, 64);
  return 0;
}

size_t vk_result_build(const vk_result_t *in, uint8_t *out, size_t cap) {
  if (!in || !out || cap < VK_RESULT_LEN || in->status > VK_RESULT_FAILED) return 0;
  put_header(out, VK_T_RESULT);
  memcpy(out + 4, in->req_id, 8);
  out[12] = in->status;
  memcpy(out + 13, in->ref, 64);
  return VK_RESULT_LEN;
}

/* ---- CONTACT_HELLO: header, pubkey@4, nonce@36, name_len@52, name@53 -------------------- */
int vk_hello_parse(const uint8_t *frame, size_t len, vk_hello_t *out) {
  size_t name_len;
  if (!out || vk_frame_type(frame, len) != VK_T_CONTACT_HELLO || len < 54 || len > VK_HELLO_MAX_LEN) return -1;
  name_len = frame[52];
  if (len != 53 + name_len || !name_ok(frame + 53, name_len)) return -1;
  memset(out, 0, sizeof *out);
  memcpy(out->pubkey, frame + 4, 32);
  memcpy(out->nonce, frame + 36, 16);
  out->name_len = (uint8_t)name_len;
  memcpy(out->name, frame + 53, name_len);
  return 0;
}

size_t vk_hello_build(const vk_hello_t *in, uint8_t *out, size_t cap) {
  size_t total;
  if (!in || !out || !name_ok((const uint8_t *)in->name, in->name_len)) return 0;
  total = 53 + (size_t)in->name_len;
  if (cap < total) return 0;
  put_header(out, VK_T_CONTACT_HELLO);
  memcpy(out + 4, in->pubkey, 32);
  memcpy(out + 36, in->nonce, 16);
  out[52] = in->name_len;
  memcpy(out + 53, in->name, in->name_len);
  return total;
}

/* ---- CONTACT_CARD: header, peer_pubkey@4, peer_nonce@36, pubkey@52, name_len@84, name@85,
        sig@85+name_len ---------------------------------------------------------------------- */
int vk_card_parse(const uint8_t *frame, size_t len, vk_card_t *out) {
  size_t name_len;
  if (!out || vk_frame_type(frame, len) != VK_T_CONTACT_CARD || len < 85 + 1 + 64 || len > VK_CARD_MAX_LEN) return -1;
  name_len = frame[84];
  if (len != 85 + name_len + 64 || !name_ok(frame + 85, name_len)) return -1;
  memset(out, 0, sizeof *out);
  memcpy(out->peer_pubkey, frame + 4, 32);
  memcpy(out->peer_nonce, frame + 36, 16);
  memcpy(out->pubkey, frame + 52, 32);
  out->name_len = (uint8_t)name_len;
  memcpy(out->name, frame + 85, name_len);
  memcpy(out->sig, frame + 85 + name_len, 64);
  return 0;
}

size_t vk_card_build(const vk_card_t *in, uint8_t *out, size_t cap) {
  size_t total;
  if (!in || !out || !name_ok((const uint8_t *)in->name, in->name_len)) return 0;
  total = 85 + (size_t)in->name_len + 64;
  if (cap < total) return 0;
  put_header(out, VK_T_CONTACT_CARD);
  memcpy(out + 4, in->peer_pubkey, 32);
  memcpy(out + 36, in->peer_nonce, 16);
  memcpy(out + 52, in->pubkey, 32);
  out[84] = in->name_len;
  memcpy(out + 85, in->name, in->name_len);
  memcpy(out + 85 + in->name_len, in->sig, 64);
  return total;
}

/* peer_nonce[16] || peer_pubkey[32] || pubkey[32] || name_len[1] || name: 81 + name_len bytes.
   Returns 0 if the name is not 1..32 printable ASCII characters. */
size_t vk_card_signed_bytes(const vk_card_t *card, uint8_t out[113]) {
  if (!card || !out || !name_ok((const uint8_t *)card->name, card->name_len)) return 0;
  memcpy(out, card->peer_nonce, 16);
  memcpy(out + 16, card->peer_pubkey, 32);
  memcpy(out + 48, card->pubkey, 32);
  out[80] = card->name_len;
  memcpy(out + 81, card->name, card->name_len);
  return 81 + (size_t)card->name_len;
}
