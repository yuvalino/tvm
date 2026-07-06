#!/bin/sh
set -e
SRCDIR="$1"
cd "$SRCDIR"
if [ ! -f Rebuild.sh ]; then
    CFLAGS="-g -gdwarf-4 -fcommon -DMKSH_FORKLESS=1" sh ./Build.sh
fi
sh ./Rebuild.sh
${CC:-cc} -g -gdwarf-4 -o mksh ./*.o
