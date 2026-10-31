#!/usr/bin/env python3
"""The memory a program held, from qemu-arm -strace: the anonymous mappings
(mmap/munmap/mremap) and the heap (brk), live and at most.
    memlog.py STRACE.log [MARK...]   MARK: a byte offset in the log: the live
                                     figure there is printed too"""
import re, sys
live = peak = 0
maps = {}
brk0 = brk = None
marks = sorted(int(m) for m in sys.argv[2:])
at = {}
pos = 0
for line in open(sys.argv[1], errors="replace"):
    pos += len(line.encode("utf-8", "replace"))
    while marks and pos >= marks[0]:
        at[marks.pop(0)] = live
    m = re.search(r" (mmap2?)\((0x[0-9a-f]+|NULL),(\d+),[^,]*,([^,]*),(-?\d+),[^)]*\) = (0x[0-9a-f]+)", line)
    if m and "MAP_ANON" in m.group(4):
        a, n = int(m.group(6), 16), int(m.group(3))
        maps[a] = n; live += n
    m = re.search(r" munmap\((0x[0-9a-f]+),(\d+)\) = 0", line)
    if m:
        a, n = int(m.group(1), 16), int(m.group(2))
        if a in maps:
            live -= maps.pop(a)
        else:   # (a part of one)
            for b, l in list(maps.items()):
                if b <= a < b + l:
                    cut = min(n, b + l - a)
                    maps[b] = a - b
                    if a + cut < b + l: maps[a + cut] = b + l - a - cut
                    live -= cut
                    break
    m = re.search(r" mremap\((0x[0-9a-f]+),(\d+),(\d+),[^)]*\) = (0x[0-9a-f]+)", line)
    if m:
        a, o, n, r = int(m.group(1), 16), int(m.group(2)), int(m.group(3)), int(m.group(4), 16)
        if a in maps:
            live += n - maps.pop(a); maps[r] = n
    m = re.search(r" brk\((0x[0-9a-f]+|NULL)\) = (0x[0-9a-f]+)", line)
    if m:
        r = int(m.group(2), 16)
        if brk0 is None: brk0 = r
        if brk is not None: live += r - brk
        else: live += r - brk0
        brk = r
    peak = max(peak, live)
for k in sorted(at):
    print("at %d: %d KB" % (k, at[k] // 1024))
print("end: %d KB live, peak %d KB" % (live // 1024, peak // 1024))
