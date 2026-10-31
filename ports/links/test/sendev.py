#!/usr/bin/env python3
"""Sends Linux input events to Xfbdev's test devices (fifos given with
-keybd psi,,device=... and -mouse psi,,device=...).
    sendev.py keys FIFO KEY...     KEY: a name below, a letter, or a Linux code
    sendev.py tap FIFO X Y         a pen tap at X, Y"""
import struct, sys, time
NAMES = {"esc": 1, "enter": 28, "tab": 15, "space": 57, "bksp": 14, "up": 103, "down": 108,
         "left": 105, "right": 106, "pgup": 104, "pgdn": 109, "home": 102, "end": 107,
         "f9": 67, "f10": 68, "ctrl": 29, "shift": 42, "alt": 56}
LETTERS = {c: k for k, c in enumerate("1234567890", 2)}
for row, start in (("qwertyuiop", 16), ("asdfghjkl", 30), ("zxcvbnm", 44)):
    for i, c in enumerate(row):
        LETTERS[c] = start + i
LETTERS.update({".": 52, "/": 53, ":": 39, "-": 12})   # (":" is shift+;)
def ev(f, t, c, v): f.write(struct.pack('<IIHHi', 0, 0, t, c, v))
kind, path = sys.argv[1], sys.argv[2]
with open(path, 'wb', buffering=0) as f:
    if kind == "keys":
        for k in sys.argv[3:]:
            mods = []
            while "+" in k and len(k) > 1:            # e.g. ctrl+r
                m, k = k.split("+", 1)
                mods.append(NAMES[m])
            if k == ":": mods.append(42)
            code = NAMES.get(k) or LETTERS.get(k) or int(k)
            for m in mods: ev(f, 1, m, 1); ev(f, 0, 0, 0)
            ev(f, 1, code, 1); ev(f, 0, 0, 0); ev(f, 1, code, 0); ev(f, 0, 0, 0)
            for m in mods: ev(f, 1, m, 0); ev(f, 0, 0, 0)
            time.sleep(0.15)
    else:
        x, y = int(sys.argv[3]), int(sys.argv[4])
        ev(f, 3, 0, x); ev(f, 3, 1, y); ev(f, 1, 0x14a, 1); ev(f, 0, 0, 0)
        time.sleep(0.1); ev(f, 1, 0x14a, 0); ev(f, 0, 0, 0)
