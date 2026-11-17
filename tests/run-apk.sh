#!/bin/bash
# run-apk.sh - apk with the online catalogue on the host harness (psihost
# under qemu-arm -cpu ti925t), end to end: the catalogue made by
# tools/apkcat/mkapkcat.py from Alpine's index, served here; the packages
# from Alpine's mirror; installed by the real apk in the Alpine root just
# built (alpine/build-root.sh). Needs the Internet.
#   tools/lxbuild "tests/run-apk.sh [INDEXDIR]"
# LX_DBT_STATS=1 adds each program's memory to build/linux/apk.log.
# APKCAT_LIVE=1: the published catalogue (GitHub Pages, over https) instead.
set -e
REPO=$(cd "$(dirname "$0")/.." && pwd)
make -C "$REPO/runtime" host > /dev/null
if [ "$APKCAT_LIVE" = 1 ]; then CATENV=""
else
	C=$REPO/build/apkcat-site
	[ -d "$C/apk" ] || python3 "$REPO/tools/apkcat/mkapkcat.py" "$C" ${1:+--index "$1"}
	PORT=$((30000 + RANDOM % 10000))
	(cd "$C" && exec python3 -m http.server -b 127.0.0.1 $PORT > /dev/null 2>&1) & S=$!
	trap 'kill $S 2>/dev/null' EXIT
	sleep 1
	CATENV="APK_CATALOGUE=http://127.0.0.1:$PORT/apk/v3.24/armhf"
fi
cd "$REPO/build/linux"
R=apk-test
rm -rf $R; mkdir -p $R
for f in "$REPO"/build/alpine-root/ROOT.I*; do ln "$f" $R/ 2>/dev/null || cp "$f" $R/; done
s=$(date +%s)
LX_DRIVE_T=$REPO/tests timeout 1800 qemu-arm -cpu ti925t ./psihost -r $R /bin/sh -c \
	"export PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin APK_SOURCE=online $CATENV; /bin/sh /t/apktest.sh" \
	< /dev/null > apk.log 2>&1 || true
grep -E "^(ok|FAIL|passed)" apk.log
echo "(the harness: $(( $(date +%s) - s )) s in all; build/linux/apk.log)"
rm -rf $R
grep -q " 0 failed" apk.log
