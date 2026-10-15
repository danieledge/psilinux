#!/usr/bin/env python3
"""ptydrive.py ROOT LOG STEP... -- PROGRAM ARGS...

Runs PROGRAM on the host harness (qemu-arm -cpu ti925t ./psihost -r ROOT, run
from build/linux) in a pseudo-terminal, as a person at the Psion would: each
STEP is 'send:TEXT' (with \\r \\t \\x03 \\x1b escapes), 'sleep:SECONDS' or
'wait:REGEX[:SECONDS]' (for output after the step before; multi-line, so ^
is a line's start). Everything printed goes to LOG. Prints OK, or FAILED if
a wait timed out. (linux/tests/run-alpine.sh)"""
import os, pty, sys, time, re, select
a = sys.argv[1:]
root, log = a[0], a[1]
i = a.index('--')
steps, cmd = a[2:i], a[i+1:]
env = dict(os.environ)
pid, fd = pty.fork()
if pid == 0:
    os.execvpe('qemu-arm', ['qemu-arm', '-cpu', 'ti925t', './psihost', '-r', root] + cmd, env)
if os.environ.get('PTY_SIZE'):     # rows x cols, e.g. 27x110
    import fcntl, termios, struct
    r, c = os.environ['PTY_SIZE'].split('x')
    fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack('HHHH', int(r), int(c), 0, 0))
buf = b''
def pump(t):
    global buf
    end = time.time() + t
    while time.time() < end:
        r, _, _ = select.select([fd], [], [], 0.2)
        if r:
            try: d = os.read(fd, 65536)
            except OSError: return False
            if not d: return False
            buf += d
    return True
ok = True
for s in steps:
    k, _, v = s.partition(':')
    if k == 'send':
        os.write(fd, v.encode().decode('unicode_escape').encode('latin1'))
    elif k == 'sleep':
        pump(float(v))
    elif k == 'wait':
        rx, _, secs = v.rpartition(':') if re.search(r':\d+$', v) else (v, '', '120')
        t0 = time.time(); start = len(buf)
        while time.time() - t0 < float(secs):
            if re.search(rx.encode(), buf[start:], re.M): break
            if not pump(0.5): break
        else:
            print('TIMEOUT waiting for', rx); ok = False
        print('waited %.1fs for %s' % (time.time() - t0, rx))
pump(2)
try: os.kill(pid, 9)
except: pass
open(log, 'wb').write(buf)
print('OK' if ok else 'FAILED')
