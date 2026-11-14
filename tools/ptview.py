#!/usr/bin/env python3
"""ptview.py - PsiTerm's terminal, as PsiLinux compiles it (0.29).

    tools/ptview.py PSITERM_APP_DIR OUT.cpp OUT.rssi

PsiLinux's terminal view is PsiTerm's: the drawing, the fonts (Terminus and
PsiTerm's other faces, the zoom levels), the themes and colour schemes, the
history, selection, copying and pasting. PsiTerm keeps all of it in one class,
CTermView (app/psiterm.cpp, psiterm.h), together with its SSH session, the
serial line and tmux. Until PsiTerm moves the terminal into a file of its own
(docs/PTVIEW.md: the seam), this takes the terminal's functions out of
PsiTerm's source as they are, by name, at every build:

  - OUT.cpp: PsiTerm's file-scope constants and helpers the terminal uses,
    then the CTermView functions listed in METHODS, word for word. PsiLinux
    compiles it against PsiTerm's own psiterm.h, so the class is PsiTerm's;
    app/lxterm.cpp gives the functions that are PsiLinux's (the session with
    the runner instead of SSH, its status line, its keys) and empty ones for
    tmux's tab strip.
  - OUT.rssi: PsiTerm's theme and cursor names (psiterm.rss), for
    PsiLinux's Preferences, renamed r_lx_pt_*.

If PsiTerm renames or removes one of these, the build stops here, naming it.
"""
import os, re, sys

# CTermView's terminal, from psiterm.cpp (and pttabs.cpp for RowY)
METHODS = [
    # fonts and zoom
    'ZoomLevels', 'ZoomPixels', 'ZoomName', 'ZoomNearest', 'LoadFontFolders', 'ReleaseFont',
    'ZoomBy', 'SetFontL', 'SetFaceL', 'ApplyFontL', 'CurrentFace', 'MeasureFont', 'Layout',
    'SetTermRectL', 'SizeAtZoom',
    # libvterm and drawing
    'FeedTerminal', 'CbDamage', 'CbMoveRect', 'CbPushLine', 'CbClearScrollback', 'CbMoveCursor',
    'CbSetTermProp', 'CbBell', 'CbOutput', 'AddDamage', 'BeginPaint', 'EndPaint', 'Draw',
    'DrawStatus', 'DrawAll', 'DrawScrollIndicator', 'DrawInCell', 'DrawCells', 'GetLook', 'TextByte',
    'ColourRgb', 'SetGreys', 'CellColours', 'DrawOneCell', 'Theme', 'Grey', 'ThemePair', 'DrawCursor',
    # keys into the terminal
    'SendKey', 'SendChar', 'SendCtrl', 'SendString', 'ResetTerminal',
    # history, the pen, the clipboard
    'ScrollBy', 'ScrollTo', 'RepaintRows', 'ClearSelection', 'SelectScreen', 'HandlePointerEventL',
    'SelLineFn', 'SelMapFn', 'CopySelectionL', 'PasteL',
    # the drawing benchmark (Bench.txt beside the program)
    'RunBench',
    # pttabs.cpp
    'RowY',
]

# file-scope names those functions use (constants, tables, helpers)
STATICS = [
    'KFontCourier', 'KFontFile', 'KMaxZoomLevels', 'KCourierPixels', 'KCourierCellW', 'KDefaultZoom',
    'TPtFace', 'KFaceCount', 'KFaces', 'Face', 'KColDefFg', 'KColDefBg', 'KScrollbackLines',
    'KScrollbackCols', 'KClipMax', 'PtColourScreen', 'CopyClipped', 'IsBlankChar', 'KAnsiRgb',
    'XtermRgb', 'CubeStep', 'NearestXterm', 'RgbLum', 'ColourCode', 'TPtColourTheme', 'KMuxThemes',
    'KPtFirstMuxTheme', 'KPtThemeClean', 'KPtColourThemes', 'GreyOf', 'FontCellWidth',
]
OPTIONAL_STATICS = {'FontCellWidth', 'KPtThemeClean', 'CopyClipped'}

# resource arrays for PsiLinux's Preferences
ARRAYS = ['r_pt_theme_array', 'r_pt_theme_colour_array', 'r_pt_cursor_array']


def blocks_of(lines, i):
    """the end (inclusive) of the top-level item starting at line i"""
    first = lines[i].rstrip()
    if first.endswith(';') and first.count('{') == first.count('}'):
        return i
    depth = 0
    j = i
    seen = False
    while j < len(lines):
        code = re.sub(r'//.*$', '', lines[j])
        code = re.sub(r'"(\\.|[^"\\])*"', '""', code)
        code = re.sub(r"'(\\.|[^'\\])*'", "''", code)
        for ch in code:
            if ch == '{':
                depth += 1
                seen = True
            elif ch == '}':
                depth -= 1
        if seen and depth == 0:
            # a function ends at its brace; a table or struct at its ';'
            if code.rstrip().endswith(';') or re.match(r'^\s*\}\s*$', code):
                return j
        if not seen and code.rstrip().endswith(';') and j > i:
            return j
        j += 1
    raise SystemExit('ptview: no end for the item at line %d' % (i + 1))


def comment_before(lines, i):
    """the comment lines just above line i"""
    k = i
    while k > 0 and lines[k - 1].startswith('//'):
        k -= 1
    return k


def find_method(lines, name):
    pat = re.compile(r'^[A-Za-z].*\bCTermView::%s\(' % re.escape(name))
    found = []
    i = 0
    while i < len(lines):
        if pat.match(lines[i]) and not lines[i].rstrip().endswith(';'):
            e = blocks_of(lines, i)
            found.append((comment_before(lines, i), e))
            i = e + 1
            continue
        i += 1
    return found


def find_static(lines, name):
    n = re.escape(name)
    pats = [
        r'^(static\s+)?(const\s+)?[A-Za-z_][\w:<>]*[\s*&]+%s\s*(\[|=|\()' % n,
        r'^(static\s+)?(inline\s+)?(const\s+)?[A-Za-z_][\w:<>]*\s*[*&]?\s*%s\s*\(' % n,
        r'^struct\s+%s\b' % n,
        r'^_LIT8?\(%s,' % n,
    ]
    for i, l in enumerate(lines):
        if any(re.match(p, l) for p in pats) and not (l.rstrip().endswith(');') and '(' in l and '=' not in l and not l.startswith('_LIT')):
            return (comment_before(lines, i), blocks_of(lines, i))
    return None


def main():
    if len(sys.argv) != 4:
        print(__doc__)
        sys.exit(2)
    app, out_cpp, out_rssi = sys.argv[1:]
    srcs = {}
    for f in ('psiterm.cpp', 'pttabs.cpp'):
        srcs[f] = open(os.path.join(app, f), encoding='latin-1').read().split('\n')
    main_src = srcs['psiterm.cpp']
    parts = []
    missing = []
    # the statics, in the order they come in psiterm.cpp
    spans = []
    for s in STATICS:
        r = find_static(main_src, s)
        if r:
            spans.append(r)
        elif s not in OPTIONAL_STATICS:
            missing.append(s)
    spans = sorted(set(spans))
    merged = []
    for a, b in spans:
        if merged and a <= merged[-1][1]:
            merged[-1] = (merged[-1][0], max(b, merged[-1][1]))
        else:
            merged.append((a, b))
    for a, b in merged:
        parts.append('// psiterm.cpp:%d\n' % (a + 1) + '\n'.join(main_src[a:b + 1]))
    for m in METHODS:
        got = []
        for f, src in srcs.items():
            for a, b in find_method(src, m):
                got.append('// %s:%d\n' % (f, a + 1) + '\n'.join(src[a:b + 1]))
        if not got:
            missing.append('CTermView::' + m)
        parts.extend(got)
    if missing:
        raise SystemExit('ptview: not found in PsiTerm (%s): %s' % (app, ', '.join(missing)))
    head = ('// GENERATED by tools/ptview.py from PsiTerm\'s %s: do not edit.\n'
            '// PsiTerm\'s terminal (CTermView\'s drawing, fonts, themes, history and\n'
            '// selection), compiled into PsiLinux against PsiTerm\'s own psiterm.h.\n'
            '#include "lxptview.h"\n\n') % 'app/psiterm.cpp'
    open(out_cpp, 'w', encoding='latin-1').write(head + '\n\n'.join(parts) + '\n')
    # the resource arrays
    rss = open(os.path.join(app, 'psiterm.rss'), encoding='latin-1').read()
    out = ['// GENERATED by tools/ptview.py from PsiTerm\'s app/psiterm.rss: do not edit.']
    for a in ARRAYS:
        m = re.search(r'RESOURCE ARRAY %s\s*\{.*?\n\t\}\n' % a, rss, re.S)
        if not m:
            raise SystemExit('ptview: no %s in psiterm.rss' % a)
        out.append(m.group(0).replace(a, a.replace('r_pt_', 'r_lx_pt_')))
    open(out_rssi, 'w', encoding='latin-1').write('\n'.join(out) + '\n')
    print('ptview: %d functions and %d constants and helpers from PsiTerm' % (sum(1 for p in parts if 'CTermView::' in p.split('\n')[1] or 'CTermView::' in p), len(merged)))


if __name__ == '__main__':
    main()
