#!/bin/bash
# Build individual commands from one apple-oss-distributions *_cmds repo.
#
# Each command is compiled and linked as its own standalone executable.
# Output binaries go into <build-dir>/bin/.
#
# Usage: build_apple_cmds.sh <repo-src> <build-dir> <manifest>
#
# Manifest: '|'-separated lines — <cmd>|<subdir>|<sources>|<extra-cflags>|<platform>
# Directives: #common, #cflags, #libs (module-wide). See *.cmds files.
set -e

SRCDIR="$(cd "$1" && pwd)"
BUILDDIR="$(mkdir -p "$2" && cd "$2" && pwd)"
MANIFEST="$3"

[ -d "$SRCDIR" ] && [ -f "$MANIFEST" ] || {
    echo "usage: build_apple_cmds.sh <repo-src> <build-dir> <manifest>" >&2; exit 1
}

CC="${CC:-cc}"
APPLE_DIR="$(cd "$(dirname "$0")/../third_party/apple" && pwd)"
BINDIR="$BUILDDIR/bin"
mkdir -p "$BINDIR"

# ── Platform / header setup ──────────────────────────────────────────────
GEN_INC="$BUILDDIR/gen_include"
mkdir -p "$GEN_INC"

if [ "${TVM_IOS_CROSS:-}" = "1" ]; then
    LD="xcrun --sdk iphoneos ld -arch arm64"
    MACINC="$(xcrun --sdk macosx --show-sdk-path)/usr/include"
    SDK_FALLBACK="-idirafter $MACINC"
    TARGET_PLATFORM=ios
    mkdir -p "$GEN_INC/mach"
    printf '#include "%s/mach/mach_vm.h"\n' "$MACINC" > "$GEN_INC/mach/mach_vm.h"
else
    SDK_FALLBACK=""
    TARGET_PLATFORM=macos
fi

APPLE_INCLUDES="-I$GEN_INC -I$APPLE_DIR/include-override"
APPLE_FALLBACK="-idirafter $APPLE_DIR/include -idirafter $APPLE_DIR/include-shim $SDK_FALLBACK"

LEGACY_C_FLAGS="-Wno-error=int-conversion -Wno-error=implicit-function-declaration \
-Wno-error=incompatible-function-pointer-types -Wno-error=incompatible-pointer-types \
-Wno-error=return-type -Wno-error=deprecated-non-prototype"

BASE_CFLAGS="-g -gdwarf-4 -fno-common -w -DPRIVATE=1 $LEGACY_C_FLAGS $APPLE_INCLUDES $APPLE_FALLBACK"

# ── Build each command ───────────────────────────────────────────────────
COMMON_SRCS=""
MODULE_CFLAGS=""
MODULE_LIBS=""
N=0

while IFS='|' read -r cmd subdir sources cflags platforms; do
    case "$cmd" in
        '#common') COMMON_SRCS="$subdir"; continue ;;
        '#cflags') MODULE_CFLAGS="$subdir"; continue ;;
        '#libs')   MODULE_LIBS="$subdir"; continue ;;
        ''|'#'*) continue ;;
    esac

    platforms="${platforms%%[[:space:]]*}"
    if [ -n "$platforms" ] && [ "$platforms" != "$TARGET_PLATFORM" ]; then
        continue
    fi

    cmddir="$SRCDIR/$subdir"

    if [ -z "$sources" ]; then
        srcfiles="$cmddir"/*.c
    else
        srcfiles=""
        for s in $sources; do srcfiles="$srcfiles $cmddir/$s"; done
    fi
    for s in $COMMON_SRCS; do srcfiles="$srcfiles $SRCDIR/$s"; done

    mod_cflags="${MODULE_CFLAGS//@SRC@/$SRCDIR}"; mod_cflags="${mod_cflags//@APPLE@/$APPLE_DIR}"
    cmd_cflags="${cflags//@SRC@/$SRCDIR}"; cmd_cflags="${cmd_cflags//@APPLE@/$APPLE_DIR}"

    # shellcheck disable=SC2086
    $CC $BASE_CFLAGS $mod_cflags -I"$cmddir" $cmd_cflags \
        -o "$BINDIR/$cmd" $srcfiles $MODULE_LIBS
    N=$((N + 1))
done < "$MANIFEST"

echo "  built $N commands -> $BINDIR"
