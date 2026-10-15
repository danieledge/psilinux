#!/usr/bin/env python3
"""PsiLinux's pictures: the toolbar's (24x20, 4 greys, with masks, for
bmconv) and the program's icon (PsiLinux.aif, 24, 32 and 48 pixels, with
PsiTerm's AIF writer).

    linux/tools/mkicons.py OUTDIR HEADER

The Keys picture is PsiTerm's (tools/mkicons.py); the others are new, drawn
the same way: k black, d dark grey, l light grey, w white, . transparent.
"""
import os, sys
from PIL import Image, ImageDraw
HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, '..', '..'))
sys.path.insert(0, os.path.join(REPO, 'tools'))
sys.path.insert(0, os.path.join(REPO, 'tools', 'icon'))
import mkicons as pt          # PsiTerm's
from aif import write_aif

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

K, D, L, W = 0, 85, 170, 255


def app_icon(n):
    """A penguin on a dark rounded square, drawn 4x and scaled down."""
    S = 4
    N = n * S
    s = N / 48.0
    R = lambda v: int(round(v * s))
    big = Image.new('L', (N, N), W)
    d = ImageDraw.Draw(big)
    d.rounded_rectangle([R(2), R(2), N - 1 - R(2), N - 1 - R(2)], radius=R(11), fill=D)
    # body and belly
    d.ellipse([R(13), R(9), R(35), R(41)], fill=K)
    d.ellipse([R(17), R(17), R(31), R(40)], fill=W)
    # eyes
    d.ellipse([R(19), R(13), R(23), R(17)], fill=W)
    d.ellipse([R(25), R(13), R(29), R(17)], fill=W)
    d.ellipse([R(20.5), R(14.5), R(22.5), R(16.5)], fill=K)
    d.ellipse([R(25.5), R(14.5), R(27.5), R(16.5)], fill=K)
    # beak and feet
    d.polygon([(R(21), R(18)), (R(27), R(18)), (R(24), R(22))], fill=L)
    d.ellipse([R(14), R(38), R(23), R(43)], fill=L)
    d.ellipse([R(25), R(38), R(34), R(43)], fill=L)
    im = big.resize((n, n), Image.LANCZOS)
    im = im.point(lambda v: min((K, D, L, W), key=lambda g: abs(g - v)))
    mk = Image.new('L', (N, N), 255)
    ImageDraw.Draw(mk).rounded_rectangle([R(2), R(2), N - 1 - R(2), N - 1 - R(2)], radius=R(11), fill=0)
    mk = mk.resize((n, n), Image.LANCZOS).point(lambda v: 0 if v < 160 else 255).convert('1')
    return im, mk


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
    with open(header, 'w') as f:
        f.write('/* lxicons.h - made by linux/tools/mkicons.py: PsiLinux.mbm ids */\n')
        f.write('#ifndef LXICONS_H\n#define LXICONS_H\nenum TLxIconId\n\t{\n')
        for i, n in enumerate(ids):
            f.write(f'\tELxMbm{n} = {2 * i}, ELxMbm{n}Mask = {2 * i + 1},\n')
        f.write(f'\tELxMbmCount = {2 * len(ids)}\n\t}};\n#endif\n')
    with open(os.path.join(out, 'files.txt'), 'w') as f:
        for i, p in enumerate(files):
            f.write(('/2' if i % 2 == 0 else '/1') + os.path.basename(p) + '\n')
    icons = [app_icon(n) for n in (24, 32, 48)]
    n = write_aif(os.path.join(out, 'psilinux.aif'), 0x01000A87, [(1, 'PsiLinux'), (10, 'PsiLinux')], icons)
    prev = Image.new('L', (24 + 32 + 48 + 20, 48), W)
    x = 0
    for im, _ in icons:
        prev.paste(im, (x, 0))
        x += im.size[0] + 10
    prev.resize((prev.size[0] * 3, 144), Image.NEAREST).save(os.path.join(out, 'icon-preview.png'))
    print('psilinux.aif', n, 'bytes')


if __name__ == '__main__':
    main()
