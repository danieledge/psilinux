#!/bin/bash
# run-sleep.sh - sleep (0.39) on the host harness: Linux's memory to files
# and made inaccessible whenever it is idle (LX_SLEEP_MS), so anything that
# touches it while asleep faults; then the shell, job control, a pipeline,
# history, a background job's timer, nano held open (no sleep) and an open
# socket (no sleep), each through sleeps and wakes. Needs the Internet (a
# socket to example.com).
#   tools/lxbuild "tests/run-sleep.sh"
set -e
REPO=$(cd "$(dirname "$0")/.." && pwd)
make -C "$REPO/runtime" host > /dev/null
cd "$REPO/build/linux"
R=sleep-test
rm -rf $R; mkdir -p $R
for f in "$REPO"/build/alpine-root/ROOT.I*; do ln "$f" $R/ 2>/dev/null || cp "$f" $R/; done
export LX_SLEEP_MS=${LX_SLEEP_MS:-1500} LX_SLEEP_DIR=${LX_SLEEP_DIR:-/tmp}
C='c() { local k v; while read -r k v; do [ "$k" = slept ] && { v=${v%% *}; echo ${v/never/0}; }; done < /proc/psilinux/mem; }'
python3 "$REPO/tests/ptydrive.py" $R sleep.log 'wait:# :300' \
	"send:$C\r" 'wait:# ' 'sleep:2' \
	'send:a=$(c)\r' 'wait:# ' 'sleep:2' 'send:echo idle$(( $(c) - a ))\r' 'wait:idle1' 'sleep:2' \
	'send:ls /etc | head -3 | wc -l\r' 'wait:\r3\r' 'sleep:2' \
	'send:sleep 100 &\r' 'wait:# :60' 'sleep:2' 'send:jobs\r' 'wait:Running' 'sleep:2' 'send:kill %1\r' 'wait:# ' 'sleep:2' \
	'send:sleep 40\r' 'sleep:3' 'send:\x1a' 'wait:Stopped:60' 'sleep:2' 'send:fg\r' 'sleep:2' 'send:\x03' 'wait:# :60' 'sleep:2' \
	'send:x=$(seq 1 2000 | wc -l); echo got$x\r' 'wait:got2000' 'sleep:2' \
	'send:echo hist\r' 'wait:# ' 'sleep:2' 'send:\x1b[A\r' 'wait:hist' 'sleep:2' \
	'send:a=$(c); nano /tmp/n\r' 'sleep:4' 'send:hello' 'sleep:3' 'send:\x0f' 'sleep:1' 'send:\r' 'sleep:2' 'send:\x18' 'sleep:0.5' \
	'send:echo nano$(( $(c) - a ))\r' 'wait:nano0' 'send:cat /tmp/n\r' 'wait:hello' 'sleep:2' \
	'send:exec 3<>/dev/tcp/example.com/80; a=$(c)\r' 'wait:# :60' 'sleep:3' 'send:echo sock$(( $(c) - a ))\r' 'wait:sock0' \
	'send:printf "GET / HTTP/1.0\\r\\nHost: example.com\\r\\n\\r\\n" >&3; head -1 <&3\r' 'wait:HTTP/1' \
	'send:exec 3<&-; a=$(c)\r' 'wait:# ' 'sleep:3' 'send:echo closed$(( $(c) - a ))\r' 'wait:closed1' 'sleep:2' \
	'send:sleep 62 & a=$(c)\r' 'wait:# ' 'sleep:68' 'send:jobs; echo timer$(( $(c) - a ))\r' 'wait:Done(.|\n)*timer[12]' \
	'send:exit\r' 'sleep:3' -- /bin/bash -il > sleep.out 2>&1 || true
grep -E "TIMEOUT|^OK|FAILED" sleep.out
grep -a "psihost: slept" sleep.log
grep -q '^OK' sleep.out
