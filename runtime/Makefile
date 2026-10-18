# PsiLinux's kernel (the runner's portable part)
#   linux/tools/lxbuild "make -C linux/runtime host"   psihost: the kernel on the
#        host, an ARM Linux program run under qemu-arm (build/linux/psihost)
# The EPOC runner (psilxrun.exe) is built by linux/runner/Makefile in psion-build.
OUT    := ../build/linux
CORE   := lx_core.c lx_mem.c lx_elf.c lx_vfs.c lx_root.c lx_dev.c lx_tty.c lx_fsys.c lx_net.c lx_dbt.c lx_dbt_rt.c lx_vfp.c lx_simg.c lx_rand.c
SF     := $(wildcard softfloat/*.c)
HDRS   := lx_core.h lx_abi.h lx_sysnr.h lx_vfs.h lx_bknet.h lx_dbt.h
CC     := $(LXTC)/bin/lx-cc
CFLAGS := -O2 -g -Wall -Wno-unused-function -Wno-pointer-to-int-cast -Wno-int-to-pointer-cast -fno-strict-aliasing

host: $(OUT)/psihost

$(OUT)/psihost: $(CORE) bk_host.c lx_entry.S lx_dbt_entry.S $(HDRS) $(SF)
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) -o $@ $(CORE) bk_host.c lx_entry.S lx_dbt_entry.S $(SF) -Isoftfloat -Wl,-z,stack-size=1048576

.PHONY: host
