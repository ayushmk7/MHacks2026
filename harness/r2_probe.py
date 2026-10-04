#!/usr/bin/env python3
"""R2 network probe: laptop side. Stdlib only.

  python harness/r2_probe.py serve                      # stand-in GET /health on :8788
  python harness/r2_probe.py push --host <badge-ip> --token <code> [--listener URL]
  python harness/r2_probe.py push --out <dir> [--listener URL]        # stage only, push nothing
  python harness/r2_probe.py push --ble <badge-name> --token <code> --listener URL

`push` writes this laptop's badge-listener address into the app's config.lua (in a temporary
copy), adds lib/vk.lua, and installs it with the Solana OS push tool, then launches it
(harness/badge_app.py). `push --out DIR` stages the folder and pushes nothing. On the badge press
SELECT to run the checks; results show on screen and in the log as "[app] R2 ..." lines.

`serve` answers GET /health with {"ok":true} on the badge-listener port, for when the
dashboard server (which needs Postgres) is not running. With the server up, its own
listener answers /health instead (BADGE_LISTEN_HOST set in dashboard/.env).
"""

import argparse
import json
import sys
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

from badge_app import add_push_args, check_push_args, laptop_ip, push as push_app

APP = "r2-probe"


def serve(port):
    class Health(BaseHTTPRequestHandler):
        def do_GET(self):
            if self.path.split("?")[0] != "/health":
                self.send_response(404)
                self.end_headers()
                return
            body = json.dumps({"ok": True}).encode()
            self.send_response(200)
            self.send_header("content-type", "application/json")
            self.send_header("content-length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

        def log_message(self, fmt, *args):
            print(f"[health] {self.client_address[0]} {fmt % args}")

    print(f"Serving GET /health on http://{laptop_ip()}:{port}/health (Ctrl+C to stop)")
    try:
        ThreadingHTTPServer(("0.0.0.0", port), Health).serve_forever()
    except KeyboardInterrupt:
        pass


def push(args):
    listener = args.listener or f"http://{laptop_ip()}:{args.port}"
    sys.exit(push_app(APP, {"listener": listener}, args))


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="command", required=True)
    s = sub.add_parser("serve", help="stand-in GET /health")
    s.add_argument("--port", type=int, default=8788)
    p = sub.add_parser("push", help="install and launch the probe app on a badge over Wi-Fi")
    add_push_args(p, "badge listener base URL (default http://<this laptop>:8788)", 8788)
    args = parser.parse_args()
    if args.command == "push":
        check_push_args(parser, args)
    if args.command == "serve":
        serve(args.port)
    else:
        push(args)


if __name__ == "__main__":
    main()
