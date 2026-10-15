#!/usr/bin/env python3
"""fuzzdiff.py PROGRAM.S NATIVE DBT - the first cases whose results differ."""
import struct, sys, re
src, a, b = sys.argv[1:4]
A, B = open(a, 'rb').read(), open(b, 'rb').read()
REC = 11 * 4 + 4 + 4 + 16 * 8 + 4 + 4 + 64          # r1-r9 r11 r12, flags, fpscr, d0-d15, r0, r10, data
names = ['r1', 'r2', 'r3', 'r4', 'r5', 'r6', 'r7', 'r8', 'r9', 'r11', 'r12', 'flags', 'fpscr'] + ['s%d' % i for i in range(32)] + ['r0', 'r10'] + ['data%d' % i for i in range(16)]
cases = re.split(r'@ case \d+: ', open(src).read())[1:]
if len(A) != len(B): print('lengths differ: native %d, translated %d' % (len(A), len(B)))
shown = 0
for n in range(min(len(A), len(B)) // REC):
    x, y = A[n * REC:(n + 1) * REC], B[n * REC:(n + 1) * REC]
    if x == y: continue
    wa, wb = struct.unpack('<%dI' % (REC // 4), x), struct.unpack('<%dI' % (REC // 4), y)
    body = [l.strip() for l in cases[n].split('\n')[1:] if l.strip() and not l.strip().startswith(('ldr r', 'str r11', 'vmov s', 'msr cpsr_f', 'vmsr fpscr, r11'))]
    k = cases[n].split('\n')[0]
    i0 = body.index('mov r10, #0') + 1 if 'mov r10, #0' in body else 0
    print('case %d (%s): %s' % (n, k, ' ; '.join(body[i0:i0 + 8])))
    for i, (p, q) in enumerate(zip(wa, wb)):
        if p != q: print('   %-6s native %08x  translated %08x' % (names[i], p, q))
    shown += 1
    if shown >= 6: break
if len(A) < len(B) or len(B) < len(A):
    n = min(len(A), len(B)) // REC
    print('the shorter one stops in case %d: %s' % (n, cases[n].split('\n')[0] if n < len(cases) else '?'))
