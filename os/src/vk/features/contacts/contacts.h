// src/vk/features/contacts/contacts.h
// Contacts: the store /vk/contacts.bin (wallet/stores.md, "Contacts") and the contact swap
// (protocol/espnow.md, "CONTACT_HELLO", "CONTACT_CARD"). Private to features/contacts.
//
// A contact is written only by accept(), after a signed card was checked against this badge's key
// and its current swap nonce. Apps reach all of this only through badge.wallet.contact_* and
// badge.wallet.contacts (lua_contacts.cpp).
//
// Everything outside this feature is reached through `hooks` (time, random bytes, the signer, the
// verifier, the inbox) and through vk::fileio::ops, so the host suite test_contacts runs the whole
// file on the laptop.
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "../../wallet/pure/vk_frames.h"   // vk_hello_t, vk_card_t, the frame length constants
#include "../../wallet/reason.h"

namespace vk::contacts {

// ---- the store (stores.md) --------------------------------------------------------------------

struct Entry { uint8_t pubkey[32]; char name[33]; uint32_t added; };
size_t count();
bool at(size_t index, Entry &out);
bool upsert(const uint8_t pubkey[32], const char *name);
bool remove(const uint8_t pubkey[32]);

// ---- Added by WP34 (contacts.cpp). The five declarations above are the spec's. ----
//
// The records are kept in the order they were added: index 0 is the oldest contact. When the file
// holds CAPACITY records, a new contact drops record 0. upsert() of a key that is already there
// changes only its name (its place and `added` stay); `name` is cut to 32 characters. Both writers
// return false when the file could not be written; the file is then unchanged.

constexpr char FILE_PATH[] = "/vk/contacts.bin";
constexpr char TMP_FILE_PATH[] = "/vk/contacts.bin.tmp";
constexpr char BAD_FILE_PATH[] = "/vk/contacts.bin.bad";
constexpr char DIR_PATH[] = "/vk";

constexpr size_t CAPACITY = 64;        // records
constexpr size_t HEADER_SIZE = 8;      // magic[4] "VKC1", version u16, count u16
constexpr size_t RECORD_SIZE = 72;     // pubkey[32], name[33], flags, added u32, 2 bytes padding
constexpr uint16_t FORMAT_VERSION = 1;

// The file's header, read and checked once, so that a reader of many records (wallet.contacts)
// does not read it again for each one. `count` is only meaningful after open() returned true.
struct Cursor { uint16_t count; };
bool open(Cursor &out);                                  // false: no file, or not a valid contacts file
bool at(const Cursor &cursor, size_t index, Entry &out);

// ---- hooks ------------------------------------------------------------------------------------
// A null hook is treated as "not available": time 0, no key, no random bytes, the signature
// fails, the verification fails, nothing is posted. On the badge they are filled when the firmware
// starts; a host suite fills them with fakes.
struct Hooks {
  uint32_t (*nowMs)();                                   // millis()
  uint32_t (*unixNow)();                                 // unix seconds, or 0 when the clock has no source
  const uint8_t *(*ownKey)();                            // vk::wallet::publicKey(): 32 bytes or null
  void (*randomBytes)(uint8_t *out, size_t len);         // vk::wallet::randomBytes()
  // vk::wallet::signAuto(): the domain's validator runs, then the one signing path.
  vk::wallet::Reason (*signAuto)(const char *domain, const uint8_t *bytes, size_t len, uint8_t sig[64]);
  int (*verify)(const uint8_t *msg, size_t len, const uint8_t *sig, const uint8_t *pubkey);   // vk_verify_c: 1 = valid
  void (*notify)(const char *title, const char *body);   // notify::post(title, body, "contacts")
};
extern Hooks hooks;

// ---- the swap ---------------------------------------------------------------------------------

constexpr uint32_t NONCE_LIFETIME_MS = 60000;   // a swap nonce is valid for 60 s

// A name as HELLO and CARD carry it: at most 32 characters of `raw`, anything that is not
// printable ASCII as '?'. Returns the length (0 for an empty or null `raw`).
size_t cleanName(const char *raw, char out[VK_NAME_MAX + 1]);

// The HELLO frame of this badge: its key, its current swap nonce and `name`. A nonce is drawn when
// there is none or the last one is 60 s old, so the same frame comes back until then.
// Reasons: VK_SIGN_FAILED (the badge has no key), VK_BAD_ARG (`name` is not 1..32 printable ASCII).
vk::wallet::Reason hello(const char *name, uint8_t out[VK_HELLO_MAX_LEN], size_t &outLen);

// A CARD for the badge that sent `helloFrame`: this badge's key and `name`, signed (domain
// "contact") over the sender's nonce and key. Reasons: VK_BAD_ARG (not a HELLO frame, or a bad
// name), VK_SIGN_FAILED.
vk::wallet::Reason card(const uint8_t *helloFrame, size_t helloLen, const char *name,
                        uint8_t out[VK_CARD_MAX_LEN], size_t &outLen);

// Checks a CARD in the order of espnow.md and saves its owner as a contact:
//   not a CARD frame                                  VK_BAD_ARG
//   peer_pubkey is not this badge's key               VK_MISMATCH
//   peer_nonce is not the current, unexpired nonce    VK_EXPIRED
//   the signature does not verify with the card's key VK_BAD_PROOF
//   the contact could not be written                  VK_UNSUPPORTED (nothing else changes)
// On VK_OK the contact is stored (`saved` is its record), the nonce is rotated (the next hello()
// draws a new one, so the same card is refused from then on) and the inbox gets "Contact saved".
vk::wallet::Reason accept(const uint8_t *cardFrame, size_t cardLen, Entry &saved);

// The validator of the signing domain "contact": `bytes` is exactly
// peer_nonce[16] ‖ peer_pubkey[32] ‖ own_pubkey[32] ‖ name_len[1] ‖ name (82..113 bytes) with a
// name of 1..32 printable ASCII characters, and own_pubkey is `ownKey`.
bool validCardBytes(const uint8_t *bytes, size_t len, const uint8_t *ownKey);

// Forgets the swap nonce. Does not touch the file or `hooks`.
void reset();

}  // namespace vk::contacts
