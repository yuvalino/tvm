#!/bin/sh
set -e
SRCDIR="$1"
OUTDIR="$2"
PROJROOT="$(cd "$SRCDIR/.." && pwd)"

FLUTTER="${FLUTTER_BIN:-}"
if [ -z "$FLUTTER" ]; then
    for f in "$HOME/tools/flutter/bin/flutter" "$HOME/flutter/bin/flutter" "flutter"; do
        if command -v "$f" >/dev/null 2>&1; then FLUTTER="$f"; break; fi
    done
fi
[ -n "$FLUTTER" ] || { echo "error: flutter not found" >&2; exit 1; }

case "$(uname)" in
    Darwin)
        PLATFORM="macos-arm64"
        TVM_OBJ="$OUTDIR/build/tvm/CMakeFiles/tvm_objects.dir"
        LIBS="$OUTDIR/lib"

        # Build libtvm.dylib if not present
        if [ ! -f "$OUTDIR/libtvm.dylib" ]; then
            echo "Building libtvm.dylib..."
            cc -dynamiclib -fPIC \
                -mmacosx-version-min=26.0 \
                -install_name @rpath/libtvm.dylib \
                -o "$OUTDIR/libtvm.dylib" \
                "$SRCDIR/native/devtool_main_macos.c" \
                -I "$SRCDIR/native" -I "$PROJROOT/tvm" \
                "$TVM_OBJ/tvm.c.o" "$TVM_OBJ/rmem.c.o" "$TVM_OBJ/ulinker.c.o" \
                "$LIBS/libunicorn-static.a" "$LIBS/libunicorn-common.a" \
                "$LIBS/libx86_64-softmmu.a" "$LIBS/libaarch64-softmmu.a" \
                -lz -lpthread -lm -lncurses \
                -Wl,-sectcreate,__DATA,__initrd,"$OUTDIR/initrd.cpio"
        fi

        cd "$SRCDIR"
        "$FLUTTER" config --enable-macos-desktop 2>/dev/null || true
        "$FLUTTER" build macos --debug

        mkdir -p "$OUTDIR"
        ln -sfn "$(cd build/macos/Build/Products/Debug && pwd)/tvm.app" "$OUTDIR/flutter"
        ;;
    Linux)
        cd "$SRCDIR"
        "$FLUTTER" build linux

        mkdir -p "$OUTDIR"
        ln -sfn "$(cd build/linux/x64/release/bundle && pwd)" "$OUTDIR/flutter"
        ;;
    *)
        echo "error: unsupported platform $(uname)" >&2
        exit 1
        ;;
esac
