#!/bin/bash
# Builds Links 2.30 in graphics mode (links -g, the X11 driver) for PsiLinux's
# X desktop, with the PsiLinux toolchain against the static X libraries that
# build-x11.sh made (build/psilinux-root/x11-sysroot, read only), into
# build/psilinux-root/x11/usr/bin/links (laid out as the root, next to X).
#   tools/lxbuild ports/build-links.sh            the browser
#   tools/lxbuild "ports/build-links.sh NAME..."  only those steps (png, jpeg,
#                                                 mbedtls, mbossl, fonts, links)
# Every source is pinned by version and sha256 and cached in $LXSRC.
#
# What is in it, and why:
#   - Links 2.30 (GPL v2), graphics, the X driver only (no fb, svgalib,
#     DirectFB, FreeType). On the Psion's 8-bit PseudoColor X it draws in 31
#     greys of the shared colour map, dithering pictures once (the patch's
#     grey mode), instead of its 6x6x6 colour cube that the Psion would
#     dither again. Text: hinted DejaVu Sans strikes at the sizes drawn, and
#     DejaVu masters for the rest (fonts(), below).
#   - Pictures: GIF (Links' own), PNG (libpng 1.6.43) and JPEG (IJG jpeg-9f,
#     as PsiWeb: no SIMD to gain on an ARM710T, so not libjpeg-turbo); zlib
#     1.3.1 is build-x11.sh's.
#   - TLS: Mbed TLS 3.6.2 (TLS 1.2 and 1.3, client only, cut down: see
#     mbedtls()), under ports/links/mbossl: the part of OpenSSL's API that
#     Links uses, written over Mbed TLS. OpenSSL itself would be 2-3 MB of
#     code for the same job. Certificates are checked against Mozilla's roots
#     ($SSL_CERT_FILE, /etc/ssl/certs/ca-certificates.crt, or the
#     ca-certificates.crt that comes with links), read on demand.
#   - The patch, ports/patches/links-2.30-psilinux.diff (its head lists it
#     all): defaults for 640x240 and ~9 MB (512 KB picture cache, 1 MB memory
#     cache, no async DNS), the grey mode, the Mbed TLS certificate check, an
#     option to let an HTTP proxy such as PsiProxy fetch https:// pages
#     itself, dialogs that fit 240 pixels, graphics whenever $DISPLAY is set;
#     and from PsiWeb: dip.c's speed work and strikes, the parser's fast paths,
#     JPEGs decoded near the size shown and in grey, links underlined.
#
# Run it, with X up:  DISPLAY=:0 links -g -mode 640x240 http://...
# (-g and -mode are the defaults when $DISPLAY is set; psiwm's Web icon runs
# plain "links".) The package: the "sis" step, below.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/.." && pwd)
WORK=${PSION_WORK:-$(cd "$REPO/.." && pwd)}
LXTC=${LXTC:-$WORK/psilinux-toolchain}
LXSRC=${LXSRC:-$WORK/psilinux-src}
export PATH=$LXTC/bin:$PATH
T=arm-linux-musleabi
OUT=$REPO/build/psilinux-root
P=${X11OUT:-$OUT/x11}             # what goes into the root (next to X)
PATCHES=$HERE/patches
LX=$HERE/links                    # our own files (mbossl, mkfont.py, test/)
XSYS=$OUT/x11-sysroot             # build-x11.sh's libraries (read only)
[ -f "$XSYS/usr/lib/libX11.a" ] || { echo "no X libraries: run ports/build-x11.sh first"; exit 1; }
# this build's own libraries (not in the root), through a neutral path
SYS=/tmp/psilinux-links-sysroot
mkdir -p "$OUT/links-sysroot"
ln -sfn "$OUT/links-sysroot" "$SYS"
mkdir -p "$LXSRC/links-build"
ln -sfn "$LXSRC/links-build" /tmp/psilinux-links-build
B=/tmp/psilinux-links-build
JOBS=$(nproc)
mkdir -p "$P/usr/bin" "$SYS/usr/include" "$SYS/usr/lib" "$B"
export CC=lx-cc AR=$T-ar RANLIB=$T-ranlib STRIP=$T-strip
export CFLAGS="-Os -ffunction-sections -fdata-sections -ffile-prefix-map=$B=/src"
LDGC="-Wl,--gc-sections"

fetch() {    # url sha256 [name] -> the file's path
	local f=$LXSRC/${3:-$(basename "$1")}
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
# (Links' licences are kept apart from X's, which build.sh gathers for
# PsiLinux-X.sis: they go into PsiLinux-Links.sis instead)
lic() {      # name file...: the licence, into /usr/share/links/licenses/<name>/
	mkdir -p "$P/usr/share/links/licenses/$1"
	local n=$1; shift
	cp "$@" "$P/usr/share/links/licenses/$n/"
}
need() { local x; for x in "$@"; do [ -f "$SYS/.built-$x" ] || { $x; touch "$SYS/.built-$x"; }; done; }

# --- the picture libraries --------------------------------------------------

png() {
	log "libpng 1.6.43 (a library)"
	# (the GitHub tag's archive, as PsiWeb's links/fetch.sh)
	d=$(unpack "$(fetch https://github.com/pnggroup/libpng/archive/refs/tags/v1.6.43.tar.gz \
		fecc95b46cf05e8e3fc8a414750e0ba5aad00d89e9fdf175e94ff041caf1a03a libpng-1.6.43.tar.gz)")
	# (reading only: no png*w*.c. pngpread.c stays: Links feeds data as it comes)
	( cd "$d" && cp scripts/pnglibconf.h.prebuilt pnglibconf.h \
	  && for f in png pngerror pngget pngmem pngpread pngread pngrio pngrtran pngrutil pngset pngtrans; do
		lx-cc $CFLAGS -DPNG_ARM_NEON_OPT=0 -I. -I"$XSYS/usr/include" -c $f.c -o $f.o || exit 1; done \
	  && rm -f libpng.a && $AR rcs libpng.a png*.o \
	  && cp libpng.a "$SYS/usr/lib/" && cp png.h pngconf.h pnglibconf.h "$SYS/usr/include/" ) > "$B/png.log" 2>&1 \
		|| { tail -20 "$B/png.log"; exit 1; }
}

jpeg() {
	log "IJG libjpeg 9f (a library: decoding only)"
	unpack "$(fetch https://www.ijg.org/files/jpegsrc.v9f.tar.gz 04705c110cb2469caa79fb71fba3d7bf834914706e9641a4589485c1f832565b)" > /dev/null
	d=$B/jpeg-9f		# (the archive's folder)
	( cd "$d" && cp jconfig.txt jconfig.h \
	  && for f in jaricom jcomapi jdapimin jdapistd jdarith jdatasrc jdcoefct jdcolor jddctmgr jdhuff jdinput \
		jdmainct jdmarker jdmaster jdmerge jdpostct jdsample jerror jidctflt jidctfst jidctint jquant1 jquant2 \
		jutils jmemmgr jmemnobs; do lx-cc $CFLAGS -I. -c $f.c -o $f.o || exit 1; done \
	  && rm -f libjpeg.a && $AR rcs libjpeg.a j*.o \
	  && cp libjpeg.a "$SYS/usr/lib/" && cp jpeglib.h jconfig.h jmorecfg.h jerror.h "$SYS/usr/include/" ) > "$B/jpeg.log" 2>&1 \
		|| { tail -20 "$B/jpeg.log"; exit 1; }
}

# --- TLS ---------------------------------------------------------------------

# Mbed TLS 3.6.2 (the same release as build-ports.sh's), its configuration
# cut to a TLS client: TLS 1.2 and 1.3; ECDHE with X25519, P-256 and P-384;
# RSA and ECDSA certificates; AES-GCM and ChaCha20-Poly1305; session tickets;
# no server, DTLS, renegotiation, PSK-only, DHE, CBC, CCM, Camellia, ARIA, DES,
# RC4, certificate writing, PKCS#7/#12, self tests or error strings. A 16 KB
# record must still be taken in, but what goes out is cut into 4 KB records.
mbedtls() {
	log "Mbed TLS 3.6.2 (a library: a TLS client, cut down)"
	d=$(unpack "$(fetch https://github.com/Mbed-TLS/mbedtls/releases/download/mbedtls-3.6.2/mbedtls-3.6.2.tar.bz2 8b54fb9bcf4d5a7078028e0520acddefb7900b3e66fec7f7175bb5b7d85ccdca)")
	local c="python3 $d/scripts/config.py -f $d/include/mbedtls/mbedtls_config.h" o
	for o in MBEDTLS_SSL_SRV_C MBEDTLS_SSL_PROTO_DTLS MBEDTLS_SSL_DTLS_ANTI_REPLAY MBEDTLS_SSL_DTLS_HELLO_VERIFY \
		MBEDTLS_SSL_DTLS_CLIENT_PORT_REUSE MBEDTLS_SSL_DTLS_CONNECTION_ID MBEDTLS_SSL_DTLS_SRTP MBEDTLS_SSL_COOKIE_C \
		MBEDTLS_SSL_TICKET_C MBEDTLS_SSL_CACHE_C MBEDTLS_SSL_RENEGOTIATION MBEDTLS_SSL_CONTEXT_SERIALIZATION \
		MBEDTLS_SSL_EARLY_DATA MBEDTLS_SSL_ALPN MBEDTLS_SSL_DEBUG_ALL \
		MBEDTLS_KEY_EXCHANGE_PSK_ENABLED MBEDTLS_KEY_EXCHANGE_DHE_PSK_ENABLED MBEDTLS_KEY_EXCHANGE_ECDHE_PSK_ENABLED \
		MBEDTLS_KEY_EXCHANGE_RSA_PSK_ENABLED MBEDTLS_KEY_EXCHANGE_RSA_ENABLED MBEDTLS_KEY_EXCHANGE_DHE_RSA_ENABLED \
		MBEDTLS_KEY_EXCHANGE_ECDH_ECDSA_ENABLED MBEDTLS_KEY_EXCHANGE_ECDH_RSA_ENABLED MBEDTLS_KEY_EXCHANGE_ECJPAKE_ENABLED \
		MBEDTLS_SSL_TLS1_3_KEY_EXCHANGE_MODE_PSK_ENABLED MBEDTLS_DHM_C MBEDTLS_ECJPAKE_C MBEDTLS_CAMELLIA_C MBEDTLS_ARIA_C \
		MBEDTLS_DES_C MBEDTLS_CCM_C MBEDTLS_CMAC_C MBEDTLS_NIST_KW_C MBEDTLS_CIPHER_MODE_CBC MBEDTLS_CIPHER_MODE_CFB \
		MBEDTLS_CIPHER_MODE_CTR MBEDTLS_CIPHER_MODE_OFB MBEDTLS_CIPHER_MODE_XTS MBEDTLS_CIPHER_PADDING_PKCS7 \
		MBEDTLS_CIPHER_PADDING_ONE_AND_ZEROS MBEDTLS_CIPHER_PADDING_ZEROS_AND_LEN MBEDTLS_CIPHER_PADDING_ZEROS \
		MBEDTLS_SSL_ENCRYPT_THEN_MAC MBEDTLS_RIPEMD160_C MBEDTLS_SHA3_C MBEDTLS_PKCS5_C MBEDTLS_PKCS7_C MBEDTLS_PKCS12_C \
		MBEDTLS_PKCS1_V21 MBEDTLS_GENPRIME MBEDTLS_PK_WRITE_C MBEDTLS_PEM_WRITE_C MBEDTLS_X509_CREATE_C \
		MBEDTLS_X509_CRT_WRITE_C MBEDTLS_X509_CSR_WRITE_C MBEDTLS_X509_CSR_PARSE_C MBEDTLS_X509_CRL_PARSE_C \
		MBEDTLS_X509_RSASSA_PSS_SUPPORT MBEDTLS_PK_PARSE_EC_EXTENDED MBEDTLS_PK_PARSE_EC_COMPRESSED \
		MBEDTLS_ECP_DP_SECP192R1_ENABLED MBEDTLS_ECP_DP_SECP224R1_ENABLED MBEDTLS_ECP_DP_SECP521R1_ENABLED \
		MBEDTLS_ECP_DP_SECP192K1_ENABLED MBEDTLS_ECP_DP_SECP224K1_ENABLED MBEDTLS_ECP_DP_SECP256K1_ENABLED \
		MBEDTLS_ECP_DP_BP256R1_ENABLED MBEDTLS_ECP_DP_BP384R1_ENABLED MBEDTLS_ECP_DP_BP512R1_ENABLED \
		MBEDTLS_ECP_DP_CURVE448_ENABLED MBEDTLS_ECDSA_DETERMINISTIC MBEDTLS_HMAC_DRBG_C MBEDTLS_LMS_C \
		MBEDTLS_NET_C MBEDTLS_TIMING_C MBEDTLS_DEBUG_C MBEDTLS_SELF_TEST MBEDTLS_VERSION_FEATURES MBEDTLS_ERROR_C \
		MBEDTLS_PSA_CRYPTO_STORAGE_C MBEDTLS_PSA_ITS_FILE_C MBEDTLS_PSA_CRYPTO_SE_C \
		MBEDTLS_PADLOCK_C MBEDTLS_AESNI_C MBEDTLS_AESCE_C; do
		$c unset $o
	done
	$c set MBEDTLS_X509_TRUSTED_CERTIFICATE_CALLBACK
	$c set MBEDTLS_ERROR_STRERROR_DUMMY
	$c set MBEDTLS_SSL_OUT_CONTENT_LEN 4096
	$c set MBEDTLS_MPI_WINDOW_SIZE 2
	( cd "$d" && make -C library -j$JOBS CC=lx-cc AR=$AR CFLAGS="$CFLAGS -I../include" libmbedcrypto.a libmbedx509.a libmbedtls.a \
	  && cp library/libmbedcrypto.a library/libmbedx509.a library/libmbedtls.a "$SYS/usr/lib/" \
	  && rm -rf "$SYS/usr/include/mbedtls" "$SYS/usr/include/psa" && cp -r include/mbedtls include/psa "$SYS/usr/include/" ) \
		> "$B/mbedtls.log" 2>&1 || { grep -E "error" "$B/mbedtls.log" | head -20; tail -15 "$B/mbedtls.log"; exit 1; }
	$T-size "$SYS"/usr/lib/libmbed*.a | awk 'NR > 1 { s += $1 + $2 } END { printf "   %d bytes of code and data (all of Mbed TLS as configured)\n", s }'
}

# mbossl: OpenSSL's API as Links uses it, over Mbed TLS (ports/links/mbossl)
mbossl() {
	need mbedtls
	log "mbossl (Links' OpenSSL calls over Mbed TLS)"
	mkdir -p "$B/mbossl" "$SYS/usr/include/openssl" "$SYS/usr/lib/pkgconfig"
	lx-cc $CFLAGS -Wall -I"$SYS/usr/include" -I"$LX/mbossl/include" -c "$LX/mbossl/mbossl.c" -o "$B/mbossl/mbossl.o"
	rm -f "$SYS/usr/lib/libmbossl.a"; $AR rcs "$SYS/usr/lib/libmbossl.a" "$B/mbossl/mbossl.o"
	cp "$LX"/mbossl/include/openssl/*.h "$SYS/usr/include/openssl/"
	# (Links' configure finds "openssl" through pkg-config)
	cat > "$SYS/usr/lib/pkgconfig/openssl.pc" <<PC
prefix=/usr
Name: openssl
Description: mbossl: OpenSSL's API over Mbed TLS, for Links on PsiLinux
Version: 3.0.0
Cflags: -I$SYS/usr/include
Libs: -L$SYS/usr/lib -lmbossl -lmbedtls -lmbedx509 -lmbedcrypto
PC
}

# --- fonts ------------------------------------------------------------------

# Links draws text with its own fonts (font_inc.c: every glyph a grey PNG,
# 2.7 MB of data, all of it in the program, so in memory), scaling a master
# of each glyph to the size wanted. As in PsiWeb (its tools, in ports/links):
#   - mkfont.py keeps Latin, punctuation and a few symbols as 32 px masters
#     (FONT_H=...), drawn from DejaVu (--ttf, PsiLinux's addition) so every
#     size is one family, run-length coded (30 times cheaper to unpack than
#     PNG on the ARM; dip.c's PSIWEB code reads them);
#   - mkstrike.py renders DejaVu Sans, Sans Bold and Sans Mono with
#     FreeType's hinting at exactly the cell heights Links draws for the 15 px
#     body size (13 to 26 px; the menus' 14 px too): stems on whole pixels,
#     no scaling. Other sizes (another font size chosen in Links) use the
#     masters. A sans with a large x-height reads far better on the 640x240
#     LCD than Links' serif, whose thin strokes become grey blobs.
fonts() {
	[ -f "$B/links-2.30/.pristine-fonts" ] || links_src
	log "fonts: DejaVu masters (mkfont.py), hinted DejaVu strikes (mkstrike.py)"
	unpack "$(fetch https://github.com/dejavu-fonts/dejavu-fonts/releases/download/version_2_37/dejavu-fonts-ttf-2.37.tar.bz2 \
		fa9ca4d13871dd122f61258a80d01751d603b4d3ee14095d65453b4e846e17d7)" > /dev/null
	python3 "$LX/mkfont.py" "$B/links-2.30/font_inc.c" "$B/font_psi.c" --height ${FONT_H:-32} --rle \
		--ttf "$B/dejavu-fonts-ttf-2.37/ttf" > "$B/fonts.log" 2>&1 || { tail -20 "$B/fonts.log"; exit 1; }
	head -1 "$B/fonts.log" | sed 's/^/   /'
	python3 "$LX/mkstrike.py" "$B/strike_psi.c" --base 15 --full 0:15,0:14 --fontdir "$B/dejavu-fonts-ttf-2.37/ttf" \
		--preview "$B/strikes.png" >> "$B/fonts.log" 2>&1 || { tail -20 "$B/fonts.log"; exit 1; }
	tail -1 "$B/fonts.log" | sed 's/^/   /'
}

# --- Links --------------------------------------------------------------------

links_src() {
	log "Links 2.30 (sources and the patch)"
	d=$(unpack "$(fetch http://links.twibright.com/download/links-2.30.tar.bz2 c4631c6b5a11527cdc3cb7872fc23b7f2b25c2b021d596be410dadb40315f166)")
	patch -d "$d" -p1 < "$PATCHES/links-2.30-psilinux.diff" > "$B/links-patch.log" || { cat "$B/links-patch.log"; exit 1; }
	touch "$d/.pristine-fonts"
}

links() {
	need png jpeg mbedtls mbossl
	links_src                       # (afresh: the patch may have changed)
	[ -f "$B/font_psi.c" ] || fonts
	log "Links 2.30: links -g (X11, PNG, JPEG, GIF, TLS through mbossl)"
	d=$B/links-2.30
	# Cut down as PsiWeb does (its mkfont.py, mklang.py and mksuffix.py):
	# the fonts; Links' own texts in English only (33 languages are 370 KB);
	# the public-suffix list (for cookies) to names of one or two labels.
	# (the strikes go at the end of font_inc.c, which has included links.h)
	{ cat "$B/font_psi.c"; grep -v '^#include' "$B/strike_psi.c"; } > "$d/font_inc.c"
	rm -f "$d/.pristine-fonts"
	python3 "$LX/mklang.py" "$d/language.inc" "$B/language.inc" > /dev/null && cp "$B/language.inc" "$d/language.inc"
	python3 "$LX/mksuffix.py" "$d/suffix.inc" "$B/suffix.inc" | sed 's/^.*: /   /' && cp "$B/suffix.inc" "$d/suffix.inc"
	# PSILINUX: the patch's own changes. PSIWEB and PSI_STRIKES: PsiWeb's
	# dip.c (run-length glyphs, the hinted strikes, divisions and soft float
	# taken out of the glyph and picture scalers, width caches). No fork on
	# exit: PsiLinux's fork is a vfork with a snapshot, the parent waits.
	( cd "$d" && CPPFLAGS="-I$SYS/usr/include -I$XSYS/usr/include -DPSILINUX -DPSIWEB -DPSI_STRIKES -DNO_FORK_ON_EXIT" \
		LDFLAGS="-L$SYS/usr/lib -L$XSYS/usr/lib $LDGC" \
		LIBS="-lxcb -lXau" \
		PKG_CONFIG="pkg-config --static" PKG_CONFIG_SYSROOT_DIR= \
		PKG_CONFIG_LIBDIR="$SYS/usr/lib/pkgconfig:$XSYS/usr/lib/pkgconfig" \
		ac_cv_func_mmap_fixed_mapped=yes \
		./configure --host=$T --build=$(gcc -dumpmachine) --prefix=/usr --enable-graphics \
		--with-x --x-includes="$XSYS/usr/include" --x-libraries="$XSYS/usr/lib" \
		--without-fb --without-directfb --without-svgalib --without-pmshell --without-windows --without-atheos \
		--without-haiku --without-grx --without-gpm --without-freetype --without-librsvg --without-libtiff \
		--without-libwebp --without-libavif --without-libevent --without-ipv6 --without-openmp \
		--without-brotli --without-zstd --without-bzip2 --without-lzma --without-lzip \
		--with-ssl --with-zlib --with-libjpeg > "$B/links.log" 2>&1 \
	  && grep -E "^(Graphics|SSL|.*driver|Compression|Image)" "$B/links.log" | sed 's/^/   /' \
	  && make -j$JOBS >> "$B/links.log" 2>&1 ) \
		|| { grep -E "error|Error" "$B/links.log" | head -30; tail -25 "$B/links.log"; exit 1; }
	grep -q "^#define HAVE_SSL 1" "$d/config.h" || { echo "no TLS in this build"; exit 1; }
	grep -q "^#define GRDRV_X 1" "$d/config.h" || { echo "no X driver in this build"; exit 1; }
	$STRIP -o "$P/usr/bin/links" "$d/links"
	ls -la "$P/usr/bin/links" | awk '{print "   " $5 " bytes " $9}'
	$T-size "$d/links" | awk 'NR == 2 { printf "   text %d, data %d, bss %d\n", $1, $2, $3 }'
	# the licences of all that is in it (PsiLinux-Links.sis carries them)
	rm -rf "$P/usr/share/links/licenses"
	lic links "$d/COPYING"
	lic libpng "$B/libpng-1.6.43/LICENSE"
	lic libjpeg "$B/jpeg-9f/README"
	lic mbedtls "$B/mbedtls-3.6.2/LICENSE"
	lic dejavu-fonts "$B/dejavu-fonts-ttf-2.37/LICENSE"
	lic zlib "$P/usr/share/licenses/zlib/LICENSE"     # (build-x11.sh's zlib)
	lic libX11 "$P"/usr/share/licenses/libX11/*
	lic libxcb "$P"/usr/share/licenses/libxcb/*
	lic libXau "$P"/usr/share/licenses/libXau/*
	# the certificates: the same bundle as build-ports.sh's curl (Mozilla's, via curl.se)
	mkdir -p "$P/usr/share/links"
	cp "$(fetch https://curl.se/ca/cacert-2024-09-24.pem 189d3cf6d103185fba06d76c1af915263c6d42225481a1759e853b33ac857540)" "$P/usr/share/links/ca-certificates.crt"
}

# --- the package --------------------------------------------------------------

# dist/PsiLinux-Links.sis (pkg/psilinux-links.pkg), from what links() made.
# makesis is an EPOC tool: run this step in psion-build, as build.sh is:
#   core/tools/docker/psibuild "bash ports/build-links.sh sis"
sis() {
	: "${PSION_SDK:?run this in psion-build: core/tools/docker/psibuild \"bash ports/build-links.sh sis\"}"
	[ -f "$P/usr/bin/links" ] || { echo "no links yet: tools/lxbuild ports/build-links.sh"; exit 1; }
	local LP=$REPO/build/psilinux-links-pkg l
	rm -rf "$LP"; mkdir -p "$LP" "$REPO/dist"
	cp "$P/usr/bin/links" "$P/usr/share/links/ca-certificates.crt" "$REPO/pkg/psilinux-links.pkg" "$LP/"
	for l in "$P"/usr/share/links/licenses/*/*; do
		printf '==== %s\n\n' "${l#$P/usr/share/links/licenses/}"; cat "$l"; printf '\n'
	done > "$LP/licences.txt"
	( cd "$LP" && WINEDEBUG=-all wine "$PSION_SDK/epoc_cpp_sdk/epoc32/tools/makesis.exe" psilinux-links.pkg "$REPO/dist/PsiLinux-Links.sis" > makesis.log 2>&1 ) \
		|| { cat "$LP/makesis.log"; exit 1; }
	ls -la "$REPO/dist/PsiLinux-Links.sis" | awk '{print "   " $5 " bytes " $9}'
}

all="links"
for p in ${@:-$all}; do
	case $p in links|sis) $p ;; *) $p; touch "$SYS/.built-$p" ;; esac
done
