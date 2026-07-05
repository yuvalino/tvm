#!/bin/sh
set -e
SRCDIR="$1"
OUTDIR="$2"

FLUTTER="${FLUTTER_BIN:-}"
if [ -z "$FLUTTER" ]; then
    for f in "$HOME/tools/flutter/bin/flutter" "$HOME/flutter/bin/flutter" "flutter"; do
        if command -v "$f" >/dev/null 2>&1; then FLUTTER="$f"; break; fi
    done
fi
[ -n "$FLUTTER" ] || { echo "error: flutter not found" >&2; exit 1; }

cd "$SRCDIR"

case "$(uname)" in
    Darwin)
        "$FLUTTER" config --enable-macos-desktop 2>/dev/null || true
        "$FLUTTER" build macos --debug

        mkdir -p "$OUTDIR"
        ln -sfn "$(cd build/macos/Build/Products/Debug && pwd)/tvm.app" "$OUTDIR/flutter"
        ;;
    Linux)
        "$FLUTTER" build linux

        mkdir -p "$OUTDIR"
        ln -sfn "$(cd build/linux/x64/release/bundle && pwd)" "$OUTDIR/flutter"
        ;;
    *)
        echo "error: unsupported platform $(uname)" >&2
        exit 1
        ;;
esac
