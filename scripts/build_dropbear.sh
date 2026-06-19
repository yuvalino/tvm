#!/bin/sh
set -e
SRCDIR="$1"
cd "$SRCDIR"
if [ ! -f Makefile ]; then
    ./configure CFLAGS="-g -gdwarf-4" ac_cv_type_socklen_t=yes ac_cv_type_uid_t=yes
fi
make PROGRAMS="dropbear dbclient dropbearkey scp" MULTI=1 \
    CFLAGS="-g -gdwarf-4 -DDROPBEAR_FORKLESS" MAKEOVERRIDES=
