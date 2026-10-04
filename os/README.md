# Solana OS

Application firmware for the Solana Badge. A Lua runtime with an SDK over every
peripheral on the board, a launcher and settings UI, and app delivery over Wi-Fi
or BLE.

This is not the hardware test image — that lives in [`../testkit/`](../testkit/)
and is what you flash to find out whether a board came back from assembly
working. Solana OS assumes it did.

```
┌─ Lua apps ────────────────────────────────────────────┐
│  main.lua + app.ini, in /apps/<id>/ on LittleFS       │
├─ badge.* SDK ─────────────────────────────────────────┤
│  gfx  input  led  system  storage                     │
│  battery  mic  se050  wifi  http  espnow  ble         │
├─ Runtime ─────────────────────────────────────────────┤
│  Lua 5.4.8, one sandboxed state per app,              │
│  PSRAM heap cap + wall-clock watchdog                 │
├─ Shell ───────────────────────────────────────────────┤
│  launcher, settings, ESP-NOW radar, console           │
├─ HAL ─────────────────────────────────────────────────┤
│  ILI9341  TCA9534  WS2812B  SPH0641  SE050  ADC       │
└───────────────────────────────────────────────────────┘
```

---

## Contents

- [Quick start](#quick-start)
- [Building](#building)
- [First boot](#first-boot)
- [Using the badge](#using-the-badge)
- [Wi-Fi, including DEF CON](#wi-fi-including-def-con)
- [Writing an app](#writing-an-app)
- [Lua API reference](#lua-api-reference)
- [Pushing apps](#pushing-apps)
- [Phone bridge (HTTP over BLE)](#phone-bridge-http-over-ble)
- [Architecture](#architecture)
- [The sandbox](#the-sandbox)
- [Radio notes](#radio-notes)
- [Troubleshooting](#troubleshooting)
- [Extending the SDK](#extending-the-sdk)

---

## Quick start

```sh
# 1. Build and flash
arduino-cli compile --fqbn "esp32:esp32:esp32s3:PSRAM=opi,FlashSize=16M,PartitionScheme=custom,CDCOnBoot=cdc" .
arduino-cli upload  --fqbn "esp32:esp32:esp32s3:PSRAM=opi,FlashSize=16M,PartitionScheme=custom,CDCOnBoot=cdc" -p /dev/cu.usbserial-XXXX .

# 2. On the badge: Settings -> Wi-Fi -> Start hotspot
#    Join "badge-XXXX" from a laptop (password: solanabadge)
#    Read the pairing code from Settings -> Push

# 3. Push an app
tools/badge-push.py --host 192.168.4.1 --token 123456 push apps/hello --run
```

---

## Building

### Requirements

| | |
| --- | --- |
| Core | `esp32:esp32` 3.x (Arduino-ESP32) |
| Libraries | `LovyanGFX` |
| Board | ESP32S3 Dev Module |
| Flash | 16 MB |
| PSRAM | **OPI PSRAM — required** |
| Partition scheme | Custom (`partitions.csv`) |
| USB CDC on boot | Enabled (for the USB serial console) |

```sh
arduino-cli core install esp32:esp32
arduino-cli lib install LovyanGFX
```

PSRAM is not optional. The compositor's framebuffer is 320×240×16bpp = 150 KB
and the Lua heap is drawn from PSRAM too; without it, `display::begin()` fails
and the badge boots headless with a logged error.

### The build

```sh
cd firmware/solana-os
arduino-cli compile --fqbn "esp32:esp32:esp32s3:PSRAM=opi,FlashSize=16M,PartitionScheme=custom,CDCOnBoot=cdc" .
```

Roughly 1.75 MB of flash and 72 KB of static RAM. `partitions.csv` gives two
3.2 MB OTA slots and a 9.5 MB LittleFS volume for apps.

### Vendored Lua

`src/lua/` is Lua 5.4.8 from lua.org, with several changes — all of them
deliberate, all of them documented at the point of change:

| Change | Why |
| --- | --- |
| `LUA_32BITS 1` | `lua_Number` becomes `float` and `lua_Integer` `int32_t`, which is what the S3's FPU actually has |
| `LUAI_MAXSTACK` 1000000 → 25000 | a desktop-sized stack limit would let one app exhaust the heap |
| `lua_writestring` et al. → `badge_lua_write` | `print()` reaches UART0, USB CDC and the on-screen console |
| `linit.c`: no `io`, `os`, `debug` | see [The sandbox](#the-sandbox) |
| `lbaselib.c`: `load`/`loadfile`/`dofile` are text-only (mode `"t"`) | precompiled bytecode is never accepted — a crafted binary chunk is an arbitrary-memory-access sandbox escape, so apps are source-only |
| `lstrlib.c`: `string.dump` disabled | dumping a function to bytecode would let an app round-trip its way to a binary chunk the loaders now refuse |

`liolib.c` and `loslib.c` are deleted rather than merely unregistered. Upgrading
Lua means re-applying these; each one is marked with a comment explaining
itself.

---

## First boot

1. Two splash screens — the Solana logo, then the SKYRIZZ credit — while the
   RGB LEDs run the brand gradient: a purple-to-green wipe, a travelling wave
   with the two LEDs half a period apart, three green heartbeats, fade out.
2. A progress card for storage, peripherals, runtime and radios.
3. The launcher.

The LED sequence is interleaved with the splash rather than sequential — every
blocking wait in the boot path calls `leds::update()` from its step hook, so the
lights animate *through* the logo instead of waiting their turn.

On a fresh badge LittleFS is formatted automatically and the launcher will be
empty. That is expected; push an app.

---

## Using the badge

### Buttons

The silkscreen names the keys UP, DOWN, LEFT, RIGHT, SELECT and CANCEL. Which
expander bit each one sits on is not printed anywhere, but it has been measured
on a v1 board rather than assumed, and it matches the schematic (PB1..PB6 on
U10). The mapping lives in `src/config.h`:

| Silkscreen | Expander bit | Schematic ref | Lua name |
| --- | --- | --- | --- |
| UP | P0 | PB1 | `"up"` |
| LEFT | P1 | PB2 | `"left"` |
| RIGHT | P2 | PB3 | `"right"` |
| DOWN | P3 | PB4 | `"down"` |
| SELECT | P4 | PB5 | `"a"` |
| CANCEL | P5 | PB6 | `"b"` |

Note the order: **DOWN sits at P3, after LEFT and RIGHT** — not the P0/P1/P2/P3
= up/down/left/right that looks natural. Getting this wrong is quiet rather than
loud, because the keys still work, they just arrive as the wrong action: a badge
whose DOWN reports as RIGHT looks like it has two dead keys, since left and
right are unbound on the launcher.

To re-measure on a new board revision, watch the serial log and press each key.
`src/hal/buttons.cpp` prints one line per debounced press:

```
[btn] P4 SELECT down (raw=0x2F)
```

**Holding CANCEL for 1.5 s always force-quits a running app.** That is handled
in the main loop, not by the app, so an app that traps every button still cannot
strand you.

Switch on the Lua name, but **print the silkscreen label**. "Press A" names a
button nobody can find; the board says `SELECT`. Every bundled app writes
`SELECT` and `CANCEL`, and a new app should too.

### Screens

| Screen | What it does |
| --- | --- |
| **Launcher** | Installed apps, then Settings. SELECT runs, RIGHT deletes, CANCEL opens Settings. |
| **Wi-Fi** | Status, scan, join open networks, hotspot, forget. Secured networks are marked `*`. |
| **Bluetooth** | BLE on/off, connection state, enable-at-boot. |
| **ESP-NOW** | The radar: every badge heard, with signal bars and last-seen. SELECT toggles, left/right changes channel. |
| **App push** | The web UI address and the pairing code. |
| **Identity** | The badge ID, where its key lives, and the full public key. |
| **App store** | The broker connection, and whether the badge is registered with it. |
| **Display / LEDs** | Backlight and RGB brightness. SELECT previews the boot animation. |
| **Device info** | Chip, heap, PSRAM, storage, battery, TCA9534, SE050, MAC. |
| **Console** | The log ring buffer, errors in red. |

### Deleting an app

On the launcher, put the cursor on an app and press **RIGHT**. A confirmation
screen names the app, its id and its size; SELECT deletes it, CANCEL keeps it.
Deleting removes `/apps/<id>/` entirely — the scripts, any assets pushed with
them, and anything the app wrote through `badge.storage`. There is no undo, and
the only way back is to push the app again.

RIGHT is unbound elsewhere on the launcher, and the shortcut only appears in the
footer while the cursor is on an app rather than on the Settings row.

Apps can also be removed over the wire — `badge-push.py rm <id>`, or `DELETE` on
the HTTP API — which is the better route when you are deleting several.

### At rest

With no app running, the two RGB LEDs breathe: a six-second fade in and out,
with the colour walking around the hue wheel over ninety seconds, so successive
breaths are close but never quite the same. It never fades fully to black — a
badge that goes dark reads as a badge that crashed.

A notification pulse (an app-store offer, a delete confirmation) plays over the
top and then hands back to the breathing where it left off, rather than leaving
the LEDs dark. An app that wants the LEDs calls `led.take()` and gets them until
it exits.

There is no keyboard, so a WPA passphrase cannot be typed on the device. Open
networks join from the Wi-Fi screen; for a secured one, start the hotspot and
use the Wi-Fi card in the web UI. Once a network is saved — personal or
enterprise — **Connect saved network** rejoins it from the badge alone.

---

## Wi-Fi, including DEF CON

Both WPA2-Personal and WPA2-Enterprise (802.1X) are supported. Enterprise covers
DEF CON's `DefCon` network, eduroam, and most corporate networks.

### DEF CON in four commands

DEF CON publishes its settings at
[wifireg.defcon.org](https://wifireg.defcon.org/linux.html): SSID `DefCon`, EAP
**PEAP**, phase 2 **MSCHAPv2**, a CA certificate reissued every year, and
credentials you register for.

```sh
# 1. On the badge: Settings -> Wi-Fi -> Start hotspot, and join it from a laptop.
#    Read the pairing code from Settings -> Push.

# 2. Convert the certificate if it came as DER (the .crt download usually is)
openssl x509 -inform der -in defcon34-wifi.crt -out defcon34-wifi.pem

# 3. Upload it
tools/badge-push.py --host 192.168.4.1 --token 123456 cert defcon34-wifi.pem

# 4. Join
tools/badge-push.py --host 192.168.4.1 --token 123456 \
    defcon --user MYUSER --password MYPASS --ca defcon34-wifi.pem
```

`defcon` fills in SSID, PEAP, MSCHAPv2 and the `wifireg.defcon.org` domain
match. If exactly one certificate with "defcon" in its name is installed,
`--ca` can be left off.

The **DEF CON preset** button in the web UI's Wi-Fi card does the same thing
with a file picker instead of a shell.

Any other enterprise network:

```sh
tools/badge-push.py --host 192.168.4.1 --token 123456 \
    join eduroam --enterprise --user me@uni.edu --password hunter2 \
    --ca uni.pem --domain radius.uni.edu
```

### Why the certificate is not bundled

It is reissued every year — DEF CON's is named for the year it belongs to — so a
compiled-in copy would be stale before anyone used it, and the badge would then
either fail to associate or get reflashed with validation switched off. It goes
in `/certs` on LittleFS instead, and survives reflashing the firmware.

### What the badge actually verifies

Two things, and the second is the one people usually skip:

- **the chain**, against the CA you uploaded
- **the server's identity**, against the `domain` you set — the equivalent of
  wpa_supplicant's `altsubject_match=DNS:wifireg.defcon.org`

Without the domain check, any certificate issued by that CA would be accepted,
which is a meaningful gap at a conference full of people who find this
interesting. The Arduino Wi-Fi wrapper does not expose it, so `wifi_mgr` calls
`esp_eap_client_set_domain_name()` directly.

Naming a certificate that is not installed is a **hard failure** — the badge
refuses to associate rather than silently falling back to an unvalidated
connection. A join with **no** CA at all now also **fails closed** by default:
`wifi_mgr` refuses it unless the caller explicitly opts in (`allowNoCa`), which
is what the web UI's confirmation and a deliberately saved no-CA profile do. An
opted-in unvalidated join still logs `(UNVALIDATED)`. PEAP/MSCHAPv2 sends a
crackable hash of your password to whatever answers, so an unvalidated
connection is not a small thing.

### The clock problem

Certificate validity windows need a real clock. The badge has no RTC, so at boot
it thinks it is 1970 and every certificate's `notBefore` is in the future.

Setting the clock first would need the network, which needs the clock. Solana OS
seeds the clock from the firmware's build timestamp and disables **only** the
validity-window check (`esp_eap_client_set_disable_time_check`), keeping chain
and domain verification. The practical consequence: an expired or
not-yet-valid CA certificate will be accepted. Set `EAP_DISABLE_TIME_CHECK` to
`false` in `config.h` if you would rather set the clock yourself and have the
dates enforced.

### Notes

- PEAP label is the standard PEAPv0 `peaplabel=0`, matching DEF CON's config.
  ESP-IDF does not expose a setter and uses that value.
- Identity, username and password are each capped at 64 characters by the driver.
- The outer identity defaults to the username, which is what DEF CON's published
  config does. Set `identity` explicitly for a network that wants a real
  anonymous outer identity (`anonymous@realm`).
- EAP-TLS (client-certificate) is not wired up. The plumbing takes a client cert
  and key, but there is no UI or storage for them yet.
- Credentials live in NVS, not on the filesystem, so pushing or deleting apps
  cannot touch them. They are stored **unencrypted** — anyone with the badge and
  a serial cable can read them out. Use a conference-only password.

---

## Writing an app

An app is a directory on LittleFS:

```
/apps/hello/
  app.ini        metadata (optional)
  main.lua       entry point
  ...            anything else
```

The id is the directory name, restricted to `[a-z0-9._-]`, 1–32 characters. It
is the handle the launcher, the push API and `badge.system.launch()` all use.

### `app.ini`

`key=value`, one per line, `#` and `;` comment. Every field is optional; an app
that is nothing but a `main.lua` shows up named after its directory.

```ini
name=Hello Solana
version=1.0.0
author=you
description=What it does, one line.
entry=main.lua
```

### Lifecycle

Entry points are globals in your script. All are optional.

```lua
function on_start()                      end  -- once, after the script loads
function on_update(dt)                   end  -- every frame; dt in seconds
function on_draw()                       end  -- every frame, after on_update
function on_button(key, pressed)         end  -- "up".."b", true on press
function on_espnow(mac, data, rssi)      end  -- an ESP-NOW app message
function on_ble(line)                    end  -- a BLE line, after ble.listen()
function on_stop()                       end  -- before teardown
```

The script's top level runs once before `on_start()`, so a script that only
wants to print something needs no callbacks at all.

Frame order:

```
espnow/ble events → on_button (per edge) → on_update(dt) → on_draw() → flush
```

You never call a "present" or "swap" function. Everything draws into a shared
PSRAM framebuffer and the runtime pushes it once `on_draw()` returns, which is
why apps are flicker-free without doing anything about it.

### The smallest app

```lua
function on_draw()
  local g = badge.gfx
  g.clear()
  g.text_center("gm", g.width() // 2, 100, g.SOLANA_GREEN, 3)
end

function on_button(key, pressed)
  if key == "b" and pressed then badge.system.exit() end
end
```

### Multiple files

`require` works. `package.path` is set per app to:

```
/littlefs/apps/<id>/?.lua ; /littlefs/apps/<id>/?/init.lua ; /littlefs/lib/?.lua
```

So `require("util")` finds `util.lua` next to your `main.lua`, and `/lib/` is
there for modules shared between apps. There is no `package.cpath` — no dynamic
C loading exists on the badge.

### Bundled examples

| App | Shows |
| --- | --- |
| [`apps/hello`](apps/hello/) | The basic shape: gradient, LEDs, buttons |
| [`apps/dice`](apps/dice/) | Hardware entropy: a dice roller and a shuffled 52-card deck |
| [`apps/gallery`](apps/gallery/) | `gfx.image` — pages through the PNGs and JPEGs pushed into the app, with zoom |
| [`apps/radar`](apps/radar/) | ESP-NOW discovery, signal plotting, send/receive |
| [`apps/whosnear`](apps/whosnear/) | ESP-NOW app messaging: who else is running this app right now |
| [`apps/vumeter`](apps/vumeter/) | PDM microphones, `storage.kv` persistence |

---

## Lua API reference

Everything hangs off the `badge` global.

### Top level

| | |
| --- | --- |
| `badge.version` | `"0.1.0"` |
| `badge.api_version` | integer; bumps when a binding changes shape |
| `badge.device_name` | e.g. `"badge-4F2A"` |
| `badge.log(...)` | tagged log line; honours `__tostring` |
| `badge.millis()` | alias of `system.millis` |
| `badge.sleep(ms)` | alias of `system.sleep` |

`print()` also works and goes to the same places.

---

### `badge.gfx`

Colours are RGB565 integers. The framebuffer is 320×240 in landscape.

**Geometry**

| | |
| --- | --- |
| `gfx.width()` / `gfx.height()` | 320 / 240 |
| `gfx.clear([color])` | fills the frame; defaults to the theme background |
| `gfx.pixel(x, y [, c])` | |
| `gfx.line(x0, y0, x1, y1 [, c])` | |
| `gfx.rect(x, y, w, h [, c])` | outline |
| `gfx.fill_rect(x, y, w, h [, c])` | |
| `gfx.round_rect(x, y, w, h [, r] [, c])` | `r` defaults to 4 |
| `gfx.fill_round_rect(x, y, w, h [, r] [, c])` | |
| `gfx.circle(x, y, r [, c])` | |
| `gfx.fill_circle(x, y, r [, c])` | |
| `gfx.triangle(x0,y0, x1,y1, x2,y2 [, c])` | |
| `gfx.fill_triangle(x0,y0, x1,y1, x2,y2 [, c])` | |

**Text**

| | |
| --- | --- |
| `gfx.text(s, x, y [, c] [, size])` | top-left anchored |
| `gfx.text_center(s, cx, y [, c] [, size])` | |
| `gfx.text_right(s, rx, y [, c] [, size])` | |
| `gfx.text_width(s [, size])` → px | |
| `gfx.text_height([size])` → px | |

`size` is an integer scale factor, 1 by default. The built-in font is 6×8 at
size 1.

**Colour**

| | |
| --- | --- |
| `gfx.color(r, g, b)` → rgb565 | 8-bit components |
| `gfx.hsv(h, s, v)` → rgb565 | `h` degrees, `s`/`v` 0..1 |
| `gfx.gradient(t)` → rgb565 | the Solana ramp, 0.0 purple → 1.0 green |

Constants: `SOLANA_PURPLE` `SOLANA_GREEN` `SOLANA_TEAL` `SOLANA_MAGENTA`
`BLACK` `WHITE` `BG` `PANEL` `BORDER` `MUTED` `RED` `ORANGE` `YELLOW` `GREEN`
`CYAN` `BLUE`.

**Other**

| | |
| --- | --- |
| `gfx.image(path, x, y [, scale])` → ok | PNG or JPEG from your app directory |
| `gfx.brightness([0..255])` → current | backlight |
| `gfx.flush()` | force a push; only needed outside `on_draw` |

---

### `badge.input`

Keys are `"up"`, `"down"`, `"left"`, `"right"`, `"a"`, `"b"`. Numeric indices
also work; the constants `input.UP` … `input.B` and `input.COUNT` are provided.

| | |
| --- | --- |
| `input.down(key)` | held right now |
| `input.pressed(key)` | went down this frame |
| `input.released(key)` | came up this frame |
| `input.repeated(key)` | pressed, or an auto-repeat tick while held |
| `input.held_ms(key)` | ms held, 0 when up |
| `input.any()` | anything held |
| `input.keys()` | the six names, in index order |
| `input.label(key)` | `"SELECT"` — the silkscreen word, for on-screen hints |
| `input.present()` | false if the TCA9534 did not answer |

Events and polling both work — `on_button` for menus, `input.down()` for games.

---

### `badge.led`

Two WS2812Bs. Writes are buffered; `led.show()` latches them, so you can stage
both and update in one go.

| | |
| --- | --- |
| `led.set(index, r, g, b)` | index 0 or 1 |
| `led.all(r, g, b)` | |
| `led.gradient(index, t [, intensity])` | a point on the Solana ramp |
| `led.show()` | latch |
| `led.off()` | |
| `led.pulse(r, g, b [, ms])` | self-decaying flash, driven by the OS |
| `led.brightness([0..255])` → current | global scale |
| `led.take()` | stop the OS animation and claim the LEDs |

`led.COUNT` is 2. Call `led.take()` in `on_start` or the launcher's idle
animation will fight you.

---

### `badge.system`

| | |
| --- | --- |
| `system.millis()` | ms since boot |
| `system.uptime()` | seconds, fractional |
| `system.sleep(ms)` | blocking, capped at 2000 ms; extends the watchdog |
| `system.heap()` / `system.psram()` | free bytes |
| `system.lua_memory()` → used, limit | your app's Lua heap |
| `system.chip()` | `{model, revision, mhz, flash_bytes, psram_bytes}` |
| `system.name([new])` → name | device name; a new name must be 1–23 printable-ASCII characters (it flows into BLE, mDNS, SoftAP and ESP-NOW), and an invalid one raises rather than being stored |
| `system.apps()` | array of `{id, name, version, author, description, bytes}` |
| `system.current_app()` | id |
| `system.launch(id)` | hand over to another app (deferred) |
| `system.exit()` | back to the launcher (deferred) |
| `system.reboot()` | |

`launch` and `exit` are deferred to the end of the frame, so they are safe to
call from anywhere including deep inside `on_draw`.

**`os`** is also available, trimmed to `os.time`, `os.clock`, `os.date`,
`os.difftime`. There is no RTC, so `os.time()` is seconds since boot unless
something has set the system clock.

---

### `badge.storage`

Paths are relative to your app's own directory and cannot leave it — no leading
slash, no `..`. That is the whole file sandbox.

| | |
| --- | --- |
| `storage.read(path)` → string, or nil + reason | one call returns at most 64 KB; a larger file is refused, use `storage.size` and read in chunks |
| `storage.write(path, data)` → ok | creates parent directories |
| `storage.append(path, data)` → ok | |
| `storage.exists(path)` | |
| `storage.size(path)` → bytes or nil | |
| `storage.remove(path)` | |
| `storage.mkdir(path)` | |
| `storage.list([subdir])` | names; directories get a trailing `/` |
| `storage.space()` → used, total | whole filesystem |

**`storage.kv`** is NVS-backed and survives a filesystem reflash — the right
place for settings and high scores.

| | |
| --- | --- |
| `kv.get(key [, default])` | nil when unset and no default given |
| `kv.set(key, value)` | strings only |
| `kv.remove(key)` | |

Keys are namespaced per app, so two apps using the same key never collide. NVS
caps a key at 15 characters and the prefix is a 6-hex-char hash of your *full*
app id plus a colon — a hash rather than the first 6 characters of the id, so
apps that share a prefix no longer land in the same namespace — which leaves
8 characters for your key.

---

### `badge.battery`

| | |
| --- | --- |
| `battery.volts()` | |
| `battery.percent()` | from a Li-Po curve, not a linear map |
| `battery.charging()` | *inferred* from voltage — there is no charge-status line on v1 |

---

### `badge.mic`

Off until asked for; the runtime turns them back off when your app stops.

| | |
| --- | --- |
| `mic.enable([true])` → ok | `mic.enable(false)` to stop |
| `mic.enabled()` | |
| `mic.level()` → left, right | 0..100, smoothed — for meters |
| `mic.db()` → left, right | dBFS, roughly −58..0, unsmoothed |
| `mic.read([n])` | interleaved L/R 16-bit samples, n capped at 1024 |

---

### `badge.se050`

| | |
| --- | --- |
| `se050.present()` | |
| `se050.atr()` | hex string, or nil |
| `se050.test()` | re-run the link test (~20 ms) |
| `se050.random(n)` → bytes | n is 1..64; nil + reason if the part does not answer |
| `se050.random_available()` | end-to-end probe — costs one exchange, so ask once |

`random()` never falls back: the whole point of asking the secure element is to
be told no when it is not the source. Use `system.random_bytes()` for entropy
that just has to be hardware.

Both block on I²C for tens of milliseconds, so draw a block and buffer it rather
than calling per value. And treat what comes back as untrusted — a part that
answers the framing without producing entropy hands you a constant block, which
will sit in a rejection-sampling loop forever if nothing checks for it.

---

### `badge.wifi`

`connect` returns immediately — joining takes seconds and blocking the loop for
that long would stall the display and every other radio. Poll `connected()`.

| | |
| --- | --- |
| `wifi.connect(ssid [, password])` | does not overwrite the user's saved network |
| `wifi.connect_enterprise{...}` | WPA2-Enterprise; see below |
| `wifi.enterprise()` | true if the current/saved network is 802.1X |
| `wifi.disconnect()` | |
| `wifi.connected()` | true for **any** route, including a [phone bridge](#phone-bridge-http-over-ble) |
| `wifi.status()` | `"connecting"`, `"connected"`, `"auth failed"`, `"bridged via phone"`, … |
| `wifi.ip()` / `wifi.ssid()` / `wifi.rssi()` / `wifi.mac()` / `wifi.channel()` | `ssid()` is `"phone-bridge"` when bridged; the rest are never faked |

MAC addresses are **lowercase everywhere** in this SDK — `wifi.mac()`,
`espnow.peers()[i].mac` and the `mac` argument to `on_espnow`. That matters
because comparing your own address against an ESP-NOW sender is the obvious way
to ignore your own broadcasts, and a case mismatch would make it silently never
match.
| `wifi.scan()` | start an async scan |
| `wifi.scanning()` | |
| `wifi.networks()` | array of `{ssid, rssi, encrypted}` |
| `wifi.hotspot([password])` | SoftAP at 192.168.4.1 |

`connect_enterprise` takes a table, because eight positional arguments would be
unreadable:

```lua
local ok, err = badge.wifi.connect_enterprise{
  ssid     = "DefCon",
  username = "myuser",           -- required for PEAP/TTLS
  password = "mypass",
  ca       = "defcon34-wifi.pem", -- a name in /certs; omit to skip validation
  domain   = "wifireg.defcon.org",
  method   = "peap",             -- or "ttls"
  identity = nil,                -- outer identity; defaults to username
  phase2   = "mschapv2",         -- TTLS only
}
```

Returns `false, reason` if the profile is rejected — most often a `ca` that is
not installed. Like `wifi.connect`, it never overwrites the user's saved
network, and it returns immediately: poll `wifi.connected()`.

### `badge.http`

These **do** block — there is no sane non-blocking shape for them in a
callback-driven script. They extend the watchdog by their own timeout so a slow
request is not killed as a runaway loop.

| | |
| --- | --- |
| `http.get(url [, timeout_ms])` → status, body | nil + message on failure |
| `http.post(url, body [, content_type] [, timeout_ms])` → status, body | |

Timeout defaults to 5000 ms and is capped at 10000. `nil, "wifi not connected"`
still means exactly what it always did: **no route at all**, neither Wi-Fi nor a
phone bridge.

The transport is chosen by `net_route`: real Wi-Fi if there is any, otherwise a
[phone bridge over BLE](#phone-bridge-http-over-ble). Over the bridge the
response body is capped at 32 KB and **the phone terminates TLS** — read that
section before sending anything sensitive.

---

### `badge.espnow`

Connectionless badge-to-badge messaging. Presence beacons are handled by the OS
and never surface as app messages, so an app that only wants to know who is
nearby can poll `peers()` and never define `on_espnow`.

| | |
| --- | --- |
| `espnow.enable([true] [, channel])` → ok | |
| `espnow.enabled()` / `espnow.channel()` | |
| `espnow.broadcast(data)` → ok | |
| `espnow.send(mac, data)` → ok | `mac` is `"aa:bb:cc:dd:ee:ff"` |
| `espnow.peers()` | array of `{mac, name, rssi, age_ms, packets}`, **strongest first** |
| `espnow.signal(mac)` → rssi, age_ms | nil if not heard |
| `espnow.beacon([bool])` | whether this badge announces itself |
| `espnow.clear()` | forget the peer table |

`espnow.MAX_PAYLOAD` is 240 bytes; `espnow.MAX_PEERS` is 20. A peer that has
been quiet for 12 s drops off.

Incoming: `on_espnow(mac, data, rssi)`.

---

### `badge.ble`

| | |
| --- | --- |
| `ble.enable([true] [, name])` → ok | |
| `ble.enabled()` / `ble.connected()` | |
| `ble.send(line)` → ok | a newline is appended if missing |
| `ble.listen([true])` | claim the link; lines arrive as `on_ble(line)` |
| `ble.listening()` | |
| `ble.address()` | |

Until you call `ble.listen()`, incoming lines are interpreted as app-push
commands. That is what keeps "push an app over BLE" working from the launcher,
and the claim is released automatically when your app stops.

---

## Pushing apps

Three transports, one set of operations. All of them want the six-digit pairing
code from **Settings → Push** unless pairing has been turned off there.

### The tool

```sh
tools/badge-push.py --host solana-badge.local --token 123456 push apps/hello --run
tools/badge-push.py --host solana-badge.local --token 123456 list
tools/badge-push.py --host solana-badge.local --token 123456 rm hello
tools/badge-push.py --host solana-badge.local --token 123456 logs

# over BLE (needs `pip install bleak`)
tools/badge-push.py --ble badge-4F2A --token 123456 push apps/hello --run
```

`BADGE_TOKEN` works instead of `--token`. Pushing a directory sends every
`.lua`, `.ini`, `.txt`, `.json`, `.csv`, `.png`, `.jpg` in it, recursively,
skipping dotfiles.

### The web UI

`http://<ip>/` or `http://solana-badge.local/` — one self-contained page, no
external assets (the badge is often its own access point with no route to the
internet). It has an editor, a file picker, the app list with run/delete, the
log, and the Wi-Fi setup form.

### HTTP API

| | |
| --- | --- |
| `GET /api/status` | generic hardware status — the only route reachable without the token; unauthenticated it omits device name, IP, SSID, RSSI and the running app, and never returns an `auth` field |
| `GET /api/apps` | installed apps |
| `POST /api/app?id=&path=&enc=&append=` | write one file; body is the content |
| `DELETE /api/app?id=` | uninstall |
| `POST /api/run?id=` | launch |
| `POST /api/stop` | back to the launcher |
| `GET /api/wifi` | status + last scan |
| `POST /api/wifi/scan` | start an async scan |
| `POST /api/wifi` | join; see the body shapes below |
| `GET /api/certs` | installed CA certificates |
| `POST /api/certs?name=` | upload a PEM; body is the certificate |
| `DELETE /api/certs?name=` | |
| `GET /api/logs` | the log ring |
| `POST /api/reboot` | |

`POST /api/wifi` takes either

```json
{"ssid": "home-wifi", "password": "hunter2"}
```

or, for 802.1X,

```json
{"ssid": "DefCon", "security": "eap", "eap_method": "peap", "phase2": "mschapv2",
 "username": "myuser", "password": "mypass", "identity": "",
 "ca": "defcon34-wifi.pem", "domain": "wifireg.defcon.org"}
```

Auth is the pairing code in an `X-Badge-Token: <code>` header. A `?token=<code>`
query parameter is accepted only on read-only `GET` routes; every state-changing
route requires the header, so a token cannot ride along in a URL that ends up in
a log or a referrer. Those routes additionally check the request's `Host` (and
`Origin`, when present) against the badge's own address, which defeats DNS
rebinding — a malicious page cannot drive a write just because the browser can
reach the badge. There is no wildcard `Access-Control-Allow-Origin`, so a
cross-origin page has to preflight and is refused. Wrong codes are compared in
constant time and, after 5 failures, lock the write path out with a growing
backoff (30 s, up to 5 min). `enc=base64` decodes the body first, which is how
binary assets get through. One file is capped at 96 KB — bodies are buffered in
RAM.

```sh
curl -H "X-Badge-Token: 123456" --data-binary @main.lua \
  "http://solana-badge.local/api/app?id=hello&path=main.lua"
curl -H "X-Badge-Token: 123456" -X POST \
  "http://solana-badge.local/api/run?id=hello"
```

### BLE / serial line protocol

The same operations reduced to text lines that fit a 20-byte GATT notification.
Nordic UART Service: `6e400001-b5a3-f393-e0a9-e50e24dcca9e`, RX `…0002`
(write), TX `…0003` (notify). The identical protocol is available on the USB
serial console, which is how you get an app onto a badge with no radio
configured yet.

| Command | Reply |
| --- | --- |
| `PING` | `OK pong` |
| `INFO` | `OK <name> <version> heap=… fs_free=…` — **needs auth** |
| `AUTH <code>` | `OK authed` / `ERR bad code` / `ERR locked out` |
| `LIST` | `+ <id> <bytes> <name>` per app, then `OK <count>` |
| `BEGIN <id> <path>` | `OK begin` — creates or truncates |
| `DATA <base64>` | `OK <bytes>` — appends a decoded chunk |
| `END` | `OK <total>` |
| `ABORT` | `OK aborted` |
| `DEL <id>` | `OK deleted` |
| `RUN <id>` | `OK launching` |
| `STOP` | `OK stopped` |
| `CERTBEGIN <name>` | `OK begin` — then `DATA`/`END` as for a file |
| `CERTS` | `+ <name> <bytes>` per certificate, then `OK <count>` |
| `CERTDEL <name>` | `OK deleted` |
| `SETWIFI <field> <value>` | `OK <field>` — stages one field |
| `JOINWIFI` | `OK joining <ssid>` |
| `WIFI` | `OK <status> ssid=… ip=… rssi=…` |
| `BRIDGE ON` / `OFF` / `?` | `OK bridge ready` / `OK bridge off` / `OK bridge on\|off` — see [Phone bridge](#phone-bridge-http-over-ble) |

`SETWIFI` fields: `ssid`, `pass` (PSK), `security` (`psk`/`eap`), `method`,
`identity`, `user`, `eappass`, `ca`, `domain`, `phase2`. The value is the rest
of the line verbatim, so passwords containing spaces survive. A whole enterprise
profile needs several `SETWIFI` lines and then one `JOINWIFI` — no single line
of a 20-byte-MTU protocol could carry it.

Every command answers with exactly one `OK`/`ERR` line, optionally preceded by
`+` data lines — so waiting for `OK`/`ERR` is a complete framing rule and no
client needs to guess a timeout.

Only `PING` answers before authentication; `INFO`, `ECHO` and every operation
below them require an `AUTH` first. A wrong `AUTH` invalidates the session, so a
guessed code cannot escalate an already-open one, and the code is compared in
constant time. Five wrong codes lock the `AUTH` path out with a growing backoff
(30 s, up to 5 min); that counter is deliberately **not** cleared by a
disconnect, so reconnecting cannot reset the guess rate. Authorisation itself is
per-session and is dropped when the transport disconnects — a later central does
not inherit the previous one's authorisation or any half-finished transfer.

---

## Phone bridge (HTTP over BLE)

A badge with no Wi-Fi still has a phone standing next to it, and that phone has
internet. The phone connects to the same Nordic UART link the push protocol
uses, says `BRIDGE ON`, and from then on the badge hands it HTTP requests to run
and gets the responses back. `badge.http`, the app-store broker and anything
else that goes through `net_route` work unchanged.

The wire format lives in `BRIDGE_PROTOCOL.md` next to this file — the same
document the phone side is built from — and is implemented by
`src/net/ble_bridge.{h,cpp}`.

> **One badge-side extension to v1.** The spec has no frame for request headers,
> and `broker_client` cannot talk to a broker without `Authorization: Bearer`.
> The badge therefore emits `%HDR <id> <b64 name> <b64 value>` between `%CT` and
> `%BODY`, and only when a header is actually needed. A phone that does not
> implement `%HDR` should ignore the unknown frame; the request then goes out
> unauthenticated, the broker answers 401, and that surfaces as an ordinary
> broker error rather than a hang. Nothing else on the badge sends headers, so
> `badge.http` over the bridge is unaffected either way.

### Read this first: the phone terminates TLS

**A bridged request is not protected end-to-end.** The badge sends a URL over
BLE, the phone performs the fetch on its own network, and the phone sends bytes
back. That means:

- **The broker's pinned-CA guarantee does not hold over the bridge.** On Wi-Fi,
  `broker_client` validates the broker's certificate against a compiled-in
  Let's Encrypt root (`src/net/broker_ca.h`) and refuses to fall back to
  `setInsecure()`. Over the bridge the badge never sees a certificate at all.
- `https://` in a bridged URL describes the phone's connection, not the badge's.
  The phone, its browser, its OS and anything on its network can read **and
  rewrite** every request and response — including offers, tokens and script
  bodies. The `sha256` in a broker offer is checked against a body the same
  phone supplied, so it does not close this hole either. (The real fix, as noted
  in `broker_client.cpp`, is a publisher signature over the offer.)
- Requests originate from the **phone's** network position. A hostile installed
  Lua app can therefore probe the phone's LAN; the phone side is required to
  carry an SSRF guard that refuses loopback/private literals by default.

So: **only bridge to a phone you own.** The bridge is off by default, requires
an authenticated push session to turn on, and is dropped on every one of BLE
disconnect, `BRIDGE OFF`, `ble.listen(false)`, and a Lua app exiting. After an
app exits the phone must re-send `AUTH` and `BRIDGE ON`.

### Handshake

```
phone -> badge   AUTH <pairingCode>        OK authed
phone -> badge   BRIDGE ON                 OK bridge ready
                 ... badge now emits %REQ frames whenever it wants internet ...
phone -> badge   BRIDGE OFF                OK bridge off
```

Every bridge frame starts with `%` and is pulled out of the receive stream
before the app/push-protocol decision is made (`ble_mgr::update()` →
`ble_mgr::pumpBridge()`). An app that called `ble.listen()` **never** sees a
`%`-prefixed line, and cannot forge a reply to one — which is what lets the
bridge keep working while an app owns the link.

### Limits

| | |
| --- | --- |
| Response cap | **32 KB** decoded (`ble_bridge::RESP_CAP`), buffered in PSRAM |
| In flight | one request at a time |
| Payload lines | ≤ 240 base64 characters (180 decoded bytes) |
| Timeout | the caller's timeout plus 2.5 s of BLE slack |

A response larger than 32 KB comes back truncated and flagged. For the broker
that means a script over 32 KB cannot be installed over the bridge — it fails
with *"script too large for the phone bridge (32 KB cap)"* rather than failing
its hash check and looking like tampering. Install those over Wi-Fi.

### `wifi.connected()` when bridged — a deliberate convenience lie

`badge.wifi.connected()` returns **true whenever the badge has a route**, which
includes a phone bridge with no Wi-Fi at all. `wifi.ssid()` then reads
`"phone-bridge"` and `wifi.status()` reads `"bridged via phone"`.

This is on purpose. Every app already written gates its HTTP on
`wifi.connected()` and none of them know the bridge exists; making the gate mean
*"can I make a request"* is what lets them work unchanged. `wifi_mgr::connected()`
already tells a comparable lie for SoftAP mode. `wifi.ip()`, `wifi.rssi()`,
`wifi.mac()` and `wifi.channel()` are **not** faked — they report the real radio,
so a bridged badge shows no IP.

One consequence worth knowing: while the badge runs its own hotspot,
`wifi_mgr::connected()` is true, so `net_route` picks Wi-Fi and never the
bridge, even though a SoftAP has no route to the internet. That is pre-existing
behaviour, not something the bridge introduced.

### Throughput

`ble_mgr::send()` used to push 20 bytes per notification with a 4 ms sleep
between them — about 5 KB/s, which made a bridged response unusable. It now
sizes each notification from the **negotiated ATT MTU** and drops the sleep to
one scheduler tick. A central that never runs an MTU exchange still gets the
original 20 bytes / 4 ms exactly, so nothing about a transfer over a dumb link
changes. `badge-push.py` reassembles on `\n` and is chunk-size agnostic.

### Bringing it up on hardware

1. **Flash** the firmware and watch the console
   (`arduino-cli monitor -p /dev/cu.usbmodem* -c baudrate=115200`).
2. **Regression check first, before touching the bridge.** With Wi-Fi joined,
   push an app the old way — `python3 tools/badge-push.py --ble push apps/hello`.
   This exercises the new MTU-sized `ble_mgr::send()`; a transfer that used to
   work and now fails means the notify pacing is too aggressive (raise
   `NOTIFY_PACING_FAST_MS` in `src/net/ble_mgr.cpp`). Also confirm
   `badge.http.get` still works over Wi-Fi.
3. **Disconnect Wi-Fi** (Settings → Wi-Fi → Disconnect) so the bridge is the
   only route. `wifi.connected()` should now be false.
4. **Connect the phone** from the webapp's bridge page and send `AUTH <code>`
   (pairing code is in Settings → Push) — expect `OK authed`. Then `BRIDGE ON` —
   expect `OK bridge ready`, and `[bridge] on - HTTP tunnels through the phone`
   in the badge console.
5. **Run a Lua `http.get`.** Either push a two-line app or use the console. The
   badge console should show the request being issued; the phone should log
   `%REQ / %URL / %SEND` in and `%RES / %DATA… / %END` out.
6. **Check the numbers.** A ~10 KB response should come back in a couple of
   seconds, not thirty. If frames are lost, the badge logs
   `bridge rx queue full, frame dropped` — raise `BRIDGE_QUEUE_LEN`.
7. **Truncation.** Fetch something over 32 KB; the badge should return the first
   32 KB and log `response truncated to 32768 bytes`.
8. **Teardown paths**, each of which must log `[bridge] off` and leave
   `wifi.connected()` false: stop the running app; send `BRIDGE OFF`; walk the
   phone out of range. After each, `BRIDGE ?` (once reconnected/re-authed)
   should answer `OK bridge off`.
9. **Timeout.** Have the phone stall (or kill the page) mid-response; the badge's
   `http.get` should return `nil, "bridge timeout"` after the request timeout
   plus ~2.5 s, and the app must survive — not die on its time budget.
10. **Broker over the bridge** (optional): enable the broker in Settings with no
    Wi-Fi. Registration and polling should work; a script over 32 KB should fail
    with *"script too large for the phone bridge"*.

---

## Identity

Every badge generates an **Ed25519 keypair** on first boot and keeps it for life.
Ed25519 because that is what Solana signs with, so the public key is a Solana
address and the base58 the rest of the system passes around is the encoding a
wallet would show.

The **badge ID** is the first eight base58 characters of that public key —
derived, never assigned. It is what someone types into the web App Store to send
you an app, and it is on the badge under **Settings → Identity**.

The private half lives in one of two places, and which one is not cosmetic:

| Source | What it means |
| --- | --- |
| **secure element** | Generated inside the SE050 and used there. The private key is never readable over I²C, by this firmware or by anyone who takes the badge apart. |
| **software** | A key in NVS, signed on the ESP32 with a vendored TweetNaCl. What a badge falls back to when the SE050 does not answer or its applet refuses. |

The Settings screen reports which one honestly, and so does the registration
request. A badge with a dead secure element still has a working identity rather
than none at all — but it is a weaker one, and it says so.

Both the seed and the six-digit pairing code are drawn from the hardware TRNG:
`bootloader_random_enable()` is turned on before either is generated and turned
off afterwards, so neither comes from a boot-seeded PRNG.

The software seed, though, is written to NVS **in the clear**. Unlike the SE050
key — which is never readable over I²C and genuinely never leaves the part — a
software-fallback seed can be recovered by anyone who dumps the SPI flash, so it
does *not* "never leave the device." Flash encryption closes that gap, but it,
along with secure boot, is a provisioning-time step: both are irreversible eFuse
burns done at the factory/build stage and deliberately not something the firmware
turns on itself. On a badge without flash encryption, treat the software key as
readable by anyone holding the hardware.

**Settings → Identity → New identity** throws the keypair away and makes another.
That changes the badge ID and un-registers the badge from any broker, so it asks
first.

### Honest status of the secure-element path

The SE050 support is written from NXP's AN12413 APDU specification and their
Apache-2.0 host middleware, with every command encoding cited in the source. It
has **not been run against a real SE050** — no hardware was available. It is
built to fail closed: any wrong TLV, timeout or unexpected status word drops the
badge to the software key rather than to a broken one, and every stage logs under
the `id` and `se050` tags so a serial cable is enough to see which command failed.

Two things most likely to be wrong on real silicon, both named in
`src/hal/se050_apdu.cpp`: an applet configured to require SCP03, which is
deliberately not implemented; and a variant without Ed25519.

One detail worth knowing if you debug it: after generating a key the firmware
signs a probe **inside the SE050 and verifies it with TweetNaCl** before trusting
the key. That catches the riskiest undocumented detail — the SE050 returns
Ed25519 public keys and signature halves byte-reversed relative to RFC 8032 — and
means a badge refuses a secure-element key it cannot prove rather than shipping
signatures nobody can check.

---

## The app store

Someone opens the web App Store, points it at a GitHub repository, picks Lua
scripts out of its `scripts/` folder and addresses them to a badge ID. That badge
gets a prompt, and installs them only if the wearer says yes.

Turn it on in **Settings → App store**. Out of the box the broker address is the
official DEF CON broker, `https://broker.solanadefcon.com` (`DEFAULT_BROKER_URL`
in `config.h`): the firmware ships the Let's Encrypt/ISRG trust anchors compiled
in (`src/net/broker_ca.h`, regenerated by `tools/fetch-broker-ca.ts`), so a stock
badge validates that broker's TLS with no per-badge certificate upload. To point
at a different broker — there is no keyboard to type a URL, so set it from the web
UI (`POST /api/broker`) or over the push API — an `https://` one needs its CA
uploaded to `cert_store` under the name `broker-ca`; there is no `setInsecure()`
fallback, so an `https` broker with no pinned CA is refused. Plain `http://` is
accepted only in a build with `BROKER_ALLOW_INSECURE_HTTP` (the default, for a
LAN broker at a conference). The wire format is
[`broker/PROTOCOL.md`](../../broker/PROTOCOL.md).

What the badge does:

1. Registers once, proving it owns its public key by signing a challenge. The
   token it gets back is kept in NVS, so a reboot does not re-register.
2. Polls for offers every six seconds. Not a long-poll — a held request would
   stall the main loop, and a background task would need a lock around LittleFS.
   Six seconds of latency is cheaper than either.
3. On an offer, raises the prompt screen and pulses the LEDs. It shows the
   repository, and every script with its size and description. **A installs,
   B declines.** Nothing is downloaded before that.
4. On accept, fetches one script per frame, checks each against the SHA-256 in
   the offer, and installs it as its own app. Then it tells the broker what
   actually landed, which is what the sender sees on the web page. The SHA-256
   check proves the body matches the hash *the same broker sent*, so it only
   catches a corrupted download — it is not a publisher signature. Its integrity
   is only as good as the transport, which is why the default broker is pinned
   `https`. There is no end-to-end offer signature yet (a TODO in
   `broker_client.cpp`): a broker you do not trust, or a MITM on an unpinned
   `http` broker, could serve a body and a matching hash of its own choosing.

An offer can never interrupt a running app: the prompt is raised from the shell,
which only runs when no app does.

Each script becomes an app in its own right — `scripts/blinky.lua` installs as
`/apps/blinky/main.lua` and appears in the launcher.

The thing to be clear about: **anyone who knows your badge ID can offer you
scripts.** That is what an app store is, and the badge is what decides. Nothing
installs without a button press on the device, scripts run in the sandbox
described below with its memory and callback-time limits, and the broker
rate-limits offers so the prompt cannot be used as a nuisance. The sender is not
authenticated and does not claim to be.

---

## Architecture

```
solana-os.ino              setup/loop, button routing, app-exit cleanup
partitions.csv             16 MB layout
splash_images.h            boot artwork (PNG blobs)

src/
  config.h                 pins, tunables, key mapping, limits
  badge_log.{h,cpp}        UART0 + USB CDC + ring buffer
  settings.{h,cpp}         NVS: system config, and the Lua kv store

  hal/
    badge_i2c              shared bus, recovery, scan
    display                panel + the PSRAM framebuffer everything draws into
    leds                   WS2812B over RMT, boot/idle/pulse animations
    buttons                TCA9534, debounce, edge masks, auto-repeat
    power                  battery ADC + Li-Po curve
    mic                    PDM stereo, RMS levels
    se050                  soft reset + ATR, and the hardware RNG
    se050_t1               the T=1-over-I2C block layer, shared by both callers
    se050_apdu             SE05x commands: keygen, public key, EdDSA sign

  identity/
    identity               the Ed25519 badge identity, and the badge ID
    ed25519                signing, over the SE050 or over TweetNaCl
    encoding               base58 and base64
    tweetnacl.{c,h}        vendored — see TWEETNACL-README

  net/
    wifi_mgr               STA/AP, async scan, auto-connect, WPA2-Enterprise
    cert_store             CA certificates on /certs
    espnow_mgr             framing, peer table, beacons, RX queue
    ble_mgr                NUS transport, MTU-sized notify, two RX queues
    ble_bridge             HTTP-over-BLE: the badge half of the phone bridge
    net_route              picks Wi-Fi or the bridge for every outbound request
    push_protocol          the line protocol (BLE + serial)
    push_server            HTTP API + the web UI
    broker_client          app-store registration, polling, offer install

  apps/
    app_store              LittleFS catalogue, app.ini, install/remove

  lua_sdk/
    lua_runtime            the VM: allocator, watchdog, lifecycle, dispatch
    lua_bindings           builds the `badge` table
    lib_*.cpp              one file per module

  ui/
    theme                  the palette and the Solana gradient
    boot                   splash + LED sequence
    shell                  launcher, settings, radar, console, error screen

  lua/                     vendored Lua 5.4.8 (see Building)

apps/                      bundled example apps
tools/badge-push.py        the push client
```

### Two rules worth knowing

**Everything draws into one framebuffer.** `display::canvas()` is a 320×240
PSRAM sprite; `display::flush()` blits it once per frame if anything marked it
dirty. Nothing paints the panel directly except the splash. A still screen costs
no SPI traffic at all.

**Radio callbacks never touch Lua.** The ESP-NOW receive callback runs on the
Wi-Fi task and the BLE write callback on the BLE stack's task. Both push into a
small ring and the main loop drains it. Touching the Lua state from another task
would be a data race with no visible symptom until it corrupts the heap.

---

## The sandbox

An app is one `lua_State`, built fresh at launch and closed at stop. Nothing an
app leaves behind can reach the next one.

**Memory.** The allocator draws from PSRAM — internal SRAM is wanted by the
Wi-Fi and BLE stacks, and an app that eats it starves the radios. A hard 1 MB
cap turns "the badge ran out of memory" into "this app ran out of memory",
surfacing as an ordinary Lua error the app can even catch.

**Time.** An instruction-count hook checks a wall-clock deadline: 250 ms for a
normal callback, 5 s for `on_start`. Past it, the app gets an error — "app
exceeded its time budget (stuck in a loop?)" — instead of hanging the badge.
Bindings that legitimately block (`system.sleep`, `http.get`, `se050.random`)
push the deadline out by what they intend to spend, but only up to 12 s in total
per callback. That ceiling is what keeps the guarantee: a binding that grants
more time than it costs — `se050.random` grants 180 ms for an exchange that
usually takes 30 — would otherwise let a loop around it move the deadline away
faster than the clock reaches it, and nothing would ever stop the callback.

**Lifecycle.** `launch` and `stop` are *requests* applied between frames. An app
cannot destroy the state it is currently executing in.

**Files.** `badge.storage` is chrooted to `/apps/<your id>/`.

**Standard library.** `base`, `package`, `coroutine`, `table`, `string`, `math`,
`utf8` are open. `io`, `os` and `debug` are not: `io` would bypass the file
sandbox, and `os` carries `execute`, `remove`, `rename` and `exit`, none of
which mean anything sane here. A curated `os` with `time`/`clock`/`date`/
`difftime` is installed instead, because enough library code assumes it exists.

**Source only, no bytecode.** `load`, `loadfile` and `dofile` are text-only and
`string.dump` is disabled, so an app cannot load a precompiled Lua chunk from
any source. A crafted binary chunk is an arbitrary-memory-access escape, and
blocking it means every app that runs is source you could have read. The `entry`
file itself is loaded the same way.

**What this is not.** These limits stop accidents — a typo'd loop, a runaway
table, a stray path. They are not a defence against a deliberately hostile app:
it can still spin the CPU inside a single C binding, flood a radio, or fill the
filesystem. Treat pushing an app as running code you trust, and keep the pairing
code on.

---

## Radio notes

**ESP-NOW and Wi-Fi share one radio and therefore one channel.** Joining an
access point moves the station to that AP's channel and drags ESP-NOW with it.
Two badges on different Wi-Fi networks cannot see each other over ESP-NOW. The
firmware handles this honestly rather than hiding it:

- `wifi_mgr` tells `espnow_mgr` about every channel change
- the peer table is cleared on a move — stale RSSI from an unreachable channel
  would be worse than no reading
- the Settings hotspot is pinned to the configured ESP-NOW channel, so turning
  it on does not silently move everyone

For badge-to-badge work at an event, leave Wi-Fi off and set a channel everyone
agrees on.

**BLE and Wi-Fi coexist** but share the 2.4 GHz front end; running both costs
throughput on each. BLE is off by default for that reason. The BLE stack is
NimBLE (what Arduino-ESP32 3.x builds by default), which is why `ble_mgr.cpp`
does not add a 2902 descriptor by hand — NimBLE adds it for any characteristic
that declares NOTIFY, and doing it manually is deprecated.

---

## Troubleshooting

**Nothing on the screen, badge otherwise alive.** PSRAM. Check the board is
built with `PSRAM=opi`; the log says `FATAL: could not allocate framebuffer`.

**Launcher is empty after pushing.** The app id must be `[a-z0-9._-]`. Check
`GET /api/apps` — a rejected id returns 400 rather than failing silently.

**App dies instantly with "exceeded its time budget".** A loop in `on_update`
that does not return. Callbacks must return every frame; keep state in upvalues
and advance it a bit at a time.

**App dies with "not enough memory" well under 1 MB free.** That is the app's
own cap, not the system heap. `system.lua_memory()` reports used and limit.

**Can't see other badges on the radar.** Same channel? Wi-Fi joined to
something? See [Radio notes](#radio-notes).

**Web UI unreachable at `solana-badge.local`.** mDNS is unreliable on some
networks; use the IP from Settings → Push.

**Pushed an app and the badge is now stuck in it.** Hold B for 1.5 s.

**Enterprise Wi-Fi never associates.** Check the Console. `certificate '…' is
missing` means the CA name does not match anything in `/api/certs`. Repeated
`connecting` with no progress is usually a wrong username/password or a
`domain` that does not match the server certificate — try once with `domain`
empty to isolate which.

**`not a PEM certificate` when uploading.** The `.crt` is DER. Convert it:
`openssl x509 -inform der -in x.crt -out x.pem`.

**Enterprise works, then a normal network fails afterwards.** Should not happen
— `wifi_mgr` calls `esp_wifi_sta_enterprise_disable()` before any PSK
association — but if it does, reboot and check the Console for the order of
events.

**Serial console prints but does not respond.** It speaks the push protocol, not
free text. Try `PING`, then `AUTH <code>`.

---

## Extending the SDK

Adding a binding is four steps. Say you want `badge.gfx.star(x, y, r)`:

1. Write the C function in `src/lua_sdk/lib_gfx.cpp`:

   ```cpp
   int l_star(lua_State *L) {
     const int32_t x = (int32_t)luaL_checkinteger(L, 1);
     // ...
     display::touch();   // any drawing must mark the frame dirty
     return 0;           // number of return values
   }
   ```

2. Add it to that file's `FUNCTIONS[]` table.
3. Bump `SOLANA_OS_API_VERSION` in `config.h` if you changed an existing
   signature.
4. Document it above.

A whole new module is the same plus an `openFoo()` declared in
`lua_bindings.h` and called from `openBadge()`. Each opener is handed the
`badge` table on top of the stack, builds a sub-table with `setTable()`, and
leaves the stack as it found it.

Things to keep in mind:

- Use `luaL_check*` for required arguments and `luaL_opt*` for optional ones —
  they produce good error messages for free.
- `luaL_error` does not return; do not write cleanup after it. Free anything you
  allocated *before* raising.
- Anything that draws must call `display::touch()`.
- Anything that blocks must call `runtime::extendDeadline()` first.
- Never call into Lua from a FreeRTOS task other than the main loop. Queue it,
  the way `espnow_mgr` and `ble_mgr` do.
