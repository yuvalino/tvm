#!/bin/sh
set -e
SRCDIR="$1"
cd "$SRCDIR"
if [ ! -f Rebuild.sh ]; then
    CFLAGS="-g -gdwarf-4 -fcommon -DMKSH_FORKLESS=1" sh ./Build.sh
fi
sh ./Rebuild.sh
cc -g -gdwarf-4 -o mksh ./*.o

# static lib
ar rcs libmksh.a ./*.o
# shared lib
cc -dynamiclib -fPIC -g -o libmksh.dylib ./*.o -install_name libmksh.dylib 2>/dev/null || true
