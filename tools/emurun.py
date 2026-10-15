#!/usr/bin/env python3
"""emurun.py - runs PsiLinux in the emulator and types into it.

    emurun.py NAME [--card CARD.img] [--at SECONDS] [--wait S] STEP...

Each STEP is text to type (\\r is Enter; ^C is Ctrl+C), "+N" to wait
N seconds of simulated time, or one of the keys {left} {right} {up} {down}
{tab} {tap X Y}. Screenshots go to build/emu/NAME/ (end.png is
the last). It uses tools/emu/run.sh and the key tables of tools/emu/net.py.
"""
import os, subprocess, sys
HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, '..', '..'))
sys.path.insert(0, os.path.join(REPO, 'tools', 'emu'))
import net   # noqa: E402  (Script, the key codes)

def main():
    a = sys.argv[1:]
    if not a:
        print(__doc__)
        sys.exit(2)
    name = a.pop(0)
    card = os.path.join(REPO, 'build', 'emu', 'lxcard.img')
    at, secs = 60.0, 10
    steps = []
    while a:
        x = a.pop(0)
        if x == '--card': card = a.pop(0)
        elif x == '--at': at = float(a.pop(0))
        elif x == '--wait': secs = int(a.pop(0))
        else: steps.append(x.replace('\\r', '\r').replace('\\n', '\r').replace('\\e', '\x1b'))
    s = net.Script(at)
    for st in steps:
        if st.startswith('+') and st[1:].replace('.', '').isdigit():
            s.wait(float(st[1:]))
            continue
        keys = {'{left}': net.LEFT, '{right}': net.RIGHT, '{up}': net.UP, '{down}': net.DOWN, '{tab}': net.TAB}
        if st in keys:
            s.key(keys[st], None, 0.8)
            continue
        if st.startswith('{tap ') and st.endswith('}'):
            x, y = st[5:-1].split()
            s.ev.append('%.2f tap %s %s' % (s.t, x, y))
            s.wait(1)
            continue
        i = 0
        while i < len(st):
            if st[i] == '^' and i + 2 < len(st) and st[i + 1] == '+':
                s.ctrl(st[i + 2], shift=True)    # ^+x: Shift+Ctrl+x
                i += 3
                continue
            if st[i] == '^' and i + 1 < len(st):
                s.ctrl(st[i + 1])
                i += 2
                continue
            if st[i] == '\x1b':
                s.key(net.ESC, None, 0.4)
                i += 1
                continue
            if st[i] == '|':
                s.ctrl('i', shift=True)          # PsiLinux's Shift+Ctrl+I
                i += 1
                continue
            j = min([k for k in (st.find('^', i), st.find('|', i), st.find('\x1b', i)) if k >= 0] or [len(st)])
            s.type(st[i:j], gap=0.25)
            i = j
    env = dict(os.environ, EMU_CARD=card, EMU_SECS=str(secs))
    cmd = [os.path.join(REPO, 'tools', 'emu', 'run.sh'), name] + s.ev
    subprocess.run(cmd, env=env, check=False)

if __name__ == '__main__':
    main()
