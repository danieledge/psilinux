#!/bin/sh
# apktest.sh - apk with the online catalogue, in PsiLinux's Alpine mode (run
# by tests/run-apk.sh, which serves the catalogue at $APK_CATALOGUE): install
# git from Alpine's mirror, use it, remove it; with the time each step takes
pass=0; fail=0
ok() { echo "ok   $*"; pass=$((pass + 1)); }
bad() { echo "FAIL $*"; fail=$((fail + 1)); }
t0=$(date +%s)
apk update > /tmp/u 2>&1; cat /tmp/u
grep -q "^OK: [0-9]* distinct packages" /tmp/u && ok "update ($(( $(date +%s) - t0 )) s)" || bad "update"
t0=$(date +%s)
apk search -v fastfetch > /tmp/s 2>&1; head -3 /tmp/s
grep -q "^fastfetch-" /tmp/s && ok "search ($(( $(date +%s) - t0 )) s)" || bad "search"
t0=$(date +%s)
apk add -s git > /tmp/p 2>&1; cat /tmp/p
grep -q "^OK (simulated)" /tmp/p && ok "add -s git: the plan ($(( $(date +%s) - t0 )) s)" || bad "add -s git"
t0=$(date +%s)
apk add git > /tmp/a 2>&1; tail -4 /tmp/a
git --version > /tmp/g 2>&1 && ok "add git, and it runs: $(cat /tmp/g) ($(( $(date +%s) - t0 )) s)" || { bad "add git"; cat /tmp/a /tmp/g; }
grep -qx git /etc/apk/world && ok "git is in the world file" || bad "world"
apk info -e git > /dev/null 2>&1 && ok "the real apk has it (signatures checked)" || bad "apk info -e git"
t0=$(date +%s)
apk del git > /tmp/d 2>&1; tail -1 /tmp/d
[ ! -x /usr/bin/git ] && ok "del git ($(( $(date +%s) - t0 )) s)" || bad "del git"
apk add -s no-such-package-xyz > /tmp/n 2>&1
grep -q "no such package" /tmp/n && ok "no such package" || bad "missing"
cat /proc/psilinux/mem 2>/dev/null | head -5
echo "passed $pass, $fail failed"
