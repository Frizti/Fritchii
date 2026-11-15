# SPDX-FileCopyrightText: Copyright 2026 Fritchii contributors
# SPDX-License-Identifier: GPL-3.0-or-later

# Build environment for the Fritchii Linux AppImage (tools/appimage/build.sh), the same toolchain
# as .github/workflows/linux-build.yml. Ubuntu 24.04 sets the minimum glibc of the result (2.39).
FROM ubuntu:24.04

ARG LLVM_VERSION=23
ARG QT_VERSION=6.8.3

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
        autoconf automake binutils ca-certificates curl file g++ gcc gettext git gnupg \
        glslang-tools libtool make nasm ninja-build patch perl pkg-config python3 python3-venv \
        unzip xz-utils zip \
        libasound2-dev libavcodec-dev libavfilter-dev libboost-context-dev libboost-dev \
        libboost-fiber-dev libegl1 libgl-dev libglu1-mesa-dev libopus-dev libpulse-dev \
        libswscale-dev libudev-dev libusb-1.0-0-dev libva-dev libvdpau-dev mesa-common-dev \
        spirv-headers spirv-tools zlib1g-dev \
        libdbus-1-3 libfontconfig1 libfreetype6 libglib2.0-0t64 libwayland-client0 \
        libwayland-cursor0 libwayland-egl1 libxcb-cursor0 libxcb-icccm4 libxcb-image0 \
        libxcb-keysyms1 libxcb-randr0 libxcb-render-util0 libxcb-shape0 libxcb-util1 \
        libxcb-xinerama0 libxcb-xkb1 libxkbcommon-x11-0 libxkbcommon0 \
        libfribidi0 libcom-err2 libgpg-error0 libopengl0

# Ubuntu 24.04's clang is 18; a current one from apt.llvm.org (-mtls-dialect=gnu2 needs 19+).
RUN curl -fsSL https://apt.llvm.org/llvm-snapshot.gpg.key \
        -o /etc/apt/trusted.gpg.d/apt.llvm.org.asc \
    && chmod 644 /etc/apt/trusted.gpg.d/apt.llvm.org.asc \
    && echo "deb http://apt.llvm.org/noble/ llvm-toolchain-noble-${LLVM_VERSION} main" \
        > /etc/apt/sources.list.d/llvm.list \
    && apt-get update \
    && apt-get install -y --no-install-recommends "clang-${LLVM_VERSION}" "lld-${LLVM_VERSION}"

# The project needs CMake 3.31 and Ubuntu 24.04 packages 3.28.
RUN python3 -m venv /opt/aqt \
    && /opt/aqt/bin/pip install --no-cache-dir aqtinstall==3.3.0 cmake==3.31.6 \
    && /opt/aqt/bin/aqt install-qt linux desktop "${QT_VERSION}" linux_gcc_64 \
        -m qtcharts -O /opt/qt

# OpenSSL's bundled build calls plain "clang", so the LLVM bin folder goes on PATH.
ENV PATH=/opt/aqt/bin:/usr/lib/llvm-${LLVM_VERSION}/bin:$PATH \
    QT_ROOT_DIR=/opt/qt/${QT_VERSION}/gcc_64 \
    LLVM_VERSION=${LLVM_VERSION}
