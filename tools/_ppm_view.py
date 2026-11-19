#!/usr/bin/env python3
"""Temporary helper: statistics, ASCII preview and PNG copy of a PPM frame dump.

    python3 tools/_ppm_view.py <file.ppm> [...]

ASCII: ' ' = black, '.' = dark, ':' = mid, '+' = bright, '#' = very bright.
"""
import os
import struct
import sys
import zlib

RAMP = " .:-=+*#%@"


def read_ppm(path):
    with open(path, "rb") as f:
        data = f.read()
    # P6\n<w> <h>\n255\n
    parts = data.split(b"\n", 3)
    assert parts[0] == b"P6", parts[0]
    w, h = (int(v) for v in parts[1].split())
    assert parts[2].strip() == b"255"
    px = parts[3]
    return w, h, px


def write_png(path, w, h, px):
    raw = b"".join(b"\x00" + px[y * w * 3:(y + 1) * w * 3] for y in range(h))

    def chunk(tag, blob):
        c = struct.pack(">I", len(blob)) + tag + blob
        return c + struct.pack(">I", zlib.crc32(tag + blob) & 0xFFFFFFFF)

    png = b"\x89PNG\r\n\x1a\n"
    png += chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(raw, 6))
    png += chunk(b"IEND", b"")
    with open(path, "wb") as f:
        f.write(png)


for path in sys.argv[1:]:
    w, h, px = read_ppm(path)
    n = w * h
    sums = [0, 0, 0]
    nonblack = 0
    bright = 0
    for i in range(n):
        r, g, b = px[i * 3], px[i * 3 + 1], px[i * 3 + 2]
        sums[0] += r
        sums[1] += g
        sums[2] += b
        if r or g or b:
            nonblack += 1
        if r + g + b > 96:
            bright += 1
    mean = [s // n for s in sums]
    print(os.path.basename(path), f"{w}x{h}", "mean", mean,
          "nonblack %.2f%%" % (100.0 * nonblack / n), "bright %.2f%%" % (100.0 * bright / n))
    cols, rows = 60, 20
    for ry in range(rows):
        line = ""
        for rx in range(cols):
            x = rx * w // cols
            y = ry * h // rows
            i = (y * w + x) * 3
            v = (px[i] + px[i + 1] + px[i + 2]) // 3
            line += RAMP[min(len(RAMP) - 1, v * len(RAMP) // 256)]
        print("  |" + line + "|")
    out = os.path.splitext(path)[0] + ".png"
    write_png(out, w, h, px)
    print("  png:", out)
