#!/bin/bash
# Builds PsiLinux for the Psion Series 5mx: the runner (psilxrun.exe), the
# app (PsiLinux.app) and, with "sis", dist/PsiLinux.sis.
#   tools/docker/psibuild "linux/build.sh [sis]"
# The Linux programs and ROOT.IMG are built separately, in psilinux-build:
#   linux/tools/lxbuild "linux/rootfs/build-rootfs.sh"
set -e
: "${PSION_SDK:?set PSION_SDK to your psion_cpp_sdk_linux directory}"
HERE=$(cd "$(dirname "$0")" && pwd)
TOP=$(cd "$HERE/.." && pwd)
export EPOCROOT=$PSION_SDK/epoc_cpp_sdk
REL=$EPOCROOT/epoc32/release/marm/rel
export PATH=/usr/bin:/bin:$PSION_SDK/gcc-3.0-psion-98r2-9/bin:$EPOCROOT/epoc32/tools
export TMP=$EPOCROOT/tmp WINEDEBUG=-all
mkdir -p "$TMP" "$EPOCROOT/ptproj" "$TOP/build"

echo "== libvterm (PsiTerm's)"
[ -f "$REL/psivterm.lib" ] || "$TOP/build_vterm.sh"
echo "== psilxrun.exe (the runner)"
make -C "$HERE/runner" SDK="$PSION_SDK" > "$TOP/build/psilxrun.log" 2>&1 \
	|| { grep -E "error|undefined" "$TOP/build/psilxrun.log" | head -30; exit 1; }
echo "== pictures (PsiLinux.mbm, PsiLinux.aif)"
python3 "$HERE/tools/mkicons.py" "$TOP/build/psilinux-icons" "$HERE/app/lxicons.h" > /dev/null
( cd "$TOP/build/psilinux-icons" && wine "$EPOCROOT/epoc32/tools/bmconv.exe" /q psilinux.mbm $(tr '\n' ' ' < files.txt) > /dev/null 2>&1 )
echo "== PsiLinux.app"
# makmake works on projects inside the SDK tree: link the folders there
for l in psilinux:linux/app psilxterm:app psilxssh:ssh psilxcommon:linux/common; do
	name=${l%%:*}; dir=$TOP/${l#*:}
	if [ -e "$EPOCROOT/ptproj/$name" ] && [ ! -L "$EPOCROOT/ptproj/$name" ]; then
		echo "$EPOCROOT/ptproj/$name exists and is not a link - move it away"; exit 1
	fi
	ln -sfn "$dir" "$EPOCROOT/ptproj/$name"
done
( cd "$EPOCROOT/ptproj/psilinux" && makmake psilinux marm > "$TOP/build/psilinux-makmake.log" 2>&1 \
  && make -f psilinux.marm rel > "$TOP/build/psilinux.log" 2>&1 ) \
  || { grep -i -B1 -A4 "error\|undefined" "$TOP/build/psilinux.log" | head -40; exit 1; }

P=$TOP/build/psilinux-pkg
rm -rf "$P"; mkdir -p "$P"
cp "$REL/psilinux.app" "$REL/psilinux.rsc" "$TOP/build/lxrun-obj/psilxrun.exe" "$P/"
cp "$TOP/pkg/psiterm.gdr" "$P/psiterm.gdr"          # (PsiTerm's Terminus font)
cp "$TOP/build/psilinux-icons/psilinux.aif" "$TOP/build/psilinux-icons/psilinux.mbm" "$P/"
if [ "$1" = sis ]; then
	install -m 644 "$REL/stdlib.sis" "$P/STDLIB.SIS"
	cp "$HERE/pkg/psilinux.pkg" "$TOP/pkg/terminus-licence.txt" "$P/"
	cp "$HERE/runtime/softfloat/COPYING.txt" "$P/softfloat-licence.txt"     # (the translator's VFP)
	( cd "$P" && wine "$EPOCROOT/epoc32/tools/makesis.exe" psilinux.pkg "$TOP/dist/PsiLinux.sis" > /dev/null )
	ls -la "$TOP/dist/PsiLinux.sis"
	# the programs: ROOT.IMG (linux/rootfs/build-rootfs.sh makes it)
	R=$TOP/build/psilinux-root
	if [ -f "$R/ROOT.IMG" ]; then
		cp "$HERE/pkg/psilinux-root.pkg" "$R/"
		( cd "$R" && wine "$EPOCROOT/epoc32/tools/makesis.exe" psilinux-root.pkg "$TOP/dist/PsiLinux-root.sis" > /dev/null )
		ls -la "$TOP/dist/PsiLinux-root.sis"
	else
		echo "(no ROOT.IMG yet: run linux/rootfs/build-rootfs.sh in psilinux-build for PsiLinux-root.sis)"
	fi
	# Alpine mode's root: build/alpine-root/ROOT.IMG (linux/alpine/build-root.sh)
	A=$TOP/build/alpine-root
	if [ -f "$A/ROOT.IMG" ]; then
		cp "$HERE/pkg/psilinux-alpine.pkg" "$A/"
		( cd "$A" && wine "$EPOCROOT/epoc32/tools/makesis.exe" psilinux-alpine.pkg "$TOP/dist/PsiLinux-alpine-root.sis" > /dev/null )
		ls -la "$TOP/dist/PsiLinux-alpine-root.sis"
	else
		echo "(no Alpine ROOT.IMG yet: run linux/alpine/build-root.sh for PsiLinux-alpine-root.sis)"
	fi
fi
ls -la "$P"
