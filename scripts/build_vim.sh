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
TLIB_FLAG="--with-tlib=ncurses"
CC_FLAG=""
CFLAGS_EXTRA=""
LDFLAGS_EXTRA=""
if [ -n "$TVM_IOS_CROSS" ]; then
    CROSS_FLAGS="--host=aarch64-apple-darwin vim_cv_toupper_strstrprogram=yes vim_cv_terminfo=no vim_cv_tgetent=zero vim_cv_getcwd_broken=no vim_cv_stat_ignores_slash=yes vim_cv_memmove_handles_overlap=yes"
    TLIB_FLAG=""
    CC_FLAG="${CC:+CC=$CC}"
    CFLAGS_EXTRA="${TVM_STUBS_DIR:+-I$TVM_STUBS_DIR}"
elif [ -n "$TVM_ANDROID_CROSS" ]; then
    # Same shape as iOS: bionic has no ncurses/termcap, so drop --with-tlib and
    # build against stubs/ (TVM intercepts tputs at runtime). configure cannot
    # run target binaries when cross-compiling, so pre-answer its vim_cv probes.
    # stat_ignores_slash=no here (linux semantics, unlike darwin's yes).
    CROSS_FLAGS="--host=aarch64-linux-android vim_cv_toupper_strstrprogram=yes vim_cv_terminfo=no vim_cv_tgetent=zero vim_cv_getcwd_broken=no vim_cv_stat_ignores_slash=no vim_cv_memmove_handles_overlap=yes ac_cv_header_iconv_h=no"
    # Unlike the iOS SDK (which ships ncurses, so configure's tgetent probe
    # passes and the stubs only matter at link time), android has no terminal
    # library at all — point --with-tlib at our stub archive so the probe links.
    TLIB_FLAG="--with-tlib=termcap_stubs"
    LDFLAGS_EXTRA="${TVM_STUBS_LIB_DIR:+-L$TVM_STUBS_LIB_DIR}"
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
        $TLIB_FLAG \
        --without-wayland \
        ac_cv_header_iconv_h=no \
        $EXTRA_FLAGS \
        $CROSS_FLAGS \
        $CC_FLAG \
        CFLAGS="-g -gdwarf-4 $CFLAGS_EXTRA" \
        LDFLAGS="$LDFLAGS_EXTRA"
fi
make VIMRUNTIMEDIR=/tvm/share/vim CFLAGS="-g -gdwarf-4 $CFLAGS_EXTRA" MAKEOVERRIDES=
