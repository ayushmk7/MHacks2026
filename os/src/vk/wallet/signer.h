// The wallet core's public interface: the signing-domain table and the one signing path
// (signing.md, "Domain table" and "Cross-feature interfaces").
#pragma once

#include <Arduino.h>

#include "../core/registry.h"
#include "pure/vk_checks.h"   // vk_presence_t
#include "reason.h"

namespace vk::wallet {

struct ApprovalRequest;                    // approval.h

struct Ctx {                               // what an app may attach to a button-domain request
  const uint8_t *record = nullptr;  size_t record_len = 0;   // registry record, canonical bytes
  const uint8_t *record_sig = nullptr;                       // 64 bytes or null
  const uint8_t *req = nullptr;     size_t req_len = 0;      // full REQ frame including its signature
};

struct SignDomain : Registered<SignDomain> {
  const char *name;          // "solana"
  const char *prefix;        // "" or lower-case ASCII ending in ':'
  bool needs_button;         // true: only through begin() and the approval
  const char *permission;    // permission an app needs to call begin() for it; nullptr = firmware-internal
  size_t max_len;            // largest `bytes` accepted, prefix not counted
  // Button domains. Decode the bytes, run the checks, fill `out`. Return VK_OK to open the approval
  // (even a red one), or a reason to refuse immediately without a screen.
  Reason (*decode)(const uint8_t *bytes, size_t len, const Ctx &ctx, ApprovalRequest &out);
  // Auto domains. True only if `bytes` has exactly the expected structure.
  bool (*validate)(const uint8_t *bytes, size_t len);

  SignDomain(const char *n, const char *p, bool button, const char *perm, size_t maxLen,
             Reason (*d)(const uint8_t *, size_t, const Ctx &, ApprovalRequest &),
             bool (*v)(const uint8_t *, size_t))
      : name(n), prefix(p), needs_button(button), permission(perm), max_len(maxLen), decode(d), validate(v) {}
};

#define VK_SIGN_DOMAIN(ident, name, prefix, needs_button, permission, max_len, decode_fn, validate_fn) \
  static vk::wallet::SignDomain vk_domain_##ident{name, prefix, needs_button, permission, max_len, decode_fn, validate_fn}

const SignDomain *findDomain(const char *name);

// Auto domains only. Firmware-internal: never exposed to apps with caller-chosen bytes.
Reason signAuto(const char *domain, const uint8_t *bytes, size_t len, uint8_t sig[64]);

// Button domains only. `app_id` is shown on the screen and written to the history.
Reason begin(const char *domain, const uint8_t *bytes, size_t len, const Ctx &ctx, const char *app_id);

enum class Poll : uint8_t { IDLE, PENDING, SIGNED, FAILED };
Poll poll(uint8_t sig[64], Reason &reason);       // SIGNED and FAILED are returned once, then IDLE

const char *keyLocation();                        // "se050" | "software" | "none"
size_t maxSignBytes();                            // 242 or 1248
const uint8_t *publicKey();                       // 32 bytes, or nullptr when there is no identity
String addressBase58();                           // "" when there is no identity
bool selfCheckOk();
String signStoreRegistration(const String &message);   // target of hook H10; "" on any refusal

// --- Cross-feature interfaces. Defined (as null) in signer.cpp. ---

// Set by the requests feature. Result for req_id; also writes the payee key the proof was checked
// against and the nonce that was sent. Null when the feature is absent.
extern vk_presence_t (*presenceLookup)(const uint8_t req_id[8], uint8_t payee_pubkey_out[32], uint8_t nonce_out[16]);

// Set by the balance feature. Null when the feature is absent.
struct TokenInfo { bool balance_known; uint64_t raw; bool account_known; uint8_t account[32]; };
extern bool (*tokenInfoLookup)(const uint8_t mint[32], TokenInfo &out);

// --- Self-check (signing.md, "Self-check"). Added by WP11. ---

// One row of the domain table as the self-check sees it. A plain struct, so the host test can check
// a table without registering anything.
struct DomainRow {
  const char *name;
  const char *prefix;
  bool needs_button;
  bool has_decode;
  bool has_validate;
};

// The six rules of the self-check, as a pure function. Returns 0 when the table is valid, else the
// number (1 to 6) of the first rule that is broken; `*bad_name` (when not null) then names the row.
int checkDomainTable(const DomainRow *rows, size_t count, const char **bad_name);

#ifdef VK_HOST_TEST
// Host-test seam (test/host/test_domains.cpp). On the badge signer.cpp calls the config store, the
// permissions, the approval engine and the identity directly; on the host it reads these instead.
struct HostHooks {
  bool provisioned = true;                             // vk::config::provisioned()
  bool (*granted)(const char *permission) = nullptr;   // vk::host::granted(); null = everything granted
  bool busy = false;                                   // an approval is open or a result is waiting
  const uint8_t *public_key = nullptr;                 // the badge key, 32 bytes; null = no identity
  size_t max_sign_bytes = 1248;                        // maxSignBytes()
  // identity::sign(); null = the identity refuses.
  bool (*sign)(const uint8_t *message, size_t length, uint8_t out[64]) = nullptr;
  // The last two steps of begin(): domain->decode, then approval::open. Null = VK_UNSUPPORTED.
  Reason (*open)(const SignDomain *domain, const uint8_t *bytes, size_t len, const Ctx &ctx, const char *app_id) = nullptr;
};
extern HostHooks hostHooks;

// What the signer service does at boot: checks the registered rows and sets selfCheckOk().
// Returns the broken rule (0 = valid), like checkDomainTable.
int runSelfCheck(const char **bad_name);
#endif

}  // namespace vk::wallet
