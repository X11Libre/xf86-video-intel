#!/bin/bash

set -e

. .github/scripts/util.sh

mkdir -p $X11_BUILD_DIR
cd $X11_BUILD_DIR

if [ "$X11_OS" = "Linux" ]; then
build_meson   drm               https://gitlab.freedesktop.org/mesa/drm                  libdrm-2.4.121   "" \
    -Domap=enabled
fi
build_meson   libxcvt           https://gitlab.freedesktop.org/xorg/lib/libxcvt          libxcvt-0.1.0
build_ac      xorgproto         https://gitlab.freedesktop.org/xorg/proto/xorgproto      xorgproto-2024.1
