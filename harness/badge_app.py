"""Shared by the harness push helpers (r2_probe.py, r3_app.py, r4_timing.py, r6_app.py). Stdlib only.

An app folder under harness/apps/ is laid out like a BadgeOS app (os/apps/<id>/ on the badge-os
branch): app.ini, main.lua, config.lua. Pushing one makes a temporary copy, writes this laptop's
values into config.lua, adds lib/vk.lua as vk.lua (upstream's push writes only under /apps/<id>/,
so the shared library travels with each app, as os/scripts/push-apps.sh does), and installs it with
the Solana OS push tool. `--out DIR` stages the copy in DIR and pushes nothing.
"""

import json
import re
import shutil
import socket
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parent
APPS = HERE / "apps"
BADGES = REPO / "dashboard" / "server" / "config" / "badges.json"
VK_BRANCH = "origin/badge-os"


def laptop_ip():
    """The address other devices on this network reach us at (no packet is sent)."""
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
        try:
            s.connect(("10.255.255.255", 1))
            return s.getsockname()[0]
        except OSError:
            return "127.0.0.1"


def find_push_tool():
    found = sorted(REPO.glob("*firmware*/*/tools/badge-push.py")) + sorted(REPO.glob("os/tools/badge-push.py"))
    if not found:
        sys.exit("badge-push.py not found under *firmware*/*/tools/ or os/tools/ - pass --push-tool")
    return found[0]


def vk_source():
    """lib/vk.lua: os/lib/vk.lua in this checkout, else read from the badge-os branch with git."""
    local = REPO / "os" / "lib" / "vk.lua"
    if local.exists():
        return local.read_text()
    try:
        return subprocess.run(["git", "-C", str(REPO), "show", f"{VK_BRANCH}:os/lib/vk.lua"],
                              check=True, capture_output=True, text=True).stdout
    except (OSError, subprocess.CalledProcessError):
        sys.exit(f"lib/vk.lua not found: no os/lib/vk.lua here and `git show {VK_BRANCH}:os/lib/vk.lua` failed "
                 "(run `git fetch`)")


def badge_pubkey(badge_id):
    for b in json.loads(BADGES.read_text())["badges"]:
        if str(b["id"]) == str(badge_id) and b.get("pubkey"):
            return b["pubkey"]
    sys.exit(f"badge {badge_id} has no pubkey in {BADGES}")


def set_config(text, key, value):
    """Replaces the quoted value of `key = "..."` in config.lua, keeping the rest of the line."""
    pattern = re.compile(rf'^(\s*{re.escape(key)}\s*=\s*)"[^"]*"', re.M)
    if len(pattern.findall(text)) != 1:
        sys.exit(f'config.lua: expected exactly one `{key} = "..."` line')
    return pattern.sub(lambda m: f'{m.group(1)}"{value}"', text, count=1)


def stage(app_id, values, target):
    """Copies apps/<app_id> to target with `values` written into config.lua and vk.lua added."""
    shutil.copytree(APPS / app_id, target)
    config = target / "config.lua"
    text = config.read_text()
    for key, value in values.items():
        text = set_config(text, key, value)
    config.write_text(text)
    (target / "vk.lua").write_text(vk_source())
    return target


def push(app_id, values, args):
    """Stages the app and pushes it (or only stages it, with --out)."""
    shown = " ".join(f"{k}={v}" for k, v in values.items())
    if args.out:
        out = Path(args.out) / app_id
        if out.exists():
            shutil.rmtree(out)
        stage(app_id, values, out)
        print(f"Staged {app_id} in {out} with {shown} (not pushed)")
        return 0
    tool = Path(args.push_tool) if args.push_tool else find_push_tool()
    with tempfile.TemporaryDirectory() as tmp:
        target = stage(app_id, values, Path(tmp) / app_id)   # folder name = app id
        print(f"Pushing {app_id} with {shown}")
        where = ["--ble", args.ble] if args.ble else ["--host", args.host]
        return subprocess.call([sys.executable, str(tool), *where, "--token", args.token, "push", str(target), "--run"])


def add_push_args(p, listener_help, port):
    """The options every helper's `push` command takes."""
    where = p.add_mutually_exclusive_group(required=True)
    where.add_argument("--host", help="badge address shown on Settings > App push")
    where.add_argument("--ble", help="badge BLE name (e.g. badge-51A0), for networks that block laptop->badge;"
                                     " needs bleak, and usually --listener with a public URL")
    where.add_argument("--out", help="stage the app folder here (with vk.lua and this laptop's values) and push nothing")
    p.add_argument("--token", help="six-digit pairing code shown on Settings > App push")
    p.add_argument("--listener", help=listener_help)
    p.add_argument("--port", type=int, default=port)
    p.add_argument("--push-tool", help="path to badge-push.py")


def check_push_args(parser, args):
    if not args.out and not args.token:
        parser.error("--token is required to push (or use --out to stage only)")
