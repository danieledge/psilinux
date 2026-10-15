# psilinux-build: the build environment for PsiLinux's Linux side (the
# arm-linux-musleabi toolchain, BusyBox and the other programs, and the host
# harness, which runs under qemu-arm). The EPOC side still builds in
# psion-build (tools/docker/Dockerfile).
#   docker build -t psilinux-build linux/tools
FROM ubuntu:24.04
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential gcc g++ make patch git wget curl ca-certificates \
        xz-utils bzip2 gzip unzip rsync file texinfo bison flex bc cpio \
        python3 python3-pil qemu-user perl gawk libncurses-dev pkg-config \
        autoconf automake libtool m4 \
    && rm -rf /var/lib/apt/lists/*
