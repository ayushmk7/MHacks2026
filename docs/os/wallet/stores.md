# Stores

The data the firmware keeps on the badge's filesystem: the signature history, the contacts and the consent list. Apps read them through functions and can never write them.

## Where

LittleFS, directory `/vk/` (C path `FS_ROOT "/vk/"`, that is `/littlefs/vk/`). Upstream lets apps and the push protocol write only under `/apps/<id>/` (finding F10), so nothing outside the firmware can reach `/vk/`.

| File | Owner | Format |
|---|---|---|
| `/vk/history.bin` | `features/history` | ring of fixed records |
| `/vk/contacts.bin` | `features/contacts` | array of fixed records |
| `/vk/consent.bin` | `host/consent` | array of fixed records |

Common rules: little-endian; an 8-byte header `magic[4]`, `version u16`, `count u16`; a file with a wrong magic or version is renamed to `<name>.bad` and a new empty file is started; writes go to `<name>.tmp` and are renamed over the original, except the history ring, which rewrites one record in place.

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

Every approval outcome, written by a `VK_ON_APPROVAL` listener: signed, cancelled, timed out, blocked or failed. Confirmations that are not signatures (consent, setting changes) are logged too, with domain `confirm`.

Header magic `VKH1`. The header's `count` is the number of valid records (up to 128) and is followed by `head u16` (index of the next record to write) and 2 bytes of padding, making the header 12 bytes. Records are 192 bytes:

| Offset | Size | Field |
|---|---|---|
| 0 | 4 | `time` u32, unix seconds (0 if the clock had no source) |
| 4 | 12 | `domain`, NUL-padded |
| 16 | 1 | `outcome`: 1 signed, 2 cancelled, 3 timeout, 4 blocked, 5 failed, 6 approved (a confirmation that is not a signature) |
| 17 | 1 | `reason` (`vk_reason_t`) |
| 18 | 1 | `severity`: 0 green, 1 amber, 2 red |
| 19 | 1 | flags: bit 0 = dev override used. Set only when the override led to a signature; an override screen that was cancelled is `blocked` with the flag clear |
| 20 | 8 | `amount` u64 raw units (0 if not a payment) |
| 28 | 1 | `decimals` |
| 29 | 9 | `symbol`, NUL-padded |
| 38 | 32 | `recipient` (device key from the record, else the destination account, else zeros) |
| 70 | 33 | `recipient_name`, NUL-padded |
| 103 | 24 | `app_id`, NUL-padded, truncated to 23 characters (display only) |
| 127 | 64 | `sig` (zeros unless signed) |
| 191 | 1 | padding |

```cpp
// src/vk/features/history/history.h
namespace vk::history {
struct Entry { /* the fields above, unpacked */ };
size_t count();
bool at(size_t newestFirstIndex, Entry &out);
}
```

The header also declares what the listener, the Lua binding and the host suite use: `Cursor` with `open(Cursor &)` and `at(cursor, i, out)` (the file header is read once for a reader of many records), `entryFor(outcome, unix_s)`, `append(entry)`, `outcomeName(code)`, the constants `OUTCOME_*`, `FILE_PATH`, `RING_CAPACITY`, `HEADER_SIZE`, `RECORD_SIZE`, and the function pointer `unixTime` (the clock on the badge; null in a host suite).

Which outcome is written:

- approved with a signature → `signed`; approved without one (a confirmation) → `approved`;
- closed with reason `cancelled` → `cancelled`; `timeout` → `timeout`; `sign_failed` → `failed`;
- anything else → `blocked`. A red approval reports its cause as the reason however it is closed, so a red screen closed with CANCEL or left to time out is `blocked`, with the cause in `reason`.

Writing: the record goes into slot `head` first and the header second. If the record write fails nothing has changed. If only the header write fails, the header still describes the records that were there before; in a full ring the slot just written is the oldest record's, so the oldest entry shows the new record's contents until the next write. The two-write format cannot avoid that. `/vk/` is created when missing. A failed write is logged (`[vk] history: write failed …`) and ignored: the signature was already made and stays valid.

A file with a wrong magic or version is renamed on the next **write**, not when it is read: until then readers report an empty history and change nothing. If the rename itself fails, the bad file is overwritten, so history keeps working.

One record is written per approval, in the `RESULT` phase, while the result screen is showing. **The write is not quick, and it grows with the file.** LittleFS rewrites a file from the touched block to its end, and every append also rewrites the header at offset 0, so each append rewrites the whole file. Measured on the badge (integrator I3, 2026-10-03, dev build; the dev profile logs each append as `[vk] history write <ms> ms`):

| Records in the file | One append |
|---|---|
| 0 to 16 | 22 to 75 ms |
| 16 to 48 | 41 to 102 ms |
| 48 to 112 | 60 to 231 ms (about 145 ms at 60 records) |
| 112 to 128 | 134 to 181 ms |
| full ring (every later write) | 245 to 320 ms, one of 32 at 406 ms |

The loop is blocked for that time. It fits inside the 800 ms result screen, and no key or frame is acted on during `RESULT` anyway, but ESP-NOW frames that arrive meanwhile wait in upstream's 7-frame queue (U12). If this has to shrink, keep `count` and `head` out of the ring file (a second small file, or derive them by scanning at boot) so that an append touches one block.

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

The header also declares, for the feature's own bindings and its host suite: `Cursor` / `open` / `at(cursor, …)` (several reads of one file), `hooks` (time, random bytes, sign and verify as function pointers), `hello`, `card`, `accept`, `validCardBytes` (the `contact` domain's validator), `cleanName`, `reset`, and the path and size constants. `contacts.cpp` holds one static 4,616-byte buffer, the whole file image, used for writes.

## Consent

Header magic `VKP1`. Up to 32 records of 40 bytes: `app_id[33]`, 3 bytes of padding, `hash u32` (FNV-1a over the app's sorted, comma-joined permission list). The id is stored in full (upstream ids are up to 32 characters) because consent must not be shared between two apps with a common prefix. See [app host](../platform/app-host.md#consent). Erased by `VKRESET` through a `VK_ON_RESET` listener.

"Oldest" is file order: a new entry is appended and, at 32, the first is dropped; approving an app again (its permissions changed) removes its old entry and appends the new one. The file is read once and kept in RAM. If the rename of `.tmp` over the file is refused, the file is written in place.

## Adding a store

Create it inside the feature that owns it, following the common rules above, and expose it to apps only through `VK_LUA_FUNCTION` readers. Add a row to the table at the top. If the store should be erased by `VKRESET`, say so here and register a `VK_ON_RESET` listener.

## Tests

Host: `test_stores` runs the three formats against an in-memory file: round trip, ring wrap-around at 128, oldest-replaced at capacity, bad magic recovery; for the history also a failed record write, a failed header write, a missing `/vk/`, the listener for all six outcome codes, and the balance reply scanner. `test_contacts` and `test_consent` do the same for their stores (exact file bytes, failing writes and renames, order at capacity). Device: `t_con_single.py` (one badge: a contact saved, renamed over the existing file, kept over a reset, removed), T-STO1 (history survives a reboot), T-STO2 (an app cannot open `/vk/history.bin` through `badge.storage`).
