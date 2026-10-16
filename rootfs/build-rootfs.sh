#!/bin/bash
# Builds PsiLinux's root file system, ROOT.IMG: BusyBox (NOMMU), the other
# programs (linux/ports), /etc, and the tests; then packs it with mkroot.py.
#   linux/tools/lxbuild "linux/rootfs/build-rootfs.sh [busybox|ports|image]"
# Needs the toolchain ($LXTC, linux/toolchain/build-toolchain.sh). Sources
# are downloaded to $LXSRC; the result is build/psilinux-root/ROOT.IMG.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
WORK=${PSION_WORK:-$(cd "$REPO/.." && pwd)}
LXTC=${LXTC:-$WORK/psilinux-toolchain}
LXSRC=${LXSRC:-$WORK/psilinux-src}
export PATH=$LXTC/bin:$PATH
OUT=$REPO/build/psilinux-root
STAGE=$OUT/stage
BB_VER=1.36.1
BB_SHA=b8cc24c9574d809e7279c3be349795c5d5ceb6fdf19ca709f80cde50e47de314
JOBS=$(nproc)
what=${1:-all}
mkdir -p "$LXSRC" "$OUT"

fetch() {    # url sha256
	local f=$LXSRC/$(basename "$1")
	[ -f "$f" ] || curl -sSfL --retry 3 -o "$f" "$1"
	echo "$2  $f" | sha256sum -c --quiet
}

busybox() {
	fetch https://busybox.net/downloads/busybox-$BB_VER.tar.bz2 $BB_SHA
	local B=$LXSRC/busybox-build
	rm -rf "$B"; mkdir -p "$B"
	tar xjf "$LXSRC/busybox-$BB_VER.tar.bz2" -C "$B"
	cd "$B/busybox-$BB_VER"
	for p in "$HERE"/patches/busybox-*.diff; do [ -f "$p" ] && patch -p1 -s < "$p"; done
	make defconfig > /dev/null
	# our changes: each CONFIG_X line replaces X's line
	while IFS= read -r line; do
		case "$line" in
		"# CONFIG_"*" is not set") k=${line#\# }; k=${k%% *} ;;
		CONFIG_*=*) k=${line%%=*} ;;
		*) continue ;;
		esac
		sed -i "/^$k=/d; /^# $k is not set/d" .config
		echo "$line" >> .config
	done < "$HERE/busybox.config"
	yes "" | make oldconfig > /dev/null 2>&1
	make -j$JOBS CC=lx-cc HOSTCC=gcc CROSS_COMPILE=arm-linux-musleabi- busybox > "$OUT/busybox.log" 2>&1 \
		|| { grep -E "error|Error" "$OUT/busybox.log" | head -30; exit 1; }
	arm-linux-musleabi-strip -o "$OUT/busybox" busybox_unstripped
	./applets/busybox.mkll > /dev/null 2>&1 || true
	make CC=lx-cc CROSS_COMPILE=arm-linux-musleabi- busybox.links > /dev/null
	cp busybox.links "$OUT/busybox.links"
	cp LICENSE "$OUT/busybox.LICENSE"
	ls -la "$OUT/busybox"
}

image() {
	rm -rf "$STAGE"
	mkdir -p "$STAGE"/{bin,sbin,usr/bin,usr/sbin,usr/lib,usr/share,etc,root,home,tmp,dev,proc,var/log,var/tmp,mnt,c,d,opt}
	install -m 755 "$OUT/busybox" "$STAGE/bin/busybox"
	# the applets: links to /bin/busybox
	while read -r a; do
		a=${a#/}
		[ "$a" = bin/busybox ] && continue
		mkdir -p "$STAGE/$(dirname "$a")"
		rel=$(realpath --relative-to="$STAGE/$(dirname "$a")" "$STAGE/bin/busybox")
		ln -sf "$rel" "$STAGE/$a"
	done < "$OUT/busybox.links"
	cp -a "$HERE/etc/." "$STAGE/etc/"
	# the licences, and where every source came from (BusyBox and nano are GPL)
	mkdir -p "$STAGE/usr/share/licenses/busybox" "$STAGE/usr/share/licenses/musl"
	cp "$OUT/busybox.LICENSE" "$STAGE/usr/share/licenses/busybox/LICENSE"
	cp "$LXSRC/musl-build/musl-1.2.6/COPYRIGHT" "$STAGE/usr/share/licenses/musl/COPYRIGHT"
	cp "$HERE/SOURCES.txt" "$STAGE/usr/share/licenses/SOURCES.txt"
	# the other programs (linux/ports), and the tests
	[ -d "$OUT/ports" ] && cp -a "$OUT/ports/." "$STAGE/"
	mkdir -p "$STAGE/usr/lib/psilinux/tests"
	for t in "$REPO"/linux/tests/*.c; do
		n=$(basename "$t" .c)
		lx-cc -Os -s -o "$STAGE/usr/lib/psilinux/tests/$n" "$t" -lm
	done
	cp "$REPO"/linux/tests/*.sh "$STAGE/usr/lib/psilinux/tests/" 2>/dev/null || true
	chmod 755 "$STAGE"/usr/lib/psilinux/tests/*
	ln -sf ../lib/psilinux/tests/hello "$STAGE/usr/bin/hello"
	python3 "$REPO/linux/tools/mkroot.py" "$STAGE" "$OUT/ROOT.IMG" --parts 1024    # (in parts: DECISIONS.md 54)
}

case "$what" in
busybox) busybox ;;
image) image ;;
all) busybox; [ -x "$HERE/../ports/build-ports.sh" ] && "$HERE/../ports/build-ports.sh"; image ;;
*) echo "build-rootfs.sh [busybox|image|all]"; exit 2 ;;
esac
