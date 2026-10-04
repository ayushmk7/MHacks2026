#!/usr/bin/env python3
"""vkdev.py - one tool for everything done with a badge over USB serial.

Spec: docs/os/testing/testing.md ("The serial tool", "Dev hooks") and docs/os/platform/config.md
("Provisioning", "Serial commands"). Needs Python 3 and pyserial; nothing else.

    vkdev.py --port P info                       VKINFO, parsed
    vkdev.py --port P cmd "VKGET tokens"         any command
    vkdev.py --port P wait-ready [--timeout 30]  waits for "[os] ready" after a flash or reset
    vkdev.py --port P reset                      pulses the reset line, then waits for ready
    vkdev.py --port P state                      VKSTATE as JSON
    vkdev.py --port P btn a tap                  VKBTN <key> <tap|press|release|hold> [ms]
    vkdev.py --port P shot out.png               VKSHOT decoded to a PNG
    vkdev.py --port P push apps/pay [--id pay]   push one app folder
    vkdev.py --port P run pay                    RUN <id>          (stop: STOP)
    vkdev.py --port P monitor                    tail the log
    vkdev.py --port P [--port2 P2] test test/device/t_boot.py ... [--include-deferred]
    vkdev.py --port P provision --env dashboard/.env --cap 100.00 --max 1000.00 ...
    vkdev.py --selftest                          checks the codecs in this file; needs no badge

The badge's protocol, in short: one command per line at 115200 baud. Every command is answered by
exactly one line starting with OK or ERR, possibly after "+ ..." lines, and mixed in with "[tag]"
log lines. This tool waits for that line before it sends the next command.
"""

import argparse
import base64
import binascii
import errno
import importlib.util
import inspect
import json
import os
import queue
import re
import struct
import sys
import threading
import time
import traceback
import zlib

BAUD = 115200
MAX_LINE_CHARS = 250              # the badge's UART buffer holds 256 bytes
DATA_CHUNK_BYTES = 180            # raw bytes per DATA line (upstream's push protocol)
MAX_FILE_BYTES = 96 * 1024 - 187  # upstream refuses a chunk that could pass 96 KB; 187 is its margin
TAP_MS = 80                       # what the firmware uses for "tap" with no time given
BTN_SETTLE_S = 0.15               # after a key change: time for the loop to see it and redraw

_FINAL = re.compile(r"^(OK|ERR)\b")


class BadgeError(AssertionError):
    """The badge did not do what was asked: an ERR reply, a timeout, a failed check."""


class ShotCorrupt(BadgeError):
    """A VKSHOT transfer that did not decode; worth one more try."""


# --------------------------------------------------------------------------------------------
# Codecs (no badge needed; covered by --selftest)
# --------------------------------------------------------------------------------------------

_B58 = "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz"


def b58enc(data):
    """bytes -> base58 text (the Bitcoin alphabet, as the Solana blockchain uses)."""
    data = bytes(data)
    number = int.from_bytes(data, "big")
    text = ""
    while number:
        number, digit = divmod(number, 58)
        text = _B58[digit] + text
    return "1" * (len(data) - len(data.lstrip(b"\0"))) + text


def b58dec(text):
    """base58 text -> bytes. Raises ValueError on a character outside the alphabet."""
    number = 0
    for char in text:
        digit = _B58.find(char)
        if digit < 0:
            raise ValueError("not base58: %r" % char)
        number = number * 58 + digit
    body = number.to_bytes((number.bit_length() + 7) // 8, "big")
    return b"\0" * (len(text) - len(text.lstrip("1"))) + body


def rle_encode(pixels):
    """What the firmware's VKSHOT does: RGB565 little-endian bytes -> (count u8, pixel u16) triples."""
    out = bytearray()
    run_pixel = None
    run_length = 0
    for i in range(0, len(pixels), 2):
        pixel = pixels[i:i + 2]
        if run_length and pixel == run_pixel and run_length < 255:
            run_length += 1
        else:
            if run_length:
                out.append(run_length)
                out += run_pixel
            run_pixel = pixel
            run_length = 1
    if run_length:
        out.append(run_length)
        out += run_pixel
    return bytes(out)


def rle_decode(stream):
    """(count u8, pixel u16) triples -> RGB565 little-endian bytes."""
    if len(stream) % 3:
        raise ValueError("run-length stream is %d bytes, not a multiple of 3" % len(stream))
    out = bytearray()
    for i in range(0, len(stream), 3):
        if stream[i] == 0:
            raise ValueError("run of length 0")
        out += stream[i + 1:i + 3] * stream[i]
    return bytes(out)


def shot_encode(pixels):
    """The '+ <base64>' lines of a VKSHOT reply and its CRC text, as the firmware sends them."""
    stream = rle_encode(pixels)
    lines = ["+ " + base64.b64encode(stream[i:i + 72]).decode("ascii") for i in range(0, len(stream), 72)]
    return lines, "%08x" % (zlib.crc32(pixels) & 0xFFFFFFFF)


def shot_decode(payloads, width, height, crc_text):
    """The base64 payloads of a VKSHOT reply -> width*height*2 bytes of RGB565 little-endian."""
    try:
        stream = b"".join(base64.b64decode(payload, validate=True) for payload in payloads)
        pixels = rle_decode(stream)
        crc = int(crc_text, 16)
    except (binascii.Error, ValueError) as exc:
        raise ShotCorrupt("screenshot did not decode: %s" % exc)
    if len(pixels) != width * height * 2:
        raise ShotCorrupt("screenshot is %d bytes, expected %d" % (len(pixels), width * height * 2))
    if zlib.crc32(pixels) & 0xFFFFFFFF != crc:
        raise ShotCorrupt("screenshot CRC mismatch")
    return pixels


def rgb565_to_png(pixels, width, height):
    """RGB565 little-endian bytes -> the bytes of an 8-bit RGB PNG file."""
    values = struct.unpack("<%dH" % (width * height), pixels)
    table = {}
    rows = bytearray()
    for y in range(height):
        rows.append(0)  # filter type: none
        for value in values[y * width:(y + 1) * width]:
            rgb = table.get(value)
            if rgb is None:
                r, g, b = (value >> 11) & 0x1F, (value >> 5) & 0x3F, value & 0x1F
                rgb = bytes(((r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2)))
                table[value] = rgb
            rows += rgb

    def chunk(tag, body):
        return struct.pack(">I", len(body)) + tag + body + struct.pack(">I", zlib.crc32(tag + body) & 0xFFFFFFFF)

    return (b"\x89PNG\r\n\x1a\n"
            + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(bytes(rows), 9))
            + chunk(b"IEND", b""))


def parse_info(text):
    """'profile=dev api=2' -> {'profile': 'dev', 'api': '2'}."""
    return dict(pair.split("=", 1) for pair in text.split() if "=" in pair)


# --------------------------------------------------------------------------------------------
# The badge
# --------------------------------------------------------------------------------------------

class Badge:
    """One badge on a serial port. The device-test API of execution-plan.md section 5.2."""

    def __init__(self, port, baud=BAUD, code=None, _serial=None):
        self.port = port
        self.pair_code = code          # push pairing code; read with VKPAIR when None
        self._code_given = code is not None
        self._lines = queue.Queue()
        self._log = []
        self._closed = False
        if _serial is not None:
            self.ser = _serial
        else:
            import serial  # pyserial; imported here so --selftest runs without it
            ser = serial.Serial()
            ser.port = port
            ser.baudrate = baud
            ser.timeout = 0.05
            ser.write_timeout = 5
            # The badge's reset line is pulled low while RTS is asserted and DTR is not. The OS
            # asserts both when the port opens. pyserial then applies DTR before RTS, so asking for
            # both off before open() would pass through exactly that combination. Instead RTS is
            # dropped by open() while DTR is still on, and DTR is dropped afterwards: the port ends
            # up with both lines deasserted and the reset line is never touched.
            ser.dtr = True
            ser.rts = False
            ser.open()
            try:
                ser.dtr = False
            except OSError as exc:  # a port with no modem lines (pyserial's open() ignores the same)
                if exc.errno not in (errno.EINVAL, errno.ENOTTY):
                    raise
            self.ser = ser
        self._thread = threading.Thread(target=self._reader, daemon=True)
        self._thread.start()

    def close(self):
        self._closed = True
        try:
            self.ser.close()
        except Exception:
            pass

    # -- plumbing ---------------------------------------------------------------------------

    def _reader(self):
        buffer = bytearray()
        while not self._closed:
            try:
                data = self.ser.read(self.ser.in_waiting or 1)
            except Exception as exc:
                if not self._closed:
                    self._lines.put(BadgeError("serial port %s lost: %s" % (self.port, exc)))
                return
            if not data:
                continue
            buffer += data
            while True:
                cut = buffer.find(b"\n")
                if cut < 0:
                    break
                self._lines.put(bytes(buffer[:cut]).decode("utf-8", "replace").rstrip("\r"))
                del buffer[:cut + 1]

    def _next(self, deadline):
        """The next line from the badge, or None once `deadline` (time.monotonic()) has passed."""
        try:
            item = self._lines.get(timeout=max(deadline - time.monotonic(), 0.001))
        except queue.Empty:
            return None
        if isinstance(item, Exception):
            raise item
        return item

    def _drain(self):
        """Moves every line already received into the log."""
        while True:
            try:
                item = self._lines.get_nowait()
            except queue.Empty:
                return
            if isinstance(item, Exception):
                raise item
            self._log.append(item)

    def _send(self, line):
        if "\n" in line or "\r" in line:
            raise BadgeError("a command is one line: %r" % line)
        data = line.encode("utf-8")
        if len(data) > MAX_LINE_CHARS:
            raise BadgeError("line is %d bytes; the badge takes at most %d" % (len(data), MAX_LINE_CHARS))
        self.ser.write(data + b"\n")
        self.ser.flush()

    # -- commands ---------------------------------------------------------------------------

    def cmd(self, line, timeout=5):
        """Sends one line. Returns its '+ ...' lines followed by the final 'OK...' or 'ERR...' line."""
        self._drain()
        self._send(line)
        deadline = time.monotonic() + timeout
        reply = []
        while True:
            got = self._next(deadline)
            if got is None:
                raise BadgeError("no reply to %r within %g s" % (line, timeout))
            if got.startswith("+ ") or got == "+":
                reply.append(got)
            elif _FINAL.match(got):
                reply.append(got)
                return reply
            else:
                self._log.append(got)

    def ok(self, line, timeout=5):
        """cmd(), asserting an OK reply. Returns the text after 'OK '."""
        final = self.cmd(line, timeout)[-1]
        if not final.startswith("OK"):
            raise BadgeError("%s -> %s" % (line, final))
        return final[3:] if final.startswith("OK ") else ""

    def info(self):
        """VKINFO as a dict of strings, by field name."""
        return parse_info(self.ok("VKINFO"))

    def state(self):
        """VKSTATE as a dict."""
        text = ""
        for _ in range(2):  # a log line from another task can land inside the reply; ask again once
            text = self.ok("VKSTATE")
            try:
                return json.loads(text)
            except ValueError:
                continue
        raise BadgeError("VKSTATE is not JSON: %r" % text)

    def wait_state(self, pred, timeout=10):
        """Polls state() until pred(state) is true and returns that state."""
        deadline = time.monotonic() + timeout
        while True:
            state = self.state()
            if pred(state):
                return state
            if time.monotonic() >= deadline:
                raise BadgeError("state not reached within %g s; last: %s" % (timeout, json.dumps(state)))
            time.sleep(0.05)

    def btn(self, key, action="tap", ms=None, wait=True):
        """VKBTN. key: up down left right a b. action: tap, press, release, hold (needs ms).

        Returns once the key change has happened and the badge has had time to redraw: a tap or a
        hold blocks for its whole duration. Pass wait=False to return as soon as it is queued."""
        self.ok("VKBTN %s %s" % (key, action) + ("" if ms is None else " %d" % ms))
        if wait:
            if action in ("tap", "hold"):
                time.sleep((TAP_MS if ms is None else ms) / 1000.0)
            time.sleep(BTN_SETTLE_S)

    def shot(self, path=None):
        """The screen as RGB565 little-endian bytes (153,600 for 320x240). Writes a PNG if `path`."""
        failure = None
        for _ in range(3):
            try:
                width, height, pixels = self._shot_once()
                break
            except ShotCorrupt as exc:
                failure = exc
                time.sleep(0.2)
        else:
            raise failure
        if path:
            with open(path, "wb") as handle:
                handle.write(rgb565_to_png(pixels, width, height))
        return pixels

    def _shot_once(self):
        self._drain()
        self._send("VKSHOT")
        deadline = time.monotonic() + 60
        size = None
        payloads = []
        while True:
            got = self._next(deadline)
            if got is None:
                raise BadgeError("VKSHOT did not finish within 60 s")
            if size is None:
                header = re.match(r"^OK shot (\d+) (\d+)$", got)
                if header:
                    size = (int(header.group(1)), int(header.group(2)))
                elif _FINAL.match(got):
                    raise BadgeError("VKSHOT -> %s" % got)
                else:
                    self._log.append(got)
            elif got.startswith("+ "):
                payloads.append(got[2:])
            elif got.startswith("OK end "):
                return size[0], size[1], shot_decode(payloads, size[0], size[1], got[7:].strip())
            elif _FINAL.match(got):
                raise BadgeError("VKSHOT -> %s" % got)
            else:
                self._log.append(got)

    def _auth(self):
        """AUTH with the pairing code. Upstream drops the session whenever an app stops or a launch
        fails, so this runs before every file and before every RUN and STOP."""
        fetched = False
        if self.pair_code is None:
            self.pair_code = self._read_pair_code()
            fetched = True
        final = self.cmd("AUTH " + self.pair_code)[-1]
        if final.startswith("ERR bad code") and not fetched and not self._code_given:
            self.pair_code = self._read_pair_code()  # the code was changed on the badge
            final = self.cmd("AUTH " + self.pair_code)[-1]
        if not final.startswith("OK"):
            raise BadgeError("AUTH -> %s" % final)

    def _read_pair_code(self):
        final = self.cmd("VKPAIR")[-1]
        if not final.startswith("OK "):
            raise BadgeError("VKPAIR -> %s (not a dev build? give the pairing code from "
                             "Settings > Push with --code)" % final)
        return final[3:].strip()

    def push(self, folder, app_id=None, extra=None):
        """Pushes every file of an app folder, then `extra` ({relative_path: bytes}). Returns the
        relative paths written. app_id defaults to the folder's name."""
        folder = os.path.abspath(folder)
        if not os.path.isdir(folder):
            raise BadgeError("not a folder: %s" % folder)
        app_id = app_id or os.path.basename(folder)
        files = {}
        for root, dirs, names in os.walk(folder):
            dirs[:] = sorted(name for name in dirs if not name.startswith("."))
            for name in sorted(names):
                if name.startswith("."):
                    continue
                full = os.path.join(root, name)
                with open(full, "rb") as handle:
                    files[os.path.relpath(full, folder).replace(os.sep, "/")] = handle.read()
        for relative, data in (extra or {}).items():
            files[relative] = data.encode("utf-8") if isinstance(data, str) else bytes(data)
        for relative, data in files.items():  # refuse before anything is sent
            if " " in relative:
                raise BadgeError("the push protocol cannot carry a path with a space: %r" % relative)
            if len(data) > MAX_FILE_BYTES:
                raise BadgeError("%s is %d bytes; a pushed file is limited to 96 KB" % (relative, len(data)))
        for relative, data in files.items():
            self._push_file(app_id, relative, data)
        return list(files)

    def _push_file(self, app_id, relative, data):
        self._auth()
        self.ok("BEGIN %s %s" % (app_id, relative), timeout=10)
        for start in range(0, len(data), DATA_CHUNK_BYTES):
            chunk = data[start:start + DATA_CHUNK_BYTES]
            self.ok("DATA " + base64.b64encode(chunk).decode("ascii"), timeout=10)
        total = self.ok("END", timeout=20)
        if total.strip() != str(len(data)):
            raise BadgeError("%s: the badge stored %s bytes of %d" % (relative, total, len(data)))

    def run(self, app_id):
        """AUTH + RUN. The launch itself happens on the badge's next loop pass."""
        self._auth()
        self.ok("RUN " + app_id)

    def stop(self):
        """AUTH + STOP."""
        self._auth()
        self.ok("STOP")

    def reset(self, timeout=30):
        """Pulses the reset line the way esptool's hard reset does, then wait_ready()."""
        self.ser.dtr = False
        self.ser.rts = True   # reset line low
        time.sleep(0.1)
        self.ser.rts = False  # and released: the badge boots
        return self.wait_ready(timeout)

    def wait_ready(self, timeout=30):
        """Returns when the badge has logged '[os] ready', or answers a PING (sent every 2 s) with
        'OK pong'. On return every PING sent here has been answered, so the next cmd() is in step.
        Returns the seconds waited."""
        started = time.monotonic()
        deadline = started + timeout
        next_ping = started
        sent = 0
        answered = 0
        ready = False
        while not ready:
            now = time.monotonic()
            if now >= deadline:
                raise BadgeError("badge not ready within %g s" % timeout)
            if now >= next_ping:
                self._send("PING")
                sent += 1
                next_ping = now + 2.0
            got = self._next(min(deadline, next_ping))
            if got is None:
                continue
            if got == "OK pong":
                answered += 1
                ready = True
            else:
                self._log.append(got)
                ready = "[os] ready" in got
        waited = time.monotonic() - started

        # A PING sent while the badge was booting may still be waiting in its UART buffer; its
        # answer must not be taken for the reply to a later command. The badge handles everything
        # it has buffered in one loop pass, so the answers come in one burst.
        if answered == 0:
            self._send("PING")  # the loop is running now: this one is certainly answered
            sent += 1
        quiet = time.monotonic() + (5.0 if answered == 0 else 0.4)
        while answered < sent:
            got = self._next(quiet)
            if got is None:
                if answered == 0:
                    raise BadgeError("the badge logged ready but does not answer PING")
                break  # the rest were lost while the badge was in reset
            if got == "OK pong":
                answered += 1
                quiet = time.monotonic() + 0.4
            else:
                self._log.append(got)
        return waited

    # -- log --------------------------------------------------------------------------------

    def log(self):
        """Every line since the last clear_log() that was not a command's reply."""
        self._drain()
        return list(self._log)

    def clear_log(self):
        self._drain()
        self._log = []

    def wait_log(self, regex, timeout=10):
        """The first log line since the last clear_log() that matches `regex` (re.search)."""
        pattern = re.compile(regex)
        deadline = time.monotonic() + timeout
        checked = 0
        while True:
            self._drain()
            for line in self._log[checked:]:
                if pattern.search(line):
                    return line
            checked = len(self._log)
            if time.monotonic() >= deadline:
                raise BadgeError("no log line matching %r within %g s" % (regex, timeout))
            got = self._next(deadline)
            if got is not None:
                self._log.append(got)


# --------------------------------------------------------------------------------------------
# Scripted device tests
# --------------------------------------------------------------------------------------------

def _load_test(path):
    path = os.path.abspath(path)
    folder = os.path.dirname(path)
    if folder not in sys.path:
        sys.path.insert(0, folder)  # so a test can "import common"
    name = os.path.splitext(os.path.basename(path))[0]
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def _failure_text(exc, path):
    """One line: the message, and where in the test file it happened."""
    message = " ".join(str(exc).split()) or type(exc).__name__
    if not isinstance(exc, AssertionError):
        message = "%s: %s" % (type(exc).__name__, message)
    frames = traceback.extract_tb(exc.__traceback__)
    inside = [frame for frame in frames if os.path.abspath(frame.filename) == os.path.abspath(path)]
    if inside:
        frame = inside[-1]
        message += " [%s:%d: %s]" % (os.path.basename(frame.filename), frame.lineno, (frame.line or "").strip())
    return message


def run_tests(files, open_badge, port2, include_deferred):
    """Runs each test file. Prints PASS, FAIL or SKIP per file. Returns the number of failures."""
    badges = {}

    def badge_on(which):
        if which not in badges:
            badges[which] = open_badge(which)
        return badges[which]

    failures = 0
    for path in files:
        try:
            module = _load_test(path)
            run = getattr(module, "run", None)
            if not callable(run):
                raise BadgeError("no run(badge) function")
            needs = getattr(module, "NEEDS", None)
            if needs and not include_deferred:
                print("SKIP %s (needs %s)" % (path, needs))
                continue
            two = len(inspect.signature(run).parameters) >= 2
            if two and not port2:
                raise BadgeError("needs a second badge: give --port2")
            badge = badge_on(1)
            badge.clear_log()
            if two:
                second = badge_on(2)
                second.clear_log()
                run(badge, second)
            else:
                run(badge)
            print("PASS %s" % path)
        except KeyboardInterrupt:
            raise
        except Exception as exc:  # a failed assert, a BadgeError, or a bug in the test
            failures += 1
            print("FAIL %s: %s" % (path, _failure_text(exc, path)))
            sys.stdout.flush()
            traceback.print_exc()
            for which, badge in sorted(badges.items()):
                try:
                    tail = badge.log()[-40:]
                except Exception:
                    tail = []
                if tail:
                    print("--- last log lines, badge %d ---" % which, file=sys.stderr)
                    for line in tail:
                        print(line, file=sys.stderr)
        sys.stdout.flush()
    return failures


# --------------------------------------------------------------------------------------------
# Provisioning (config.md, "With the tool")
# --------------------------------------------------------------------------------------------

def read_env(path):
    """KEY=VALUE lines of an env file as a dict."""
    values = {}
    with open(path, "r", encoding="utf-8") as handle:
        for line in handle:
            line = line.strip()
            if not line or line.startswith("#") or "=" not in line:
                continue
            if line.startswith("export "):
                line = line[7:]
            key, value = line.split("=", 1)
            value = value.strip()
            if len(value) >= 2 and value[0] == value[-1] and value[0] in "\"'":
                value = value[1:-1]
            values[key.strip()] = value
    return values


def issuer_from_keypair(path):
    """The public key (base58) of a 64-byte keypair file: its last 32 bytes. The file is a JSON
    array of 64 numbers, as the dashboard and solana-keygen write it, or 64 raw bytes."""
    with open(path, "rb") as handle:
        raw = handle.read()
    try:
        raw = bytes(json.loads(raw.decode("utf-8")))
    except (ValueError, TypeError):
        pass
    if len(raw) != 64:
        raise BadgeError("%s is not a 64-byte keypair; give the issuer with --issuer <base58>" % path)
    return b58enc(raw[32:])


def _provision_set(badge, key, value):
    reply = badge.cmd("VKSET %s %s" % (key, value))[-1]
    if reply == "OK":
        return
    if not reply.startswith("OK pending"):
        raise BadgeError("VKSET %s -> %s" % (key, reply))
    print("%s: hold SELECT on the badge to confirm the change" % key)
    deadline = time.monotonic() + 130  # longer than the longest approval timeout
    while badge.ok("VKGET " + key) != value:
        if time.monotonic() >= deadline:
            raise BadgeError("%s was not confirmed on the badge" % key)
        time.sleep(0.5)


def provision(badge, args):
    info = badge.info()
    if info.get("provisioned") == "1" and not args.force:
        raise BadgeError("this badge is already provisioned; --force changes it (each secure key "
                         "then needs a hold on the badge)")
    env = read_env(args.env)
    missing = [name for name in ("RPC_URL", "HACK_MINT", "HACK_SYMBOL", "HACK_DECIMALS") if not env.get(name)]
    if missing:
        raise BadgeError("%s has no value for %s (run `npm run devnet:setup` in dashboard/)"
                         % (args.env, ", ".join(missing)))
    if args.issuer:
        issuer = args.issuer
    else:
        keypair = env.get("AUTHORITY_KEYPAIR") or "server/.keys/authority.json"
        issuer = issuer_from_keypair(os.path.join(os.path.dirname(os.path.abspath(args.env)), keypair))
    try:
        valid = len(b58dec(issuer)) == 32
    except ValueError:
        valid = False
    if not valid:
        raise BadgeError("the issuer key is not base58 of 32 bytes: %s" % issuer)
    if args.wifi and "|" in args.wifi[0]:
        raise BadgeError("VKWIFI cannot carry an SSID that contains '|'")

    tokens = "%s:%s:%s:%s:%s" % (env["HACK_MINT"], env["HACK_DECIMALS"], env["HACK_SYMBOL"], args.cap, args.max)
    _provision_set(badge, "issuer_key", issuer)
    _provision_set(badge, "tokens", tokens)
    _provision_set(badge, "rpc_url", env["RPC_URL"])
    if args.listener is not None:
        _provision_set(badge, "listener_url", args.listener)
    if args.wifi:
        badge.ok("VKWIFI %s|%s" % (args.wifi[0], args.wifi[1]))
    badge.ok("VKCOMMIT")
    if args.autostart is not None:
        badge.ok("VKAUTOSTART %s" % args.autostart)
    info = badge.info()
    print("provisioned: pubkey=%s key=%s" % (info.get("pubkey", "?"), info.get("key", "?")))


# --------------------------------------------------------------------------------------------
# Self-test
# --------------------------------------------------------------------------------------------

def selftest():
    import random
    rng = random.Random(2026)

    # base58
    assert b58enc(b"") == "" and b58dec("") == b""
    assert b58enc(bytes(32)) == "1" * 32 and b58dec("1" * 32) == bytes(32)
    assert b58enc(b"Hello World!") == "2NEpo7TZRRrLZSi2U"
    wrapped_sol = bytes.fromhex("069b8857feab8184fb687f634618c035dac439dc1aeb3b5598a0f00000000001")
    assert b58enc(wrapped_sol) == "So11111111111111111111111111111111111111112"
    assert b58dec("So11111111111111111111111111111111111111112") == wrapped_sol
    for _ in range(200):
        data = bytes(rng.randrange(256) for _ in range(rng.randrange(0, 40)))
        if rng.random() < 0.3:
            data = bytes(rng.randrange(0, 4)) + data
        assert b58dec(b58enc(data)) == data
    try:
        b58dec("0OIl")
        raise AssertionError("b58dec accepted characters outside the alphabet")
    except ValueError:
        pass

    # VKSHOT: flat areas, runs longer than 255, a gradient and noise
    width, height = 320, 240
    values = []
    for y in range(height):
        for x in range(width):
            if y < 60:
                values.append(0x0000)
            elif y < 120:
                values.append(0xF800 if x < 300 else 0x07E0)
            elif y < 180:
                values.append(((x * 31 // 319) << 11) | ((y * 63 // 239) << 5) | (x & 0x1F))
            else:
                values.append(rng.randrange(0x10000))
    pixels = struct.pack("<%dH" % (width * height), *values)
    assert len(pixels) == 153600
    lines, crc_text = shot_encode(pixels)
    assert all(line.startswith("+ ") and len(line) <= 98 for line in lines)
    assert all(len(line) == 98 for line in lines[:-1])
    payloads = [line[2:] for line in lines]
    assert shot_decode(payloads, width, height, crc_text) == pixels
    assert rle_decode(rle_encode(b"\x34\x12" * 1000)) == b"\x34\x12" * 1000
    assert rle_encode(b"\x34\x12" * 256) == b"\xff\x34\x12\x01\x34\x12"
    for broken in (payloads[:-1], payloads[1:], [payloads[0][:-4] + "AAAA"] + payloads[1:]):
        try:
            shot_decode(broken, width, height, crc_text)
            raise AssertionError("a damaged screenshot was accepted")
        except ShotCorrupt:
            pass

    # PNG: structure, chunk CRCs, and the pixels come back as RGB888
    png = rgb565_to_png(pixels, width, height)
    assert png[:8] == b"\x89PNG\r\n\x1a\n"
    offset = 8
    chunks = []
    while offset < len(png):
        (length,) = struct.unpack(">I", png[offset:offset + 4])
        tag = png[offset + 4:offset + 8]
        body = png[offset + 8:offset + 8 + length]
        (crc,) = struct.unpack(">I", png[offset + 8 + length:offset + 12 + length])
        assert crc == zlib.crc32(tag + body) & 0xFFFFFFFF
        chunks.append((tag, body))
        offset += 12 + length
    assert [tag for tag, _ in chunks] == [b"IHDR", b"IDAT", b"IEND"]
    assert chunks[0][1] == struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)
    rows = zlib.decompress(chunks[1][1])
    assert len(rows) == height * (1 + width * 3)

    def rgb(x, y):
        start = y * (1 + width * 3) + 1 + x * 3
        return tuple(rows[start:start + 3])

    assert all(rows[y * (1 + width * 3)] == 0 for y in range(height))
    assert rgb(0, 0) == (0, 0, 0)
    assert rgb(0, 60) == (255, 0, 0)        # 0xF800, stored as bytes 00 F8
    assert rgb(319, 60) == (0, 255, 0)      # 0x07E0
    tiny = zlib.decompress(rgb565_to_png(b"\x1f\x00\xff\xff", 2, 1)[41:-16])
    assert tiny == b"\x00\x00\x00\xff\xff\xff\xff"  # blue, then white

    # VKINFO
    assert parse_info("profile=dev api=2 pubkey=Gn2G key=software") == {
        "profile": "dev", "api": "2", "pubkey": "Gn2G", "key": "software"}
    print("selftest ok")


# --------------------------------------------------------------------------------------------
# Command line
# --------------------------------------------------------------------------------------------

def build_parser():
    parser = argparse.ArgumentParser(description="BadgeOS serial tool (see docs/os/testing/testing.md)")
    parser.add_argument("--port", help="serial port of the badge, e.g. /dev/cu.usbserial-10")
    parser.add_argument("--port2", help="serial port of a second badge (two-badge tests)")
    parser.add_argument("--baud", type=int, default=BAUD)
    parser.add_argument("--code", help="push pairing code (default: read with VKPAIR on a dev build)")
    parser.add_argument("--selftest", action="store_true", help="check this file's codecs; needs no badge")
    sub = parser.add_subparsers(dest="command")

    sub.add_parser("info", help="VKINFO, parsed")
    p = sub.add_parser("cmd", help="send one command line and print the reply")
    p.add_argument("line")
    p.add_argument("--timeout", type=float, default=5)
    p = sub.add_parser("wait-ready", help='wait for "[os] ready" after a flash or reset')
    p.add_argument("--timeout", type=float, default=30)
    p = sub.add_parser("reset", help="pulse the reset line and wait for ready")
    p.add_argument("--timeout", type=float, default=30)
    sub.add_parser("state", help="VKSTATE as JSON")
    p = sub.add_parser("btn", help="press a button")
    p.add_argument("key", choices=["up", "down", "left", "right", "a", "b"])
    p.add_argument("action", nargs="?", default="tap", choices=["tap", "press", "release", "hold"])
    p.add_argument("ms", nargs="?", type=int)
    p = sub.add_parser("shot", help="save the screen as a PNG")
    p.add_argument("path")
    p = sub.add_parser("push", help="push one app folder")
    p.add_argument("folder")
    p.add_argument("--id", help="app id (default: the folder's name)")
    p = sub.add_parser("run", help="RUN <id>")
    p.add_argument("id")
    sub.add_parser("stop", help="STOP the running app")
    p = sub.add_parser("monitor", help="print the badge's log until interrupted")
    p.add_argument("--seconds", type=float, help="stop after this long")
    p = sub.add_parser("test", help="run scripted device tests")
    p.add_argument("files", nargs="+")
    p.add_argument("--include-deferred", action="store_true", help="also run tests that declare NEEDS")
    p = sub.add_parser("provision", help="first-time setup of a badge (config.md)")
    p.add_argument("--env", required=True, help="the dashboard's .env file")
    p.add_argument("--cap", required=True, help="amount above which SELECT becomes a hold, e.g. 100.00")
    p.add_argument("--max", required=True, help="amount above which a payment is blocked, e.g. 1000.00")
    p.add_argument("--listener", help="backend listener URL, e.g. http://192.168.4.20:8788")
    p.add_argument("--wifi", nargs=2, metavar=("SSID", "PASSWORD"))
    p.add_argument("--autostart", metavar="APP_ID", help="app to start at boot")
    p.add_argument("--issuer", help="issuer public key in base58 (default: from AUTHORITY_KEYPAIR)")
    p.add_argument("--force", action="store_true", help="change a badge that is already provisioned")
    return parser


def main(argv=None):
    sys.modules.setdefault("vkdev", sys.modules[__name__])  # "import vkdev" in a test gets this module
    parser = build_parser()
    args = parser.parse_args(argv)
    if args.selftest:
        selftest()
        return 0
    if not args.command:
        parser.error("give a command (or --selftest)")
    if not args.port:
        parser.error("--port is required")

    opened = []

    def open_badge(which=1):
        port = args.port if which == 1 else args.port2
        badge = Badge(port, args.baud, code=args.code)
        opened.append(badge)
        return badge

    try:
        if args.command == "test":
            return 1 if run_tests(args.files, open_badge, args.port2, args.include_deferred) else 0

        badge = open_badge()
        if args.command == "info":
            for name, value in badge.info().items():
                print("%s=%s" % (name, value))
        elif args.command == "cmd":
            reply = badge.cmd(args.line, args.timeout)
            print("\n".join(reply))
            return 0 if reply[-1].startswith("OK") else 1
        elif args.command == "wait-ready":
            print("ready after %.1f s" % badge.wait_ready(args.timeout))
        elif args.command == "reset":
            print("ready after %.1f s" % badge.reset(args.timeout))
        elif args.command == "state":
            print(json.dumps(badge.state()))
        elif args.command == "btn":
            if args.action == "hold" and args.ms is None:
                parser.error("hold needs a time in ms")
            badge.btn(args.key, args.action, args.ms)
        elif args.command == "shot":
            pixels = badge.shot(args.path)
            print("%s (%d bytes of RGB565)" % (args.path, len(pixels)))
        elif args.command == "push":
            for relative in badge.push(args.folder, args.id):
                print("pushed %s" % relative)
        elif args.command == "run":
            badge.run(args.id)
        elif args.command == "stop":
            badge.stop()
        elif args.command == "monitor":
            end = None if args.seconds is None else time.monotonic() + args.seconds
            while end is None or time.monotonic() < end:
                line = badge._next(time.monotonic() + 0.5)
                if line is not None:
                    print(line)
                    sys.stdout.flush()
        elif args.command == "provision":
            provision(badge, args)
        return 0
    except KeyboardInterrupt:
        return 130
    except BadgeError as exc:
        print("error: %s" % exc, file=sys.stderr)
        return 1
    finally:
        for badge in opened:
            badge.close()


if __name__ == "__main__":
    sys.exit(main())
