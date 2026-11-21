#!/usr/bin/env python3
"""codaicon.py - Coda's own icons, one recipe for all (PsiLinux's copy of psion-netbook
psiicons/art/codaicon.py at 1a1d5a0: keep them the same) (UI review 2.3, 21
November 2026; design audit DA-15 to DA-18).

Every Coda program and Control panel item draws its icon here: each
pkg/mkicon.py describes its object as a few shapes and calls icons() and
write_aif(). The recipe, applied by this module (not by each icon):
- a single object, front-facing (or Psion's three-quarter view), no tile
  or rounded square behind it;
- each shape is filled flat with a colour of the 6x6x6 cube (every channel
  a multiple of 0x33: DESIGN.md's rule, the panel shows them cleanly);
- each shape gets a 1 px outline in 333333, a lighter edge (one cube step)
  inside it top and left, a darker one bottom and right, when it is big
  enough for that at the size drawn; details (lines of text, dots) are
  flat, with no outline;
- a 2 px drop shadow in 999999 to the bottom right (1 px at 24 and 32), as
  Psion's own icons have;
- hard edges only: a pixel is the shape's or not (the masks are 1-bit in
  effect: README, "EPOC does not blend 2-bit masks properly"), so no pale
  fringe on Dark's charcoal or the grey toolbar;
- the 48 px icon leaves 2 rows clear at the top and bottom (the Extras
  bar's cell must not cut it), the 24 and 32 one row;
- each size is drawn, not shrunk: a shape can be limited to some sizes
  (only=(32, 48)), so the 24 px icon is the object with its detail left
  out.
The Coda colours (UI review 2.4): navy 003366, blue 0066cc, teal 009999,
orange ff6600, greys, white; pale blue ccccff and pale orange ffcc99.
Three hues at most in one icon (checked).

Coordinates are in a 48 x 48 grid whatever the size: (0,0) the top left
corner of the top left pixel; rect(x0, y0, x1, y1) covers x0 <= x < x1.
Use y from 2 to 44 and x from 1 to 45: the shadow takes 2 more.

The .aif: 8-bit colour bitmaps and 2-bit masks, each RLE-compressed when
that is smaller (as Psion's own .aif files are): flat fills compress to
well under half, so a redrawn icon takes less room in the ROM than the
old uncompressed one.

    python3 codaicon.py SHEET.png      the sheet of every Coda icon
"""
import os
import struct
import sys

from PIL import Image, ImageDraw

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)   # (PsiLinux's copy: epoc256.py beside it; psion-netbook keeps it in psihub/pkg)
from epoc256 import PALETTE  # noqa: E402

RGB = [(v & 0xff, (v >> 8) & 0xff, (v >> 16) & 0xff) for v in PALETTE]
INDEX = {c: i for i, c in reversed(list(enumerate(RGB)))}

OUTLINE = '333333'
SHADOW = '999999'
# the drop shadow (the UI review's recipe, as Psion's icons); False draws
# none (the 14 November house style, README: for Dan to choose)
DROP_SHADOW = True
SIZES = (24, 32, 48)

# the Coda set and the greys
NAVY, BLUE, TEAL, ORANGE = '003366', '0066cc', '009999', 'ff6600'
WHITE, BLACK = 'ffffff', '000000'
G1, G2, G3, G4, G5 = 'cccccc', '999999', '666666', '333333', 'eeeeee'
PALEBLUE, PALEORANGE, SKY = 'ccccff', 'ffcc99', '99ccff'


def rgb(h):
    h = h.lstrip('#')
    return tuple(int(h[i:i + 2], 16) for i in (0, 2, 4))


def hexs(c):
    return '%02x%02x%02x' % c


def on_cube(c):
    return all(v % 0x33 == 0 for v in c)


def step(h, k):
    """one cube step lighter (k=+1) or darker (k=-1), each channel"""
    return hexs(tuple(max(0, min(255, v + 0x33 * k)) if (k > 0 or v) else v for v in rgb(h)))


def hue(c):
    """a rough hue family for the three-hue check: None for a grey"""
    r, g, b = c
    if max(c) - min(c) < 0x33:
        return None
    if r >= g and r >= b:
        return 'orange' if g > b else 'red'
    if b >= r and b >= g:
        return 'blue'
    return 'green' if g - b > 0x33 else 'teal'


class Canvas:
    """the shapes of one icon at one size, back to front"""

    def __init__(self, n):
        self.n = n
        self.s = n / 48.0
        self.shapes = []

    def _p(self, v):
        return int(round(v * self.s))

    def _add(self, draw, fill, only, edge, bevel):
        if only and self.n not in only:
            return
        lay = Image.new('1', (self.n, self.n), 0)
        draw(ImageDraw.Draw(lay))
        self.shapes.append((lay, fill, edge, bevel))

    def rect(self, x0, y0, x1, y1, fill, only=None, edge=True, bevel=True):
        p = self._p
        a, b, c, d = p(x0), p(y0), max(p(x0) + 1, p(x1)) - 1, max(p(y0) + 1, p(y1)) - 1
        self._add(lambda g: g.rectangle([a, b, c, d], fill=1), fill, only, edge, bevel)

    def ellipse(self, x0, y0, x1, y1, fill, only=None, edge=True, bevel=True):
        p = self._p
        a, b, c, d = p(x0), p(y0), max(p(x0) + 1, p(x1)) - 1, max(p(y0) + 1, p(y1)) - 1
        self._add(lambda g: g.ellipse([a, b, c, d], fill=1), fill, only, edge, bevel)

    def poly(self, pts, fill, only=None, edge=True, bevel=True):
        q = [(v[0] * self.s, v[1] * self.s) for v in pts]
        self._add(lambda g: g.polygon(q, fill=1), fill, only, edge, bevel)

    def line(self, pts, w, fill, only=None, edge=False, bevel=False):
        """a stroke w grid units wide (at least 1 px); flat unless edge"""
        q = [(v[0] * self.s, v[1] * self.s) for v in pts]
        px = max(1, int(round(w * self.s)))
        self._add(lambda g: g.line(q, fill=1, width=px), fill, only, edge, bevel)

    def arc(self, x0, y0, x1, y1, start, end, w, fill, only=None, edge=False):
        """a stroke along an ellipse (degrees clockwise from 3 o'clock);
        edge=True outlines a thick one as a shape"""
        p = self._p
        px = max(1, int(round(w * self.s)))
        box = [p(x0), p(y0), max(p(x0) + 1, p(x1)) - 1, max(p(y0) + 1, p(y1)) - 1]
        self._add(lambda g: g.arc(box, start, end, fill=1, width=px), fill, only, edge, False)

    def dot(self, x, y, fill, only=None):
        """one pixel at any size (a detail)"""
        p = self._p
        self._add(lambda g: g.point((min(self.n - 1, p(x)), min(self.n - 1, p(y))), fill=1), fill, only, False, False)

    def bar(self, x0, y0, x1, y1, fill, only=None):
        """a flat detail: a line of text, a key, a contact"""
        self.rect(x0, y0, x1, y1, fill, only, edge=False, bevel=False)

    # ---- to pixels

    def render(self):
        n = self.n
        col = [[None] * n for _ in range(n)]
        for lay, fill, edge, bevel in self.shapes:
            L = lay.load()
            inside = lambda x, y: 0 <= x < n and 0 <= y < n and L[x, y]
            xs = [x for x in range(n) for y in range(n) if L[x, y]]
            if not xs:
                continue
            ys = [y for x in range(n) for y in range(n) if L[x, y]]
            big = (max(xs) - min(xs) >= 7) and (max(ys) - min(ys) >= 7)
            light, dark = step(fill, 1), step(fill, -1)
            rim = lambda x, y: inside(x, y) and not (inside(x - 1, y) and inside(x + 1, y) and inside(x, y - 1) and inside(x, y + 1))
            for y in range(n):
                for x in range(n):
                    if not L[x, y]:
                        continue
                    c = fill
                    if edge and rim(x, y):
                        c = OUTLINE
                    elif edge and bevel and big:
                        if rim(x - 1, y) or rim(x, y - 1):
                            c = light
                        elif rim(x + 1, y) or rim(x, y + 1):
                            c = dark
                    col[y][x] = c
        # the drop shadow: the silhouette moved down and right
        k = (2 if n >= 48 else 1) if DROP_SHADOW else n
        drawn = [[col[y][x] is not None for x in range(n)] for y in range(n)]
        for y in range(n):
            for x in range(n):
                if not drawn[y][x] and x >= k and y >= k and drawn[y - k][x - k]:
                    col[y][x] = SHADOW
                elif not drawn[y][x] and x >= 1 and y >= 1 and k == 2 and drawn[y - 1][x - 1]:
                    col[y][x] = SHADOW
        im = Image.new('RGB', (n, n), (255, 255, 255))
        mk = Image.new('1', (n, n), 1)
        for y in range(n):
            for x in range(n):
                if col[y][x] is not None:
                    im.putpixel((x, y), rgb(col[y][x]))
                    mk.putpixel((x, y), 0)
        return im, mk


def check(im, mk, name=''):
    """the recipe's rules, at each size: cube colours only (DA-15), at
    most three hues, rows clear at the top and bottom (DA-18)"""
    n = im.size[0]
    hues = set()
    for y in range(n):
        for x in range(n):
            if mk.getpixel((x, y)) == 0:
                c = im.getpixel((x, y))[:3]
                if not on_cube(c):
                    raise SystemExit(f'{name} {n}: {hexs(c)} at {x},{y} is not a cube colour')
                h = hue(c)
                if h:
                    hues.add(h)
    if len(hues) > 3:
        raise SystemExit(f'{name} {n}: {len(hues)} hues ({", ".join(sorted(hues))}): three at most')
    clear = 2 if n >= 48 else 1
    for y in list(range(clear)) + list(range(n - clear, n)):
        if any(mk.getpixel((x, y)) == 0 for x in range(n)):
            raise SystemExit(f'{name} {n}: row {y} is not clear (the {clear} px margin)')


def icons(design, name='', sizes=SIZES):
    """[(RGB image, 1-bit mask)] at each size, checked"""
    out = []
    for n in sizes:
        c = Canvas(n)
        design(c)
        im, mk = c.render()
        check(im, mk, name)
        out.append((im, mk))
    return out


# ---- the .aif

def _rle(raw):
    out = bytearray()
    i, n = 0, len(raw)
    while i < n:
        j = i
        while j < n and j - i < 128 and raw[j] == raw[i]:
            j += 1
        if j - i >= 2:
            out += bytes([j - i - 1, raw[i]]); i = j
            continue
        j = i
        while j < n and j - i < 128 and not (j + 1 < n and raw[j] == raw[j + 1]):
            j += 1
        if j == i:
            j = i + 1
        out += bytes([256 - (j - i)]) + raw[i:j]; i = j
    return bytes(out)


def _stream(w, h, bpp, colour, raw):
    """CFbsBitmap::ExternalizeL: RLE (compression 1) when smaller"""
    packed = _rle(raw)
    comp = 1 if len(packed) < len(raw) else 0
    data = packed if comp else raw
    return struct.pack('<IIiiiiIIII', 40 + len(data), 40, w, h, 0, 0, bpp, colour, 0, comp) + data


def colour_bitmap(img):
    """8 bpp, EPOC's 256 colours (the cube colours exactly)"""
    w, h = img.size
    stride = (w + 3) & ~3
    raw = bytearray()
    for y in range(h):
        row = bytearray(stride)
        for x in range(w):
            c = img.getpixel((x, y))[:3]
            row[x] = INDEX[c]
        raw += row
    return _stream(w, h, 8, 1, bytes(raw))


def grey_bitmap(img, bpp=2):
    """2 bpp greys (a mask, or the 5mx's grey icon): 0 black"""
    img = img.convert('L')
    w, h = img.size
    stride = ((w * bpp + 31) // 32) * 4
    levels = (1 << bpp) - 1
    raw = bytearray()
    for y in range(h):
        row = bytearray(stride)
        for i in range((w * bpp + 7) // 8, stride):
            row[i] = 0xff
        for x in range(w):
            v = round(img.getpixel((x, y)) * levels / 255)
            bit = x * bpp
            row[bit // 8] |= v << (bit % 8)
        raw += row
    return _stream(w, h, bpp, 0, bytes(raw))


def _des8(text):
    """an externalised narrow descriptor (psion-core's aif.des8)"""
    b = text.encode('latin-1')
    return bytes([((len(b) << 1) | 1) << 1]) + b


def _crc16(data):
    crc = 0
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xffff if crc & 0x8000 else (crc << 1) & 0xffff
    return crc


def _uid_checksum(u1, u2, u3):
    b = struct.pack('<III', u1, u2, u3)
    return (_crc16(b[1::2]) << 16) | _crc16(b[0::2])


def write_aif(path, app_uid, captions, icons_, hidden=0, grey=False, extra=b'', extra_root=b''):
    """an .aif: captions [(language, text)], icons [(RGB image, mask)];
    grey=True: the icons in 2-bit greys (a 5mx-only build). extra: more
    streams after the icons; extra_root: what follows the four flags in
    the root (data types: psiview's), with offsets into extra already
    made relative to... (callers that need it pass both ready)."""
    out = bytearray(20)
    cap_hdr, icon_hdr = [], []
    for lang, text in captions:
        cap_hdr.append((len(out), lang))
        out += _des8(text)
    for img, mask in icons_:
        icon_hdr.append((len(out), img.size[0]))
        out += (grey_bitmap(img) if grey else colour_bitmap(img)) + grey_bitmap(mask)
    out += extra
    root = len(out)
    out += bytes([len(cap_hdr) << 1])
    for sid, lang in cap_hdr:
        out += struct.pack('<Ih', sid, lang)
    out += bytes([len(icon_hdr) << 1])
    for sid, side in icon_hdr:
        out += struct.pack('<Ih', sid, side)
    out += struct.pack('<iiii', 1, 0, 0, hidden)
    out += extra_root
    struct.pack_into('<IIIII', out, 0, 0x10000037, 0x1000006A, app_uid,
                     _uid_checksum(0x10000037, 0x1000006A, app_uid), root)
    with open(path, 'wb') as f:
        f.write(out)
    return len(out)


def preview(path, sets, scale=3):
    """a sheet: each icon's three sizes on white, the toolbar's grey, the
    Extras bar's pale blue and Dark's charcoal; sets: [(name, icons)]"""
    grounds = [(255, 255, 255), (204, 204, 204), (204, 221, 255), (51, 51, 51)]
    cw = (24 + 32 + 48 + 16) * len(grounds) + 8
    sheet = Image.new('RGB', (cw * scale, (52 * len(sets) + 4) * scale), (255, 255, 255))
    for r, (name, ics) in enumerate(sets):
        x = 4
        for g in grounds:
            for im, mk in ics:
                n = im.size[0]
                tile = Image.new('RGB', (n, n), g)
                tile.paste(im, (0, 0), Image.eval(mk.convert('L'), lambda v: 255 - v))
                sheet.paste(tile.resize((n * scale, n * scale), Image.NEAREST), (x * scale, (4 + r * 52) * scale))
                x += n + 4
            x += 4
    sheet.save(path)


def same_file_icons():
    """(path, name) of every Coda icon script, for the sheet"""
    top = os.path.join(HERE, '..', '..')
    return top


if __name__ == '__main__':
    print(__doc__)
