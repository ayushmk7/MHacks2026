# ESP-NOW protocol and router

Badge-to-badge messages: the frame header, the registry of frame types, every frame's bytes, the router that decides who receives a frame, and the payment-request and presence exchanges. Files: `src/vk/wallet/pure/vk_frames.{h,c}` (codec, host-tested), `src/vk/host/router.{h,cpp}`, `src/vk/features/requests/`.

All badges and the laptop join one phone hotspot on 2.4 GHz so that every radio is on the same channel.

## Frame header

Upstream already wraps every ESP-NOW payload as `SBDG` + type byte and delivers type `0x02` ("app") payloads of up to 240 bytes ([baseline](../architecture/upstream-baseline.md#esp-now-framing)). A Badge OS frame is such a payload:

| Offset | Size | Field |
|---|---|---|
| 0 | 2 | magic `'V'`, `'K'` |
| 2 | 1 | version, `1` |
| 3 | 1 | type |
| 4 | ≤ 236 | body |

Integers are little-endian. A payload that does not start with `VK` + version 1 is not ours and is treated as a plain app message.

## Type registry

| Range | Owner | Assigned |
|---|---|---|
| 1–15 | payments (`features/requests`) | 1 REQ · 2 CHAL · 3 PROOF · 4 RESULT |
| 16–31 | contacts (`features/contacts`) | 16 CONTACT_HELLO · 17 CONTACT_CARD |
| 32–63 | reserved for firmware | none |
| 64–255 | apps | 64–71 Duel · 72–255 free |

An app picks an unused block in the app range and records it in this table. Only one app runs at a time, so two apps never receive each other's frames; the table exists so two badges running different apps do not misread each other.

## Frames

### REQ

Type 1. "Pay me." Broadcast by the payee's firmware once a second while the request is open; the same bytes every time.

| Offset | Size | Field |
|---|---|---|
| 0 | 4 | header |
| 4 | 1 | `rail`: 1 Solana, 2 bank |
| 5 | 32 | `payee_pubkey` (the sender's device key) |
| 37 | 8 | `amount` u64, raw units (token base units, or cents) |
| 45 | 4 | `currency`, ASCII, NUL-padded (`HACK`, `USD\0`) |
| 49 | 8 | `req_id`, random |
| 57 | 4 | `expiry` u32, unix seconds |
| 61 | 1 | `name_len`, 1..32 |
| 62 | `name_len` | `name`, printable ASCII. **A claim only**: the payer's screen shows the name from the verified record |
| 62 + `name_len` | 64 | `sig`: the payee's signature, domain `pay-req`, over bytes 0 .. 62 + `name_len` |

Total 127–158 bytes.

### CHAL

Type 2. "Prove you are here." Unicast from payer to the MAC the REQ came from. 60 bytes.

| Offset | Size | Field |
|---|---|---|
| 4 | 8 | `req_id` |
| 12 | 16 | `nonce`, random, fresh per challenge |
| 28 | 32 | `payer_pubkey` |

### PROOF

Type 3. Unicast from payee to the challenger. 76 bytes.

| Offset | Size | Field |
|---|---|---|
| 4 | 8 | `req_id` |
| 12 | 64 | `sig`: the payee's signature, domain `pay-proof`, over `req_id ‖ nonce ‖ payer_pubkey` |

### RESULT

Type 4. "I paid" or "I did not." Unicast from payer to payee. 77 bytes. **Unauthenticated**: the payee confirms on chain before showing PAID.

| Offset | Size | Field |
|---|---|---|
| 4 | 8 | `req_id` |
| 12 | 1 | `status`: 0 ok, 1 rejected, 2 failed |
| 13 | 64 | `ref`: the transaction signature (64 raw bytes), or zeros |

### CONTACT_HELLO

Type 16. "I am swapping contacts." Broadcast once a second by a badge in swap mode.

| Offset | Size | Field |
|---|---|---|
| 4 | 32 | `pubkey` |
| 36 | 16 | `nonce`: the sender's current swap nonce |
| 52 | 1 | `name_len`, 1..32 |
| 53 | `name_len` | `name` |

### CONTACT_CARD

Type 17. A signed card, unicast in answer to a HELLO.

| Offset | Size | Field |
|---|---|---|
| 4 | 32 | `peer_pubkey`: who the card is made for (from the HELLO being answered) |
| 36 | 16 | `peer_nonce`: the nonce from that HELLO |
| 52 | 32 | `pubkey` (the card's owner, the sender) |
| 84 | 1 | `name_len`, 1..32 |
| 85 | `name_len` | `name` |
| 85 + `name_len` | 64 | `sig`: domain `contact`, over `peer_nonce[16] ‖ peer_pubkey[32] ‖ pubkey[32] ‖ name_len[1] ‖ name` |

Total 150–181 bytes. The receiver checks, in this order: `peer_pubkey` is its own key (else `mismatch`); `peer_nonce` is its current, unexpired swap nonce (else `expired`); the signature verifies with `pubkey` (else `bad_proof`). Because the signature covers the receiver's nonce and key, a card is valid for one receiver and one swap. A recorded card replayed later, or to someone else, fails.

## Codec

```c
/* src/vk/wallet/pure/vk_frames.h — every function is pure; parse returns 0 on success */
enum { VK_T_REQ = 1, VK_T_CHAL = 2, VK_T_PROOF = 3, VK_T_RESULT = 4, VK_T_CONTACT_HELLO = 16, VK_T_CONTACT_CARD = 17 };

int vk_frame_type(const uint8_t *frame, size_t len);          /* type, or -1 if not a VK v1 frame */

typedef struct {
  uint8_t rail; uint8_t payee_pubkey[32]; uint64_t amount; char currency[5];
  uint8_t req_id[8]; uint32_t expiry; uint8_t name_len; char name[33]; uint8_t sig[64];
  size_t signed_len;                                           /* bytes of the frame the signature covers */
} vk_req_t;
int    vk_req_parse(const uint8_t *frame, size_t len, vk_req_t *out);
size_t vk_req_build(const vk_req_t *in, uint8_t *out, size_t cap);     /* writes all fields incl. in->sig */

typedef struct { uint8_t req_id[8]; uint8_t nonce[16]; uint8_t payer_pubkey[32]; } vk_chal_t;
int    vk_chal_parse(const uint8_t *frame, size_t len, vk_chal_t *out);
size_t vk_chal_build(const vk_chal_t *in, uint8_t *out, size_t cap);

typedef struct { uint8_t req_id[8]; uint8_t sig[64]; } vk_proof_t;
int    vk_proof_parse(const uint8_t *frame, size_t len, vk_proof_t *out);
size_t vk_proof_build(const vk_proof_t *in, uint8_t *out, size_t cap);
size_t vk_proof_signed_bytes(const uint8_t req_id[8], const uint8_t nonce[16], const uint8_t payer[32], uint8_t out[56]);

typedef struct { uint8_t req_id[8]; uint8_t status; uint8_t ref[64]; } vk_result_t;
int    vk_result_parse(const uint8_t *frame, size_t len, vk_result_t *out);
size_t vk_result_build(const vk_result_t *in, uint8_t *out, size_t cap);

typedef struct { uint8_t pubkey[32]; uint8_t nonce[16]; uint8_t name_len; char name[33]; } vk_hello_t;
int    vk_hello_parse(const uint8_t *frame, size_t len, vk_hello_t *out);
size_t vk_hello_build(const vk_hello_t *in, uint8_t *out, size_t cap);

typedef struct { uint8_t peer_pubkey[32]; uint8_t peer_nonce[16]; uint8_t pubkey[32]; uint8_t name_len; char name[33]; uint8_t sig[64]; } vk_card_t;
int    vk_card_parse(const uint8_t *frame, size_t len, vk_card_t *out);
size_t vk_card_build(const vk_card_t *in, uint8_t *out, size_t cap);
size_t vk_card_signed_bytes(const vk_card_t *card, uint8_t out[113]);
```

Parsers are strict: exact length, `name_len` in range, printable ASCII names, known `rail`. Host suite `test_frames` round-trips every type and refuses each truncated or over-long copy.

## Router

```cpp
// src/vk/host/router.h
namespace vk::host::router {
// Return true to consume the frame; false to let it continue to the running app.
using Handler = bool (*)(const uint8_t mac[6], const uint8_t *frame, size_t len, int8_t rssi, uint32_t rx_ms);

struct EspnowRoute : Registered<EspnowRoute> {
  uint8_t first, last;      // inclusive type range
  const char *name;
  Handler fn;
  EspnowRoute(uint8_t f, uint8_t l, const char *n, Handler h) : first(f), last(l), name(n), fn(h) {}
};
#define VK_ESPNOW_ROUTE(ident, first, last, handler) \
  static vk::host::router::EspnowRoute vk_route_##ident(first, last, #ident, handler)

void install();                                                     // hook H3
bool send(const uint8_t *mac, const uint8_t *frame, size_t len);    // mac nullptr = broadcast
}
```

`install()` sets the one upstream receive handler. For each received payload:

1. If it is a VK v1 frame and a route covers its type, call the route with `rx_ms = espnow_mgr::lastRxMs()` (hook H13). If the route returns true, stop.
2. CHAL and PROOF (types 2 and 3) are never forwarded to an app, whether or not a route exists.
3. Everything else that no route consumed (REQ, RESULT, the contact frames, app-range frames, payloads that are not VK frames at all) is forwarded to the app.

"Forward to the app" means `runtime::dispatchEspnow(mac, data, len, rssi)`, and only when an app is running, the approval is not active, and the app was granted the `espnow` permission (native apps always are). There is no queue of our own: a frame that arrives with no eligible app is dropped, except that firmware routes have already seen it.

Upstream's receive queue holds 7 frames and silently drops the newest when full. While the loop is blocked by a signature (one to three seconds with a software key), requests rebroadcast by several badges can fill it and a CHAL or PROOF can be lost. A lost exchange leaves the slot `PENDING`, which the approval shows as amber; the Pay app may call `wallet.challenge` again (a fresh nonce replaces the slot).

This also fixes upstream finding F1: the handler is installed once and never cleared (hook H9).

## Payment requests (payee side)

`src/vk/features/requests/requests.{h,cpp}`.

- `wallet.request_open{...}` ([Lua API](../platform/lua-api.md#badgewallet-requests)) makes the firmware build a REQ (`payee_pubkey` = this badge, random `req_id`, `expiry = now + req_ttl_s`), sign it once (domain `pay-req`), and store it as an **active request**. At most 2 are active; opening a third fails with `busy`.
- The feature's service broadcasts each active request every `req_period_ms` (default 1000).
- An active request closes on `wallet.request_close(req_id)`, at its expiry, or when the app that opened it stops.
- Opening a request needs the clock (`no_time` otherwise): the expiry is a real time.

## Presence

The question presence answers: is the holder of the payee key within radio range and answering *now*? It defeats a replayed REQ (the replayer cannot sign a PROOF). It does not defeat a live relay to the real payee in range; say so.

### Payee: answering CHAL

Route for type 2, in firmware, no app involved:

1. `req_id` matches an active request, else drop.
2. Rate limits per active request: at most `req_max_proofs` (default 8) proofs in total, and at least `req_gap_ms` (default 200) since the last one. Otherwise drop.
3. Sign `pay-proof` over `req_id ‖ nonce ‖ payer_pubkey` (from the CHAL) and unicast PROOF to the sender's MAC.

The loop is blocked for one signature. That is the latency the payer measures.

### Payer: challenging and judging

`src/vk/features/requests/presence.{h,cpp}`. A table of 4 slots: `req_id`, `payee_pubkey` (from the REQ), `mac`, `nonce`, `t0_ms`, `result`. The oldest slot is reused when the table is full.

- `wallet.challenge(mac, req_frame)` parses the REQ, fills a slot with a fresh random nonce and result `PENDING`, sets `t0_ms = millis()` and sends CHAL to `mac`.
- Route for type 3: find the `PENDING` slot with that `req_id` whose `mac` equals the sender's; verify the signature over `req_id ‖ nonce ‖ own_pubkey` with the slot's `payee_pubkey`. Invalid → `BAD_SIG`. Valid and `rx_ms − t0_ms ≤ presence_ms` → `PRESENT`. Valid but slower → `LATE`. The first PROOF decides; later ones for that slot are ignored.
- `wallet.presence(req_id)` returns the slot's result as a string for the app's UI. The approval reads the same slot through `presenceLookup` ([checks](../wallet/checks.md#presence-lookup)).

`presence_ms` is a config key. Its default (1500) allows for a software signature on the payee; measurement M1 ([testing](../testing/testing.md#measurements)) sets the real value: the 95th percentile of CHAL→PROOF plus half again.

### Request cache (payer side)

The route for type 1 keeps the last 8 distinct requests seen (by `req_id`): frame, MAC, RSSI, time seen. An entry is dropped at its expiry or 30 s after it was last heard. `wallet.requests()` returns the cache. When a new `req_id` appears and the running app is not the configured `pay_app`, the route posts a notification ([app host](../platform/app-host.md#notifications)). The route returns false, so a running app also gets the frame.

## Sequences

Honest payment:

```
payee app          payee firmware            payer firmware           payer app
request_open  -->  build+sign REQ
                   REQ (broadcast, 1 Hz) --> cache, notify
                                                                <--   requests()
                                                                <--   challenge(mac, req)
                   <-- CHAL ----------------  slot PENDING, t0
                   sign PROOF
                   --- PROOF -------------->  verify, PRESENT
                                                                <--   fetch record (HTTP), build_transfer
                                                                <--   begin_solana(msg, ctx)
                                              checks -> GREEN, approval, SELECT, sign
                                                                <--   poll() -> sig; submit; send RESULT
on_espnow(RESULT) <------------------------------------------------- RESULT
confirm on chain, show PAID
```

What each attack looks like on the payer's screen:

| Attack | What happens | Screen |
|---|---|---|
| Impostor badge broadcasts its own REQ under the merchant's name | the backend has no record for the impostor's key | red, UNVERIFIED RECIPIENT |
| Impostor replays the merchant's real REQ | record and request verify; the impostor cannot sign a PROOF | amber, VERIFIED - NOT PRESENT |
| Impostor answers CHAL with a made-up PROOF | signature fails | red, BAD PROOF |
| App builds a transaction paying someone other than the record's account | destination ≠ `record.solana_ata` | red, WRONG RECIPIENT |
| App builds a transaction for more than the request | amount ≠ `req.amount` | red, WRONG AMOUNT |
| Merchant's attestation is revoked | record says revoked | red, REVOKED |

## Adding a frame type

For an app: pick a free block in 64–255, add it to the type registry above, build and parse the frames in Lua with `lib/vk.lua` helpers. No firmware change.

For firmware: add the struct and the build/parse pair to `vk_frames`, a test to `test_frames`, and one `VK_ESPNOW_ROUTE(...)` line in the feature that handles it. Recipe: [../guides/extending.md](../guides/extending.md#add-an-esp-now-frame-type).
