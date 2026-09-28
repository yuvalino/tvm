#!/bin/sh
# Build ncurses for TVM (unpatched upstream, out-of-tree autotools build).
#
# usage: build_ncurses.sh target|host <srcdir> <builddir>
# env:   CC (the target compiler, e.g. ios-cc.sh when cross-compiling),
#        TVM_IOS_CROSS=1 when building for iOS
#
# target: the static wide-character library (libncursesw.a, with the
#         terminfo code built in) that nano and less link against, installed
#         into <builddir>/prefix with its headers and a pkg-config file. No
#         programs, no terminfo database: the database is compiled at initrd
#         time (scripts/CMakeLists.txt) from ncurses' own misc/terminfo.src.
# host:   a build-machine ncurses whose only purpose is progs/tic, the
#         terminfo compiler used to produce that database. Built with the host
#         compiler even when the target is iOS, like toybox_host.
set -e
MODE="$1"
SRCDIR="$2"
BUILDDIR="$3"

mkdir -p "$BUILDDIR"
cd "$BUILDDIR"
PREFIX="$BUILDDIR/prefix"

# Common configuration: static only, no C++/Ada bindings, no manpages or
# tests, nothing installed with the database (--disable-db-install), no
# stripping (keep debug info).
#  --enable-mixed-case: pinned so the database layout does not depend on the
#     build machine's filesystem. ncurses otherwise probes whether the
#     filesystem is case-sensitive and, on a case-insensitive macOS volume,
#     uses hex directory names (78/xterm) instead of the usual letter
#     directories (x/xterm). The library and tic must agree with each other,
#     and with the initrd, on every build host.
COMMON_FLAGS="--prefix=$PREFIX --without-shared --with-normal --without-debug \
--without-ada --without-cxx --without-cxx-binding --without-manpages \
--without-tests --disable-db-install --disable-stripping --without-gpm \
--enable-mixed-case"

JOBS="$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)"

case "$MODE" in
    target)
        #  --enable-widec: wide-character library (nano needs wget_wch etc.)
        #  --enable-ext-colors: 256-colour pairs (xterm-256color)
        #  --without-progs: no tic/infocmp/tput for the target
        #  --disable-overwrite: headers under include/ncursesw/, as in a
        #     system install; the .pc file carries the include path
        #  --with-default-terminfo-dir/--with-terminfo-dirs: the compiled-in
        #     database location inside the VM (the rc files also export
        #     TERMINFO to the same path)
        #  --enable-pc-files: ncursesw.pc, which nano's configure uses via
        #     PKG_CONFIG_LIBDIR (see build_nano.sh)
        #  --with-ospeed=int: `ospeed` as an int instead of the traditional
        #     short. With a short, configure sets NCURSES_OSPEED_COMPAT and
        #     lib_baudrate.c takes its "old tty" path on every __APPLE__
        #     build, which #includes <sys/ttydev.h>; the iPhoneOS SDK has no
        #     such header (the macOS SDK does). Pinned on every platform so
        #     the macOS and iOS libraries are configured alike; the only
        #     consumers of ospeed are less and nano, built against these
        #     headers.
        FLAGS="$COMMON_FLAGS --enable-widec --enable-ext-colors --without-progs \
--disable-overwrite --with-ospeed=int \
--with-default-terminfo-dir=/tvm/share/terminfo \
--with-terminfo-dirs=/tvm/share/terminfo \
--enable-pc-files --with-pkg-config-libdir=$PREFIX/lib/pkgconfig"
        CC_FLAG="${CC:+CC=$CC}"
        if [ -n "$TVM_IOS_CROSS" ]; then
            # --with-build-cc: make_hash/make_keys run on the build host.
            FLAGS="$FLAGS --host=aarch64-apple-darwin --with-build-cc=cc"
        fi
        if [ ! -f Makefile ]; then
            "$SRCDIR/configure" $FLAGS $CC_FLAG CFLAGS="-g -gdwarf-4"
        fi
        make -j"$JOBS"
        make install.libs install.includes
        ;;
    host)
        if [ ! -f Makefile ]; then
            "$SRCDIR/configure" $COMMON_FLAGS --with-progs CC=cc
        fi
        make -j"$JOBS" libs
        make -C progs -j"$JOBS"
        ;;
    *)
        echo "usage: $0 target|host <srcdir> <builddir>" >&2
        exit 2
        ;;
esac
