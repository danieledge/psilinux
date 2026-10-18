#!/bin/sh
# run-host.sh - every PsiLinux test suite on the host harness (psihost under
# qemu-arm), with the root image just built.
#   linux/tools/lxbuild "linux/tests/run-host.sh"
set -e
REPO=$(cd "$(dirname "$0")/.." && pwd)
make -C "$REPO/runtime" host > /dev/null
R=$REPO/build/linux/test-root
rm -rf "$R"; mkdir -p "$R"
cp "$REPO"/build/psilinux-root/ROOT.I* "$R/"     # (the image and its parts)
cd "$REPO/build/linux"
fail=0
for t in m1test m2test.sh m4test.sh; do
	echo "== $t"
	case $t in *.sh) cmd="/bin/sh /usr/lib/psilinux/tests/$t" ;; *) cmd="/usr/lib/psilinux/tests/$t" ;; esac
	timeout 600 qemu-arm ./psihost -r test-root $cmd < /dev/null > "$t.log" 2>&1 || true
	grep -E "FAIL|passed" "$t.log" || { echo "(no result: see build/linux/$t.log)"; fail=1; }
	grep -q " 0 failed" "$t.log" || fail=1
done
exit $fail
