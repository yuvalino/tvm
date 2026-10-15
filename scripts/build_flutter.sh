#!/bin/sh
set -e
SRCDIR="$1"

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
            if [ "${TVM_NO_CODESIGN:-}" = "1" ]; then
                # an earlier signed build's profile and signature would stay
                # in the bundle otherwise (Xcode's product, and flutter's copy)
                rm -rf build/ios/Release-iphoneos/Runner.app build/ios/iphoneos/Runner.app
                "$FLUTTER" build ios --release --no-codesign
            else
                "$FLUTTER" build ios --release
            fi
        else
            "$FLUTTER" config --enable-macos-desktop 2>/dev/null || true
            "$FLUTTER" build macos --release
        fi
        ;;
    *)
        echo "error: unsupported platform $(uname) (macOS/iOS only)" >&2
        exit 1
        ;;
esac
