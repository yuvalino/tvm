#!/bin/sh
set -e
SRCDIR="$1"
cd "$SRCDIR/src"
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
        --with-tlib=ncurses \
        --without-wayland \
        ac_cv_header_iconv_h=no \
        $EXTRA_FLAGS \
        CFLAGS="-g -gdwarf-4"
fi
make VIMRUNTIMEDIR=/tvm/share/vim CFLAGS="-g -gdwarf-4" MAKEOVERRIDES=

# static lib
ar rcs libvim.a objects/*.o
# shared lib
cc -dynamiclib -fPIC -g -o libvim.dylib objects/*.o -lm -lncurses -install_name libvim.dylib 2>/dev/null || true
