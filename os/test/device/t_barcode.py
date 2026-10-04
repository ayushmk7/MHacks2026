"""The launcher's barcode is a real Code 128 that a scanner reads back as the badge ID (the first 8
characters of the address), in both themes. Needs zxing-cpp and numpy in the .venv (OpenCV reads no Code 128); without it the decode is skipped."""
import time

import common


def barcode_decode(pixels, width=320, height=240, scale=3):
    try:
        import numpy
        import zxingcpp
    except ImportError:
        return None
    values = numpy.frombuffer(pixels, dtype="<u2").reshape(height, width).astype(numpy.uint32)
    red, green, blue = (values >> 11) & 0x1F, (values >> 5) & 0x3F, values & 0x1F
    grey = ((red * 255 // 31) * 299 + (green * 255 // 63) * 587 + (blue * 255 // 31) * 114) // 1000
    big = numpy.kron(grey.astype(numpy.uint8), numpy.ones((scale, scale), dtype=numpy.uint8))
    found = zxingcpp.read_barcodes(big)
    return found[0].text if found else ""


def run(badge):
    common.provision_test(badge)
    address = badge.info()["pubkey"]
    try:
        for theme in ("receipt-dark", "receipt-light"):          # ends on the light theme
            badge.ok("VKSET theme %s" % theme)
            common.to_launcher(badge)
            time.sleep(0.8)                                       # the shell looks at the theme every 500 ms
            pixels = badge.shot("test/device/shots/launcher_barcode_%s.png" % theme)
            text = barcode_decode(pixels)
            if text is None:
                print("  (no zxing-cpp: barcode not decoded)")
                return
            assert text == address[:8], "%s: the barcode reads %r, expected %r" % (theme, text, address[:8])
    finally:
        badge.ok("VKSET theme receipt-light")
