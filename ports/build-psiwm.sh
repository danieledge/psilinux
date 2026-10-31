#!/bin/bash
# Builds psiwm, PsiLinux's window manager and desktop (ports/psiwm/), with
# the PsiLinux toolchain against the X client libraries that build-x11.sh
# made (build/psilinux-root/x11-sysroot, read only), into
# build/psilinux-root/x11/usr/bin/psiwm.
#   tools/lxbuild ports/build-psiwm.sh            the program
#   tools/lxbuild "ports/build-psiwm.sh demo"     also psiwm-demo (scripted, for
#                                                 screenshots: ports/psiwm/test/)
# psiwm is one static program: the window manager, the toolbar, the desktop,
# and Files, Notes, Calc, Clock, System and Help, sharing one copy of Xlib.
# Also psisplash (the start-up screen, straight on /dev/fb0 before X is up)
# and psiwm-alone (runs Doom and the like without X, then brings X back).
# Start them:  psisplash "Starting the X server…" 30
#              X :0 -nolisten tcp -noreset -dumbSched & psiwm &
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/.." && pwd)
WORK=${PSION_WORK:-$(cd "$REPO/.." && pwd)}
LXTC=${LXTC:-$WORK/psilinux-toolchain}
export PATH=$LXTC/bin:$PATH
T=arm-linux-musleabi
OUT=$REPO/build/psilinux-root
P=${X11OUT:-$OUT/x11}
SYS=$OUT/x11-sysroot
SRC=$HERE/psiwm
B=$REPO/build/psiwm
mkdir -p "$B" "$P/usr/bin"
[ -f "$SYS/usr/lib/libX11.a" ] || { echo "no X libraries: run ports/build-x11.sh first"; exit 1; }
CFLAGS="-Os -ffunction-sections -fdata-sections -Wall -Wno-parentheses -Wno-format-truncation -Wno-misleading-indentation -I$SYS/usr/include -I$SRC"
LIBS="-L$SYS/usr/lib -Wl,--gc-sections -lXext -lX11 -lxcb -lXau -lm"
OBJS="wm ui desk files notes calc clock sysmon help splash fontdata"
build() {     # name extra-cflags
	local o= f
	for f in $OBJS; do
		lx-cc $CFLAGS $2 -c "$SRC/$f.c" -o "$B/$f$3.o"
		o="$o $B/$f$3.o"
	done
	lx-cc -o "$B/$1" $o $LIBS
}
echo "== psiwm"
build psiwm "" ""
$T-strip -o "$P/usr/bin/psiwm" "$B/psiwm"
ls -la "$P/usr/bin/psiwm" | awk '{print "   " $5 " bytes " $9}'
# what each part costs (code and data, before linking)
$T-size $(for f in $OBJS; do echo "$B/$f.o"; done) | awk 'NR > 1 {sub(".*/", "", $6); printf "   %-10s %6d bytes\n", $6, $1 + $2 + $3}'
# psisplash: the start-up screen before X (no Xlib), and psiwm-alone (Doom)
lx-cc $CFLAGS -c "$SRC/splashmain.c" -o "$B/splashmain.o"
lx-cc -o "$B/psisplash" "$B/splashmain.o" "$B/splash.o" "$B/fontdata.o" -Wl,--gc-sections
$T-strip -o "$P/usr/bin/psisplash" "$B/psisplash"
cp "$SRC/psiwm-alone" "$P/usr/bin/psiwm-alone"
ls -la "$P/usr/bin/psisplash" "$P/usr/bin/psiwm-alone" | awk '{print "   " $5 " bytes " $9}'
mkdir -p "$P/usr/share/licenses/psiwm"
cp "$SRC/THIRD-PARTY" "$P/usr/share/licenses/psiwm/"
if [ "$1" = demo ]; then
	echo "== psiwm-demo (scripted, for screenshots)"
	build psiwm-demo -DPSIWM_DEMO -demo
	$T-strip "$B/psiwm-demo"
fi
