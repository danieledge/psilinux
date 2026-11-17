#!/bin/bash
# run.sh - the online catalogue's tests, on the host: the generator
# (mkapkcat.py), and the Psion's apk wrapper reading what it made, compared
# with psiapk.py's answers (the PsiProxy helper) for the same index. The
# wrapper runs in an alpine container: BusyBox sh, wget and awk, as on the
# Psion.
#   tools/apkcat/test/run.sh [INDEXDIR]
# INDEXDIR has Alpine's APKINDEX-<repo>.tar.gz (or psiapk's cache names);
# default ~/.cache/psiapk, else they are downloaded. Needs docker and
# alpine:3.24. APKCAT_IMAGEDB: the image's apk database (else build/alpine-root's).
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../../.." && pwd)
T=${APKCAT_TEST_DIR:-$REPO/build/apkcat-test}
IDX=${1:-$HOME/.cache/psiapk}
rm -rf "$T"; mkdir -p "$T/idx"
for r in main community; do
	f=$IDX/APKINDEX-$r.tar.gz; [ -f "$f" ] || f=$IDX/APKINDEX-v3.24-$r-armhf.tar.gz
	if [ -f "$f" ]; then cp -p "$f" "$T/idx/APKINDEX-$r.tar.gz"
	else curl -sSfL -o "$T/idx/APKINDEX-$r.tar.gz" "https://dl-cdn.alpinelinux.org/alpine/v3.24/$r/armhf/APKINDEX.tar.gz"; fi
done
fail=0
ok() { echo "ok   $*"; }
bad() { echo "FAIL $*"; fail=1; }

echo "== the generator"
python3 "$REPO/tools/apkcat/mkapkcat.py" "$T/site" --index "$T/idx" | tee "$T/gen.log"
C=$T/site/apk/v3.24/armhf
python3 "$HERE/gentest.py" "$T/idx" "$C" && ok "generator: every file, record and shard checked" || bad "generator"

echo "== the servers"
PA=$((20000 + RANDOM % 10000)); PB=$((PA + 1))
python3 "$HERE/psiapkd.py" $PA "$T/idx" & S1=$!
(cd "$T/site" && exec python3 -m http.server -b 127.0.0.1 $PB > /dev/null 2>&1) & S2=$!
trap 'kill $S1 $S2 2>/dev/null' EXIT
sleep 2

# the installed packages: the PsiLinux image's, if built (else a small
# system of the index's own), and the same with some versions made older
python3 "$HERE/mkdb.py" "$T/idx" "${APKCAT_IMAGEDB:-$REPO/build/alpine-root/stage/lib/apk/db/installed}" "$T"

echo "== the wrapper (BusyBox, in alpine:3.24), online catalogue against psiapk"
docker run --rm --network host -e PA=$PA -e PB=$PB \
	-v "$REPO/alpine/overlay/usr/local/sbin/apk:/usr/local/sbin/apk:ro" \
	-v "$T:/t" -v "$HERE/wrapper.sh:/wrapper.sh:ro" alpine:3.24 /bin/sh /wrapper.sh | tee "$T/wrapper.log"
grep -q "^FAIL" "$T/wrapper.log" && fail=1
grep -q "^passed .*, 0 failed" "$T/wrapper.log" || fail=1
[ $fail = 0 ] && echo "all passed" || echo "SOME FAILED"
exit $fail
