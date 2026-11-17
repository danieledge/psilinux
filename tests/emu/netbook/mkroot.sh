#!/bin/sh
# mkroot.sh - a small Linux root for the netBook test image (nbimage.mts):
# the PsiLinux root's stage (rootfs/build-rootfs.sh) without its big
# programs (git, curl, sqlite3, MicroPython, jq, Lua, nano), which would not
# fit in the ROM. -> build/nbtest/root/ROOT.IMG
#   tools/lxbuild "tests/emu/netbook/mkroot.sh"
set -e
TOP=$(cd "$(dirname "$0")/../../.." && pwd)
S=$TOP/build/nbtest/stage
rm -rf "$S" "$TOP/build/nbtest/root"; mkdir -p "$TOP/build/nbtest/root"
cp -a "$TOP/build/psilinux-root/stage" "$S"
rm -rf "$S"/usr/bin/git* "$S"/usr/libexec/git-core "$S"/usr/bin/curl "$S"/usr/bin/sqlite3 "$S"/usr/bin/micropython \
	"$S"/usr/bin/jq "$S"/usr/bin/lua* "$S"/usr/bin/nano "$S"/usr/share/git-core "$S"/usr/share/terminfo/[!lvx]* "$S"/usr/lib/psilinux/tests "$S"/usr/share/licenses 2>/dev/null || :
mkdir -p "$S/usr/share/licenses"; echo "(the netBook test root: see the PsiLinux root for the licences)" > "$S/usr/share/licenses/README"
# the tests, typed as "sh t1"... (nbrun.py types only letters, digits and . / -)
cp "$(dirname "$0")"/t/t[0-9] "$S/root/"
du -sh "$S"
python3 "$TOP/tools/mkroot.py" "$S" "$TOP/build/nbtest/root/ROOT.IMG" --parts 1024
