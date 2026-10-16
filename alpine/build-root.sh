#!/bin/bash
# build-root.sh - Alpine mode's root image (linux/docs/ALPINE.md): Alpine
# Linux's own minirootfs for armhf, OpenRC added from Alpine's repository by
# apk (Alpine's signed packages, their signatures checked), PsiLinux's own
# files over it (overlay/), packed by mkroot.py into
# build/alpine-root/ROOT.IMG.
#   linux/alpine/build-root.sh
# It needs docker: an alpine container does the apk work, as root, so that
# owners and modes come out right. Downloads go to $LXSRC/alpine.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
WORK=${PSION_WORK:-$(cd "$REPO/.." && pwd)}
LXSRC=${LXSRC:-$WORK/psilinux-src}
ALP=3.24
ALP_VER=3.24.2
MRF=alpine-minirootfs-$ALP_VER-armhf.tar.gz
MRF_SHA=d86af88b58954d8a90c211004f1e61f208a62d1bb5a2f525f0c1cc26408ab92b
# Preinstalled (ALPINE.md, "What is preinstalled", and DECISIONS.md 50):
# each runs in the memory a 5mx has with bash running. Everything else is
# an apk add away. ALPINE_PKGS replaces the list (for measuring).
TOOLS="bash nano less htop tree ncdu jq bc mandoc openssh-client"
FUN="fortune figlet sl cmatrix micro-tetris bsd-games cbonsai nyancat fastfetch"
DOCS="nano-doc less-doc htop-doc tree-doc ncdu-doc jq-doc bc-doc mandoc-doc openssh-doc busybox-doc
	apk-tools-doc fortune-doc figlet-doc sl-doc cmatrix-doc bsd-games-doc cbonsai-doc nyancat-doc fastfetch-doc"
PKGS="openrc busybox-openrc ${ALPINE_PKGS-$TOOLS $FUN $DOCS}"
OUT=${ALPINE_OUT:-$REPO/build/alpine-root}
S=$OUT/stage
mkdir -p "$LXSRC/alpine" "$OUT"
f=$LXSRC/alpine/$MRF
[ -f "$f" ] || curl -sSfL --retry 3 -o "$f" https://dl-cdn.alpinelinux.org/alpine/v$ALP/releases/armhf/$MRF
echo "$MRF_SHA  $f" | sha256sum -c --quiet

# the minirootfs, and OpenRC (apk for armhf, in an x86 alpine container: the
# packages' scripts are not run, being ARM code; what they do is done below)
rm -rf "$S"
mkdir -p "$S"
docker run --rm -v "$OUT:/out" -v "$LXSRC/alpine:/src:ro" alpine:$ALP sh -c "
	set -e
	tar xzf /src/$MRF -C /out/stage
	apk --arch armhf --root /out/stage --keys-dir /usr/share/apk/keys/armhf \
		--repositories-file /out/stage/etc/apk/repositories --no-scripts --no-cache --quiet add $(echo $PKGS)
	chown -R $(id -u):$(id -g) /out/stage
"

# no indexes in the image: Alpine's apk would load them (30 MB; the Psion's
# apk asks the server instead: overlay/usr/local/sbin/apk)
rm -rf "$S"/var/cache/apk/*
# PsiLinux's files
cp -a "$HERE/overlay/." "$S/"
# OpenRC: the runlevels (no hardware: devices, disks, clocks and the
# network belong to EPOC). hostname and bootmisc at boot; nothing else.
mkdir -p "$S/etc/runlevels/sysinit" "$S/etc/runlevels/boot" "$S/etc/runlevels/default" "$S/etc/runlevels/shutdown" "$S/etc/runlevels/nonetwork"
ln -sfn /etc/init.d/psilinux "$S/etc/runlevels/sysinit/psilinux"
for svc in hostname bootmisc; do ln -sfn /etc/init.d/$svc "$S/etc/runlevels/boot/$svc"; done
# rc.conf: not a container OpenRC knows, but none of its hardware either
sed -i 's/^#rc_sys=""/rc_sys="lxc"/; s/^#rc_logger="NO"/rc_logger="NO"/' "$S/etc/rc.conf"
grep -q '^rc_sys="lxc"' "$S/etc/rc.conf" || echo 'rc_sys="lxc"' >> "$S/etc/rc.conf"
# bash: its post-install adds it to /etc/shells; and it is root's shell
# (the psilinux service gives root BusyBox sh if bash ever stops starting)
if [ -x "$S/bin/bash" ]; then
	grep -qx /bin/bash "$S/etc/shells" || echo /bin/bash >> "$S/etc/shells"
	sed -i 's|^\(root:.*:\)/bin/sh$|\1/bin/bash|' "$S/etc/passwd"
fi
# a copy of apk's database as built, for psilinux-merge-db (a new image over
# an old one's changes)
mkdir -p "$S/usr/lib/psilinux"
cp "$S/lib/apk/db/installed" "$S/usr/lib/psilinux/apk-installed"
cp "$S/etc/apk/world" "$S/usr/lib/psilinux/apk-world"
# the licences and where the source is (SOURCES.md): every package's name,
# version, origin and licence, and the GPL texts
L=$S/usr/share/licenses
mkdir -p "$L"
for t in gpl-2.0:edaef632cbb643e4e7a221717a6c441a4c1a7c918e6e4d56debc3d8739b233f6 \
	 gpl-3.0:3972dc9744f6499f0f9b2dbf76696f2ae7ad8af9b23dde66d6af86c9dfb36986 \
	 lgpl-2.1:20e50fe7aae3e56378ebf0417d9de904f55a0e61e4df315333e632a4d3555d95; do
	f=$LXSRC/alpine/${t%%:*}.txt
	[ -f "$f" ] || curl -sSfL --retry 3 -o "$f" https://www.gnu.org/licenses/${t%%:*}.txt
	echo "${t#*:}  $f" | sha256sum -c --quiet
	cp "$f" "$L/$(echo ${t%%:*} | tr a-z A-Z).txt"
done
{
	sed -n '1,/^PACKAGES$/p' "$HERE/SOURCES.txt" | sed '$d'
	awk '/^P:/{p=substr($0,3)} /^V:/{v=substr($0,3)} /^o:/{o=substr($0,3)} /^L:/{l=substr($0,3)}
	     /^$/{if(p)printf "  %-26s %-20s %-20s %s\n", p, v, o, l; p=""}' "$S/lib/apk/db/installed" | sort
} > "$L/SOURCES.txt"
# the busybox install script's user (klogd), as the post-install would
grep -q '^klogd:' "$S/etc/passwd" || echo 'klogd:x:100:101:klogd:/dev/null:/sbin/nologin' >> "$S/etc/passwd"
grep -q '^klogd:' "$S/etc/group" || echo 'klogd:x:101:klogd' >> "$S/etc/group"
# /c and /d are the Psion's disks (mounted by the kernel)
mkdir -p "$S/c" "$S/d"
# in parts of 1 MB: the EPOC installer cannot take one 25 MB file (DECISIONS.md 54)
python3 "$REPO/linux/tools/mkroot.py" "$S" "$OUT/ROOT.IMG" --parts 1024
