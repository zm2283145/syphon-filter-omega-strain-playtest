"""Port-only (checkpoint 132vt1, research/VITA.md "Packaging"): draws the PS Vita LiveArea images of the native
port's .vpk files (icon0 128x128, bg 840x500, startup 280x158, 8-bit palette PNGs as the Vita requires) and the
LiveArea template.xml. Everything is drawn here from a small built-in 5x7 font: no disc-derived art.

Usage: python tools/vita_livearea.py --out DIR --title "OMEGA STRAIN" --subtitle "NATIVE PORT"
"""
import argparse
import os
import struct
import zlib

FONT = {
    'A': ["01110", "10001", "10001", "11111", "10001", "10001", "10001"],
    'B': ["11110", "10001", "10001", "11110", "10001", "10001", "11110"],
    'C': ["01110", "10001", "10000", "10000", "10000", "10001", "01110"],
    'D': ["11110", "10001", "10001", "10001", "10001", "10001", "11110"],
    'E': ["11111", "10000", "10000", "11110", "10000", "10000", "11111"],
    'F': ["11111", "10000", "10000", "11110", "10000", "10000", "10000"],
    'G': ["01110", "10001", "10000", "10111", "10001", "10001", "01111"],
    'H': ["10001", "10001", "10001", "11111", "10001", "10001", "10001"],
    'I': ["01110", "00100", "00100", "00100", "00100", "00100", "01110"],
    'J': ["00111", "00010", "00010", "00010", "00010", "10010", "01100"],
    'K': ["10001", "10010", "10100", "11000", "10100", "10010", "10001"],
    'L': ["10000", "10000", "10000", "10000", "10000", "10000", "11111"],
    'M': ["10001", "11011", "10101", "10101", "10001", "10001", "10001"],
    'N': ["10001", "10001", "11001", "10101", "10011", "10001", "10001"],
    'O': ["01110", "10001", "10001", "10001", "10001", "10001", "01110"],
    'P': ["11110", "10001", "10001", "11110", "10000", "10000", "10000"],
    'Q': ["01110", "10001", "10001", "10001", "10101", "10010", "01101"],
    'R': ["11110", "10001", "10001", "11110", "10100", "10010", "10001"],
    'S': ["01111", "10000", "10000", "01110", "00001", "00001", "11110"],
    'T': ["11111", "00100", "00100", "00100", "00100", "00100", "00100"],
    'U': ["10001", "10001", "10001", "10001", "10001", "10001", "01110"],
    'V': ["10001", "10001", "10001", "10001", "10001", "01010", "00100"],
    'W': ["10001", "10001", "10001", "10101", "10101", "10101", "01010"],
    'X': ["10001", "10001", "01010", "00100", "01010", "10001", "10001"],
    'Y': ["10001", "10001", "01010", "00100", "00100", "00100", "00100"],
    'Z': ["11111", "00001", "00010", "00100", "01000", "10000", "11111"],
    '0': ["01110", "10001", "10011", "10101", "11001", "10001", "01110"],
    '1': ["00100", "01100", "00100", "00100", "00100", "00100", "01110"],
    '2': ["01110", "10001", "00001", "00010", "00100", "01000", "11111"],
    '3': ["11110", "00001", "00001", "01110", "00001", "00001", "11110"],
    '4': ["00010", "00110", "01010", "10010", "11111", "00010", "00010"],
    '5': ["11111", "10000", "11110", "00001", "00001", "10001", "01110"],
    '6': ["00110", "01000", "10000", "11110", "10001", "10001", "01110"],
    '7': ["11111", "00001", "00010", "00100", "01000", "01000", "01000"],
    '8': ["01110", "10001", "10001", "01110", "10001", "10001", "01110"],
    '9': ["01110", "10001", "10001", "01111", "00001", "00010", "01100"],
    '-': ["00000", "00000", "00000", "11111", "00000", "00000", "00000"],
    '.': ["00000", "00000", "00000", "00000", "00000", "01100", "01100"],
    ' ': ["00000"] * 7,
}

# Palette: 0 background, 1 band, 2 text, 3 accent.
PALETTE = [(10, 14, 22), (24, 40, 60), (230, 236, 240), (200, 40, 30)]


class Image:
    def __init__(self, width, height):
        self.width, self.height = width, height
        self.pixels = bytearray(width * height)

    def fill(self, x0, y0, x1, y1, index):
        for y in range(max(0, y0), min(self.height, y1)):
            row = y * self.width
            for x in range(max(0, x0), min(self.width, x1)):
                self.pixels[row + x] = index

    def text(self, cx, y, string, scale, index):
        width = len(string) * 6 * scale - scale
        x = cx - width // 2
        for ch in string.upper():
            glyph = FONT.get(ch, FONT[' '])
            for gy, line in enumerate(glyph):
                for gx, bit in enumerate(line):
                    if bit == '1':
                        self.fill(x + gx * scale, y + gy * scale, x + (gx + 1) * scale, y + (gy + 1) * scale, index)
            x += 6 * scale

    def png(self, path):
        def chunk(kind, data):
            body = kind + data
            return struct.pack('>I', len(data)) + body + struct.pack('>I', zlib.crc32(body) & 0xffffffff)
        raw = b''.join(b'\0' + bytes(self.pixels[y * self.width:(y + 1) * self.width]) for y in range(self.height))
        data = b'\x89PNG\r\n\x1a\n'
        data += chunk(b'IHDR', struct.pack('>IIBBBBB', self.width, self.height, 8, 3, 0, 0, 0))
        data += chunk(b'PLTE', b''.join(bytes(c) for c in PALETTE))
        data += chunk(b'IDAT', zlib.compress(raw, 9))
        data += chunk(b'IEND', b'')
        with open(path, 'wb') as f:
            f.write(data)


def scale_for(image_width, string, fraction):
    return max(1, int(image_width * fraction) // max(1, len(string) * 6))


def draw(width, height, title, subtitle):
    image = Image(width, height)
    band_top, band_bottom = height * 30 // 100, height * 74 // 100
    image.fill(0, band_top, width, band_bottom, 1)
    image.fill(0, band_bottom, width, band_bottom + max(2, height // 60), 3)
    title_scale = scale_for(width, title, 0.86)
    sub_scale = max(1, title_scale // 2)
    title_h, sub_h = 7 * title_scale, 7 * sub_scale
    gap = max(2, title_scale * 2)
    y = (band_top + band_bottom - title_h - gap - sub_h) // 2
    image.text(width // 2, y, title, title_scale, 2)
    image.text(width // 2, y + title_h + gap, subtitle, sub_scale, 3)
    return image


TEMPLATE = """<?xml version="1.0" encoding="utf-8"?>
<livearea style="a1" format-ver="01.00" content-rev="1">
  <livearea-background>
    <image>bg.png</image>
  </livearea-background>
  <gate>
    <startup-image>startup.png</startup-image>
  </gate>
</livearea>
"""


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--out', required=True)
    parser.add_argument('--title', required=True)
    parser.add_argument('--subtitle', required=True)
    args = parser.parse_args()
    os.makedirs(args.out, exist_ok=True)
    draw(128, 128, args.title, args.subtitle).png(os.path.join(args.out, 'icon0.png'))
    draw(840, 500, args.title, args.subtitle).png(os.path.join(args.out, 'bg.png'))
    draw(280, 158, args.title, args.subtitle).png(os.path.join(args.out, 'startup.png'))
    with open(os.path.join(args.out, 'template.xml'), 'w', encoding='utf-8', newline='\n') as f:
        f.write(TEMPLATE)


if __name__ == '__main__':
    main()
