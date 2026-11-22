#!/usr/bin/env python3
"""waittest.py - (0.50, RR-05) the epoc command's wait for a program's start,
on the host: the host build of epoc.c talks to a pretend /dev/epoc (a
pseudo-terminal in raw mode, EPOC_DEVICE), which answers RUN, OPEN, TASKS
and ALIVE as each case needs.
  make -C epoc test        (or: python3 epoc/waittest.py build/epoc/epoc-host)
"""
import os, pty, select, struct, subprocess, sys, termios, time, tty

EPOC = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(__file__), '..', 'build', 'epoc', 'epoc-host')
MAGIC, HDR = 0x434f5045, struct.Struct('<IHHIiiII')
OP_OPEN, OP_RUN, OP_TASKS, OP_ALIVE = 16, 17, 18, 34
OK, FAILED, NOTFOUND, BUSY = 0, 1, 3, 6
F_BG = 16
checks = fails = 0


def check(cond, what):
    global checks, fails
    checks += 1
    if not cond:
        fails += 1
        print('FAIL', what)


def run(args, app):
    """epoc ARGS against app(op, flags, arg, text) -> (status, arg, text);
    (exit code, seconds, the ops asked, stderr)"""
    master, slave = pty.openpty()
    tty.setraw(slave)
    attrs = termios.tcgetattr(slave)
    attrs[3] &= ~termios.ECHO
    termios.tcsetattr(slave, termios.TCSANOW, attrs)
    env = dict(os.environ, EPOC_DEVICE=os.ttyname(slave))
    t0 = time.time()
    p = subprocess.Popen([EPOC] + args, env=env, stderr=subprocess.PIPE, stdout=subprocess.DEVNULL)
    ops = []
    while p.poll() is None:
        r, _, _ = select.select([master], [], [], 0.05)
        if not r:
            continue
        d = os.read(master, 20000)
        if len(d) < HDR.size:
            continue
        magic, ver, op, flags, st, err, arg, n = HDR.unpack(d[:HDR.size])
        ops.append(op)
        status, rarg, text = app(op, flags, arg, d[HDR.size:HDR.size + n])
        body = text.encode()
        os.write(master, HDR.pack(MAGIC, 1, op, 0, status, 0, rarg, len(body)) + body)
    took = time.time() - t0
    errs = p.stderr.read().decode()
    os.close(master)
    os.close(slave)
    return p.returncode, took, ops, errs


def starting(listed_after, fg=True, tid=101, answer='0x1000007F\tWord\tstarted\n', alive=True):
    """A program that is on the task list after LISTED_AFTER looks"""
    looks = [0]

    def app(op, flags, arg, text):
        if op in (OP_RUN, OP_OPEN):
            return OK, tid, answer
        if op == OP_TASKS:
            looks[0] += 1
            word = '%d\t0x1000007F\tWord\t\t%s\n' % (tid, 'fg' if fg else '')
            lx = '102\t0x01000A87\tPsiLinux\t\t%s\n' % ('' if fg else 'fg')
            return OK, 0, (word if looks[0] > listed_after else '') + lx
        if op == OP_ALIVE:
            return (OK if alive else NOTFOUND), 0, ''
        return FAILED, 0, 'not in this test'
    return app


# the standard: until the program is on the task list, in front
rc, took, ops, errs = run(['run', 'Word'], starting(3))
check(rc == 0 and ops.count(OP_TASKS) == 4, 'run waits until Word is listed and in front (%d, %r, %s)' % (rc, ops, errs))
check(0.5 < took < 3, 'run: about 3 looks 0.2 s apart (%.2f s)' % took)
# --no-wait: no look at all
rc, took, ops, errs = run(['run', '--no-wait', 'Word'], starting(3))
check(rc == 0 and ops == [OP_RUN], 'run --no-wait returns at once (%d, %r)' % (rc, ops))
# --bg: on the list is enough
rc, took, ops, errs = run(['run', '--bg', 'Word'], starting(1, fg=False))
check(rc == 0 and ops.count(OP_TASKS) == 2 and took < 1.5, 'run --bg: listed, behind (%d, %r, %.2f s)' % (rc, ops, took))
# listed but never in front (another program came first): 2 s more, then done
rc, took, ops, errs = run(['run', 'Word'], starting(0, fg=False))
check(rc == 0 and 1.9 < took < 4, 'run: on the list but not in front: done after 2 s (%d, %.2f s)' % (rc, took))
# a program already running: switched to, no wait
rc, took, ops, errs = run(['run', 'Word'], starting(3, answer='0x1000007F\tWord\tswitched\n'))
check(rc == 0 and ops == [OP_RUN], 'run of a running program: no wait (%d, %r)' % (rc, ops))
# an .exe: no window to wait for
rc, took, ops, errs = run(['run', 'C:\\Thing.exe'], starting(3, answer='C:\\Thing.exe\n'))
check(rc == 0 and ops == [OP_RUN], 'run of an .exe: no wait (%d, %r)' % (rc, ops))
# never shows: --timeout
rc, took, ops, errs = run(['--timeout', '1', 'run', 'Word'], starting(10 ** 6))
check(rc == BUSY and 'not started after 1 s' in errs and 0.9 < took < 2.5, 'run --timeout 1: gives up, exit 6 (%d, %.2f s, %s)' % (rc, took, errs.strip()))
# ends as it starts
rc, took, ops, errs = run(['run', 'Word'], starting(10 ** 6, alive=False))
check(rc == FAILED and 'ended as it started' in errs, 'run: a program that ends as it starts, exit 1 (%d, %s)' % (rc, errs.strip()))
# open: the same wait
rc, took, ops, errs = run(['open', 'C:\\Letter.txt'], starting(2, answer='0x1000007F\tWord\n'))
check(rc == 0 and ops.count(OP_TASKS) == 3, 'open waits for the program too (%d, %r)' % (rc, ops))
rc, took, ops, errs = run(['open', '--no-wait', 'C:\\Letter.txt'], starting(2, answer='0x1000007F\tWord\n'))
check(rc == 0 and ops == [OP_OPEN], 'open --no-wait (%d, %r)' % (rc, ops))
print('waittest: %d checks, %d failed' % (checks, fails))
sys.exit(1 if fails else 0)
