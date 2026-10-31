#!/bin/bash
# Builds a minimal X11 for PsiLinux with the PsiLinux toolchain: the kdrive
# frame-buffer server Xfbdev (xorg-server 1.19.7, the last release with it),
# the client libraries (static), and a few clients, into
# build/psilinux-root/x11/ (laid out as the root: usr/bin...). It is not in
# ROOT.IMG unless copied into build/psilinux-root/ports/ (or X11OUT=... is
# set to that) before rootfs/build-rootfs.sh image.
#   tools/lxbuild "ports/build-x11.sh [NAME...]"
# Every source is pinned by version and sha256 and cached in $LXSRC.
# Patches (ports/patches/): xorg-server-1.19.7-psilinux.diff (the Psion's
# OS and input layer, PSIFB_DAMAGE, a precompiled keymap, a file as frame
# buffer for testing, smaller limits and queues, Render's colour tables made
# when first used), libX11-1.8.10-psilinux.diff (XLIB_NO_CJK: no CJK
# conversion tables, 400 KB less in every client) and
# pixman-0.42.2-psilinux.diff (PIXMAN_NO_FLOAT: no floating-point path).
#
# The server:
#   /usr/bin/Xfbdev, linked as /usr/bin/X: /dev/fb0 (8 bits, pseudo-colour),
#   keyboard /dev/input/event0, pen /dev/input/event1; fonts "fixed" and
#   "cursor" are built in (libXfont2's built-ins, no font files); the keymap
#   is /usr/share/X11/xkb/psi/<layout>.xkm, made here with xkbcomp, so the
#   server never runs xkbcomp. Run it as
#       X :0 -nolisten tcp -noreset -dumbSched &
#       DISPLAY=:0 xsetroot -solid grey50; DISPLAY=:0 xeyes &
#   (-dumbSched: no 20 ms SIGALRM timer; -noreset: no screen rebuild when the
#   last client goes.) Testing without /dev/fb0, e.g. under qemu-arm:
#       : > fb.raw; Xfbdev :1 -fb fb.raw -xkbdir $ROOT/usr/share/X11/xkb
#   (a plain file is a 640x240x8 frame buffer, its colour map after the
#   pixels; PSIX_FB=WxHxD, PSIX_DAMAGE_LOG=1 prints each PSIFB_DAMAGE.)
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/.." && pwd)
WORK=${PSION_WORK:-$(cd "$REPO/.." && pwd)}
LXTC=${LXTC:-$WORK/psilinux-toolchain}
LXSRC=${LXSRC:-$WORK/psilinux-src}
export PATH=$LXTC/bin:$PATH
T=arm-linux-musleabi
OUT=$REPO/build/psilinux-root
P=${X11OUT:-$OUT/x11}             # what goes into the root
PATCHES=$HERE/patches
# the libraries and headers (not in the root): installed with DESTDIR into a
# sysroot, used through a neutral path, as build-ports.sh does
SYS=/tmp/psilinux-x11-sysroot
mkdir -p "$OUT/x11-sysroot"
ln -sfn "$OUT/x11-sysroot" "$SYS"
mkdir -p "$LXSRC/x11-build"
ln -sfn "$LXSRC/x11-build" /tmp/psilinux-x11-build
B=/tmp/psilinux-x11-build
JOBS=$(nproc)
mkdir -p "$P/usr/bin" "$P/usr/share" "$SYS/usr/include" "$SYS/usr/lib" "$B"
CFG="--host=$T --build=$(gcc -dumpmachine) --prefix=/usr --sysconfdir=/etc --localstatedir=/var
	--disable-shared --enable-static --disable-dependency-tracking --disable-silent-rules"
export CC=lx-cc CXX=lx-c++ AR=$T-ar RANLIB=$T-ranlib STRIP=$T-strip
# (sections, so that the static programs keep only what they use)
export CFLAGS="-Os -ffunction-sections -fdata-sections -ffile-prefix-map=$B=/src"
export CPPFLAGS="-I$SYS/usr/include" LDFLAGS="-L$SYS/usr/lib -Wl,--gc-sections"
export PKG_CONFIG="pkg-config --static" PKG_CONFIG_PATH= PKG_CONFIG_SYSROOT_DIR=$SYS
export PKG_CONFIG_LIBDIR=$SYS/usr/lib/pkgconfig:$SYS/usr/share/pkgconfig
XORG=https://www.x.org/releases/individual

fetch() {    # url sha256 -> the file's path
	local f=$LXSRC/$(basename "$1")
	[ -f "$f" ] || curl -sSfL --retry 3 -o "$f" "$1"
	echo "$2  $f" | sha256sum -c --quiet >&2
	echo "$f"
}

unpack() {   # file -> fresh folder in $B
	local f=$1 d
	d=$B/$(basename "$f" | sed -E 's/\.(tar\.(gz|xz|bz2)|tgz|zip)$//')
	rm -rf "$d"
	tar xf "$f" -C "$B"
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

# ac name dir [configure args...]: configure, make, install into the sysroot
ac() {
	local n=$1 d=$2; shift 2
	( cd "$d" && ./configure $CFG "$@" > "$B/$n.log" 2>&1 \
	  && make -j$JOBS >> "$B/$n.log" 2>&1 && make install DESTDIR="$SYS" >> "$B/$n.log" 2>&1 ) \
		|| { grep -E "error|Error" "$B/$n.log" | head -20; tail -15 "$B/$n.log"; exit 1; }
	rm -f "$SYS"/usr/lib/*.la
}
# need lib...: build the libraries not built yet (a stamp each, in the sysroot)
need() { local x; for x in "$@"; do [ -f "$SYS/.built-$x" ] || { $x; touch "$SYS/.built-$x"; }; done; }

# --- headers and the X protocol -------------------------------------------

xorgproto() {
	log "xorgproto 2024.1 (headers)"
	d=$(unpack "$(fetch $XORG/proto/xorgproto-2024.1.tar.xz 372225fd40815b8423547f5d890c5debc72e88b91088fbfb13158c20495ccb59)")
	ac xorgproto "$d" --disable-specs --without-xmlto --without-fop --without-xsltproc
	lic xorgproto "$d/COPYING-x11proto"
}

xcbproto() {
	log "xcb-proto 1.17.0 (protocol descriptions)"
	d=$(unpack "$(fetch $XORG/proto/xcb-proto-1.17.0.tar.xz 2c1bacd2110f4799f74de6ebb714b94cf6f80fb112316b1219480fd22562148c)")
	ac xcbproto "$d"
}

xtrans() {
	log "xtrans 1.5.0 (source, built into each user)"
	d=$(unpack "$(fetch $XORG/lib/xtrans-1.5.0.tar.xz 1ba4b703696bfddbf40bacf25bce4e3efb2a0088878f017a50e9884b0c8fb1bd)")
	ac xtrans "$d" --disable-docs --without-xmlto --without-fop
}

zlib() {
	log "zlib 1.3.1 (a library, for libXfont2)"
	d=$(unpack "$(fetch https://zlib.net/fossils/zlib-1.3.1.tar.gz 9a93b2b7dfdac77ceba5a558a580e74667dd6fede4585b91eefb60f03b72df23)")
	( cd "$d" && CHOST=$T ./configure --static --prefix=/usr > "$B/zlib.log" 2>&1 && make -j$JOBS libz.a >> "$B/zlib.log" 2>&1 \
	  && make install DESTDIR="$SYS" >> "$B/zlib.log" 2>&1 ) || { tail -20 "$B/zlib.log"; exit 1; }
	lic zlib "$d/LICENSE"
}

ncurses() {
	log "ncurses 6.4 (a library, for xterm's termcap; xterm, vt100 and linux built in)"
	d=$(unpack "$(fetch https://ftp.gnu.org/gnu/ncurses/ncurses-6.4.tar.gz 6931283d9ac87c5073f30b6290c4c75f21632bb4fc3603ac8100812bed248159)")
	( cd "$d" && ./configure --host=$T --prefix=/usr --without-cxx --without-cxx-binding --without-ada --without-tests \
		--without-progs --without-manpages --disable-database --with-fallbacks=xterm,vt100,linux \
		--disable-stripping --without-debug --with-termlib --enable-pc-files --with-pkg-config-libdir=/usr/lib/pkgconfig \
		> "$B/ncurses.log" 2>&1 \
	  && make -j$JOBS >> "$B/ncurses.log" 2>&1 && make install DESTDIR="$SYS" >> "$B/ncurses.log" 2>&1 ) \
		|| { tail -30 "$B/ncurses.log"; exit 1; }
	lic ncurses "$d/COPYING"
}

nettle() {
	log "Nettle 3.9.1 (a library: the server's SHA-1)"
	d=$(unpack "$(fetch https://ftp.gnu.org/gnu/nettle/nettle-3.9.1.tar.gz ccfeff981b0ca71bbd6fbcb054f407c60ffb644389a5be80d6716d5b550c6ce3)")
	( cd "$d" && ./configure $CFG --disable-public-key --disable-assembler --disable-documentation \
		--disable-openssl --disable-gcov --disable-fat > "$B/nettle.log" 2>&1 \
	  && make -j$JOBS libnettle.a >> "$B/nettle.log" 2>&1 \
	  && make install-headers install-static install-pkgconfig DESTDIR="$SYS" >> "$B/nettle.log" 2>&1 ) \
		|| { tail -30 "$B/nettle.log"; exit 1; }
	lic nettle "$d/COPYINGv3" "$d/COPYING.LESSERv3"
}

pixman() {
	log "pixman 0.42.2 (a library: the server's drawing)"
	d=$(unpack "$(fetch $XORG/lib/pixman-0.42.2.tar.xz 5747d2ec498ad0f1594878cc897ef5eb6c29e91c53b899f7f71b506785fc1376)")
	# (no ARMv6/NEON/iWMMXt paths on an ARM710T; generic C. No float path:
	# the patch, PIXMAN_NO_FLOAT)
	patch -d "$d" -p1 < "$PATCHES/pixman-0.42.2-psilinux.diff" > "$B/pixman.log"
	CFLAGS="$CFLAGS -DPIXMAN_NO_FLOAT" ac pixman "$d" --disable-openmp --disable-arm-simd --disable-arm-neon --disable-arm-a64-neon \
		--disable-arm-iwmmxt --disable-arm-iwmmxt2 --disable-gtk --disable-libpng --disable-loongson-mmi \
		--disable-mmx --disable-sse2 --disable-ssse3 --disable-vmx --disable-mips-dspr2
	lic pixman "$d/COPYING"
}

# --- client libraries -----------------------------------------------------

xau() {
	need xorgproto
	log "libXau 1.0.11"
	d=$(unpack "$(fetch $XORG/lib/libXau-1.0.11.tar.xz f3fa3282f5570c3f6bd620244438dbfbdd580fc80f02f549587a0f8ab329bbeb)")
	ac xau "$d"
	lic libXau "$d/COPYING"
}

xcb() {
	need xcbproto xau
	log "libxcb 1.17.0"
	d=$(unpack "$(fetch $XORG/lib/libxcb-1.17.0.tar.xz 599ebf9996710fea71622e6e184f3a8ad5b43d0e5fa8c4e407123c88a59a6d55)")
	ac xcb "$d" --disable-devel-docs --without-doxygen --disable-composite --disable-damage --disable-dbe \
		--disable-dpms --disable-dri2 --disable-dri3 --disable-glx --disable-present --disable-record \
		--disable-resource --disable-screensaver --disable-shm --disable-sync --disable-xevie \
		--disable-xfree86-dri --disable-xinerama --disable-xprint --disable-selinux --disable-xtest \
		--disable-xv --disable-xvmc --disable-xinput --disable-ge
	lic libxcb "$d/COPYING"
}

x11() {
	need xtrans xcb
	log "libX11 1.8.10"
	d=$(unpack "$(fetch $XORG/lib/libX11-1.8.10.tar.xz 2b3b3dad9347db41dca56beb7db5878f283bde1142f04d9f8e478af435dfdc53)")
	# (no Xcms, no XF86BigFont, no loadable modules; no CJK tables: the patch)
	patch -d "$d" -p1 < "$PATCHES/libX11-1.8.10-psilinux.diff" > "$B/x11.log"
	CFLAGS="$CFLAGS -DXLIB_NO_CJK" ac x11 "$d" --disable-specs --without-xmlto --without-fop --without-xsltproc --without-perl \
		--disable-ipv6 --disable-loadable-i18n --disable-loadable-xcursor \
		--disable-thread-safety-constructor --disable-xcms --disable-xf86bigfont --disable-composecache \
		--disable-malloc0returnsnull --without-launchd --without-lint
	lic libX11 "$d/COPYING"
	# the C locale's tables (enough for Xlib; no compose tables)
	mkdir -p "$P/usr/share/X11/locale/C"
	cp "$d/nls/C/XLC_LOCALE" "$P/usr/share/X11/locale/C/"
	printf 'C/XLC_LOCALE\t\t\tC\nC/XLC_LOCALE:\t\t\tC\n' > "$P/usr/share/X11/locale/locale.dir"
	printf 'C\t\t\t\t\tC\nPOSIX\t\t\t\t\tC\nC.UTF-8\t\t\t\t\tC\nen_GB.UTF-8\t\t\t\tC\nen_US.UTF-8\t\t\t\tC\n' > "$P/usr/share/X11/locale/locale.alias"
	: > "$P/usr/share/X11/locale/compose.dir"
}

xext() {
	need x11
	log "libXext 1.3.6"
	d=$(unpack "$(fetch $XORG/lib/libXext-1.3.6.tar.xz edb59fa23994e405fdc5b400afdf5820ae6160b94f35e3dc3da4457a16e89753)")
	ac xext "$d" --disable-specs --without-xmlto --without-fop --without-xsltproc --disable-malloc0returnsnull
	lic libXext "$d/COPYING"
}

xkbfile() {
	need x11
	log "libxkbfile 1.1.3"
	d=$(unpack "$(fetch $XORG/lib/libxkbfile-1.1.3.tar.xz a9b63eea997abb9ee6a8b4fbb515831c841f471af845a09de443b28003874bec)")
	ac xkbfile "$d"
	lic libxkbfile "$d/COPYING"
}

fontenc() {
	need xorgproto zlib
	log "libfontenc 1.1.8"
	d=$(unpack "$(fetch $XORG/lib/libfontenc-1.1.8.tar.xz 7b02c3d405236e0d86806b1de9d6868fe60c313628b38350b032914aa4fd14c6)")
	ac fontenc "$d" --with-encodingsdir=/usr/share/fonts/X11/encodings
	lic libfontenc "$d/COPYING"
}

xfont2() {
	need xtrans fontenc zlib
	log "libXfont2 2.0.6 (PCF and the built-in fonts only)"
	d=$(unpack "$(fetch $XORG/lib/libXfont2-2.0.6.tar.xz 74ca20017eb0fb3f56d8d5e60685f560fc85e5ff3d84c61c4cb891e40c27aef4)")
	ac xfont2 "$d" --disable-devel-docs --disable-freetype --disable-bdfformat --disable-snfformat \
		--disable-fc --disable-ipv6 --disable-tcp-transport --without-bzip2 --without-xmlto --without-fop
	lic libXfont2 "$d/COPYING"
}

xrender() {
	need x11
	log "libXrender 0.9.11"
	d=$(unpack "$(fetch $XORG/lib/libXrender-0.9.11.tar.xz bc53759a3a83d1ff702fb59641b3d2f7c56e05051fa0cfa93501166fa782dc24)")
	ac xrender "$d" --disable-malloc0returnsnull
	lic libXrender "$d/COPYING"
}

xfixes() {
	need x11
	log "libXfixes 6.0.1"
	d=$(unpack "$(fetch $XORG/lib/libXfixes-6.0.1.tar.xz b695f93cd2499421ab02d22744458e650ccc88c1d4c8130d60200213abc02d58)")
	ac xfixes "$d" --disable-malloc0returnsnull
	lic libXfixes "$d/COPYING"
}

xcursor() {
	need xrender xfixes
	log "libXcursor 1.2.2"
	d=$(unpack "$(fetch $XORG/lib/libXcursor-1.2.2.tar.xz 53d071bd2cc56e517a30998d5e685c8a74556ddada43c6985d14da9a023a88ee)")
	ac xcursor "$d" --with-icondir=/usr/share/icons --with-cursorpath=/usr/share/icons
	lic libXcursor "$d/COPYING"
}

ice() {
	need xtrans xorgproto
	log "libICE 1.1.1"
	d=$(unpack "$(fetch $XORG/lib/libICE-1.1.1.tar.xz 03e77afaf72942c7ac02ccebb19034e6e20f456dcf8dddadfeb572aa5ad3e451)")
	ac ice "$d" --disable-docs --disable-specs --without-xmlto --without-fop --without-xsltproc --disable-ipv6
	lic libICE "$d/COPYING"
}

sm() {
	need ice
	log "libSM 1.2.4"
	d=$(unpack "$(fetch $XORG/lib/libSM-1.2.4.tar.xz fdcbe51e4d1276b1183da77a8a4e74a137ca203e0bcfb20972dd5f3347e97b84)")
	ac sm "$d" --disable-docs --without-xmlto --without-fop --without-xsltproc --without-libuuid --disable-ipv6
	lic libSM "$d/COPYING"
}

xt() {
	need sm x11
	log "libXt 1.3.0"
	d=$(unpack "$(fetch $XORG/lib/libXt-1.3.0.tar.xz 52820b3cdb827d08dc90bdfd1b0022a3ad8919b57a39808b12591973b331bf91)")
	ac xt "$d" --disable-specs --without-xmlto --without-fop --without-xsltproc --disable-malloc0returnsnull \
		--with-appdefaultdir=/usr/share/X11/app-defaults
	lic libXt "$d/COPYING"
}

xmu() {
	need xt xext
	log "libXmu 1.2.1"
	d=$(unpack "$(fetch $XORG/lib/libXmu-1.2.1.tar.xz fcb27793248a39e5fcc5b9c4aec40cc0734b3ca76aac3d7d1c264e7f7e14e8b2)")
	ac xmu "$d" --disable-docs --without-xmlto --without-fop --without-xsltproc --disable-ipv6
	lic libXmu "$d/COPYING"
}

xaw() {
	need xmu
	log "libXaw 1.0.16 (Xaw6 only: no Xpm)"
	d=$(unpack "$(fetch $XORG/lib/libXaw-1.0.16.tar.xz 731d572b54c708f81e197a6afa8016918e2e06dfd3025e066ca642a5b8c39c8f)")
	ac xaw "$d" --disable-specs --without-xmlto --without-fop --without-xsltproc --disable-xaw7 --enable-xaw6
	lic libXaw "$d/COPYING"
}

xbitmaps() {
	log "xbitmaps 1.1.3 (headers)"
	d=$(unpack "$(fetch $XORG/data/xbitmaps-1.1.3.tar.xz ad6cad54887832a17d86c2ccfc5e52a1dfab090f8307b152c78b0e1529cd0f7a)")
	ac xbitmaps "$d"
}

# --- the keymap -----------------------------------------------------------

# xkbcomp is built for ARM and run under qemu-arm here (it is a static
# program), to compile the keymaps once; it is not put in the root.
keymaps() {
	need xkbfile
	log "xkbcomp 1.4.7 + xkeyboard-config 2.42: the precompiled keymaps"
	d=$(unpack "$(fetch $XORG/app/xkbcomp-1.4.7.tar.xz 0a288114e5f44e31987042c79aecff1ffad53a8154b8ec971c24a69a80f81f77)")
	ac xkbcomp "$d" --with-xkb-config-root=/usr/share/X11/xkb
	k=$(unpack "$(fetch $XORG/data/xkeyboard-config/xkeyboard-config-2.42.tar.xz a6b06ebfc1f01fc505f2f05f265f95f67cc8873a54dd247e3c2d754b8f7e0807)")
	mkdir -p "$P/usr/share/X11/xkb/psi"
	# evdev keycodes (X keycode = Linux KEY_* + 8), no geometry
	for m in gb:gb us:us; do
		cat > "$B/keymap-${m%%:*}.xkb" <<KM
xkb_keymap {
	xkb_keycodes { include "evdev+aliases(qwerty)" };
	xkb_types    { include "complete" };
	xkb_compat   { include "complete" };
	xkb_symbols  { include "pc+${m#*:}+inet(evdev)" };
};
KM
		qemu-arm "$d/xkbcomp" -w 0 -I"$k" -xkm "$B/keymap-${m%%:*}.xkb" "$P/usr/share/X11/xkb/psi/${m%%:*}.xkm" \
			|| { echo "xkbcomp failed for ${m%%:*}"; exit 1; }
	done
	ln -sf gb.xkm "$P/usr/share/X11/xkb/psi/default.xkm"
	ls -la "$P/usr/share/X11/xkb/psi" | awk 'NR>3 {print "   " $5 " bytes " $9}'
	lic xkeyboard-config "$k/COPYING"
}

# --- the server -----------------------------------------------------------

xserver() {
	need xorgproto xtrans xau xfont2 pixman nettle xkbfile
	log "xorg-server 1.19.7: Xfbdev (kdrive)"
	d=$(unpack "$(fetch $XORG/xserver/xorg-server-1.19.7.tar.bz2 7112f7128a4f5b06ceb8bba1bdc5e5c9e0fae682a42d35218bc12ba693f4c80c)")
	patch -d "$d" -p1 < "$PATCHES/xorg-server-1.19.7-psilinux.diff" > "$B/xserver.log"
	# ospoll: poll, not epoll (PsiLinux has none). GCC 14 makes some old
	# warnings errors: CARD32 is unsigned long, libXfont2 says uint32_t (the
	# same on ARM), so they are warnings again here.
	( cd "$d" && CFLAGS="$CFLAGS -Wno-error=incompatible-pointer-types -Wno-error=int-conversion" \
		ac_cv_func_epoll_create1=no ./configure $CFG \
		--disable-selective-werror --disable-docs --disable-devel-docs --disable-unit-tests --without-doxygen --without-xmlto --without-fop \
		--disable-xorg --disable-dmx --disable-xvfb --disable-xnest --disable-xquartz --disable-xwayland \
		--disable-xwin --disable-glamor --enable-kdrive --enable-xfbdev --disable-xephyr --disable-xfake \
		--disable-kdrive-kbd --disable-kdrive-mouse --disable-kdrive-evdev --disable-tslib \
		--disable-glx --disable-dri --disable-dri2 --disable-dri3 --disable-present --disable-xv --disable-xvmc \
		--disable-mitshm --disable-xres --disable-record --disable-screensaver --disable-xdmcp --disable-xdm-auth-1 \
		--disable-xinerama --disable-xf86vidmode --disable-xace --disable-xselinux --disable-xcsecurity \
		--disable-dbe --disable-xf86bigfont --disable-dpms --disable-composite --disable-dga \
		--disable-config-udev --disable-config-udev-kms --disable-config-hal --disable-config-wscons \
		--disable-systemd-logind --disable-suid-wrapper --disable-libdrm --disable-pciaccess \
		--disable-linux-acpi --disable-linux-apm --disable-libunwind --disable-xshmfence --disable-clientids \
		--disable-xfree86-utils --disable-vgahw --disable-vbe --disable-int10-module \
		--disable-input-thread --disable-ipv6 --disable-secure-rpc --disable-listen-tcp --without-dtrace \
		--without-systemd-daemon --with-sha1=libnettle --with-default-font-path=built-ins \
		--with-xkb-path=/usr/share/X11/xkb --with-xkb-output=/tmp --with-xkb-bin-directory=/usr/bin \
		--with-default-xkb-rules=evdev --with-default-xkb-model=pc105 --with-default-xkb-layout=gb \
		--with-os-name=PsiLinux --with-vendor-name="PsiLinux" --with-builderstring="PsiLinux" \
		>> "$B/xserver.log" 2>&1 \
	  && make -j$JOBS LD_EXPORT_SYMBOLS_FLAG= >> "$B/xserver.log" 2>&1 ) \
		|| { grep -E "error|Error" "$B/xserver.log" | head -20; tail -15 "$B/xserver.log"; exit 1; }
	# (LD_EXPORT_SYMBOLS_FLAG: no --export-dynamic, which is for loadable
	# modules (none here) and kept every global function from --gc-sections)
	put "$d/hw/kdrive/fbdev/Xfbdev"
	ln -sf Xfbdev "$P/usr/bin/X"
	lic xorg-server "$d/COPYING"
}

# --- clients --------------------------------------------------------------

xsetroot() {
	need x11 xmu xcursor xbitmaps
	log "xsetroot 1.1.3"
	d=$(unpack "$(fetch $XORG/app/xsetroot-1.1.3.tar.xz 6081b45a9eb4426e045d259d1e144b32417fb635e5b96aa90647365ac96638d1)")
	( cd "$d" && ./configure $CFG > "$B/xsetroot.log" 2>&1 && make -j$JOBS >> "$B/xsetroot.log" 2>&1 ) \
		|| { tail -20 "$B/xsetroot.log"; exit 1; }
	put "$d/xsetroot"
	lic xsetroot "$d/COPYING"
}

xeyes() {
	need xmu xext
	log "xeyes 1.1.2 (core drawing: no Xrender, no XInput 2)"
	d=$(unpack "$(fetch $XORG/app/xeyes-1.1.2.tar.bz2 57bcec0d2d167af9e5d44d0dbd74c6d7c0f0591cd0608952b23c749fdd910553)")
	( cd "$d" && ./configure $CFG --without-xrender > "$B/xeyes.log" 2>&1 && make -j$JOBS >> "$B/xeyes.log" 2>&1 ) \
		|| { tail -20 "$B/xeyes.log"; exit 1; }
	put "$d/xeyes"
	lic xeyes "$d/COPYING"
}

xterm() {
	need xaw ncurses
	log "xterm 397 (Xaw6, no Xft, no Tek/ReGIS/sixel; needs ptys: /dev/ptmx)"
	d=$(unpack "$(fetch https://invisible-island.net/archives/xterm/xterm-397.tgz 2e9b742b9cba44ecec58074e513237f6cd6d5923f1737cb36a4e5625f4ae8662)")
	# (xterm finds its libraries itself: the static ones they need go last)
	( cd "$d" && LIBS="$($PKG_CONFIG --libs x11 sm) -ltinfo" ./configure $CFG --with-x --x-includes="$SYS/usr/include" --x-libraries="$SYS/usr/lib" \
		--with-pkg-config --disable-freetype --without-xpm --without-xinerama --disable-xcursor \
		--disable-setuid --disable-setgid --disable-desktop --without-utempter \
		--disable-tek4014 --disable-sixel-graphics --disable-print-graphics --disable-screen-dumps \
		--disable-regis-graphics --disable-luit --disable-mini-luit --disable-i18n --disable-input-method \
		--disable-session-mgt --disable-active-icon --disable-direct-color --disable-88-color \
		--disable-dabbrev --disable-rectangles --disable-doublechars --disable-maximize --disable-paste64 \
		--disable-regex --disable-ziconbeep --disable-exec-selection --disable-selection-ops \
		--disable-readline-mouse --disable-tcap-query --disable-vt52 --disable-logging --disable-toolbar \
		--with-app-defaults=/usr/share/X11/app-defaults > "$B/xterm.log" 2>&1 \
	  && make -j$JOBS xterm >> "$B/xterm.log" 2>&1 ) \
		|| { grep -E "error|Error" "$B/xterm.log" | head -20; tail -20 "$B/xterm.log"; exit 1; }
	put "$d/xterm"
	mkdir -p "$P/usr/share/X11/app-defaults"
	cp "$d/XTerm.ad" "$P/usr/share/X11/app-defaults/XTerm"
	lic xterm "$d/COPYING"
}

all="xserver keymaps xsetroot xeyes xterm"
for p in ${@:-$all}; do $p; touch "$SYS/.built-$p"; done
du -sh "$P"
