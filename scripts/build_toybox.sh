#!/bin/sh
set -e
SRCDIR="$1"
cd "$SRCDIR"

if [ "$(uname)" = "Darwin" ]; then
    export PATH="/opt/homebrew/opt/gnu-sed/libexec/gnubin:$PATH"
    OPENSSL_PREFIX="$(brew --prefix openssl 2>/dev/null || echo /opt/homebrew/opt/openssl)"
    CFLAGS_EXTRA="-I$OPENSSL_PREFIX/include"
fi

HOSTCC="${CC:-cc}"
make CC="$HOSTCC" CFLAGS="-g -gdwarf-4 -DTOYBOX_FORKLESS=1 $CFLAGS_EXTRA" V=1 MAKEOVERRIDES= || true
