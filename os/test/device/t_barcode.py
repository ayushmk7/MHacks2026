"""The launcher's QR code reads back as config key `repo_url` (the project's repository), in both
themes. Needs OpenCV in the .venv (common.qr_decode); without it the decode is skipped."""
import time

import common


def run(badge):
    common.provision_test(badge)
    reply = badge.cmd("VKGET repo_url")[-1]
    link = reply[3:].strip() if reply.startswith("OK ") else ""
    if not link:
        print("  (repo_url not set: no code on the launcher)")
        return
    try:
        for theme in ("receipt-dark", "receipt-light"):          # ends on the light theme
            badge.ok("VKSET theme %s" % theme)
            common.to_launcher(badge)
            time.sleep(0.8)                                       # the shell looks at the theme every 500 ms
            pixels = badge.shot("test/device/shots/launcher_qr_%s.png" % theme)
            text = common.qr_decode(pixels)
            if text is None:
                print("  (no OpenCV: QR code not decoded)")
                return
            assert text == link, "%s: the QR code reads %r, expected %r" % (theme, text, link)
    finally:
        badge.ok("VKSET theme receipt-light")
