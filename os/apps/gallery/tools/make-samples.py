#!/usr/bin/env python3
"""Regenerate the sample images that ship with the Gallery app.

Stdlib only - zlib and struct are enough to write an 8-bit RGB PNG, and
depending on Pillow to produce three test cards would be a silly thing to make
someone install. Run from anywhere:

    python3 apps/gallery/tools/make-samples.py

The output goes next to the app's main.lua, which is where badge.storage.list()
will find it. Keep every file well under the push server's 96 KB per-file cap.
"""

import os
import struct
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.dirname(HERE)

# The badge's brand ramp, from src/ui/theme.h.
PURPLE = (0x99, 0x45, 0xFF)
GREEN = (0x14, 0xF1, 0x95)
BG = (0x0B, 0x0B, 0x12)


# --- PNG encoder -----------------------------------------------------------
# Colour type 2 (truecolour), 8 bits per channel, no interlace. Every scanline
# gets one of the three cheap filters and we keep whichever predicts best, which
# is what pulls a smooth gradient down from 220 KB of raw pixels to a few KB.


def _chunk(tag, data):
    return (
        struct.pack(">I", len(data))
        + tag
        + data
        + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)
    )


def _filter_row(row, prev):
    """Return the best of None/Sub/Up as (type, filtered bytes)."""
    candidates = [(0, row)]

    sub = bytearray(len(row))
    for i, value in enumerate(row):
        sub[i] = (value - (row[i - 3] if i >= 3 else 0)) & 0xFF
    candidates.append((1, sub))

    up = bytearray(len(row))
    for i, value in enumerate(row):
        up[i] = (value - prev[i]) & 0xFF
    candidates.append((2, up))

    # The standard heuristic: smallest sum of the filtered bytes read as signed.
    def cost(candidate):
        return sum(b if b < 128 else 256 - b for b in candidate[1])

    return min(candidates, key=cost)


def write_png(path, width, height, shade):
    """shade(x, y) -> (r, g, b), called once per pixel."""
    raw = bytearray()
    prev = bytearray(width * 3)
    for y in range(height):
        row = bytearray(width * 3)
        for x in range(width):
            r, g, b = shade(x, y)
            row[x * 3 : x * 3 + 3] = bytes((int(r) & 0xFF, int(g) & 0xFF, int(b) & 0xFF))
        kind, filtered = _filter_row(row, prev)
        raw.append(kind)
        raw += filtered
        prev = row

    png = b"\x89PNG\r\n\x1a\n"
    png += _chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))
    png += _chunk(b"IDAT", zlib.compress(bytes(raw), 9))
    png += _chunk(b"IEND", b"")

    with open(path, "wb") as handle:
        handle.write(png)
    print("%-38s %5d x %-3d  %6.1f KB" % (path, width, height, len(png) / 1024.0))


# --- shading helpers -------------------------------------------------------


def ramp(t):
    t = max(0.0, min(1.0, t))
    return tuple(a + (b - a) * t for a, b in zip(PURPLE, GREEN))


def mix(a, b, t):
    t = max(0.0, min(1.0, t))
    return tuple(p + (q - p) * t for p, q in zip(a, b))


# --- the three cards -------------------------------------------------------


def brand_ramp(width, height):
    """A full-screen diagonal ramp with a faint measuring grid."""

    def shade(x, y):
        colour = ramp(x / width * 0.7 + y / height * 0.3)
        if x % 32 == 0 or y % 32 == 0:
            colour = mix(colour, BG, 0.35)
        return colour

    return shade


def target(width, height):
    """A small card whose detail makes 1x / 2x / fit obviously different."""
    centre = (width - 1) / 2.0

    def shade(x, y):
        dx, dy = x - centre, y - centre
        distance = (dx * dx + dy * dy) ** 0.5
        if distance > centre:
            # Checkerboard corners: 4 px squares, aliased hard on purpose.
            return BG if ((x // 4) + (y // 4)) % 2 else mix(BG, PURPLE, 0.25)
        if int(distance) % 12 < 6:
            return ramp(distance / centre)
        return BG

    return shade


if __name__ == "__main__":
    # 01-solana.png is deliberately NOT generated here. It is the official
    # logomark from https://solana.com/brand, rasterised from
    #   https://solana.com/src/img/branding/solanaLogoMark.svg
    # at 234x204 (the gallery's picture area, so "fit" is 1:1) over the theme
    # background. solana_mark() below approximated it and got the geometry
    # wrong - the bars sheared the opposite way and each carried its own
    # gradient instead of one ramp across the whole mark. Re-create it with:
    #
    #   curl -O https://solana.com/src/img/branding/solanaLogoMark.svg
    #   inkscape solanaLogoMark.svg -w 234 -h 204 -b "#0B0B12" -y 1.0 \
    #           --export-type=png -o 01-solana.png
    #
    # The brand mark is Solana's; do not redraw it by hand.

    # 04-hacker-shield.png and 05-defcon-34.png are likewise real artwork, not
    # generated: the BurbSec shield from
    #   https://github.com/BurbSec/Assets  (Official Shield Logos/hacker_shield.png)
    # and the DEF CON 34 logo from
    #   https://defcon.org/images/defcon-34/dc-34-logo-transparent.avif
    # Each is scaled to fit 320x204 (the picture area) with Lanczos, composited
    # onto the theme background #0B0B12 to drop the alpha channel, then reduced
    # to a 64-colour indexed palette:
    #
    #   im = Image.open(src).convert("RGBA")
    #   im = im.resize(fit, Image.LANCZOS)
    #   im = Image.alpha_composite(Image.new("RGBA", im.size, "#0B0B12"), im)
    #   im.convert("RGB").quantize(64, dither=Image.FLOYDSTEINBERG) \
    #     .save(dst, "PNG", optimize=True)
    #
    # Indexed (PNG colour type 3) rather than truecolour because it is ~3x
    # smaller at no visible cost on a 320x240 panel, and pngle - the decoder
    # behind gfx.image() - handles types 0, 2, 3, 4 and 6 at depths 1/2/4/8.
    # Do NOT reach for WebP to shrink these further: LovyanGFX ships no WebP
    # decoder, and gfx.image() routes .jpg/.jpeg to drawJpg and everything else
    # to drawPng, so a .webp would simply fail to decode. JPEG would work but
    # rings badly on sharp logo edges and small text.
    write_png(os.path.join(OUT, "02-ramp.png"), 320, 240, brand_ramp(320, 240))
    write_png(os.path.join(OUT, "03-target.png"), 96, 96, target(96, 96))
