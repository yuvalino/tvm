#!/bin/sh
set -e
SRCDIR="$1"
OUTDIR="$2"

FLUTTER="${FLUTTER_BIN:-}"
if [ -z "$FLUTTER" ]; then
    for f in "$HOME/tools/flutter/bin/flutter" "flutter"; do
        if command -v "$f" >/dev/null 2>&1; then FLUTTER="$f"; break; fi
    done
fi
[ -n "$FLUTTER" ] || { echo "error: flutter not found" >&2; exit 1; }

cd "$SRCDIR"
"$FLUTTER" build linux

mkdir -p "$OUTDIR"
ln -sfn "$(realpath build/linux/x64/release/bundle)" "$OUTDIR/flutter"
