#!/usr/bin/env python3
"""
Push apps to a Solana Badge over Wi-Fi (HTTP) or BLE.

Wi-Fi needs nothing beyond the standard library. BLE needs `bleak`
(pip install bleak) and is only imported when a BLE transport is asked for, so
the HTTP path keeps working on a machine without it.

Examples
--------
  # push a directory of Lua files and launch it
  badge-push.py --host solana-badge.local --token 123456 push apps/hello --run

  # single file
  badge-push.py --host 192.168.4.1 --token 123456 push apps/hello/main.lua --id hello

  # over BLE, by advertised name
  badge-push.py --ble badge-4F2A --token 123456 push apps/hello --run

  # housekeeping
  badge-push.py --host solana-badge.local --token 123456 list
  badge-push.py --host solana-badge.local --token 123456 rm hello
  badge-push.py --host solana-badge.local --token 123456 logs

  # WPA2-Enterprise, e.g. DEF CON. Upload this year's CA first, then join.
  badge-push.py --host 192.168.4.1 --token 123456 cert defcon34-wifi.pem
  badge-push.py --host 192.168.4.1 --token 123456 defcon --user MYUSER --password MYPASS \
      --ca defcon34-wifi.pem

  # any other enterprise network
  badge-push.py --host 192.168.4.1 --token 123456 join eduroam --enterprise \
      --user me@uni.edu --password hunter2 --ca uni.pem --domain radius.uni.edu

The token is the six-digit pairing code from Settings -> Push on the badge. It
can also come from the BADGE_TOKEN environment variable.
"""

from __future__ import annotations

import argparse
import base64
import json
import os
import sys
import urllib.error
import urllib.parse
import urllib.request
from pathlib import Path

# Files worth sending. Anything else in an app directory is skipped, which keeps
# editor backups and .DS_Store off the badge.
PUSHABLE = {".lua", ".ini", ".txt", ".json", ".csv", ".png", ".jpg", ".jpeg"}
BINARY = {".png", ".jpg", ".jpeg"}


class BadgeError(RuntimeError):
    pass


# ---------------------------------------------------------------------------
# HTTP transport
# ---------------------------------------------------------------------------
class HttpBadge:
    def __init__(self, host: str, token: str = "", timeout: float = 10.0):
        if not host.startswith("http"):
            host = "http://" + host
        self.base = host.rstrip("/")
        self.token = token
        self.timeout = timeout

    def _request(self, method: str, path: str, body=None, content_type="text/plain"):
        url = self.base + path
        headers = {"X-Badge-Token": self.token}
        if body is not None:
            headers["Content-Type"] = content_type
        request = urllib.request.Request(url, data=body, headers=headers, method=method)
        try:
            with urllib.request.urlopen(request, timeout=self.timeout) as response:
                payload = response.read().decode("utf-8", "replace")
        except urllib.error.HTTPError as exc:
            detail = exc.read().decode("utf-8", "replace")
            raise BadgeError(f"{method} {path} -> {exc.code}: {detail}") from None
        except urllib.error.URLError as exc:
            raise BadgeError(f"cannot reach {url}: {exc.reason}") from None
        return json.loads(payload) if payload.strip().startswith(("{", "[")) else payload

    def status(self):
        return self._request("GET", "/api/status")

    def apps(self):
        return self._request("GET", "/api/apps")["apps"]

    def write(self, app_id: str, rel_path: str, data: bytes, binary: bool):
        query = urllib.parse.urlencode(
            {"id": app_id, "path": rel_path, **({"enc": "base64"} if binary else {})}
        )
        payload = base64.b64encode(data) if binary else data
        return self._request("POST", f"/api/app?{query}", payload,
                             "application/octet-stream" if binary else "text/plain")

    def run(self, app_id: str):
        return self._request("POST", "/api/run?" + urllib.parse.urlencode({"id": app_id}))

    def stop(self):
        return self._request("POST", "/api/stop")

    def remove(self, app_id: str):
        return self._request("DELETE", "/api/app?" + urllib.parse.urlencode({"id": app_id}))

    def logs(self):
        return self._request("GET", "/api/logs")["lines"]

    def certs(self):
        return self._request("GET", "/api/certs")["certs"]

    def put_cert(self, name: str, pem: bytes):
        return self._request("POST", "/api/certs?" + urllib.parse.urlencode({"name": name}),
                             pem, "text/plain")

    def del_cert(self, name: str):
        return self._request("DELETE", "/api/certs?" + urllib.parse.urlencode({"name": name}))

    def join(self, profile: dict):
        # The badge answers before it associates, then drops this connection as
        # it leaves the current network - so a read timeout here is the normal
        # outcome, not a failure.
        try:
            return self._request("POST", "/api/wifi", json.dumps(profile).encode(),
                                 "application/json")
        except BadgeError as exc:
            if "timed out" in str(exc).lower():
                return {"ok": True, "note": "connection dropped while joining (expected)"}
            raise

    def close(self):
        pass


# ---------------------------------------------------------------------------
# BLE transport - the line protocol from src/net/push_protocol.h
# ---------------------------------------------------------------------------
NUS_RX = "6e400002-b5a3-f393-e0a9-e50e24dcca9e"
NUS_TX = "6e400003-b5a3-f393-e0a9-e50e24dcca9e"

# One base64 chunk per DATA line. 180 bytes of source encodes to 240 characters,
# which stays clear of the badge's 4 KB line buffer and keeps each notification
# round trip short.
BLE_CHUNK = 180


class BleBadge:
    def __init__(self, name: str, token: str = "", timeout: float = 20.0):
        try:
            import asyncio

            from bleak import BleakClient, BleakScanner
        except ImportError:
            raise BadgeError("BLE needs bleak - pip install bleak") from None

        self._asyncio = asyncio
        self._BleakClient = BleakClient
        self._BleakScanner = BleakScanner
        self.name = name
        self.token = token
        self.timeout = timeout
        self.loop = asyncio.new_event_loop()
        self.client = None
        self._buffer = ""
        self._lines: list[str] = []

        self.loop.run_until_complete(self._connect())
        if token:
            self.command(f"AUTH {token}")

    async def _connect(self):
        device = await self._BleakScanner.find_device_by_name(self.name, timeout=self.timeout)
        if device is None:
            raise BadgeError(f"no BLE device named '{self.name}' found")
        self.client = self._BleakClient(device)
        await self.client.connect()
        await self.client.start_notify(NUS_TX, self._on_notify)

    def _on_notify(self, _sender, data: bytearray):
        self._buffer += data.decode("utf-8", "replace")
        while "\n" in self._buffer:
            line, self._buffer = self._buffer.split("\n", 1)
            self._lines.append(line.strip())

    async def _exchange(self, line: str, expect_multi: bool):
        self._lines.clear()
        await self.client.write_gatt_char(NUS_RX, (line + "\n").encode(), response=False)

        # The badge answers every command with exactly one OK/ERR line, possibly
        # preceded by '+' data lines - so waiting for OK/ERR is a complete
        # framing rule and no timeout guessing is needed.
        deadline = self.loop.time() + self.timeout
        collected: list[str] = []
        while self.loop.time() < deadline:
            while self._lines:
                reply = self._lines.pop(0)
                if reply.startswith("+"):
                    collected.append(reply[1:].strip())
                    continue
                if reply.startswith("ERR"):
                    raise BadgeError(f"{line.split()[0]}: {reply[4:].strip()}")
                if reply.startswith("OK"):
                    return (collected, reply[3:].strip()) if expect_multi else reply[3:].strip()
            await self._asyncio.sleep(0.02)
        raise BadgeError(f"timed out waiting for a reply to '{line.split()[0]}'")

    def command(self, line: str, expect_multi: bool = False):
        return self.loop.run_until_complete(self._exchange(line, expect_multi))

    def status(self):
        out = {"raw": self.command("INFO")}
        try:
            out["wifi"] = self.command("WIFI")
        except BadgeError:
            pass
        return out

    def apps(self):
        rows, _ = self.command("LIST", expect_multi=True)
        out = []
        for row in rows:
            parts = row.split(" ", 2)
            if len(parts) >= 2:
                out.append({"id": parts[0], "bytes": int(parts[1]),
                            "name": parts[2] if len(parts) > 2 else parts[0],
                            "version": ""})
        return out

    def write(self, app_id: str, rel_path: str, data: bytes, binary: bool):
        self.command(f"BEGIN {app_id} {rel_path}")
        for offset in range(0, len(data), BLE_CHUNK):
            chunk = base64.b64encode(data[offset:offset + BLE_CHUNK]).decode()
            self.command(f"DATA {chunk}")
        return self.command("END")

    def run(self, app_id: str):
        return self.command(f"RUN {app_id}")

    def stop(self):
        return self.command("STOP")

    def remove(self, app_id: str):
        return self.command(f"DEL {app_id}")

    def logs(self):
        rows, _ = self.command("LOGS", expect_multi=True)
        return rows

    def certs(self):
        rows, _ = self.command("CERTS", expect_multi=True)
        out = []
        for row in rows:
            parts = row.split(" ", 1)
            if len(parts) == 2:
                out.append({"name": parts[0], "bytes": int(parts[1])})
        return out

    def put_cert(self, name: str, pem: bytes):
        self.command(f"CERTBEGIN {name}")
        for offset in range(0, len(pem), BLE_CHUNK):
            chunk = base64.b64encode(pem[offset:offset + BLE_CHUNK]).decode()
            self.command(f"DATA {chunk}")
        return self.command("END")

    def del_cert(self, name: str):
        return self.command(f"CERTDEL {name}")

    def join(self, profile: dict):
        # One field per line: no single line of a 20-byte-MTU protocol can carry
        # a whole enterprise profile.
        mapping = {"ssid": "ssid", "password": "eappass", "identity": "identity",
                   "username": "user", "ca": "ca", "domain": "domain", "phase2": "phase2",
                   "eap_method": "method"}
        enterprise = profile.get("security") in ("eap", "enterprise")
        if not enterprise:
            mapping["password"] = "pass"

        self.command(f"SETWIFI security {'eap' if enterprise else 'psk'}")
        for key, field in mapping.items():
            value = profile.get(key)
            if value:
                self.command(f"SETWIFI {field} {value}")
        return self.command("JOINWIFI")

    def close(self):
        if self.client:
            self.loop.run_until_complete(self.client.disconnect())
        self.loop.close()


# ---------------------------------------------------------------------------
# Commands
# ---------------------------------------------------------------------------
def collect_files(source: Path) -> list[tuple[str, Path]]:
    """Returns (relative-path-on-badge, local-path) pairs."""
    if source.is_file():
        return [(source.name, source)]

    files = []
    for path in sorted(source.rglob("*")):
        if not path.is_file():
            continue
        if path.suffix.lower() not in PUSHABLE:
            continue
        if path.name.startswith("."):
            continue
        files.append((path.relative_to(source).as_posix(), path))
    return files


def command_push(badge, args) -> int:
    source = Path(args.source)
    if not source.exists():
        raise BadgeError(f"{source} does not exist")

    app_id = args.id or (source.stem if source.is_file() else source.name)
    app_id = app_id.lower()

    files = collect_files(source)
    if not files:
        raise BadgeError(f"nothing pushable in {source}")

    total = 0
    for rel_path, path in files:
        data = path.read_bytes()
        binary = path.suffix.lower() in BINARY
        badge.write(app_id, rel_path, data, binary)
        total += len(data)
        print(f"  {rel_path:<28} {len(data):>7} B")

    print(f"pushed {len(files)} file(s), {total} B to '{app_id}'")

    if args.run:
        badge.run(app_id)
        print(f"launched '{app_id}'")
    return 0


def command_list(badge, args) -> int:
    apps = badge.apps()
    if not apps:
        print("no apps installed")
        return 0
    for app in apps:
        version = f" v{app['version']}" if app.get("version") else ""
        print(f"  {app['id']:<20} {app['bytes']:>7} B  {app.get('name', '')}{version}")
    return 0


def command_run(badge, args) -> int:
    badge.run(args.id)
    print(f"launched '{args.id}'")
    return 0


def command_stop(badge, args) -> int:
    badge.stop()
    print("stopped")
    return 0


def command_rm(badge, args) -> int:
    badge.remove(args.id)
    print(f"deleted '{args.id}'")
    return 0


def command_logs(badge, args) -> int:
    for line in badge.logs():
        print(line)
    return 0


def command_status(badge, args) -> int:
    print(json.dumps(badge.status(), indent=2))
    return 0


def command_cert(badge, args) -> int:
    path = Path(args.file)
    if not path.is_file():
        raise BadgeError(f"{path} does not exist")

    pem = path.read_bytes()
    if b"-----BEGIN CERTIFICATE-----" not in pem:
        raise BadgeError(
            f"{path} is not PEM. Convert it first:\n"
            f"  openssl x509 -inform der -in {path} -out {path.with_suffix('.pem')}"
        )

    name = (args.name or path.name).lower()
    badge.put_cert(name, pem)
    print(f"stored '{name}' ({len(pem)} B)")
    return 0


def command_certs(badge, args) -> int:
    certs = badge.certs()
    if not certs:
        print("no certificates installed")
        return 0
    for cert in certs:
        print(f"  {cert['name']:<32} {cert['bytes']:>6} B")
    return 0


def command_rmcert(badge, args) -> int:
    badge.del_cert(args.name)
    print(f"deleted '{args.name}'")
    return 0


def build_profile(args, ssid: str) -> dict:
    if not args.enterprise:
        return {"ssid": ssid, "password": args.password or ""}

    if not args.user:
        raise BadgeError("--user is required for an enterprise network")
    if not args.ca:
        # Not fatal, but it means the badge cannot tell the real network from
        # anyone impersonating it - and MSCHAPv2 hands over a crackable hash.
        print("warning: no --ca given, the RADIUS server will NOT be validated",
              file=sys.stderr)
    return {
        "ssid": ssid,
        "security": "eap",
        "eap_method": args.method,
        "phase2": args.phase2,
        "username": args.user,
        "password": args.password or "",
        "identity": args.identity or "",
        "domain": args.domain or "",
        "ca": args.ca or "",
    }


def command_join(badge, args) -> int:
    badge.join(build_profile(args, args.ssid))
    print(f"joining '{args.ssid}'" + (" (enterprise)" if args.enterprise else ""))
    print("the badge drops this connection while it associates; check Settings -> Wi-Fi")
    return 0


def command_defcon(badge, args) -> int:
    """The DEF CON profile, prefilled: SSID DefCon, PEAP/MSCHAPv2, wifireg domain."""
    args.enterprise = True
    args.method = "peap"
    args.phase2 = "mschapv2"
    args.identity = args.identity or ""
    args.domain = args.domain or "wifireg.defcon.org"

    if not args.ca:
        certs = {c["name"] for c in badge.certs()}
        # Pick an obvious DEF CON certificate if exactly one is installed, so the
        # common case needs no --ca at all.
        guesses = sorted(n for n in certs if "defcon" in n or "dc3" in n)
        if len(guesses) == 1:
            args.ca = guesses[0]
            print(f"using installed certificate '{args.ca}'")

    badge.join(build_profile(args, "DefCon"))
    print("joining 'DefCon' (PEAP/MSCHAPv2)")
    print("the badge drops this connection while it associates; check Settings -> Wi-Fi")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Push apps to a Solana Badge.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__.split("Examples")[1] if "Examples" in __doc__ else None,
    )
    transport = parser.add_mutually_exclusive_group()
    transport.add_argument("--host", help="badge address, e.g. solana-badge.local or 192.168.4.1")
    transport.add_argument("--ble", metavar="NAME", help="badge BLE name, e.g. badge-4F2A")
    parser.add_argument("--token", default=os.environ.get("BADGE_TOKEN", ""),
                        help="six-digit pairing code (or set BADGE_TOKEN)")
    parser.add_argument("--timeout", type=float, default=10.0)

    sub = parser.add_subparsers(dest="command", required=True)

    push = sub.add_parser("push", help="upload a file or an app directory")
    push.add_argument("source", help="app directory or single .lua file")
    push.add_argument("--id", help="app id (defaults to the directory or file name)")
    push.add_argument("--run", action="store_true", help="launch it once uploaded")
    push.set_defaults(handler=command_push)

    sub.add_parser("list", help="list installed apps").set_defaults(handler=command_list)
    sub.add_parser("stop", help="return to the launcher").set_defaults(handler=command_stop)
    sub.add_parser("logs", help="recent log lines").set_defaults(handler=command_logs)
    sub.add_parser("status", help="device status").set_defaults(handler=command_status)

    run = sub.add_parser("run", help="launch an installed app")
    run.add_argument("id")
    run.set_defaults(handler=command_run)

    remove = sub.add_parser("rm", help="uninstall an app")
    remove.add_argument("id")
    remove.set_defaults(handler=command_rm)

    # -- certificates --------------------------------------------------------
    cert = sub.add_parser("cert", help="upload a CA certificate (PEM)")
    cert.add_argument("file")
    cert.add_argument("--name", help="name on the badge (defaults to the file name)")
    cert.set_defaults(handler=command_cert)

    sub.add_parser("certs", help="list installed certificates").set_defaults(
        handler=command_certs)

    rmcert = sub.add_parser("rmcert", help="delete a certificate")
    rmcert.add_argument("name")
    rmcert.set_defaults(handler=command_rmcert)

    # -- wi-fi ---------------------------------------------------------------
    def add_wifi_args(parser_):
        parser_.add_argument("--enterprise", action="store_true",
                             help="WPA2-Enterprise instead of a pre-shared key")
        parser_.add_argument("--user", help="enterprise username (inner identity)")
        parser_.add_argument("--password", help="PSK, or the enterprise password")
        parser_.add_argument("--identity", help="outer/anonymous identity; defaults to --user")
        parser_.add_argument("--ca", help="name of a certificate already on the badge")
        parser_.add_argument("--domain", help="expected server domain in the certificate")
        parser_.add_argument("--method", default="peap", choices=["peap", "ttls"])
        parser_.add_argument("--phase2", default="mschapv2",
                             choices=["mschapv2", "pap", "chap", "mschap"],
                             help="TTLS inner method; ignored for PEAP")

    join = sub.add_parser("join", help="join a network")
    join.add_argument("ssid")
    add_wifi_args(join)
    join.set_defaults(handler=command_join)

    defcon = sub.add_parser(
        "defcon", help="join the DEF CON network (SSID DefCon, PEAP/MSCHAPv2)")
    add_wifi_args(defcon)
    defcon.set_defaults(handler=command_defcon)

    args = parser.parse_args()

    if not args.host and not args.ble:
        parser.error("one of --host or --ble is required")

    badge = None
    try:
        badge = (BleBadge(args.ble, args.token, max(args.timeout, 20.0)) if args.ble
                 else HttpBadge(args.host, args.token, args.timeout))
        return args.handler(badge, args)
    except BadgeError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1
    except KeyboardInterrupt:
        return 130
    finally:
        if badge is not None:
            badge.close()


if __name__ == "__main__":
    sys.exit(main())
