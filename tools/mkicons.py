#!/usr/bin/env python3
"""PsiLinux's pictures: the toolbar's (24x20, 4 greys, with masks, for
bmconv), the same four in colour for the netBook's toolbar (0.43, below:
COLOUR) and the program's icon (PsiLinux.aif, 24, 32 and 48 pixels: the
penguin in Coda's icon style, drawn with Coda's icon kit, codaicon.py, a
copy of psion-netbook's psiicons/art/codaicon.py, with epoc256.py, EPOC's
palette, beside this file).

    tools/mkicons.py OUTDIR HEADER

The Keys picture is PsiTerm's (psion-core's tools/mkicons.py, from this
checkout's core/ submodule or $PSION_CORE); the others are new, drawn
the same way: k black, d dark grey, l light grey, w white, . transparent.
"""
import os, sys
from PIL import Image
HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, '..'))
CORE = os.environ.get('PSION_CORE') or os.path.join(REPO, 'core')
sys.path.insert(0, os.path.join(CORE, 'tools'))
sys.path.insert(0, os.path.join(CORE, 'tools', 'icon'))
import mkicons as pt          # PsiTerm's
sys.path.insert(0, HERE)
import codaicon               # Coda's icon kit (the program's icon)

ICONS = [
# Stop (Ctrl+C): an octagon with a white bar, as a road sign
('ToolStop', """
........................
.......kkkkkkkkkk.......
......kddddddddddk......
.....kddddddddddddk.....
....kddddddddddddddk....
...kddddddddddddddddk...
...kddddddddddddddddk...
...kddddddddddddddddk...
...kddwwwwwwwwwwwwddk...
...kddwwwwwwwwwwwwddk...
...kddwwwwwwwwwwwwddk...
...kddddddddddddddddk...
...kddddddddddddddddk...
...kddddddddddddddddk...
....kddddddddddddddk....
.....kddddddddddddk.....
......kddddddddddk......
.......kkkkkkkkkk.......
........................
........................
"""),
('ToolKeys', dict(pt.ICONS)['ToolKeys']),
# Copy: two pages, one over the other
('ToolCopy', """
........................
...kkkkkkkkkkkk.........
...kwwwwwwwwwwkk........
...kwkkkkkkkwwkwk.......
...kwwwwwwwwwwkkkk......
...kwkkkkkkkkkkkkkkkkk..
...kwwwwwwwwkwwwwwwwwkk.
...kwkkkkkkkkwkkkkkkwkwk
...kwwwwwwwwkwwwwwwwwkkk
...kwkkkkkkkkwkkkkkkkkwk
...kwwwwwwwwkwwwwwwwwwwk
...kwkkkkkkkkwkkkkkkkkwk
...kwwwwwwwwkwwwwwwwwwwk
...kkkkkkkkkkwkkkkkkkkwk
............kwwwwwwwwwwk
............kwkkkkkkkkwk
............kwwwwwwwwwwk
............kkkkkkkkkkkk
........................
........................
"""),
# Paste: a clipboard with a page on it
('ToolPaste', """
.........kkkkkk.........
....kkkkkkllllkkkkkk....
....kddddkkkkkkddddk....
....kddddddddddddddk....
....kddkkkkkkkkkkddk....
....kddkwwwwwwwwkddk....
....kddkwkkkkkkwkddk....
....kddkwwwwwwwwkddk....
....kddkwkkkkkkwkddk....
....kddkwwwwwwwwkddk....
....kddkwkkkkkkwkddk....
....kddkwwwwwwwwkddk....
....kddkwkkkkwwwkddk....
....kddkwwwwwwwwkddk....
....kddkkkkkkkkkkddk....
....kddddddddddddddk....
....kkkkkkkkkkkkkkkk....
........................
........................
........................
"""),
]

# (0.43) The netBook's toolbar pictures, in colour, drawn as Coda's (its
# style sheet, 2.3): one object seen from the front, a 1-pixel outline in
# 333333, flat fills from the 6x6x6 cube with a lighter edge at the top left
# and a darker one at the bottom right, and a 2-pixel shadow in 999999 at the
# bottom right; colours from Coda's set (navy 003366, blue 0066cc, teal
# 009999, orange ff6600) and its greys. 24x20, with masks; bmconv keeps
# them in 256 colours (/c8).
PAL = {
    'k': (0x33, 0x33, 0x33), 's': (0x99, 0x99, 0x99), 'w': (0xff, 0xff, 0xff),
    'g': (0xcc, 0xcc, 0xcc), 'd': (0x66, 0x66, 0x66), 'o': (0xff, 0x66, 0x00),
    'O': (0xff, 0x99, 0x33), 'r': (0xcc, 0x33, 0x00), 'n': (0x00, 0x33, 0x66),
    'b': (0x00, 0x66, 0xcc), 't': (0x00, 0x99, 0x99), 'T': (0x33, 0xcc, 0xcc),
    'D': (0x00, 0x66, 0x66),
}


def recipe(shapes):
    """shapes: [(inside(x, y), fill, light, dark, marks)] drawn in order, the
    later over the earlier; marks: {(x, y): colour letter} over the fill.
    Each shape gets its outline, its light and dark edges and its shadow."""
    W_, H_ = 24, 20
    grid = [['.'] * W_ for _ in range(H_)]
    for inside, fill, light, dark, marks in shapes:
        cells = {(x, y) for y in range(H_) for x in range(W_) if inside(x, y)}
        for (x, y) in cells:                      # the shadow first, under it
            for sx, sy in ((x + 1, y + 1), (x + 2, y + 2), (x + 2, y + 1), (x + 1, y + 2)):
                if (sx, sy) not in cells and 0 <= sx < W_ and 0 <= sy < H_ and grid[sy][sx] == '.':
                    grid[sy][sx] = 's'
        for (x, y) in cells:
            edge = any((x + dx, y + dy) not in cells for dx in (-1, 0, 1) for dy in (-1, 0, 1))
            if edge:
                c = 'k'
            elif (x - 2, y) not in cells or (x, y - 2) not in cells:
                c = light
            elif (x + 2, y) not in cells or (x, y + 2) not in cells:
                c = dark
            else:
                c = fill
            grid[y][x] = marks.get((x, y), c) if not edge else c
    return grid


def octagon(x, y):
    # 16 wide, 16 high, corners cut by 4
    x0, y0, n, cut = 4, 1, 16, 4
    u, v = x - x0, y - y0
    if not (0 <= u < n and 0 <= v < n):
        return False
    return u + v >= cut and (n - 1 - u) + v >= cut and u + (n - 1 - v) >= cut and (n - 1 - u) + (n - 1 - v) >= cut


def rect(x0, y0, x1, y1):
    return lambda x, y: x0 <= x <= x1 and y0 <= y <= y1


def lines(x0, x1, ys, colour):
    return {(x, y): colour for y in ys for x in range(x0, x1 + 1)}


COLOUR = [
    # Stop: an orange octagon with a white bar, as a road sign
    ('ToolStopC', [(octagon, 'o', 'O', 'r', lines(7, 16, (8, 9, 10), 'w'))]),
    # Keys: two key caps, a ~ and a | on them (keys the keyboard lacks)
    ('ToolKeysC', [(rect(2, 2, 13, 11), 'g', 'w', 's', {(4, 7): 'n', (5, 6): 'n', (6, 5): 'n', (7, 5): 'n', (8, 6): 'n',
                                                     (9, 7): 'n', (10, 7): 'n', (11, 6): 'n', (12, 5): 'n'}),
                   (rect(9, 7, 20, 16), 'g', 'w', 's', lines(14, 15, (9, 10, 11, 12, 13, 14), 'n'))]),
    # Copy: two pages, one over the other, navy lines of text
    ('ToolCopyC', [(rect(2, 1, 13, 13), 'w', 'w', 'g', lines(5, 10, (4, 6, 8, 10), 'b')),
                   (rect(9, 5, 20, 17), 'w', 'w', 'g', lines(12, 17, (8, 10, 12, 14), 'n'))]),
    # Paste: a teal clipboard, a grey clip, a page with navy lines
    ('ToolPasteC', [(rect(4, 2, 19, 17), 't', 'T', 'D', {}),
                    (rect(7, 5, 16, 15), 'w', 'w', 'g', lines(9, 14, (8, 10, 12), 'n')),
                    (rect(9, 1, 14, 4), 'g', 'w', 'd', {})]),
]


def _blob(c, ells, fill, only=None, edge=True, bevel=True):
    """the union of several ellipses on a codaicon canvas, outlined as one
    shape (the penguin's head and body)"""
    p = c._p
    boxes = [[p(a), p(b), max(p(a) + 1, p(x)) - 1, max(p(b) + 1, p(y)) - 1] for a, b, x, y in ells]

    def draw(g):
        for box in boxes:
            g.ellipse(box, fill=1)
    c._add(draw, fill, only, edge, bevel)


def penguin(c):
    """The program's icon (0.46): the penguin alone, standing, front on, in
    Coda's icon style (the UI review's style sheet 2.3): a black body, a
    white belly, an orange beak and feet; the kit gives each shape its
    1-pixel 333333 outline, the lighter and darker edges and the 999999 drop
    shadow, and checks the cube colours, the hues and the margins. At 24 the
    flippers and the pupils are left out."""
    B, W_, O = codaicon.BLACK, codaicon.WHITE, codaicon.ORANGE
    c.poly([(13, 17), (5, 31), (7, 35), (14, 31)], B, only=(32, 48), bevel=False)    # the flippers
    c.poly([(35, 17), (43, 31), (41, 35), (34, 31)], B, only=(32, 48), bevel=False)
    _blob(c, [(14, 2, 34, 22), (10, 13, 38, 42)], B, bevel=False)                    # head and body
    _blob(c, [(15, 18, 33, 40)], W_)                                                  # the belly
    c.ellipse(17, 7, 23, 14, W_, only=(32, 48), edge=False, bevel=False)              # the eyes
    c.ellipse(25, 7, 31, 14, W_, only=(32, 48), edge=False, bevel=False)
    c.bar(20, 9, 22, 13, B, only=(48,))
    c.bar(26, 9, 28, 13, B, only=(48,))
    c.dot(21, 11, B, only=(32,))
    c.dot(27, 11, B, only=(32,))
    c.dot(19, 9, W_, only=(24,))
    c.dot(28, 9, W_, only=(24,))
    c.poly([(18, 14), (30, 14), (24, 20)], O, bevel=False)                           # the beak
    c.ellipse(10, 37, 23, 44, O, bevel=False)                                         # the feet
    c.ellipse(25, 37, 38, 44, O, bevel=False)


def main():
    out, header = sys.argv[1], sys.argv[2]
    os.makedirs(out, exist_ok=True)
    files, ids = [], []
    for name, art in ICONS:
        rows, w, h = pt.parse(art)
        if (w, h) != (24, 20):
            raise SystemExit(f'{name}: {w}x{h}, expected 24x20')
        img = Image.new('L', (w, h), 255)
        mask = Image.new('1', (w, h), 1)
        for y, r in enumerate(rows):
            for x, c in enumerate(r):
                img.putpixel((x, y), pt.GREY[c])
                if c != '.':
                    mask.putpixel((x, y), 0)
        fi = os.path.join(out, f'{name}.bmp')
        fm = os.path.join(out, f'{name}_m.bmp')
        img.convert('RGB').save(fi)
        mask.convert('RGB').save(fm)
        files += [fi, fm]
        ids.append(name)
    for name, shapes in COLOUR:
        grid = recipe(shapes)
        img = Image.new('RGB', (24, 20), (255, 255, 255))
        mask = Image.new('1', (24, 20), 1)
        for y, r in enumerate(grid):
            for x, c in enumerate(r):
                if c != '.':
                    img.putpixel((x, y), PAL[c])
                    mask.putpixel((x, y), 0)
        fi = os.path.join(out, f'{name}.bmp')
        fm = os.path.join(out, f'{name}_m.bmp')
        img.save(fi)
        mask.convert('RGB').save(fm)
        files += [fi, fm]
        ids.append(name)
        img.resize((96, 80), Image.NEAREST).save(os.path.join(out, f'{name}-4x.png'))
    with open(header, 'w') as f:
        f.write('/* lxicons.h - made by tools/mkicons.py: PsiLinux.mbm ids */\n')
        f.write('#ifndef LXICONS_H\n#define LXICONS_H\nenum TLxIconId\n\t{\n')
        for i, n in enumerate(ids):
            f.write(f'\tELxMbm{n} = {2 * i}, ELxMbm{n}Mask = {2 * i + 1},\n')
        f.write(f'\tELxMbmCount = {2 * len(ids)}\n\t}};\n#endif\n')
    with open(os.path.join(out, 'files.txt'), 'w') as f:
        for i, p in enumerate(files):
            colour = os.path.basename(p).split('_')[0].split('.')[0].endswith('C')
            f.write(('/1' if i % 2 else '/c8' if colour else '/2') + os.path.basename(p) + '\n')
    icons = codaicon.icons(penguin, 'PsiLinux')
    n = codaicon.write_aif(os.path.join(out, 'psilinux.aif'), 0x01000A87, [(1, 'PsiLinux'), (10, 'PsiLinux')], icons)
    codaicon.preview(os.path.join(out, 'icon-preview.png'), [('PsiLinux', icons)])
    print('psilinux.aif', n, 'bytes')


if __name__ == '__main__':
    main()
