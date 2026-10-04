#!/usr/bin/env python3
"""Pushes prepared app folders to one badge over USB serial, in one session
(docs/os/guides/build-flash-provision.md, "Installing apps"). scripts/push-apps.sh runs it.

    push_serial.py --port /dev/cu.usbserial-10 [--code 123456] <id>=<folder> [<id>=<folder> ...]

Why this exists instead of one `vkdev.py push` per app. A push used to fail now and then in the
middle of a file with "ERR not authorised - send AUTH <code>" or "ERR unknown command":

  1. The push session is one flag on the badge (upstream's push_protocol), shared by USB, BLE and
     Wi-Fi, and upstream clears it whenever an app stops or a launch fails (os.ino), and when a
     BLE central disconnects. vkdev.py authenticates once per file, so an app that stops while a
     file is on its way (someone presses CANCEL on the badge, a test stops an app) fails the rest
     of that file with "ERR not authorised".
  2. Nothing stopped a second program from opening the same serial port: macOS lets any number of
     processes open a tty. Two programs then read each other's replies (garbled "O 180" for
     "OK 180") and the other one's RUN and STOP clear the session as in 1.

What this does about it:
  - the port is opened exclusively (TIOCEXCL): while a push runs, any other program's open()
    fails with "Resource busy" instead of corrupting both; and the push refuses to start while
    another program already has the port open (lsof);
  - the running app is stopped once before the first file, so no app is left to stop mid-push;
  - an "ERR not authorised" reply (that exact reply only) re-authenticates and sends that one file
    again from its BEGIN, at most twice per file, and says so. Any other error fails the app.

Exit status: 0 if every app was pushed, 1 if any failed, 2 for a usage error.
"""

import argparse
import errno
import os
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from vkdev import BAUD, Badge, BadgeError  # noqa: E402

NOT_AUTHORISED = "ERR not authorised"
FILE_TRIES = 3            # the first attempt and two re-authenticated ones


def other_users(port):
    """Pids of other processes that have `port` open, from lsof. [] when lsof is missing."""
    try:
        out = subprocess.run(["lsof", "-t", port], capture_output=True, text=True, timeout=10).stdout
    except (OSError, subprocess.SubprocessError):
        return []
    return [pid for pid in out.split() if pid.isdigit() and int(pid) != os.getpid()]


def open_exclusive(port):
    """The port, opened as vkdev.py opens it (the reset line untouched), then made exclusive."""
    import fcntl
    import serial
    import termios

    ser = serial.Serial()
    ser.port = port
    ser.baudrate = BAUD
    ser.timeout = 0.05
    ser.write_timeout = 5
    ser.dtr = True            # see vkdev.Badge.__init__: RTS is dropped by open(), DTR after it
    ser.rts = False
    ser.open()
    try:
        ser.dtr = False
    except OSError as exc:
        if exc.errno not in (errno.EINVAL, errno.ENOTTY):
            raise
    fcntl.ioctl(ser.fileno(), termios.TIOCEXCL)
    return ser


class PushBadge(Badge):
    """vkdev's Badge, with a file transfer that survives a dropped session."""

    def _push_file(self, app_id, relative, data):
        for attempt in range(1, FILE_TRIES + 1):
            try:
                return Badge._push_file(self, app_id, relative, data)
            except BadgeError as exc:
                if NOT_AUTHORISED not in str(exc) or attempt == FILE_TRIES:
                    raise
                # The badge cleared the session: authenticate and send the whole file again; BEGIN
                # truncates what the first attempt wrote. What the badge logged meanwhile says why.
                said = [line for line in self.log() if not line.startswith("[push] wrote")][-4:]
                print("  %s: the badge dropped the push session; authenticating again%s" % (
                    relative, "".join("\n    | " + line for line in said)))
                self.clear_log()


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--port", required=True)
    parser.add_argument("--code", help="pairing code (a dev build gives it itself)")
    parser.add_argument("apps", nargs="+", metavar="ID=FOLDER")
    args = parser.parse_args()

    apps = []
    for item in args.apps:
        app_id, sep, folder = item.partition("=")
        if not sep or not app_id or not os.path.isdir(folder):
            parser.error("not ID=FOLDER: %s" % item)
        apps.append((app_id, folder))

    users = other_users(args.port)
    if users:
        print("push: %s is open in another program (pid %s); close it first: two programs on one "
              "port corrupt each other's replies" % (args.port, ", ".join(users)), file=sys.stderr)
        return 1
    try:
        badge = PushBadge(args.port, code=args.code, _serial=open_exclusive(args.port))
    except (OSError, BadgeError) as exc:
        print("push: cannot open %s: %s" % (args.port, exc), file=sys.stderr)
        return 1

    failed = []
    try:
        try:
            if badge.state().get("app"):
                badge.stop()                 # no app left that could stop during the push
                badge.wait_state(lambda s: s["app"] == "", timeout=5)
        except BadgeError:
            pass                             # a release build has no VKSTATE: push anyway
        for app_id, folder in apps:
            print("== %s" % app_id)
            try:
                for relative in badge.push(folder, app_id):
                    print("pushed %s" % relative)
                print("push: %s ok" % app_id)          # push-apps.sh counts these lines
            except BadgeError as exc:
                print("push: %s failed: %s" % (app_id, exc), file=sys.stderr)
                failed.append(app_id)
    finally:
        badge.close()
    if failed:
        print("push: FAILED: %s" % " ".join(failed), file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
