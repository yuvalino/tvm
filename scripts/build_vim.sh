#!/bin/sh
# Build vim for TVM (unpatched upstream, out-of-tree build in a tree of
# symlinks).
#
# usage: build_vim.sh <srcdir> <builddir> <ncurses-prefix>
# env:   CC (the target compiler, e.g. ios-cc.sh when cross-compiling),
#        TVM_IOS_CROSS=1 when building for iOS
#
# Terminal library: the static ncursesw of build_ncurses.sh with the
# terminfo database shipped in the initrd (/tvm/share/terminfo), the same as
# bash, less and nano use, on every platform. vim therefore talks terminfo
# (TERMINFO) everywhere and emits the terminal's own control sequences; it
# used to link stub termcap functions on iOS, which swallowed every
# sequence, and the host's libncurses on macOS and Linux.
set -e
SRCDIR="$1"
BUILDDIR="$2"
NCURSES_PREFIX="$3"
. "$(cd "$(dirname "$0")" && pwd)/tvm_stamp.sh"
tvm_stamp_begin "$BUILDDIR" "$0" "$SRCDIR" "$NCURSES_PREFIX/lib/libncursesw.a"

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
CC_FLAG=""
if [ -n "$TVM_IOS_CROSS" ]; then
    CROSS_FLAGS="--host=aarch64-apple-darwin vim_cv_toupper_strstrprogram=yes vim_cv_getcwd_broken=no vim_cv_stat_ignores_slash=yes vim_cv_memmove_handles_overlap=yes"
    CC_FLAG="${CC:+CC=$CC}"
fi

# Configure:
#  --with-tlib=ncursesw: -lncursesw, found through LDFLAGS; with the library
#     named, configure only link-tests it (the automatic selection runs a
#     test program, impossible when cross-compiling). CPPFLAGS puts our
#     <termcap.h> ahead of the SDK's (the iPhoneOS SDK has none).
#  vim_cv_terminfo=yes, vim_cv_tgetent=zero: what ncurses answers (tgoto
#     speaks terminfo; tgetent() returns 0 for an unknown terminal). Run-time
#     probes that cannot execute when cross-compiling; pinned on every
#     platform so the builds are configured alike.
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
        --with-tlib=ncursesw \
        --without-wayland \
        vim_cv_terminfo=yes vim_cv_tgetent=zero \
        $EXTRA_FLAGS \
        $CROSS_FLAGS \
        $CC_FLAG \
        CPPFLAGS="-I$NCURSES_PREFIX/include -I$NCURSES_PREFIX/include/ncursesw" \
        LDFLAGS="-L$NCURSES_PREFIX/lib" \
        CFLAGS="-g -gdwarf-4"
fi
make VIMRUNTIMEDIR=/tvm/share/vim CFLAGS="-g -gdwarf-4" MAKEOVERRIDES=

# Build-time assertion: the terminal library must be our ncursesw, not a host
# library (which the iOS device has no database for).
if nm -u vim | grep -q 'tputs'; then
    echo "error: vim imports tputs from a host library" >&2
    exit 1
fi
