# PsiLinux: design

PsiLinux runs real Linux programs, natively and at full speed, inside an EPOC
program on a Psion Series 5mx. Like WSL1, it has no Linux kernel: a small
kernel of its own (the *runner*) gives Linux programs the Linux system call
interface, built on EPOC's file server, sockets and memory.

```
 PsiLinux.app (EIKON: the terminal window, menus)          linux/app
      |   global chunk "PsiLinuxTerm" + doorbells           linux/common/lxshared.h
 psilxrun.exe (the runner)                                  linux/runner
      kernel thread: lx_core, lx_vfs, lx_tty, lx_net...     linux/runtime (portable C)
      Linux thread 1 ... n: the programs' own code (RThreads)
      ROOT.IMG + <disk>:\Linux\root\  = the root file system
```

The decisions and the alternatives are in [DECISIONS.md](DECISIONS.md).

## M0: what EPOC R5 allows (measured)

The probe is `linux/probe` (LxProbe: `lxprobe.exe`, which writes `LxProbe.log`
on the card, and a viewer app). It was run in the emulator (5mx ROM 1.05(260)).
The dangerous probes run only when `D:\LXDANGER.TXT` exists.

| Question | Finding |
|---|---|
| Can code in an RChunk run? | **Yes.** A word written to a local chunk and called returns as it should. No XN on ARMv4: every readable page is executable. |
| Does writing code need an I-cache flush? | In the emulator, 0 of 100 rewrite-then-run tests ran stale code. The ARM710T cache is unified (one cache for code and data), so a store is seen by the next fetch. **To confirm on the 5mx** (the probe repeats the test there). The runner never patches code, so this matters only for JITs (none yet). |
| ARMv4T instructions in user mode | LDRH, LDRSH, STRH, UMULL, SMULL, SWP and BX all run and give the right answers. MRC p15 c13 (the ARMv6 TLS register) runs in the emulator and reads 0; on a real ARM710T it should be an undefined instruction. musl is patched never to use it. |
| What does `svc 0` (Linux's system call) do? | **It is an EPOC executive call, and it takes the machine down.** svc 0 caused a data abort in the kernel (PC 0x50082e78, address 0x80321480, SVC mode), then the 5mx reset. A stray svc is catastrophic, not a catchable fault. So programs must contain **no svc at all** (see "system calls"). |
| Faults in user code | A thread can catch its own faults: `RThread::SetExceptionHandler` + a null or wild access gives the handler EExcPageFault (12). But a longjmp out of the handler leaves the kernel thinking the thread is still in it: the next fault panics the thread (KERN-EXEC 3). A handler that reports the fault and then waits, while another thread kills it, works cleanly (exit Kill, **no "Program closed" dialog**; a panic does show that dialog). |
| Interrupting a busy thread (preemption by exception) | `RThread::RaiseException(EExcUserInterrupt)` does reach a spinning thread's handler. But switching stacks inside the handler and repeating it brought the whole machine down. **Not used.** |
| Address space | Local chunks start at 0x00B00000 (the process's run area). One chunk can reserve 256 MB; a second 256 MB one fails: about 256-511 MB of address space per process. Chunks are placed 1 MB apart (64 chunks of 1 MB max: 0x00B00000 to 0x04A00000). |
| Memory | Emulator: 16 MB RAM, 14 MB free, one chunk grew to 14,080 KB. A real 5mx (the author's, measured by an earlier kernel-driver experiment): 9.5 MB free. |
| Thread stacks | RThread::Create accepted 8 KB to 8 MB stacks, and each thread could write near its stack's end. |
| EPOC calls on a stack in a chunk | A thread that switched its sp to a stack in its own chunk could still use RFs, RFile, User::After and the heap. Linux programs run on such stacks. |
| Cost of a request between two threads | 2,000 round trips (RequestComplete one way, WaitForRequest, RequestComplete back) took 8 ticks (125 ms) in the emulator: about 62 µs each in emulated time. The emulator runs the CPU about 3 times slower than a 5mx, so a system call should cost **about 20 µs** on the device. |
| CPU speed | Emulator 2M-loop: 63 ticks (984 ms). A real 5mx: 328 ms for the same loop (the same experiment): about 6 cycles a turn at 36.9 MHz. |

## The runner

### Threads, and why not green threads

Every Linux thread is an EPOC RThread of `psilxrun.exe`. The program's code runs
on it at full speed, preempted by EPOC like any thread. One more thread, the
**kernel thread**, runs the whole Linux kernel (`linux/runtime`), one request
at a time, so the kernel needs no locks. EPOC's handles (files, sockets, the
file server session) all belong to the kernel thread; Linux threads hold none,
so they can be killed at any moment.

A system call: the Linux thread completes a request of the kernel thread's
active object (`RThread::RequestComplete`), then waits on a request of its own.
The kernel thread handles the call and completes that request. A call that
would block (a read from an empty pipe, wait4, sleep) is *parked*: the kernel
answers it later, when something changes, by trying it again. Signals interrupt
parked calls (EINTR, or the call is made again after the handler with
SA_RESTART).

### System calls: no svc in the program

M0 showed that `svc 0` restarts the Psion. So PsiLinux's toolchain builds musl
so that every system call is `bl __psi_svc`, one function:

```
__psi_svc:  ldr ip, =__sysinfo  ; (PC-relative)
            ldr ip, [ip]
            cmp ip, #0
            ldrne pc, [ip]      ; the runner's entry (from AT_SYSINFO)
            svc 0               ; a real Linux kernel (qemu, a Linux board)
            bx lr
```

`__sysinfo` comes from the auxiliary vector (AT_SYSINFO), which the runner
sets to its table: `{ system call entry, compare-and-swap, get thread pointer,
barrier }`. The same binary therefore runs on PsiLinux *and* on real ARM Linux,
which the tests use. The patch (`linux/toolchain/patches/musl-1.2.6-psilinux.diff`)
touches only musl's ARM system call sites (`syscall_arch.h`, vfork, clone,
`__syscall_cp`, sigreturn, `__unmapself`) and `__set_thread_area`. The
toolchain build checks that libc.a has exactly one svc left (the fallback).

`__psi_svc`'s object file also carries an ELF note, "PsiLinux". The loader
refuses a program that has no note and has svc instructions in its executable
segments (it was built for another libc and would restart the Psion), with a
message in the log. Scanning and patching instructions was rejected: literal
pools hold data that looks like svc, and a wrong patch corrupts data.

`lx-cc` checks every program it links: it disassembles it while it still has
its ARM mapping symbols (which tell code from data) and fails the build if
any svc is outside `__psi_svc`. This caught libgcc's `__clear_cache`, an
inline svc that MicroPython calls; `psi_atomic.c` supplies one that goes
through `__psi_svc`.

### Thread pointer and atomics without kuser helpers

musl on ARMv4T gets its thread pointer and compare-and-swap from the Linux
"kuser helpers" at 0xffff0fxx, which EPOC has not got. With the patch,
`__set_thread_area` takes them from the runner's table instead (and GCC's own
`__sync_*` functions, which libgcc builds on the kuser page too, come from
`linux/toolchain/psi_atomic.c`, which lx-cc links into every program):

- **gettp** finds the calling thread by its stack pointer (every Linux thread's
  stack range is known) and returns its TLS pointer. No CP15 register.
- **cas** takes a lock with SWP (atomic on the ARM710T), because threads of one
  program are real EPOC threads and can be preempted between the compare and
  the store. A thread that finds the lock taken gives way (User::After).

### Memory (no MMU)

There is one address space. Each program is loaded at its own address:

- the **image** (text, data, bss) is an RChunk of its own, released whole when
  the program ends;
- each thread's **stack** is a double-ended chunk with 16 KB uncommitted below
  it, so running off the end faults (SIGSEGV) instead of overwriting memory;
- **mmap** gives out 4 KB pages of one arena chunk (first fit); the chunk
  shrinks whenever its top pages are free, so memory goes back to EPOC;
- **brk** never moves: musl's malloc then uses mmap.

Pointers from programs are checked against what the program owns before the
kernel uses them (EFAULT), because a fault in the kernel thread would end every
program.

### Programs: static-pie, self-relocating

Programs are static-pie ELF (ARM EABI, soft float, ARMv4T). The loader copies
the PT_LOAD segments into the image chunk and builds the stack (argv, envp,
auxv with AT_PHDR, AT_RANDOM, AT_SYSINFO...). musl's `rcrt1` relocates the
program itself, so the runner does not. `#!` scripts work. A **program cache**
keeps the bytes of the last programs run (2 MB, 4 programs, dropped when free
RAM is under 3 MB), because reading BusyBox from the card was the slow part of
every command.

### Processes: vfork, and fork by snapshot

Without an MMU two copies of a program cannot sit at the same address. As on
uClinux:

- **vfork**: the child runs on the parent's thread and stack, borrowing them,
  until it calls execve or _exit. Then the parent's vfork returns the child's
  pid, with the registers it had at the call (kept by the kernel, because the
  child has written over the stack). A new thread is made for the new program.
- **clone(CLONE_VM | CLONE_VFORK)** (posix_spawn) is the same, with the child
  on its own stack.
- **clone(CLONE_THREAD)**: threads (pthreads), as more RThreads in the program.
- **fork** is a vfork with a snapshot: the parent's writable memory (data and
  bss, every mapping, the used part of its stack) is saved, the child borrows
  the parent's thread and memory, and when the child execs or exits
  everything is put back and the parent's fork returns the child's pid. So
  fork-then-exec (git, `popen`, `system`) works, and the parent never sees
  what the child changed. A parent and child that must both run on (a server
  that forks to serve) cannot: the parent waits until the child execs or
  exits. The stack part is put back by the thread itself as it resumes,
  using no stack (`lx_resume_copy`), because it may be running on it.

BusyBox is built with CONFIG_NOMMU, and its shell is **hush** (BusyBox's ash
needs an MMU). hush runs subshells and `$(...)` by re-executing itself
(`/proc/self/exe`).

### Signals and job control

Signals are delivered when a thread's system call returns: the kernel builds
the Linux ARM rt_sigframe (siginfo, ucontext, sigcontext) and the thread jumps
to the handler, which returns through musl's `__restore_rt` (rt_sigreturn).
The frame is built in the kernel and copied onto the program's stack by the
thread itself as it resumes (in assembler, using no stack), because the
thread's own side of the call is still running on that part of its stack.

A signal whose default action ends or stops the process acts at once, even on
a thread that is running its own code (RThread::Kill, Suspend and Resume). A
signal with a handler, for a thread that makes no system calls (a busy loop),
waits for its next call. So ^C reaches every program that does I/O; for one
that does not, a second ^C within 5 s, while the first is still waiting, ends
the job.

Sessions, process groups and the terminal's foreground group are Linux's:
^C, ^Z and ^\ signal the foreground group; background jobs get SIGTTIN, and
SIGTTOU with `stty tostop`; `jobs`, `fg`, `bg`, `kill %1` work in hush.

### Files

```
/        ROOT.IMG (read-only) + <disk>:\Linux\root\ (writable): an overlay
/c, /d   the Psion's disks, as they are (and /z, the ROM)
/tmp     part of the root's writable layer
/dev     null zero full random urandom tty console tty1-4 stdin stdout stderr fd
/proc    self, <pid>/{stat,status,cmdline,comm,exe,cwd,fd,maps...}, meminfo, cpuinfo, uptime...
```

ROOT.IMG (made by `linux/tools/mkroot.py`) holds BusyBox and the other
programs, with their symbolic links and modes. Changes go to the layer, a folder
on the Psion's disk; changing a file of the image copies it up first, and
deleting one leaves a whiteout. FAT has no links or modes, so the layer keeps
them in a small file per folder, `_LXMETA`. Names with characters FAT refuses
are written `%XX`. The Psion's own disks (/c, /d) show every file as 0755 (as
WSL's /mnt/c does): chmod does nothing there, and `ln` makes no links.

### The terminal

Each terminal is a `lx_tty` in the kernel: canonical editing, echo, ISIG,
termios, the window size, the foreground process group. PsiLinux.app's view
(`lxterm.cpp`) is PsiTerm's terminal (libvterm, xterm-256color, the 16-grey
mapping of 256 colours, history, pen selection, the clipboard), talking to the
runner through the global chunk's two rings and doorbells.

### Networking

`lx_net.c` gives AF_INET TCP and UDP through the backend (ESOCK, the Psion's
own Internet connection), socketpair (two pipes), and a DNS answerer: musl's
resolver asks 127.0.0.53 (`/etc/resolv.conf`), and the kernel answers with
EPOC's RHostResolver.

## The host harness

`linux/runtime` builds twice: with `bk_epoc.cpp` into the runner, and with
`bk_host.c` into **psihost**, an ARM Linux program run under qemu-arm on a PC.
psihost runs the same kernel and the same entry path; its Linux threads are
pthreads, its memory mmap, its files the PC's. Tests run there in seconds:

```
linux/tools/lxbuild "make -C linux/runtime host"
linux/tools/lxbuild "cd build/linux && qemu-arm ./psihost -r ROOTDIR /bin/sh /usr/lib/psilinux/tests/m2test.sh"
```

## Views: the terminal, and a place for Remote apps

PsiLinux's window holds one view at a time. The terminal is one control,
`CLxTermView` (`app/lxterm.cpp`), which owns the terminal and its link to the
runner (`CLxSession`). `CLxAppUi` (`app/psilinux.cpp`) swaps views with
Ctrl+Q and View > Switch view (Terminal, Remote apps), and gives each the
view's room (`ViewRect`: the screen less the toolbar when it shows).

The Remote apps view (built separately; not in this repository) plugs in where
`CLxRemoteView` is now: a placeholder control that says the view is not
there yet. A real one needs only to be a `CCoeControl` made with
`ViewRect()`, put on the control stack by `SwitchToL`, and resized in
`ShowToolBarL`; commands that make no sense there are dimmed by
`HandleCommandL`'s check of `iView`.

## Alpine mode

Alpine Linux's armhf programs are built for ARMv6K with VFPv2. The 5mx's
ARM710T is ARMv4T with no FPU, and on EPOC an `svc` restarts the machine. So
programs without PsiLinux's ELF note are run through a dynamic binary
translator (`runtime/lx_dbt*.c`, `lx_vfp.c`). Using it is in
[ALPINE.md](ALPINE.md); the decisions are 24 to 48 in DECISIONS.md.

```
 guest code (Alpine's, ARMv6)    --translate a block on first run-->   code cache (1 MB)
   ordinary ARMv4 instructions:   copied as they are
   pc-relative, branches:          constants; chained branches; indirect via the dispatcher
   svc:                            into the runtime -> PsiLinux's kernel (as natively)
   ARMv6 media/multiply, VFP:      inline ARMv4 sequences, or the interpreter / SoftFloat
   unaligned access (faults):      fixed in the exception handler, then the site patched
```

- **Registers and memory.** Guest registers are the host's: r0-r14 and the
  flags. Guest addresses are host addresses (one address space, no MMU).
  Two words below the guest's sp are the translated code's scratch.
- **Blocks.** Up to 48 guest instructions, ending at a branch. Exits are
  chained (patched into direct branches) once their target is translated.
  Indirect branches (`bx lr`, `ldr pc`, `pop {…, pc}`) go through a
  dispatcher in the cache: a 4,096-entry hash of guest pc to host code.
- **Into the runtime** (`dbt_enter`, `lx_dbt_entry.S`). Every reason (system
  call, interpreter, VFP, unchained exit, dispatcher miss, SIGILL, an
  unaligned site) saves the guest's registers in the thread's frame
  (`t->dframe`), switches to the thread's own stack and calls
  `dbt_runtime`, which returns the host address to go on at.
- **One CPU.** Only one translated thread runs guest code at a time. The
  kernel hands the CPU over at system calls, and every 40 ms through the
  dispatcher's yield flag. So the translator's state needs no locks,
  LDREX/STREX need no monitor, and the cache can be flushed whenever the
  translator runs.
- **VFP.** The 32 single (16 double) registers and FPSCR are in the thread.
  Every VFP instruction goes to `lx_vfp.c`, on Berkeley SoftFloat 3e,
  bit-exact with an ARM's.
- **Shared images** (`lx_simg.c`). A program or library is loaded once, at
  one address, for every process that uses it, so its translations serve
  them all. Its writable part is each process's own, swapped in when that
  process gets the CPU.
- **fork.** fork is the native snapshot fork, with nested children; the
  VFP registers, thread pointer and signal state go in the snapshot too.
- **Booting.** `/sbin/init` is pid 1. OpenRC runs its `sysinit`, `boot` and
  `default` runlevels as in a container.
- **Testing.** `linux/tests/dbt/fuzz.py` makes random instruction sequences.
  Each runs natively under qemu (`-cpu arm1176`, an ARMv6 with VFP) and
  through psihost (`-cpu ti925t`, an ARMv4T as the 5mx), and the outputs must
  agree byte for byte. The `LX_DBT_SLOW` bits send classes of instruction to
  the interpreter (and 128 sends loads and stores through the unaligned-site
  path), so each path is checked against the hardware too.

### M0: what the translator can rely on (measured)

| Question | Finding |
|---|---|
| Unaligned loads and stores | EPOC enables alignment checking (CP15 control 0x27F). An unaligned word or halfword access raises EExcAlignment in the thread. The handler can find the faulting registers on the thread's stack (EUSER's trampoline frame), do the access a byte at a time, and return: 40 of 40 in the faithful emulator. EPOC does not keep the flags across the handler, so `lx_exc_entry` saves and restores them. The ARM710T updates a written-back base before the abort. **To confirm on a 5mx** (LxProbe sections 10 and 13). |
| How many unaligned accesses | One OpenRC boot made 36,000 faults, nearly all in musl's `memset` (halfword stores at odd addresses). After patching each site on its first fault: 2 faults, and 367,000 accesses through the patched sites (decision 40). |
| Code written, then run | 0 of 4,096 functions written and run at once ran stale code (the cache is unified). **To confirm on a 5mx** (LxProbe section 11). |
| apk with the full index | 29.6 MB peak (51 MB on x86-64): too much for a 5mx's 9 MB, hence the server helper (decision 41). |
| Translation overhead | Instructions executed, translated against native ARMv6: 1.6 times (md5sum) to 6.3 times (sort); 10 times for a short `ls -l` (mostly translating). |
| A boot's translation | OpenRC to the login prompt: 8,047 blocks, 605 KB of code, no flush of the 1 MB cache; 15,448 system calls; 55,844 instructions interpreted. About 8.3 MB free afterwards (emulator). |
