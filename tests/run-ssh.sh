#!/bin/bash
# run-ssh.sh - ssh from Alpine mode on the host harness (psihost under
# qemu-arm -cpu ti925t, an ARMv4T as the 5mx), against a throwaway sshd in
# docker (linux/tests/sshd). Run it on the host, beside docker:
#   linux/tests/run-ssh.sh
# Each run makes a new key and a random password for the sshd's only user,
# passed in the environment, and removes them at the end. It checks:
#   - a key login and a command (`echo`), with /etc/ssh/ssh_config.d/psilinux.conf's
#     choices (ML-KEM against OpenSSH 10; never sntrup761);
#   - against a server without ML-KEM (OpenSSH 9.x's choices): curve25519;
#   - interactively, in a pseudo-terminal, by name (the kernel's DNS answerer):
#     the first-connection question, the password, a command, exit.
# (The Alpine root: build/alpine-root, from linux/alpine/build-root.sh.)
set -e
REPO=$(cd "$(dirname "$0")/../.." && pwd)
B=$REPO/build/linux
T=$B/ssh-test
NAME=psilinux-sshd-$$

if [ "$1" = inner ]; then
	# (in psilinux-build, in the sshd's network: 127.0.0.1:22 is the sshd)
	cd "$B"
	fail=0
	fresh() { rm -rf "$T/root"; mkdir -p "$T/root"; for f in "$REPO"/build/alpine-root/ROOT.I*; do ln "$f" "$T/root/" 2>/dev/null || cp "$f" "$T/root/"; done; }
	check() {   # name, log, what must be in it
		if grep -q -- "$3" "$2"; then echo "ok   $1"; else echo "FAIL $1: see ${2#$REPO/}"; fail=1; fi
	}
	key='cp /t/key /root/k; chmod 600 /root/k'
	opt='-i /root/k -o StrictHostKeyChecking=accept-new -o BatchMode=yes'

	echo "== a key login"
	fresh
	LX_DRIVE_T=$T timeout 900 qemu-arm -cpu ti925t ./psihost -r "$T/root" /bin/sh -c \
		"$key; ssh -G x | grep -E '^(kexalgorithms|updatehostkeys|serveraliveinterval) '; ssh -v $opt psitest@127.0.0.1 'echo ok-\$((6*7))'" \
		< /dev/null > "$T/key.log" 2>&1 || true
	check "logged in, ran a command" "$T/key.log" "^ok-42"
	check "ML-KEM with OpenSSH 10" "$T/key.log" "kex: algorithm: mlkem768x25519-sha256"
	if grep -q "^kexalgorithms .*sntrup" "$T/key.log"; then echo "FAIL sntrup761 is offered (psilinux.conf)"; fail=1; else echo "ok   sntrup761 is not offered"; fi

	echo "== a server without ML-KEM (as OpenSSH 9.x)"
	LX_DRIVE_T=$T timeout 900 qemu-arm -cpu ti925t ./psihost -r "$T/root" /bin/sh -c \
		"$key; ssh -v -p 2200 $opt psitest@127.0.0.1 'echo ok-\$((6*7))'" \
		< /dev/null > "$T/old.log" 2>&1 || true
	check "logged in" "$T/old.log" "^ok-42"
	check "curve25519, not sntrup761" "$T/old.log" "kex: algorithm: curve25519-sha256"

	echo "== interactive, by name: the first-connection question and the password"
	fresh
	python3 "$REPO/linux/tests/ptydrive.py" "$T/root" "$T/pty.log" 'wait:# :300' \
		'send:ssh psitest@psisshd\r' 'wait:continue connecting:600' 'send:yes\r' \
		'wait:password:600' 'sendenv:PSITEST_PASS' 'wait:\$ :600' \
		'send:echo ok-$((6*7))\r' 'wait:ok-42:120' 'send:exit\r' 'wait:closed[^#]*# :120' \
		'send:ssh -o BatchMode=yes psitest@psisshd true; echo again=$?\r' 'wait:again=:600' \
		'send:exit\r' 'sleep:3' -- /bin/bash -il > "$T/pty.out" 2>&1 || true
	grep -E "TIMEOUT|^OK|FAILED" "$T/pty.out" | sed 's/^/     /'
	check "first-connection question, password, command, exit" "$T/pty.out" "^OK"
	check "the host key was kept" "$T/pty.log" "Permanently added 'psisshd'"
	# (BatchMode: no password, so "again" fails to log in, but asks nothing about the host)
	if grep -a -A3 "BatchMode" "$T/pty.log" | grep -q "authenticity"; then echo "FAIL the host was asked about again"; fail=1; else echo "ok   known the second time"; fi
	exit $fail
fi

# ---- on the host -----------------------------------------------------------------
command -v docker > /dev/null || { echo "run-ssh.sh needs docker"; exit 1; }
"$REPO/linux/tools/lxbuild" "make -C linux/runtime host" > /dev/null
docker build -q -t psilinux-sshd-test "$REPO/linux/tests/sshd" > /dev/null
rm -rf "$T"; mkdir -p "$T"; chmod 700 "$T"
cleanup() { docker rm -f "$NAME" > /dev/null 2>&1 || true; rm -rf "$T/key" "$T/key.pub" "$T/root"; }
trap cleanup EXIT
# a new key and password, this run only
docker run --rm --user "$(id -u):$(id -g)" -v "$T:/k" psilinux-sshd-test ssh-keygen -q -t ed25519 -N '' -C psilinux-test -f /k/key
PSITEST_PASS=$(head -c 24 /dev/urandom | base64 | tr -dc 'A-Za-z0-9' | head -c 20)
PSITEST_KEY=$(cat "$T/key.pub")
export PSITEST_PASS PSITEST_KEY
docker run -d --rm --name "$NAME" --hostname psisshd -e PSITEST_PASS -e PSITEST_KEY psilinux-sshd-test > /dev/null
for i in 1 2 3 4 5 6 7 8 9 10; do docker logs "$NAME" 2>&1 | grep -q "listening on .* port 22" && break; sleep 1; done
W=$(cd "$REPO/.." && pwd)
docker run --rm -i --user "$(id -u):$(id -g)" --network "container:$NAME" \
	-v "$W:$W" -w "$PWD" -e HOME="$W" -e PSITEST_PASS \
	psilinux-build bash "$REPO/linux/tests/run-ssh.sh" inner
