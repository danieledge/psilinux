#!/bin/sh
# roottest.sh - (0.42) a damaged ROOT.IMG is refused with a clear message,
# not read: each case below changes one number of the index in a copy of
# the root image just built, then psihost must say "the root image is
# damaged" (exit 126); the image as built still runs.
#   tools/lxbuild "tests/roottest.sh"
set -e
REPO=$(cd "$(dirname "$0")/.." && pwd)
make -C "$REPO/runtime" host > /dev/null
B=$REPO/build/linux
T=$B/root-test
cd "$B"
pass=0; fail=0
run() {   # name, then the python that changes h (the header) or e(i, field, value)
	rm -rf "$T"; mkdir -p "$T"
	cp "$REPO"/build/psilinux-root/ROOT.I* "$T/"
	python3 - "$T/ROOT.IMG" "$2" <<'PY'
import struct, sys
p, code = sys.argv[1], sys.argv[2]
b = bytearray(open(p, 'rb').read())
n, eoff, noff, nsz = struct.unpack_from('<IIII', b, 8)
F = dict(parent=0, name=1, mode=2, size=3, data=4, mtime=5, first=6, count=7)
def get(i, f): return struct.unpack_from('<I', b, eoff + i * 32 + F[f] * 4)[0]
def e(i, f, v): struct.pack_into('<I', b, eoff + i * 32 + F[f] * 4, v & 0xffffffff)
def h(off, v): struct.pack_into('<I', b, off, v & 0xffffffff)
def find(kind):   # the first entry of a kind (0o04 dir, 0o10 file, 0o12 link)
	for i in range(1, n):
		if get(i, 'mode') >> 12 == kind: return i
exec(code)
if code == 'cut': b = b[:eoff + 100]
open(p, 'wb').write(b)
PY
	out=$(LX_DRIVE_C=$T timeout 120 qemu-arm ./psihost -r "$T" /bin/true 2>&1 < /dev/null); r=$?
	if [ "$3" = ok ]; then want=0; else want=126; fi
	if [ $r = $want ] && { [ $want = 0 ] || echo "$out" | grep -q "the root image is damaged"; }; then
		pass=$((pass+1)); echo "ok   $1: $(echo "$out" | grep -o 'damaged.*' | head -1)"
	else fail=$((fail+1)); echo "FAIL $1: exit $r: $(echo "$out" | tail -2)"; fi
}
set +e
run "the image as built" "pass" ok
run "a count that wraps" "h(8, 0x08000001)"
run "entries past the end" "h(12, 0x7fffff00)"
run "names past the end" "h(16, 0x7fffff00)"
run "names too long" "h(20, 0xfffffff0)"
run "the root not a folder" "e(0, 'mode', 0o100644)"
run "a name out of range" "e(find(0o10), 'name', 0x7fffffff)"
run "a parent out of range" "e(find(0o10), 'parent', n + 5)"
run "a folder's children out of range" "e(0, 'count', n)"
run "a folder's first past the end" "d = find(0o04); e(d, 'first', 0xfffffff0)"
run "children of the wrong folder" "d = find(0o04); e(get(d, 'first'), 'parent', 0) if get(d, 'count') else None"
run "a folder not in order" "c = get(0, 'first'); a, z = get(c, 'name'), get(c + 1, 'name'); e(c, 'name', z); e(c + 1, 'name', a)"
run "a file past the image" "e(find(0o10), 'data', 0xfffffff0)"
run "a file's size past the image" "e(find(0o10), 'size', 0x7ffffff0)"
run "a link's target out of range" "e(find(0o12), 'data', 0x7fffffff)"
run "an entry of no kind" "e(find(0o10), 'mode', 0o070644)"
echo "roottest: $pass passed, $fail failed"
[ $fail = 0 ]
