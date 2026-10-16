#!/usr/bin/env python3
"""mkroot.py - packs a folder into ROOT.IMG, PsiLinux's read-only root
file system (the format is described in linux/runtime/lx_root.c).

    mkroot.py STAGING_DIR OUT.IMG [--owner-mode] [--parts KB]

--parts KB cuts the image into parts of KB kilobytes: OUT.IMG is the first
(with the header, which records the part size and count), then OUT.I01,
OUT.I02... (the EPOC installer cannot take one very large file: 0.22).

Symbolic links and modes are kept; owners are not (everything is root's).
Children of a folder are stored together, sorted by name (bytes), so the
runner can find a name with a binary search. File data is 4-byte aligned.
"""
import os, stat, struct, sys, time

def main():
    if len(sys.argv) < 3:
        print(__doc__)
        sys.exit(2)
    src, out = sys.argv[1], sys.argv[2]
    # breadth first: every folder's children consecutive
    entries = []          # [name, parent, mode, size, kind, path, link]
    entries.append(['', 0, stat.S_IFDIR | 0o755, 0, 'd', src, None])
    queue = [0]
    first = {}
    count = {}
    while queue:
        i = queue.pop(0)
        path = entries[i][5]
        names = sorted(os.listdir(path), key=lambda n: n.encode())
        first[i] = len(entries)
        count[i] = len(names)
        for n in names:
            p = os.path.join(path, n)
            st = os.lstat(p)
            if stat.S_ISLNK(st.st_mode):
                entries.append([n, i, stat.S_IFLNK | 0o777, 0, 'l', p, os.readlink(p)])
            elif stat.S_ISDIR(st.st_mode):
                entries.append([n, i, stat.S_IFDIR | (st.st_mode & 0o7777), 0, 'd', p, None])
                queue.append(len(entries) - 1)
            elif stat.S_ISREG(st.st_mode):
                entries.append([n, i, stat.S_IFREG | (st.st_mode & 0o7777), st.st_size, 'f', p, None])
    # names (and link targets)
    names = bytearray()
    name_off = []
    link_off = []
    for e in entries:
        name_off.append(len(names))
        names += e[0].encode() + b'\0'
        if e[4] == 'l':
            link_off.append(len(names))
            names += e[6].encode() + b'\0'
        else:
            link_off.append(0)
    n = len(entries)
    eoff = 64
    noff = eoff + n * 32
    doff = (noff + len(names) + 3) & ~3
    data = bytearray()
    data_off = []
    for e in entries:
        if e[4] == 'f':
            with open(e[5], 'rb') as f:
                b = f.read()
            data_off.append(doff + len(data))
            data += b
            while len(data) % 4:
                data.append(0)
        else:
            data_off.append(0)
    now = int(time.time())
    hdr = b'LXROOT01' + struct.pack('<IIIIII', n, eoff, noff, len(names), 0, now)
    hdr += b'\0' * (64 - len(hdr))
    body = bytearray(hdr)
    for i, e in enumerate(entries):
        d = link_off[i] if e[4] == 'l' else data_off[i]
        body += struct.pack('<IIIIIIII', e[1], name_off[i], e[2], e[3], d, now,
                            first.get(i, 0), count.get(i, 0))
    body += names
    while len(body) < doff:
        body.append(0)
    body += data
    base = out[:-4] if out.upper().endswith('.IMG') else out
    for old in __import__('glob').glob(base + '.I[0-9][0-9]'):
        os.remove(old)                     # (parts of an earlier build)
    part = int(sys.argv[sys.argv.index('--parts') + 1]) * 1024 if '--parts' in sys.argv else 0
    if part and len(body) > part:
        nparts = (len(body) + part - 1) // part
        if nparts > 99:
            sys.exit('mkroot.py: more than 99 parts: make them bigger')
        body[32:40] = struct.pack('<II', part, nparts)
        for k in range(nparts):
            name = out if k == 0 else '%s.I%02d' % (base, k)
            with open(name, 'wb') as f:
                f.write(body[k * part:(k + 1) * part])
        print('%s: %d entries, %d KB, in %d parts of %d KB' % (out, n, len(body) // 1024, nparts, part // 1024))
        return
    with open(out, 'wb') as f:
        f.write(body)
    print('%s: %d entries, %d KB' % (out, n, len(body) // 1024))

if __name__ == '__main__':
    main()
