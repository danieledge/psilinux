#!/usr/bin/env python3
"""nbrun.py - runs the netBook emulator on the test image (nbimage.mts) and
types into PsiLinux.

  python3 tests/emu/netbook/nbrun.py OS.IMG OUTDIR STEP...

STEP: "@T" (the clock: simulated seconds after the image is attached; it
starts at 150, when PsiLinux is up), "+S" (wait S seconds), "=NAME" (a
screenshot now, OUTDIR/NAME.png), "{enter}" "{esc}" "{menu}" (keys), or
text to type (letters, digits, space . / - & and Enter as \\r). The test
root has scripts in /root (t1, t2...) so that only those are needed.
Each key moves the clock 0.4 s. Needs ../psionEmulators (or $PSION_EMU).
One frame in five is kept as kNNN.png (NB_EVERY=1: every one).
"""
import os, subprocess, sys
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
EMU = os.environ.get('PSION_EMU', os.path.abspath(os.path.join(HERE, '../../../../psionEmulators')))
SHIFT = 18
SIGNS = {' ': 5, '.': 122, ',': 121, '/': 123, '-': 130, '=': 131, '\r': 3}
SHIFTED = {'&': ord('7')}

def main():
    img, out = sys.argv[1:3]
    os.makedirs(out, exist_ok=True)
    t, args, shots = 150.0, [], []
    def key(code, frames=3):
        args.extend(['--press-key', '%.2f' % t, str(code), str(frames)])
    for st in sys.argv[3:]:
        if st.startswith('@'): t = float(st[1:]); continue
        if st.startswith('+') and st[1:].replace('.', '').isdigit(): t += float(st[1:]); continue
        if st.startswith('='): shots.append((t, st[1:])); continue
        named = {'{enter}': 3, '{esc}': 4, '{menu}': 148}
        if st in named: key(named[st]); t += 0.6; continue
        for ch in st.replace('\\r', '\r'):
            if ch in SIGNS: key(SIGNS[ch])
            elif ch in SHIFTED: key(SHIFT, 24); t += 0.08; key(SHIFTED[ch])
            elif ch.isupper(): key(SHIFT, 24); t += 0.08; key(ord(ch))
            elif ch.isalnum(): key(ord(ch.upper()))
            else: raise SystemExit('cannot type %r' % ch)
            t += 0.4
    end = max([t] + [s[0] for s in shots]) + 3
    env = dict(os.environ, PSION_NB_HOOK_MASK=os.environ.get('PSION_NB_HOOK_MASK', '0xdf'), HARNESS_RGB='1')
    cmd = ['./harness/run', 'roms/netBook/BootLoader/netBook_BL_v011_eng/netBook_BL_v011_eng.bin',
           '--device', 'netbook', '--boot-seconds', '3', '--card-path', os.path.abspath(img),
           '--post-attach-seconds', '%.1f' % end, '--screenshot-every', '1', os.path.join(os.path.abspath(out), 'f')] + args
    with open(os.path.join(out, 'log.txt'), 'w') as log:
        subprocess.run(cmd, cwd=EMU, stdout=log, stderr=subprocess.STDOUT, env=env)
    frames = sorted(f for f in os.listdir(out) if f.startswith('f-') and f.endswith('.pgm'))
    if not frames: raise SystemExit('no frames: see %s/log.txt' % out)
    for ts, name in shots:
        k = min(len(frames) - 1, max(0, int(round(ts)) - 1))
        Image.open(os.path.join(out, frames[k])).convert('RGB').save(os.path.join(out, name + '.png'))
    Image.open(os.path.join(out, frames[-1])).convert('RGB').save(os.path.join(out, 'end.png'))
    # keep one frame in ten, as PNGs, to look through
    for i, f in enumerate(frames):
        if i % int(os.environ.get('NB_EVERY', '5')) == 0: Image.open(os.path.join(out, f)).convert('RGB').save(os.path.join(out, 'k%03d.png' % (i + 1)))
        os.remove(os.path.join(out, f))
    print('%d frames; shots: %s' % (len(frames), ', '.join(s[1] for s in shots)))

main()
