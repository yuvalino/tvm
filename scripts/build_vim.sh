#!/bin/sh
set -e
SRCDIR="$1"
cd "$SRCDIR/src"

CROSS_FLAGS=""
TLIB="ncurses"
if [ -n "$TVM_IOS_CROSS" ]; then
    CROSS_FLAGS="--host=aarch64-apple-darwin vim_cv_toupper_strstrprogram=yes vim_cv_terminfo=no vim_cv_tgetent=zero vim_cv_getcwd_broken=no vim_cv_stat_ignores_slash=yes vim_cv_memmove_handles_overlap=yes"
    TLIB=""
fi

if [ ! -f auto/config.mk ]; then
    EXTRA_FLAGS=""
    if [ "$(uname)" = "Darwin" ]; then
        EXTRA_FLAGS="--disable-darwin"
    fi
    ./configure \
        --with-features=normal \
        --enable-gui=no \
        --disable-nls \
        --disable-netbeans \
        --disable-channel \
        --without-x \
        --disable-xsmp \
        --disable-xattr \
        --disable-selinux \
        --disable-smack \
        ${TLIB:+--with-tlib=$TLIB} \
        --without-wayland \
        ac_cv_header_iconv_h=no \
        $EXTRA_FLAGS \
        $CROSS_FLAGS \
        CFLAGS="-g -gdwarf-4"
fi
make VIMRUNTIMEDIR=/tvm/share/vim CFLAGS="-g -gdwarf-4" MAKEOVERRIDES=
