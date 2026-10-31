#!/usr/bin/env python3
"""Writes the local test pages for Links on PsiLinux (test/shot.sh serves
them on 127.0.0.1): text, links, a form, a table with coloured cells,
colour swatches (to see the greys colours become on the Psion), and JPEG
(baseline and progressive), PNG (with transparency) and GIF pictures.
    mkpage.py DIR
"""
import math
import os
import sys

from PIL import Image, ImageDraw


def photo(w, h):
    """Something photo-like: a sky, hills, a sun and a house."""
    im = Image.new("RGB", (w, h))
    px = im.load()
    for y in range(h):
        for x in range(w):
            c = (90 + y * 120 // h, 140 + y * 80 // h, 230 - y * 60 // h)
            hill = h * 0.65 + math.sin(x / w * 6.3) * h * 0.12
            if y > hill:
                g = 90 + int(40 * math.sin(x * 0.21) * math.cos(y * 0.17))
                c = (40 + g // 3, g + 40, 30)
            px[x, y] = c
    d = ImageDraw.Draw(im)
    d.ellipse((w * 0.7, h * 0.1, w * 0.85, h * 0.1 + w * 0.15), fill=(255, 220, 90))
    d.rectangle((w * 0.15, h * 0.45, w * 0.3, h * 0.75), fill=(170, 60, 50))
    d.polygon([(w * 0.13, h * 0.45), (w * 0.225, h * 0.3), (w * 0.32, h * 0.45)], fill=(90, 40, 30))
    d.rectangle((w * 0.2, h * 0.58, w * 0.25, h * 0.75), fill=(60, 30, 20))
    return im


def main():
    out = sys.argv[1]
    os.makedirs(out, exist_ok=True)
    p = photo(240, 156)
    p.save(os.path.join(out, "photo.jpg"), quality=82)
    p.save(os.path.join(out, "photo-prog.jpg"), quality=82, progressive=True)
    photo(800, 520).save(os.path.join(out, "big.jpg"), quality=75)

    logo = Image.new("RGBA", (120, 56), (0, 0, 0, 0))
    d = ImageDraw.Draw(logo)
    d.rounded_rectangle((2, 2, 117, 53), 12, fill=(30, 90, 200, 255), outline=(0, 0, 0, 255), width=3)
    d.text((18, 21), "PNG + alpha", fill=(255, 255, 255, 255))
    logo.save(os.path.join(out, "logo.png"))

    g = Image.new("P", (96, 48), 0)
    d = ImageDraw.Draw(g)
    g.putpalette([255, 255, 255, 200, 30, 30, 30, 160, 30, 0, 0, 0] + [0] * 756)
    d.rectangle((0, 0, 95, 47), fill=1, outline=3)
    d.ellipse((24, 6, 72, 42), fill=2)
    d.text((38, 18), "GIF", fill=0)
    g.save(os.path.join(out, "badge.gif"))

    sw = "".join('<td bgcolor="%s" width=36>&nbsp;</td>' % c for c in
                 ("#ff0000", "#ff8000", "#ffff00", "#00c000", "#00ffff", "#0000ff", "#8000ff", "#ff00ff",
                  "#000000", "#444444", "#888888", "#bbbbbb", "#eeeeee", "#ffffff"))
    names = "".join("<td><small>%s</small></td>" % n for n in
                    ("red", "orng", "yell", "grn", "cyan", "blue", "purp", "mag", "blk", "44", "88", "bb", "ee", "wht"))
    with open(os.path.join(out, "index.html"), "w") as f:
        f.write("""<!DOCTYPE html>
<html><head><title>Links on PsiLinux</title></head>
<body>
<h2>Links on the Psion</h2>
<table cellpadding=2><tr><td valign=top><img src="photo.jpg" width=240 height=156 alt="A photo"></td>
<td valign=top>
<p><b>Links 2.30</b> in graphics mode, on <a href="about.html">PsiLinux</a>'s X desktop:
a Linux web browser running on the 5mx itself. Pictures: <a href="pics.html">JPEG, PNG and GIF</a>.
Secure pages: <a href="https://example.com/">TLS through Mbed TLS</a>.</p>
<ul>
<li><a href="pics.html">Pictures</a> &middot; <a href="colours.html">Colours</a> &middot; <a href="form.html">A form</a></li>
<li>Text in <i>italics</i>, <b>bold</b> and <tt>monospaced</tt>.</li>
</ul>
</td></tr></table>
<p>This page is served on 127.0.0.1 inside the test; on the Psion it would come over its
own connection, or through PsiProxy.</p>
</body></html>
""")
    with open(os.path.join(out, "colours.html"), "w") as f:
        f.write("""<!DOCTYPE html>
<html><head><title>Colours to greys</title></head>
<body>
<h3>Colours as the Psion's 16 greys</h3>
<table border=1 cellspacing=0 cellpadding=0><tr>%s</tr><tr>%s</tr></table>
<p><font color="#cc0000">Red text</font>, <font color="#008000">green text</font>,
<font color="#0000cc">blue text</font>, <font color="#808080">grey text</font>.</p>
<table border=1 cellpadding=4>
<tr><th bgcolor="#336699"><font color="#ffffff">Heading</font></th><th bgcolor="#336699"><font color="#ffffff">Cell</font></th></tr>
<tr><td bgcolor="#ffffcc">Pale yellow</td><td bgcolor="#ccffcc">Pale green</td></tr>
<tr><td bgcolor="#ffcccc">Pale red</td><td bgcolor="#ccccff">Pale blue</td></tr>
</table>
</body></html>
""" % (sw, names))
    with open(os.path.join(out, "pics.html"), "w") as f:
        f.write("""<!DOCTYPE html>
<html><head><title>Pictures</title></head>
<body>
<h3>Pictures</h3>
<table cellpadding=3><tr>
<td><img src="photo-prog.jpg" alt="progressive JPEG"><br><small>progressive JPEG</small></td>
<td bgcolor="#ffcc66" valign=top><img src="logo.png" alt="PNG"><br><img src="badge.gif" alt="GIF"></td>
<td valign=top><img src="big.jpg" width=200 alt="big JPEG"><br><small>800x520 JPEG shown 200 wide</small></td>
</tr></table>
</body></html>
""")
    with open(os.path.join(out, "form.html"), "w") as f:
        f.write("""<!DOCTYPE html>
<html><head><title>A form</title></head>
<body>
<h3>Search</h3>
<form action="index.html">
<p>Words: <input name=q size=24 value="psion 5mx"> <input type=submit value="Search"></p>
<p><input type=checkbox name=c checked> Pictures &nbsp; <input type=radio name=r checked> Web
<input type=radio name=r> News &nbsp; <select name=s><option>English<option>Deutsch</select></p>
<p><textarea name=t rows=3 cols=40>A text area.</textarea></p>
</form>
</body></html>
""")
    with open(os.path.join(out, "sizes.html"), "w") as f:
        rows = "".join('<font size=%d>%d: Psion 5mx <b>bold</b> <tt>mono</tt></font><br>\n' % (n, n) for n in range(1, 8))
        f.write("<html><head><title>Type sizes</title></head><body>\n%s</body></html>\n" % rows)
    with open(os.path.join(out, "about.html"), "w") as f:
        f.write("<html><head><title>About</title></head><body><h3>PsiLinux</h3>"
                "<p>Modern Linux on a 1999 Psion Series 5mx.</p></body></html>\n")


if __name__ == "__main__":
    main()
