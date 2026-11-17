# The epoc command (a Linux program: PsiLinux's toolchain, lx-cc).
#   tools/lxbuild "make -C epoc"         build/epoc/epoc (static, for the Psion)
#   tools/lxbuild "make -C epoc test"    the request format's tests, on the host (gcc)
OUT := ../build/epoc
CC  := $(LXTC)/bin/lx-cc

all: $(OUT)/epoc

$(OUT)/epoc: epoc.c ../common/lxepoc.h
	@mkdir -p $(OUT)
	$(CC) -Os -s -Wall -o $@ epoc.c

$(OUT)/epoctest: epoctest.c epoc.c ../common/lxepoc.h
	@mkdir -p $(OUT)
	gcc -O1 -g -Wall -Wno-unused-function -o $@ epoctest.c

test: $(OUT)/epoctest
	$(OUT)/epoctest

.PHONY: all test
