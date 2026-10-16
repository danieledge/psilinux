# Building PsiLinux

PsiLinux has two halves, built with two toolchains:

- **The EPOC side**: `PsiLinux.app` (the window) and `psilxrun.exe` (the
  runner), built with the Psion EPOC R5 C++ SDK (GCC 3.0, 2002).
- **The Linux side**: the programs in the root images, built with a modern
  GCC 14 and musl, patched for PsiLinux.

Both run in Docker. No root is needed on the host, beyond being able to run
Docker.

## Where it sits

PsiLinux shares its terminal (libvterm, the glyphs, the font) and its build
environment with [PsiTerm](https://github.com/danieledge/psiterm). So this
repository is checked out **as the `linux/` folder of a psiterm checkout**,
and the EPOC SDK sits beside psiterm:

```
work/
  psion_cpp_sdk_linux/       the EPOC R5 SDK (static-void/psion_cpp_sdk_linux @ 7763ee8)
  psiterm/                   github.com/danieledge/psiterm (dev branch)
    linux/                   this repository
    build/  dist/            what the builds make (both halves write here)
  psilinux-toolchain/        made by "lxbuild toolchain"
  psilinux-src/              downloaded sources, checked by sha256
  psionEmulators/            optional: the emulator, for testing
```

```sh
mkdir work && cd work
git clone https://github.com/static-void/psion_cpp_sdk_linux
git -C psion_cpp_sdk_linux checkout 7763ee8
git clone -b dev https://github.com/danieledge/psiterm
git clone https://github.com/danieledge/psilinux psiterm/linux
cd psiterm
```

psiterm's own `.gitignore` ignores `/linux/`, so the two repositories do not
see each other's files. The SDK is never committed to either.

What the EPOC side takes from psiterm:

| From psiterm | Used for |
|---|---|
| `tools/docker/psibuild` | the `psion-build` image: the SDK's GCC 3.0, wine (for makmake, petran, bmconv, makesis), python3, perl |
| `build_vterm.sh`, `libvterm/` | `psivterm.lib`, the terminal emulator |
| `app/glyphs.cpp`, `app/termsb.cpp` | Unicode to the Psion's code page; the scroll bar |
| `ssh/psishared.h`, `ssh/psibell.h` | the shared-chunk layout and the doorbells |
| `pkg/psiterm.gdr`, `pkg/terminus-licence.txt` | the Terminus font |
| `tools/emu/` | the emulator scripts (testing only) |

## The EPOC side

```sh
tools/docker/psibuild setup                        # once: the psion-build image and the SDK
tools/docker/psibuild "linux/build.sh"             # build/psilinux-pkg/
tools/docker/psibuild "linux/build.sh sis"         # and dist/PsiLinux.sis, plus the root installers if built
```

`linux/build.sh sis` also packs `build/psilinux-root/ROOT.IMG` into
`dist/PsiLinux-root.sis` and `build/alpine-root/ROOT.IMG` into
`dist/PsiLinux-alpine-root.sis`, if those images exist. So build the images
first (below) when making a release.

The probe (a test program, never a release):

```sh
tools/docker/psibuild "linux/probe/build.sh sis"   # dist/LxProbe-test.sis
```

The SDK keeps the objects of every `ptproj` app in one folder. After
switching branches, if the app panics oddly, delete the `*.o` files in
`$EPOCROOT/epoc32/build/.../ptproj/marmd/rel`.

## The Linux side

```sh
linux/tools/lxbuild setup                          # once: the psilinux-build image (Ubuntu 24.04)
linux/tools/lxbuild toolchain                      # once: GCC 14.3 + binutils 2.44 + musl 1.2.6, about 30 min
linux/tools/lxbuild "linux/rootfs/build-rootfs.sh" # BusyBox + ports + tests -> build/psilinux-root/ROOT.IMG
linux/alpine/build-root.sh                         # Alpine 3.24.2 + OpenRC -> build/alpine-root/ROOT.IMG
```

- `toolchain/build-toolchain.sh` builds an `arm-linux-musleabi` toolchain
  (ARMv4T, ARM mode, soft float) with musl-cross-make at a pinned commit,
  applies `toolchain/patches/musl-1.2.6-psilinux.diff`, and writes the
  `lx-cc` and `lx-c++` wrappers. They build static-pie programs and refuse
  any program with a system call instruction outside `__psi_svc`.
- `rootfs/build-rootfs.sh` builds BusyBox 1.36.1 (NOMMU, hush) and runs
  `ports/build-ports.sh` (nano, Lua, MicroPython, SQLite, jq, curl with
  Mbed TLS, git). Every download is pinned by version and sha256.
- `alpine/build-root.sh` needs Docker on the host. An `alpine:3.24` container
  unpacks Alpine's armhf minirootfs (pinned by sha256) and adds OpenRC with
  Alpine's own `apk --arch armhf`, checking the packages' signatures.
  `ALPINE_PKGS="nano less"` adds packages to the image.
- `tools/mkroot.py` packs a folder into `ROOT.IMG`, PsiLinux's read-only
  image format (it keeps symbolic links and modes, which FAT cannot).

Building your own programs for PsiLinux:

```sh
linux/tools/lxbuild "lx-cc -Os -o hello hello.c"   # a static-pie ARMv4T program
```

Copy it to the Psion (for example to `D:\hello`) and run it as `/d/hello`.

## The package helper (for apk in Alpine mode)

Alpine's catalogue is too big for a Psion's memory, so `apk` asks a helper
on a PC for searches and install plans: `alpine/server/psiapk.py`. It is a
plug-in for PsiProxy (psiterm's `proxy/`):

```sh
PSIPROXY_ADDR=0.0.0.0 PSIPROXY_PLUGINS=psiapk PSIPROXY_PLUGIN_PATH=$PWD/linux/alpine/server python3 proxy/psiproxyd.py
```

PsiProxy listens on 127.0.0.1:8081 unless `PSIPROXY_ADDR` says otherwise;
the Psion must be able to reach it, so give it your PC's LAN address (or
0.0.0.0) and keep it off the open Internet. See psiterm's
`proxy/README.md`.

It downloads Alpine's index (main and community, armhf, about 25,000
packages) into `~/.cache/psiapk` and refreshes it every 6 hours. Settings:
`PSIAPK_MIRROR`, `PSIAPK_BRANCH` (`v3.24`), `PSIAPK_ARCH`, `PSIAPK_CACHE`,
`PSIAPK_REFRESH`. See the module's docstring for the protocol.

Then, on the Psion, set `APK_SERVER=http://<that PC>:8081` in
`/etc/psilinux/apk.conf`.

## Tests

```sh
linux/tools/lxbuild "linux/tests/run-host.sh"            # M1, M2 and M4 suites on the host harness
linux/tools/lxbuild "linux/tests/dbt/run-fuzz.sh 1 12"   # the translator against a real ARMv6, 12 seeds
```

The host harness (`psihost`) is the same kernel built with `runtime/bk_host.c`
instead of the EPOC back end: an ARM Linux program run under `qemu-arm`. Run
it as an ARMv4T (`qemu-arm -cpu ti925t`) to match the 5mx.

`LX_DBT_SLOW=N` sends classes of instruction to the interpreter, so each
path can be fuzzed: bits 1 data processing, 2 loads and stores, 4 halfword
and dual, 8 ldm/stm, 16 media, 32 multiplies, 64 the rest, 128 loads and
stores through the unaligned-site path. `LX_DBT_STATS=1` prints the
translator's figures at the end.

### In the emulator

The emulator is [psionEmulators](https://github.com/joehaines/psionEmulators),
next to psiterm (`../psionEmulators`, or `$PSION_EMU`). It needs a Series 5mx
ROM, which you must supply yourself.

```sh
node --experimental-strip-types linux/tools/mkcard.ts psilinux    # build/emu/lxcard.img
python3 linux/tools/emurun.py NAME --at 85 'ls -l /bin | head\r' +5
```

`emurun.py` types into PsiLinux (`\r` Enter, `\e` Esc, `^c` Ctrl+C, `^+h`
Shift+Ctrl+H, `|` Shift+Ctrl+I) and leaves a screenshot a second in
`build/emu/NAME/`. A file named `TRACE` in the card's `\Linux\` folder makes
the runner log every system call to `\Linux\PsiLinux.log`.

**The emulator's card fails under long reads.** Runs that read tens of
thousands of sectors (the M2 suite, git, an Alpine boot) sometimes end in an
EPOC kernel fault in the timer queue, or the card stops answering. Every
such fault seen came during a card transfer, just after the emulator's own
"CF direct-invoke" workaround called the card driver. Rerun before
suspecting the code; [DECISIONS.md](DECISIONS.md) 21 and 48 have the detail.

## Making a release

1. Bump the version in `app/psilinux.cpp` and `pkg/psilinux.pkg` (two
   digits: 0.21 is `0,21,0`, because the EPOC installer compares only
   major.minor).
2. Build both images, then `tools/docker/psibuild "linux/build.sh sis"`.
3. Update the tables in [SOURCES.md](../SOURCES.md) if the images changed,
   and make the source archive it describes.
4. Tag `v0.NN` and attach the `.sis` files and the source archive to a GitHub
   release. Installers are never committed.
