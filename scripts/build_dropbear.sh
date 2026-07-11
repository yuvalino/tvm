#!/bin/sh
set -e
SRCDIR="$1"
BUILDDIR="$2"

rsync -a --exclude='*.o' --exclude='*.a' --exclude=obj/ \
    --exclude=Makefile --exclude=config.h --exclude=config.log --exclude=config.status \
    "$SRCDIR/" "$BUILDDIR/"

cd "$BUILDDIR"

CROSS_FLAGS=""
if [ -n "$TVM_IOS_CROSS" ]; then
    CROSS_FLAGS="--host=aarch64-apple-darwin"
fi

if [ ! -f Makefile ]; then
    ./configure CFLAGS="-g -gdwarf-4" ac_cv_type_socklen_t=yes ac_cv_type_uid_t=yes $CROSS_FLAGS
fi
make PROGRAMS="dropbear dbclient dropbearkey scp" MULTI=1 \
    CFLAGS="-g -gdwarf-4 -DDROPBEAR_FORKLESS" MAKEOVERRIDES=
