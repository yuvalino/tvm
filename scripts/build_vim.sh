#!/bin/sh
set -e
SRCDIR="$1"
cd "$SRCDIR/src"
if [ ! -f auto/config.mk ]; then
    ./configure \
        --with-features=normal \
        --enable-gui=no \
        --disable-darwin \
        --disable-netbeans \
        --disable-channel \
        --without-x \
        --disable-xsmp \
        --disable-xattr \
        --disable-selinux \
        --disable-smack \
        --with-tlib=ncurses \
        --without-wayland \
        CFLAGS="-g -gdwarf-4"
fi
make VIMRUNTIMEDIR=/tvm/share/vim CFLAGS="-g -gdwarf-4" MAKEOVERRIDES=
