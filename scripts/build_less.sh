#!/bin/sh
# Build GNU less for TVM (unpatched upstream, built in a shadow tree).
#
# usage: build_less.sh <srcdir> <builddir> <ncurses-prefix>
# env:   CC (the target compiler, e.g. ios-cc.sh when cross-compiling),
#        TVM_IOS_CROSS=1 when building for iOS
#
# The less git repository ships no configure script, help.c, funcs.h or
# lessmsg.inc: releases generate them with `make -f Makefile.aut` (autoconf +
# perl). That step writes next to the sources, so it runs in a shadow tree of
# symlinks under $BUILDDIR/src (as build_vim.sh does) and the submodule stays
# untouched. Everything TVM-specific is a configure option or cache variable.
#
# Terminal library: the static ncursesw built by build_ncurses.sh, with the
# terminfo database shipped in the initrd (/tvm/share/terminfo). The same
# library and database serve nano, on macOS and iOS alike, so less does not
# depend on the host's terminfo (absent on iOS) or on bash's termcap file.
#
# After `make less`, the objects of less's own link line are exported to
# $BUILDDIR/objects/ so third_party/CMakeLists.txt can relink them as
# libless.dylib for the Flutter initrd (lesskey.o and lessecho.o each have a
# main() of their own, so a *.o glob cannot be used).
set -e
SRCDIR="$1"
BUILDDIR="$2"
NCURSES_PREFIX="$3"

SHADOW="$BUILDDIR/src"
mkdir -p "$SHADOW"
cd "$SHADOW"

# Shadow tree: one symlink per top-level source entry (the lesstest/ and
# unicode/ directories are read-only during the build).
for f in "$SRCDIR"/*; do
    ln -sfn "$f" "$(basename "$f")"
done

# Generated sources. Only the four named targets, not Makefile.aut's `all`:
# `all` also covers the *.uni tables, and a regenerated table would be written
# through its symlink into the submodule.
if [ ! -f configure ]; then
    make -f Makefile.aut help.c funcs.h lessmsg.inc ./configure
fi

TVM_CFLAGS="-g -gdwarf-4"
TVM_MAKE_CFLAGS="$TVM_CFLAGS"
CROSS_FLAGS=""
CC_FLAG=""
if [ -n "$TVM_IOS_CROSS" ]; then
    CROSS_FLAGS="--host=aarch64-apple-darwin"
    CC_FLAG="${CC:+CC=$CC}"
    # The iPhoneOS SDK marks system(3) unavailable and lsystem.c (the `!`
    # command) calls it; TVM interposes it. Bind the call to the symbol
    # through the same force-included header toybox uses. Make-time only: a
    # forced <stdlib.h> plus the macro would turn configure's
    # AC_CHECK_FUNCS(system) probe into a conflicting declaration and less
    # would drop shell escapes entirely (HAVE_SYSTEM undefined).
    INCLUDE_SHIM="$(cd "$(dirname "$0")/.." && pwd)/third_party/include-shim"
    TVM_MAKE_CFLAGS="$TVM_CFLAGS -include $INCLUDE_SHIM/tvm_ios_libc.h"
fi

# Configure:
#  --prefix=/tvm: SYSDIR=/tvm/etc (system lesskey file), LIBEXECDIR=/tvm/libexec
#  --with-regex=posix: the libc regcomp(); no pcre dependency
#  ac_cv_lib_tinfo{,w}_tgoto=no, ac_cv_lib_xcurses_initscr=no: the terminal
#     library probe tries these before ncursesw; answered `no` so the only
#     candidate is our static libncursesw.a (found through LDFLAGS)
if [ ! -f Makefile ]; then
    ./configure \
        --prefix=/tvm \
        --with-regex=posix \
        $CROSS_FLAGS \
        $CC_FLAG \
        ac_cv_lib_tinfo_tgoto=no ac_cv_lib_tinfow_tgoto=no \
        ac_cv_lib_xcurses_initscr=no \
        CPPFLAGS="-I$NCURSES_PREFIX/include -I$NCURSES_PREFIX/include/ncursesw" \
        LDFLAGS="-L$NCURSES_PREFIX/lib" \
        CFLAGS="$TVM_CFLAGS"
fi
make CFLAGS="$TVM_MAKE_CFLAGS" less

# Build-time assertion: the terminal library must be our ncursesw, not a host
# library (which the iOS device does not have a database for).
if ! grep -q '^LIBS = .*-lncursesw' Makefile; then
    echo "error: less was not configured against libncursesw" >&2
    exit 1
fi

# Export the link inputs for the libless.dylib relink.
printf 'include Makefile\ntvm-print-objects:\n\t@echo $(OBJ)\n' > tvm_link.mk
OBJECTS="$(make -s -f tvm_link.mk tvm-print-objects)"
rm -rf "$BUILDDIR/objects"
mkdir -p "$BUILDDIR/objects"
for o in $OBJECTS; do
    ln -sf "$SHADOW/$o" "$BUILDDIR/objects/$o"
done
