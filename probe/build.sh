#!/bin/bash
# Builds the PsiLinux M0 probe (LXPROBE.APP) into build/lxprobe-pkg/, and with
# "sis" dist/LxProbe-test.sis (a test program: never a release).
#   tools/docker/psibuild "linux/probe/build.sh [sis]"
# Then: node --experimental-strip-types linux/tools/mkcard.ts probe
set -e
: "${PSION_SDK:?set PSION_SDK to your psion_cpp_sdk_linux directory}"
HERE=$(cd "$(dirname "$0")" && pwd)
TOP=$(cd "$HERE/.." && pwd)
export EPOCROOT=$PSION_SDK/epoc_cpp_sdk
REL=$EPOCROOT/epoc32/release/marm/rel
export PATH=/usr/bin:/bin:$PSION_SDK/gcc-3.0-psion-98r2-9/bin:$EPOCROOT/epoc32/tools
export TMP=$EPOCROOT/tmp WINEDEBUG=-all
mkdir -p "$TMP" "$EPOCROOT/ptproj" "$TOP/build"
if [ -e "$EPOCROOT/ptproj/lxprobe" ] && [ ! -L "$EPOCROOT/ptproj/lxprobe" ]; then
	echo "$EPOCROOT/ptproj/lxprobe exists and is not a link - move it away"; exit 1
fi
ln -sfn "$HERE" "$EPOCROOT/ptproj/lxprobe"
( cd "$EPOCROOT/ptproj/lxprobe"
  makmake lxprobe marm > "$TOP/build/lxprobe-makmake.log" 2>&1
  make -f lxprobe.marm rel > "$TOP/build/lxprobe.log" 2>&1 \
	|| { grep -i -B1 -A4 "error\|undefined" "$TOP/build/lxprobe.log" | head -40; exit 1; } )
make -C "$HERE" SDK="$PSION_SDK" > "$TOP/build/lxprobe-exe.log" 2>&1 \
	|| { grep -i -B1 -A4 "error\|undefined" "$TOP/build/lxprobe-exe.log" | head -40; exit 1; }
P=$TOP/build/lxprobe-pkg
rm -rf "$P"; mkdir -p "$P"
cp "$REL/lxprobe.app" "$REL/lxprobe.rsc" "$TOP/build/lxprobe-obj/lxprobe.exe" "$P/"
ls -la "$P"
if [ "$1" = sis ]; then
	cp "$HERE/lxprobe.pkg" "$P/"
	( cd "$P" && wine "$EPOCROOT/epoc32/tools/makesis.exe" lxprobe.pkg "$TOP/dist/LxProbe-test.sis" > /dev/null )
	ls -la "$TOP/dist/LxProbe-test.sis"
fi
