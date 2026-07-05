#!/bin/sh
set -e
SRCDIR="$1"
cd "$SRCDIR"

LDFLAGS="-g -gdwarf-4"
LIBS="-lcrypto -lm -lresolv -lssl -lz"

if [ "$(uname)" = "Darwin" ]; then
    export PATH="/opt/homebrew/opt/gnu-sed/libexec/gnubin:$PATH"
    OPENSSL_PREFIX="$(brew --prefix openssl 2>/dev/null || echo /opt/homebrew/opt/openssl)"
    CFLAGS_EXTRA="-I$OPENSSL_PREFIX/include"
    LDFLAGS="$LDFLAGS -Wl,-dead_strip -L$OPENSSL_PREFIX/lib"
else
    LIBS="$LIBS -lcrypt -lutil"
    LDFLAGS="$LDFLAGS -Wl,--gc-sections -Wl,--as-needed"
fi

make CFLAGS="-g -gdwarf-4 -DTOYBOX_FORKLESS=1 $CFLAGS_EXTRA" V=1 MAKEOVERRIDES= || true
rm -f toybox
cc $LDFLAGS generated/obj/*.o $LIBS -o toybox

# static lib
ar rcs libtoybox.a generated/obj/*.o
# shared lib
cc -dynamiclib -fPIC $LDFLAGS generated/obj/*.o $LIBS -o libtoybox.dylib -install_name libtoybox.dylib 2>/dev/null || true
