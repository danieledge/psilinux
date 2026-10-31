#!/usr/bin/env python3
"""A file frame buffer (640x240x8 + its RGB colour map, as Xfbdev -fb writes
it) to a PNG of what the Psion shows: each colour's grey as the runtime makes
it (runtime/lx_fb.c: Rec.601 on the 16-bit map), then 16 greys with the 2x2
ordered dither of common/lxfbconv.h.
    fb2psion.py fb.raw out.png [scale]"""
import sys
from PIL import Image
W, H = 640, 240
b = open(sys.argv[1], 'rb').read()
pix, cm = b[:W * H], b[W * H:W * H + 768]
grey = []
for i in range(256):
	r, g, bl = (cm[3 * i], cm[3 * i + 1], cm[3 * i + 2]) if len(cm) >= 3 * i + 3 else (0, 0, 0)
	grey.append((r * 257 * 77 + g * 257 * 150 + bl * 257 * 29) >> 16)
t = [[2, 10], [14, 6]]
lut = [[[0] * 256 for x in range(2)] for y in range(2)]
for y in range(2):
	for x in range(2):
		for i in range(256):
			v = (grey[i] * 15 * 16 + 127) // 255 + t[y][x]
			l = min(15, v >> 4)
			if grey[i] == 255: l = 15
			lut[y][x][i] = l * 17
out = bytearray(W * H)
for y in range(H):
	row = lut[y & 1]
	o = y * W
	for x in range(W):
		out[o + x] = row[x & 1][pix[o + x]]
im = Image.frombytes('L', (W, H), bytes(out))
s = int(sys.argv[3]) if len(sys.argv) > 3 else 1
if s > 1: im = im.resize((W * s, H * s), Image.NEAREST)
im.save(sys.argv[2])
