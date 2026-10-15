# psilxrun.exe - PsiLinux's runner for EPOC R5 (the kernel, linux/runtime,
# on top of EPOC: bk_epoc.cpp, bk_esock.cpp). Built in psion-build:
#   tools/docker/psibuild "make -C linux/runner"
SDK      ?= $(PSION_SDK)
E        := $(SDK)/epoc_cpp_sdk/epoc32
REL      := $(E)/release/marm/rel
GINC     := $(SDK)/gcc-3.0-psion-98r2-9/lib/gcc-lib/arm-epoc-pe/3.0-psion-98r2/include
export PATH := /usr/bin:/bin:$(SDK)/gcc-3.0-psion-98r2-9/bin:$(E)/tools
export TMP := /tmp/
export WINEDEBUG := -all
OBJ      := ../../build/lxrun-obj
RT       := ../runtime
CORE     := lx_core lx_mem lx_elf lx_vfs lx_root lx_dev lx_tty lx_fsys lx_net lx_dbt lx_dbt_rt lx_vfp lx_simg
SF       := $(basename $(notdir $(wildcard $(RT)/softfloat/*.c)))
OBJS     := $(addprefix $(OBJ)/,$(addsuffix .o,$(CORE))) $(addprefix $(OBJ)/sf/,$(addsuffix .o,$(SF))) $(OBJ)/lx_entry.o $(OBJ)/lx_dbt_entry.o $(OBJ)/bk_epoc.o $(OBJ)/bk_esock.o
DEFS     := -D__SYMBIAN32__ -D__PSISOFT32__ -D__GCC32__ -D__EPOC32__ -D__MARM__ -D__EXE__ -DNDEBUG
ARCH     := -fomit-frame-pointer -O2 -fno-strict-aliasing -nostdinc -mapcs-32 -msoft-float
CFLAGS   := $(ARCH) -std=gnu99 -fno-builtin -I $(GINC) -I $(RT) -I $(RT)/compat -Wall -Wno-unused
CXXFLAGS := $(ARCH) $(DEFS) -Wno-ctor-dtor-privacy -fcheck-new -fvtable-thunks -I $(GINC) -I $(RT) -I ../common -I ../../ssh -I $(E)/include
LIBGCC   := $(SDK)/gcc-3.0-psion-98r2-9/lib/gcc-lib/arm-epoc-pe/3.0-psion-98r2/libgcc.a
LIBS     := $(REL)/euser.lib $(REL)/efsrv.lib $(REL)/esock.lib $(REL)/insock.lib $(REL)/nifman.lib $(LIBGCC)
HDRS     := $(wildcard $(RT)/*.h) ../common/lxshared.h

all: $(OBJ)/psilxrun.exe

$(OBJ)/%.o: $(RT)/%.c $(HDRS)
	@mkdir -p $(OBJ)
	arm-pe-gcc -c $(CFLAGS) $< -o $@

$(OBJ)/sf/%.o: $(RT)/softfloat/%.c $(wildcard $(RT)/softfloat/*.h)
	@mkdir -p $(OBJ)/sf
	arm-pe-gcc -c $(CFLAGS) -I $(RT)/softfloat $< -o $@

$(OBJ)/lx_entry.o: $(RT)/lx_entry.S
	@mkdir -p $(OBJ)
	arm-pe-gcc -c $< -o $@

$(OBJ)/lx_dbt_entry.o: $(RT)/lx_dbt_entry.S
	@mkdir -p $(OBJ)
	arm-pe-gcc -c $< -o $@

$(OBJ)/%.o: %.cpp $(HDRS)
	@mkdir -p $(OBJ)
	arm-pe-gcc -c $(CXXFLAGS) $< -o $@

# (stack: the kernel thread only; heap up to 8 MB for the kernel's tables)
$(OBJ)/psilxrun.exe: $(OBJS)
	cd $(OBJ) && arm-pe-ld -s -e _E32Startup --base-file psilxrun.bas -o psilxrun.tmp.exe $(REL)/eexe.o $(OBJS:$(OBJ)/%=%) $(LIBS)
	cd $(OBJ) && arm-pe-dlltool --as=arm-pe-as --output-exp psilxrun.exp --base-file psilxrun.bas $(REL)/eexe.o $(OBJS:$(OBJ)/%=%) $(LIBS)
	cd $(OBJ) && arm-pe-ld -s -e _E32Startup -Map psilxrun.map -o psilxrun.tmp.exe psilxrun.exp $(REL)/eexe.o $(OBJS:$(OBJ)/%=%) $(LIBS)
	cd $(OBJ) && wine $(E)/tools/petran.exe psilxrun.tmp.exe psilxrun.exe -nocall -uid1 0x1000007a -uid2 0x00000000 -uid3 0x01000A88 -heap 0x10000 0x800000 -stack 0x10000 >/dev/null

clean:
	rm -rf $(OBJ)
.PHONY: all clean
