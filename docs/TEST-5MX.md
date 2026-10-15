# Testing Alpine mode on a real 5mx (PsiLinux 0.21)

PsiLinux 0.21 runs Alpine Linux 3.24 on the 5mx through PsiLinux's
translator, with bash as the shell and a set of programs ready to use. Two things can only be learnt on the real
machine: how the ARM710T handles unaligned accesses and freshly written code
(the probe), and how fast and how stable Alpine is.

Data loss is possible: the Psion may restart in the middle of a test.
**Back up C: and D: first.** Nothing here can harm the hardware.

## 1. The files

All are on the Releases page of github.com/danieledge/psilinux (the test builds were in psiterm's `dist/` on the dev branch).

| File | Size | Install on | What it is |
|---|---|---|---|
| `LxProbe-test.sis` | 25 KB | C: | The hardware probe (a test program) |
| `PsiLinux.sis` | 400 KB | C: | PsiLinux 0.21 with Alpine mode |
| `PsiLinux-alpine-root.sis` | 25.9 MB | **D:** | Alpine Linux 3.24.2 with bash and the programs (`D:\Linux\Alpine\ROOT.IMG`, 25.3 MB) |
| `PsiLinux-root.sis` | 11.8 MB | D: | PsiLinux's own programs. Optional: only for the "PsiLinux" start mode. |

- The Alpine root must be on the disk PsiLinux uses. That is D:, unless
  Tools > Linux disk… says C:.
- If the `.sis` file itself is on D:, the card needs room for both: about
  52 MB free for the Alpine root. Delete the `.sis` after installing.
- Leave a few MB free on D: for the tests in step 6.
- 0.21 installs over 0.19 and 0.20 (PsiLinux and the Alpine root). Your
  changes in `D:\Linux\Alpine\root\` are kept.

## 2. The probe (please do this first)

1. Make sure there is **no** file `D:\LXDBT.TXT`.
2. Open **LxProbe** in Extras. It runs by itself and shows its log.
3. Wait for `== done`. It takes about a minute.
4. The last section (13) takes 1,500 alignment faults in a row. If the Psion
   restarts there, that is a useful answer, not a failure of the test.

**Send back:** `D:\LxProbe.log` (`C:\LxProbe.log` if there is no card). It is
replaced each run, so copy it before running the probe again.

**What good looks like:**
- section 10b: `results right 48 wrong 0` and `flags lost 0`;
- section 11: `Stale: 0, 0 (rewritten), 0 (write-one-run-one)`;
- section 13: a line with the tick counts.

If the probe stops part-way, create `D:\LXDBT.TXT` (any file with that name,
e.g. saved from Word) and run it again. It then runs only the Alpine-mode
sections (10 to 13).

## 3. Alpine, shell only (the quick test)

1. Open PsiLinux. If Linux starts straight away, end it with File > End Linux
   (Shift+Ctrl+D).
2. Go to Tools > Preferences… (Shift+Ctrl+K) and set **Start** to
   **Alpine Linux, shell only**.
3. Choose File > Start Linux (Shift+Ctrl+N). **Time it** until the bash
   prompt `psion:~#` appears (after a short fortune).
4. Type these commands:
   ```
   echo $BASH_VERSION             # 5.3.9(1)-release
   cat /etc/alpine-release        # 3.24.2
   uname -m                       # armv6l
   echo $((6*7))                  # 42
   ls /bin | wc -l
   time ls -l /etc
   time for i in {1..1000}; do :; done
   free
   cat /proc/psilinux/mem
   cat /proc/psilinux/dbt
   ```
5. **Note:**
   - the time to the prompt;
   - the two `time` results (the `real` lines);
   - `free`'s "available" and `/proc/psilinux/mem`;
   - the last section of `/proc/psilinux/dbt` (the "unaligned" line).

   A photo of the screen is fine.

## 4. Alpine, full boot

1. End Linux (Shift+Ctrl+D).
2. Set Preferences > Start to **Alpine Linux**.
3. Start Linux, and **time it** until the `psion:~#` prompt.
   - You should see OpenRC's lines, each ending `[ ok ]`, then the welcome
     text, a fortune and the prompt. In the emulator this took about
     2 minutes 15 seconds.
   - If a line says "bash does not start", note it: root then has BusyBox
     sh (see the end of this sheet).
4. Type `echo $SHELL` (`/bin/bash`), `free`, `cat /proc/psilinux/mem` and
   `cat /proc/psilinux/dbt`, and note what they show.

## 5. The preinstalled programs

In either Alpine mode. Note anything that is slow, draws wrongly or stops.

```
ll /etc | less              # Space pages, q ends
man tree                    # q ends
nano /root/test.txt         # type a line, Ctrl+O Enter saves, Ctrl+X ends
htop                        # q ends
ncdu /usr                   # q ends
echo '{"a":[1,2]}' | jq .a
echo 'scale=10; 4*a(1)' | bc -l
fortune; figlet 5mx
sl                          # a train crosses
cmatrix                     # q ends
tetris                      # q ends
worm                        # Ctrl+C ends
cbonsai -p
fastfetch                   # note how long it takes
free
```

With the network (step 6's connection): `ssh -V`, then `ssh you@a-computer`
if you have one, and note how long the login takes.

Memory to keep for other programs: Tools > Preferences… > General, tick
**Keep memory for other programs**, Memory to keep 2 MB, OK. Then:

```
head -c 6000000 /dev/zero | sort | wc -c     # "out of memory"
cat /proc/psilinux/mem                       # "refused" is not 0
```

Open another Psion program (Word, say) while Linux runs: it should open.

**If it stops** (no new lines for 5 minutes):
- note the last line on the screen;
- try File > End Linux;
- if the Psion itself no longer responds, a soft reset (the reset button) is
  the way out. Note that it happened.

## 6. Optional: packages (needs the network)

This uses the Psion's Internet connection and the package helper (`psiapk`
on PsiProxy; see docs/BUILDING.md). apk uses the proxy set in Connection
settings when it is on (and is your PsiProxy); otherwise set the address in
`/etc/psilinux/apk.conf` (`APK_SERVER=http://<your PC>:8081`, with
`nano /etc/psilinux/apk.conf`).

```
apk update                  # "25135 distinct packages available"
apk search -v nano
apk add -s lua5.4           # sizes only, nothing installed
apk add lua5.4              # downloads, then Alpine's apk installs it
lua5.4 -v
apk del cmatrix && apk add cmatrix   # a preinstalled package removed and back
```

**Note:**
- how long `apk add lua5.4` takes;
- any WARNING lines (memory);
- whether `apk del` and `apk add` of cmatrix work.

## 7. Native PsiLinux still works

Set Preferences > Start to **PsiLinux**, then start Linux.

If PsiLinux's own programs are installed, this is the PsiLinux shell you had
before. If they are not, it says it could not start.

(Separately: if Start is an Alpine mode but the Alpine root is missing,
PsiLinux says so and starts its own programs instead.)

## 8. What to send back

1. `D:\LxProbe.log`.
2. `D:\Linux\Alpine\PsiLinux.log`. This is the runner's log, added to on each
   run. It has a line for each program started, with the time since Linux
   started, and every 30 s a line with free memory and the translator's
   figures. If Alpine stopped or the Psion restarted, the end of this file
   says how far it got.
3. Your notes:
   - the times to the prompt (steps 3 and 4);
   - what step 5's programs did, and anything slow or drawn wrongly;
   - the `time` results;
   - the "unaligned" line;
   - anything that stopped, and where.

## 9. Removing it all

1. In the Control panel, open **Add/remove** and remove:
   - **LxProbe**;
   - **PsiLinux Alpine root**;
   - **PsiLinux** (only if you want PsiLinux gone too);
   - **PsiLinux programs** (the same).
2. Delete these with the file manager:
   - the folder `D:\Linux\Alpine\` (Alpine's changes, in `root\`, and
     `PsiLinux.log`);
   - `D:\LxProbe.log`;
   - `D:\LXDBT.TXT`, if you made it.
3. To go back to an earlier PsiLinux, install its `.sis` again. The settings
   in `C:\System\Apps\PsiLinux\PsiLinux.ini` are kept unless PsiLinux is
   removed.

## Known from the emulator

- In one full boot of three, bash crashed (`signal 11 at 0` in the log)
  when the `psilinux` service tried it, so root had BusyBox sh for that
  session; the next boot gave bash back. The host harness never shows it
  (80 runs). If it happens on the 5mx, send `PsiLinux.log`.
- About one boot in four ends in an emulator fault of its own (the card's
  timer): a reboot, or a blank screen. Not the 5mx's.
