#!/bin/sh
set -e
SRCDIR="$1"
BUILDDIR="$2"

# Vim's build system lives in src/
SRC="$SRCDIR/src"

mkdir -p "$BUILDDIR/auto" "$BUILDDIR/objects" "$BUILDDIR/xdiff"
touch "$BUILDDIR/objects/.dirstamp"

cd "$BUILDDIR"

# Symlink source files into the build directory.
# Individual file symlinks (not directory symlinks) so that relative
# #include paths like "auto/config.h" and "../auto/config.h" resolve
# to the build directory's generated headers, not stale source tree copies.
for f in "$SRC"/*.c "$SRC"/*.h "$SRC"/*.in "$SRC"/*.sh "$SRC"/Makefile "$SRC"/*.mak; do
    [ -e "$f" ] && ln -sf "$f" .
done
for f in "$SRC"/xdiff/*.c "$SRC"/xdiff/*.h; do
    [ -e "$f" ] && ln -sf "$f" xdiff/
done
# Read-only subdirectories (no auto/ refs) — safe as directory symlinks
for d in proto libvterm xxd testdir; do
    ln -sf "$SRC/$d" .
done
# configure scripts
ln -sf "$SRC/configure" .
ln -sf "$SRC/auto/configure" auto/

CROSS_FLAGS=""
TLIB="ncurses"
CC_FLAG=""
CFLAGS_EXTRA=""
if [ -n "$TVM_IOS_CROSS" ]; then
    CROSS_FLAGS="--host=aarch64-apple-darwin vim_cv_toupper_strstrprogram=yes vim_cv_terminfo=no vim_cv_tgetent=zero vim_cv_getcwd_broken=no vim_cv_stat_ignores_slash=yes vim_cv_memmove_handles_overlap=yes"
    TLIB=""
    CC_FLAG="${CC:+CC=$CC}"
    CFLAGS_EXTRA="${TVM_STUBS_DIR:+-I$TVM_STUBS_DIR}"
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
        $CC_FLAG \
        CFLAGS="-g -gdwarf-4 $CFLAGS_EXTRA"
fi
make VIMRUNTIMEDIR=/tvm/share/vim CFLAGS="-g -gdwarf-4 $CFLAGS_EXTRA" MAKEOVERRIDES=
