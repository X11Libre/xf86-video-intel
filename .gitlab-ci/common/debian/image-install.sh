#!/bin/bash

set -e
set -o xtrace

echo 'deb-src https://deb.debian.org/debian bullseye main' >>/etc/apt/sources.list.d/deb-src.list
echo 'deb-src https://deb.debian.org/debian bullseye-updates main' >>/etc/apt/sources.list.d/deb-src.list
echo 'deb http://deb.debian.org/debian bullseye-backports main' >> /etc/apt/sources.list

apt-get update

apt-get autoremove -y --purge

apt-get install -y --no-remove \
	autoconf \
	automake \
	build-essential \
	libtool \
	pkg-config \
	ca-certificates \
	git \
	debian-archive-keyring \
	python3 python3-setuptools libxshmfence-dev \
	clang \
	libxcb-icccm4-dev libxcb-xkb-dev \
	libxcb1-dev libx11-xcb-dev libxcb-dri2-0-dev libxcb-util-dev \
	libxfixes-dev libxcb-xfixes0-dev libxrender-dev libxdamage-dev libxrandr-dev \
	libxcursor-dev libxss-dev libxinerama-dev libxtst-dev libpng-dev libssl-dev \
	libxcb-dri3-dev libxxf86vm-dev libxfont-dev libxkbfile-dev libdrm-dev \
	libgbm-dev libgl1-mesa-dev libpciaccess-dev libpixman-1-dev libudev-dev \
	libgcrypt-dev libepoxy-dev libevdev-dev libmtdev-dev libinput-dev \
	mesa-common-dev libspice-protocol-dev libspice-server-dev \
	meson/bullseye-backports \
	nettle-dev \
	pkg-config \
	valgrind \
	x11-xkb-utils xfonts-utils xutils-dev x11proto-dev

build_autoconf() {
    local subdir="$1"
    shift
    (
        cd $subdir
        ./autogen.sh "$@"
        make -j${FDO_CI_CONCURRENT:-4}
        make -j${FDO_CI_CONCURRENT:-4} install
    )
}

build_meson() {
    local subdir="$1"
    shift
    (
        cd $subdir
        meson _build -Dprefix=/usr "$@"
        ninja -C _build -j${FDO_CI_CONCURRENT:-4} install
    )
}

do_clone() {
    git clone "$1" --depth 1 --branch="$2"
}

mkdir -p /tmp/build-deps
cd /tmp/build-deps

# xserver 1.18 and older branches require libXfont 1.5 instead of 2.0
echo "Installing libXfont 1.5"
do_clone https://gitlab.freedesktop.org/xorg/lib/libXfont.git libXfont-1.5-branch
build_autoconf libXfont

echo "Installing font-util"
do_clone https://gitlab.freedesktop.org/xorg/font/util.git font-util-1.4.1
build_autoconf util --prefix=/usr

echo "Installing libxcvt"
do_clone https://gitlab.freedesktop.org/xorg/lib/libxcvt.git libxcvt-0.1.0
build_meson libxcvt

# xserver requires xorgproto >= 2024.1 for XWAYLAND
echo "Installing xorgproto"
do_clone https://gitlab.freedesktop.org/xorg/proto/xorgproto.git xorgproto-2024.1
build_autoconf xorgproto

# Xwayland requires drm 2.4.116 for drmSyncobjEventfd
# xf86-video-freedreno and xf86-video-omap need extra features
echo "Installing libdrm"
do_clone https://gitlab.freedesktop.org/mesa/drm libdrm-2.4.116
build_meson drm -Dfreedreno=enabled -Dnouveau=enabled -Domap=enabled

rm -Rf /tmp/build-deps
