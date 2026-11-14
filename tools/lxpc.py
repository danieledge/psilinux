#!/usr/bin/env python3
"""lxpc.py - names the runner's functions in a PsiLinux.log fault report.

    tools/lxpc.py PsiLinux.log [build/lxrun-obj/psilxrun.map]

For each "the kernel thread faulted" report (0.29 on) it takes the address
of E32Main logged with it, and names the function each address in the report
lies in (pc, lr, and every word on the stack that falls in the runner's
code), from the map the same build made. The map must be from the build that
ran: keep build/lxrun-obj/psilxrun.map with each .sis given out.
"""
import bisect, re, sys

def load_map(path):
    syms = []
    for line in open(path, errors='replace'):
        m = re.match(r'\s+0x([0-9a-f]{8})\s+(\S.*)$', line)
        if m and not m.group(2).startswith('0x'):
            syms.append((int(m.group(1), 16), m.group(2).strip()))
    syms.sort()
    return syms

def name(syms, keys, a):
    i = bisect.bisect_right(keys, a) - 1
    if i < 0:
        return None
    return '%s+0x%x' % (syms[i][1], a - syms[i][0])

def main():
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(2)
    log = open(sys.argv[1], errors='replace').read().splitlines()
    syms = load_map(sys.argv[2] if len(sys.argv) > 2 else 'build/lxrun-obj/psilxrun.map')
    keys = [s[0] for s in syms]
    e32 = [s[0] for s in syms if s[1].startswith('E32Main')]
    if not e32:
        sys.exit('no E32Main in the map')
    lo, hi = syms[0][0], syms[-1][0]
    delta = None
    for line in log:
        m = re.search(r'the kernel thread faulted: .* at pc ([0-9a-f]+) .*lr ([0-9a-f]+), .*E32Main is at ([0-9a-f]+)', line)
        if m:
            delta = int(m.group(3), 16) - e32[0]
            print(line)
            for what, v in (('pc', m.group(1)), ('lr', m.group(2))):
                print('  %s %s: %s' % (what, v, name(syms, keys, int(v, 16) - delta)))
            continue
        if delta is not None and line.startswith('  sp+'):
            for w in line.split(':', 1)[1].split():
                a = int(w, 16) - delta
                if lo <= a <= hi:
                    print('  stack %s: %s' % (w, name(syms, keys, a)))

if __name__ == '__main__':
    main()
