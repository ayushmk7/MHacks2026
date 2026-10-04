# Signing

The badge key, the one code path that uses it, and the table of signing domains. Files: `src/vk/wallet/signer.{h,cpp}`, `crypto.{h,cpp}`, `reason.h`, `pure/vk_reason.{h,c}`, and one `domain_*.cpp` per feature.

## Key

The key is upstream's device identity: one Ed25519 key pair made on first boot ([baseline](../architecture/upstream-baseline.md)). We never generate, export or copy it.

| Where the private key is | `vk::wallet::keyLocation()` | Largest message it can sign |
|---|---|---|
| Inside the SE050 | `"se050"` | 242 bytes after hook H12 [UNVERIFIED on silicon] |
| NVS flash, signed with TweetNaCl | `"software"` | no practical limit (we cap at 1248) |
| No identity | `"none"` | nothing; every signature fails with `sign_failed` |

Sizes that matter: a token transfer message is 214 bytes and fits both. A transfer with a Memo instruction is at least 250 bytes and **does not fit the SE050 path**; on an SE050-keyed badge it is refused with `too_long` before any screen is shown. Apps that add a memo must handle that reason.

Honesty rule: the UI and the Lua API always report the true key location. A software key is a plaintext seed in flash, readable with a USB cable; never describe it as hardware-protected.

`VK_FORCE_SOFTWARE_KEY` (hook H18) is the fallback when the SE050 path fails on a badge ([build guide](../guides/build-flash-provision.md#se050-fallback)).

## The key gate

`identity::sign` is upstream and public (finding F5). The gate is a rule plus a check:

- The only file that calls `identity::sign` or `identity::signBase64` is `src/vk/wallet/signer.cpp`.
- `signer.cpp` calls it from exactly one static function, `signRaw`, which every signature goes through.
- `scripts/preflash-check.sh` fails if `identity::sign`, `signBase64`, `se050_apdu::signEd25519`, `ed25519::sign` or `crypto_sign(` appears anywhere outside `src/identity/`, `src/hal/se050_apdu.cpp` and `src/vk/wallet/signer.cpp`.

## Reason codes

One enum for every refusal, defined in C so the host-tested code shares it. Full table with where each is shown: [../reference/reasons.md](../reference/reasons.md).

```c
/* src/vk/wallet/pure/vk_reason.h */
typedef enum {
  VK_OK = 0, VK_CANCELLED, VK_TIMEOUT, VK_UNDECODABLE, VK_UNVERIFIED, VK_REVOKED, VK_EXPIRED,
  VK_MISMATCH, VK_BAD_PROOF, VK_OVER_CAP, VK_NO_TIME, VK_BUSY, VK_DENIED, VK_NOT_PROVISIONED,
  VK_TOO_LONG, VK_SIGN_FAILED, VK_BAD_ARG, VK_UNSUPPORTED, VK_IDLE
} vk_reason_t;
const char *vk_reason_name(vk_reason_t r);   /* "ok", "cancelled", "timeout", ... lower-case, same order */
```

```cpp
// src/vk/wallet/reason.h
#include "pure/vk_reason.h"
namespace vk::wallet { using Reason = vk_reason_t; inline const char *reasonName(Reason r) { return vk_reason_name(r); } }
```

## Domain table

A signing domain is one kind of signature. Each is a row that a feature registers.

```cpp
// src/vk/wallet/signer.h
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

}  // namespace vk::wallet
```

(`SignDomain` needs a constructor taking the seven fields in that order; aggregate initialisation is not available for a class with a base.)

`publicKey()` and `addressBase58()` are how features and native apps learn the badge's own key; they never include anything from `src/identity/`.

### Who owns what

- `signer.cpp` owns the key path. Besides the public functions above it exposes one more, declared in `src/vk/wallet/signer_internal.h`, which only `approval.cpp` includes: `Reason signForApproval(const SignDomain *domain, const uint8_t *bytes, size_t len, uint8_t sig[64]);`. It calls `signRaw`.
- The approval engine owns the pending request and the result. `vk::wallet::begin()` validates and then calls `approval::open(request, domain, bytes, len, app_id)`; `vk::wallet::poll()` is implemented as `approval::takeResult()` ([approval](approval.md#the-request)).

### Cross-feature interfaces

Features may not include each other's headers. The two things one feature needs from another go through function pointers declared here and defined (as null) in `signer.cpp`:

```cpp
namespace vk::wallet {
// Set by the requests feature. Result for req_id; also writes the payee key the proof was checked
// against and the nonce that was sent. Null when the feature is absent.
extern vk_presence_t (*presenceLookup)(const uint8_t req_id[8], uint8_t payee_pubkey_out[32], uint8_t nonce_out[16]);

// Set by the balance feature. Null when the feature is absent.
struct TokenInfo { bool balance_known; uint64_t raw; bool account_known; uint8_t account[32]; };
extern bool (*tokenInfoLookup)(const uint8_t mint[32], TokenInfo &out);
}
```

The rows:

| Name | Prefix | Button | Permission | Max bytes | Registered by | Bytes after the prefix |
|---|---|---|---|---|---|---|
| `solana` | (none) | yes | `sign` | 1232 | `features/solana_pay` | a serialized legacy Solana message ([solana-payments.md](solana-payments.md)) |
| `bank` | `bank-auth:` | yes | `sign` | 512 | `features/bank` | the bank payload text ([checks.md](checks.md#bank-rail)) |
| `pay-req` | `pay-req:` | no | internal | 94 | `features/requests` | a REQ frame from its header to the end of the name ([protocol](../protocol/espnow.md#req)) |
| `pay-proof` | `pay-proof:` | no | internal | 56 | `features/requests` | `req_id[8] ‖ nonce[16] ‖ payer_pubkey[32]` |
| `contact` | `contact:` | no | internal | 113 | `features/contacts` | `peer_nonce[16] ‖ peer_pubkey[32] ‖ own_pubkey[32] ‖ name_len[1] ‖ name[≤32]` |
| `store-reg` | (none) | no | internal | 200 | `features/store_reg` | exactly `solana-badge-register:<own pubkey base58>:<nonce>`; the nonce uses upstream's `isSafeNonce` character set (copy that function from `broker_client.cpp`) |

"Internal" means no app can hand the wallet core bytes for that domain. Apps call a higher-level function (`wallet.request_open`, `wallet.contact_card`), and the firmware builds the bytes itself.

## The signing path

```
begin(domain, bytes, len, ctx, app_id)
  not provisioned                         -> VK_NOT_PROVISIONED
  domain unknown or not a button domain   -> VK_UNSUPPORTED
  running app lacks domain->permission    -> VK_DENIED
  an approval is open or a result is waiting to be polled -> VK_BUSY
  len == 0 or len > domain->max_len       -> VK_TOO_LONG
  strlen(prefix) + len > maxSignBytes()   -> VK_TOO_LONG
  domain->decode(bytes, len, ctx, request) returns a reason != VK_OK -> that reason
  copy bytes; approval::open(request)     -> VK_OK         (see approval.md)

on SELECT in the approval:
  signRaw(domain, bytes, len) -> result stored for poll()

signAuto(domain, bytes, len, sig)
  domain unknown or needs_button          -> VK_UNSUPPORTED
  !domain->validate(bytes, len)           -> VK_BAD_ARG
  signRaw(domain, bytes, len)

signRaw(domain, bytes, len, sig)          // static, the only caller of identity::sign
  !selfCheckOk()                          -> VK_SIGN_FAILED
  buffer = prefix ‖ bytes
  identity::sign(buffer, n, sig) ? VK_OK : VK_SIGN_FAILED
  log: [vk] sign <domain> <n> bytes <ms> ms
```

Calls from inside a Lua callback (`signAuto` through `request_open` or `contact_card`) first call `runtime::extendDeadline(2500)`: a software signature can take about a second (finding F7).

## Why domains cannot be confused

- Every text domain's signed bytes begin with a lower-case ASCII letter (`0x61`–`0x7A`).
- The `solana` decoder accepts only a message whose first byte is `0x01` (one required signature). So no text-domain message is a valid transaction, and no transaction begins with a text prefix.
- Text prefixes are pairwise prefix-free, so one domain's message is never another's.
- An auto domain with no prefix (`store-reg`) must have a validator that requires fixed leading ASCII text.

### Self-check

`vk::begin()` verifies this over the registered rows before anything can sign:

1. names are unique;
2. every non-empty prefix is 2–15 characters of `[a-z-]` followed by `:`;
3. no non-empty prefix is a prefix of another;
4. at most one button domain has an empty prefix;
5. every auto domain has a `validate` function, every button domain a `decode` function;
6. no prefix equals a reserved prefix. The reserved list is `registry:` (signed by the issuer, never by a badge).

On failure it logs `[vk] DOMAIN TABLE INVALID: <rule> <name>`, `selfCheckOk()` stays false, and every signature fails with `sign_failed`. The host test `test_domains` runs the same function over a copy of the table.

## Crypto backend

```cpp
// src/vk/wallet/crypto.h
namespace vk::wallet {
bool verify(const uint8_t *msg, size_t len, const uint8_t sig[64], const uint8_t pubkey[32]);
void randomBytes(uint8_t *out, size_t len);        // esp_fill_random
}
// C adapter with the signature the pure code's function pointers expect; returns 1 when valid.
extern "C" int vk_verify_c(const uint8_t *msg, size_t len, const uint8_t *sig, const uint8_t *pubkey);
```

`VK_ED25519_BACKEND 0` (default) calls upstream's `ed25519::verify` (TweetNaCl). `VK_ED25519_BACKEND 1` calls Monocypher 4.0.2 (`crypto_ed25519_check`), vendored as `src/vk/wallet/vendor/monocypher.{c,h}` and `monocypher-ed25519.{c,h}`; signatures are interchangeable. The backend is switched only if measurement M2 ([testing](../testing/testing.md#measurements)) shows verification is too slow for the presence deadline.

Verification inside a Lua callback is preceded by `runtime::extendDeadline(2500)`.

## Adding a signing domain

1. Pick a name and a prefix that satisfies the self-check rules.
2. In your feature folder create `domain_<name>.cpp` with one `VK_SIGN_DOMAIN(...)` line and its `decode` (button) or `validate` (auto) function.
3. For a button domain, `decode` fills an `ApprovalRequest` ([approval.md](approval.md)); no screen code is written.
4. Add the row to the table above and to `test/host/test_domains.cpp`.

Nothing in `signer.cpp`, the approval engine or the screen changes. Removing a domain is deleting its file. Full recipe: [../guides/extending.md](../guides/extending.md#add-a-signing-domain).

`scripts/preflash-check.sh` fails if `VK_SIGN_DOMAIN(` appears outside `src/vk/features/*/domain_*.cpp`, so the set of things the badge can sign is always one `grep` away.
