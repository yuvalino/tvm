#!/bin/sh
set -e
SRCDIR="$1"
cd "$SRCDIR"
if [ "$(uname)" = "Darwin" ]; then
    export PATH="/opt/homebrew/opt/gnu-sed/libexec/gnubin:$PATH"
    OPENSSL_PREFIX="$(brew --prefix openssl 2>/dev/null || echo /opt/homebrew/opt/openssl)"
fi
make CFLAGS="-g -gdwarf-4 -DTOYBOX_FORKLESS=1 ${OPENSSL_PREFIX:+-I$OPENSSL_PREFIX/include}" V=1 MAKEOVERRIDES=
rm -f toybox
if [ "$(uname)" = "Darwin" ]; then
    cc -g -gdwarf-4 generated/obj/*.o \
        -Wl,-dead_strip -L"$OPENSSL_PREFIX/lib" -lcrypto -lm -lresolv -lssl -lz -o toybox
else
    cc -g -gdwarf-4 generated/obj/*.o -Wl,--gc-sections -Wl,--as-needed \
        -lcrypt -lcrypto -lm -lresolv -lssl -lutil -lz -o toybox
fi
