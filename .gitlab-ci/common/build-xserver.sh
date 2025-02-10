#!/usr/bin/env bash

set -e
set -x

PLATFORM="$1"
XSERVER_REF="$2"

if [ ! "$XSERVER_REF" ]; then
    echo "missing XSERVER_REF variable" >&2
    exit 1
fi

XSERVER_CLONE=/tmp/xserver
XSERVER_BUILD=$XSERVER_CLONE/_builddir
XSERVER_REPO=https://gitlab.freedesktop.org/xorg/xserver.git

MACH=`gcc -dumpmachine`

export PKG_CONFIG_PATH="/usr/lib/$MACH/pkgconfig:/usr/share/pkgconfig:$PKG_CONFIG_PATH"
export PKG_CONFIG_PATH="/usr/local/lib/$MACH/pkgconfig:/usr/local/lib/pkgconfig:/usr/local/share/pkgconfig:$PKG_CONFIG_PATH"

echo "cloning xserver"
rm -Rf $XSERVER_CLONE
git clone --depth=1 -b $XSERVER_REF $XSERVER_REPO $XSERVER_CLONE

echo "checking platform: $PLATFORM"
case "$PLATFORM" in
    freebsd)
        echo "Building on FreeBSD"
        XSERVER_OS_AUTOCONF_FLAGS="--without-dtrace"
        XSERVER_MESON_DISABLE="glx udev udev_kms"
    ;;
    debian)
        echo "Building on Debian"
    ;;
    *)
        echo "unknown platform $PLATFORM" >&2
        exit 1
    ;;
esac

if [ -f $XSERVER_CLONE/meson.build ]; then
    (
        echo "Building Xserver via meson"
        for opt in $XSERVER_MESON_DISABLE ; do
            if grep "'$opt'" $XSERVER_CLONE/meson_options.txt ; then
                echo "disable $opt"
                XSERVER_MESON_FLAGS="$XSERVER_MESON_FLAGS -D$opt=false"
            else
                echo "no option $opt"
            fi
        done
        mkdir -p $XSERVER_BUILD
        cd $XSERVER_BUILD
        meson setup --prefix=/usr $XSERVER_MESON_FLAGS
        meson compile
        meson install
    )
else
    (
        echo "Building Xserver via autotools"
        cd $XSERVER_CLONE
        # Workaround glvnd having reset the version in gl.pc from what Mesa used
        # similar to xserver commit e6ef2b12404dfec7f23592a3524d2a63d9d25802
        sed -i -e 's/gl >= [79].[12].0/gl >= 1.2/' configure.ac
        ./autogen.sh --prefix=/usr $XSERVER_AUTOCONF_FLAGS $XSERVER_OS_AUTOCONF_FLAGS
        make -j`nproc`
        make -j`nproc` install
    )
fi
