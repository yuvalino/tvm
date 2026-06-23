#!/bin/sh
set -e
SRCDIR="$1"
cd "$SRCDIR/src"
if [ ! -f auto/config.mk ]; then
    EXTRA_FLAGS=""
    if [ "$(uname)" = "Darwin" ]; then
        EXTRA_FLAGS="--disable-darwin --disable-nls ac_cv_header_iconv_h=no"
    fi
    ./configure \
        --with-features=normal \
        --enable-gui=no \
        --disable-netbeans \
        --disable-channel \
        --without-x \
        --disable-xsmp \
        --disable-xattr \
        --disable-selinux \
        --disable-smack \
        --with-tlib=ncurses \
        --without-wayland \
        $EXTRA_FLAGS \
        CFLAGS="-g -gdwarf-4"
fi
make VIMRUNTIMEDIR=/tvm/share/vim CFLAGS="-g -gdwarf-4" MAKEOVERRIDES=
