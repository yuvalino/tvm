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

# Android is a cross build from any host (Linux/macOS) — select it explicitly
# rather than by uname. build.py sets TVM_ANDROID_FLUTTER=1 for -a android-arm64.
if [ "${TVM_ANDROID_FLUTTER:-}" = "1" ]; then
    ABI=arm64-v8a
    JNI="$SRCDIR/android/app/src/main/jniLibs/$ABI"
    mkdir -p "$JNI"
    # Stage all prebuilt shared libs (built by the root CMake with the NDK):
    # libtvm + sidecars + the FFI glue libtvm_bindings.so. gradle just packages
    # them, mirroring how Xcode's "Copy TVM" phase consumes prebuilt dylibs.
    for lib in libtvm libtvm_bindings libdropbear libmksh libtoybox libvim; do
        cp -f "$OUTDIR/$lib.so" "$JNI/$lib.so"
    done

    "$FLUTTER" build apk --release

    APK="$SRCDIR/build/app/outputs/flutter-apk/app-release.apk"
    [ -f "$APK" ] || { echo "error: apk not found at $APK" >&2; exit 1; }
    mkdir -p "$OUTDIR/flutter"
    ln -sfn "$(cd "$(dirname "$APK")" && pwd)/$(basename "$APK")" "$OUTDIR/flutter/app.apk"
    exit 0
fi

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
    *)
        echo "error: unsupported platform $(uname) (macOS/iOS only)" >&2
        exit 1
        ;;
esac
