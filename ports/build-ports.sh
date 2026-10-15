#!/bin/bash
# Builds PsiLinux's other programs (beyond BusyBox) with the PsiLinux
# toolchain, into build/psilinux-root/ports/ (laid out as the root: usr/bin...),
# which linux/rootfs/build-rootfs.sh packs into ROOT.IMG.
#   linux/tools/lxbuild "linux/ports/build-ports.sh [NAME...]"
# Every source is pinned by version and sha256. Sources are cached in $LXSRC.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
WORK=${PSION_WORK:-$(cd "$REPO/.." && pwd)}
LXTC=${LXTC:-$WORK/psilinux-toolchain}
LXSRC=${LXSRC:-$WORK/psilinux-src}
export PATH=$LXTC/bin:$PATH
T=arm-linux-musleabi
OUT=$REPO/build/psilinux-root
P=$OUT/ports                      # what goes into the root
# libraries the ports build against (not in the root), in $OUT/sysroot, used
# through a neutral path: some programs keep their build flags (git, jq)
SYS=/tmp/psilinux-sysroot
mkdir -p "$OUT/sysroot"
ln -sfn "$OUT/sysroot" "$SYS"
# (and the build folder too: $LXSRC/ports-build, as /tmp/psilinux-build)
mkdir -p "$LXSRC/ports-build"
ln -sfn "$LXSRC/ports-build" /tmp/psilinux-build
B=/tmp/psilinux-build
JOBS=$(nproc)
mkdir -p "$P/usr/bin" "$P/usr/share" "$SYS/include" "$SYS/lib" "$B"
CFG="--host=$T --prefix=/usr --disable-shared --enable-static"
export CC=lx-cc CXX=lx-c++ AR=$T-ar RANLIB=$T-ranlib STRIP=$T-strip
# (no build folders in the programs: __FILE__ and debug names start /src)
export CFLAGS="-Os -ffile-prefix-map=$B=/src" CPPFLAGS="-I$SYS/include" LDFLAGS="-L$SYS/lib"

fetch() {    # url sha256 -> the file's path
	local f=$LXSRC/$(basename "$1")
	[ -f "$f" ] || curl -sSfL --retry 3 -o "$f" "$1"
	echo "$2  $f" | sha256sum -c --quiet >&2
	echo "$f"
}

unpack() {   # file -> fresh folder in $B
	local f=$1 d
	d=$B/$(basename "$f" | sed -E 's/\.(tar\.(gz|xz|bz2)|zip)$//')
	rm -rf "$d"
	case "$f" in
	*.zip) (cd "$B" && unzip -q "$f") ;;
	*) tar xf "$f" -C "$B" ;;
	esac
	echo "$d"
}

log() { echo "== $*"; }
lic() {      # name file...: the licence, into /usr/share/licenses/<name>/
	mkdir -p "$P/usr/share/licenses/$1"
	local n=$1; shift
	cp "$@" "$P/usr/share/licenses/$n/"
}
put() {      # binary -> /usr/bin (stripped)
	$STRIP -o "$P/usr/bin/$(basename "${2:-$1}")" "$1"
	ls -la "$P/usr/bin/$(basename "${2:-$1}")" | awk '{print "   " $5 " bytes " $9}'
}

lua() {
	log "Lua 5.4.7"
	d=$(unpack "$(fetch https://www.lua.org/ftp/lua-5.4.7.tar.gz 9fbf5e28ef86c69858f6d3d34eccc32e911c1a28b4120ff3e84aaa70cfbf1e30)")
	make -C "$d/src" posix CC=lx-cc AR="$T-ar rcu" RANLIB=$RANLIB MYCFLAGS=-Os > "$B/lua.log" 2>&1 || { tail -20 "$B/lua.log"; exit 1; }
	put "$d/src/lua"
	put "$d/src/luac"
	mkdir -p "$P/usr/share/licenses/lua"
	sed -n '/Copyright (C) 1994/,/SOFTWARE\./p' "$d/src/lua.h" > "$P/usr/share/licenses/lua/COPYRIGHT"
}

sqlite() {
	log "SQLite 3.46.1"
	d=$(unpack "$(fetch https://www.sqlite.org/2024/sqlite-amalgamation-3460100.zip 77823cb110929c2bcb0f5d48e4833b5c59a8a6e40cdea3936b99e199dbbe5784)")
	lx-cc $CFLAGS -DSQLITE_THREADSAFE=0 -DSQLITE_OMIT_LOAD_EXTENSION -DSQLITE_DEFAULT_MEMSTATUS=0 \
		-o "$d/sqlite3" "$d/shell.c" "$d/sqlite3.c" -lm > "$B/sqlite.log" 2>&1 || { tail -20 "$B/sqlite.log"; exit 1; }
	put "$d/sqlite3"
	mkdir -p "$P/usr/share/licenses/sqlite"
	echo "SQLite is in the public domain: https://www.sqlite.org/copyright.html" > "$P/usr/share/licenses/sqlite/COPYRIGHT"
}

micropython() {
	log "MicroPython 1.23.0"
	d=$(unpack "$(fetch https://github.com/micropython/micropython/releases/download/v1.23.0/micropython-1.23.0.tar.xz 0ab283c2fc98d466c1ff26692bee46abaeeab55d488a36fc3cb6372cb8fb390d)")
	( unset CC CXX AR RANLIB STRIP CFLAGS CPPFLAGS LDFLAGS; make -C "$d/mpy-cross" -j$JOBS ) > "$B/mpy-cross.log" 2>&1 || { tail -20 "$B/mpy-cross.log"; exit 1; }
	# no FFI (no dlopen), no threads, no SSL (BusyBox wget and curl do HTTPS)
	( unset CFLAGS LDFLAGS CPPFLAGS
	  make -C "$d/ports/unix" -j$JOBS CROSS_COMPILE=$T- CC=lx-cc LD=lx-cc STRIP=$STRIP \
		MICROPY_PY_FFI=0 MICROPY_PY_THREAD=0 MICROPY_PY_SSL=0 MICROPY_PY_BTREE=0 MICROPY_PY_SOCKET=1 \
		MICROPY_STANDALONE=1 CFLAGS_EXTRA="-DMICROPY_GCREGS_SETJMP=1" ) > "$B/micropython.log" 2>&1 \
		|| { grep -E "error|Error" "$B/micropython.log" | head -30; exit 1; }
	put "$d/ports/unix/build-standard/micropython"
	lic micropython "$d/LICENSE"
}

jq() {
	log "jq 1.7.1"
	d=$(unpack "$(fetch https://github.com/jqlang/jq/releases/download/jq-1.7.1/jq-1.7.1.tar.gz 478c9ca129fd2e3443fe27314b455e211e0d8c60bc8ff7df703873deeee580c2)")
	( cd "$d" && ./configure $CFG --disable-docs --without-oniguruma --disable-maintainer-mode > "$B/jq.log" 2>&1 \
	  && make -j$JOBS >> "$B/jq.log" 2>&1 ) || { tail -30 "$B/jq.log"; exit 1; }
	put "$d/jq"
	lic jq "$d/COPYING"
}

zlib() {
	log "zlib 1.3.1 (a library)"
	d=$(unpack "$(fetch https://zlib.net/fossils/zlib-1.3.1.tar.gz 9a93b2b7dfdac77ceba5a558a580e74667dd6fede4585b91eefb60f03b72df23)")
	( cd "$d" && CHOST=$T ./configure --static --prefix="$SYS" > "$B/zlib.log" 2>&1 && make -j$JOBS libz.a >> "$B/zlib.log" 2>&1 \
	  && make install >> "$B/zlib.log" 2>&1 ) || { tail -20 "$B/zlib.log"; exit 1; }
}

ncurses() {
	log "ncurses 6.4 (a library; xterm-256color, vt100 and linux built in)"
	d=$(unpack "$(fetch https://ftp.gnu.org/gnu/ncurses/ncurses-6.4.tar.gz 6931283d9ac87c5073f30b6290c4c75f21632bb4fc3603ac8100812bed248159)")
	( cd "$d" && ./configure --host=$T --prefix="$SYS" --without-cxx --without-cxx-binding --without-ada --without-tests \
		--without-progs --without-manpages --disable-database --with-fallbacks=xterm-256color,xterm,vt100,linux \
		--enable-widec --disable-stripping --without-debug > "$B/ncurses.log" 2>&1 \
	  && make -j$JOBS >> "$B/ncurses.log" 2>&1 && make install >> "$B/ncurses.log" 2>&1 ) || { tail -30 "$B/ncurses.log"; exit 1; }
}

nano() {
	log "nano 8.2"
	[ -f "$SYS/lib/libncursesw.a" ] || ncurses
	d=$(unpack "$(fetch https://www.nano-editor.org/dist/v8/nano-8.2.tar.xz d5ad07dd862facae03051c54c6535e54c7ed7407318783fcad1ad2d7076fffeb)")
	( cd "$d" && PKG_CONFIG=false NCURSESW_CFLAGS="-I$SYS/include/ncursesw -I$SYS/include" NCURSESW_LIBS="-L$SYS/lib -lncursesw" \
		./configure $CFG --disable-nls --disable-libmagic --disable-speller > "$B/nano.log" 2>&1 \
	  && make -j$JOBS >> "$B/nano.log" 2>&1 ) || { tail -30 "$B/nano.log"; exit 1; }
	put "$d/src/nano"
	lic nano "$d/COPYING"
	lic ncurses "$B/ncurses-6.4/COPYING"
}

mbedtls() {
	log "Mbed TLS 3.6.2 (a library)"
	d=$(unpack "$(fetch https://github.com/Mbed-TLS/mbedtls/releases/download/mbedtls-3.6.2/mbedtls-3.6.2.tar.bz2 8b54fb9bcf4d5a7078028e0520acddefb7900b3e66fec7f7175bb5b7d85ccdca)")
	( cd "$d" && make -C library -j$JOBS CC=lx-cc AR=$AR CFLAGS="-Os -I../include" > "$B/mbedtls.log" 2>&1 \
	  && cp -r include/mbedtls include/psa "$SYS/include/" && cp library/*.a "$SYS/lib/" ) || { tail -30 "$B/mbedtls.log"; exit 1; }
}

curl() {
	log "curl 8.10.1 (with Mbed TLS)"
	[ -f "$SYS/lib/libmbedtls.a" ] || mbedtls
	[ -f "$SYS/lib/libz.a" ] || zlib
	d=$(unpack "$(fetch https://curl.se/download/curl-8.10.1.tar.xz 73a4b0e99596a09fa5924a4fb7e4b995a85fda0d18a2c02ab9cf134bebce04ee)")
	( cd "$d" && ./configure $CFG --with-mbedtls="$SYS" --with-zlib="$SYS" --without-libpsl --without-brotli --without-zstd \
		--without-libidn2 --without-nghttp2 --disable-ldap --disable-rtsp --disable-dict --disable-telnet --disable-tftp \
		--disable-pop3 --disable-imap --disable-smtp --disable-gopher --disable-mqtt --disable-manual --disable-docs \
		--disable-threaded-resolver --with-ca-bundle=/etc/ssl/certs/ca-certificates.crt > "$B/curl.log" 2>&1 \
	  && make -j$JOBS >> "$B/curl.log" 2>&1 ) || { tail -30 "$B/curl.log"; exit 1; }
	put "$d/src/curl"
	# libcurl too, for git
	cp "$d/lib/.libs/libcurl.a" "$SYS/lib/" && mkdir -p "$SYS/include/curl" && cp "$d"/include/curl/*.h "$SYS/include/curl/"
	lic curl "$d/COPYING"
	lic mbedtls "$B/mbedtls-3.6.2/LICENSE"
	lic zlib "$B/zlib-1.3.1/LICENSE"
	mkdir -p "$P/etc/ssl/certs"
	cp "$(fetch https://curl.se/ca/cacert-2024-09-24.pem 189d3cf6d103185fba06d76c1af915263c6d42225481a1759e853b33ac857540)" "$P/etc/ssl/certs/ca-certificates.crt"
}

git() {
	log "git 2.47.0 (https through libcurl and Mbed TLS)"
	[ -f "$SYS/lib/libcurl.a" ] || curl
	d=$(unpack "$(fetch https://mirrors.edge.kernel.org/pub/software/scm/git/git-2.47.0.tar.xz 1ce114da88704271b43e027c51e04d9399f8c88e9ef7542dae7aebae7d87bc4e)")
	cat > "$d/config.mak" <<CFG
CC = lx-cc
AR = $AR
uname_S = Linux
uname_M = armv4tl
prefix = /usr
CFLAGS = -Os
LDFLAGS = -L$SYS/lib
NO_PERL = 1
NO_PYTHON = 1
NO_TCLTK = 1
NO_GETTEXT = 1
NO_EXPAT = 1
NO_OPENSSL = 1
NO_REGEX = NeedsStartEnd
CURLDIR = $SYS
CURL_LDFLAGS = -lcurl -lmbedtls -lmbedx509 -lmbedcrypto -lz
ZLIB_PATH = $SYS
SKIP_DASHED_BUILT_INS = 1
NO_INSTALL_HARDLINKS = 1
CFG
	( cd "$d" && make -j$JOBS git git-remote-http > "$B/git.log" 2>&1 ) || { grep -E "error|Error" "$B/git.log" | head -30; exit 1; }
	put "$d/git"
	mkdir -p "$P/usr/libexec/git-core"
	$STRIP -o "$P/usr/libexec/git-core/git-remote-http" "$d/git-remote-http"
	ln -sf git-remote-http "$P/usr/libexec/git-core/git-remote-https"
	ls -la "$P/usr/libexec/git-core/git-remote-http" | awk '{print "   " $5 " bytes git-remote-http"}'
	mkdir -p "$P/usr/share/git-core/templates"
	lic git "$d/COPYING"
}

all="lua sqlite micropython jq nano curl git"
for p in ${@:-$all}; do $p; done
du -sh "$P"
