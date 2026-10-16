#!/usr/bin/env python3
"""sisparts.py - the .pkg files for a root image in parts (DECISIONS.md 54).

    sisparts.py DIR MAIN.pkg TARGET UID2,UID3,... [MB]

The EPOC R5 installer cannot open a very large .sis, whatever is in it
(0.21's 25.9 MB one: "Not enough memory"; 0.20's 7.9 MB one installs). DIR holds ROOT.IMG and its
parts (ROOT.I01, ... from mkroot.py --parts); they are shared out, in order,
among .sis files of at most MB megabytes (standard 6). The first is MAIN.pkg
(its header, with ROOT.IMG) plus the parts that fit; each further one gets
its own UID, the name "<main's name> (part N of M)", the main's version, and
the main as a requisite. TARGET is the folder on the Psion, e.g.
'!:\\Linux\\Alpine\\'. Writes DIR/part1.pkg ... and prints their names.
"""
import os, re, sys

def main():
    if len(sys.argv) < 5:
        print(__doc__)
        sys.exit(2)
    d, main_pkg, target, uids = sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4].split(',')
    limit = int(float(sys.argv[5]) * 1024 * 1024) if len(sys.argv) > 5 else 6 * 1024 * 1024
    files = ['ROOT.IMG'] + sorted(f for f in os.listdir(d) if re.fullmatch(r'ROOT\.I\d\d', f))
    groups, cur, size = [], [], 0
    for f in files:
        n = os.path.getsize(os.path.join(d, f))
        if cur and size + n > limit:
            groups.append(cur)
            cur, size = [], 0
        cur.append(f)
        size += n
    groups.append(cur)
    if len(groups) - 1 > len(uids):
        sys.exit('sisparts.py: %d .sis files need %d more UIDs' % (len(groups), len(groups) - 1 - len(uids)))
    head = open(main_pkg).read()
    m = re.search(r'#\{"([^"]+)"\},\((0x[0-9A-Fa-f]+)\),(\d+),(\d+),(\d+)', head)
    name, uid, major, minor, variant = m.group(1), m.group(2), m.group(3), m.group(4), m.group(5)
    # (the main's own line for ROOT.IMG is in its template)
    main_lines = [l for l in head.splitlines() if not l.startswith('"ROOT.IMG"')]
    total = len(groups)
    for k, g in enumerate(groups, 1):
        out = os.path.join(d, 'part%d.pkg' % k)
        with open(out, 'w', newline='\n') as f:
            if k == 1:
                f.write('\n'.join(main_lines) + '\n')
            else:
                f.write('; %s, part %d of %d: more of the root image (DECISIONS.md 54).\n' % (name, k, total))
                f.write('; Install every part, on the same disk as part 1.\n')
                f.write('&EN\n')
                f.write('#{"%s (part %d of %d)"},(%s),%s,%s,%s\n' % (name, k, total, uids[k - 2], major, minor, variant))
                f.write('(%s),%s,%s,%s,{"%s"}\n' % (uid, major, minor, variant, name))
            for fn in g:
                f.write('"%s"-"%s%s"\n' % (fn, target, fn))
        print(out)

if __name__ == '__main__':
    main()
