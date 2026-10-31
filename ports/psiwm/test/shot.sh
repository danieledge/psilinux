#!/bin/bash
# Screenshots of psiwm: Xfbdev on a file under qemu-arm, psiwm-demo running
# a script, the frame buffer turned into what the Psion shows.
#   (in the container, with the test disks at /c and /d: see run.sh)
#   shot.sh NAME 'SCRIPT' [CLIENT...]
# SCRIPT is psiwm-demo's (wm.c: run N, key K [cs], tap X Y, menu N, wait MS,
# exec PROG...); CLIENT is an X program from the x11 root started beside it,
# e.g. xeyes. Writes build/psiwm-shots/NAME.png (and NAME-2x.png).
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../../.." && pwd)
R=$REPO/build/psilinux-root/x11
XS=${XSERVER:-$R/usr/bin/Xfbdev}
OUTD=$REPO/build/psiwm-shots
T=$REPO/build/psiwm/test
NAME=$1; SCRIPT=$2; shift 2
mkdir -p "$OUTD" "$T"
FR=$T/root
D=:$((RANDOM % 50 + 10))
cd "$T"
rm -f fb-$NAME.raw; touch fb-$NAME.raw
qemu-arm "$XS" $D -fb "$T/fb-$NAME.raw" -nolisten tcp -noreset -dumbSched -xkbdir "$R/usr/share/X11/xkb" > xs-$NAME.log 2>&1 &
XP=$!
for i in $(seq 50); do [ -e /tmp/.X11-unix/X${D#:} ] && break; sleep 0.2; done
export DISPLAY=$D TZ=${TZ:-GMT0}
PSIWM_EXEC_WRAP=$T/wrap.sh PSIWM_PROC=$FR/fakeproc PSIWM_DEMO="$SCRIPT" \
	qemu-arm ${QEMUOPT:-} "$REPO/build/psiwm/psiwm-demo" > wm-$NAME.log 2>&1 &
WP=$!
printf '#!/bin/sh\nexec qemu-arm %s/usr/bin/$(basename "$1") "$@"\n' "$R" > wrap.sh; chmod +x wrap.sh
sleep 1.5
for c in "$@"; do
	case $c in
	xterm) qemu-arm "$R/usr/bin/xterm" -geometry 80x14 -T xterm -e "$HERE/xterm-demo.sh" > c-$c.log 2>&1 & ;;
	*) qemu-arm "$R/usr/bin/$c" > c-$c.log 2>&1 & ;;
	esac
	sleep 2
done
# the script ends with "shot FILE": wait for it
for i in $(seq 150); do [ -e "$T/done-$NAME" ] && break; sleep 0.2; done
sleep ${SETTLE:-1}
cp fb-$NAME.raw snap-$NAME.raw
rm -f "$T/done-$NAME"
kill $WP 2>/dev/null; for c in "$@"; do pkill -f "usr/bin/$c" 2>/dev/null || true; done
kill $XP 2>/dev/null; wait 2>/dev/null || true
python3 "$HERE/fb2psion.py" snap-$NAME.raw "$OUTD/$NAME.png"
python3 "$HERE/fb2psion.py" snap-$NAME.raw "$OUTD/$NAME-2x.png" 2
echo "$OUTD/$NAME.png"
