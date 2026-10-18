# lxprobe.exe - the PsiLinux M0 probes (an exe: it needs writable static data)
SDK      ?= $(PSION_SDK)
E        := $(SDK)/epoc_cpp_sdk/epoc32
REL      := $(E)/release/marm/rel
export PATH := /usr/bin:/bin:$(SDK)/gcc-3.0-psion-98r2-9/bin:$(E)/tools
export TMP := /tmp/
export WINEDEBUG := -all
OBJ      := ../build/lxprobe-obj
CXXFLAGS := -s -fomit-frame-pointer -O2 -fno-strict-aliasing -nostdinc -mcpu=arm710 -mapcs-32 -msoft-float \
            -D__SYMBIAN32__ -D__PSISOFT32__ -D__GCC32__ -D__EPOC32__ -D__MARM__ -D__EXE__ -DNDEBUG \
            -Wno-ctor-dtor-privacy -fcheck-new -fvtable-thunks -I $(E)/include
LIBGCC   := $(SDK)/gcc-3.0-psion-98r2-9/lib/gcc-lib/arm-epoc-pe/3.0-psion-98r2/libgcc.a
LIBS     := $(REL)/euser.lib $(REL)/efsrv.lib $(LIBGCC)

all: $(OBJ)/lxprobe.exe

$(OBJ)/lxprobe.o: lxprobe.cpp
	@mkdir -p $(OBJ)
	arm-pe-gcc -c $(CXXFLAGS) $< -o $@

$(OBJ)/lxprobe.exe: $(OBJ)/lxprobe.o
	cd $(OBJ) && arm-pe-ld -s -e _E32Startup --base-file lxprobe.bas -o lxprobe.tmp.exe $(REL)/eexe.o lxprobe.o $(LIBS)
	cd $(OBJ) && arm-pe-dlltool --as=arm-pe-as --output-exp lxprobe.exp --base-file lxprobe.bas $(REL)/eexe.o lxprobe.o $(LIBS)
	cd $(OBJ) && arm-pe-ld -s -e _E32Startup -o lxprobe.tmp.exe lxprobe.exp $(REL)/eexe.o lxprobe.o $(LIBS)
	cd $(OBJ) && wine $(E)/tools/petran.exe lxprobe.tmp.exe lxprobe.exe -nocall -uid1 0x1000007a -uid2 0x00000000 -uid3 0x0100FA13 -heap 0x10000 0x400000 -stack 0x8000 >/dev/null
