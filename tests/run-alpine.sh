#!/bin/bash
# run-alpine.sh - Alpine mode's suites on the host harness (psihost under
# qemu-arm -cpu ti925t, an ARMv4T as the 5mx), with the Alpine root just built
# (linux/alpine/build-root.sh):
#   - m2test.sh under BusyBox sh and under bash (46 checks each)
#   - bashtest.sh: what bash has beyond sh
#   - bash interactive, in a pty: job control, Ctrl+C, Ctrl+Z, Tab, history
#   - the memory reserve, with a pretend Psion short of memory
#   linux/tools/lxbuild "linux/tests/run-alpine.sh"
set -e
REPO=$(cd "$(dirname "$0")/.." && pwd)
make -C "$REPO/runtime" host > /dev/null
cd "$REPO/build/linux"
R=alpine-test
fresh() {   # (the image and its parts: ROOT.I01...)
	rm -rf $R; mkdir -p $R
	for f in "$REPO"/build/alpine-root/ROOT.I*; do ln "$f" $R/ 2>/dev/null || cp "$f" $R/; done
}
fail=0
run() {   # name, then the program and its arguments
	local n=$1; shift
	echo "== $n"
	fresh
	LX_DRIVE_T=$REPO/tests timeout 900 qemu-arm -cpu ti925t ./psihost -r $R "$@" < /dev/null > "$n.log" 2>&1 || true
	grep -E "^FAIL|passed" "$n.log" || { echo "(no result: see build/linux/$n.log)"; fail=1; }
	grep -q " 0 failed" "$n.log" || fail=1
}
run m2-ash /bin/sh /t/m2test.sh
run m2-bash /bin/bash /t/m2test.sh
run bashtest /bin/bash /t/bashtest.sh

echo "== bash, interactive"
fresh
python3 "$REPO/tests/ptydrive.py" $R bash-pty.log 'wait:# :300' \
	'send:ls /etc | head -3 | wc -l\r' 'wait:\r3\r' \
	'send:sleep 30\r' 'sleep:5' 'send:\x03' 'wait:# :60' 'send:echo int=$?\r' 'wait:int=130' \
	'send:ls /etc/alpine-rel\t\r' 'wait:/etc/alpine-release' \
	'send:echo hist\r' 'wait:# ' 'send:\x1b[A\r' 'wait:hist' \
	'send:sleep 100 &\r' 'wait:# :60' 'send:jobs\r' 'wait:Running' 'send:kill %1\r' 'wait:# ' \
	'send:sleep 40\r' 'sleep:5' 'send:\x1a' 'wait:Stopped:60' 'send:fg\r' 'sleep:3' 'send:\x03' 'wait:# :60' \
	'send:read -p "name? " n\r' 'wait:name' 'send:psion\r' 'wait:# ' 'send:echo got=$n\r' 'wait:got=psion' \
	'send:exit\r' 'sleep:3' -- /bin/bash -il > bash-pty.out 2>&1 || true
cat bash-pty.out | grep -E "TIMEOUT|OK|FAILED"
grep -q '^OK' bash-pty.out || fail=1

echo "== memory reserve (a pretend Psion with 6 MB free, 1 MB kept)"
fresh
LX_SIM_FREE=6144 LX_RESERVE=1024 timeout 600 qemu-arm -cpu ti925t ./psihost -r $R /bin/sh -c \
	'awk "BEGIN { s = \"x\"; for (i = 0; i < 24; i++) s = s s; print length(s) }"; echo "awk=$?"; cat /proc/psilinux/mem; echo alive' \
	< /dev/null > reserve.log 2>&1 || true
if grep -q "^memory: kept 1024 KB" reserve.log && grep -q "awk=1" reserve.log && grep -q "^alive" reserve.log \
   && ! grep -q "^refused	0 " reserve.log; then echo "ok   refused, logged once, Linux still runs"
else echo "FAIL reserve: see build/linux/reserve.log"; fail=1; fi
rm -rf $R
exit $fail
