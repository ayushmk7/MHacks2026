# Stores

The data the firmware keeps on the badge's filesystem: the signature history, the contacts and the consent list. Apps read them through functions; the only write an app can cause is a received-payment row, which the firmware builds from a transaction it checked itself (`wallet.record_received`).

## Where

LittleFS, directory `/vk/` (C path `FS_ROOT "/vk/"`, that is `/littlefs/vk/`). Upstream lets apps and the push protocol write only under `/apps/<id>/` (finding F10), so nothing outside the firmware can reach `/vk/`.

| File | Owner | Format |
|---|---|---|
| `/vk/history.bin` | `features/history` | ring of fixed records |
| `/vk/contacts.bin` | `features/contacts` | array of fixed records |
| `/vk/consent.bin` | `host/consent` | array of fixed records |

Common rules: little-endian; an 8-byte header `magic[4]`, `version u16`, `count u16`; a file with a wrong magic or version is renamed to `<name>.bad` and a new empty file is started; writes go to `<name>.tmp` and are renamed over the original, except the history ring, which rewrites records in place (its header is 12 bytes: `head` follows `count`).

How the three stores apply them (the same in each):

- A bad file (wrong magic or version, a count above the capacity or above what the file's size holds) reads as empty and is left alone by readers; the next write renames it to `<name>.bad` and starts a new file.
- The rename of `<name>.tmp` over an existing `<name>` relies on `fileio`'s `renameFile` replacing its target. It does on the badge's LittleFS (checked by `t_con_single.py`: a contact renamed, so written over an existing file, and read back after a reset).

The three stores have three owners that may not include each other, so they share one file layer, `src/vk/core/fileio.{h,cpp}`. A store never calls the filesystem directly: every read, write, rename and remove goes through `vk::fileio::ops`. The host tests install an in-memory implementation behind the same pointer ([testing](../testing/testing.md#host-tests)).

```cpp
// src/vk/core/fileio.h
namespace vk::fileio {
struct Ops {
  bool (*exists)(const char *path);
  long (*size)(const char *path);
  bool (*read)(const char *path, size_t offset, uint8_t *out, size_t len);
  bool (*writeAll)(const char *path, const uint8_t *data, size_t len);
  bool (*writeAt)(const char *path, size_t offset, const uint8_t *data, size_t len);
  bool (*renameFile)(const char *from, const char *to);
  bool (*removeFile)(const char *path);
  bool (*makeDir)(const char *path);
};
extern const Ops *ops;
}
```

Paths are LittleFS-relative (`"/vk/history.bin"`). The firmware implementation prepends `FS_ROOT` and uses POSIX stdio.

A filesystem image flashed at `0x670000` replaces the whole volume, these files included.

## History

The signature log. **Every signature the badge key makes leaves a record**, and so does every approval outcome. Three kinds of row share one ring:

| Kind | Written by | When | What it holds |
|---|---|---|---|
| approval | a `VK_ON_APPROVAL` listener | each approval outcome, in the `RESULT` phase: signed, cancelled, timed out, blocked or failed; confirmations that are not signatures (consent, setting changes) with domain `confirm` | domain, outcome, reason, severity, amount and token, the other party, app, request id, signature |
| auto | a `VK_ON_SIGN` listener, called by `signRaw()` | every signature of an auto domain (`pay-req`, `pay-proof`, `contact`, `store-reg`, and any future one), batched | domain, outcome (`signed` or `failed`), and for each signature its time and the first 16 bytes of SHA-256 of what was signed |
| received | `wallet.record_received` | an incoming payment the firmware checked against its transaction ([solana-payments](solana-payments.md#checking-a-received-payment)) | payer, amount and token, request id, transaction signature, app |

### Why every signature is in it

`signRaw()` is the one function that asks the key for a signature ([signing](signing.md#every-signature-is-logged)). After the key answers (signed or refused) it calls every `VK_ON_SIGN` listener with the domain, the exact bytes signed and the result. The history's listener ignores button domains (their approval row records them, with how the approval ended) and queues the rest. A new auto domain is logged without any change here; a new button domain is logged by the approval listener. A request refused before the key was asked (self-check, length, validator) made no signature and is not logged.

### Format (version 2)

Header magic `VKH1`, version 2. The header's `count` is the number of valid records (up to 128) and is followed by `head u16` (index of the next record to write) and 2 bytes of padding, making the header 12 bytes. Records are 192 bytes. Bytes 0 to 19 are common:

| Offset | Size | Field |
|---|---|---|
| 0 | 4 | `time` u32, unix seconds (0 if the clock had no source); an auto row: its newest signature's |
| 4 | 12 | `domain`, NUL-padded |
| 16 | 1 | `outcome`: 1 signed, 2 cancelled, 3 timeout, 4 blocked, 5 failed, 6 approved (a confirmation that is not a signature), 7 received |
| 17 | 1 | `reason` (`vk_reason_t`) |
| 18 | 1 | `severity`: 0 green, 1 amber, 2 red (0 for auto and received rows) |
| 19 | 1 | flags: bit 0 = dev override used (set only when the override led to a signature; an override screen that was cancelled is `blocked` with the flag clear); bit 1 = auto row; bit 2 = `req_id` present |

Approval and received rows (flags bit 1 clear):

| Offset | Size | Field |
|---|---|---|
| 20 | 8 | `amount` u64 raw units (0 if not a payment) |
| 28 | 1 | `decimals` |
| 29 | 9 | `symbol`, NUL-padded |
| 38 | 32 | `recipient`: the other party. Approval: the device key from the record, else the destination account, else zeros. Received: the payer's key (the transaction's fee payer) |
| 70 | 33 | `recipient_name`, NUL-padded (empty for a received row: only an issuer-signed name is ever stored) |
| 103 | 16 | `app_id`, NUL-padded, truncated to 15 characters (display only) |
| 119 | 8 | `req_id` (when flags bit 2): the request this payment answered, or was received for |
| 127 | 64 | `sig`: the signature made (signed approval), the transaction's signature (received), else zeros |
| 191 | 1 | padding |

Auto rows (flags bit 1 set): consecutive signatures of one domain with one outcome share a row, up to 8.

| Offset | Size | Field |
|---|---|---|
| 20 | 1 | `count`: signatures in the row, 1 to 8 |
| 21 | 3 | padding |
| 24 | 8 × 20 | per signature: `time` u32, then `digest[16]`, the first 16 bytes of SHA-256(prefix ‖ bytes) |
| 184 | 8 | padding |

A presence round of up to 8 PROOFs is one row, so a payee answering CHALs cannot push the payer's own approvals out of the ring 8 times as fast.

### Version 1 files

Version 1 had the same header and the same first 103 bytes; `app_id` was 24 bytes (23 characters) and only flags bit 0 existed. A version 1 record is therefore a valid version 2 approval row: neither new flag is set, so bytes 119 to 126 (the tail of a long version 1 app id) are not read as a request id, and `app_id` reads as its first 15 characters. **Migration:** `open()` accepts versions 1 and 2; nothing is converted on read; the first append rewrites the header (as every append does) with version 2. No record is rewritten, so the migration costs nothing and cannot lose a row. A firmware that only knows version 1 treats a version 2 file as bad (renamed `.bad` on its next write): going back to an older firmware loses the log.

### API

```cpp
// src/vk/features/history/history.h
namespace vk::history {
struct AutoItem { uint32_t time; uint8_t digest[16]; };
struct Entry {
  uint32_t time; char domain[12]; uint8_t outcome, reason, severity;
  bool dev, automatic, has_req_id;
  uint64_t amount; uint8_t decimals; char symbol[9];
  uint8_t recipient[32]; char recipient_name[33]; char app_id[16];
  uint8_t req_id[8]; uint8_t sig[64];
  uint8_t auto_count; AutoItem items[8];
};
size_t count();
bool at(size_t newestFirstIndex, Entry &out);

enum class Kind : uint8_t { APPROVAL, AUTO, RECEIVED };
Kind kindOf(const Entry &entry);               // auto (flag), received (outcome 7), else approval
const char *kindName(Kind kind);               // "approval", "auto", "received"
const char *outcomeName(uint8_t outcome);      // "signed" ... "approved", "received"

struct Cursor { uint16_t count; uint16_t head; uint16_t version; };
bool open(Cursor &out);
bool at(const Cursor &cursor, size_t newestFirstIndex, Entry &out);

struct Received { uint8_t payer[32]; uint64_t amount; uint8_t decimals; char symbol[9];
                  uint8_t req_id[8]; uint8_t sig[64]; char app_id[16]; };
enum class ReceiveResult : uint8_t { WRITTEN, ALREADY, FAILED };
ReceiveResult recordReceived(const Received &payment, uint32_t unix_s);   // ALREADY: that signature is in the ring

bool spentWithin(const vk_token_t *tokens, size_t count, uint32_t now, uint64_t out[VK_MAX_TOKENS]);
void eraseAll();                               // the VK_ON_RESET listener
}
```

A reader (the History app through `wallet.history`, the Wallet app natively) tells the kinds apart with `kindOf` (Lua: the `kind` field). The header also declares what the listeners and the host suite use: `entryFor(outcome, unix_s)`, `append(entry)`, `onSign(event)`, `pendingCount()`, `flushDue(nowMs)`, `flushPending()`, `droppedCount()`, the constants `OUTCOME_*`, `FILE_PATH`, `RING_CAPACITY`, `HEADER_SIZE`, `RECORD_SIZE`, `FORMAT_VERSION`, `QUEUE_CAPACITY`, `FLUSH_*`, and the function pointers `unixTime` (the clock on the badge) and `millisNow` (the loop clock); both are null in a host suite.

### Which outcome is written

- approved with a signature → `signed`; approved without one (a confirmation) → `approved`;
- closed with reason `cancelled` → `cancelled`; `timeout` → `timeout`; `sign_failed` → `failed`;
- anything else → `blocked`. A red approval reports its cause as the reason however it is closed, so a red screen closed with CANCEL or left to time out is `blocked`, with the cause in `reason`;
- an auto signature → `signed`, or `failed` (reason `sign_failed`) when the key refused;
- `wallet.record_received` → `received`, reason `ok`.

### Writing

Records go into slot `head` first and the header second. If a record write fails nothing has changed. If only the header write fails, the header still describes the records that were there before; in a full ring the slots just written are the oldest records', so those entries show the new contents until the next write. The two-write format cannot avoid that. `/vk/` is created when missing. A failed write is logged (`[vk] history: write failed …`) and ignored: the signature was already made and stays valid. A signed payment whose approval row could not be written is kept in a RAM list (four entries) so the daily limit still counts it ([checks](checks.md#daily-limit)).

A file with a wrong magic or an unknown version is renamed on the next **write**, not when it is read: until then readers report an empty history and change nothing. If the rename itself fails, the bad file is overwritten, so history keeps working.

**Approval rows** are written in the `RESULT` phase, while the result screen is showing. **The write is not quick, and it grows with the file.** LittleFS rewrites a file from the touched block to its end, and every append also rewrites the header at offset 0, so each append rewrites the whole file. Measured on the badge (integrator I3, 2026-10-03, dev build; the dev profile logs each append as `[vk] history write <ms> ms`):

| Records in the file | One append |
|---|---|
| 0 to 16 | 22 to 75 ms |
| 16 to 48 | 41 to 102 ms |
| 48 to 112 | 60 to 231 ms (about 145 ms at 60 records) |
| 112 to 128 | 134 to 181 ms |
| full ring (every later write) | 245 to 320 ms, one of 32 at 406 ms |

The loop is blocked for that time. It fits inside the 800 ms result screen, and no key or frame is acted on during `RESULT` anyway, but ESP-NOW frames that arrive meanwhile wait in upstream's 7-frame queue (U12). If this has to shrink, keep `count` and `head` out of the ring file (a second small file, or derive them by scanning at boot) so that an append touches one block.

**Auto signatures are never written while the signer waits.** An auto signature is often on a deadline (a PROOF must reach the payer within `presence_ms`), and one append can take 300 ms. So the `VK_ON_SIGN` listener only copies the domain, the outcome, the time and the 16-byte digest into a RAM queue of 32 (a SHA-256 of at most 1248 bytes: microseconds) and returns. The queue is written, every queued signature in one append (one header write for the batch), by:

- the history service's loop step, when no approval is on the screen and either no auto signature was made for 2 s (`FLUSH_QUIET_MS`), the oldest queued one is 10 s old (`FLUSH_MAX_AGE_MS`), or 24 are queued (`FLUSH_HIGH_WATER`);
- the approval listener and `record_received`, **before** their own row, so the ring stays in the order things were signed;
- the listener itself, only when the queue is full (32), as a last resort.

After a failed write the queue is kept and the next try waits 5 s (`FLUSH_RETRY_MS`). If the queue is full and cannot be written, the oldest entry is dropped and counted (`droppedCount()`, logged). The dev profile logs each batch as `[vk] history auto flush <n> ok <ms> ms`.

**Guarantee.** A signing reply never waits for the file. Every auto signature reaches the file within about 10 s of being made (or when the next approval closes, whichever is first), unless storage is failing. The badge has no power-off hook, so pulling the power loses at most the signatures queued in the last 10 s, never more than 32; approval and received rows are on file before their screen or call returns.

## Contacts

Header magic `VKC1`. Up to 64 records of 72 bytes; when full, the oldest is replaced.

| Offset | Size | Field |
|---|---|---|
| 0 | 32 | `pubkey` |
| 32 | 33 | `name`, NUL-padded |
| 65 | 1 | flags (reserved, 0) |
| 66 | 4 | `added` u32, unix seconds |
| 70 | 2 | padding |

A contact is written only by `wallet.contact_accept` after the card's signature, nonce and addressee were verified ([protocol](../protocol/espnow.md#contact_card)). A second card from the same key updates the name. Contact names are self-chosen and are never used by the approval screen.

Order: records are kept in insertion order, index 0 the oldest (the header has no head index and `added` can be 0 when the clock had no source, so "oldest" is the position in the file). The 65th contact drops record 0. An upsert of a known key changes only the name; its place and its `added` stay. `wallet.contacts()` is therefore oldest first. If the file cannot be written the contact is not saved, `[contact] write failed (…)` is logged and `contact_accept` returns `unsupported`.

```cpp
// src/vk/features/contacts/contacts.h
namespace vk::contacts {
struct Entry { uint8_t pubkey[32]; char name[33]; uint32_t added; };
size_t count();
bool at(size_t index, Entry &out);
bool upsert(const uint8_t pubkey[32], const char *name);
bool remove(const uint8_t pubkey[32]);
}
```

The header also declares, for the feature's own bindings and its host suite: `Cursor` / `open` / `at(cursor, …)` (several reads of one file), `hooks` (time, random bytes, sign and verify as function pointers), `hello`, `card`, `accept`, `validCardBytes` (the `contact` domain's validator), `cleanName`, `reset`, `eraseAll` (the `VK_ON_RESET` listener), and the path and size constants.

`contacts.cpp` holds one static 4,616-byte buffer, the whole file image, used for writes.

Erased by `VKRESET` through a `VK_ON_RESET` listener in `contacts.cpp`: the file, its `.tmp` and `.bad`, and the swap nonce.

## Consent

Header magic `VKP1`. Up to 32 records of 40 bytes: `app_id[33]`, 3 bytes of padding, `hash u32` (FNV-1a over the app's sorted, comma-joined permission list). The id is stored in full (upstream ids are up to 32 characters) because consent must not be shared between two apps with a common prefix. See [app host](../platform/app-host.md#consent). Erased by `VKRESET` through a `VK_ON_RESET` listener.

"Oldest" is file order: a new entry is appended and, at 32, the first is dropped; approving an app again (its permissions changed) removes its old entry and appends the new one. The file is read once and kept in RAM. If the rename of `.tmp` over the file is refused, the file is written in place.

## Reset

A wallet reset (`VKRESET`, or the Wallet app's Reset page, both through `vk::config::requestReset()` and the on-badge confirmation) erases the config namespace and then runs every `VK_ON_RESET` listener. Each store registers its own; there is no central list:

| Store | Listener | Erases |
|---|---|---|
| history | `history.cpp` → `vk::history::eraseAll` | `history.bin`, `.bad`, the auto queue, the unlogged-payment list |
| contacts | `contacts.cpp` → `vk::contacts::eraseAll` | `contacts.bin`, `.tmp`, `.bad`, the swap nonce |
| consent | `consent.cpp` → `vk::host::consent::eraseAll` | `consent.bin`, `.tmp`, `.bad` |

The listeners run after the confirmation's own approval row was written, so the log is empty afterwards. The device key is not touched by a reset. **New identity** (Settings → Identity) regenerates the key only and does not run the reset listeners; its screen says "Keeps apps, history, contacts".

## Adding a store

Create it inside the feature that owns it, following the common rules above, and expose it to apps only through `VK_LUA_FUNCTION` readers. Add a row to the table at the top. If the store holds anything personal, register a `VK_ON_RESET` listener in the store's own file and add it to the table under [Reset](#reset).

## Tests

Host: `test_stores` runs the three formats against an in-memory file: round trip, ring wrap-around at 128, oldest-replaced at capacity, bad magic recovery; for the history also a failed record write, a failed header write, a missing `/vk/`, the listener for all six outcome codes, a version 1 file read as it is and upgraded by its next append with no record rewritten, auto rows (grouping 8 to a row, the digests, the flush policy, a failed and retried write, a full queue), the order of auto and approval rows, received rows (layout, duplicates, failure), the daily total (window, clock unset, other decimals, cancelled and received excluded, a wrapped ring, an unreadable file, unlogged payments), the reset listener, and the balance reply scanner. `test_domains` checks that `signRaw` reports every attempt to the `VK_ON_SIGN` listeners and nothing it refused before asking the key. `test_contacts` and `test_consent` do the same for their stores (exact file bytes, failing writes and renames, order at capacity, and the reset listener). Device: `t_con_single.py` (one badge: a contact saved, renamed over the existing file, kept over a reset, removed), T-STO1 (history survives a reboot), T-STO2 (an app cannot open `/vk/history.bin` through `badge.storage`).
