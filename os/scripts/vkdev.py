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
    vkdev.py --port P provision --listener URL --wifi SSID PASSWORD [--badge-id N --label L]
    vkdev.py provision --dry-run ...             what provision would send; opens no port
    vkdev.py --selftest                          checks the codecs and the provisioning values; needs no badge

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

    def tidy(self):
        """Deletes every installed app that is not a folder of os/apps/, so every badge lists the
        same apps (test fixtures and hand-pushed apps are removed). Returns the ids deleted."""
        shipped = {name for name in os.listdir(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "apps"))
                   if os.path.isfile(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "apps", name, "app.ini"))}
        self.stop()
        self._auth()
        listed = [line[2:].split()[0] for line in self.cmd("LIST", timeout=10) if line.startswith("+ ")]
        extra = []
        for app_id in listed:
            if app_id in shipped:
                continue
            self._auth()
            final = self.cmd("DEL %s" % app_id, timeout=10)[-1]
            if final.startswith("OK"):
                extra.append(app_id)
            # anything else is a native app (compiled in, not a file): it is part of the firmware
        return extra

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
    # Fixtures the tests pushed are removed, so the launcher is the same on every badge afterwards.
    for which, badge in sorted(badges.items()):
        try:
            removed = badge.tidy()
            if removed:
                print("tidy badge %d: removed %s" % (which, " ".join(removed)))
        except Exception as exc:
            print("tidy badge %d failed: %s" % (which, exc), file=sys.stderr)
    return failures


# --------------------------------------------------------------------------------------------
# Provisioning (config.md, "With the tool")
#
# Where each value comes from; the first source that has it wins:
#   1. a flag: --issuer --mint --decimals --symbol --cap --max --rpc
#   2. os/provision.public.env, the committed file of public values (--public-env names another)
#   3. the file named with --env, and only when it is named: no dashboard .env is read by default
# The issuer is always given as a public key. Nothing in this file opens a keypair file: the
# AUTHORITY_KEYPAIR line of an env file is dropped while the file is parsed, so the path of the
# secret key is never known here, let alone opened. --selftest proves both.
# --------------------------------------------------------------------------------------------

PUBLIC_ENV_DEFAULT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..",
                                                   "provision.public.env"))

# (field, flag, name in an env file). These seven names are all that is ever taken from an env file.
PROVISION_FIELDS = (
    ("issuer", "--issuer", "ISSUER_PUBKEY"),
    ("mint", "--mint", "HACK_MINT"),
    ("decimals", "--decimals", "HACK_DECIMALS"),
    ("symbol", "--symbol", "HACK_SYMBOL"),
    ("cap", "--cap", "HACK_CAP"),
    ("max", "--max", "HACK_MAX"),
    ("rpc", "--rpc", "RPC_URL"),
)
ENV_NAMES = tuple(name for _, _, name in PROVISION_FIELDS)

TOKEN_ENTRY_MAX = 95              # the firmware's limit for one mint:decimals:symbol:cap:max entry
PUBKEY_PLACEHOLDER = "<PUBKEY: printed by the real run>"
KEY_PLACEHOLDER = "<se050|software: printed by the real run>"

_URL_USERINFO = re.compile(r"^[A-Za-z][A-Za-z0-9+.-]*://[^/?#]*@")


def _show_path(path):
    """A path as short as it can be written from the current directory."""
    try:
        relative = os.path.relpath(path)
    except ValueError:
        return path
    return path if relative.startswith("..") else relative


def _looks_secret(value):
    """True for text shaped like key material: a JSON array (the content of a keypair file), a PEM
    block, hex of 32 or 64 bytes, base58 of 64 bytes, or a URL that carries a user and password."""
    text = value.strip()
    if text.startswith("[") or "-----BEGIN" in text:
        return True
    if re.fullmatch(r"[0-9a-fA-F]{64}|[0-9a-fA-F]{128}", text):
        return True
    if _URL_USERINFO.match(text):
        return True
    try:
        return len(text) >= 80 and len(b58dec(text)) == 64
    except ValueError:
        return False


def read_env(path, public):
    """The provisioning values of an env file: {name: value}, for the names in ENV_NAMES only.

    Every other line is dropped while parsing and its value is not kept, so nothing later can use
    it. public=True is for the committed file: it is refused if it holds any other name, or a
    value shaped like key material, because that file must stay safe to commit."""
    values = {}
    shown = _show_path(path)
    try:
        handle = open(path, "r", encoding="utf-8")
    except OSError as exc:
        raise BadgeError("cannot read %s: %s" % (shown, exc.strerror or exc))
    with handle:
        for line in handle:
            line = line.strip()
            if not line or line.startswith("#") or "=" not in line:
                continue
            if line.startswith("export "):
                line = line[7:]
            name, value = line.split("=", 1)
            name = name.strip()
            if name not in ENV_NAMES:
                if public:
                    raise BadgeError(
                        "%s holds %s, which is not one of the public names (%s). That file is "
                        "committed and may hold public values only; nothing was read from it. A "
                        "dashboard .env is read only when it is named with --env"
                        % (shown, name, ", ".join(ENV_NAMES)))
                continue  # not ours: the value is never kept
            value = value.strip()
            if len(value) >= 2 and value[0] == value[-1] and value[0] in "\"'":
                value = value[1:-1]
            if public and _looks_secret(value):
                raise BadgeError("%s: the value of %s looks like a secret; that file is committed "
                                 "and may hold public values only" % (shown, name))
            values[name] = value
    return values


def resolve_provision(args):
    """({field: text}, {field: where it came from}) for PROVISION_FIELDS. Opens env files only."""
    public, public_label = {}, None
    if args.public_env is None:
        if os.path.isfile(PUBLIC_ENV_DEFAULT):
            public, public_label = read_env(PUBLIC_ENV_DEFAULT, True), _show_path(PUBLIC_ENV_DEFAULT)
    elif args.public_env != "none":
        public, public_label = read_env(args.public_env, True), _show_path(args.public_env)
    extra = read_env(args.env, False) if args.env else {}

    values, sources, missing, clashes = {}, {}, [], []
    for field, flag, name in PROVISION_FIELDS:
        given = getattr(args, field)
        first, second = public.get(name) or "", extra.get(name) or ""
        if given is not None:
            values[field], sources[field] = given, flag
        elif first and second and first != second:
            clashes.append("%s (%s)" % (name, flag))
        elif first:
            values[field], sources[field] = first, "%s %s" % (public_label, name)
        elif second:
            values[field], sources[field] = second, "--env %s %s" % (_show_path(args.env), name)
        else:
            missing.append("%s (%s)" % (name, flag))
    if clashes:
        # The values are not printed: an env file given with --env is not a public file.
        raise BadgeError("%s and --env %s disagree on %s. The badge and the backend must use the same "
                         "values: fix the file that is wrong, or decide with the flag"
                         % (public_label, _show_path(args.env), ", ".join(clashes)))
    if missing:
        raise BadgeError("no value for %s. Give the flag, or the name in %s. The issuer is given as a "
                         "public key (base58); this tool never reads a keypair file"
                         % (", ".join(missing), public_label or "a public env file (--public-env)"))
    return values, sources


def _is_key32(text):
    """base58 of exactly 32 bytes, as the firmware's KEY32 type accepts."""
    try:
        return 32 <= len(text) <= 44 and len(b58dec(text)) == 32
    except ValueError:
        return False


def _amount_raw(text, decimals):
    """Display units -> raw units by the rules of the firmware's sol_parse_amount; None if refused."""
    if not re.fullmatch(r"[0-9]*\.?[0-9]*", text) or not re.search(r"[0-9]", text):
        return None
    whole, _, fraction = text.partition(".")
    if len(fraction) > decimals:
        return None
    raw = int(whole + fraction.ljust(decimals, "0"))
    return raw if raw < 1 << 64 else None


def _url_problem(text, shortest, longest):
    """Why the firmware's STR type, or common sense, refuses this URL; None if it is fine."""
    if not all(" " < char <= "~" for char in text):
        return "must be printable ASCII without blanks"
    if not shortest <= len(text) <= longest:
        return "must be %d to %d characters" % (shortest, longest)
    if text and not re.match(r"^https?://[^/]+", text):
        return "must start with http:// or https://"
    return None


def _step_line(step, show=False):
    """The line one provisioning step sends. show=True: the same, with a Wi-Fi password left out."""
    if step[0] == "VKSET":
        return "VKSET %s %s" % (step[1], step[2])
    if step[0] == "VKWIFI":
        password = "<password, %d characters, not shown>" % len(step[2]) if show else step[2]
        return "VKWIFI %s|%s" % (step[1], password)
    if step[0] == "VKAUTOSTART":
        return "VKAUTOSTART %s" % step[1]
    return step[0]


def provision_lines(steps, show=False):
    """Every line a run sends to an unprovisioned badge, in order: VKINFO, the steps, a read-back
    of every key that was set, VKINFO."""
    read_back = ["VKGET " + step[1] for step in steps if step[0] == "VKSET"]
    return ["VKINFO"] + [_step_line(step, show) for step in steps] + read_back + ["VKINFO"]


def provision_plan(args):
    """Resolves and validates everything `provision` needs, before any badge is touched. Reads
    env files and nothing else. Returns {"values", "sources", "tokens", "steps"}."""
    values, sources = resolve_provision(args)
    problems = []

    for field, what in (("issuer", "the issuer key"), ("mint", "the mint")):
        if not _is_key32(values[field]):
            problems.append("%s is not base58 of 32 bytes: %r" % (what, values[field]))
    if values["issuer"] == values["mint"]:
        problems.append("the issuer key and the mint are the same address")
    if not re.fullmatch(r"[0-9]", values["decimals"]):
        problems.append("decimals must be one digit, 0 to 9: %r" % values["decimals"])
    else:
        decimals = int(values["decimals"])
        raw = {}
        for field in ("cap", "max"):
            raw[field] = _amount_raw(values[field], decimals)
            if raw[field] is None:
                problems.append("%s must be an amount with at most %d fraction digits: %r"
                                % (field, decimals, values[field]))
        if None not in raw.values() and raw["max"] != 0 and raw["cap"] > raw["max"]:
            problems.append("cap %s is above max %s" % (values["cap"], values["max"]))
    if not re.fullmatch(r"[A-Z0-9]{1,4}", values["symbol"]):
        problems.append("the symbol must be 1 to 4 characters of A-Z and 0-9: %r" % values["symbol"])
    tokens = "%s:%s:%s:%s:%s" % (values["mint"], values["decimals"], values["symbol"],
                                 values["cap"], values["max"])
    if len(tokens) > TOKEN_ENTRY_MAX:
        problems.append("the token entry is %d characters; the firmware takes %d" % (len(tokens), TOKEN_ENTRY_MAX))

    problem = _url_problem(values["rpc"], 8, 128)
    if problem:
        problems.append("the RPC URL %s" % problem)
    elif sources["rpc"] != "--rpc" and (_URL_USERINFO.match(values["rpc"]) or "?" in values["rpc"]):
        # Not printed. Every config value can be read from a badge with VKGET.
        problems.append("the RPC URL from %s carries a login or a query string. Anyone holding the "
                        "badge can read rpc_url: give a plain URL, or pass it with --rpc if this "
                        "is intended" % sources["rpc"])
    if args.listener is not None:
        problem = _url_problem(args.listener, 0, 128)
        if problem:
            problems.append("the listener URL %s" % problem)
    if args.wifi and (not args.wifi[0] or "|" in args.wifi[0]):
        problems.append("VKWIFI cannot carry an empty SSID or one that contains '|'")
    if args.badge_id is not None and not 1 <= args.badge_id <= 99:
        problems.append("--badge-id must be 1 to 99")

    steps = [("VKSET", "issuer_key", values["issuer"]), ("VKSET", "tokens", tokens),
             ("VKSET", "rpc_url", values["rpc"])]
    if args.listener is not None:
        steps.append(("VKSET", "listener_url", args.listener))
    if args.wifi:
        steps.append(("VKWIFI", args.wifi[0], args.wifi[1]))
    steps.append(("VKCOMMIT",))
    if args.autostart is not None:
        steps.append(("VKAUTOSTART", args.autostart))
    for step in steps:
        line = _step_line(step)
        if "\n" in line or "\r" in line or len(line.encode("utf-8")) > MAX_LINE_CHARS:
            problems.append("%s does not fit one line of %d bytes" % (_step_line(step, True)[:40], MAX_LINE_CHARS))
    if problems:
        raise BadgeError("these values would be refused; nothing was sent:\n  " + "\n  ".join(problems))
    return {"values": values, "sources": sources, "tokens": tokens, "steps": steps}


def _print_values(plan, args):
    """What will be set and where each value came from. Never prints a Wi-Fi password."""
    values, sources = plan["values"], plan["sources"]
    rows = [("issuer_key", values["issuer"], sources["issuer"]), ("tokens", plan["tokens"], "composed from:")]
    rows += [("  " + field, values[field], sources[field]) for field in ("mint", "decimals", "symbol", "cap", "max")]
    rows.append(("rpc_url", values["rpc"], sources["rpc"]))
    if args.listener is not None:
        rows.append(("listener_url", args.listener, "--listener"))
    else:
        rows.append(("listener_url", "(not sent)", "per venue: --listener http://<backend laptop IP>:8788"))
    if args.wifi:
        rows.append(("wifi", args.wifi[0], "--wifi (password not shown)"))
    else:
        rows.append(("wifi", "(not sent)", 'per venue: --wifi "<SSID>" "<password>"'))
    if args.autostart is not None:
        rows.append(("autostart", args.autostart, "--autostart"))
    print("values, and where each one comes from:")
    for name, value, source in rows:
        print("  %-12s = %s   [%s]" % (name, value, source))


# -- the entry for dashboard/server/config/badges.json ----------------------------------------

def _badges_load(path):
    """A badges file ({"badges": [...]}, the shape of dashboard/server/config/badges.json) as a
    dict. A file that does not exist yet is an empty one. Anything else is refused and left alone."""
    if not os.path.exists(path):
        return {"badges": []}
    try:
        with open(path, "r", encoding="utf-8") as handle:
            document = json.load(handle)
    except (OSError, ValueError) as exc:
        raise BadgeError("%s is not readable JSON (%s); it was left as it is" % (_show_path(path), exc))
    badges = document.get("badges") if isinstance(document, dict) else None
    if not isinstance(badges, list) or not all(isinstance(badge, dict) for badge in badges):
        raise BadgeError('%s is not {"badges": [...]}; it was left as it is' % _show_path(path))
    return document


def _badges_save(path, document):
    temporary = path + ".tmp"
    with open(temporary, "w", encoding="utf-8") as handle:
        handle.write(json.dumps(document, indent=2) + "\n")
    os.replace(temporary, path)


def badge_entry(badges, pubkey, key_location, badge_id, label, allocate):
    """Puts one badge into `badges` (a list of badges.json entries) and returns (entry, what was
    done). An entry with the same public key is updated. Otherwise the entry with `badge_id` is
    replaced. Otherwise a new entry is added, with `badge_id` or, if `allocate`, the lowest free id."""
    by_key = next((badge for badge in badges if badge.get("pubkey") == pubkey), None)
    by_id = None
    if badge_id is not None:
        by_id = next((badge for badge in badges if badge.get("id") == badge_id), None)
    if by_key is not None:
        if by_id is not None and by_id is not by_key:
            raise BadgeError("badge id %d belongs to another public key in the badges file (%s); "
                             "this badge is already there as id %s" % (badge_id, by_id.get("pubkey"), by_key.get("id")))
        old, what = by_key, "updated"
    elif by_id is not None:
        old, what = by_id, "replaced (was %s)" % by_id.get("pubkey")
        old["tokenAccount"] = None  # it belonged to the other key
    else:
        old, what = {}, "added"
        badges.append(old)
    if badge_id is None:
        badge_id = old.get("id")
    if badge_id is None and allocate:
        taken = set(badge.get("id") for badge in badges)
        badge_id = next(number for number in range(1, 101) if number not in taken)
    if label is None:
        label = old.get("label") or (None if badge_id is None else "Badge %d" % badge_id)
    new = {"id": badge_id, "label": label, "pubkey": pubkey,
           "keyLocation": key_location if key_location in ("se050", "software") else "unknown",
           "tokenAccount": old.get("tokenAccount"), "standIn": False}
    for name, value in old.items():
        new.setdefault(name, value)  # a field this tool does not know stays
    old.clear()
    old.update(new)
    badges.sort(key=lambda badge: badge["id"] if isinstance(badge.get("id"), int) else 1000)
    return old, what


def _badge_block(args, pubkey, key_location):
    """(document or None, entry, note) for this badge: the badges file's content with the badge in
    it (not yet written) when --badges-json is given, the entry, and one line saying what to do."""
    if args.badges_json:
        document = _badges_load(args.badges_json)
        entry, what = badge_entry(document["badges"], pubkey, key_location, args.badge_id, args.label, True)
        note = "%s: id %s %s; %d in the file" % (_show_path(args.badges_json), entry["id"], what, len(document["badges"]))
        return document, entry, note
    entry, _ = badge_entry([], pubkey, key_location, args.badge_id, args.label, False)
    note = None if entry["id"] is not None else \
        'no --badge-id: put the badge\'s number (1 to 99) in "id" before the entry goes into badges.json'
    return None, entry, note


def _print_badge_block(entry):
    """The last thing a run prints: one blank line, one label, one line of JSON to copy."""
    sys.stdout.flush()
    print()
    print("badges.json entry (copy the next line):")
    print(json.dumps(entry))
    sys.stdout.flush()


def provision_dry_run(args, plan):
    """Prints what a run would do. Opens no serial port and writes no file."""
    print("dry run: nothing is sent, no serial port is opened, no file is written")
    _print_values(plan, args)
    print("lines a run sends to an unprovisioned badge, in order (with --force on a provisioned one,")
    print("each secure VKSET answers OK pending and is followed by VKGET polls while SELECT is held):")
    for line in provision_lines(plan["steps"], show=True):
        print(line)
    _, entry, note = _badge_block(args, PUBKEY_PLACEHOLDER, KEY_PLACEHOLDER)
    entry["keyLocation"] = KEY_PLACEHOLDER
    if note:
        print(("would write " if args.badges_json else "") + note)
    _print_badge_block(entry)


def _provision_set(badge, key, value):
    reply = badge.cmd(_step_line(("VKSET", key, value)))[-1]
    if reply == "OK":
        return
    if not reply.startswith("OK pending"):
        raise BadgeError("VKSET %s -> %s" % (key, reply))
    print("%s: hold SELECT on the badge to confirm the change" % key)
    sys.stdout.flush()
    deadline = time.monotonic() + 130  # longer than the longest approval timeout
    while badge.ok("VKGET " + key) != value:
        if time.monotonic() >= deadline:
            raise BadgeError("%s was not confirmed on the badge" % key)
        time.sleep(0.5)


def provision(badge, args, plan):
    """Sends a plan made by provision_plan() to a badge, reads it back, and prints the badge's
    entry for badges.json."""
    info = badge.info()
    if info.get("provisioned") == "1" and not args.force:
        raise BadgeError("this badge is already provisioned; --force changes it (each secure key "
                         "then needs a hold on the badge)")
    pubkey = info.get("pubkey", "")
    if pubkey:
        _badge_block(args, pubkey, info.get("key", ""))  # a problem with the badges file stops the run here
    _print_values(plan, args)
    print("badge: pubkey=%s key=%s" % (pubkey or "(none)", info.get("key", "?")))
    sys.stdout.flush()

    for step in plan["steps"]:
        if step[0] == "VKSET":
            _provision_set(badge, step[1], step[2])
        else:
            badge.ok(_step_line(step))
    for step in plan["steps"]:
        if step[0] == "VKSET":
            stored = badge.ok("VKGET " + step[1])
            if stored != step[2]:
                raise BadgeError("%s reads back as %r, not %r" % (step[1], stored, step[2]))
    info = badge.info()
    if info.get("provisioned") != "1":
        raise BadgeError("VKCOMMIT answered OK but VKINFO says provisioned=%s" % info.get("provisioned"))
    if not info.get("pubkey"):
        raise BadgeError("the badge is provisioned but reports no public key (key=%s): there is no "
                         "entry for badges.json" % info.get("key", "?"))

    document, entry, note = _badge_block(args, info["pubkey"], info.get("key", ""))
    print("provisioned; read back from the badge: %s"
          % ", ".join(step[1] for step in plan["steps"] if step[0] == "VKSET"))
    failure = None
    if document is not None:
        try:
            _badges_save(args.badges_json, document)
        except OSError as exc:
            failure = "could not write %s: %s" % (_show_path(args.badges_json), exc.strerror or exc)
            note = None
    if note:
        print(("wrote " if document is not None else "") + note)
    _print_badge_block(entry)
    if failure:
        raise BadgeError(failure + " (the badge is provisioned; the entry above is complete)")


# --------------------------------------------------------------------------------------------
# Self-test
# --------------------------------------------------------------------------------------------

def _selftest_provision():
    """The provisioning value resolution, the plan, a whole run against a badge made of Python,
    and the proof that none of it touches the issuer's keypair file. No badge, no network."""
    import builtins
    import contextlib
    import io
    import tempfile

    issuer = "2SXh6Xng9b1qQBCEn3pt2Ucwcb4ibBWwBKuuTwNgEzJy"
    mint = "3VmWnzfWTfS5UGkwEjbMZnpjd1DPtKyMcwKeJc4QM9wP"
    pinned_tokens = "3VmWnzfWTfS5UGkwEjbMZnpjd1DPtKyMcwKeJc4QM9wP:2:HACK:100.00:1000.00"
    rpc = "https://api.devnet.solana.com"
    other = b58enc(bytes(range(32)))
    pinned_flags = ["--issuer", issuer, "--mint", mint, "--decimals", "2", "--symbol", "HACK",
                    "--cap", "100.00", "--max", "1000.00", "--rpc", rpc]

    def parse(*extra):
        return build_parser().parse_args(["provision"] + list(extra))

    def refused(*extra):
        """The message of the BadgeError that provision_plan raises for these arguments."""
        try:
            provision_plan(parse(*extra))
        except BadgeError as exc:
            return str(exc)
        raise AssertionError("accepted: %r" % (extra,))

    def swap(flag, value):
        changed = list(pinned_flags)
        changed[changed.index(flag) + 1] = value
        return ["--public-env", "none"] + changed

    class FakeBadge:
        """Answers the provisioning commands the way the firmware does; records every line."""
        ok = Badge.ok
        info = Badge.info

        def __init__(self, provisioned=False, key="software"):
            self.sent = []
            self.store = {}
            self.provisioned = provisioned
            self.key = key

        def cmd(self, line, timeout=5):
            self.sent.append(line)
            word, _, rest = line.partition(" ")
            if word == "VKINFO":
                return ["OK profile=dev api=2 provisioned=%d pubkey=%s key=%s"
                        % (self.provisioned, other if self.key != "none" else "", self.key)]
            if word == "VKSET":
                name, _, value = rest.partition(" ")
                self.store[name] = value
                secure = name in ("issuer_key", "tokens")
                return ["OK pending" if self.provisioned and secure else "OK"]
            if word == "VKGET":
                return ["OK " + self.store[rest] if self.store.get(rest) else "OK"]
            if word == "VKCOMMIT":
                self.provisioned = True
                return ["OK provisioned"]
            return ["OK joining" if word == "VKWIFI" else "OK"]

    def run(badge, *extra):
        """provision() on a FakeBadge; returns what it printed."""
        args = parse(*extra)
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            provision(badge, args, provision_plan(args))
        return out.getvalue()

    # -- the pinned values compose to the pinned token table, from flags alone ----------------
    plan = provision_plan(parse("--public-env", "none", *pinned_flags))
    assert plan["tokens"] == pinned_tokens
    assert plan["values"]["issuer"] == issuer and plan["sources"]["issuer"] == "--issuer"
    assert provision_lines(plan["steps"]) == [
        "VKINFO", "VKSET issuer_key " + issuer, "VKSET tokens " + pinned_tokens, "VKSET rpc_url " + rpc,
        "VKCOMMIT", "VKGET issuer_key", "VKGET tokens", "VKGET rpc_url", "VKINFO"]

    # -- the committed file holds the pinned values and nothing else ---------------------------
    # (A deployment with another issuer or mint changes os/provision.public.env and these lines.)
    assert os.path.isfile(PUBLIC_ENV_DEFAULT), "os/provision.public.env is missing"
    committed = read_env(PUBLIC_ENV_DEFAULT, True)  # raises on any name that is not public
    assert sorted(committed) == sorted(ENV_NAMES)
    plan = provision_plan(parse())
    assert plan["tokens"] == pinned_tokens and plan["values"]["issuer"] == issuer
    assert plan["values"]["rpc"] == rpc
    assert all(source.endswith("provision.public.env " + name)
               for (field, _, name) in PROVISION_FIELDS for source in [plan["sources"][field]])

    # -- validation: what the firmware would refuse is refused before a badge is touched -------
    assert _amount_raw("100.00", 2) == 10000 and _amount_raw("100", 2) == 10000
    assert _amount_raw("0", 2) == 0 and _amount_raw(".5", 2) == 50 and _amount_raw("7.", 0) == 7
    assert _amount_raw("1.001", 2) is None and _amount_raw("", 2) is None and _amount_raw(".", 2) is None
    assert _amount_raw("1.0.0", 2) is None and _amount_raw("-1", 2) is None and _amount_raw("1e3", 2) is None
    assert _amount_raw(str(1 << 64), 0) is None and _amount_raw(str((1 << 64) - 1), 0) == (1 << 64) - 1
    assert _is_key32(issuer) and _is_key32(mint) and _is_key32("1" * 32)
    assert not _is_key32(issuer[:-1]) and not _is_key32(issuer + "1") and not _is_key32("0" + issuer[1:])
    assert not _is_key32(b58enc(bytes(range(31)))) and not _is_key32(b58enc(bytes(range(33))))
    assert "issuer key is not base58 of 32 bytes" in refused(*swap("--issuer", issuer[:-1]))
    assert "mint is not base58 of 32 bytes" in refused(*swap("--mint", "O0Il"))
    assert "same address" in refused(*swap("--mint", issuer))
    for bad in ("10", "a", "", "-1"):
        assert "decimals must be one digit" in refused(*swap("--decimals", bad))
    for bad in ("hack", "HACKS", "", "H-K"):
        assert "symbol must be" in refused(*swap("--symbol", bad))
    assert "cap 1000.01 is above max 1000.00" in refused(*swap("--cap", "1000.01"))
    assert "cap must be an amount with at most 2 fraction digits" in refused(*swap("--cap", "100.001"))
    assert "max must be an amount" in refused(*swap("--max", "1,000"))
    assert provision_plan(parse(*swap("--max", "0")))["tokens"].endswith(":100.00:0")  # 0 = no max
    assert "RPC URL must start with http" in refused(*swap("--rpc", "api.devnet.solana.com"))
    assert "RPC URL must be printable ASCII" in refused(*swap("--rpc", "https://a b.example"))
    assert "listener URL must start with http" in refused(*swap("--rpc", rpc), "--listener", "192.168.4.20:8788")
    assert "SSID" in refused(*swap("--rpc", rpc), "--wifi", "a|b", "password")
    assert "--badge-id must be 1 to 99" in refused(*swap("--rpc", rpc), "--badge-id", "100")
    assert "does not fit one line" in refused(*swap("--rpc", rpc), "--wifi", "ssid", "p" * 250)
    assert "no value for" in refused("--public-env", "none") and "ISSUER_PUBKEY (--issuer)" in refused("--public-env", "none")

    assert _looks_secret("[12,34,56]") and _looks_secret(b58enc(bytes(range(64)))) and _looks_secret("ab" * 64)
    assert _looks_secret("ab" * 32) and _looks_secret("postgres://user:pass@host/db")
    assert _looks_secret("-----BEGIN PRIVATE KEY-----")
    assert not _looks_secret(issuer) and not _looks_secret(rpc) and not _looks_secret("100.00")

    with tempfile.TemporaryDirectory() as folder:
        def write(name, text):
            path = os.path.join(folder, name)
            with open(path, "w", encoding="utf-8") as handle:
                handle.write(text)
            return path

        # A stand-in for the backend laptop: a .env that names a keypair file, and that file.
        # (The 64 numbers are a counting pattern, not a key.)
        trap_name = "authority-keypair-TRAP.json"
        trap = write(trap_name, json.dumps(list(range(64))))
        dashboard_lines = [
            "DATABASE_URL=postgres://user:not-a-real-password@127.0.0.1:5433/db",
            "RPC_URL=" + rpc, "HACK_MINT=" + mint, "HACK_SYMBOL=HACK", "HACK_DECIMALS=2",
            "AUTHORITY_KEYPAIR=" + trap, "BADGE_LISTEN_PORT=8788"]
        dashboard_env = write("dashboard.env", "\n".join(dashboard_lines) + "\n")
        relative_env = write("relative.env", "\n".join(dashboard_lines).replace(trap, trap_name) + "\n")
        public_env = write("public.env", "# public\nISSUER_PUBKEY=%s\nHACK_CAP=100.00\nexport HACK_MAX='1000.00'\n" % issuer)
        badges_path = os.path.join(folder, "badges.json")

        # -- an env file gives up its public names and nothing else ----------------------------
        assert read_env(dashboard_env, False) == {
            "RPC_URL": rpc, "HACK_MINT": mint, "HACK_SYMBOL": "HACK", "HACK_DECIMALS": "2"}
        assert read_env(public_env, True) == {"ISSUER_PUBKEY": issuer, "HACK_CAP": "100.00", "HACK_MAX": "1000.00"}
        assert "issuer_from_keypair" not in globals(), "the keypair reader is back"

        # -- the committed kind of file is refused when it holds anything else -----------------
        for text, expect in (
                ("HACK_MINT=%s\nAUTHORITY_KEYPAIR=server/.keys/authority.json\n" % mint, "holds AUTHORITY_KEYPAIR"),
                ("DATABASE_URL=postgres://x\n", "holds DATABASE_URL"),
                ("ISSUER_PUBKEY=%s\n" % json.dumps(list(range(64))), "looks like a secret"),
                ("ISSUER_PUBKEY=%s\n" % b58enc(bytes(range(64))), "looks like a secret"),
                ("RPC_URL=https://user:pass@rpc.example\n", "looks like a secret")):
            message = refused("--public-env", write("bad.env", text), *pinned_flags)
            assert expect in message, message
            assert "pass@" not in message and "[0, 1" not in message  # a refusal does not echo the value
        assert "holds DATABASE_URL" in refused("--public-env", dashboard_env, *pinned_flags)
        assert "cannot read" in refused("--public-env", os.path.join(folder, "absent.env"), *pinned_flags)

        # -- precedence: a flag, then the public file, then --env; a disagreement stops the run --
        plan = provision_plan(parse("--public-env", public_env, "--env", dashboard_env))
        assert plan["tokens"] == pinned_tokens and plan["values"]["issuer"] == issuer
        assert plan["sources"]["issuer"].endswith("public.env ISSUER_PUBKEY")
        assert plan["sources"]["mint"].startswith("--env ") and plan["sources"]["mint"].endswith("HACK_MINT")
        plan = provision_plan(parse("--public-env", public_env, "--env", dashboard_env, "--cap", "5", "--issuer", other))
        assert plan["values"]["cap"] == "5" and plan["sources"]["cap"] == "--cap" and plan["values"]["issuer"] == other
        clash_env = write("clash.env", "HACK_MINT=%s\nHACK_DECIMALS=2\nHACK_SYMBOL=HACK\nRPC_URL=%s\n" % (mint, rpc))
        clash_public = write("clash.public.env", "ISSUER_PUBKEY=%s\nHACK_MINT=%s\nHACK_CAP=1\nHACK_MAX=2\n" % (issuer, other))
        message = refused("--public-env", clash_public, "--env", clash_env)
        assert "disagree on HACK_MINT (--mint)" in message and mint not in message and other not in message
        assert provision_plan(parse("--public-env", clash_public, "--env", clash_env, "--mint", mint))["values"]["mint"] == mint
        keyed = write("keyed.env", "RPC_URL=https://rpc.example/?api-key=not-a-real-key\n")
        message = refused("--public-env", public_env, "--env", keyed, "--mint", mint, "--decimals", "2", "--symbol", "HACK")
        assert "carries a login or a query string" in message and "not-a-real-key" not in message

        # -- the keypair file: never opened, never looked at, its path never known --------------
        # Every way Python opens or stats a path is replaced by one that refuses the trap file.
        hits = []
        patched = []

        def guard(module, name):
            real = getattr(module, name)

            def guarded(path, *rest, **more):
                text = os.fsdecode(path) if isinstance(path, (str, bytes, os.PathLike)) else ""
                if "TRAP" in text:
                    hits.append("%s(%s)" % (name, os.path.basename(text)))
                    raise AssertionError("%s() reached the keypair file" % name)
                return real(path, *rest, **more)

            patched.append((module, name, real))
            setattr(module, name, guarded)

        def no_badge(*_args, **_more):
            raise AssertionError("a serial port was about to be opened")

        real_badge = globals()["Badge"]
        outputs = []
        try:
            for module, name in ((builtins, "open"), (io, "open"), (os, "open"), (os, "stat"), (os, "lstat"),
                                 (os, "access"), (os, "readlink"), (os, "listdir"), (os, "scandir"),
                                 (os.path, "exists"), (os.path, "lexists"), (os.path, "isfile"),
                                 (os.path, "isdir"), (os.path, "getsize")):
                guard(module, name)

            # The guard works: each of these reaches the file without it.
            for attempt in (lambda: open(trap), lambda: os.stat(trap), lambda: os.path.exists(trap),
                            lambda: os.path.isfile(trap), lambda: os.path.getsize(trap), lambda: os.access(trap, os.R_OK)):
                try:
                    attempt()
                    raise BadgeError("the guard let an access through")
                except BadgeError:
                    raise
                except AssertionError:
                    pass
            assert len(hits) == 6, hits
            del hits[:]

            cwd = os.getcwd()
            os.chdir(folder)  # "relative.env" names the keypair by a relative path, as the dashboard's does
            try:
                for env_file in (dashboard_env, relative_env):
                    # --issuer with a dashboard .env: the request's case
                    plan = provision_plan(parse("--public-env", "none", "--env", env_file, "--issuer", issuer,
                                                "--cap", "100.00", "--max", "1000.00"))
                    assert plan["values"]["issuer"] == issuer and plan["tokens"] == pinned_tokens
                    # ISSUER_PUBKEY from the public file, with the same .env
                    plan = provision_plan(parse("--public-env", public_env, "--env", env_file))
                    assert plan["values"]["issuer"] == issuer and plan["tokens"] == pinned_tokens
                    # no issuer anywhere: an error, not a look at the keypair
                    message = refused("--public-env", "none", "--env", env_file, "--cap", "100.00", "--max", "1000.00")
                    assert "no value for ISSUER_PUBKEY (--issuer)" in message and "never reads a keypair file" in message
                    assert "TRAP" not in message
                    # the path is not in anything the tool resolved
                    assert "TRAP" not in json.dumps(plan)

                    # the whole command, twice: a dry run through main(), and a run against a badge
                    globals()["Badge"] = no_badge
                    out = io.StringIO()
                    with contextlib.redirect_stdout(out):
                        status = main(["provision", "--dry-run", "--public-env", public_env, "--env", env_file,
                                       "--listener", "http://192.168.4.20:8788", "--wifi", "Hot spot", "hunter2hunter2",
                                       "--autostart", "home", "--badge-id", "1", "--label", "Merchant"])
                    globals()["Badge"] = real_badge
                    assert status == 0
                    outputs.append(out.getvalue())
                    badge = FakeBadge()
                    outputs.append(run(badge, "--public-env", public_env, "--env", env_file, "--issuer", issuer,
                                       "--badges-json", badges_path, "--label", "Merchant"))
                    assert badge.store["issuer_key"] == issuer and badge.store["tokens"] == pinned_tokens
            finally:
                os.chdir(cwd)
                globals()["Badge"] = real_badge
            assert hits == [], "the tool touched the keypair file: %s" % hits
        finally:
            for module, name, real in patched:
                setattr(module, name, real)
        assert all("TRAP" not in text and "not-a-real-password" not in text for text in outputs)

        # -- the dry run: the lines of a real run, no password, the entry with placeholders -----
        dry = outputs[0].splitlines()
        args = parse("--public-env", public_env, "--env", dashboard_env, "--listener", "http://192.168.4.20:8788",
                     "--wifi", "Hot spot", "hunter2hunter2", "--autostart", "home")
        plan = provision_plan(args)
        shown = provision_lines(plan["steps"], show=True)
        start = dry.index("VKINFO")
        assert dry[start:start + len(shown)] == shown
        assert "hunter2hunter2" not in outputs[0] and "VKWIFI Hot spot|<password, 14 characters, not shown>" in dry
        assert dry[-3] == "" and dry[-2] == "badges.json entry (copy the next line):"
        assert json.loads(dry[-1]) == {"id": 1, "label": "Merchant", "pubkey": PUBKEY_PLACEHOLDER,
                                       "keyLocation": KEY_PLACEHOLDER, "tokenAccount": None, "standIn": False}

        # -- a run sends exactly the lines the dry run shows -----------------------------------
        badge = FakeBadge()
        text = run(badge, "--public-env", public_env, "--env", dashboard_env, "--listener", "http://192.168.4.20:8788",
                   "--wifi", "Hot spot", "hunter2hunter2", "--autostart", "home", "--badge-id", "7")
        assert badge.sent == provision_lines(plan["steps"])
        assert "VKWIFI Hot spot|hunter2hunter2" in badge.sent and "hunter2hunter2" not in text
        lines = text.splitlines()
        assert lines[-3] == "" and lines[-2] == "badges.json entry (copy the next line):"
        assert lines[-1] == ('{"id": 7, "label": "Badge 7", "pubkey": "%s", "keyLocation": "software", '
                             '"tokenAccount": null, "standIn": false}' % other)

        # -- a provisioned badge: refused without --force; with it, the secure keys wait for a hold --
        try:
            run(FakeBadge(provisioned=True), "--public-env", public_env, "--env", dashboard_env)
            raise AssertionError("a provisioned badge was changed without --force")
        except BadgeError as exc:
            assert "already provisioned" in str(exc)
        badge = FakeBadge(provisioned=True)
        text = run(badge, "--public-env", public_env, "--env", dashboard_env, "--force")
        assert badge.sent[:2] == ["VKINFO", "VKSET issuer_key " + issuer] and badge.sent[2] == "VKGET issuer_key"
        assert "issuer_key: hold SELECT" in text and "tokens: hold SELECT" in text
        assert text.splitlines()[-1].startswith('{"id": null, "label": null, "pubkey": "%s"' % other)
        assert 'put the badge\'s number (1 to 99) in "id"' in text
        try:
            run(FakeBadge(key="none"), "--public-env", public_env, "--env", dashboard_env)
            raise AssertionError("a badge with no key got a badges.json entry")
        except BadgeError as exc:
            assert "reports no public key" in str(exc)

        # -- the badges file ---------------------------------------------------------------------
        with open(badges_path, "r", encoding="utf-8") as handle:
            document = json.load(handle)
        assert document == {"badges": [{"id": 1, "label": "Merchant", "pubkey": other, "keyLocation": "software",
                                        "tokenAccount": None, "standIn": False}]}
        badges = [{"id": 1, "label": "Merchant (team)", "pubkey": "A", "keyLocation": "software",
                   "tokenAccount": "T", "standIn": True, "note": "kept"}]
        entry, what = badge_entry(badges, "A", "se050", None, None, True)       # the same key again
        assert what == "updated" and entry == {"id": 1, "label": "Merchant (team)", "pubkey": "A", "keyLocation": "se050",
                                               "tokenAccount": "T", "standIn": False, "note": "kept"}
        entry, what = badge_entry(badges, "B", "software", None, "Judge A", True)  # a new badge: lowest free id
        assert what == "added" and entry["id"] == 2 and entry["label"] == "Judge A" and len(badges) == 2
        entry, what = badge_entry(badges, "C", "none", 1, None, True)           # a new key under an old id
        assert what == "replaced (was A)" and entry["pubkey"] == "C" and entry["tokenAccount"] is None
        assert entry["keyLocation"] == "unknown" and entry["label"] == "Merchant (team)" and len(badges) == 2
        entry, what = badge_entry(badges, "D", "software", 9, None, True)
        entry, what = badge_entry(badges, "E", "software", 4, None, True)
        assert [badge["id"] for badge in badges] == [1, 2, 4, 9] and entry["label"] == "Badge 4"
        try:
            badge_entry(badges, "B", "software", 1, None, True)                 # id 1 is C's
            raise AssertionError("two badges got one id")
        except BadgeError:
            pass
        broken = write("broken.json", '["not", "a badges file"]')
        badge = FakeBadge()
        try:
            run(badge, "--public-env", public_env, "--env", dashboard_env, "--badges-json", broken)
            raise AssertionError("a file that is not a badges file was accepted")
        except BadgeError as exc:
            assert "left as it is" in str(exc)
        assert badge.sent == ["VKINFO"], "the badge was changed before the refusal"
        with open(broken, "r", encoding="utf-8") as handle:
            assert handle.read() == '["not", "a badges file"]'


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

    _selftest_provision()
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
    parser.add_argument("--selftest", action="store_true",
                        help="check this file's codecs and the provisioning values; needs no badge")
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
    sub.add_parser("tidy", help="delete every installed app that is not in os/apps/")
    sub.add_parser("menu", help="print the launcher's top level (VKSTATE menu)")
    p = sub.add_parser("monitor", help="print the badge's log until interrupted")
    p.add_argument("--seconds", type=float, help="stop after this long")
    p = sub.add_parser("test", help="run scripted device tests")
    p.add_argument("files", nargs="+")
    p.add_argument("--include-deferred", action="store_true", help="also run tests that declare NEEDS")
    p = sub.add_parser(
        "provision", help="first-time setup of a badge (config.md)",
        description="Sets a badge's issuer key, token table, RPC URL and, per venue, listener URL and "
                    "Wi-Fi. Each pinned value comes from its flag, else from os/provision.public.env, "
                    "else from the file named with --env. No keypair file is ever read.")
    p.add_argument("--issuer", metavar="BASE58", help="issuer public key (env name ISSUER_PUBKEY)")
    p.add_argument("--mint", metavar="BASE58", help="token mint address (HACK_MINT)")
    p.add_argument("--decimals", metavar="0-9", help="the mint's decimals (HACK_DECIMALS)")
    p.add_argument("--symbol", help="token symbol, 1 to 4 of A-Z 0-9 (HACK_SYMBOL)")
    p.add_argument("--cap", metavar="AMOUNT", help="above this SELECT becomes a hold, e.g. 100.00; 0 = no cap (HACK_CAP)")
    p.add_argument("--max", metavar="AMOUNT", help="above this a payment is blocked, e.g. 1000.00; 0 = no max (HACK_MAX)")
    p.add_argument("--rpc", metavar="URL", help="JSON-RPC endpoint (RPC_URL)")
    p.add_argument("--public-env", metavar="PATH",
                   help="file of public values to use in place of os/provision.public.env; "
                        "'none' reads no such file")
    p.add_argument("--env", metavar="PATH",
                   help="one more env file for values the public file lacks, e.g. the dashboard's .env on "
                        "the backend laptop. Never read unless named here; only the public names are taken")
    p.add_argument("--listener", metavar="URL", help="backend listener, per venue, e.g. http://192.168.4.20:8788")
    p.add_argument("--wifi", nargs=2, metavar=("SSID", "PASSWORD"), help="network to join, per venue")
    p.add_argument("--autostart", metavar="APP_ID", help="app to start at boot")
    p.add_argument("--force", action="store_true", help="change a badge that is already provisioned")
    p.add_argument("--dry-run", action="store_true",
                   help="validate everything and print the lines a run would send; opens no serial port")
    p.add_argument("--badge-id", type=int, metavar="N", help="this badge's id in badges.json, 1 to 99")
    p.add_argument("--label", help='this badge\'s label in badges.json, e.g. "Merchant"')
    p.add_argument("--badges-json", metavar="FILE",
                   help="also add or update this badge's entry in FILE (the shape of "
                        "dashboard/server/config/badges.json; created if missing)")
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
    plan = None
    if args.command == "provision":
        # Everything is resolved and checked before a port is opened. This reads env files only.
        try:
            plan = provision_plan(args)
            if args.dry_run:
                provision_dry_run(args, plan)
                return 0
        except BadgeError as exc:
            print("error: %s" % exc, file=sys.stderr)
            return 1
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
        elif args.command == "tidy":
            print("removed: %s" % (" ".join(badge.tidy()) or "nothing"))
        elif args.command == "menu":
            print(" ".join(badge.state().get("menu", [])))
        elif args.command == "monitor":
            end = None if args.seconds is None else time.monotonic() + args.seconds
            while end is None or time.monotonic() < end:
                line = badge._next(time.monotonic() + 0.5)
                if line is not None:
                    print(line)
                    sys.stdout.flush()
        elif args.command == "provision":
            provision(badge, args, plan)
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
