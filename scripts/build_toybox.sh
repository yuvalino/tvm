#!/bin/bash
# Out-of-tree build for toybox via source-tree symlinks.
# Usage: build_toybox.sh <source-dir> <build-dir>
set -e

SRCDIR="$(cd "$1" && pwd)"
BUILDDIR="$(mkdir -p "$2" && cd "$2" && pwd)"
SCRIPTDIR="$(cd "$(dirname "$0")" && pwd)"
THIRDPARTY="$(cd "$SCRIPTDIR/../third_party" && pwd)"
. "$SCRIPTDIR/tvm_stamp.sh"
tvm_stamp_begin "$BUILDDIR" "$0" "$SRCDIR" "$THIRDPARTY/configs/toybox.config" "$THIRDPARTY/include-shim/tvm_ios_libc.h"

# Symlink source tree into build dir (read-only references)
for item in Config.in Makefile configure kconfig lib main.c scripts toys toys.h; do
    ln -sfn "$SRCDIR/$item" "$BUILDDIR/$item"
done

# .config is per-build — use TVM's config, not the submodule's. Refresh it
# whenever the tracked config changed, so enabling an applet takes effect
# without a clean build.
if ! cmp -s "$THIRDPARTY/configs/toybox.config" "$BUILDDIR/.config"; then
    cp "$THIRDPARTY/configs/toybox.config" "$BUILDDIR/.config"
    rm -rf "$BUILDDIR/generated"
fi

mkdir -p "$BUILDDIR/generated"

cd "$BUILDDIR"

if [ "$(uname)" = "Darwin" ] && [ -z "$TVM_IOS_CROSS" ]; then
    export PATH="/opt/homebrew/opt/gnu-sed/libexec/gnubin:$PATH"
    OPENSSL_PREFIX="$(brew --prefix openssl 2>/dev/null || echo /opt/homebrew/opt/openssl)"
    CFLAGS_EXTRA="-I$OPENSSL_PREFIX/include"
elif [ "$(uname)" = "Darwin" ]; then
    export PATH="/opt/homebrew/opt/gnu-sed/libexec/gnubin:$PATH"
    # iOS SDK marks system(3) unavailable; awk needs it (see the header)
    CFLAGS_EXTRA="-include $THIRDPARTY/include-shim/tvm_ios_libc.h"
fi

# Shim headers for iOS SDK gaps (e.g. sys/disk.h)
CFLAGS_EXTRA="${CFLAGS_EXTRA} -idirafter $THIRDPARTY/include-shim"

export CC="${CC:-cc}"
export HOSTCC="${HOSTCC:-cc}"
export OPTIMIZE="-ffunction-sections -fdata-sections -fno-asynchronous-unwind-tables -fno-strict-aliasing"
export CFLAGS="-g -gdwarf-4 -DTOYBOX_FORKLESS=1 ${CFLAGS_EXTRA}"
export V=1

# Never ship a binary left over from an earlier build: `make.sh || true`
# (its own final link can fail when cross-compiling, hence the fallback link
# below) used to leave a failed compile unnoticed, and the old toybox went into
# the initrd with symlinks for applets it did not contain.
rm -f "$BUILDDIR/toybox" "$BUILDDIR/generated/unstripped/toybox"
make_status=0
bash scripts/make.sh || make_status=$?

# make.sh strips with -s which removes .symtab. The ulinker needs "main" in
# .symtab to exec the binary. Re-strip keeping symbols (debug sections only).
if [ -f "$BUILDDIR/generated/unstripped/toybox" ]; then
    strip -S -o "$BUILDDIR/toybox" "$BUILDDIR/generated/unstripped/toybox"
    chmod 555 "$BUILDDIR/toybox"
fi

# Link the binary ourselves — vanilla make.sh may or may not have linked
if [ ! -x "$BUILDDIR/toybox" ] || file "$BUILDDIR/toybox" | grep -q "text"; then
    rm -f "$BUILDDIR/toybox"
    LINK_LIBS="-lm -lz"
    STRIP_FLAG=""
    if [ "$(uname)" = "Darwin" ] && [ -z "$TVM_IOS_CROSS" ]; then
        LINK_LIBS="$LINK_LIBS -lcrypto -lresolv -lssl -L$OPENSSL_PREFIX/lib"
    fi
    # -dead_strip is an Apple ld flag; GNU ld rejects it. Use --gc-sections there.
    if [ "$(uname)" = "Darwin" ]; then
        STRIP_FLAG="-Wl,-dead_strip"
    else
        STRIP_FLAG="-Wl,--gc-sections"
    fi
    $CC -g $STRIP_FLAG generated/obj/*.o $LINK_LIBS -o "$BUILDDIR/toybox"
fi

if [ ! -x "$BUILDDIR/toybox" ]; then
    echo "build_toybox.sh: no toybox binary produced (make.sh exit status $make_status)" >&2
    exit 1
fi
