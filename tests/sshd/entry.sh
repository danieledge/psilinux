#!/bin/sh
# (run-ssh.sh's sshd) the password and key come from the test, in the environment
echo "psitest:$PSITEST_PASS" | chpasswd 2>/dev/null
mkdir -p /home/psitest/.ssh
echo "$PSITEST_KEY" > /home/psitest/.ssh/authorized_keys
chown -R psitest /home/psitest/.ssh
chmod 700 /home/psitest/.ssh; chmod 600 /home/psitest/.ssh/authorized_keys
O="-e -o PasswordAuthentication=yes -o LoginGraceTime=600"
/usr/sbin/sshd $O -p 2200 -o KexAlgorithms=sntrup761x25519-sha512,sntrup761x25519-sha512@openssh.com,curve25519-sha256,curve25519-sha256@libssh.org,ecdh-sha2-nistp256,diffie-hellman-group14-sha256
exec /usr/sbin/sshd -D $O -p 22
