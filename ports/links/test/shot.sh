#!/bin/bash
# Screenshots of Links (links -g) on PsiLinux's X server, in the container:
# Xfbdev on a file under qemu-arm, the test pages (mkpage.py) served by
# python's http.server on 127.0.0.1, Links run under qemu-arm (with
# -strace, for its memory), the frame buffer turned into what the Psion
# shows (16 greys, ports/psiwm/test/fb2psion.py: common/lxfbconv.h's way).
#   tools/lxbuild "ports/links/test/shot.sh NAME PATH-OR-URL [STEP...]"
# PATH is one of the test pages; tls:PATH serves it over TLS (https://localhost,
# with a certificate from a test CA that is added to the trusted roots);
# tlsbad:PATH the same with a self-signed certificate, which Links must question.
# STEP: k=KEY,KEY...  keys (sendev.py's names: down, pgdn, esc, enter, ctrl+r...)
#       t=X,Y         a pen tap
#       w=SECONDS     wait
#       s=NAME        a screenshot now (one is always taken at the end, as NAME)
# PROXY=1: Links goes through proxy.py (a stand-in for PsiProxy), with
# https:// sent to it as plain requests (-https-via-http-proxy 1); its log of
# request lines is printed at the end.
# WM=1: psiwm (build-psiwm.sh's, from the x11 root) runs first, and Links is
# one of its windows, as when started from the desktop's Web icon.
# Writes build/links-shots/NAME.png (and NAME-2x.png) and prints Links'
# memory (memlog.py) at each screenshot.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../../.." && pwd)
R=$REPO/build/psilinux-root/x11
OUTD=$REPO/build/links-shots
T=$REPO/build/links-test
NAME=$1; URL=$2; shift 2
mkdir -p "$OUTD" "$T/www"
python3 "$HERE/mkpage.py" "$T/www"
cd "$T"
D=$((RANDOM % 50 + 10))
PORT=$((8100 + D))
CERTS=$R/usr/share/links/ca-certificates.crt
TLSP=
case $URL in
http*|file*|about*) ;;
tls:*|tlsbad:*)
	# a test CA and a certificate for localhost (or, for tlsbad, one of its own)
	mkdir -p tls; ( cd tls
	openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:P-256 -nodes -days 30 -subj "/O=PsiLinux Test/CN=PsiLinux Test CA" \
		-keyout ca.key -out ca.crt 2> /dev/null
	openssl req -newkey ec -pkeyopt ec_paramgen_curve:P-256 -nodes -subj "/CN=localhost" -keyout srv.key -out srv.csr 2> /dev/null
	printf 'subjectAltName=DNS:localhost\n' > san.ext
	if [ "${URL%%:*}" = tls ]; then
		openssl x509 -req -in srv.csr -CA ca.crt -CAkey ca.key -CAcreateserial -days 30 -extfile san.ext -out srv.crt 2> /dev/null
	else
		openssl x509 -req -in srv.csr -signkey srv.key -days 30 -extfile san.ext -out srv.crt 2> /dev/null
	fi )
	cat "$R/usr/share/links/ca-certificates.crt" tls/ca.crt > tls/bundle.crt
	CERTS=$T/tls/bundle.crt
	TPORT=$((PORT + 100))
	python3 -c '
import http.server, ssl, sys, os
os.chdir(sys.argv[1])
s = http.server.ThreadingHTTPServer(("127.0.0.1", int(sys.argv[2])), http.server.SimpleHTTPRequestHandler)
c = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER); c.load_cert_chain(sys.argv[3], sys.argv[4])
s.socket = c.wrap_socket(s.socket, server_side=True)
s.serve_forever()' "$T/www" $TPORT "$T/tls/srv.crt" "$T/tls/srv.key" > tls.log 2>&1 &
	TLSP=$!
	URL=https://localhost:$TPORT/${URL#*:} ;;
*) URL=http://127.0.0.1:$PORT/$URL ;;
esac
( cd "$T/www" && exec python3 -m http.server -b 127.0.0.1 $PORT ) > www.log 2>&1 &
WWW=$!
rm -f fb.raw kbd.fifo pen.fifo; touch fb.raw; mkfifo kbd.fifo pen.fifo
sleep 1000 > kbd.fifo & K1=$!
sleep 1000 > pen.fifo & K2=$!
qemu-arm "$R/usr/bin/Xfbdev" :$D -fb "$T/fb.raw" -nolisten tcp -noreset -dumbSched -xkbdir "$R/usr/share/X11/xkb" \
	-keybd psi,,device=$T/kbd.fifo -mouse psi,,device=$T/pen.fifo > xs.log 2>&1 &
XP=$!
for i in $(seq 50); do [ -e /tmp/.X11-unix/X$D ] && break; sleep 0.2; done
rm -rf home; mkdir -p home
WMP=
if [ -n "${WM:-}" ]; then
	DISPLAY=:$D TZ=${TZ:-GMT0} qemu-arm "$R/usr/bin/psiwm" > wm.log 2>&1 & WMP=$!
	sleep 3
	# (no welcome box: a configuration already there)
	mkdir -p home/.links; : > home/.links/links.cfg
fi
export DISPLAY=:$D HOME=$T/home
# (the test CA's bundle for tls:, otherwise Links finds its own)
[ "$CERTS" = "$R/usr/share/links/ca-certificates.crt" ] || export SSL_CERT_FILE=$CERTS
# LINKS_ARGS: what links is run with (psiwm's Web runs plain "links")
PXP=; PXARGS=
if [ -n "${PROXY:-}" ]; then
	rm -f proxy.log; touch proxy.log
	python3 "$HERE/proxy.py" $((PORT + 200)) "$T/proxy.log" & PXP=$!
	PXARGS="-http-proxy 127.0.0.1:$((PORT + 200)) -https-via-http-proxy 1"
	sleep 0.5
fi
qemu-arm -strace "$R/usr/bin/links" ${LINKS_ARGS--g -mode 640x240} $PXARGS "$URL" > links.out 2> strace.log &
LP=$!
# (a screen is "drawn" when the frame buffer has not changed for 1.5 s)
settle() {
	local a b n=0
	sleep 1
	a=$(md5sum < fb.raw)
	while [ $n -lt 60 ]; do
		sleep 0.5; b=$(md5sum < fb.raw)
		if [ "$a" = "$b" ]; then n=$((n + 1)); [ $n -ge 3 ] && return; else n=0; a=$b; fi
	done
}
marks=
shot() {
	settle
	cp fb.raw "snap-$1.raw"
	python3 "$REPO/ports/psiwm/test/fb2psion.py" "snap-$1.raw" "$OUTD/$1.png"
	python3 "$REPO/ports/psiwm/test/fb2psion.py" "snap-$1.raw" "$OUTD/$1-2x.png" 2
	marks="$marks $(stat -c %s strace.log)"
	echo "$OUTD/$1.png"
}
settle
for s in "$@"; do
	case $s in
	k=*) IFS=, read -ra ks <<< "${s#k=}"; python3 "$HERE/sendev.py" keys kbd.fifo "${ks[@]}"; settle ;;
	t=*) IFS=, read -r x y <<< "${s#t=}"; python3 "$HERE/sendev.py" tap pen.fifo "$x" "$y"; settle ;;
	w=*) sleep "${s#w=}" ;;
	s=*) shot "${s#s=}" ;;
	esac
done
shot "$NAME"
kill $LP 2>/dev/null || true; sleep 0.5
kill $XP $WWW $K1 $K2 $TLSP $PXP $WMP 2>/dev/null || true; wait 2>/dev/null || true
[ -z "$PXP" ] || { echo "the proxy was asked:"; sed 's/^/   /' proxy.log; }
echo "Links' memory (anonymous mappings and heap; at each screenshot, in order):"
python3 "$HERE/memlog.py" strace.log $marks | sed 's/^/   /'
grep -v "^$" links.out | head -5 || true
