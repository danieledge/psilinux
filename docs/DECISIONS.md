# PsiLinux: decisions

Each entry records what was chosen, what else was possible, and why.

## 1. No svc in programs: musl calls one function (`__psi_svc`)
- **Chosen:** patch musl so every system call is `bl __psi_svc`, which jumps to
  the runner's entry from AT_SYSINFO (or does a real svc on Linux).
- **Alternatives:** scan executable segments for `svc 0` and patch each into a
  branch; trap svc in the EPOC kernel with a driver; binary
  translation.
- **Why:** M0 showed svc 0 restarts the 5mx, so a missed svc is a crash, and a
  wrong patch (literal pools hold data that looks like svc) corrupts a program.
  A single known function is exact. A kernel driver would be ROM-specific
  (ordinals) and could take the machine down. The same binaries also run on
  real ARM Linux, which the tests use.

## 2. A kernel thread plus one EPOC thread per Linux thread
- **Chosen:** the program's code runs on its own RThread; system calls are
  requests to one kernel thread.
- **Alternatives:** green threads on one EPOC thread (switch at system calls,
  preempt with RThread::RaiseException); handle calls on the caller's thread
  with locks and shared handles.
- **Why:** preemption by RaiseException took the emulated machine down (M0).
  Green threads without preemption let one busy program freeze the others. One
  kernel thread owns every EPOC handle, so no locks, no handle sharing, and a
  Linux thread can be killed at any moment. A call costs about 20 µs on a 5mx
  (two requests), small next to F32 or a pipe.

## 3. The program relocates itself (musl's rcrt1)
- **Chosen:** load PT_LOAD segments as they are; rcrt1 applies the relative
  relocations, using AT_PHDR.
- **Alternatives:** the runner relocates (the brief's first idea); fixed-address
  (non-PIE) programs.
- **Why:** rcrt1 is what Linux itself relies on for static-pie, so nothing is
  duplicated and there is one way relocation can go wrong. Applying them in the
  runner too would relocate twice. Non-PIE programs would all want the same
  address.

## 4. vfork and posix_spawn; fork by snapshot
- **Chosen:** vfork (and clone with CLONE_VFORK) run the child on the parent's
  thread until execve or _exit. fork does the same after saving the parent's
  writable memory, and puts it back before the parent goes on.
- **Alternatives:** fork as ENOSYS (the first version); copying the child to
  another address (impossible: its pointers would be wrong).
- **Why:** uClinux's model covers BusyBox, musl's posix_spawn and `system()`.
  The snapshot adds the programs that fork and then exec or exit, git among
  them (`git clone` over https works through it on the host harness), at the cost of a copy of
  the parent's data for as long as the child borrows it. Programs whose
  parent and child must both run on cannot work without an MMU whatever is
  done: the parent waits.

## 5. hush, not ash
- **Chosen:** BusyBox's hush as /bin/sh.
- **Alternatives:** ash (the brief named it).
- **Why:** BusyBox's ash depends on !NOMMU (it forks for subshells); hush is
  BusyBox's shell for machines without an MMU. It has job control, functions,
  `$(...)`, here-documents and line editing. It has no aliases: /etc/profile
  uses functions.

## 6. The root: a read-only image under a writable layer
- **Chosen:** ROOT.IMG (our own simple format) + a folder on the Psion's disk,
  as an overlay with whiteouts; links and modes in `_LXMETA` files.
- **Alternatives:** unpack a tarball onto the card on first run; a FAT folder
  only, with links emulated by special files.
- **Why:** BusyBox alone is 200 links. The emulator's card writer holds about
  16 entries a folder, and on the 5mx thousands of small files cost time and
  card space (FAT clusters). One image file is quick to copy and to read,
  keeps links and modes exactly, and can be replaced to update every program,
  while the user's changes stay in the layer.

## 7. /c and /d are the Psion's disks, with every file 0755
- **Chosen:** mount each disk at /<letter>, files shown 0755 (folders 0755,
  read-only files 0555); chmod quietly does nothing there.
- **Alternatives:** keep modes in `_LXMETA` there too; 0644.
- **Why:** these are the user's own folders: PsiLinux must not leave files in
  them. 0755 lets scripts on the card run (as WSL1's /mnt/c shows rwx).

## 8. Signals at system calls, default actions at once
- **Chosen:** handlers run when the thread's call returns; ending or stopping
  a process acts at once (Kill, Suspend).
- **Alternatives:** RaiseException to interrupt the thread anywhere.
- **Why:** see 2. A second ^C ends a job stuck in a loop that makes no calls.

## 9. Atomics: a SWP lock; thread pointer: found by sp
- **Chosen:** the runner's cas takes a SWP lock; gettp finds the thread by its
  stack pointer.
- **Alternatives:** CP15 (not on ARMv4); a restartable sequence (needs the
  kernel to rewind preempted threads, which EPOC will not do for us);
  RThread::Id() and a table (an executive call each time).
- **Why:** both work in user mode on ARMv4T with no help from EPOC, and are a
  few instructions.

## 10. The program cache
- **Chosen:** keep the bytes of the last 4 programs run (2 MB at most, each in
  a chunk of its own), drop it when free RAM is under 3 MB.
- **Alternatives:** share text between processes (impossible: text reaches its
  data by fixed offsets, so a second copy of the data needs a second copy of
  the text); read from the card every time.
- **Why:** in the emulator every command took tens of seconds to read BusyBox;
  a copy in RAM is about 45 ms on a 5mx. The first version kept the cache on
  the runner's heap: F32 reading an 825 KB program into one heap cell made the
  emulated 5mx's kernel fault (a NULL read in kernel mode) and stop. In chunks
  it works, and a dropped program goes straight back to EPOC.

## 11. BusyBox trimmed
- **Chosen:** defconfig, CONFIG_NOMMU, minus 191 applets that mean nothing on a
  Psion (init systems, disks and partitions, kernel modules, daemons, raw
  sockets, consoles): 825 KB, 204 applets.
- **Why:** every process holds its own copy of the image, so its size is memory
  per command in a pipeline.

## 12. Reading programs in 32 KB pieces
- **Chosen:** the loader reads at most 32 KB per F32 request.
- **Why:** one 1.2 MB read wedged the emulator's card on some card layouts
  (the README's known flake), and long requests hold the file server.

## 13. `|` on Shift+Ctrl+I
- **Chosen:** Shift+Ctrl+I types `|`; Keys > Symbols has `|`, `` ` `` and `~`.
- **Why:** the 5mx keyboard has no key for `|` (probed: Fn and Shift+Fn with
  every letter and digit), and a shell needs it constantly. Shift+Ctrl+I is
  "Merge in", which has no place in a terminal.

## 14. Ctrl+Q switches views
- **Chosen:** Ctrl+Q (the style guide's Switch view) cycles between the
  terminal and Remote apps, even though every other plain Ctrl+letter goes to
  Linux.
- **Why:** it is the style guide's key for it; ^Q (XON) is the one Ctrl key a Linux user
  can spare (output flow control is rarely used, and ^S/^Q stay usable from
  the Keys menu if added).

## 15. The DNS answerer in the kernel
- **Chosen:** /etc/resolv.conf names 127.0.0.53; the kernel answers DNS queries
  sent there by asking EPOC (RHostResolver).
- **Alternatives:** find the PPP link's DNS server and let musl ask it.
- **Why:** NIFMAN knows the DNS servers, programs do not; EPOC's resolver also
  starts the dial-up when needed, as every other Psion program expects.

## 16. GCC's atomics without kuser helpers (psi_atomic.o)
- **Chosen:** lx-cc links `psi_atomic.o` into every program: GCC's 65
  `__sync_*` functions, built on musl's compare-and-swap (the runner's).
- **Alternatives:** patch and rebuild libgcc (`linux-atomic.c`); find and
  refuse programs that use them.
- **Why:** on ARMv4T libgcc implements them with the kuser helpers at
  0xffff0fc0, which EPOC has not got: curl jumped there and faulted. An object
  linked ahead of libgcc replaces them for every program without rebuilding
  GCC; it defines the same 65 symbols, so libgcc's are never pulled in.

## 17. A build-time check for svc, not a load-time scan
- **Found:** it caught libgcc's `__clear_cache` (an inline svc, which
  MicroPython calls for its native code): `psi_atomic.c` now has one that asks
  through `__psi_svc`.
- **Chosen:** lx-cc disassembles every program it links and fails the build if
  any svc is outside `__psi_svc`. The runner trusts programs with the
  PsiLinux note.
- **Alternatives:** scan every program at load.
- **Why:** at link time the program still has its ARM mapping symbols, so
  objdump knows code from data; a stripped program on the Psion has none, and
  a scan would refuse good programs whose literal pools look like svc.

## 18. wget's own TLS
- **Chosen:** BusyBox wget with its built-in TLS (CONFIG_FEATURE_WGET_OPENSSL
  off); curl with Mbed TLS for everything else.
- **Why:** with the OpenSSL helper, wget vforks to run `openssl`, and when that
  fails the child calls exit(), not _exit(): without an MMU that runs exit in
  the parent's memory, and musl's exit lock then stops the parent (SIGILL).

## 19. Two installers
- **Chosen:** `dist/PsiLinux.sis` (the app, the runner, the font and the C
  library, 325 KB) and `dist/PsiLinux-root.sis` (ROOT.IMG, 11.8 MB with git,
  to `!:\Linux\`).
- **Alternatives:** one SIS; the app unpacking a tarball on first run.
- **Why:** the programs and the root update separately, and the 5 MB image can
  go on the card while the program goes anywhere. Unpacking thousands of small
  files on first run would be slow and fill FAT clusters (see 6).

## 20. /tmp in the root's writable layer
- **Chosen:** /tmp is part of the layer (`<disk>:\Linux\root\tmp\`).
- **Alternatives:** its own FAT folder, mounted.
- **Why:** programs make links and set modes in /tmp all the time; as part of
  the layer it has both.

## 21. The runner's timer is set again only when it must go off sooner
- **Chosen:** after a system call the kernel's wake-up RTimer is cancelled and
  set again only if it must go off before the time it is set for.
- **Alternatives:** cancel and set it after every call (the first version).
- **Why:** with hundreds of Cancel/After a second, the emulated 5mx stopped
  now and then: a kernel fault in the timer queue's insert (PC 0x5003f670; a
  queued timer's link read as NULL, so the kernel followed the exception
  vector's word, 0xe59ff020). The change is cheaper anyway (a timer that goes
  off early costs one look at the parked calls), so it stays.
- **But it did not cure the fault.** Later A/B runs (the same M2 suite, the
  start time varied) faulted 1 in 3 with the change and 1 in 3 without. All 9
  faulted runs failed during a card transfer, 0.6 to 2.6 ms after the
  emulator's "CF direct-invoke" (the emulator calls the card driver's retry
  callback itself, with interrupts off, to work round its slow card
  interrupts; the callback re-arms its own kernel timer, which may still be
  queued). In one run the fault followed the driver's data read in the same
  instant. Runs that read little from the card (M1, m4test, `curl
  --version`) have not faulted; runs that read tens of thousands of sectors
  (M2, git) fault or the card stops answering (the emulator's known card
  wedge). Turning direct-invoke off (`PSION_NO_CF_DIRECT_INVOKE=1`) leaves the
  card too slow to start Linux, so it cannot be ruled out that way.
- **Verdict:** an emulator artefact as far as can be shown here. **Run a long
  job (the M2 suite, `git init`/`add`/`commit`) on a real 5mx to confirm.**
- **0.37: first starts of Alpine on a fresh card** hang in the 5mx emulator
  about 2 times in 5 (the banner work saw 3 in 15, at `mv` in
  psilinux-seed), with the clock stopped: 7 of 15 with 0.37 before
  decision 63, 6 of 15 after it. Every one stops during the root image's
  reads, at a card transfer: with the emulator's logs on, either this
  fault (PC 0x5003f670, FAR 0xe59ff020, then the card driver's own
  faults) or the card driver left with its interrupt masked
  (PcCdIntMask 0) and no further transfer. The point is the same from run
  to run (the reads are the same; the RTC is what varies), and moves when
  the runner's code changes. Not ours, as far as can be told; the
  emulator's `EMU_LOGS=1` log and `CF_STATS` (`sector_drains` stopping
  short) show which runs it hit.

## 22. git
- **Chosen:** git 2.47 with https through libcurl and Mbed TLS; no Perl,
  Python, Tcl or gettext; git's own regex (musl's lacks REG_STARTEND).
- **Why:** it is the most-asked-for "real" program, and with fork by snapshot
  it works: init, add, commit, log, diff, status, clone over https (tested on
  the host harness). It is big (git 3.2 MB, git-remote-https 3 MB, each
  process a copy), so on a 5mx it suits small repositories.
- **In the emulator:** `git --version` works (3 runs of 4; the fourth hit the
  emulator's card wedge while reading git), but a whole `init`/`add`/`commit`
  reads git from the card again for each command (it is over the program
  cache's 1 MB limit) and runs into the emulator's card fault (see 21) before
  it finishes. Not yet seen to finish in the emulator; to try on the 5mx.

## 23. The compare-and-swap lock records its holder
- **Chosen:** the SWP lock holds the holder's stack pointer, not 1. When a
  thread dies, the kernel lets the lock go if the value lies in that thread's
  stack.
- **Why:** a thread killed inside the lock's six instructions (a second ^C,
  exit_group ending the other threads, a fault) would leave it held, and
  every other Linux thread would then wait for it for ever. The stack
  pointer says whose it is at no cost.

# Alpine mode (0.20)

Alpine Linux's own armhf programs, unmodified, through a binary translator:
[ALPINE.md](ALPINE.md) for using it, DESIGN.md "Alpine mode" for how.

## 24. Alpine's programs through an ARM-to-ARM translator
- **Chosen:** a dynamic binary translator in the runner (`linux/runtime/lx_dbt*.c`):
  blocks are translated when they first run, ordinary ARMv4 instructions copied
  as they are, the rest rewritten. Programs without the PsiLinux note are
  translated; PsiLinux's own programs still run natively.
- **Alternatives:** rebuild Alpine's packages for ARMv4T soft-float (thousands of
  packages, and no `apk` from Alpine's servers); an interpreter (tens of times
  slower); patch the binaries ahead of time (data in the text segment looks
  like code, and v6/VFP needs code that does not fit in place).
- **Why:** 98% of Alpine's instructions are plain ARMv4 and run at full speed
  when copied; only executed code is translated, so data is never mistaken for
  code; and Alpine's signed packages install as they are.

## 25. Guest registers in host registers; guest addresses are host addresses
- **Chosen:** r0-r14 and the flags of the guest are the host's own while
  translated code runs. A guest's pc is a constant in the translation.
  Scratch space is the 24 bytes below the guest's sp (nothing writes there:
  signals are delivered by the runtime, not asynchronously).
- **Alternatives:** keep guest registers in memory (every instruction rewritten);
  steal a register for the runtime (every instruction using it rewritten).
- **Why:** it is what lets ordinary instructions be copied unchanged.

## 26. One translated thread runs guest code at a time ("the CPU")
- **Chosen:** `lx_cpu.cur`: the kernel hands the CPU over when the holder's call
  waits, when it ends, and after a 40 ms slice when others wait (at its next
  call or indirect branch: `lx_cpu.yield`).
- **Alternatives:** let EPOC schedule translated threads freely (as native ones).
- **Why:** the 5mx has one CPU, so nothing is lost; and it makes everything else
  simple: the runtime's state, the code cache and the swapped images (34) need
  no locks, the cache can be flushed whenever the translator runs (every other
  translated thread is in the kernel and comes back through the dispatcher),
  and LDREX/STREX need no monitor (27).

## 27. LDREX/STREX: a plain load, and a store that always succeeds
- **Chosen:** `ldrex` is `ldr`; `strex` is `str` and `mov rd, #0`; `clrex` and
  the barriers (`dmb`, `dsb`, `isb`, CP15 c7) are nothing.
- **Why:** with 26, no other translated thread runs between the two. This is
  consistent with the runner's SWP lock (native programs) because the two never
  share memory.

## 28. VFP on SoftFloat, the registers in the thread
- **Chosen:** every VFP instruction goes to the runtime (`lx_vfp.c`), which keeps
  s0-s31 (d0-d15) and the FPSCR in the thread and computes with Berkeley
  SoftFloat 3e (BSD; the subset needed, `linux/runtime/softfloat/`), the
  ARM-VFPv2 specialisation. Rounding modes and the cumulative flags work;
  traps, flush-to-zero, default NaN and short vectors do not (Linux armhf does
  not use them). VFPv3 instructions raise SIGILL, as on a VFPv2 CPU.
- **Alternatives:** GCC's soft-float library (its doubles have the old FPA word
  order in the EPOC SDK's GCC); my own float code (no).
- **Why:** bit-exact IEEE results, checked against a real VFPv2 (29).
- **Also:** the Q and GE flags are kept in the thread (the ARM710T's CPSR has
  neither): `mrs` and `msr` go to the runtime.

## 29. Testing the translator against a real ARMv6
- **Chosen:** `linux/tests/dbt/fuzz.py` writes programs of random instructions
  (data processing with every shifter form, multiplies, loads and stores in
  every addressing mode, ldrd/strd, ldm/stm, the v6 media and DSP
  instructions, exclusives, VFP arithmetic, conversions and compares with
  NaNs, infinities and subnormals in all four rounding modes, VFP loads and
  stores, conditional branches, calls, jump tables, pc as an operand); each
  case stores every register, the flags, the VFP registers and a data buffer.
  `run-fuzz.sh` runs each natively under qemu as an ARM1176 (ARMv6KZ + VFPv2)
  and translated under psihost as an ARMv4T (`qemu-arm -cpu ti925t`), and
  compares byte for byte. 30 seeds (9,000 cases) agree, and 12 more with every
  instruction class forced through the interpreter (`LX_DBT_SLOW=127`).
- **Why:** a translator is only as good as its rarest instruction; differential
  testing finds what reading the ARM ARM misses (it found a wrong operand in
  the `smlsldx` family, the Q and GE flags, and two encodings).

## 30. Unaligned loads and stores: fixed up in the thread's exception handler
- **Found (M0, the emulator with alignment faults as the ARM710T has them):**
  EPOC turns the CP15 alignment check on (0x27F). An unaligned word or halfword
  access in user code gives the thread's handler EExcAlignment. EUSER's
  exception trampoline leaves the faulting registers on the thread's stack,
  above User::HandleException's frame: `[r3..r12, lr, r0, r1, r2, pc]`, pc the
  faulting instruction, and returns through them. A handler that changes them
  and returns carries on after the instruction, any number of times (40 of 40
  in the probe). Two traps: EPOC does not keep the flags across a handler (the
  handler's own leak through), so the handler's entry (`lx_exc_entry`) saves
  the flags it was entered with and puts them back; and the ARM710T updates a
  written-back base before the abort, so the emulation works back from it.
- **Chosen:** translated loads and stores are copied as they are; an unaligned
  one faults, and the handler (`bk_epoc.cpp`, `dbt_fixup`) does it a byte at a
  time and goes on. It finds the frame from just above its own entry (a first
  version searched from its own sp and matched a stale copy, left by an
  earlier fault, in its own locals).
- **Alternatives:** check every load's alignment in translated code (2-4
  instructions each, and the flags); a kernel driver to emulate in
  the abort handler (ROM-specific, and a mistake takes the machine down).
- **Why:** aligned accesses (nearly all) cost nothing; an unaligned one costs a
  fault, about 50 µs. **To check on the real 5mx:** the probe (`LxProbe`,
  `D:\LXDBT.TXT`) runs the same tests there.
- **Also found:** the ARM710T cache is unified: 0 of 4,096 functions written and
  run at once ran stale code in the emulator (the probe repeats it on a 5mx).
- **Since:** a site that faults is patched so that it does not fault again
  (decision 40).

## 31. Into the runtime: one way in, the frame in the thread
- **Chosen:** a system call, an instruction for the interpreter, VFP, an exit not
  yet chained, a dispatcher miss and SIGILL all enter `dbt_enter` with a
  descriptor; it saves every register and the flags in the thread
  (`t->dframe`), moves to the thread's own EPOC stack, and the runtime returns
  the host address to go on at. Interpreted instructions go on in the same
  block; the others through the dispatcher.
- **Why:** one path to get right; the guest's stack is never used by the
  runtime; and a thread in the kernel holds no host address of the code cache,
  so the cache can be flushed (26).

## 32. One code cache for every process
- **Chosen:** 1 MB of address space, committed 64 KB at a time, flushed whole
  when full; blocks chained directly; a 4,096-entry hash for indirect branches
  (looked up by the dispatcher, copied into the cache so blocks can reach it,
  and the runtime keeps every block in its own hash too). When guest code goes
  (an image freed, a library unmapped) its range is queued, and the thread
  holding the CPU drops those blocks before it next looks one up.
- **Why:** processes that share code (34, and every fork child) share its
  translations.

## 33. HWCAP and AT_PLATFORM
- **Chosen:** translated programs get HWCAP_HALF, FAST_MULT, VFP, EDSP and TLS
  (no NEON, VFPv3 or Thumb), AT_PLATFORM "v6l", no AT_SYSINFO, and `uname -m`
  says armv6l.
- **Why:** musl's `__set_thread_area` reads the kuser helpers' page (absent on
  EPOC) unless HWCAP_TLS is set; with "v6l" it uses CP15 barriers (translated
  to nothing). OpenSSL probes for NEON with SIGILL, which works (24).

## 34. Shared images: text once, data each process's own
- **Chosen:** a translated program, its dynamic linker and its libraries are
  loaded once, at one address, for every process that uses the same file
  (`lx_simg.c`), and kept (with their translations) when unused while memory
  allows (8 kept; all dropped when free RAM runs short). Each process has its
  own copy of the writable part (data, bss, to the end of the page: about 10
  KB each for BusyBox and musl), kept in a store and swapped in when its
  process gets the CPU. A parked call of a process whose data is not in place
  is retried only when the data can be swapped in (the CPU free, or its holder
  in the kernel).
- **Alternatives:** each process its own copy at its own address (the first
  version: BusyBox and musl, 1.6 MB, for every command, and translated again
  each time); move the data elsewhere (code reaches it by fixed offsets).
- **Why:** on a 16 MB Psion with 9.5 MB free, init, a shell and a two-command
  pipeline did not fit, and short commands spent most of their time being
  translated. The swap is cheap because the writable parts are small, and safe
  because of 26.

## 35. fork for translated programs: the snapshot, nested
- **Chosen:** as for native programs (4), with children nesting (a subshell's
  pipeline): each child on a thread has a record, newest first, with its
  parent's registers, signal mask, VFP registers, thread pointer and
  snapshot. A child that faults ends alone: its parent goes on in a new
  thread.
- **Found:** the snapshot must reach the end of the data's last page: musl's
  dynamic linker gives that space to malloc (ash's job table was there).
- **Not yet:** a parent and child that both run on (a background subshell);
  see ALPINE.md.

## 36. Booting: BusyBox init as pid 1, OpenRC, mounts noted
- **Chosen:** in Alpine mode the first program is `/sbin/init` (BusyBox init),
  and it is pid 1 itself (it insists), reaping orphans as on Linux. OpenRC
  runs `sysinit` and `boot`. `mount` notes a mount (proc, sysfs, tmpfs,
  devpts...) and shows it in /proc/mounts and /proc/self/mountinfo, changing
  nothing: the kernel's file systems are fixed. `reboot`, `halt` and
  `poweroff` end Linux. A small service, `psilinux`, provides `dev` and `net`
  (the Psion's devices and network are EPOC's). `rc_sys="lxc"` keeps OpenRC's
  hardware services out; boot runs only `hostname` and `bootmisc`.
- **Why:** the boot is Alpine's own, with nothing for hardware to do.

## 37. Alpine's root: the minirootfs and Alpine's packages, built in docker
- **Chosen:** `linux/alpine/build-root.sh` unpacks Alpine 3.24.2's armhf
  minirootfs (pinned by checksum) and adds OpenRC with Alpine's own apk, run
  in an x86 alpine container with `--arch armhf` (the packages' signatures
  checked against Alpine's keys; their scripts, being ARM code, not run: what
  they do is done by the script). PsiLinux's files (`linux/alpine/overlay`)
  go over it, and mkroot.py packs it. It goes in `\Linux\Alpine\`, beside
  (not instead of) PsiLinux's own root.
- **Why:** the image is exactly Alpine's, so its `apk` database is right and
  later packages install over it.

## 38. Start modes
- **Chosen:** Tools > Preferences > Start: "Alpine Linux" (boots: BusyBox init,
  OpenRC, then a login on the window), "Alpine Linux, shell only" (straight to
  `sh -l`), "PsiLinux" (the native root, as before). About shows the Alpine
  release the runner read from /etc/alpine-release.

## 39. smla, smul and clz inline; smla's Q flag not set
- **Chosen:** `clz` becomes a flagless ARMv4 sequence (smear, then count the
  bits), and `smlaxy`/`smulxy` (16x16 multiplies) become `mov`/`mul`/`mla`
  inline. The inline `smla` does not set the Q flag when its addition
  overflows: Q is kept in the thread (`dbt_q`) and only the interpreter sets
  it. Everything else in the 0111 media group goes to the interpreter.
- **Why:** these are in compilers' hot loops (memchr, hashing, codecs), and
  the interpreter costs about 100 times as much. Q is sticky and read only by
  `mrs`; nothing in Alpine's main packages reads it after an `smla`. The
  fuzzer's smla cases compare registers, not Q.
- **Risk:** a program that tests Q after `smla` overflow (DSP code) would see
  it clear. LX_DBT_SLOW=32 interprets every multiply if it matters.

## 40. Unaligned sites patched after their first fault
- **Found (the emulator with the ARM710T's alignment faults):** an OpenRC
  boot took about 36,000 alignment faults, nearly all at two instructions:
  musl's `memset` stores halfwords at odd addresses (`strh r12, [r0, #1]`),
  which ARMv6 allows. Each fault is an EPOC exception (a kernel round trip).
- **Chosen:** when the handler has fixed an access (decision 30), it patches
  the faulting host instruction into a branch to a small stub, built at the
  end of the code cache: the condition, then into the runtime (`DBT_FIXUP`),
  which does the access a byte at a time and comes back after it. So each
  site faults once; a flush of the cache forgets the patches with the code.
  The same boot then takes 2 faults. `LX_DBT_SLOW=128` sends every word and
  halfword access through `DBT_FIXUP` on the host harness (qemu never faults
  on alignment): the fuzzer (12 seeds) and the shell tests (46 of 46, 3.7
  million accesses through it) agree.
- **Alternatives:** check alignment inline before every load and store (2-4
  instructions each, everywhere); interpret the instruction (`DBT_HELPER`,
  needs the guest pc, which a host address does not give).
- **Why:** pay only where it happens, once.

## 41. apk: the catalogue on the server, Alpine's packages on the Psion
- **Found:** Alpine's own apk with the full main and community index needs
  29.6 MB (the host harness; 51 MB on x86-64). A 5mx has about 9 MB free.
- **Chosen:** `psiapk` (linux/alpine/server/psiapk.py, a PsiProxy hook at
  `/_apk/`) holds the catalogue (25,135 packages), answers `search` and
  `info`, and resolves an install against what the Psion has (it sends its
  installed list). The Psion downloads Alpine's own signed `.apk` files
  (through the proxy, or straight from the mirror with APK_DIRECT=1) and the
  real apk installs them from the files (`--no-network`, no repositories), so
  the signatures are checked against `/etc/apk/keys` as always. The wrapper
  (`/usr/local/sbin/apk`) has the usual verbs and says, before anything is
  downloaded, the download and disk sizes, whether it fits on the disk, and
  whether the programs and libraries are likely to fit in memory.
- **Alternatives:** a trimmed index on the Psion (still several MB, and stale);
  apk's own `--repository` pointing at a server-made small index (the
  signatures then are ours, not Alpine's).
- **Why:** the packages and their signatures are Alpine's; only the search is
  done elsewhere.
- **Also:** the real apk's `add` itself needs about 7 MB while it works; the
  wrapper warns when less is free.

## 42. PsiProxy sends large answers to slow clients in full
- **Found:** big downloads stopped at about 290 KB ("connection closed
  prematurely"). The proxy's sockets are non-blocking and `sendall()` gave up
  silently when the send buffer filled, which a slow client (a Psion) always
  does.
- **Chosen:** `SocketTransport.send` (psiproxy_core.py, shared by PsiWeb and
  PsiMail's web pictures too) waits for room with `select`, up to 120 s without
  progress.

## 43. Up to 24 shared images in a process, then private copies
- **Found:** curl loads 13 libraries; a process could share 12 images (and
  the kernel keep 16), and the 13th failed with "Out of memory".
- **Chosen:** 24 a process, 32 in all; past that a library is loaded as the
  process's own copy (as before decision 34) instead of failing.

## 44. The code cache's memory, and stacks
- **Chosen:** 1 MB of address space for the code cache, committed 64 KB at a
  time; at a flush (when the cache is full) it gives back all but 64 KB if
  less than 2 MB of RAM is free. A translated program's main stack is 128 KB
  (committed RAM).
- **Why:** Alpine's busiest programs (apk, OpenRC's scripts) translate about
  600 KB of code; most need far less. Stacks: musl's default thread stack is
  128 KB and ash's recursion needs more than PsiLinux's own 64 KB.

## 45. A test build: 0.19
- **Chosen:** before 0.20, a test build for a real 5mx: PsiLinux 0.19
  (dist/PsiLinux-0.19-test.sis), the Alpine root (dist/PsiLinux-alpine-root.sis,
  UID 0x01000A8B: 0x01000A89 and 0x01000A8A are reserved) and the probe
  (dist/LxProbe-test.sis, never a release). The test sheet is
  linux/docs/TEST-5MX.md. 0.20 installs over 0.19.

## 46. Alpine mode logs where it got to
- **Chosen:** in Alpine mode the runner's log (`\Linux\Alpine\PsiLinux.log`,
  flushed line by line) has every 30 s a line with free memory and the
  translator's figures (blocks, flushes, system calls, alignment faults and
  patched sites). With a file `BOOTLOG` in `\Linux\Alpine\` (and always in
  the 0.19 test build) it also has a line for each program started, with the
  time since Linux started.
- **Why:** a boot that stops on a real 5mx then says where, and when; the
  numbers are the ones the emulator cannot give. The per-program lines are
  about 190 small writes to the card in a boot, which the emulator's card
  does not always survive, so they are not on by default.

## 47. No Alpine root: PsiLinux starts instead, and says so
- **Chosen:** when Start is an Alpine mode and `\Linux\Alpine\ROOT.IMG` is not
  on the Linux disk, the window says so (and which installer to use) and
  starts PsiLinux's own programs.
- **Why:** someone who updates PsiLinux without the Alpine root still gets a
  working Linux, and is told why it is not Alpine.

## 48. Emulator: the CF accelerator and translated code
- **Found:** in the emulator, Alpine boots sometimes restarted the machine: a
  kernel fault walking the timer queue, because one timer was queued twice.
  The timer is the CF card driver's, and the emulator's own accelerator
  (`CF direct-invoke`, which calls the driver's callback from whatever the CPU
  was doing) queued it while the kernel did too. Translated code keeps the CPU
  in user mode almost all the time, which is when the accelerator fires.
- **Chosen:** tests in the alignment-faithful emulator copy allow the
  accelerator only in the idle context (`PSION_CF_INVOKE_IDLE_ONLY`, a
  private change to that copy). That made the fault rarer (1 boot in 4) but
  not gone: the callback can re-arm a timer that is still queued, whatever
  the mode. Without the accelerator (`PSION_NO_CF_DIRECT_INVOKE=1`) the card
  is too slow for PsiLinux to start at all. A run that ends in this fault is
  rerun. The real 5mx has no accelerator.
- **Also seen:** whether a boot meets the fault depends on the card's
  timing. Runs that logged every program (BOOTLOG) booted 3 times out of 3
  with the standard image. A benchmark image with extra files stalled at
  the same early step each time (the card stopped answering). The speed
  figures in ALPINE.md were therefore taken in native mode, which reads
  the card far less, running Alpine's BusyBox through Alpine's own dynamic
  linker.
- **Also:** the same fault, at the same address, was already seen with native
  PsiLinux under long card reads (README, "Testing"; decision 21). The tick
  queue dump now shows why: it is the emulator's, not the runner's.

## 49. bash is the shell in Alpine mode
- **Chosen:** Alpine's bash (with readline and ncurses) is in the Alpine root,
  root's login shell in `/etc/passwd`, and the shell of "Alpine Linux, shell
  only": the app starts `/usr/sbin/psilinux-shell`, which runs `bash -l`
  when `/bin/bash -c :` works, else BusyBox `sh -l`. In a full boot the
  `psilinux` service makes the same test and gives root `/bin/sh` while bash
  does not start, and bash again once it does. With an Alpine root from
  before 0.21 (no psilinux-shell) the runner starts `/bin/sh -l`. Settings:
  `/etc/bash/psilinux.sh` (prompt `psion:~#`, 200 lines of history,
  `ll`/`la`/`l`), `~/.bashrc` for the person's own; no bash-completion (1 MB
  of scripts, seconds to load on a 5mx; names still complete with Tab).
- **Found:** interactive bash hung at the first external command. With job
  control, the first process of a job reads a pipe (`pgrp_pipe`) until the
  shell has started the whole job and closed it. Here the child of a fork
  runs first, until it execs or exits, so the shell could not run to close
  it. **Fix** (`lx_dev.c`): a read of an empty pipe whose only writers are
  parents waiting on the reader's fork returns end of file, as the wait
  could never end. Also: `/dev/fd/N` of a pipe opened the link's text
  ("pipe:[4001]"); opening it now gives that pipe, so `<(...)` works.
- **Checked** (host harness, `tests/run-alpine.sh`): the 46 M2 shell tests
  under BusyBox sh and under bash, 39 bash tests (arrays, `[[ ]]`, `=~`,
  here-strings, `mapfile`, `<(...)`, `PIPESTATUS`...), and an interactive
  session in a pseudo-terminal: pipes, `$(...)`, Ctrl+C, Tab, history, `&`,
  `jobs`, `kill %1`, Ctrl+Z and `fg`, `read`. bash takes about 2.1 MB while
  it runs (its image 1.5 MB, readline and ncurses 0.6 MB), about 0.5 MB more
  than BusyBox sh.
- **Cost, in the emulator:** after a full boot to bash, 5.2 MB free and
  2.2 MB in caches that give way (programs and unused libraries): about
  7.4 MB for programs, against 8.3 MB free with BusyBox sh in 0.20. bash's
  code also fills the 1 MB code cache (it was 0.6 MB). Shell only: 5.9 MB
  free and 2.0 MB cached. Worth it for the shell people expect; anyone who
  needs the memory back can `apk del bash` (the fallback gives sh).
- **Seen once** in three emulator boots: bash crashed (`signal 11 at 0`)
  when the `psilinux` service tried it, and the fallback gave root sh for
  that session. Not seen on the host harness (80 runs); to watch on a 5mx.

## 50. What the Alpine root has preinstalled
- **Chosen:** the tools bash, nano, less, mandoc (`man`), htop, ncdu, tree,
  jq, bc and openssh-client (ssh, scp); for fun fortune, figlet, sl,
  cmatrix, micro-tetris (`tetris`), bsd-games (worm, snake, robots,
  adventure...), cbonsai, nyancat and fastfetch; and the small manual pages
  of those (not bash's: 2.9 MB; `help` has it). Installed by Alpine's own
  apk into the image, so apk's database lists them and `apk del`/`apk
  upgrade` work on them. nano is `EDITOR` and `VISUAL`.
- **The rule:** a program is preinstalled if it runs under the translator,
  draws correctly, and fits in memory beside bash with Alpine running: the
  largest (ssh, 5.4 MB) leaves about 2 MB in a full boot. The table is in
  ALPINE.md.
- **Not preinstalled:** vim (37 MB while editing), links (7.6 MB), curl
  (9.5 MB), git (6.6 MB, and 13 MB more for `git clone https://`), file
  (its 10 MB magic database is read whole), frotz (its sound libraries:
  9.1 MB), nethack (4 MB: installable), asciiquarium (perl, 32 MB of disk),
  gnuchess (too slow to think), tmux (needs Unix sockets and a server
  running beside the client), rsync (its local copy needs two processes
  running at once). Not in Alpine 3.24 for armhf: cowsay, toilet, lolcat,
  nsnake, bastet, moon-buggy, ninvaders, greed, 2048, neofetch.
- **A new image over an old one's changes:** `\Linux\Alpine\root\` keeps
  apk's database if packages were added, and it would then not list what the
  new image brings. The image carries its database as built
  (`/usr/lib/psilinux/apk-installed`); `psilinux-merge-db` (run at boot and
  by psilinux-shell, once per image version) adds the missing packages.
- **The installer's version** of the Alpine root is 3.25 (it was 3.24): EPOC
  compares only major.minor, and it must be higher to install over 0.20's.
  Alpine is still 3.24.2.

## 51. Keep memory for other programs (the reserve)
- **Chosen:** Preferences > General: "Keep memory for other programs" (off
  as standard) and how much (512 KB, 1 MB or 2 MB; 1 MB at first). The
  setting takes two spare fields of TLxSettings, so older PsiLinux.ini files
  load (off). The app passes it in the shared chunk (`reserve`, version 3),
  before the runner starts and whenever Preferences change.
- **How:** every growth that takes RAM from EPOC goes through
  `lx_reserve_ok`: images and libraries, stacks, a fork's saved memory, the
  arena (mmap and brk), the code cache and threads. A growth that would
  leave less free than the reserve fails with ENOMEM, so the program sees
  an ordinary "out of memory"; the kernel's own small allocations are not
  checked, so the runner keeps working. The first refusal in a run is
  logged; `/proc/psilinux/mem` shows free RAM, the reserve, what is
  available to Linux and the refusals. `/proc/meminfo`'s MemAvailable (the
  "available" of `free`) is free less the reserve; the program cache and the
  code cache give memory back by the same figure.
- **Checked:** on the host harness with a pretend Psion (`LX_SIM_FREE`,
  `LX_RESERVE`): a greedy awk is refused, the refusal is logged once, and
  Linux goes on.

## 52. The package server: no address built in
- **Chosen:** apk's server is `APK_SERVER` (the environment, then
  `/etc/psilinux/apk.conf`, shipped empty), else the proxy in the shared
  Connection settings (`C:\System\Data\PsiLink.ini`, read through `/c`) when
  it is on and is not the Atom modem's own (192.168.7.1 or "psiproxy"),
  then `C:\System\Data\PsiDevBox.ini`, then the Atom's own proxy on the
  PPP link (192.168.7.1, asked with "Host: psiproxy"), which passes `/_apk/`
  on to the PsiProxy set in the modem (AT$PU); its 501, or an older Atom's
  HTML status page, is "Needs PsiProxy on a computer". With none, apk says
  "No package server set - set APK_SERVER in /etc/psilinux/apk.conf".
- **Why:** the image is published; a LAN address in it would be wrong for
  everyone else. The owner's Psion already has its PsiProxy in Connection
  settings, so it needs no change.

## 53. Published images: licences, sources, no build paths
- **Chosen:** the Alpine root has `/usr/share/licenses/SOURCES.txt` (every
  package with its version, aport and licence, and the written offer) and
  the GPL-2.0, GPL-3.0 and LGPL-2.1 texts. PsiLinux-root's SOURCES.txt
  lists git and nano and points at github.com/danieledge/psilinux. The ports
  build against `/tmp/psilinux-sysroot` and in `/tmp/psilinux-build`
  (symbolic links in the build container) with `-ffile-prefix-map`, so no
  build folder is in a program (git and jq keep their build flags; SQLite
  its file names). Both images are checked for "172.24." and "/home/".

## 54. Root images in parts, and in several .sis files (0.22)
- **Found:** on the owner's 5mx, 0.21's `PsiLinux-alpine-root.sis` (25.9 MB,
  one 25.3 MB ROOT.IMG) failed with "Not enough memory"; 0.20's 7.9 MB one
  installed. ER5's .sis files hold their files uncompressed (the records
  checked: an EPOC program starts with its own header, not zlib's), so
  compression is not the cause. In the emulator (about 8.7 MB free) the
  installer opened a 6.3 MB .sis in 86 s, an 8 MB one in 111 s and a
  12.9 MB one (one file) in 176 s, but never a 25.9 MB one in 1,000 s,
  **whether that .sis held one 25 MB file or 25 files of 1 MB**. So what
  matters is the size of the whole .sis as the installer opens it, not of
  the files in it: cutting the image into files inside one .sis is not
  enough. (The limit is somewhere between 13 and 26 MB; 0.20's 7.9 MB
  .sis is known to install on a 5mx.)
- **Chosen:**
  - mkroot.py `--parts 1024` cuts an image into 1 MB parts: ROOT.IMG (the
    header, now with the part size and count at 32, the index, and the
    first data), then ROOT.I01, ROOT.I02... The runner (`lx_root.c`
    `img_read`) reads across them, opening parts as needed and keeping 3
    open besides ROOT.IMG. An image with no part count (0.20, 0.21) reads
    as one file, as before. A missing part stops the start with "The root
    image ... is not complete: ROOT.Inn is missing".
  - The parts go in .sis files of at most 6 MB (`tools/sisparts.py`):
    `PsiLinux-alpine-root.sis` (ROOT.IMG and the first parts, UID
    0x01000A8B) and `-2` to `-5` (UIDs 0x01000A8C to 0x01000A8F, each
    needing the first); `PsiLinux-root.sis` and `-2` (0x01000A88,
    0x01000A90). 6 MB is below what a 5mx is known to take (0.20's 7.9 MB).
  - Installed over 0.20 or 0.21 (the same UID, a newer version), the
    installer removes the old single ROOT.IMG with the old version, and
    ROOT.IMG is then the new first part: nothing is left behind.
    `\Linux\Alpine\root\` is not the installer's, so it is kept.
- **Alternatives:** one .sis of many small files (fails as above); a
  download-and-unpack step in PsiLinux (needs the network, and the space
  twice).
- **Tested:** host harness: all 500 files of the Alpine image read through
  the 25 parts match the build byte for byte; the suites pass on split
  images; a 0.20 single image still reads. Emulator: the installer put
  part 1 (6.3 MB) over an old single ROOT.IMG (now the 1 MB first part,
  with ROOT.I01 to I05) and kept `root\`; part 3 alone was refused with
  "Missing dependency" (the requisite works); with all 25 parts on the card
  "shell only" started bash (bash's checksum right); with ROOT.I12 taken
  away, the window said which part was missing. The emulator's card stalls
  on long writes, so a whole five-part install did not finish in one run.
- **Also found:** while PsiLinux runs, its root image is open and the
  installer stops with "In use": close PsiLinux first. In the emulator the
  installer keeps its records on C:.

## 55. ssh's settings for a Psion: no sntrup761 (0.23)
- **Found:** "ssh seems to hang" on the owner's 5mx. On the host harness
  (qemu as an ARMv4T, every instruction of the kernel, the translator and
  ssh counted from qemu's log) a key login is 167 million instructions with
  ML-KEM (OpenSSH 10 servers), but **1,470 million with
  sntrup761x25519-sha512**, OpenSSH 10's second choice and what every
  OpenSSH 8.5 to 9.8 server (most in 2026) picks: about two minutes at the
  5mx's 13 million translated instructions a second (the shell loop of
  ALPINE.md's Speed table, 8.8 million instructions in 2.3 s in the
  emulator, and the emulator 3 times slower than a 5mx). The test server
  dropped the run under qemu's logging at its 2-minute login limit, as a
  real server would. No system call was at fault: key and password logins,
  the host key question on /dev/tty, raw mode, and poll on the socket and
  the terminal all worked on the harness.
- **Chosen:** `/etc/ssh/ssh_config.d/psilinux.conf`: KexAlgorithms ML-KEM,
  curve25519 (141 million), ECDH P-256, DH group 14; no sntrup761, no DH
  16/18 or group exchange (big modular powers); `WarnWeakCrypto no-pq-kex`
  (else a warning at every login to an OpenSSH 9 server, caused by the line
  before); `ObscureKeystrokeTiming no` (its 20 ms chaff tripled the packets
  while typing); `UpdateHostKeys no` (it rewrites known_hosts with link(),
  which PsiLinux's disks refuse); `ServerAliveInterval 60`. Ciphers and
  host key types stay OpenSSH's (within 2% for a login).
- **Alternatives:** keep sntrup761 for its post-quantum protection (two
  minutes a login, and failures at servers' time limits); P-256 before
  curve25519 (2 s quicker, but against the usual preference).
- **Also measured:** ssh's start alone (OpenSSL's loading and relocation)
  is 54 million; an RSA 3072 user key adds 195 million (ed25519 is
  recommended); a second login costs as much as the first. With
  `LX_DBT_SLOW=128` the harness now also counts unaligned accesses as an
  ARM710T would meet them (a site's first unaligned access faults and
  patches it, later ones go through the runtime): 61 sites and 35,000
  accesses in a login, under a second.

## 56. The DNS answerer keeps every question (0.23)
- **Found:** musl sends the A and AAAA questions for a name together, on one
  socket. The answerer kept one question per socket: the AAAA (answered at
  once, empty) replaced the A, whose lookup was cancelled, and musl asked
  again after 2.5 s. Every `ssh name` waited 2.5 s more; with a lookup over
  a slow link taking longer than musl's remaining 2.5 s, the name failed.
- **Chosen:** up to 4 questions a socket, each with its own lookup and
  answer, read in the order asked; the same question again (musl's retry)
  joins the lookup already running.

## 57. Random numbers: ChaCha20, fed with events and a saved seed (0.23)
- **Found:** getrandom, /dev/urandom and AT_RANDOM were EPOC's Math::Rand
  (a linear congruential generator) seeded with the start time, so an
  observer could work out ssh's session keys.
- **Chosen:** `runtime/lx_rand.c`: ChaCha20 with fast key erasure. Folded
  into the key before a request: the clocks, Math::Rand and the free memory
  at the first; the time of every key typed and every network or timer
  event; and whatever is written to /dev/urandom, as on Linux.
  `/usr/sbin/psilinux-seed` (run by psilinux-shell and the psilinux service)
  writes the seed saved last time back in and saves a new one
  (`/var/lib/psilinux/random-seed`), so the pool carries over between
  sessions. It never blocks.
- **Limits:** a 5mx has no hardware source and a 1/64 s clock; the first
  session after an install has little more than the clock and the key
  timings. A fresh seed can be given with `cat FILE > /dev/urandom`.

## 58. PsiTerm's colour cells, and an object of PsiLinux's own (0.27)
- **Found:** PsiLinux 0.26 corrupted its heap once its history held 200
  lines: in the emulators (5mx and netBook) `seq 1 400` ended the program
  with KERN-EXEC 3. (Dan saw "Main USER 47" on the netBook when starting
  Linux: see the runner, below.) The SDK keeps
  the objects of every `ptproj` program in one folder, and PsiLinux compiled
  PsiTerm's `termsb.cpp` to the same `termsb.o` as PsiTerm's own build. 0.26
  was compiled against PsiTerm before 0.99 (4-byte history cells), but make
  kept the `termsb.o` that PsiTerm 0.99 had just built (6-byte cells, newer
  than the old source). Relinking 0.26's objects with that `termsb.o` gives
  the published `PsiLinux.app` byte for byte (only the header's time
  differs). So `SbPush` wrote the history with a 6-byte stride into a buffer
  of 300 x 128 4-byte cells: from line 200 on, past its end, over the heap
  cells after it (the line lengths, then the app's session with the runner).
- **Chosen:** `app/lxtermsb.cpp` compiles PsiTerm's `termsb.cpp` under a
  name of its own (as `lxglyphs.cpp` does for `glyphs.cpp`), and PsiLinux
  uses PsiTerm 0.99's cells; `build.sh` refuses an older PsiTerm, and checks
  the app for BX as it does the runner.
- **Colour (PsiTerm 0.99's terminal):** the window takes the screen's mode
  (256 colours on the netBook and Series 7, 16 greys on the 5mx, as before).
  On a colour screen cells keep xterm colours 0..255 (24-bit colour as the
  nearest of them), or the theme's own text and paper; the history keeps
  them too (`KSbColour`, `KSbDefFg`, `KSbDefBg`). The four themes are
  PsiTerm's first four; coloured text too close to its paper is made lighter
  or darker; the cursor is drawn in reverse; the status line is navy. A
  colour screen starts in Light on dark (no saved settings). The code is
  PsiTerm's, kept as close to it as possible; psion-core has no terminal
  code to share, so it is not moved there.
- **The runner:** `bk_alloc` refuses a size of 1 GB or more (EUSER would
  panic the kernel thread, USER 47) and logs it with the caller's address.
  The netBook's "Main USER 47" names a thread called Main, which is what an
  EXE's thread is called (EIKON renames an app's thread to the app's name
  before the app runs; the panic dialog shows the thread's name), so it
  may have been psilxrun.exe's kernel thread rather than the app. No size
  that large was found in the kernel's allocations, and the netBook
  emulator (v1.05(450), with Alpine in the ROM, as the card cannot be
  mounted there) boots Alpine with both 0.26 and 0.27. If it is seen again
  with 0.27, PsiLinux.log (Tools > Keep a log) has the line.

## 59. The kernel thread's own faults, a lost invalidation, PsiTerm's terminal as it is (0.29)
- **Found (the netBook's "Main KERN-EXEC 3"):** Dan's PsiLinux.log of 0.28
  ends in the middle of an Alpine boot with no fault line: "Main" is the
  runner's kernel thread, which had no exception handler, so EPOC ended
  the runner and nothing was written. In the 5mx emulator, once in about
  ten starts with the log on from the start, the same dialog came, or the
  start stopped (the emulator's card is known to stall under writes, so
  that is not proof of a bug of PsiLinux's).
- **Chosen:**
  - The kernel thread has `lx_exc_entry` as well. `KernelFault` logs the
    exception, pc, the instruction and the address it used, lr, sp,
    r0-r12, what the kernel was doing (a call, the timer, a thread's
    death), the last 16 calls (tid, pid, number, first argument), the
    translator's state, how much of its stack it has used and 32 words of
    the stack; it opens the log even without Keep a log, tells the app
    ("The Linux runner failed: exception N at pc...") and waits for the app
    to end the runner, so there is no "Program closed" dialog.
    `E32Main`'s address is logged with it, `build.sh sis` copies the link
    map to `dist/psilxrun.map`, and `tools/lxpc.py PsiLinux.log` names the
    functions. `<root>\KFAULT` makes the kernel thread fault at 20 s, to
    test it (emulator: the report came, and `lxpc.py` named
    `CLxTerm::Housekeeping`).
  - How deep the kernel thread's stack has gone is sampled where it is
    deepest (file reads, allocations, the log) and given in the 30 s lines
    and the last line (about 17 KB of 64 KB for PsiLinux's shell in the
    emulator; GCC 3.0's largest frames are the path functions, 2 to 4 KB
    each). (Painting the stack below `E32Main` was tried first: it wrote
    past the stack's end, and one start in three ended at once.)
  - The translator's invalidation queue had one count changed by both the
    kernel thread (adding the ranges of programs that end or unmap code)
    and the thread holding the CPU (setting it back to 0 when done). A
    range added while the reader worked was lost, and the old
    translations of that range then ran for the next program loaded at the
    same address: the kind of fault that comes and goes. Now a ring with a
    writer index and a reader index, each moved by one side only.
  - LxSync1.ldd cleans every range line by line. 0.28 cleaned a range over
    16 KB (every program it loads natively) by reading 16 KB of the ROM
    through the data cache, which only works if the ROM is mapped cacheable
    and outside the minicache.
  - `bk_log` from a Linux thread (the translator's runtime runs on them)
    used the kernel thread's file server session, which EPOC refuses other
    threads (the thread would panic); such lines now wait in a buffer the
    kernel thread writes.
  - The kernel's clock counted system ticks as 1/64 s each, as on the 5mx;
    it now takes the tick's length from `UserHal::TickPeriod` (logged on
    the first line), so sleeps, timeouts and the translator's slice are
    right on a machine whose tick differs.
  - A faulted kernel thread waits on a semaphore at the lowest priority
    for the app to end the runner. (Waiting with `User::WaitForAnyRequest`
    in a loop can spin: requests completed earlier make it return at once.)
  - In the emulator, a start with the log written from its first line
    sometimes stops in the middle of a write and the whole machine stands
    still (0.28 did the same without a log, one start in five): the
    emulator's card is known to stall under writes (21, 48). A report the
    fault test wrote in full in one run stopped half way in another.
  - Audited and found right: every code write is followed by
    `bk_code_sync` (the dispatcher and its veneer at the start, blocks,
    their exits and literals, chains, the unaligned-site stubs and patches,
    native programs as they load); a flush is followed by fresh blocks
    only, and a chain is patched only if no flush came between (`gen`);
    shared images and fork move data only; translated code is run only by
    the thread holding the CPU.
- **Typing (Dan: "slow on the netBook"):** PsiTerm's drawing benchmark
  (Bench.txt beside the program; added to a build of 0.28 to compare) in
  the 5mx emulator, 95 x 18 cells: 0.28 and 0.29 alike, 2.4 ms a key for
  an echo drawn, 32 ms a line scrolled, 290 ms a whole redraw (the
  emulator is about 3 times slower than a 5mx). So the drawing is not the
  delay; the time goes in Linux. Now:
  - A key that wakes the program reading the terminal no longer waits for
    a busy translated program's 40 ms slice and the queue behind it: while
    typed input is handed in, the thread it wakes goes to the front and the
    holder gives way at its next call or indirect branch (`lx_k_boost`).
  - The log's 30 s line says how long Linux took to answer keys (average,
    most, how many over 100 ms), and with the log on the status line shows
    the echo time as the app sees it (key to output drawn).
  - `<root>\LOGON` turns the runner's log on without the app's setting
    (for the emulator, whose C: cannot be prepared).
- **The terminal is PsiTerm's** (Dan: "adopt PsiTerm behind the scenes"):
  [PTVIEW.md](PTVIEW.md). `tools/ptview.py` takes PsiTerm's `CTermView`
  terminal functions and their constants out of `psiterm.cpp` at every
  build, compiled against PsiTerm's own `psiterm.h`; `lxterm.cpp` gives
  the parts that are PsiLinux's. PsiTerm's typefaces (the `PsiTerm.gdr` of
  PsiTerm's own psion-core), zoom levels, themes and colour schemes come
  with it; Preferences gains Typeface, and a colour screen starts in
  PsiTerm's Clean. PTVIEW.md describes the change PsiTerm could make so
  that this needs no extraction.

## 60. Your Psion from the shell: /psion, /dev/epoc and the epoc command (0.36)
- **The disks under /psion.** /psion/c, /psion/d... and /psion/z are the same
  mounts as /c and /d (kept, for scripts that use them), under one folder,
  with /psion/bin (the runner's own `bin\` folder, read-only: the epoc
  command). On every Psion disk: EPOC's names (the file server is
  case-blind, so `Letter` and `LETTER` are one file); the w bits are the
  read-only attribute, and `chmod a-w` / `u+w` set and clear it; Z: is
  read-only (EROFS); and so is every disk's `\System` folder, so that a
  stray `rm -rf` or `cp` cannot break the programs EPOC runs.
  `epoc syswrite on --force` lifts that until `off` or the next start.
  EROFS rather than EACCES: Linux's root expects to pass permission
  checks, and "Read-only file system" says what is going on. No execute
  bits from attributes: EPOC has none, and every file stays runnable, as on
  WSL's /mnt/c.
- **Found on the way:** `mv letter Letter` on a Psion disk found the target
  "there", deleted it (the same file) and then had nothing to rename. A
  rename to the same file now only renames (`bk_same_file`; the runner
  goes by way of a temporary name if the file server takes a case change
  for a clash).
- **The request path.** A new device, `/dev/epoc` (`runtime/lx_epoc.c`):
  one write is a request, one read its answer, in a versioned format
  (`common/lxepoc.h`, LXE_VERSION 1: a 28-byte header, then NUL-ended UTF-8
  strings; answers are tab-separated lines). The kernel checks it, turns
  Linux paths into EPOC names (only for files on a Psion disk or in the
  root's writable layer, which is a folder on one), answers HELLO,
  SYSWRITE and PATH itself, and queues the rest, one at a time, to the app
  through new fields of the shared chunk (LX_SHARED_VERSION 6) and the
  doorbells it already has. The read parks, as a terminal read does; a
  signal interrupts it, and an answer nobody waits for is dropped.
  Alternatives: a new system call (no: Linux programs and the host harness
  would need it in their libc), files under /proc (no: one request with
  its answer, and the arguments of open, are clumsy as files).
- **The app does the work, not the runner.** PsiLinux.app already has the
  window server, apparc, EIKON's infoprints and a CCoeEnv (Coda's
  Screenshot.ctl needs one); the runner's kernel thread must never block
  on a server, and stays without those libraries. Messages (`alert`,
  `confirm`) are RNotifier's, asynchronous, so PsiLinux's window goes on
  drawing while one is up.
- **Who may.** Only a process whose controlling terminal is the PsiLinux
  window (the console ttys): not a pseudo-terminal (an ssh login, tmux)
  and not a process with none (a daemon). Nothing listens on the network.
  `kill` never ends EPOC itself (EKern, EFile, EWSrv, FbServ, the Shell,
  C32, apparc's and EIKON's servers) or PsiLinux, and ends another system
  or protected process only with `--force`. `close` asks politely
  (TApaTask::EndTask), so the program saves.
- **The epoc command** (`epoc/epoc.c`) is a static Linux program shipped in
  PsiLinux.sis beside the runner (`\System\Apps\PsiLinux\bin\epoc`), so it
  is always the runner's own version, and is /psion/bin/epoc in Linux; the
  roots have /usr/bin/epoc, xdg-open and open as links to it. For scripts:
  tab-separated lists, `-H` for a heading, `--json`, nothing printed on
  success, errors on stderr, exit codes 0 to 9 (`epoc help exit`), and
  waits (`run --wait`, `open --wait`, `wait`, `wait-file`,
  `wait-battery`). (0.50, epoc 0.38) `run` and `open` return once the
  program has started (its window group on the task list, in front unless
  `--bg`), not when EPOC has been asked, so `time epoc run Calc` is the
  start; the command polls the task list every 0.2 s (no change to the
  app or the protocol); `--no-wait` is the old way. `print` says EPOC has no "print this file" (each
  program prints its own documents) and exits 5.
- **Exit closes PsiLinux.** `exit` (or ^D) at the first shell used to leave
  "Linux has ended. Enter starts it again". Now the runner asks the
  programs left to end (SIGHUP, SIGTERM and SIGCONT), gives them 2 s, ends
  the rest, closes every file and socket (each RFile flushed), and only
  then says it has ended; the app then closes. A shell killed by a signal,
  or a failure, still stays on the screen to be read. hush (patched,
  `rootfs/patches/busybox-hush-checkjobs.diff`) and bash (`checkjobs`)
  warn "There are running jobs." first; a second exit leaves. In Alpine's
  full boot, `/usr/sbin/psilinux-login` logs root in and powers off when
  that shell ends, so OpenRC stops its services first.

## 61. apk with no PsiProxy: Alpine's catalogue as small static files (0.37)
- **Found:** decision 41's helper needs a PC running PsiProxy, so a Psion
  on its own could not install anything. Alpine's real apk needs about
  30 MB for the full index (29.7 MB of mappings on the host harness; with
  a pretend Psion it needs 36 MB free, and fails at 32 MB).
- **Chosen:** `tools/apkcat/mkapkcat.py` turns Alpine's APKINDEX into about
  39,000 files of at most 8 KB: one per package, with each dependency
  already matched to the package that provides it; what provides each
  name, in 512 buckets; every version, in as few shards as fit (48); the
  names by each three-letter group, and description words by their first
  three letters, for search. `.github/workflows/apkcat.yml` remakes it
  every day and publishes it to the gh-pages branch (GitHub Pages) only when
  the index or the generator changed. The Psion's wrapper reads only the
  files it needs (`apk add git`: 13 records; a search: one or two files),
  keeps them until the catalogue's index changes, and works out the plan
  itself in BusyBox awk (under 200 KB): what is installed is read from
  apk's own database, versions are compared by Alpine's rules (the same as
  psiapk's, checked on 3,227 pairs). The plan is psiapk's for every package
  tested (`tools/apkcat/test/run.sh`). The packages still come from Alpine's
  mirror and the real apk still checks their signatures.
- **Order:** the source chosen in apk.conf (`apk source`), else a PsiProxy
  in Connection settings (as before), else the online catalogue.
- **https:** BusyBox wget's https (its ssl_client helper over a
  socketpair) gives zeros in place of the data in long answers under
  PsiLinux 0.36 (the second 8 KB of every 16 KB past the first 8 KB, the
  same each time; ssl_client writing to a file, and a socketpair on its
  own, are right). The catalogue's files are at most 8 KB and each is
  checked as it comes (a packed one must unpack, a text has no NUL). The
  cause was found in 0.37 (decision 63); the packages now come from the
  mirror over https again, plain http only if https fails (apk's signature
  check makes that safe).
- **Where it is published (17 Nov 2026):** GitHub runs scheduled workflows
  only from the default branch (main), and psilinux is not merged to main
  yet, so the daily run never ran. It now runs from a small public
  repository, danieledge/psilinux-apkcat, whose main holds only a copy of
  the generator, the workflow and a README; it publishes to that
  repository's own Pages
  (`https://danieledge.github.io/psilinux-apkcat/apk/v3.24/armhf/`), the
  wrapper's default. psilinux's Pages (the first URL) are the wrapper's
  fallback, used only when the new one cannot be reached or is not a
  catalogue, until they are retired; `/var/cache/apk/psion/cat-url` keeps
  which one the cached stats came from, so the cached files always match
  them. Not chosen: keeping the old URL by pushing to psilinux's gh-pages
  from the new repository, which needs a long-lived token with write access
  to psilinux stored as a secret there; or merging to main early. The
  generator stays here with its tests; the copy there is updated by hand.
- **Alternatives:** a new repository for the catalogue (not needed at
  first: this repository's Pages were free; see above); one file per search (25,000 names is 80 KB
  packed, too slow over serial); Alpine's own apk on the netBook (it needs
  36 MB free; too much for a 5mx, and tight on a 64 MB netBook).
- **Also:** `sendfile` into a full pipe returned 0, which callers read as
  the end of the input: `cat FILE | ...` stopped at 8 KB. It now waits, as
  write does.

## 62. The welcome banner: fastfetch's look, drawn by bash (0.37)
- **What.** When Alpine's shell starts on PsiLinux's window, it shows what
  the marketing pictures show: Alpine's logo beside root@psion, OS, Kernel,
  Packages, Shell, Terminal, Memory, Disk, Locale and the colour blocks.
  Before, a first start showed a fortune (if that) and the prompt.
- **Not fastfetch at each start.** A new Linux session translates every
  program afresh, so fastfetch is always a first run at login: 19 s in the
  5mx emulator, 11 s the second time, about 6 s and 4 s on a 5mx by the
  emulator's factor of 3 (in the netBook emulator, 0.6 s and 0.4 s). Caching its output was the other way
  (a background run when apk changes something), but a background program
  of 3 MB on a 5mx with 6 MB free, slowing the first minutes, for figures
  that are wrong anyway (Memory and Disk change; fastfetch's Uptime said
  "496 days" on the netBook), was worse than drawing it: bash already runs,
  and everything but the disk is a read of /proc or a file. Measured in
  the emulators (a login shell, `time bash -lic exit`, three times each):

  | | with the banner | without (~/.hushlogin) | the banner itself |
  |---|---|---|---|
  | 5mx emulator | 3.8 s (3.8, 6.0, 3.8) | 1.8 s (1.8, 2.8, 1.8) | about 2 s: about 0.7 s on a 5mx |
  | netBook emulator | | | 0.30 s, then 0.22 s (`psilinux_welcome`); fastfetch 0.60 s, 0.40 s |

  On the 5mx about a third of it is df (0.45 s in the emulator, its fork
  and exec), a third reading the files, and the rest bash reading the
  function and drawing.
- **Written for bash on a 5mx.** Small things are dear there: a bash loop
  of 20 string appends took 1.2 s in the emulator, reading /etc/os-release
  a line at a time 1.1 s and /proc/mounts 1.2 s (bash's `read` takes one
  byte per call from a file it cannot seek), `printf -v` and `[[ =~ ]]`
  each tens of milliseconds, a here-string failed outright (below). So:
  no loops; each file read whole with `read -d ''`; the file system's name
  from df's own line; numbers formatted with arithmetic; and the screen
  drawn by one `printf` from a fixed format per layout (generated once,
  with the logo in it), the figures as its arguments.
- **Packages** are counted with one grep of apk's list (6 s in the
  emulator) only when the list is newer than the count kept in
  /var/cache/psilinux/welcome-packages; the image ships the count.
- **The layout follows the window** (COLUMNS and LINES, which an
  interactive bash has from the start; `stty size` if not): fastfetch's big logo when 21 rows and the widest
  line plus 44 columns fit (the netBook), the small one with 13 rows (a 5mx
  at the standard text size, toolbar shown or not), text only otherwise,
  and without the colour blocks, then without the title, when the window
  is shorter still, so that the prompt stays on the screen.
- **Where.** `/etc/profile.d/psilinux-welcome.sh` (a new file, so it reaches
  a root installed over an older one even where the user has changed
  /etc/bash/psilinux.sh), `/usr/lib/psilinux/welcome` (the function). Only
  a login bash that is interactive, on a terminal, not over ssh, without
  `~/.hushlogin`: never for scripts, `bash -c`, the epoc command or ssh
  commands. The fortune moved there from /etc/bash/psilinux.sh: it shows
  when the banner is turned off. A user's own copy of psilinux.sh keeps
  its fortune, so they see both until they remove it.
- **Tools > Show welcome banner** (ticked as standard; the Tools menu has 7
  items). The app keeps it in a spare field (KLxSpareNoWelcome: 0 shows, as
  in older settings files) and the shared chunk (LX_SHARED_VERSION 7,
  `welcome_off`, live: the next shell reads it); the kernel shows it as
  /proc/psilinux/welcome (1 or 0). A root on an older PsiLinux has no such
  file and shows the banner.
- **Found on the way:** a bash here-string or here-document said "cannot
  create temp file for here-document: Resource busy" in the 5mx emulator,
  at login (perhaps bash deleting its file in /tmp while it is open, which
  EPOC refuses; the host harness, on Linux's own files, does not show it).
  Not looked into further here.

## 63. A blocking write to a pipe or a stream socket writes everything (0.37)
- **Found:** BusyBox wget's https downloads had 8 KB of zeros in every
  16 KB. wget reads from Alpine's `ssl_client` through a socketpair (two
  8 KB pipes here). ssl_client writes each TLS record (up to 16 KB) with one
  `write`; ours wrote what fitted and returned less, and ssl_client's loop
  then went on from the wrong place: its buffer is `size_t buf[]` and it
  counts bytes as elements (`&buf[i]`, `i += r`), so the rest came from 4
  times as far in, from zeros. On Linux this never shows: a blocking write
  to a pipe or a stream socket returns only when all is written (POSIX), and
  a socketpair holds far more than 16 KB.
- **Chosen:** as Linux: a blocking `write`, `writev`, `send`, `sendto` or
  `sendmsg` to a pipe or a stream socket (AF_UNIX or TCP) that writes part
  waits for room and goes on where it got to (`t->wdone`, kept while the
  call is parked, so the registers are untouched), and returns once all is
  written; with what went before an error, or before a signal (no
  SA_RESTART restart then, as Linux). Non-blocking ones still write what
  fits. `tests/writetest.c` (in `tests/run-host.sh`) checks each call, a
  signal in the middle, and the bytes.
- **Alternatives:** bigger pipes for socketpairs (only moves the edge, and
  costs the Psion's memory); patching ssl_client (Alpine's own package; any
  program counting on full writes would still break).

## 64. Here-documents, and big $(...), as bash expects (0.37)
- **Found:** in the 5mx emulator bash said "cannot create temp file for
  here-document: Resource busy" for any `<<<` or `<<`. bash asks a pipe's
  size (F_GETPIPE_SZ), and ours said 0, so it used a temporary file; it
  opens it twice, closes one, and unlinks it while the other is open, and
  EPOC refuses to delete an open file (KErrInUse, EBUSY). The host harness
  never showed it: Linux deletes open files.
- **Chosen:** F_GETPIPE_SZ says 8 KB (F_SETPIPE_SZ: 8 KB or less is
  accepted, more is EPERM), so short documents go through a pipe as on
  Linux. And a file of ours unlinked while open is hidden at once and
  deleted at its last close (`lx_native_unlink`, as Linux does it); one
  that EPOC itself holds is still EBUSY. The host harness now refuses to
  delete or rename an open file, as EPOC does (`LX_HOST_UNIX_FILES=1`:
  not), so the suites see what the Psion sees. Left: the name stays in a
  directory listing until the close, and stays on the disk if the runner
  stops before that.
- **Also found:** a fork child (bash's `$(...)`, a subshell) writing more
  than a pipe holds to its parent hung for ever: the parent waits for the
  child (a fork here, DESIGN.md), so nobody reads. Also `$(a | b)`, where b
  writes and the subshell waits for it. Now a pipe whose every reader is a
  parent that cannot run until a fork child is done grows to take the
  write (and shrinks when emptied); if the parent would have run on, more
  memory is all it cost. `x=$(seq 1 20000)` and `$(head -c 30000 f | tr
  ...)` work.
- **Tests:** m1test (unlink an open file; F_GETPIPE_SZ), writetest (a fork
  child writing 120 KB, itself and through a program it waits for).

## 65. "Linux has ended" by itself: a fork child's signal, sendfile, and the end log (0.37)
- **Found:** one run in testing ended with "Linux has ended" (the first
  program killed by a signal, or ended by the runner) for no visible
  reason, and was never repeated. Nothing said why: the runner's log is
  off by default, and its own reasons (End Linux, the app's heartbeat)
  were not logged at all.
- **Found while looking:** a fork child (no exec) runs on its parent's
  thread here (DESIGN.md), and the signals for a thread stayed with the
  thread when it went back. A child with SIGPIPE blocked that wrote to a
  pipe nobody reads left a SIGPIPE pending, and its parent, the shell,
  took it at its next call and ended silently. A write of the child's
  killed (SIGKILL) while it waited left `t->wdone` too, which the parent's
  next interrupted call returned. Now the borrow keeps the parent's
  pending signals and gives the child none; they come back when it is
  done (an exec takes the child's with it); `t->wdone` is cleared.
- **Also:** `c=$(cat <<EOF ... EOF)` with a document over 8 KB hung for
  ever: BusyBox's cat sendfiles into the `$(...)` pipe, and sendfile
  waited for poll's 4 KB of room, so the pipe never grew for the waiting
  shell (decision 64). sendfile into a pipe now asks as write does
  (`lx_pipe_write_waits`).
- **The heartbeat:** the app's tick (EPriorityLow) is its heartbeat; a
  steady stream of output keeps the bell's callback (EPriorityStandard)
  busy and could starve it, and after 45 s the runner ends Linux as if the
  app had gone. The app now beats in every Pump too.
- **The end log:** each session's end is one line in
  `\System\Apps\PsiLinux\PsiLinux-end.log` (beside the runner; 8 KB, then
  `.old`), always: the time, the version, the runner's reason if it ended
  it (the app asked; the heartbeat stopped; reboot), and how the first
  program ended (exit code, or the signal, a fault or not, the last pid
  that sent it one, the call it was in). The host harness prints it with
  `LX_END_NOTE=1`.
- **Tests:** writetest (sendfile from a program a fork child waits for;
  a fork child's blocked SIGPIPE), bashtest (a 14 KB here-document in
  `$(cat)`).
