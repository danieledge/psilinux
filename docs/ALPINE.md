# Alpine mode

PsiLinux 0.21 can run **Alpine Linux 3.24**: Alpine's own armhf programs,
unmodified, with Alpine's package manager and its whole catalogue. The shell
is bash, and a set of useful (and some fun) programs comes ready to use.

The 5mx's processor (an ARM710T: ARMv4T, no floating point) cannot run
Alpine's programs directly, because they are built for ARMv6 with VFP. So
PsiLinux translates them, a piece at a time, as they first run (DESIGN.md,
"Alpine mode"). PsiLinux's own programs still run natively, as before.

## Installing

| Installer | On | What |
|---|---|---|
| `PsiLinux.sis` | C: or D: | PsiLinux 0.21 (the window and the runner) |
| `PsiLinux-alpine-root.sis` | the Linux disk (D:) | Alpine 3.24.2 with bash and the programs below: `\Linux\Alpine\ROOT.IMG`, 25.3 MB (the installer is 25.9 MB; 0.20's was 7.9 MB) |
| `PsiLinux-root.sis` | the Linux disk | PsiLinux's own programs (optional) |

The Linux disk is D: unless Tools > Linux disk… says C:. What you change in
Alpine (packages, files in /root, /etc) is kept beside the image, in
`\Linux\Alpine\root\`. A new image installs over an old one and keeps
those changes; packages you added stay listed by apk alongside the new
image's (`psilinux-merge-db`, DECISIONS.md 50).

## Starting

Tools > Preferences… > **Start** chooses what Start Linux does:

- **Alpine Linux** boots as Alpine does: BusyBox init, then OpenRC. You are
  logged in as root on the window, in bash. `poweroff` or File > End Linux
  ends it.
- **Alpine Linux, shell only** goes straight to bash. It is quicker and
  leaves more memory free.
- **PsiLinux** runs PsiLinux's own programs (the native root), as in 0.1x.

If the Alpine root is not installed, PsiLinux says so and starts its own
programs instead. About (Shift+Ctrl+A) shows the Alpine release.

## The shell: bash

bash 5.3 is root's shell in both Alpine modes, with:
- the prompt `psion:~#` (the host name and folder: short, for the screen);
- Tab to complete file and command names, Up and Down for earlier commands
  (200 kept between sessions, in `~/.bash_history`), Ctrl+R to search them;
- `ll` (`ls -l`), `la` and `l`; nano as `EDITOR` and `VISUAL`; less as `PAGER`;
- a short fortune at login (an empty file `~/.hushlogin` stops it, and the
  welcome text too).

PsiLinux's settings are in `/etc/bash/psilinux.sh`; yours go in `~/.bashrc`.
bash-completion (completing options and package names) is not installed:
it is 1 MB of scripts that take seconds to load on a 5mx (`apk add
bash-completion` if you want it).

Everything bash does works under the translator: pipes, `$(...)`, `<(...)`,
here-documents, arrays, `[[ ]]`, `read`, job control (`&`, `jobs`, Ctrl+Z,
`fg`, `bg`, `kill %1`) and Ctrl+C. The host harness checks it
(`tests/run-alpine.sh`: the 46 shell tests under bash and under BusyBox sh,
39 bash tests and an interactive session).

bash takes about 2.1 MB while it runs (BusyBox sh about 1.7 MB, most of
which BusyBox's other commands share). If bash is removed or does not
start, the shell is BusyBox sh instead (`psilinux-shell`, and the
`psilinux` service in a full boot), and bash again once it starts.

## What is preinstalled

Measured on the host harness (qemu with an ARMv4T processor, as the 5mx),
each doing something typical. "Memory" is the most the program used: its
image, the libraries it maps and its heap; libraries (musl, ncurses) are
shared with the programs already running. "On disk" is the package itself
(its libraries are counted once, with the first package that needs them).
"Fun" marks the games and toys.

After a full boot to bash a 5mx (in the emulator) has about 5.2 MB free and
2.2 MB more held by caches that give it back when needed: about 7.4 MB for
programs. Shell only leaves more (see TEST-5MX.md for the figures to check
on a real 5mx). Preinstalled means: it runs, it draws correctly, and it fits
beside bash.

| Package | Command | On disk | Memory | Preinstalled | Why |
|---|---|---|---|---|---|
| bash (readline) | bash | 1.4 MB | 2.1 MB | yes | the shell |
| nano | nano | 0.3 MB | 1.6 MB | yes | the editor (`EDITOR`) |
| less | less | 0.2 MB | 1.3 MB | yes | the pager (`PAGER`, man) |
| mandoc | man | 0.4 MB | 1.2 MB | yes | manual pages of what is installed (not bash's: 2.9 MB) |
| htop (lsof) | htop | 0.6 MB | 1.6 MB | yes | memory and CPU (its process list is empty: it does not understand PsiLinux's /proc yet; `ps` and `top` do) |
| ncdu | ncdu | 0.1 MB | 1.3 MB | yes | what uses the disk |
| tree | tree | 0.1 MB | 0.8 MB | yes | folders as a tree |
| jq (oniguruma) | jq | 0.9 MB | 2.2 MB | yes | JSON |
| bc | bc | 0.1 MB | 1.4 MB | yes | a calculator |
| openssh-client | ssh, scp | 3.3 MB | 5.4 MB | yes | other computers; the largest that fits |
| fortune | fortune | 3.3 MB | 0.8 MB | yes, fun | at login too |
| figlet | figlet | 0.6 MB | 0.9 MB | yes, fun | big letters |
| sl | sl | 13 KB | 1.2 MB | yes, fun | a steam train |
| cmatrix | cmatrix | 17 KB | 1.3 MB | yes, fun | falling characters (q ends it) |
| micro-tetris | tetris | 13 KB | 0.7 MB | yes, fun | Tetris; needs 21 rows: the smallest text size |
| bsd-games | worm, snake, robots, adventure, gomoku, hangman... | 0.5 MB | 1.3 MB | yes, fun | the BSD games; adventure is the Colossal Cave; robots needs 24 rows (the smallest text size) |
| cbonsai | cbonsai | 21 KB | 1.3 MB | yes, fun | grows a bonsai (`cbonsai -l`; `-p` prints stray "m"s: its own colour codes are wrong) |
| nyancat | nyancat | 36 KB | 0.7 MB | yes, fun | the cat |
| fastfetch (hwdata-pci) | fastfetch | 3.7 MB | 3.1 MB | yes, fun | says "CPU: Psion Series 5mx" |
| nethack | nethack | 4.1 MB | 4.0 MB | no, fun | fits, but 4 MB of disk: `apk add nethack` |
| git | git | 6.8 MB (+ libraries) | 6.6 MB; `git clone https://` 13 MB more | no | local work fits; fetching over https does not |
| links | links | 1.5 MB | 7.6 MB | no | only in shell-only mode |
| vim | vim | 31 MB | 37 MB editing | no | far too big; `vi` (BusyBox) is there |
| curl | curl | 1.0 MB | 9.5 MB | no | OpenSSL and 12 more libraries |
| file | file | 10.5 MB | 19 MB | no | reads its whole magic database |
| frotz | frotz | 0.1 MB (+ sound libraries) | 9.1 MB | no, fun | its sound libraries |
| asciiquarium | asciiquarium | 32 MB (perl) | | no, fun | needs perl |
| gnuchess | gnuchess | 0.6 MB | | no, fun | too slow to think at 36 MHz |
| tmux | tmux | 0.9 MB | 2.2 MB | no | needs Unix sockets, and its server running beside it |
| rsync | rsync | 0.5 MB | | no | its own copying needs two processes at once |
| python3 | python3 | 26.5 MB | 11.1 MB | no | more than a 5mx has |

Not in Alpine 3.24 for armhf: cowsay, toilet, lolcat, nsnake, bastet,
moon-buggy, ninvaders, greed, 2048 and neofetch (fastfetch takes its place).

Everything above that is not preinstalled stays an `apk add` away, with
apk's memory warning.

The first run of a program is slow, while it is translated: in the
emulator (about 3 times slower than a 5mx) htop took about 80 seconds to
draw the first time, nano about 60, fastfetch 9. Later runs are much
quicker while their translations stay in the code cache.

Text size: the window has 18 rows of 95 columns at the standard size
(Small). Tools > Preferences… > Screen > Text size > Smallest gives the most
rows, which tetris and robots need.

## Packages: apk

`apk` works as on Alpine, with the full main and community catalogue (about
25,000 packages):

```
apk update                 the catalogue's size and date
apk search [-v] [-d] TEXT  find packages (-d: in descriptions too)
apk info [-a] NAME         a package's description and sizes
apk add [-s] NAME...       install (-s: only say what it would do)
apk del NAME...            remove
apk upgrade [-s]           newer versions of what is installed
apk list [-I]              what is installed (-I) or available
```

Alpine's catalogue takes about 30 MB of memory to load, and a 5mx has about
9 MB free. So the catalogue stays on the package server (PsiProxy's `psiapk`
helper), which answers searches and works out what an install needs. The
packages themselves are Alpine's own signed files. The Psion downloads them,
and Alpine's real apk (`/sbin/apk`) installs them from the files, checking
their signatures against Alpine's keys as usual.

Before anything is downloaded, `apk add` says:
- how much it will download, and how much disk it needs;
- whether that fits on the Linux disk (if not, it stops);
- whether the programs and libraries are likely to fit in memory (a
  WARNING if not);
- whether apk itself has room to work (about 7 MB).

**The package server.** apk uses, in this order:
1. `APK_SERVER` set in `/etc/psilinux/apk.conf` (empty as installed), for
   example `APK_SERVER=http://192.168.1.10:8081` (edit it with
   `nano /etc/psilinux/apk.conf`);
2. the proxy in the Psion's Connection settings (PsiTerm, PsiMail or
   PsiWeb), if it is on and is a PsiProxy. The Atom modem's own proxy
   (192.168.7.1, "psiproxy") is not a package server;
3. the dev box set in PsiInstall's Tools > Dev box… (`PsiDevBox.ini`), which
   is a PsiProxy.

With none of these, apk says "No package server set". No address is built in.
`APK_DIRECT=1 apk add …` downloads from Alpine's mirror instead of the server
(the catalogue still comes from the server).

## What runs: the compatibility table

The figures are from the host harness (qemu with an ARMv4T processor, as
the 5mx), installed with the Psion's apk from Alpine's real packages.

Memory is the most a program used: its own image, the libraries it maps and
its heap. The libraries are shared between programs that use the same ones.
(These were measured in 0.20, installed with apk; nano and less are now
preinstalled, with newer figures above.) With BusyBox sh as the shell a 5mx
had about 8.3 MB free after Alpine booted, and about 9 MB in shell-only
mode; with bash, about 7.4 MB is available after a full boot.

| Package | Runs | Download / on disk | Memory | Fits on a 5mx? |
|---|---|---|---|---|
| nano | yes | 0.3 / 0.7 MB | 1.3 MB | yes |
| less | yes | 0.3 / 0.6 MB | 1.3 MB | yes |
| htop | yes | 0.4 / 1.0 MB | 1.3 MB | yes |
| lua5.4 | yes | 0.5 / 1.0 MB | 1.5 MB | yes |
| sqlite (sqlite3) | yes | 1.3 / 2.5 MB | 3.2 MB | yes |
| git | yes | 5.8 / 12.1 MB | 4.7 MB | yes (`git --version`, init, add, commit, log) |
| vim | yes | 10.2 / 31.1 MB | 5.2 MB | yes, but its runtime files need 31 MB of disk |
| links | yes | 1.3 / 2.8 MB | 7.6 MB | only just: in shell-only mode |
| curl | yes (HTTP and HTTPS) | 2.2 / 4.8 MB | 9.5 MB | **probably not**: 13 libraries (OpenSSL alone is 4 MB) |
| python3 | yes | 10.1 / 26.5 MB | 11.1 MB | **no**: more than a 5mx has |

Everything in the table installs and runs on the harness. Where it says
"probably not" or "no", the program needs more memory than a 5mx has free.
apk warns about this before downloading.

**Not run in this table:** anything with threads that both run at once (they
take turns), and Thumb or NEON code, which Alpine's armhf does not use.

## Speed

Most of a program's instructions are copied unchanged and run at the 5mx's
full speed. What costs time:
- translating code the first time it runs;
- instructions that need help (VFP floating point, which is done in software;
  some ARMv6 instructions);
- system calls.

**Against PsiLinux's own programs.** These are the same BusyBox commands, run
in the alignment-faithful emulator. Native is PsiLinux's BusyBox (ARMv4T,
soft float). Alpine is Alpine's BusyBox, translated. The data is a file of
3,000 lines; the times are the second run.

| Command | Native | Alpine | Alpine's first run |
|---|---|---|---|
| a shell loop of 500 turns | 2.4 s | 2.3 s (1.0 times) | 2.5 s |
| md5sum | 0.35 s | 0.47 s (1.3 times) | 0.50 s |
| grep -c | 1.5 s | 2.6 s (1.7 times) | 2.7 s |
| `ls -l /etc` | 0.17 s | 0.32 s (1.9 times) | 0.41 s |
| sort | 2.4 s | 12.0 s (5.1 times) | 21.0 s |
| awk, 3,000 square roots | 1.7 s | 10.1 s (5.9 times) | 10.1 s |

The emulator runs about 3 times slower than a real 5mx, so expect a third of
these times on the device.
- Plain integer code (shell scripts, md5sum) runs at nearly native speed.
- Floating point is the slowest: VFP is emulated, while PsiLinux's own
  programs use the compiler's soft float.
- sort makes many indirect calls (its comparison function), and each goes
  through the translator's dispatcher.

**Instructions executed** (the host harness, M0), translated against native
ARMv6: md5sum 1.6 times, a shell loop 2.1, grep 3.5, awk with floating point
5.2, sort 6.3, and `ls -l /etc` 10 (it is short, so mostly translating).

**Booting** Alpine (BusyBox init and OpenRC, to the login prompt) took about
136 s in the emulator: perhaps a minute on a 5mx. That boot translates
605 KB of code (8,047 blocks) and leaves 8.2 MB free. (0.21, logging in to
bash, with its translations: about 1.6 MB of code, the 1 MB cache flushed
once; about 2 minutes to the prompt, as before.)

The first run of a program is the slowest. Its code stays translated while
there is room (a 1 MB code cache), so running it again is quicker.

## Keeping memory for other programs

Linux can take nearly all the Psion's free memory, and then the Psion's own
programs may not open. Tools > Preferences… > General > **Keep memory for
other programs** (off as standard) keeps 512 KB, 1 MB or 2 MB free for
them: a Linux program that would take more gets "out of memory" (as on any
Linux), and Linux itself goes on. It can be changed while Linux runs.

`cat /proc/psilinux/mem` shows the Psion's free memory, what Linux's caches
hold (they give it back when it is needed), the memory kept, what is
available to Linux, and how often memory was refused. `free`'s "available"
is free and cached memory, less what is kept. The first refusal in a run is
noted in the runner's log.

## Limits

- **Memory:** there is no MMU, so no swap and no demand paging. A program's
  whole image and libraries are in RAM while it runs.
- **fork:** the child runs first, until it calls exec or exits, and then the
  parent goes on. A background subshell (`( … ) &`) therefore does not run
  at the same time as its parent. Pipelines, `$(…)`, `<(…)` and bash's job
  control work; programs that fork a copy to work beside them (tmux's
  server, rsync's local copy) do not.
- **One CPU:** threads take turns.
- **Mounts:** `mount` notes a file system and lists it in /proc/mounts, but
  the file systems are PsiLinux's own.
- **Kernel:** `uname -m` says armv6l and the kernel is "5.15.0-psilinux".
  There are no kernel modules, no /sys devices and no hardware services.
  OpenRC runs as in a container (`rc_sys="lxc"`).
- **Floating point:** VFPv2 is done in software (SoftFloat), exactly as IEEE
  754 asks, but slowly.

## When something goes wrong

- The runner's log is `\Linux\Alpine\PsiLinux.log` on the Linux disk. In
  Alpine mode it has every 30 s a line with free memory and the translator's
  figures. A file named `BOOTLOG` in `\Linux\Alpine\` adds a line for each
  program started, with the time: where a boot got to, if it stops.
- `cat /proc/psilinux/mem` shows the memory (above).
- `cat /proc/psilinux/dbt` shows the translator's figures: the code cache,
  blocks, flushes and unaligned accesses. `cat /proc/psilinux/images` shows
  the programs and libraries that processes share.
- File > End Linux (Shift+Ctrl+D) always stops Linux.
