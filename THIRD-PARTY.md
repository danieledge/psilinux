# Third-party components

PsiLinux's own code is MIT ([LICENSE](LICENSE)). This page lists the
third-party code that is compiled into PsiLinux's two EPOC programs, and
what each installer carries. The Linux programs inside the root images, and
where to get their source, are in [SOURCES.md](SOURCES.md).

## What is in each program

### psilxrun.exe (the runner)

Built by `runner/Makefile` from `runtime/` and `runner/`.

| Component | Where | Licence |
|---|---|---|
| PsiLinux's kernel, translator and EPOC back end | `runtime/`, `runner/`, `common/` | MIT (this repository) |
| [Berkeley SoftFloat](http://www.jhauser.us/arithmetic/SoftFloat.html) 3e (John R. Hauser), the subset Alpine mode's VFP needs, with the ARM-VFPv2 specialisation | `runtime/softfloat/` | BSD 3-clause: `runtime/softfloat/COPYING.txt`, installed as `SoftFloat-licence.txt` |
| libgcc from the Psion SDK's GCC 3.0 (Psion 98r2): integer division and other compiler helpers | linked from the SDK; not in this repository | GPL with the libgcc linking exception (the exception lets it be linked into programs under any licence) |
| EPOC import libraries (`euser`, `efsrv`, `esock`, `insock`, `nifman`) | from the SDK; not in this repository | Psion's SDK terms (they only name the ROM's DLLs) |

No GPL code is compiled into the runner. It loads Linux programs as data and
runs them as separate programs, in the same way as an operating system does.

### PsiLinux.app (the window)

Built by `build.sh` with makmake, from `app/` and some of PsiTerm's files.

| Component | Where | Licence |
|---|---|---|
| PsiLinux's app | `app/` | MIT (this repository) |
| PsiTerm's terminal: the glyphs (`glyphs.cpp`), the scroll bar (`termsb.cpp`) and the shared-chunk headers (`psishared.h`, `psibell.h`) | the [psiterm](https://github.com/danieledge/psiterm) checkout this sits in (`app/`, `ssh/`) | MIT |
| [libvterm](https://www.leonerd.org.uk/code/libvterm/) (PsiTerm's copy, with its Psion changes), as `psivterm.lib` | psiterm's `libvterm/` | MIT |
| [Terminus Font](https://terminus-font.sourceforge.net/), converted to EPOC format (`PsiTerm.gdr`) | psion-core's `pkg/psiterm.gdr` (this repository's `core/` submodule) | SIL Open Font License 1.1: installed as `Terminus-licence.txt` |
| EPOC C Standard Library (`STDLIB.SIS`, ESTLIB.DLL), embedded in `PsiLinux.sis` | from the SDK; not in this repository | Psion's redistributable runtime, shipped as the SDK directs |

## Build tools and patches in this repository

| Component | Where | Licence |
|---|---|---|
| The PsiLinux patch to [musl](https://musl.libc.org/) 1.2.6 (system calls through `__psi_svc`) | `toolchain/patches/musl-1.2.6-psilinux.diff` | MIT, as musl |
| `psi_atomic.c`: GCC's `__sync_*` functions without the kernel's helper page | `toolchain/psi_atomic.c` | MIT (this repository; written for PsiLinux) |
| BusyBox's configuration | `rootfs/busybox.config` | a configuration for BusyBox (GPL v2) |
| The toolchain: GCC 14.3.0, binutils 2.44, musl 1.2.6, through [musl-cross-make](https://github.com/richfelker/musl-cross-make) at `227df8b` | downloaded by `toolchain/build-toolchain.sh`; not in this repository | GCC and binutils GPL v3; GCC's runtime libraries GPL v3 with the GCC Runtime Library Exception; musl MIT |

## The installers

| Installer | Holds | Licences that apply |
|---|---|---|
| `PsiLinux.sis` | PsiLinux.app, psilxrun.exe, the Terminus font, STDLIB.SIS | MIT, BSD 3-clause, OFL 1.1, Psion's runtime terms (above) |
| `PsiLinux-root.sis` | `ROOT.IMG`: BusyBox, nano, git, curl and the rest, built from source | each program's own: GPL v2, GPL v3, MIT and others (SOURCES.md). The licences are in the image, in `/usr/share/licenses/` |
| `PsiLinux-alpine-root.sis` | `ROOT.IMG`: Alpine Linux 3.24.2 for armhf, unmodified packages | each package's own: GPL v2, GPL v3 (bash, nano, readline...), MIT, BSD, Apache 2.0 and others (SOURCES.md; in the image, `/usr/share/licenses/`). `apk info -a NAME` shows a package's licence |
| `LxProbe-test.sis` | the hardware probe (a test program) | MIT |
