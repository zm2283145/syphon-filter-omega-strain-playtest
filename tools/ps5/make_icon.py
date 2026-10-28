#!/usr/bin/env python3
# Port-only (132ps5d, research/PS5.md "Build"): writes the folder title's sce_sys/icon0.png (512 x 512, a plain
# generated picture; the title has no artwork of its own and uses none of the game's).
# usage: make_icon.py <icon0.png>
import struct, sys, zlib

W = H = 512
rows = []
for y in range(H):
    row = bytearray(b'\x00')  # filter: none
    for x in range(W):
        band = 216 <= y < 296
        edge = x < 16 or x >= W - 16 or y < 16 or y >= H - 16
        if edge:
            row += bytes((150, 20, 20))
        elif band:
            row += bytes((200, 200, 200))
        else:
            row += bytes((24, 28, 36))
    rows.append(bytes(row))


def chunk(kind, data):
    return struct.pack('>I', len(data)) + kind + data + struct.pack('>I', zlib.crc32(kind + data) & 0xffffffff)


png = b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', W, H, 8, 2, 0, 0, 0))
png += chunk(b'IDAT', zlib.compress(b''.join(rows), 9)) + chunk(b'IEND', b'')
with open(sys.argv[1], 'wb') as out:
    out.write(png)