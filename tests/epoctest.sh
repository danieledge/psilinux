#!/bin/sh
# epoctest.sh - the epoc command and the Psion's disks under /psion, end to
# end on the host harness: the epoc command (build/epoc/epoc), the kernel's
# /dev/epoc and path names, and psihost's pretend PsiLinux.app (bk_host.c).
#   tools/lxbuild "tests/epoctest.sh"
# The pretend C: and Z: disks are folders here (LX_DRIVE_C, LX_DRIVE_Z);
# the epoc command is in "the runner's folder" (LX_SELF), as on the Psion.
set -e
REPO=$(cd "$(dirname "$0")/.." && pwd)
make -C "$REPO/runtime" host > /dev/null
make -C "$REPO/epoc" > /dev/null
make -C "$REPO/epoc" test
B=$REPO/build/linux
T=$B/epoc-test
rm -rf "$T"; mkdir -p "$T/root" "$T/C/Documents" "$T/C/System/Apps/Thing" "$T/Z/System" "$T/self/bin"
cp "$REPO"/build/psilinux-root/ROOT.I* "$T/root/"
cp "$REPO/build/epoc/epoc" "$T/self/bin/epoc"
echo "Dear Sir" > "$T/C/Documents/Letter.txt"
echo "keep" > "$T/C/System/Apps/Thing/Thing.app"
echo "rom" > "$T/Z/System/Rom.txt"
# the tests themselves run in Linux, from the C: disk
cat > "$T/C/t.sh" <<'EOF'
pass=0; fail=0
ok() { if [ "$1" = "$2" ]; then pass=$((pass+1)); else fail=$((fail+1)); echo "FAIL $3: got [$1] wanted [$2]"; fi; }
E=/psion/bin/epoc
# the disks
ok "$(ls /psion | tr '\n' ' ')" "bin c z " "ls /psion"
ok "$(cat /psion/c/Documents/Letter.txt)" "Dear Sir" "read a file"
echo new > /psion/c/Documents/New.txt; ok "$(cat /c/Documents/New.txt)" "new" "write, and the same disk at /c"
ok "$(ls -ld /psion/z/System | cut -c1-10)" "dr-xr-xr-x" "Z: is read-only"
(echo x > /psion/z/x.txt) 2>/dev/null; ok "$?" "1" "Z: refuses a write"
ok "$(ls -ld /psion/c/System | cut -c1-10)" "dr-xr-xr-x" "\\System shows read-only"
rm -f /psion/c/System/Apps/Thing/Thing.app 2>/dev/null; ok "$(cat /psion/c/System/Apps/Thing/Thing.app)" "keep" "rm in \\System refused"
(echo bad > /psion/c/System/x.txt) 2>/dev/null; ok "$?" "1" "a new file in \\System refused"
mv /psion/c/Documents/New.txt /psion/c/System/ 2>/dev/null; ok "$?" "1" "mv into \\System refused"
ok "$($E syswrite)" "off" "syswrite says off"
$E syswrite on 2>/dev/null; ok "$?" "4" "syswrite on needs --force"
$E syswrite on --force > /dev/null; echo ok > /psion/c/System/x.txt; ok "$(cat /psion/c/System/x.txt)" "ok" "writing \\System when unlocked"
rm /psion/c/System/x.txt; $E syswrite off > /dev/null
(echo bad > /psion/c/System/y.txt) 2>/dev/null; ok "$?" "1" "locked again"
chmod a-w /psion/c/Documents/Letter.txt; ok "$(ls -l /psion/c/Documents/Letter.txt | cut -c1-10)" "-r-xr-xr-x" "chmod a-w: read-only"
chmod u+w /psion/c/Documents/Letter.txt; ok "$(ls -l /psion/c/Documents/Letter.txt | cut -c1-10)" "-rwxr-xr-x" "chmod u+w"
mkdir /psion/x 2>/dev/null; ok "$?" "1" "nothing new in /psion itself"
# epoc: paths, programs, lists
cd /psion/c/Documents
ok "$($E path Letter.txt)" "C:/Documents/Letter.txt" "epoc path (the host's / for EPOC's \\)"
$E path /etc/passwd 2>/dev/null; ok "$?" "4" "a file not on a Psion disk"
$E open Letter.txt; ok "$?" "0" "epoc open"
$E open Nothing.txt 2>/dev/null; ok "$?" "3" "epoc open: no such file"
xdg-open() { $E open "$@"; }
ok "$($E run Word; echo $?)" "0" "epoc run Word"
$E run Nonesuch 2>/dev/null; ok "$?" "3" "epoc run: no such program"
$E run --wait Word; ok "$?" "0" "epoc run --wait (the pretend Word closes)"
$E --timeout 1 run --wait Word 2>/dev/null; ok "$?" "6" "--wait --timeout"
# (0.38, RR-05) run returns once the program has started: its window on the
# task list, in front unless --bg; --no-wait at once
$E run --no-wait Word; ok "$($E tasks | cut -f3 | tr '\n' ' ')" "PsiLinux " "epoc run --no-wait: returns before the start"
$E tasks > /dev/null; $E tasks > /dev/null
$E run Word; ok "$($E tasks | grep fg | cut -f3)" "Word" "epoc run: returns once Word is up, in front"
$E run --bg Word; ok "$($E tasks | grep fg | cut -f3) $($E tasks | cut -f3 | tr '\n' ' ')" "PsiLinux Word PsiLinux " "epoc run --bg: up, behind"
$E run Crash 2>/dev/null; ok "$?" "1" "epoc run: a program that ends as it starts"
$E --timeout 1 run Slow 2>/dev/null; ok "$?" "6" "epoc run: a program that never shows, --timeout"
$E --timeout 1 run --no-wait Slow; ok "$?" "0" "epoc run --no-wait: no wait for the start"
ok "$($E tasks | cut -f3 | tr '\n' ' ')" "Word PsiLinux " "epoc tasks"
ok "$($E -H ps | head -1)" "$(printf 'id\tname\tuid\tmem_kb\tpriority\tfile\tsys')" "epoc ps -H"
ok "$($E ps --json | grep -c '\"name\"')" "3" "epoc ps --json"
$E kill EFile 2>/dev/null; ok "$?" "4" "epoc kill refuses EPOC's own"
$E kill Word; ok "$?" "0" "epoc kill Word"
$E close Nobody 2>/dev/null; ok "$?" "3" "epoc close: not there"
ok "$($E info machine)" "Psion Series 5mx" "epoc info KEY"
ok "$($E info --json | grep -c '\"disk\": \[')" "2" "epoc info --json"
ok "$($E time offset)" "+0000" "epoc time KEY"
ok "$($E printers | cut -f1)" "Epson ESC/P2" "epoc printers"
$E print Letter.txt 2>/dev/null; ok "$?" "5" "epoc print: not on EPOC"
# talking to you
$E note "Backup done"; ok "$?" "0" "epoc note"
$E alert "Hello" "World"; ok "$?" "0" "epoc alert"
$E confirm "Delete?"; ok "$?" "0" "epoc confirm (yes)"
$E beep; ok "$?" "0" "epoc beep"
echo "from a pipe" | $E clip set; ok "$($E clip get)" "from a pipe" "epoc clip, through pipes"
$E clip set one two; ok "$($E clip)" "one two" "epoc clip set TEXT"
$E shot /psion/c/shot.png > /dev/null; ok "$(cat /psion/c/shot.png)" "pretend screenshot" "epoc shot FILE"
$E shot /psion/c/System/shot.png 2>/dev/null; ok "$?" "4" "epoc shot into \\System is refused"
ok "$(ls /psion/c/System/shot.png 2>/dev/null)" "" "...and nothing is written there"
# (0.42) the file as an EPOC name (no Linux path: the kernel's test on its /psion name)
$E shot 'C:\System\raw.png' 2>/dev/null; ok "$?" "4" "epoc shot C:\\System\\... is refused"
ok "$(ls /psion/c/System/raw.png 2>/dev/null)" "" "...and nothing is written there"
$E shot 'Z:\raw.png' 2>/dev/null; ok "$?" "4" "epoc shot Z:\\... is refused"
$E shot 'c:\Documents\..\System\raw.png' 2>/dev/null; ok "$?" "4" "epoc shot ..\\System is refused"
$E shot 'C:\a/b.png' 2>/dev/null; ok "$?" "4" "epoc shot: a / in an EPOC name"
$E shot 'C:\raw.png' > /dev/null; ok "$(cat /psion/c/raw.png)" "pretend screenshot" "epoc shot C:\\FILE"
# for scripts
$E nonesuch 2>/dev/null; ok "$?" "2" "a bad command: 2"
ok "$($E note quietly)" "" "quiet on success"
ok "$($E version | head -1)" "$(printf 'epoc\t0.38')" "epoc version"
ok "$($E version | grep '^proto' | cut -f2)" "1" "the kernel's protocol"
(sleep 2; echo later > /psion/c/later.txt) & $E --timeout 10 wait-file /psion/c/later.txt; ok "$?" "0" "wait-file"
$E --timeout 1 wait-file /psion/c/never.txt; ok "$?" "6" "wait-file --timeout"
$E wait-battery 50; ok "$?" "0" "wait-battery (80%)"
# not from a pseudo-terminal or with no terminal at all (ssh, daemons)
setsid $E note "from a daemon" 2>/dev/null; ok "$?" "4" "no terminal: refused"
echo "epoctest: $pass passed, $fail failed"
EOF
cd "$B"
LX_DRIVE_C=$T/C LX_DRIVE_Z=$T/Z LX_SELF=$T/self/ timeout 300 qemu-arm ./psihost -r "$T/root" /bin/sh /c/t.sh < /dev/null > "$T/log" 2> "$T/err" || true
cat "$T/log"
grep -q " 0 failed" "$T/log"
