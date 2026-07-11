#!/bin/sh
set -e
SRCDIR="$1"
BUILDDIR="$2"

mkdir -p "$BUILDDIR"
cd "$BUILDDIR"

if [ ! -f Rebuild.sh ]; then
    CFLAGS="-g -gdwarf-4 -fcommon -DMKSH_FORKLESS=1" sh "$SRCDIR/Build.sh"
fi
sh ./Rebuild.sh
${CC:-cc} -g -gdwarf-4 -o mksh ./*.o
