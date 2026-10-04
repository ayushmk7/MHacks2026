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
| 19 | 1 | flags: bit 0 = dev override used |
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

One record is written per approval; the write takes a few milliseconds and happens in the `RESULT` phase, while the result screen is showing.

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

## Consent

Header magic `VKP1`. Up to 32 records of 40 bytes: `app_id[33]`, 3 bytes of padding, `hash u32` (FNV-1a over the app's sorted, comma-joined permission list). The id is stored in full (upstream ids are up to 32 characters) because consent must not be shared between two apps with a common prefix. See [app host](../platform/app-host.md#consent). Erased by `VKRESET` through a `VK_ON_RESET` listener.

## Adding a store

Create it inside the feature that owns it, following the common rules above, and expose it to apps only through `VK_LUA_FUNCTION` readers. Add a row to the table at the top. If the store should be erased by `VKRESET`, say so here and register a `VK_ON_RESET` listener.

## Tests

Host: `test_stores` runs the three formats against an in-memory file: round trip, ring wrap-around at 128, oldest-replaced at capacity, bad magic recovery. Device: T-STO1 (history survives a reboot), T-STO2 (an app cannot open `/vk/history.bin` through `badge.storage`).
