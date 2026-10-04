# Configuration, provisioning and serial commands

Nothing that identifies a deployment is compiled into the firmware: no issuer key, no mint, no URL, no Wi-Fi name, no limit. Every such value is a **config key** stored in NVS and set by provisioning. Files: `src/vk/core/config.{h,cpp}`, `src/vk/core/serial.{h,cpp}`, `scripts/vkdev.py`.

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

Type text forms:

| Type | Text form | Example |
|---|---|---|
| `STR` | UTF-8, printable ASCII only | `https://api.devnet.solana.com` |
| `U32` | decimal | `45` |
| `KEY32` | base58 of 32 bytes | `FZEA...ei3K` |
| `TOKENS` | see [Token table](#token-table) | `9xQe...:2:HACK:100.00:1000.00` |

## Keys

Each key is registered by the code that uses it. This table is the complete list; a new key is a new row here and one `VK_CONFIG_KEY` line.

| Key | Type | Default | Flags | Range | Owner | Meaning |
|---|---|---|---|---|---|---|
| `issuer_key` | KEY32 | — | secure, required | | `solana_pay` | public key that signs registry records |
| `tokens` | TOKENS | — | secure, required | 1–3 entries | `solana_pay` | payment tokens with caps |
| `rpc_url` | STR | — | required | 8–128 | `core` | Solana JSON-RPC endpoint (used by the balance feature and by `vk.rpc` in apps) |
| `listener_url` | STR | (empty) | | 0–128 | `core` | base URL of the backend's badge listener, e.g. `http://192.168.4.20:8788` |
| `display_name` | STR | (empty) | | 0–32 | `core` | name this badge claims in requests and contact cards; empty means upstream's device name |
| `ntp_server` | STR | `pool.ntp.org` | | 3–64 | `core` (clock) | SNTP host |
| `approval_tmo_s` | U32 | 45 | secure | 10–120 | approval | approval timeout; keep below the ~60 s blockhash lifetime |
| `hold_ms` | U32 | 3000 | secure | 1000–10000 | approval | hold-SELECT duration |
| `record_ttl_s` | U32 | 30 | secure | 5–3600 | `solana_pay` | maximum age of a registry record under SNTP |
| `presence_ms` | U32 | 1500 | secure | 50–5000 | `requests` | CHAL→PROOF deadline |
| `req_ttl_s` | U32 | 60 | | 10–600 | `requests` | lifetime of a payment request |
| `req_period_ms` | U32 | 1000 | | 250–5000 | `requests` | REQ rebroadcast period |
| `req_max_proofs` | U32 | 8 | | 1–64 | `requests` | proofs answered per request |
| `req_gap_ms` | U32 | 200 | | 0–5000 | `requests` | minimum time between proofs |
| `balance_poll_s` | U32 | 15 | | 0–3600 | `balance` | balance poll period; 0 disables |
| `pay_app` | STR | `pay` | | 1–24 | `requests` | app opened from a payment-request notification |
| `theme` | STR | (empty) | | 0–24 | `ui` | active theme: `receipt-light` (also when empty) or `receipt-dark` ([ui](../ui/ui.md#theme)) |
| `home_app` | STR | `launcher` | | 0–32 | `host` (home service) | app kept in front when nothing else runs; empty leaves upstream's launcher in charge |

Secure keys are the ones whose change could turn a blocked payment into an approved one. Wi-Fi credentials are upstream settings, not config keys ([Wi-Fi](#wi-fi)).

### Token table

Text form: entries separated by `,`; each entry is `mint:decimals:symbol:cap:max`.

| Field | Form |
|---|---|
| `mint` | base58 mint address |
| `decimals` | 0–9 |
| `symbol` | 1–4 characters of `[A-Z0-9]` (it must fit a REQ frame's 4-byte currency field) |
| `cap` | display units (`100.00`); above this SELECT becomes a hold; `0` = no cap |
| `max` | display units; above this the payment is blocked; `0` = no max |

Example for one token: `9xQeWvG816bUx9EPjHmaT23yvVM2ZWbrrpZb9PusVFin:2:HACK:100.00:1000.00`. The first entry is the default token (balance in the status bar, default in apps).

## Provisioning

A badge is **unprovisioned** until every required key is set and `VKCOMMIT` succeeds.

| State | Behaviour |
|---|---|
| Unprovisioned | boots normally; apps run; `wallet.provisioned()` is false; every `begin` returns `not_provisioned`; `request_open` returns `not_provisioned`; the status bar shows `SETUP`; any key can be set over USB without confirmation |
| Provisioned | normal operation; non-secure keys can be set over USB; a secure key change needs a hold-SELECT confirmation on the badge's own screen |

Why first provisioning needs no confirmation: it is possible only over the USB cable, on a badge that cannot sign anything yet.

The confirmation for a secure change is an approval ([approval](../wallet/approval.md)): title `Change setting`, headline `SECURITY SETTING`, big = the key name, lines `Old` and `New` (values shortened to 35 characters, keys to first 4 + `..` + last 4), amber, hold. `VKRESET` raises the same kind of confirmation with headline `ERASE WALLET CONFIG`; on approval it erases `vkconf` and calls every `VK_ON_RESET` listener (the consent store registers one and erases itself). It does not touch the device key, the history or the contacts.

### With the tool

```bash
# from the repository root, badge on USB, dashboard already set up (`npm run devnet:setup` in dashboard/)
python3 os/scripts/vkdev.py --port /dev/cu.usbserial-10 provision \
    --env dashboard/.env \
    --listener http://192.168.4.20:8788 \
    --wifi "<hotspot SSID>" "<password>" \
    --cap 100.00 --max 1000.00 --autostart home
```

What it does, in order: `VKINFO` (refuses if already provisioned unless `--force`); reads `RPC_URL`, `HACK_MINT`, `HACK_SYMBOL`, `HACK_DECIMALS` and `AUTHORITY_KEYPAIR` from the env file; takes the issuer public key as the last 32 bytes of the 64-byte keypair file [UNVERIFIED format; override with `--issuer <base58>`]; sends `VKSET` for `issuer_key`, `tokens`, `rpc_url`, `listener_url`; sends `VKWIFI <ssid>|<password>`; sends `VKCOMMIT`; with `--autostart <id>`, sets upstream's autostart app with `VKAUTOSTART <id>`; prints the badge's public key and key location for `dashboard/server/config/badges.json`.

Four badges are four runs of that one command with different `--port` values.

### By hand

```
VKINFO
VKSET issuer_key FZEA...ei3K
VKSET tokens 9xQe...VFin:2:HACK:100.00:1000.00
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
}
```

| Command | Reply | Notes |
|---|---|---|
| `VKHELP` | one `+ <name> <help>` line per command, then `OK <count>` | |
| `VKINFO` | `OK` followed by one `name=value` pair per registered info field, space-separated. With everything built: `profile=<dev\|release> api=2 provisioned=<0\|1> pubkey=<base58> key=<se050\|software\|none> selfcheck=<0\|1> time=<none\|floor\|sntp> wifi=<0\|1>` | each module adds its own fields with `VK_INFO_FIELD(ident, "name", fn)` where `fn` returns a `String`; order is not guaranteed, so parse by name |
| `VKKEYS` | one `+ <name> <type> <flags> <help>` line per config key, then `OK <count>` | |
| `VKGET <key>` | `OK <value>` or `ERR unknown_key` | all values are public |
| `VKSET <key> <value>` | `OK` · `OK pending` (confirmation shown on the badge) · `ERR unknown_key` · `ERR invalid` · `ERR unavailable` (a secure key on a firmware without the approval engine) · `ERR nvs_full` (the write failed) | the value is the rest of the line; keep the whole line under 250 characters |
| `VKCOMMIT` | `OK provisioned` or `ERR missing <key>` | |
| `VKRESET` | `OK pending` | confirmation on the badge |
| `VKAUTOSTART <id>` | `OK` | sets upstream's autostart app (`settings::setAutostartApp`); empty id clears it |
| `VKWIFI <ssid>\|<password>` | `OK joining` | the SSID is everything before the first `\|` (it may contain spaces), the password everything after. Calls `wifi_mgr::connect(ssid, password, true)`, which saves the network and joins it, as upstream's `JOINWIFI` does |

Dev-profile commands (`VKSHOT`, `VKBTN`, `VKSTATE`, `VKTIME`) are in [../testing/testing.md](../testing/testing.md#dev-hooks).

## Wi-Fi

Upstream owns Wi-Fi: one saved network in its `sysconf` settings, joined at boot. `VKWIFI` is a USB shortcut to it. The on-badge Settings → Wi-Fi screen and the upstream push API keep working unchanged.

## Adding a config key

One line in the file that uses the value:

```cpp
VK_CONFIG_KEY(duel_rounds, "duel_rounds", vk::config::Type::U32, "3", vk::config::F_NONE, 1, 9, "rounds in a duel");
// ...
const uint32_t rounds = vk::config::u32("duel_rounds");
```

It is then listed by `VKKEYS`, settable with `VKSET`, validated, and shown in the Wallet settings app. No table elsewhere is edited (add the row to this document). Mark it `F_SECURE` only if changing it could weaken an approval. Recipe: [../guides/extending.md](../guides/extending.md#add-a-config-key).

## Tests

Host: `test_config` covers the text parsers (`TOKENS` form, base58 keys, ranges) with the NVS layer replaced by an in-memory map. Device: T-CFG1 to T-CFG4 in [../testing/testing.md](../testing/testing.md#acceptance-tests).
