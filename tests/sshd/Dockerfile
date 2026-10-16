# A throwaway sshd for linux/tests/run-ssh.sh: Alpine's OpenSSH (the same
# release as the Psion's client), a user "psitest" whose password and key
# the test makes at random each run (passed in the environment: nothing is
# kept). Port 22 offers OpenSSH 10's key exchanges; port 2200 only those of
# OpenSSH 9.x (no ML-KEM), as most servers still have.
FROM alpine:3.24
RUN apk add --no-cache openssh-server openssh-keygen && ssh-keygen -A && adduser -D -s /bin/sh psitest
COPY entry.sh /entry.sh
CMD ["/bin/sh", "/entry.sh"]
