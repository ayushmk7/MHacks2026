# ESP-NOW protocol and router

Badge-to-badge messages: the frame header, the registry of frame types, every frame's bytes, the router that decides who receives a frame, and the payment-request and presence exchanges. Files: `src/vk/wallet/pure/vk_frames.{h,c}` (codec, host-tested), `src/vk/host/router.{h,cpp}`, `src/vk/features/requests/`.

All badges and the laptop join one phone hotspot on 2.4 GHz so that every radio is on the same channel.

## Frame header

Upstream already wraps every ESP-NOW payload as a 4-byte magic + type byte (the magic is `BDOS` in BadgeOS, hook H23; upstream's was `SBDG`, so a BadgeOS badge and an upstream badge do not hear each other) and delivers type `0x02` ("app") payloads of up to 240 bytes ([baseline](../architecture/upstream-baseline.md#esp-now-framing)). A BadgeOS frame is such a payload:

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
| 64–255 | apps | 64–71 Duel: 64 INVITE · 65 ACCEPT · 66 GO · 67 TIME · 68 LEAVE · 69–71 unused · 72–255 free |

An app picks an unused block in the app range and records it in this table. Only one app runs at a time, so two apps never receive each other's frames; the table exists so two badges running different apps do not misread each other.

Duel's frames (`os/apps/duel/main.lua`; built and matched with `vk.app_frame` and `vk.app_body`, so each starts with the VK header). `game` is 8 random bytes chosen by the inviter and names one duel:

| Type | Name | Body | Sent |
|---|---|---|---|
| 64 | INVITE | `game`(8) · the inviter's public key (32) · the stake as text in display units | broadcast, repeated while inviting |
| 65 | ACCEPT | `game`(8) · the accepter's public key (32) | to the inviter, repeated until the first GO |
| 66 | GO | `game`(8) · round (1) · milliseconds until the flash (u16, big-endian) | by the inviter, repeated until the flash; each badge times itself from its own flash |
| 67 | TIME | `game`(8) · round (1) · reaction time in ms (u16; 65535 = pressed before the flash) | to the opponent |
| 68 | LEAVE | `game`(8) | to the opponent when a player quits: the other badge stops waiting at once |

The public keys are in INVITE and ACCEPT because the loser pays the winner's key and no other frame carries it. None of these frames is signed: they decide who asks whom for money, and the payment itself goes through a signed REQ and the approval screen like any other.

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

The swap nonce: 16 random bytes, drawn by `wallet.contact_hello()` and returned unchanged for 60 s (a nonce exactly 60,000 ms old has expired). A successful accept invalidates it; the next `contact_hello()` draws a new one, and until then every card is `expired`. A failed accept, a write failure included, keeps it. Nothing forbids a badge accepting a card it made for its own HELLO; the one-badge test (`t_con_single.py`) relies on that.

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

Parsers are strict: exact length, `name_len` in range, printable ASCII names, known `rail`. Two more refusals keep build-after-parse byte-identical: a RESULT `status` above 2, and a REQ `currency` that is not 1 to 4 printable ASCII characters followed only by NUL bytes.

The header also defines the length constants (`VK_FRAME_MAX` 240, `VK_REQ_MIN_LEN` 127, `VK_REQ_MAX_LEN` 158, `VK_CHAL_LEN` 60, `VK_PROOF_LEN` 76, `VK_PROOF_SIGNED_LEN` 56, `VK_RESULT_LEN` 77, `VK_HELLO_MAX_LEN` 85, `VK_CARD_MAX_LEN` 181), `VK_RAIL_SOLANA` / `VK_RAIL_BANK`, `VK_RESULT_OK` / `REJECTED` / `FAILED`, and the signing prefixes `VK_PREFIX_PAY_REQ` (`"pay-req:"`), `VK_PREFIX_PAY_PROOF` (`"pay-proof:"`) and `VK_PREFIX_CONTACT` (`"contact:"`). A REQ is signed over its prefix plus `frame[0..signed_len)`, a PROOF over its prefix plus `vk_proof_signed_bytes`, a card over its prefix plus `vk_card_signed_bytes`. Host suite `test_frames` round-trips every type and refuses each truncated or over-long copy.

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

// The field `first` hides the registry's static first(): EspnowRoute::first() does not compile.
// Iterate with: for (auto *r = firstRoute(); r; r = r->next())
inline EspnowRoute *firstRoute() { return Registered<EspnowRoute>::first(); }

void install();                                                     // hook H3
bool send(const uint8_t *mac, const uint8_t *frame, size_t len);    // mac nullptr = broadcast
}
```

`install()` sets the one upstream receive handler. For each received payload:

1. If it is a VK v1 frame and a route covers its type, call the route with `rx_ms = espnow_mgr::lastRxMs()` (hook H13). If the route returns true, stop. When several routes cover the type, each is called in registry order until one returns true.
2. CHAL and PROOF (types 2 and 3) are never forwarded to an app, whether or not a route exists.
3. Everything else that no route consumed (REQ, RESULT, the contact frames, app-range frames, payloads that are not VK frames at all) is forwarded to the app.

"Forward to the app" means `runtime::dispatchEspnow(mac, data, len, rssi)`, and only when an app is running, the approval is not active, and the app was granted the `espnow` permission (native apps always are: `router.cpp` asks `vk::host::granted("espnow")` for every frame and `permissions.cpp` answers true for a native app). `router::send` returns false for a null frame or a zero length. There is no queue of our own: a frame that arrives with no eligible app is dropped, except that firmware routes have already seen it.

Upstream's receive queue holds 7 frames and silently drops the newest when full. While the loop is blocked by a signature (211 ms with the software key, [M2](../testing/testing.md#measurements)), requests rebroadcast by several badges can fill it and a CHAL or PROOF can be lost. A lost exchange leaves the slot `PENDING`, which the approval shows as amber; the Pay app may call `wallet.challenge` again (a fresh nonce replaces the slot).

This also fixes upstream finding F1: the handler is installed once and never cleared (hook H9).

## Payment requests (payee side)

`src/vk/features/requests/requests.{h,cpp}`.

- `wallet.request_open{...}` ([Lua API](../platform/lua-api.md#badgewallet-requests)) makes the firmware build a REQ (`payee_pubkey` = this badge, random `req_id`, `expiry = now + req_ttl_s`), sign it once (domain `pay-req`), and store it as an **active request**. At most 2 are active; opening a third fails with `busy`.
- The feature's service broadcasts each active request every `req_period_ms` (default 1000).
- An active request closes on `wallet.request_close(req_id)`, at its expiry, or when the app that opened it stops. Upstream runs the stop listeners before the app's `on_stop`, so the service also closes any request whose app is not the running app: a request opened from `on_stop` does not outlive its app.
- Opening a request needs the clock (`no_time` otherwise): the expiry is a real time.
- The table logic (active requests, rate limits, cache, presence slots, judging) takes time, random bytes, signing and verification through function pointers (`vk::requests::hooks`), so the host suite `test_requests` runs it on the laptop.

## Presence

The question presence answers: is the holder of the payee key within radio range and answering *now*? It defeats a replayed REQ (the replayer cannot sign a PROOF). It does not defeat a live relay to the real payee in range; say so.

### Payee: answering CHAL

Route for type 2, in firmware, no app involved. Nothing in a CHAL is signed, so the payee cannot tell an honest payer from a stranger; the rules below bound what a stranger can take, and none of them is a total that a stranger can use up for everyone.

1. `req_id` matches an active request, else drop.
2. The nonce is not one this request has already answered (the last `ANSWERED_NONCES` = 8 are kept), else drop: a payer draws a fresh nonce for every challenge, so a repeat is a recording played back.
3. The sender's MAC has had fewer than `req_max_proofs` (default 8) answers for this request, else drop. Each active request counts up to `CHALLENGERS_PER_REQUEST` = 4 MACs; a fifth replaces the one answered least recently.
4. Queue it. The queue holds `CHAL_QUEUE_LEN` = 4 CHALs for all requests together, one per request and MAC: a newer CHAL from the same MAC for the same request replaces the queued one (as the payer's own re-challenge replaces its slot). When the queue is full the oldest entry is dropped.
5. Answer the **newest** queued CHAL, now if the rate allows, otherwise from the feature's service on a later pass. Before choosing, drop entries whose request has closed, whose age is over `presence_ms` (the payer would judge the answer late), or that steps 2 and 3 now refuse. The rate is global, across requests and senders: at least `req_gap_ms` (default 200) from the **end** of the last signature (measured from the start, a signature slower than the gap would always satisfy it), and a bucket of `ANSWER_BURST` = 3 answers that refills one per `ANSWER_REFILL_MS` = 1000 ms.
6. Sign `pay-proof` over `req_id ‖ nonce ‖ payer_pubkey` (from the CHAL) and unicast PROOF to the sender's MAC. A signature that fails counts toward the gap and the bucket but not as an answer.

The loop is blocked for one signature at a time, and never for more than one per `req_gap_ms`. Sustained, answers take at most 211 ms in every `ANSWER_REFILL_MS`, about a fifth of the loop, however many CHALs arrive; a burst of three takes about 1 s. The time from CHAL to PROOF, including any wait in the queue, is the latency the payer measures. `wallet.request_status(req_id).proofs` counts answers for the request from all senders.

### Payer: challenging and judging

`src/vk/features/requests/presence.{h,cpp}`. A table of 4 slots: `req_id`, `payee_pubkey` (from the REQ), `mac`, `nonce`, `t0_ms`, `result`. The oldest slot is reused when the table is full.

- `wallet.challenge(mac, req_frame)` parses the REQ, fills a slot with a fresh random nonce and result `PENDING`, sets `t0_ms = millis()` and sends CHAL to `mac`.
- Route for type 3: find the `PENDING` slot with that `req_id` whose `mac` equals the sender's; verify the signature over `req_id ‖ nonce ‖ own_pubkey` with the slot's `payee_pubkey`. Valid and `rx_ms − t0_ms ≤ presence_ms` → `PRESENT`. Valid but slower → `LATE`. The first **valid** PROOF decides; later ones for that slot are not checked.
- An invalid PROOF (junk, another key, another nonce such as a recording of an earlier challenge, another payer) is **ignored and counted** (`presence::badProofs(req_id)`, log line `[req] proof bad signature, ignored (<n>)`). It decides nothing: anyone can send one from the payee's MAC, and the real PROOF may still come. If no valid PROOF arrives the slot stays `PENDING`, which the approval shows as amber VERIFIED - NOT PRESENT. The firmware never stores `BAD_SIG`; the check chain still turns a valid PROOF whose key is not the record's into red BAD PROOF ([check 13](../wallet/checks.md#the-check-chain)).
- At most `PROOF_CHECKS_MAX` = 16 PROOFs are verified per challenge (one Ed25519 verification each, 18 ms); further ones are counted unchecked. A re-challenge starts a new count.
- A PROOF whose `rx_ms` is earlier than the slot's `t0_ms` is not judged at all: it answers the nonce of a challenge that has since been replaced. A PROOF from another MAC is ignored without a verification.
- `wallet.presence(req_id)` returns the slot's result as a string for the app's UI. The approval reads the same slot through `presenceLookup` ([checks](../wallet/checks.md#presence-lookup)).

`presence_ms` is a config key. Its default (1500) allows for a software signature on the payee; measurement M1 ([testing](../testing/testing.md#measurements)) sets the real value: the 95th percentile of CHAL→PROOF plus half again.

### Request cache (payer side)

The route for type 1 keeps the last 8 distinct requests seen, keyed by `req_id` and `payee_pubkey`: frame, MAC, RSSI, time seen. An entry is dropped at its expiry or 30 s after it was last heard. `wallet.requests()` returns the cache. When a new entry is made and the running app is not the configured `pay_app`, the route posts a notification ([app host](../platform/app-host.md#notifications)). The route returns false, so a running app also gets the frame.

- A new entry is made only if the REQ's signature (`pay-req:` ‖ `frame[0..signed_len)`) verifies with the `payee_pubkey` in it. That says nothing about who the key belongs to (the check chain checks it against the record); it keeps unsigned junk out of the cache and the Inbox. At most one such check runs per `CACHE_VERIFY_GAP_MS` = 100 ms; a new REQ heard inside the gap is left for its next broadcast.
- The payee sends the same bytes every time, so a frame with a cached `req_id` and `payee_pubkey` but other bytes is ignored. The same `req_id` with another key is a separate entry (an impostor's, which the record check turns red).
- A REQ already past its expiry is never cached or announced. With no clock source only the 30 s rule applies.
- A ninth request replaces the entry that has been silent longest.
- The entry's `mac` (the one the Pay app challenges) is the first MAC it was heard from. A copy of the same bytes from another MAC refreshes the entry's age but takes the MAC only once the entry's MAC has been silent for `CACHE_MAC_HOLD_MS` = 10 s (twice the top of `req_period_ms`'s range). A replayer cannot steer the challenge away from a payee that is still broadcasting.
- The notification body is `<name> <amount> <currency>`, cut to the 39 characters a note holds by shortening the name; the amount is kept whole.

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
| Impostor answers CHAL with a made-up PROOF | signature fails; the PROOF is ignored and counted; no valid one comes | amber, VERIFIED - NOT PRESENT |
| Stranger sends a forged PROOF from the payee's MAC before the real one | ignored; the real PROOF decides | green, as without the stranger |
| Stranger replays a PROOF recorded from an earlier challenge or request | its nonce or `req_id` is not the slot's; ignored | unchanged |
| Stranger sends the payee many CHALs, from one MAC or many | per-MAC budget, global rate, bounded queue; nothing is used up for the honest payer once it stops | green once the flood stops; amber while a flood outpaces the payee's rate |
| Stranger replays a recorded CHAL | its nonce was answered already; dropped unsigned | unchanged |
| Stranger rebroadcasts the payee's REQ from its own MAC | the cache keeps the payee's MAC while the payee broadcasts | green |
| Stranger sends a REQ with the payee's `req_id` and other bytes | ignored (bad signature, or other bytes under the same key) | unchanged |
| App builds a transaction paying someone other than the record's account | destination ≠ `record.solana_ata` | red, WRONG RECIPIENT |
| App builds a transaction for more than the request | amount ≠ `req.amount` | red, WRONG AMOUNT |
| Merchant's attestation is revoked | record says revoked | red, REVOKED |

## Limits

What a stranger in radio range can and cannot do to presence, after the rules above. ESP-NOW source MACs are not authenticated: an ESP32 can send as any MAC, and a badge app with the `espnow` permission can send any frame type from its own MAC.

**Cannot:**

- Make a bad payment green. Presence needs a signature by the payee key over the payer's fresh nonce, and the check chain compares that key with the issuer-signed record.
- Turn an honest exchange red. No invalid PROOF decides anything, so red BAD PROOF comes only from a valid PROOF whose key is not the record's.
- Replay its way in. Every PROOF signs `req_id ‖ nonce ‖ payer_pubkey`; the nonce is fresh per challenge and the slot holds only the latest one, so a recorded PROOF fails for any later challenge, any other request and any other payer. A recorded CHAL is either for a closed request (dropped) or carries an answered nonce (dropped unsigned).
- Use up the payee's answers for everyone. There is no per-request total: once a flood stops, the next honest challenge is answered.
- Freeze the payee's UI. At most one 211 ms signature per `req_gap_ms`, and sustained at most one per `ANSWER_REFILL_MS`.
- Steer the payer's challenge away from a payee that is still broadcasting, or replace a cached request's bytes.

**Can:**

- Deny service while it keeps transmitting: jam the channel, fill upstream's 7-frame receive queue, or send CHALs faster than the payee's answer rate from MACs it makes up. The honest payer then gets no PROOF in time: amber, never red. A jammer can always do this; no rule here prevents it.
- Send as the honest payer's MAC and spend that MAC's `req_max_proofs` answers for the request (about 6 s of CHALs at the default rate). That payer then gets no answer for this request (amber); a new request is not affected. This needs a radio that can send as another MAC, not a badge app.
- Make the payer verify up to `PROOF_CHECKS_MAX` forged PROOFs per challenge (about 290 ms), from the payee's MAC only.
- Fill the request cache with validly self-signed REQs under keys it makes up, faster than the honest REQ is rebroadcast (each one an impostor entry; the record check turns any of them red if chosen). Each costs the payer one verification, at most one per `CACHE_VERIFY_GAP_MS`.
- Relay CHAL and PROOF to a payee who is elsewhere but within relay reach. Presence proves the key holder answered in time, not where it is.

Every limit is a named constant in `features/requests/requests.h` or `presence.h`, or a config key registered in `requests.cpp`:

| Limit | Value | Where |
|---|---|---|
| answers per challenger MAC per request | `req_max_proofs`, default 8 | config key |
| gap from the end of one answer to the next, all requests | `req_gap_ms`, default 200 | config key |
| oldest CHAL still answered; CHAL→PROOF deadline | `presence_ms`, default 1500 | config key |
| answer bucket | `ANSWER_BURST` 3, refilled one per `ANSWER_REFILL_MS` 1000 ms | `requests.h` |
| CHALs waiting, all requests | `CHAL_QUEUE_LEN` 4 | `requests.h` |
| challenger MACs counted per request | `CHALLENGERS_PER_REQUEST` 4 | `requests.h` |
| answered nonces remembered per request | `ANSWERED_NONCES` 8 | `requests.h` |
| PROOFs verified per challenge | `PROOF_CHECKS_MAX` 16 | `presence.h` |
| new-REQ signature checks | one per `CACHE_VERIFY_GAP_MS` 100 ms | `requests.h` |
| silence before a cached request's MAC changes | `CACHE_MAC_HOLD_MS` = 2 × `REQ_PERIOD_MAX_MS` (5000, the top of `req_period_ms`'s range) | `requests.h` |

Refusals are counted in `vk::requests::stats()` (`chal_replayed`, `chal_over_budget`, `chal_displaced`, `chal_stale`, `req_bad_sig`, `req_deferred`, `req_conflict`) and invalid PROOFs in `presence::badProofs(req_id)`. Host suite `test_requests` reproduces each attack (`test_attack_*`).

No frame, signed byte string or sign domain changed for these rules, so the protocol version stays 1 and a badge with them talks to one without.

## Adding a frame type

For an app: pick a free block in 64–255, add it to the type registry above, build and parse the frames in Lua with `lib/vk.lua` helpers. No firmware change.

For firmware: add the struct and the build/parse pair to `vk_frames`, a test to `test_frames`, and one `VK_ESPNOW_ROUTE(...)` line in the feature that handles it. Recipe: [../guides/extending.md](../guides/extending.md#add-an-esp-now-frame-type).
