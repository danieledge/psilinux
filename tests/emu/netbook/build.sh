#!/bin/bash
# Builds the netBook emulator's test launcher (lxlaunch.cpp), in psion-build:
#   core/tools/docker/psibuild "bash tests/emu/netbook/build.sh"
# -> build/nbtest/lxlaunch.app, .rsc. Then nbimage.mts makes the test image.
set -e
: "${PSION_SDK:?set PSION_SDK}"
HERE=$(cd "$(dirname "$0")" && pwd)
TOP=$(cd "$HERE/../../.." && pwd)
export EPOCROOT=$PSION_SDK/epoc_cpp_sdk
REL=$EPOCROOT/epoc32/release/marm/rel
export PATH=/usr/bin:/bin:$PSION_SDK/gcc-3.0-psion-98r2-9/bin:$EPOCROOT/epoc32/tools
export TMP=$EPOCROOT/tmp WINEDEBUG=-all
L=$TOP/build/nbtest; mkdir -p "$L"
[ -e "$EPOCROOT/ptproj/lxlaunch" ] && [ ! -L "$EPOCROOT/ptproj/lxlaunch" ] && { echo "ptproj/lxlaunch is not a link"; exit 1; }
ln -sfn "$HERE" "$EPOCROOT/ptproj/lxlaunch"
( cd "$EPOCROOT/ptproj/lxlaunch" && makmake lxlaunch marm > "$L/makmake.log" 2>&1 && make -f lxlaunch.marm rel > "$L/build.log" 2>&1 ) \
	|| { grep -i -B1 -A4 "error\|undefined" "$L/build.log" | head -30; exit 1; }
rm -f "$HERE"/lxlaunch.marm "$HERE"/*.MARM 2>/dev/null || :
( cd "$EPOCROOT/ptproj/lxlaunch" && makmake lxswitch marm > "$L/makmake-sw.log" 2>&1 && make -f lxswitch.marm rel > "$L/build-sw.log" 2>&1 ) \
	|| { grep -i -B1 -A4 "error\|undefined" "$L/build-sw.log" | head -30; exit 1; }
rm -f "$HERE"/lxswitch.marm "$HERE"/*.MARM 2>/dev/null || :
cp "$REL/lxlaunch.app" "$REL/lxlaunch.rsc" "$L/"
cp "$REL/lxswitch.exe" "$L/"
ls -la "$L/lxlaunch.app"
