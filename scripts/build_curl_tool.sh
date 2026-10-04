#!/bin/sh
# Relink the curl tool for TVM.
#
# curl's CMake compiles the tool objects (src/CMakeFiles/curl.dir/**/*.o,
# including the curlx helpers under __/lib/curlx/) and links its own `curl`
# executable. TVM needs main() wrapped (see third_party/curl_main.c), so this
# script renames curl's main to curl_tool_main in tool_main.c.o at link time
# and relinks the objects with the wrapper into the `curl` executable (the
# Flutter apps get it converted to libcurl.dylib, third_party/CMakeLists.txt).
#
# usage: build_curl_tool.sh <curl-build-dir> <openssl-prefix> <workdir> <output>
# env:   CC (the target compiler, e.g. ios-cc.sh when cross-compiling),
#        TVM_IOS_CROSS=1 when building for iOS
set -e
CURL_BUILD="$1"
OPENSSL_PREFIX="$2"
WORKDIR="$3"
OUT="$4"
CC="${CC:-cc}"

WRAP_SRC="$(cd "$(dirname "$0")/.." && pwd)/third_party/curl_main.c"
OBJDIR="$CURL_BUILD/src/CMakeFiles/curl.dir"
TOOL_MAIN="$OBJDIR/tool_main.c.o"
[ -f "$TOOL_MAIN" ] || { echo "error: $TOOL_MAIN not found (curl not built?)" >&2; exit 1; }

mkdir -p "$WORKDIR"

# The wrapper's main(); OPENSSL_cleanup() comes from OpenSSL's headers.
"$CC" -g -gdwarf-4 -I"$OPENSSL_PREFIX/include" -c "$WRAP_SRC" -o "$WORKDIR/curl_main.o"

# Rename curl's main -> curl_tool_main without touching the sources.
case "$(uname)" in
    Darwin)
        # ld -r: add _curl_tool_main as an alias of _main, then make _main
        # private so the wrapper's _main is the only global one.
        "$CC" -nostdlib -r \
            -Wl,-alias,_main,_curl_tool_main -Wl,-unexported_symbol,_main \
            "$TOOL_MAIN" -o "$WORKDIR/tool_main_wrapped.o"
        LIBS="-lz -framework Security -framework CoreFoundation -framework CoreServices"
        # curl's CMake links SystemConfiguration on macOS only (lib/macos.c
        # SCDynamicStoreCopyProxies); iOS does not compile that call.
        if [ -z "$TVM_IOS_CROSS" ]; then
            LIBS="$LIBS -framework SystemConfiguration"
        fi
        ;;
    *)
        objcopy --redefine-sym main=curl_tool_main "$TOOL_MAIN" "$WORKDIR/tool_main_wrapped.o"
        LIBS="-lz -lpthread -ldl"
        ;;
esac

OBJS="$(find "$OBJDIR" -name '*.o' ! -name 'tool_main.c.o' | sort)"
STATIC="$CURL_BUILD/lib/libcurl.a $OPENSSL_PREFIX/lib/libssl.a $OPENSSL_PREFIX/lib/libcrypto.a"

"$CC" -g -o "$OUT" \
    "$WORKDIR/curl_main.o" "$WORKDIR/tool_main_wrapped.o" $OBJS $STATIC $LIBS
