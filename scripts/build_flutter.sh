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
        if [ "${TVM_IOS_CROSS:-}" = "1" ]; then
            "$FLUTTER" build ios --release

            mkdir -p "$OUTDIR"
            ln -sfn "$(cd build/ios/iphoneos && pwd)/Runner.app" "$OUTDIR/flutter"
        else
            "$FLUTTER" config --enable-macos-desktop 2>/dev/null || true
            "$FLUTTER" build macos --release

            mkdir -p "$OUTDIR"
            ln -sfn "$(cd build/macos/Build/Products/Release && pwd)/tvm.app" "$OUTDIR/flutter"
        fi
        ;;
    Linux)
        "$FLUTTER" build linux --release

        mkdir -p "$OUTDIR"
        ln -sfn "$(cd build/linux/x64/release/bundle && pwd)" "$OUTDIR/flutter"
        ;;
    *)
        echo "error: unsupported platform $(uname)" >&2
        exit 1
        ;;
esac
