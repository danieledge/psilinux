#!/bin/sh
# mkalpine.sh - a small Alpine root for the netBook test image (nbimage.mts),
# whose ROM has no room for the whole one: the Alpine root's stage
# (alpine/build-root.sh) with bash (or fastfetch), BusyBox and PsiLinux's own
# files, without the other programs, the manual pages and the libraries
# nothing here needs. -> build/nbtest/alpine/ROOT.IMG, which the test
# launcher copies to C:\Linux\Alpine\ (Start: Alpine Linux, shell only).
#   tools/lxbuild "tests/emu/netbook/mkalpine.sh"
set -e
TOP=$(cd "$(dirname "$0")/../../.." && pwd)
A=$TOP/build/alpine-root/stage
S=$TOP/build/nbtest/alpine-stage
O=$TOP/build/nbtest/alpine
[ -x "$A/bin/bash" ] || { echo "no Alpine root stage: alpine/build-root.sh first"; exit 1; }
rm -rf "$S" "$O"; mkdir -p "$O"
cp -a "$A" "$S"
cd "$S"
rm -rf usr/share/* usr/libexec usr/lib/bash usr/lib/engines-* usr/lib/ossl-modules etc/ssl* etc/ssh lib/apk/exec
find usr/lib -maxdepth 1 ! -type d ! -name 'libreadline*' ! -name 'libncursesw*' ! -name os-release -exec rm -f {} +
find usr/bin -type f ! -name fastfetch -exec rm -f {} +
# the ROM has room for bash or fastfetch, not both: NB_FASTFETCH=1 takes
# fastfetch (and BusyBox sh is the shell), to time it
if [ "$NB_FASTFETCH" = 1 ]; then
	rm -f bin/bash usr/lib/libreadline* usr/lib/libncursesw*
	sed -i 's|:/bin/bash$|:/bin/sh|' etc/passwd
else
	rm -f usr/bin/fastfetch
fi
# (shell only: no OpenRC)
find sbin -type f -exec rm -f {} +
rm -rf etc/init.d etc/conf.d etc/runlevels lib/rc usr/lib/psilinux/apk-*
mkdir -p usr/share/licenses
echo "(the netBook test root: see the Alpine root for the licences)" > usr/share/licenses/README
# the timings: t9, run by itself 20 s after the first login (the emulator's
# typing is not to be trusted for this: an Enter repeats); NB_T9=0: not
# run, for a screenshot of the start
install -m 755 "$TOP/tests/emu/netbook/t/t9" "$TOP/tests/emu/netbook/t/t8" "$TOP/tests/emu/netbook/t/t7" root/
# (NB_RUN=t8: the memory at idle, in place of t9; NB_RUN=t7: sleep)
RUN=${NB_RUN:-t9}
[ "$NB_T9" = 0 ] || for f in root/.profile root/.bash_profile; do
	if [ "$RUN" = t8 ] || [ "$RUN" = t7 ]; then echo '[ -x "$HOME/'$RUN'" ] && [ ! -e /tmp/.t9 ] && : > /tmp/.t9 && "$HOME/'$RUN'"' >> "$f"
	else echo '[ -x "$HOME/t9" ] && [ ! -e /tmp/.t9 ] && : > /tmp/.t9 && { sleep 20; "$HOME/t9"; }' >> "$f"; fi
done
du -sh "$S"
python3 "$TOP/tools/mkroot.py" "$S" "$O/ROOT.IMG" --parts 64   # (small parts: they fit in the gaps the programs leave)
