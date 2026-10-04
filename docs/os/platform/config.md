# Configuration, provisioning and serial commands

Nothing that identifies a deployment is compiled into the firmware: no issuer key, no mint, no URL, no Wi-Fi name, no limit. Every such value is a **config key** stored in NVS and set by provisioning. Files: `src/vk/core/config.{h,cpp}`, `src/vk/core/serial.{h,cpp}`, `scripts/vkdev.py`, `provision.public.env` (the pinned public values the tool provisions with).

## Config store

NVS namespace `vkconf`. Every value is stored as text and parsed on first read (results are cached; a write clears the cache). NVS key names are at most 15 characters.

```cpp
// src/vk/core/config.h
namespace vk::config {

enum class Type : uint8_t { STR, U32, KEY32, TOKENS };
enum : uint8_t { F_NONE = 0, F_SECURE = 1, F_REQUIRED = 2 };

struct ConfigKey : Registered<ConfigKey> {
  const char *name;            // NVS key, <= 15 chars
  Type type;
  const char *def;             // default as text; nullptr = no default
  uint8_t flags;
  uint32_t min, max;           // U32: allowed range. STR: min and max length. Others: unused
  const char *help;            // one line, shown by VKKEYS
};
#define VK_CONFIG_KEY(ident, name, type, def, flags, min, max, help) \
  static vk::config::ConfigKey vk_config_##ident{name, type, def, flags, min, max, help}

void begin();                                   // open NVS, load `provisioned`
bool provisioned();

String  text(const char *name);                 // stored text, or the default, or ""
uint32_t u32(const char *name);
bool    key32(const char *name, uint8_t out[32]);            // base58 text -> 32 bytes
size_t  tokens(vk_token_t out[VK_MAX_TOKENS]);               // parsed token table

enum class SetResult : uint8_t { OK, PENDING, UNKNOWN_KEY, INVALID, UNAVAILABLE, STORAGE };
// Validates by type and range. Unprovisioned: writes. Provisioned: non-secure keys write;
// secure keys open a firmware confirmation and return PENDING (the write happens on approval).
SetResult set(const char *name, const char *value);

bool commit(String &missing);                   // all REQUIRED keys set -> provisioned = 1
void requestReset();                            // firmware confirmation, then erase the namespace and run every reset listener

// Set at boot by the approval engine. Raises the "Change setting" confirmation and calls done(approved).
// Null until the engine exists: a secure change then returns SetResult::UNAVAILABLE.
extern bool (*confirmChange)(const char *key, const char *oldText, const char *newText, void (*done)(bool approved));

struct ResetListener : Registered<ResetListener> { void (*fn)(); explicit ResetListener(void (*f)()) : fn(f) {} };
#define VK_ON_RESET(ident, fn) static vk::config::ResetListener vk_on_reset_##ident(fn)

}  // namespace vk::config
```

(`ConfigKey` needs a constructor taking its seven fields in declaration order; the macro's first argument is only the C++ identifier.) Every NVS write's result is checked: the NVS partition is small (20 KB) and shared with upstream's settings and with apps' `badge.storage.kv`, so it can fill up.

Details of the store, as built (WP10):

- **Before `begin()`.** Every accessor calls `begin()` itself the first time (it is idempotent), because the boot screen reads the theme before `vk::begin()` runs.
- **Stored text that no longer validates** (the range of a key changed between firmware versions) is ignored: the accessors return the default, and a required key in that state counts as missing.
- **Length.** A value longer than 255 characters is refused as invalid, whatever the key's own range.
- **Empty values** can be stored (a key with a non-empty default, such as `pay_app`'s, can be replaced by any allowed value, and a key whose range starts at 0 can be set to empty). `Preferences::putString` reports 0 bytes for an empty string whether or not it was written, so an empty value is checked by reading it back.
- **The provisioned flag** is the NVS key `_provisioned` (one byte) in `vkconf`. `commit()` returns false with `missing` empty when writing it fails.
- **`set()` on a provisioned badge, secure key:** `UNAVAILABLE` while `confirmChange` is null, and also when `confirmChange` returns false because another approval is on screen (a change already waiting for its confirmation is kept). Otherwise `PENDING`.
- **`requestReset()`** uses the same pointer, called as `confirmChange("(reset)", "", "", done)`, so `core/` includes no wallet header. The confirmation is raised whether or not the badge is provisioned. `requestReset()` returns nothing: a caller that needs to know whether the confirmation opened (the Wallet app's Reset page) looks at `vk::wallet::approval::active()` straight after the call.
- **Parsers.** `parseStr`, `parseU32`, `parseKey32`, `parseTokens` and `validate` are pure functions declared in `config.h` and covered by `test_config`. `find(name)` returns the registered key.
- **Key rules.** `KeyRule`, `VK_CONFIG_RULE` and `ruleFor`: see [Key rules](#key-rules).
- **Cache.** 32 slots; keys beyond that still work but are read from NVS on every call.
- Avoid `CHANGE`, `RISING`, `FALLING` and `DISABLED` as enumerator names anywhere: the Arduino core defines them as macros.

Type text forms:

| Type | Text form | Example |
|---|---|---|
| `STR` | UTF-8, printable ASCII only | `https://api.devnet.solana.com` |
| `U32` | decimal | `45` |
| `KEY32` | base58 of 32 bytes | `2SXh6Xng9b1qQBCEn3pt2Ucwcb4ibBWwBKuuTwNgEzJy` |
| `TOKENS` | see [Token table](#token-table) | `3VmWnzfWTfS5UGkwEjbMZnpjd1DPtKyMcwKeJc4QM9wP:2:HACK:100.00:1000.00` |

The `KEY32` and `TOKENS` examples are the pinned devnet values (final): the issuer key and the token table every badge is provisioned with ([Pinned values](#pinned-values)).

## Keys

Each key is registered by the code that uses it. This table is the complete list; a new key is a new row here and one `VK_CONFIG_KEY` line.

| Key | Type | Default | Flags | Range | Owner | Meaning |
|---|---|---|---|---|---|---|
| `issuer_key` | KEY32 | — | secure, required | | `solana_pay` | public key that signs registry records |
| `tokens` | TOKENS | — | secure, required | 1–3 entries | `solana_pay` | payment tokens with caps |
| `rpc_url` | STR | — | required | 8–128 | `core` | Solana JSON-RPC endpoint (used by the balance feature and by `vk.rpc` in apps) |
| `listener_url` | STR | (empty) | | 0–128 | `core` | base URL of the backend's badge listener, e.g. `http://192.168.4.20:8788` (placeholder: the address changes with the venue) |
| `display_name` | STR | (empty) | | 0–32 | `core` | name this badge claims in requests and contact cards; empty means upstream's device name |
| `ntp_server` | STR | `pool.ntp.org` | | 3–64 | `core` (clock) | SNTP host. Read once, when Wi-Fi first connects: a change takes effect at the next boot |
| `utc_offset` | STR | (empty) | | 0–6, and its [rule](#key-rules) | `core` (clock) | the venue's offset from UTC for **display only**: `+HH:MM` or `-HH:MM`, a whole quarter hour from `-12:00` to `+14:00`; empty is UTC. The header and Settings → Badge print the time moved by it; `vk::clock::now()`, the clock's source and every wallet check stay UTC. Settings → Badge → `Time zone` writes it |
| `approval_tmo_s` | U32 | 45 | secure | 10–120 | approval | approval timeout; keep below the ~60 s blockhash lifetime |
| `hold_ms` | U32 | 3000 | secure | 1000–10000 | approval | hold-SELECT duration |
| `record_ttl_s` | U32 | 30 | secure | 5–3600 | `solana_pay` | maximum age of a registry record under SNTP |
| `day_limit` | STR | — | secure | 0–96 | `solana_pay` | rolling 24-hour spending limit per token: `SYMBOL:amount[,SYMBOL:amount…]` in display units, e.g. `HACK:50.00`. Unset or empty: no daily limit (there is no compiled-in amount); a token not named, or named with `0`: no limit for it. A value that does not parse blocks every payment. Over the limit the approval is red DAILY LIMIT (`over_daily`); the total is read from the signature log ([checks](../wallet/checks.md#daily-limit)). Not provisioned by default; set with `VKSET day_limit HACK:50.00` |
| `presence_ms` | U32 | 1500 | secure | 50–5000 | `requests` | CHAL→PROOF deadline |
| `req_ttl_s` | U32 | 60 | | 10–600 | `requests` | lifetime of a payment request |
| `req_period_ms` | U32 | 1000 | | 250–5000 | `requests` | REQ rebroadcast period |
| `req_max_proofs` | U32 | 8 | | 1–64 | `requests` | proofs answered per request |
| `req_gap_ms` | U32 | 200 | | 0–5000 | `requests` | minimum time between proofs |
| `balance_poll_s` | U32 | 15 | | 0–3600 | `balance` | balance poll period; 0 disables |
| `balance_max_s` | U32 | 600 | | 15–3600 | `balance` | longest wait between polls after failed ones: each failure doubles the wait up to this, never below `balance_poll_s` ([ui](../ui/ui.md#balance)) |
| `pay_app` | STR | `pay` | | 1–24 | `requests` | app opened from a payment-request notification |
| `dim_s` | U32 | 30 | | 0–3600 | `ui` (`screen_power.cpp`) | seconds with no activity before the backlight dims; 0 never; no dim phase when not below a non-zero `sleep_s` ([ui](../ui/ui.md#screen-dim-and-sleep)) |
| `sleep_s` | U32 | 120 | | 0–3600 | `ui` (`screen_power.cpp`) | seconds with no activity before the backlight goes off; 0 never. Settings → Display → `Sleep after` writes it |
| `dim_pct` | U32 | 25 | | 1–100 | `ui` (`screen_power.cpp`) | the dimmed backlight, percent of the awake level (never 0 from a lit screen) |
| `awake_usb` | U32 | 0 | | 0–1 | `ui` (`screen_power.cpp`) | 1: never dim or sleep while on external power |
| `crit_sleep_s` | U32 | 30 | | 0–3600 | `ui` (`screen_power.cpp`) | at critical battery, sleep after this many seconds with no activity when that is sooner than `sleep_s`; 0 off ([ui](../ui/ui.md#low-battery)) |
| `batt_low_pct` | U32 | 20 | | 0–90 | `ui` (`battery.cpp`) | measured battery percent at or below which the badge warns once (notification, LED blink, `LOW` in the header); 0 off |
| `batt_crit_pct` | U32 | 8 | | 0–50 | `ui` (`battery.cpp`) | measured battery percent at or below which the battery is critical (warning, `CRIT` in the header, shorter sleep); 0 off |
| `batt_hyst_pct` | U32 | 3 | | 0–20 | `ui` (`battery.cpp`) | a battery level is left only this many percent above its threshold |
| `theme` | STR | (empty) | | 0–24 | `ui` | active theme: `receipt-light` (also when empty) or `receipt-dark` ([ui](../ui/ui.md#theme)); Settings → Theme writes it |
| `repo_url` | STR | (empty) | | 0–120 | shell (`pages/page_about.cpp`) | the link shown as a QR code on Settings → About and in Home's stub: the project's repository ([shell](../ui/shell.md#about)). Empty: About says `no link set` and Home shows its barcode. A public value; set it with `VKSET repo_url <url>` |
| `setup_done` | U32 | 0 | | 0–1 | shell (`pages/page_setup.cpp`) | 1: the setup checklist does not open by itself at boot. Set when a person leaves the checklist with CANCEL ([shell](../ui/shell.md#setup)); erased by `VKRESET` with the rest, so a reset badge shows the checklist once more |

Every key that is neither secure nor required can also be changed on the badge, on Settings → Advanced ([shell](../ui/shell.md#advanced)); `display_name`, `utc_offset` and `balance_poll_s` also on Settings → Badge. Secure and required keys are never changed from the buttons.

There is no `home_app` key: the launcher is the shell itself, not an app ([shell](../ui/shell.md)). Secure keys are the ones whose change could turn a blocked payment into an approved one. Wi-Fi credentials are upstream settings, not config keys ([Wi-Fi](#wi-fi)).

### Token table

Text form: entries separated by `,`; each entry is `mint:decimals:symbol:cap:max`.

| Field | Form |
|---|---|
| `mint` | base58 mint address |
| `decimals` | 0–9 (exactly one digit) |
| `symbol` | 1–4 characters of `[A-Z0-9]` (it must fit a REQ frame's 4-byte currency field) |
| `cap` | display units (`100.00`); above this SELECT becomes a hold; `0` = no cap |
| `max` | display units; above this the payment is blocked; `0` = no max |

The pinned devnet value (final), one token: `3VmWnzfWTfS5UGkwEjbMZnpjd1DPtKyMcwKeJc4QM9wP:2:HACK:100.00:1000.00`. The first entry is the default token (balance on the launcher, default in apps). The parser does not check for a mint or a symbol listed twice.

## Provisioning

A badge is **unprovisioned** until every required key is set and `VKCOMMIT` succeeds.

| State | Behaviour |
|---|---|
| Unprovisioned | boots normally; apps run; `wallet.provisioned()` is false; every `begin` returns `not_provisioned`; `request_open` returns `not_provisioned`; the launcher's balance row reads `SETUP NEEDED`; any key can be set over USB without confirmation |
| Provisioned | normal operation; non-secure keys can be set over USB; a secure key change needs a hold-SELECT confirmation on the badge's own screen |

Why first provisioning needs no confirmation: it is possible only over the USB cable, on a badge that cannot sign anything yet.

The confirmation for a secure change is an approval ([approval](../wallet/approval.md)): title `Change setting`, headline `SECURITY SETTING`, big = the key name, lines `Old` and `New`, amber, hold. Values are shortened for the screen: a `KEY32` value (the type is looked up in the config registry) to first 4 + `..` + last 4, any other value longer than 35 characters to its first 33 + `..`, and an empty old value is shown as `(none)`. `VKRESET` raises the same kind of confirmation: title `Change setting`, headline `ERASE WALLET CONFIG`, big `RESET` (one word, so the screen does not split it into an amount and a unit), lines `Erases` = `all wallet settings` and `Keeps` = `key, history, contacts`, amber, hold; on approval it erases `vkconf` and calls every `VK_ON_RESET` listener (the consent store registers one and erases itself). It does not touch the device key, the history or the contacts.

### Pinned values

Pinned devnet values (final), given by the backend owner on 2026-10-04. They are public, they are the same for every badge, and they are committed in `os/provision.public.env`, which the tool reads by default.

| Config key | Pinned devnet value (final) | Name in `os/provision.public.env` | Flag that overrides it |
|---|---|---|---|
| `issuer_key` | `2SXh6Xng9b1qQBCEn3pt2Ucwcb4ibBWwBKuuTwNgEzJy` | `ISSUER_PUBKEY` | `--issuer` |
| `tokens`, field `mint` | `3VmWnzfWTfS5UGkwEjbMZnpjd1DPtKyMcwKeJc4QM9wP` | `HACK_MINT` | `--mint` |
| `tokens`, field `decimals` | `2` | `HACK_DECIMALS` | `--decimals` |
| `tokens`, field `symbol` | `HACK` | `HACK_SYMBOL` | `--symbol` |
| `tokens`, field `cap` | `100.00` | `HACK_CAP` | `--cap` |
| `tokens`, field `max` | `1000.00` | `HACK_MAX` | `--max` |
| `rpc_url` | `https://api.devnet.solana.com` | `RPC_URL` | `--rpc` |

The composed `tokens` value is `3VmWnzfWTfS5UGkwEjbMZnpjd1DPtKyMcwKeJc4QM9wP:2:HACK:100.00:1000.00`. The mint is a classic SPL Token mint (program `TokenkegQfeZyiNwAJbNbGKPFXCWuBvf9Ss623VQ5DA`) with 2 decimals, the issuer as mint authority and no freeze authority; this was read back from devnet on 2026-10-04. `HACK_CAP` and `HACK_MAX` are badge-side limits: the dashboard's `.env` has no such names.

Not pinned, because they change with the venue, and so given per run:

| Value | Flag | Without it |
|---|---|---|
| `listener_url` | `--listener http://<backend laptop IP>:8788` | not sent: the key keeps its value (empty on a new badge), and every payee is `UNVERIFIED RECIPIENT` |
| Wi-Fi network | `--wifi "<SSID>" "<password>"` | not sent: the badge keeps the network it has, if any |

**The issuer's secret keypair never leaves the backend laptop.** The badge needs only the public half, and so does the tool: `vkdev.py` has no code that reads a keypair file. An env file's `AUTHORITY_KEYPAIR` line is dropped while the file is parsed, so the tool never learns the path of the secret key. `vkdev.py --selftest` proves it: it runs the value resolution, a dry run and a whole provisioning run (against a badge made of Python) with every way of opening or stat-ing a file replaced by one that fails on the keypair file.

### With the tool

```bash
# from the repository root, badge on USB. The pinned values come from os/provision.public.env.
python3 os/scripts/vkdev.py --port /dev/cu.usbserial-10 provision \
    --listener http://192.168.4.20:8788 \
    --wifi "<hotspot SSID>" "<password>" \
    --badge-id 1 --label "Merchant" --badges-json badges.provisioned.json
```

In that command the port, the listener address, the SSID, the password, the id and the label are placeholders: they differ per laptop, venue and badge. Nothing in it is pinned; the pinned values are read from the file.

**Where a value comes from.** For each of the seven pinned values the first source that has it wins:

1. its flag (`--issuer --mint --decimals --symbol --cap --max --rpc`);
2. `os/provision.public.env` (`--public-env <path>` names another file of the same kind; `--public-env none` reads none);
3. the file named with `--env <path>`, for example `dashboard/.env` on the backend laptop. No such file is read unless it is named: the tool does not look for a dashboard `.env` by itself.

Rules for these files:

- The public file may hold only the seven names of the table above. A file with any other name in it (`AUTHORITY_KEYPAIR`, `DATABASE_URL`, ...) or with a value shaped like key material (a JSON array, hex or base58 of 64 bytes, a URL with a login) is refused whole, so a dashboard `.env` cannot be used as the public file by mistake.
- From the `--env` file only those seven names are taken; every other line is dropped unread.
- If the public file and the `--env` file both have a value and the two differ, the tool stops and names the value (it does not print either one). The badge and the backend must use the same values; decide with the flag, or fix the file that is wrong.
- The issuer is always a public key in base58. If no source has it, the tool stops and asks for `--issuer`.

**Checks before a badge is touched.** Issuer and mint are base58 of exactly 32 bytes and differ from each other; decimals is one digit; the symbol is 1 to 4 characters of `[A-Z0-9]`; `cap` and `max` are amounts with at most `decimals` fraction digits; `cap` is not above `max` (unless `max` is `0`, which means no maximum); the RPC and listener URLs start with `http://` or `https://`, are printable ASCII without blanks and fit the key's length range; an RPC URL taken from a file carries no login and no query string (anyone holding the badge can read `rpc_url`; pass such a URL with `--rpc` if it is intended); the SSID has no `|`; every line fits the badge's 250-byte line. These are the firmware's own rules ([Token table](#token-table)), so a value that passes here is not answered with `ERR invalid`.

**`--dry-run`** does all of the above, prints each value with its source and the lines a run would send, and opens no serial port (no `--port` is needed) and writes no file. A Wi-Fi password is not printed. With the pinned values and the placeholder listener and network of the command above:

```
VKINFO
VKSET issuer_key 2SXh6Xng9b1qQBCEn3pt2Ucwcb4ibBWwBKuuTwNgEzJy
VKSET tokens 3VmWnzfWTfS5UGkwEjbMZnpjd1DPtKyMcwKeJc4QM9wP:2:HACK:100.00:1000.00
VKSET rpc_url https://api.devnet.solana.com
VKSET listener_url http://192.168.4.20:8788
VKWIFI <hotspot SSID>|<password, 10 characters, not shown>
VKCOMMIT
VKGET issuer_key
VKGET tokens
VKGET rpc_url
VKGET listener_url
VKINFO
```

**What a run does, in order:** resolves and checks the values (above); `VKINFO` (refuses if the badge is already provisioned unless `--force`; reads the badge's public key); `VKSET` for `issuer_key`, `tokens`, `rpc_url` and, with `--listener`, `listener_url`; with `--wifi`, `VKWIFI <ssid>|<password>`; `VKCOMMIT`; with `--autostart <id>`, `VKAUTOSTART <id>` (upstream's autostart app); `VKGET` of every key it set, compared with what it sent; `VKINFO` again. With `--force` on a provisioned badge each secure key answers `OK pending`, and the tool waits up to 130 s, polling `VKGET`, while a person holds SELECT on the badge.

**The entry for `badges.json`.** The last thing a run prints is one blank line, one label, and one line of JSON in the shape of an entry of `dashboard/server/config/badges.json`, ready to copy:

```

badges.json entry (copy the next line):
{"id": 1, "label": "Merchant", "pubkey": "<the badge's public key>", "keyLocation": "software", "tokenAccount": null, "standIn": false}
```

`id` comes from `--badge-id` (1 to 99) and `label` from `--label` (default `Badge <id>`); `keyLocation` is `se050` or `software`, as `VKINFO` reports it; `tokenAccount` is `null` (the backend derives it). Without `--badge-id` the entry has `"id": null` and a line above the block says to fill it in. With `--badges-json <file>` the same entry is also written into that file, which has the shape of `badges.json` (`{"badges": [...]}`) and is created if missing: an entry with the same public key is updated, otherwise the entry with the same id is replaced, otherwise the entry is added (with the lowest free id when `--badge-id` is not given). So several badges provisioned on one laptop end up in one file to send to the backend owner. A dry run prints the block with placeholders in place of the public key and key location.

Four badges are four runs of that one command with different `--port`, `--badge-id` and `--label` values.

### By hand

The `issuer_key`, `tokens` and `rpc_url` values are the pinned devnet values (final). The listener address, the SSID and the password are placeholders.

```
VKINFO
VKSET issuer_key 2SXh6Xng9b1qQBCEn3pt2Ucwcb4ibBWwBKuuTwNgEzJy
VKSET tokens 3VmWnzfWTfS5UGkwEjbMZnpjd1DPtKyMcwKeJc4QM9wP:2:HACK:100.00:1000.00
VKSET rpc_url https://api.devnet.solana.com
VKSET listener_url http://192.168.4.20:8788
VKWIFI My Hotspot|hunter2hunter2
VKCOMMIT
```

## Serial commands

Available on the USB serial console only (hook H6; 115200 baud, one command per line, case-insensitive). The registry, `VKHELP` and `VKINFO` are in `core/serial.cpp`; the config commands are registered from `core/config.cpp`. Every reply line starts with `OK` or `ERR`. A line whose first word is not a registered command falls through to upstream's push protocol (`PING`, `AUTH`, ...).

```cpp
// src/vk/core/serial.h
namespace vk::serial {
using Reply = std::function<void(const String &)>;
struct SerialCommand : Registered<SerialCommand> {
  const char *name;                                    // upper case, starts with "VK"
  void (*fn)(const String &args, const Reply &reply);
  const char *help;
  SerialCommand(const char *n, void (*f)(const String &, const Reply &), const char *h) : name(n), fn(f), help(h) {}
};
#define VK_SERIAL_COMMAND(ident, name, fn, help) static vk::serial::SerialCommand vk_serial_##ident(name, fn, help)
bool handleLine(const String &line, const Reply &reply);     // true if it was one of ours

// One name=value pair in the VKINFO reply.
struct InfoField : Registered<InfoField> { const char *name; String (*fn)(); InfoField(const char *n, String (*f)()) : name(n), fn(f) {} };
#define VK_INFO_FIELD(ident, name, fn) static vk::serial::InfoField vk_info_##ident(name, fn)
}
```

| Command | Reply | Notes |
|---|---|---|
| `VKHELP` | one `+ <name> <help>` line per command, then `OK <count>` | |
| `VKINFO` | `OK` followed by one `name=value` pair per registered info field, space-separated. With everything built: `profile=<dev\|release> api=2 provisioned=<0\|1> pubkey=<base58> key=<se050\|software\|none> selfcheck=<0\|1> time=<none\|floor\|sntp> wifi=<0\|1> display=<awake\|dim\|sleep> battery=<ok\|low\|critical>` | each module adds its own fields with `VK_INFO_FIELD(ident, "name", fn)` where `fn` returns a `String`; order is not guaranteed, so parse by name. `pubkey=` is empty when the badge has no identity. `wifi=1` means station mode and connected |
| `VKKEYS` | one `+ <name> <type> <flags> <help>` line per config key, then `OK <count>` | `<type>` is `STR`, `U32`, `KEY32` or `TOKENS`; `<flags>` is one word: `-`, `secure`, `required` or `secure,required` |
| `VKGET <key>` | `OK <value>` or `ERR unknown_key` | all values are public |
| `VKSET <key> <value>` | `OK` · `OK pending` (confirmation shown on the badge) · `ERR unknown_key` · `ERR invalid` · `ERR unavailable` (a secure key on a firmware without the approval engine, or while another approval is on screen) · `ERR nvs_full` (the write failed) | the value is the rest of the line: blanks after the key are skipped, nothing is trimmed from the end, and `VKSET <key>` alone sets the empty string. Keep the whole line under 250 characters |
| `VKCOMMIT` | `OK provisioned` · `ERR missing <key>` · `ERR nvs_full` (writing the flag failed) | with several required keys missing, `<key>` is the first in alphabetical order, so the reply does not depend on registration order |
| `VKRESET` | `OK pending` · `ERR unavailable` (no approval engine, or another approval is on screen) | confirmation on the badge, raised whether or not the badge is provisioned |
| `VKAUTOSTART <id>` | `OK` | sets upstream's autostart app (`settings::setAutostartApp`); empty id clears it |
| `VKWIFI <ssid>\|<password>` | `OK joining` · `ERR usage` (no `\|`, or an empty SSID) | `OK joining` is sent before the join starts. The SSID is everything before the first `\|` (it may contain spaces), the password everything after. Calls `wifi_mgr::connect(ssid, password, true)`, which saves the network and joins it, as upstream's `JOINWIFI` does |

Dev-profile commands (`VKSHOT`, `VKBTN`, `VKSTATE`, `VKTIME`, `VKPERF` and the others) are in [../testing/testing.md](../testing/testing.md#dev-hooks).

## Wi-Fi

Upstream owns Wi-Fi: one saved network in its `sysconf` settings, joined at boot. `VKWIFI` is a USB shortcut to it. The on-badge Settings → Wi-Fi screen and the upstream push API keep working unchanged.

## Key rules

A key whose valid texts are narrower than its type and range says so with a rule, registered next to the key (`src/vk/core/config.h`, added for the on-badge settings):

```cpp
struct KeyRule : Registered<KeyRule> {
  const char *name;                    // the key it belongs to
  bool (*valid)(const char *text);     // called only for a text that already passed type and range
};
#define VK_CONFIG_RULE(ident, name, fn) static vk::config::KeyRule vk_config_rule_##ident{name, fn}
const KeyRule *ruleFor(const char *name);
```

`validate()` applies it after the type and range, so `set()`, `VKSET` (`ERR invalid`), the settings pages and the read of a stored value (a stored text the rule refuses reads as the default) all agree. One key has a rule: `utc_offset`, with `vk_utc_offset_valid` (`src/vk/core/utc_offset.c`, pure C, host suite `test_setup`). Host suite `test_config_rule` checks the mechanism. The range and length a page needs come from the key itself (`find(name)->min`, `->max`, `->def`, `->help`): no other accessor was added.

## Adding a config key

One line in the file that uses the value:

```cpp
VK_CONFIG_KEY(duel_rounds, "duel_rounds", vk::config::Type::U32, "3", vk::config::F_NONE, 1, 9, "rounds in a duel");
// ...
const uint32_t rounds = vk::config::u32("duel_rounds");
```

It is then listed by `VKKEYS`, settable with `VKSET`, validated, shown in the Wallet settings app, and (unless it is secure or required) shown and editable on Settings → Advanced. If its values are a list, register them with `VK_KEY_CHOICES` (`src/vk/shell/edit.h`) and Advanced offers a picker. No table elsewhere is edited (add the row to this document). Mark it `F_SECURE` only if changing it could weaken an approval. Recipe: [../guides/extending.md](../guides/extending.md#add-a-config-key).

## Tests

Host: `test_config` covers the text parsers (`TOKENS` form, base58 keys, ranges) with the NVS layer replaced by an in-memory map. Device: T-CFG1 to T-CFG4 in [../testing/testing.md](../testing/testing.md#acceptance-tests).
