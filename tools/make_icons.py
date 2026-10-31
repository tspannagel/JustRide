#!/usr/bin/env python3
"""Render the web app's home-screen icons (lightning bolt on the accent gradient).

Writes www/stats/apple-touch-icon.png (180), icon-192.png and icon-512.png.
Pure standard library (no Pillow): the bolt is filled by scanline with 8 subrows per
pixel and exact horizontal coverage. Full-bleed squares: iOS rounds the corners itself,
Android masks them ("maskable"), so the bolt stays inside the central safe zone.
Run again only if the logo changes; the PNGs are committed.
"""
import os
import struct
import zlib

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'www', 'stats')
# Same colours and bolt as the rail logo in style.css / core.js (24x24 viewBox)
C0, C1 = (0x7c, 0x8c, 0xff), (0xb3, 0x7c, 0xff)
BOLT = [(13, 2), (3, 14), (12, 14), (11, 22), (21, 10), (12, 10)]
SCALE = 0.56  # bolt height as a share of the icon (maskable safe zone: 80 % circle)
SUB = 8


def render(n):
    s = n * SCALE / 20  # bolt spans y 2..22
    pts = [((x - 12) * s + n / 2, (y - 12) * s + n / 2) for x, y in BOLT]
    cov = [[0.0] * n for _ in range(n)]
    for py in range(n):
        for k in range(SUB):
            y = py + (k + 0.5) / SUB
            xs = []
            for (x0, y0), (x1, y1) in zip(pts, pts[1:] + pts[:1]):
                if (y0 <= y < y1) or (y1 <= y < y0):
                    xs.append(x0 + (y - y0) * (x1 - x0) / (y1 - y0))
            xs.sort()
            row = cov[py]
            for a, b in zip(xs[::2], xs[1::2]):
                for px in range(max(0, int(a)), min(n, int(b) + 1)):
                    row[px] += (min(b, px + 1) - max(a, px)) / SUB
    raw = bytearray()
    for py in range(n):
        raw.append(0)
        for px in range(n):
            t = (px + py) / (2 * (n - 1))  # 135deg gradient, top left to bottom right
            bg = [c0 + (c1 - c0) * t for c0, c1 in zip(C0, C1)]
            a = min(1.0, cov[py][px])
            raw += bytes(round(c + (255 - c) * a) for c in bg)
    return raw


def png(n, raw):
    def chunk(tag, data):
        return struct.pack('>I', len(data)) + tag + data + struct.pack('>I', zlib.crc32(tag + data))
    return (b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', n, n, 8, 2, 0, 0, 0)) +
            chunk(b'IDAT', zlib.compress(bytes(raw), 9)) + chunk(b'IEND', b''))


for name, n in (('apple-touch-icon.png', 180), ('icon-192.png', 192), ('icon-512.png', 512)):
    with open(os.path.join(ROOT, name), 'wb') as f:
        f.write(png(n, render(n)))
    print(name, n)
