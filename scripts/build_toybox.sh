#!/bin/sh
set -e
SRCDIR="$1"
cd "$SRCDIR"
make CFLAGS="-g -gdwarf-4 -DTOYBOX_FORKLESS=1" V=1 MAKEOVERRIDES=
if [ "$(uname)" = "Darwin" ]; then
    rm -f toybox
    cc -g -gdwarf-4 generated/obj/*.o \
        -Wl,-dead_strip -lcrypto -lm -lresolv -lssl -lz -o toybox
else
    cc -g -gdwarf-4 generated/obj/*.o -Wl,--gc-sections -Wl,--as-needed \
        -lcrypt -lcrypto -lm -lresolv -lssl -lutil -lz -o toybox
fi
