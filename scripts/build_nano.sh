#!/bin/sh
# Build GNU nano for TVM (unpatched upstream, autotools build from git).
#
# usage: build_nano.sh <srcdir> <builddir> <gnulibdir> <ncurses-prefix>
# env:   CC (the target compiler, e.g. ios-cc.sh when cross-compiling),
#        TVM_IOS_CROSS=1 when building for iOS
#
# nano's git tree has no configure script and no gnulib sources: upstream's
# autogen.sh clones gnulib at a pinned commit, imports the module list with
# gnulib-tool and runs autoreconf, all inside the source tree. The same steps
# run here, with the pinned gnulib checked out as the third_party/gnulib
# submodule (no network at build time) and in a shadow tree under
# $BUILDDIR/src (directories mirrored, files symlinked, as build_vim.sh does),
# so the generated files never touch the submodule. The module list is read
# from autogen.sh itself so it cannot drift from upstream's.
#
# One file is left out of the shadow tree: roll-a-release.sh. nano's configure
# takes its presence to mean "building from git" and then demands that pkg.m4
# sit in `aclocal --print-ac-dir` itself (Homebrew keeps it in a dirlist
# directory) and that msgfmt exist -- sanity checks for developers, neither is
# used by the build. Without it, --version also prints the plain release
# string ("GNU nano, version 9.2") instead of a git describe.
#
# The curses library is the static ncursesw from build_ncurses.sh, found via
# its pkg-config file (nano's configure prefers pkg-config); the terminfo
# database is shipped in the initrd at /tvm/share/terminfo.
set -e
SRCDIR="$1"
BUILDDIR="$2"
GNULIB="$3"
NCURSES_PREFIX="$4"

SHADOW="$BUILDDIR/src"
OUT="$BUILDDIR/build"
. "$(cd "$(dirname "$0")" && pwd)/tvm_stamp.sh"
tvm_stamp_begin "$BUILDDIR" "$0" "$SRCDIR" "$GNULIB" "$NCURSES_PREFIX/lib/libncursesw.a" \
    "$(cd "$(dirname "$0")/.." && pwd)/third_party/configs/nano_ios.cache"

if [ ! -f "$SHADOW/configure" ]; then
    rm -rf "$SHADOW"
    mkdir -p "$SHADOW"
    (cd "$SRCDIR" && find . -path ./.git -prune -o -type d -print) | while read -r d; do
        mkdir -p "$SHADOW/$d"
    done
    (cd "$SRCDIR" && find . -path ./.git -prune -o -type f -print) | while read -r f; do
        ln -s "$SRCDIR/$f" "$SHADOW/$f"
    done
    rm -f "$SHADOW/roll-a-release.sh"

    cd "$SHADOW"
    modules="$(sed -n '/^modules="/,/^"/p' autogen.sh | sed -e '1d' -e '$d')"
    [ -n "$modules" ] || { echo "error: no gnulib module list found in autogen.sh" >&2; exit 1; }
    "$GNULIB/gnulib-tool" --import $modules
    autoreconf --install --symlink --force
fi

CONFIGS_DIR="$(cd "$(dirname "$0")/.." && pwd)/third_party/configs"

mkdir -p "$OUT"
cd "$OUT"

CROSS_FLAGS=""
CC_FLAG=""
if [ -n "$TVM_IOS_CROSS" ]; then
    CROSS_FLAGS="--host=aarch64-apple-darwin"
    CC_FLAG="${CC:+CC=$CC}"
    # Run-time probes that cross-compiling cannot execute: answered from the
    # native macOS configure through an autoconf cache file (see the header
    # of nano_ios.cache). configure rewrites its cache file, so a copy is used.
    if [ ! -f Makefile ]; then
        cp "$CONFIGS_DIR/nano_ios.cache" config.cache
    fi
    CROSS_FLAGS="$CROSS_FLAGS --cache-file=$OUT/config.cache"
fi

# Configure (out of tree):
#  --prefix=/tvm: SYSCONFDIR=/tvm/etc (the system nanorc), data under
#     /tvm/share/nano
#  --disable-nls: no gettext/libintl dependency
#  --disable-libmagic: no libmagic on any target
#  --enable-utf8: wide-character curses, which is what we build
#  PKG_CONFIG_LIBDIR: only our ncursesw.pc is visible to pkg-config, so the
#     curses probe cannot pick a host library
#  ac_cv_func_flockfile=no, ac_cv_func_funlockfile=no: TVM interposes stdio
#     with its own FILE objects, and neither flockfile() nor Darwin's inline
#     getc_unlocked() is interposed. With HAVE_FLOCKFILE, read_file()
#     (src/files.c) brackets its getc_unlocked() loop with flockfile(f) on
#     such a FILE, and libc's flockfile dereferenced it as one of its own
#     (SIGSEGV in pthread_mutex_lock via fp->_extra, "Sorry! Nano crashed!
#     Code: 11" on opening any existing file). Answered `no`, nano falls
#     back to plain getc(), which TVM interposes. (Same class of problem as
#     bash's *_unlocked macros, see build_bash.sh.)
if [ ! -f Makefile ]; then
    PKG_CONFIG_LIBDIR="$NCURSES_PREFIX/lib/pkgconfig" \
    "$SHADOW/configure" \
        --prefix=/tvm \
        --disable-nls \
        --disable-libmagic \
        --enable-utf8 \
        $CROSS_FLAGS \
        $CC_FLAG \
        ac_cv_func_flockfile=no ac_cv_func_funlockfile=no \
        CFLAGS="-g -gdwarf-4"
fi

# Build-time assertion: the curses library must be our wide ncurses.
if ! grep -q '^CURSES_LIB = .*-lncursesw' src/Makefile; then
    echo "error: nano was not configured against libncursesw" >&2
    exit 1
fi

# lib (gnulib) and src only: doc/ needs makeinfo and po/ is disabled.
make -C lib
make -C src
