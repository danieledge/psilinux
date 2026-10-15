# Sources of the Linux programs (GPL compliance)

PsiLinux's two root images hold Linux programs that other people wrote,
several of them under the GNU GPL. This page lists every one, with its exact
version, its licence, and where to get the complete corresponding source.

PsiLinux's own code (the runner, the app, the translator) contains no GPL
code: see [THIRD-PARTY.md](THIRD-PARTY.md). The runner loads the programs in
these images as separate programs, as an operating system does.

**The written offer.** For at least three years from the last time a release
of PsiLinux was published, anyone may get the complete corresponding source
of any GPL or LGPL program in these images, at no charge beyond the cost of
sending it, by opening an issue on
[github.com/danieledge/psilinux](https://github.com/danieledge/psilinux/issues).
In practice it is already there: each release carries a source archive
beside the images (see "Source archives" below).

## PsiLinux-root.sis: PsiLinux's own programs

`\Linux\ROOT.IMG`. Every program was built from the unmodified upstream
release below (musl has one patch), with the PsiLinux toolchain, by the
scripts in this repository. Those scripts, the patch and the configuration,
together with the upstream archives, are the complete corresponding source.

| Program | Version | Licence | Source (pinned by sha256 in the script named) |
|---|---|---|---|
| BusyBox | 1.36.1 | **GPL v2** | https://busybox.net/downloads/busybox-1.36.1.tar.bz2 (`rootfs/build-rootfs.sh`); configuration `rootfs/busybox.config` over defconfig |
| nano | 8.2 | **GPL v3 or later** | https://www.nano-editor.org/dist/v8/nano-8.2.tar.xz (`ports/build-ports.sh`) |
| git | 2.47.0 | **GPL v2** | https://mirrors.edge.kernel.org/pub/software/scm/git/git-2.47.0.tar.xz (`ports/build-ports.sh`) |
| musl | 1.2.6 | MIT | https://musl.libc.org/releases/musl-1.2.6.tar.gz, with `toolchain/patches/musl-1.2.6-psilinux.diff` |
| GCC runtime (libgcc, libstdc++) | 14.3.0 | GPL v3 with the GCC Runtime Library Exception | via musl-cross-make `227df8b` (`toolchain/build-toolchain.sh`) |
| curl | 8.10.1 | curl (MIT-style) | https://curl.se/download/curl-8.10.1.tar.xz |
| Mbed TLS | 3.6.2 | Apache 2.0 | https://github.com/Mbed-TLS/mbedtls/releases/download/mbedtls-3.6.2/mbedtls-3.6.2.tar.bz2 |
| Lua | 5.4.7 | MIT | https://www.lua.org/ftp/lua-5.4.7.tar.gz |
| MicroPython | 1.23.0 | MIT | https://github.com/micropython/micropython/releases/download/v1.23.0/micropython-1.23.0.tar.xz |
| SQLite | 3.46.1 | public domain | https://www.sqlite.org/2024/sqlite-amalgamation-3460100.zip |
| jq | 1.7.1 | MIT | https://github.com/jqlang/jq/releases/download/jq-1.7.1/jq-1.7.1.tar.gz |
| ncurses | 6.4 | MIT-X11 | https://ftp.gnu.org/gnu/ncurses/ncurses-6.4.tar.gz |
| zlib | 1.3.1 | zlib | https://zlib.net/fossils/zlib-1.3.1.tar.gz |
| CA certificates | 2024-09-24 | MPL 2.0 | https://curl.se/ca/cacert-2024-09-24.pem |
| PsiLinux's tests | | MIT | `tests/` (this repository) |

The programs' licence texts are in the image, in `/usr/share/licenses/`.
The toolchain's Linux kernel headers (installed by musl-cross-make) are used
only to compile, and are not in the image.

To rebuild the image from this source: [docs/BUILDING.md](docs/BUILDING.md).

## PsiLinux-alpine-root.sis: Alpine Linux 3.24.2

`\Linux\Alpine\ROOT.IMG`. This is Alpine Linux's armhf minirootfs
(`alpine-minirootfs-3.24.2-armhf.tar.gz`, pinned by sha256 in
`alpine/build-root.sh`) with OpenRC added, all from Alpine's own signed
binary packages, **unmodified**. PsiLinux adds only its own small files
(`alpine/overlay/`, MIT) and the runlevel links that `build-root.sh` makes.

The image holds these 72 packages (from its `/lib/apk/db/installed`; the
same list, with the written offer, is in the image as
`/usr/share/licenses/SOURCES.txt`, beside the GPL-2.0, GPL-3.0 and LGPL-2.1
texts):

| Package | Version | Alpine origin (aport) | Licence |
|---|---|---|---|
| alpine-baselayout | 3.7.2-r1 | alpine-baselayout | **GPL-2.0-only** |
| alpine-baselayout-data | 3.7.2-r1 | alpine-baselayout | **GPL-2.0-only** |
| alpine-keys | 2.6-r0 | alpine-keys | MIT |
| alpine-release | 3.24.2-r0 | alpine-base | MIT |
| apk-tools | 3.0.8-r0 | apk-tools | **GPL-2.0-only** |
| apk-tools-doc | 3.0.8-r0 | apk-tools | **GPL-2.0-only** |
| bash | 5.3.9-r1 | bash | **GPL-3.0-or-later** |
| bc | 1.08.2-r1 | bc | **GPL-3.0-or-later** |
| bc-doc | 1.08.2-r1 | bc | **GPL-3.0-or-later** |
| bridge | 1.5-r5 | bridge | **GPL-2.0-or-later** |
| bsd-games | 3.3-r2 | bsd-games | BSD-3-Clause |
| bsd-games-doc | 3.3-r2 | bsd-games | BSD-3-Clause |
| busybox | 1.37.0-r31 | busybox | **GPL-2.0-only** |
| busybox-binsh | 1.37.0-r31 | busybox | **GPL-2.0-only** |
| busybox-doc | 1.37.0-r31 | busybox | **GPL-2.0-only** |
| busybox-openrc | 1.37.0-r31 | busybox | **GPL-2.0-only** |
| ca-certificates-bundle | 20260909-r0 | ca-certificates | MPL-2.0 AND MIT |
| cbonsai | 1.4.2-r0 | cbonsai | **GPL-3.0-only** |
| cbonsai-doc | 1.4.2-r0 | cbonsai | **GPL-3.0-only** |
| cmatrix | 2.0-r2 | cmatrix | **GPL-3.0-or-later** |
| cmatrix-doc | 2.0-r2 | cmatrix | **GPL-3.0-or-later** |
| fastfetch | 2.64.2-r0 | fastfetch | MIT |
| fastfetch-doc | 2.64.2-r0 | fastfetch | MIT |
| figlet | 2.2.5-r3 | figlet | BSD-3-Clause |
| figlet-doc | 2.2.5-r3 | figlet | BSD-3-Clause |
| fortune | 0.1-r5 | fortune | BSD-3-Clause |
| fortune-doc | 0.1-r5 | fortune | BSD-3-Clause |
| htop | 3.5.3-r0 | htop | **GPL-2.0-or-later** |
| htop-doc | 3.5.3-r0 | htop | **GPL-2.0-or-later** |
| hwdata-pci | 0.408-r0 | hwdata | **GPL-2.0-or-later** OR XFree86-1.1 |
| ifupdown-ng | 0.13.0-r0 | ifupdown-ng | ISC |
| jq | 1.8.2-r0 | jq | MIT |
| jq-doc | 1.8.2-r0 | jq | MIT |
| less | 702-r0 | less | **GPL-3.0-or-later** OR BSD-2-Clause |
| less-doc | 702-r0 | less | **GPL-3.0-or-later** OR BSD-2-Clause |
| libapk | 3.0.8-r0 | apk-tools | **GPL-2.0-only** |
| libbsd | 0.12.2-r0 | libbsd | BSD-3-Clause |
| libcap2 | 2.78-r0 | libcap | BSD-3-Clause OR **GPL-2.0-only** |
| libcrypto3 | 3.5.8-r0 | openssl | Apache-2.0 |
| libedit | 20260508.3.1-r1 | libedit | BSD-3-Clause |
| libmd | 1.2.0-r0 | libmd | BSD-3-Clause AND BSD-2-Clause AND ISC AND Beerware AND Public Domain |
| libncursesw | 6.6_p20260516-r0 | ncurses | X11 |
| libpanelw | 6.6_p20260516-r0 | ncurses | X11 |
| libssl3 | 3.5.8-r0 | openssl | Apache-2.0 |
| lsof | 4.99.6-r0 | lsof | custom |
| mandoc | 1.14.6-r15 | mandoc | ISC |
| mandoc-doc | 1.14.6-r15 | mandoc | ISC |
| micro-tetris | 1.5.0-r0 | micro-tetris | ISC |
| musl | 1.2.6-r2 | musl | MIT |
| musl-utils | 1.2.6-r2 | musl | MIT AND BSD-2-Clause AND **GPL-2.0-or-later** |
| nano | 9.2-r0 | nano | **GPL-3.0-or-later** |
| nano-doc | 9.2-r0 | nano | **GPL-3.0-or-later** |
| ncdu | 1.22-r2 | ncdu | MIT |
| ncdu-doc | 1.22-r2 | ncdu | MIT |
| ncurses-terminfo-base | 6.6_p20260516-r0 | ncurses | X11 |
| nyancat | 1.5.2-r1 | nyancat | NCSA |
| nyancat-doc | 1.5.2-r1 | nyancat | NCSA |
| oniguruma | 6.9.10-r0 | oniguruma | BSD-2-Clause |
| openrc | 0.63.2-r0 | openrc | BSD-2-Clause |
| openrc-user | 0.63.2-r0 | openrc | BSD-2-Clause |
| openssh-client-common | 10.3_p1-r1 | openssh | SSH-OpenSSH |
| openssh-client-default | 10.3_p1-r1 | openssh | SSH-OpenSSH |
| openssh-doc | 10.3_p1-r1 | openssh | SSH-OpenSSH |
| openssh-keygen | 10.3_p1-r1 | openssh | SSH-OpenSSH |
| readline | 8.3.3-r1 | readline | **GPL-3.0-or-later** |
| scanelf | 1.3.9-r1 | pax-utils | **GPL-2.0-only** |
| sl | 5.02-r1 | sl | MIT |
| sl-doc | 5.02-r1 | sl | MIT |
| ssl_client | 1.37.0-r31 | busybox | **GPL-2.0-only** |
| tree | 2.3.2-r0 | tree | **GPL-2.0-or-later** |
| tree-doc | 2.3.2-r0 | tree | **GPL-2.0-or-later** |
| zlib | 1.3.2-r0 | zlib | Zlib |

The copyleft aports, whose source this page offers: `main/alpine-baselayout`,
`main/apk-tools`, `main/bash`, `main/bc`, `main/bridge`, `main/busybox`,
`community/cbonsai`, `community/cmatrix`, `main/htop`, `main/less` and
`main/hwdata` and `main/libcap` (dual licensed), `main/musl` (for
musl-utils' GPL parts), `main/nano`, `main/pax-utils` (scanelf),
`main/readline` and `main/tree`. bash, bc, cmatrix, nano, readline and
cbonsai are GPL v3.

Because the packages are Alpine's own builds, the corresponding source of
each is Alpine's recipe for it (the `APKBUILD` and its patches, in
[aports](https://gitlab.alpinelinux.org/alpine/aports) on the `3.24-stable`
branch, at the commit that built the version above) plus the upstream archive
that recipe names. To fetch them all:

```sh
mkdir -p alpine-src && cd alpine-src
docker run --rm -v "$PWD:/out" alpine:3.24 sh -c '
  apk add --no-cache git abuild >/dev/null
  git clone --depth 1 -b 3.24-stable https://gitlab.alpinelinux.org/alpine/aports.git /out/aports
  adduser -D -G abuild builder
  export SRCDEST=/out/distfiles; mkdir -p $SRCDEST; chown -R builder $SRCDEST /out/aports
  for p in main/alpine-baselayout main/apk-tools main/bash main/bc main/bridge main/busybox \
           community/cbonsai community/cmatrix main/htop main/less main/hwdata main/libcap \
           main/musl main/nano main/pax-utils main/readline main/tree; do
    ( cd /out/aports/$p && su builder -c "abuild -F fetch verify" ) || echo "check $p"
  done'
```

The packages installed later with `apk add` come straight from Alpine's
mirror to the Psion. They are not distributed by PsiLinux; their source is in
aports in the same way.

**Keep this list in step with the image.** When `alpine/build-root.sh`
changes what it installs (for example, a package added to the image), regenerate the table:

```sh
awk '/^P:/{p=substr($0,3)} /^V:/{v=substr($0,3)} /^o:/{o=substr($0,3)} /^L:/{l=substr($0,3)}
     /^$/{if(p)print p" | "v" | "o" | "l; p=""}' ../build/alpine-root/stage/lib/apk/db/installed | sort
```

Some of these packages are GPL v3 (bash, nano, readline, bc, cmatrix,
cbonsai). A GPL v3 package in the image needs nothing more than a GPL v2 one
here: it is listed, its licence text is in the image, and its source is
offered in the same way.

## Source archives (for each release)

So that the source does not depend on other people's servers, each GitHub
release carries `PsiLinux-<version>-sources.tar.xz` beside the `.sis` files:

- this repository at the release tag (`git archive`);
- the upstream archives for PsiLinux-root (from `$LXSRC`, the files that
  `rootfs/build-rootfs.sh` and `ports/build-ports.sh` downloaded and checked);
- for the Alpine root, the minirootfs's checksum and the aports recipes and
  distfiles of the copyleft packages above (the `alpine-src/` folder the
  command above makes).

To make it, after building the release:

```sh
V=0.21
git -C linux archive --prefix=psilinux-$V/ v$V | xz > /tmp/psilinux-$V.tar.xz
tar cJf dist/PsiLinux-$V-sources.tar.xz \
  -C /tmp psilinux-$V.tar.xz \
  -C ../psilinux-src busybox-1.36.1.tar.bz2 nano-8.2.tar.xz git-2.47.0.tar.xz \
     musl-1.2.6.tar.gz curl-8.10.1.tar.xz mbedtls-3.6.2.tar.bz2 lua-5.4.7.tar.gz \
     micropython-1.23.0.tar.xz sqlite-amalgamation-3460100.zip jq-1.7.1.tar.gz \
     ncurses-6.4.tar.gz zlib-1.3.1.tar.gz \
  -C "$OLDPWD/alpine-src" distfiles $(cd "$OLDPWD/alpine-src" && echo aports/main/alpine-baselayout \
     aports/main/apk-tools aports/main/bash aports/main/bc aports/main/bridge aports/main/busybox \
     aports/community/cbonsai aports/community/cmatrix aports/main/htop aports/main/less \
     aports/main/hwdata aports/main/libcap aports/main/musl aports/main/nano aports/main/pax-utils \
     aports/main/readline aports/main/tree)
```

(Adjust the file names if the pinned versions change; the scripts named in
the tables above are the authority. GCC's runtime is covered by its
exception and needs no source offer; `$LXSRC/mcm/sources/` holds GCC and
binutils if you want to include them anyway.)
