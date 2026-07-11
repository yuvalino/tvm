#!/bin/bash
# Out-of-tree build for toybox via source-tree symlinks.
# Usage: build_toybox.sh <source-dir> <build-dir>
set -e

SRCDIR="$(cd "$1" && pwd)"
BUILDDIR="$(mkdir -p "$2" && cd "$2" && pwd)"

# Symlink source tree into build dir (read-only references)
for item in Config.in Makefile configure kconfig lib main.c scripts toys toys.h; do
    ln -sfn "$SRCDIR/$item" "$BUILDDIR/$item"
done

# .config is per-build
if [ ! -f "$BUILDDIR/.config" ]; then
    cp "$SRCDIR/.config" "$BUILDDIR/.config"
fi

mkdir -p "$BUILDDIR/generated"

cd "$BUILDDIR"

if [ "$(uname)" = "Darwin" ] && [ -z "$TVM_IOS_CROSS" ]; then
    export PATH="/opt/homebrew/opt/gnu-sed/libexec/gnubin:$PATH"
    OPENSSL_PREFIX="$(brew --prefix openssl 2>/dev/null || echo /opt/homebrew/opt/openssl)"
    CFLAGS_EXTRA="-I$OPENSSL_PREFIX/include"
elif [ "$(uname)" = "Darwin" ]; then
    export PATH="/opt/homebrew/opt/gnu-sed/libexec/gnubin:$PATH"
fi

export CC="${CC:-cc}"
export HOSTCC="${HOSTCC:-cc}"
export CFLAGS="-g -gdwarf-4 -DTOYBOX_FORKLESS=1 ${CFLAGS_EXTRA}"
export V=1

bash scripts/make.sh || true

# Link the binary ourselves — toybox's modified make.sh doesn't link
if [ ! -x "$BUILDDIR/toybox" ] || file "$BUILDDIR/toybox" | grep -q "text"; then
    rm -f "$BUILDDIR/toybox"
    LINK_LIBS="-lm -lz"
    if [ "$(uname)" = "Darwin" ] && [ -z "$TVM_IOS_CROSS" ]; then
        LINK_LIBS="$LINK_LIBS -lcrypto -lresolv -lssl -L$OPENSSL_PREFIX/lib"
    fi
    $CC -g -Wl,-dead_strip generated/obj/*.o $LINK_LIBS -o "$BUILDDIR/toybox"
fi
