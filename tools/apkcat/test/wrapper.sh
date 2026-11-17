#!/bin/sh
# wrapper.sh - run.sh's part inside an alpine container (BusyBox, as on the
# Psion): the apk wrapper with the online catalogue (http://127.0.0.1:$PB)
# against the same wrapper with psiapk as its PsiProxy (127.0.0.1:$PA).
# Their answers must be the same: install plans, upgrades, info, search.
pass=0; failn=0
ok() { echo "ok   $*"; pass=$((pass + 1)); }
bad() { echo "FAIL $*"; failn=$((failn + 1)); }
CATURL=http://127.0.0.1:$PB/apk/v3.24/armhf
mkdir -p /etc/psilinux
: > /etc/psilinux/apk.conf
online() { APK_SOURCE=online APK_CATALOGUE=$CATURL apk "$@" 2>&1 | grep -v '^Catalogue: '; }
proxy() { APK_SERVER=http://127.0.0.1:$PA apk "$@" 2>&1 | grep -v '^Catalogue: '; }
same() {   # a name, then the apk command
	n=$1; shift
	online "$@" > /tmp/a; proxy "$@" > /tmp/b
	if cmp -s /tmp/a /tmp/b && [ -s /tmp/a ]; then ok "$n"
	else bad "$n"; diff /tmp/b /tmp/a | head -20; fi
}

# Alpine's version order: the wrapper's awk against psiapk's vcmp
sed -n "/^AWKLIB='/,/^'/p" /usr/local/sbin/apk | sed '1d; $d' > /tmp/awklib
awk "$(cat /tmp/awklib)"'
BEGIN { libinit() }
{ r = vcmp($1, $2); if (r != $3) { bad++; if (bad < 10) print "  vcmp " $1 " " $2 ": " r ", psiapk " $3 } n++ }
END { print (bad ? "FAIL" : "ok  ") " version order: " n - bad " of " n " pairs as psiapk" }' /t/vpairs > /tmp/v
cat /tmp/v; grep -q '^ok' /tmp/v && pass=$((pass + 1)) || failn=$((failn + 1))

PKGS="bash git curl nano python3 vim fastfetch gcc htop py3-requests perl-dev gtk+3.0 R nethack aws-sdk-cpp-dev"
for db in db-image db-small; do
	[ -f /t/$db ] || continue
	cp /t/$db /lib/apk/db/installed
	echo "-- installed: $db ($(grep -c '^P:' /lib/apk/db/installed) packages)"
	for p in $PKGS; do same "add -s $p ($db)" add -s $p; done
	same "add -s git vim curl ($db)" add -s git vim curl
	same "add -s cmd:git: not a name ($db)" add -s cmd:git
	same "add -s no-such-package ($db)" add -s no-such-package-xyz
done
cp /t/db-old /lib/apk/db/installed
echo "-- installed: db-old ($(grep -c '^P:' /lib/apk/db/installed) packages, a third made older)"
same "upgrade -s (db-old)" upgrade -s
same "add -s git (db-old)" add -s git
cp /t/db-small /lib/apk/db/installed

echo "-- search, info, list"
same "update" update
for q in git vim fastf nano py3-requests 'py3-req*' '*curl*' 'lib*ssl*' 'gtk+'; do same "search $q" search "$q"; done
same "search -v curl" search -v curl
same "search -v git vim" search -v git vim
for p in git gcc py3-requests gtk+3.0 R aws-sdk-cpp-dev; do same "info -a $p" info -a $p; done
same "info no-such" info no-such-package-xyz
same "list py3-req*" list 'py3-req*'
# (the searches the catalogue answers its own way: shown, not compared)
for q in "-d editor" "lib" "vi"; do
	online search $q > /tmp/a; proxy search $q > /tmp/b
	echo "  (search $q: $(wc -l < /tmp/a) lines online, $(wc -l < /tmp/b) from psiapk, $(grep -cxFf /tmp/b /tmp/a) the same)"
done

echo "-- sources and messages"
APK_SOURCE=none apk add git > /tmp/a 2>&1; grep -q "turned off" /tmp/a && ok "none: says packages are off" || { bad "none"; cat /tmp/a; }
APK_SOURCE=online APK_CATALOGUE=http://127.0.0.1:9/x apk search git > /tmp/a 2>&1
rm -rf /var/cache/apk/psion
APK_SOURCE=online APK_CATALOGUE=http://127.0.0.1:9/x apk add git > /tmp/a 2>&1
grep -q "cannot reach Alpine's online catalogue" /tmp/a && grep -q "still work" /tmp/a && ok "offline: says what still works" || { bad "offline"; cat /tmp/a; }
APK_SOURCE=online APK_CATALOGUE=$CATURL apk update > /tmp/a 2>&1
grep -q "^Catalogue: Alpine's online catalogue of 20" /tmp/a && grep -q "OK: [0-9]* distinct" /tmp/a && ok "update: the source and its age" || { bad "update"; cat /tmp/a; }
# (a stale copy is used when the catalogue is out of reach)
APK_SOURCE=online APK_CATALOGUE=$CATURL apk info -a git > /dev/null 2>&1
touch -d "2000-01-01" /var/cache/apk/psion/stats
APK_SOURCE=online APK_CATALOGUE=http://127.0.0.1:9/x apk info -a git > /tmp/a 2>&1
grep -q "WARNING: cannot reach the online catalogue: using the copy" /tmp/a && grep -q "git-.* description" /tmp/a && ok "offline with a copy: warns, still answers" || { bad "stale copy"; cat /tmp/a; }
apk source online > /dev/null && grep -q '^APK_SOURCE=online' /etc/psilinux/apk.conf && ok "apk source online saves it" || bad "source online"
apk source psiproxy 192.168.1.10 > /dev/null && grep -q '^APK_SERVER=http://192.168.1.10:8081' /etc/psilinux/apk.conf && grep -q '^APK_SOURCE=psiproxy' /etc/psilinux/apk.conf && ok "apk source psiproxy ADDR saves it" || bad "source psiproxy"
apk source | grep -q "your PsiProxy at http://192.168.1.10:8081" && ok "apk source shows it" || bad "source show"
apk source none > /dev/null; apk source | grep -q "^Packages: none" && ok "apk source none" || bad "source none"
apk source auto > /dev/null; apk source | grep -q "online catalogue (the standard choice)" && ok "apk source auto: online with no proxy set" || bad "source auto"
# a file that comes damaged (zeros, as BusyBox wget's https can give): refused
rm -rf /var/cache/apk/psion
printf 'name\tmc\nversion\t1\000\000\000\n' > /t/site/apk/v3.24/armhf/p/mc
APK_SOURCE=online APK_CATALOGUE=$CATURL apk add -s mc > /tmp/a 2>&1
grep -q "came damaged twice" /tmp/a && ok "a damaged file is refused" || { bad "damaged"; cat /tmp/a; }
rm -f /etc/psilinux/.apk-source-told
a=$(apk source --first); b=$(apk source --first)
[ -n "$a" ] && [ -z "$b" ] && ok "first start: one line, once ($a)" || bad "first start: '$a' '$b'"
echo "passed $pass, $failn failed"

