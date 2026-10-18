#!/bin/bash
# Builds PsiLinux for the Psion Series 5mx: the runner (psilxrun.exe), the
# app (PsiLinux.app) and, with "sis", dist/PsiLinux.sis.
#   core/tools/docker/psibuild "bash build.sh [sis]"
# The Linux programs and ROOT.IMG are built separately, in psilinux-build:
#   tools/lxbuild "rootfs/build-rootfs.sh"
#
# The shared code (psishared.h, psibell.h, the Terminus font, the icon tools,
# the build container) is psion-core, this checkout's core/ submodule
# (git submodule update --init; or $PSION_CORE). The terminal (libvterm,
# termsb.cpp, glyphs.cpp, build_vterm.sh) is PsiTerm's own, from a psiterm
# checkout beside this one ($PSITERM, default ../psiterm).
set -e
: "${PSION_SDK:?set PSION_SDK to your psion_cpp_sdk_linux directory}"
HERE=$(cd "$(dirname "$0")" && pwd)
TOP=$HERE
CORE=${PSION_CORE:-$HERE/core}
[ -f "$CORE/ssh/psishared.h" ] || { echo "no psion-core in $CORE: git submodule update --init"; exit 1; }
PSITERM=${PSITERM:-$HERE/../psiterm}
[ -f "$PSITERM/build_vterm.sh" ] || { echo "no psiterm checkout at $PSITERM (PSITERM=...)"; exit 1; }
export EPOCROOT=$PSION_SDK/epoc_cpp_sdk
REL=$EPOCROOT/epoc32/release/marm/rel
export PATH=/usr/bin:/bin:$PSION_SDK/gcc-3.0-psion-98r2-9/bin:$EPOCROOT/epoc32/tools
export TMP=$EPOCROOT/tmp WINEDEBUG=-all
mkdir -p "$TMP" "$EPOCROOT/ptproj" "$TOP/build"

echo "== libvterm (PsiTerm's)"
[ -f "$REL/psivterm.lib" ] || "$PSITERM/build_vterm.sh"
echo "== psilxrun.exe (the runner)"
make -C "$HERE/runner" SDK="$PSION_SDK" PSION_CORE="$CORE" > "$TOP/build/psilxrun.log" 2>&1 \
	|| { grep -E "error|undefined" "$TOP/build/psilxrun.log" | head -30; exit 1; }
echo "== pictures (PsiLinux.mbm, PsiLinux.aif)"
python3 "$HERE/tools/mkicons.py" "$TOP/build/psilinux-icons" "$HERE/app/lxicons.h" > /dev/null
( cd "$TOP/build/psilinux-icons" && wine "$EPOCROOT/epoc32/tools/bmconv.exe" /q psilinux.mbm $(tr '\n' ' ' < files.txt) > /dev/null 2>&1 )
echo "== PsiLinux.app"
# makmake works on projects inside the SDK tree: link the folders there
for l in psilinux:$HERE/app psilxterm:$PSITERM/app psilxssh:$CORE/ssh psilxcommon:$HERE/common; do
	name=${l%%:*}; dir=${l#*:}
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
cp "$CORE/pkg/psiterm.gdr" "$P/psiterm.gdr"         # (the Terminus font, from psion-core)
cp "$TOP/build/psilinux-icons/psilinux.aif" "$TOP/build/psilinux-icons/psilinux.mbm" "$P/"
if [ "$1" = sis ]; then
	install -m 644 "$REL/stdlib.sis" "$P/STDLIB.SIS"
	cp "$HERE/pkg/psilinux.pkg" "$CORE/pkg/terminus-licence.txt" "$P/"
	cp "$HERE/runtime/softfloat/COPYING.txt" "$P/softfloat-licence.txt"     # (the translator's VFP)
	( cd "$P" && wine "$EPOCROOT/epoc32/tools/makesis.exe" psilinux.pkg "$TOP/dist/PsiLinux.sis" > /dev/null )
	ls -la "$TOP/dist/PsiLinux.sis"
	# The roots, in parts (DECISIONS.md 54): the EPOC installer cannot open a
	# very large .sis, whatever is in it, so each image goes in .sis files of at most 6 MB:
	# NAME.sis (with ROOT.IMG), then NAME-2.sis, NAME-3.sis...
	#   rootsis DIR MAIN.pkg TARGET UIDS OUTNAME
	rootsis() {
		local dir=$1 tmpl=$2 target=$3 uids=$4 name=$5 k pkg out
		rm -f "$TOP/dist/$name.sis" "$TOP/dist/$name"-[0-9].sis "$dir"/part[0-9]*.pkg
		cp "$tmpl" "$dir/main.pkg"
		python3 "$HERE/tools/sisparts.py" "$dir" "$dir/main.pkg" "$target" "$uids" > /dev/null
		for pkg in "$dir"/part[0-9]*.pkg; do
			k=${pkg##*/part}; k=${k%.pkg}
			out=$name.sis; [ "$k" = 1 ] || out=$name-$k.sis
			( cd "$dir" && wine "$EPOCROOT/epoc32/tools/makesis.exe" "${pkg##*/}" "$TOP/dist/$out" > /dev/null )
			ls -la "$TOP/dist/$out"
		done
	}
	# the programs: ROOT.IMG (linux/rootfs/build-rootfs.sh makes it)
	R=$TOP/build/psilinux-root
	if [ -f "$R/ROOT.IMG" ]; then
		rootsis "$R" "$HERE/pkg/psilinux-root.pkg" '!:\Linux\' 0x01000A90 PsiLinux-root
	else
		echo "(no ROOT.IMG yet: run linux/rootfs/build-rootfs.sh in psilinux-build for PsiLinux-root.sis)"
	fi
	# Alpine mode's root: build/alpine-root/ROOT.IMG (linux/alpine/build-root.sh)
	A=$TOP/build/alpine-root
	if [ -f "$A/ROOT.IMG" ]; then
		rootsis "$A" "$HERE/pkg/psilinux-alpine.pkg" '!:\Linux\Alpine\' 0x01000A8C,0x01000A8D,0x01000A8E,0x01000A8F PsiLinux-alpine-root
	else
		echo "(no Alpine ROOT.IMG yet: run linux/alpine/build-root.sh for PsiLinux-alpine-root.sis)"
	fi
fi
ls -la "$P"
