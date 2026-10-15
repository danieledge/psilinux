#!/bin/bash
# Builds the PsiLinux toolchain: gcc + binutils + musl (patched for PsiLinux)
# for arm-linux-musleabi, ARMv4T, ARM mode, soft float. Reproducible: every
# source is pinned (musl-cross-make's commit and its sha1 hashes).
#   linux/tools/lxbuild toolchain
# Output: $LXTC (default ../psilinux-toolchain next to this checkout), with
# lx-cc / lx-c++ wrappers that build static-pie programs for PsiLinux.
# Sources are cached in $LXSRC (default ../psilinux-src). Nothing built here
# goes into the repository.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
WORK=${PSION_WORK:-$(cd "$REPO/.." && pwd)}
LXTC=${LXTC:-$WORK/psilinux-toolchain}
LXSRC=${LXSRC:-$WORK/psilinux-src}
MCM_URL=https://github.com/richfelker/musl-cross-make.git
MCM_REV=227df8b99103f9c59f6570babf892978e293082f
GCC_VER=14.3.0
BINUTILS_VER=2.44
MUSL_VER=1.2.6
TARGET=arm-linux-musleabi
mkdir -p "$LXSRC"
cd "$LXSRC"
[ -d mcm ] || git clone -q "$MCM_URL" mcm
git -C mcm fetch -q origin 2>/dev/null || true
git -C mcm checkout -q "$MCM_REV"
# our musl patch, applied by musl-cross-make after its own
mkdir -p mcm/patches/musl-$MUSL_VER
cp "$REPO/linux/toolchain/patches/musl-$MUSL_VER-psilinux.diff" mcm/patches/musl-$MUSL_VER/9000-psilinux.diff
cat > mcm/config.mak <<CFG
TARGET = $TARGET
OUTPUT = $LXTC
GCC_VER = $GCC_VER
BINUTILS_VER = $BINUTILS_VER
MUSL_VER = $MUSL_VER
DL_CMD = curl -sSfL --retry 3 -o
GNU_SITE = https://ftp.gnu.org/gnu
COMMON_CONFIG += --disable-nls CFLAGS="-O2 -g0" CXXFLAGS="-O2 -g0" LDFLAGS="-s"
GCC_CONFIG += --with-arch=armv4t --with-mode=arm --with-float=soft --with-abi=aapcs-linux
GCC_CONFIG += --enable-languages=c,c++ --disable-libquadmath --disable-libsanitizer --disable-libitm --disable-libgomp
MUSL_CONFIG += CFLAGS="-Os -fPIC -march=armv4t -marm"
CFG
if [ "$1" != --musl-only ]; then
	make -C mcm -j"$(nproc)" install > "$LXSRC/toolchain.log" 2>&1 || { tail -40 "$LXSRC/toolchain.log"; exit 1; }
fi

# musl again, by itself, from the release tarball and our patch: position
# independent (static-pie links it into PIE programs) and ARMv4T. This is
# also how a change to the patch is tried: build-toolchain.sh --musl-only
M=$LXSRC/musl-build
rm -rf "$M"; mkdir -p "$M"
tar xzf "$LXSRC/mcm/sources/musl-$MUSL_VER.tar.gz" -C "$M"
( cd "$M/musl-$MUSL_VER"
  for p in "$LXSRC"/mcm/patches/musl-$MUSL_VER/*; do [ -f "$p" ] && patch -p1 -s < "$p"; done
  PATH="$LXTC/bin:$PATH" ./configure --prefix="$LXTC/$TARGET" --target=$TARGET --disable-shared \
	CC="$LXTC/bin/$TARGET-gcc" CFLAGS="-Os -fPIC -march=armv4t -marm -mfloat-abi=soft" > "$LXSRC/musl.log" 2>&1
  PATH="$LXTC/bin:$PATH" make -j"$(nproc)" >> "$LXSRC/musl.log" 2>&1 && PATH="$LXTC/bin:$PATH" make install >> "$LXSRC/musl.log" 2>&1 ) \
	|| { tail -30 "$LXSRC/musl.log"; exit 1; }

# GCC's __sync_* atomics without the kernel's kuser helpers (EPOC has
# none: see psi_atomic.c). lx-cc links this into every program.
"$LXTC/bin/$TARGET-gcc" -march=armv4t -marm -mfloat-abi=soft -Os -fPIE -c \
	-o "$LXTC/$TARGET/lib/psi_atomic.o" "$REPO/linux/toolchain/psi_atomic.c"

# wrappers: static-pie, 4 KB pages (no 64 KB gaps between the segments),
# a 128 KB stack unless the program asks for another (-Wl,-z,stack-size=N)
for t in cc:gcc c++:g++; do
	cat > "$LXTC/bin/lx-${t%%:*}" <<WR
#!/bin/sh
# PsiLinux: build a static-pie ARMv4T program (linux/toolchain/build-toolchain.sh)
D=\$(dirname "\$0")
case " \$* " in *" -c "*|*" -S "*|*" -E "*|*" -M "*|*" -MM "*|*" -r "*|*" -Wl,-r "*|*" -nostdlib "*|*" -shared "*) exec "\$D/$TARGET-${t#*:}" -march=armv4t -marm -mfloat-abi=soft -fPIE "\$@" ;; esac
# (-s is done after the check below, which needs the symbols)
strip=; n=\$#
for a in "\$@"; do
	if [ "\$a" = -s ]; then strip=1; else set -- "\$@" "\$a"; fi
done
shift \$n
"\$D/$TARGET-${t#*:}" -march=armv4t -marm -mfloat-abi=soft -fPIE -static-pie -Wl,-z,max-page-size=4096 -Wl,-z,common-page-size=4096 -Wl,-z,stack-size=131072 "\$D/../$TARGET/lib/psi_atomic.o" "\$@" || exit \$?
# the safety net: no svc outside __psi_svc (on EPOC an svc restarts the
# Psion). The linked program still has its mapping symbols, so objdump tells
# code from data here, as the runner cannot.
out=a.out; prev=
for a in "\$@"; do [ "\$prev" = -o ] && out=\$a; prev=\$a; done
case "\$out" in -) exit 0 ;; esac
[ -f "\$out" ] || exit 0
bad=\$("\$D/$TARGET-objdump" -d "\$out" 2>/dev/null | awk '/^[0-9a-f]+ <.*>:\$/ { f = \$2 } /\\t(svc|swi)[ \\t]/ && f != "<__psi_svc>:" { print f; n++ } END { exit n > 0 }') || {
	echo "lx-${t%%:*}: \$out has system call instructions outside __psi_svc (in \$bad): it would restart the Psion" >&2
	rm -f "\$out"; exit 1; }
[ -n "\$strip" ] && "\$D/$TARGET-strip" "\$out"
exit 0
WR
	chmod +x "$LXTC/bin/lx-${t%%:*}"
done
# No svc may be left in libc (other than __psi_svc's own fallback)
n=$("$LXTC/bin/$TARGET-objdump" -d "$LXTC/$TARGET/lib/libc.a" | grep -cE "\s(svc|swi)\s" || true)
echo "svc instructions in libc.a: $n (1 expected: __psi_svc)"
echo "Toolchain ready in $LXTC"
