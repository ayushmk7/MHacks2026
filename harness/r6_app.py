#!/usr/bin/env python3
"""R6 fuzz set: push the badge half. Stdlib only.

  python harness/r6_app.py push --host <badge-ip> --token <code> [--badge 1 | --pubkey <base58>] [--listener URL]
  python harness/r6_app.py push --out <dir> [--badge 1 | --pubkey <base58>] [--listener URL]   # stage only

Writes this laptop's R6 listener address (default http://<this laptop>:8790, the port
harness/r6_fuzz.mjs listens on) and the badge's pubkey (from dashboard/server/config/badges.json)
into harness/apps/r6-fuzz/config.lua, in a temporary copy, adds lib/vk.lua, then installs and
launches it (harness/badge_app.py).

Start `node --env-file-if-exists=dashboard/.env harness/r6_fuzz.mjs --badge <id> --mint <pinned HACK mint>`
first, then press SELECT on the badge.
"""

import argparse
import sys

from badge_app import add_push_args, badge_pubkey, check_push_args, laptop_ip, push as push_app

APP = "r6-fuzz"


def push(args):
    listener = args.listener or f"http://{laptop_ip()}:{args.port}"
    pubkey = args.pubkey or badge_pubkey(args.badge)
    sys.exit(push_app(APP, {"listener": listener, "badge": pubkey}, args))


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="command", required=True)
    p = sub.add_parser("push", help="install and launch the R6 fuzz app on a badge over Wi-Fi")
    add_push_args(p, "R6 listener base URL (default http://<this laptop>:8790)", 8790)
    who = p.add_mutually_exclusive_group()
    who.add_argument("--badge", default="1", help="badge id in badges.json (default 1, as r6_fuzz.mjs)")
    who.add_argument("--pubkey", help="badge's base58 pubkey, instead of --badge")
    args = parser.parse_args()
    check_push_args(parser, args)
    push(args)


if __name__ == "__main__":
    main()
