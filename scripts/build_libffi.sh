#!/bin/sh
# Build libffi for TVM: a static libffi.a for Python's ctypes (unpatched
# upstream, configured with its own autotools in a shadow tree).
#
# usage: build_libffi.sh <srcdir> <builddir>
# env:   CC (the target compiler, e.g. ios-cc.sh when cross-compiling),
#        TVM_IOS_CROSS=1 when building for iOS
#
# The library and headers are installed into <builddir>/prefix.
#
# The git tree ships no configure script: autogen.sh (autoreconf -i) makes
# it. That runs in a copy of the sources, so the submodule stays clean.
#
# Closures on arm64 Apple targets use libffi's trampoline table
# (FFI_EXEC_TRAMPOLINE_TABLE, chosen by configure for aarch64-apple-*):
# prebuilt trampolines remapped from libffi's own code pages, never memory
# made executable at run time, which iOS does not allow.
set -e
SRCDIR="$1"
BUILDDIR="$2"
. "$(cd "$(dirname "$0")" && pwd)/tvm_stamp.sh"
tvm_stamp_begin "$BUILDDIR" "$0" "$SRCDIR"
CC="${CC:-cc}"

SHADOW="$BUILDDIR/src"
PREFIX="$BUILDDIR/prefix"

if [ ! -f "$SHADOW/configure" ]; then
    rm -rf "$SHADOW"
    mkdir -p "$SHADOW"
    (cd "$SRCDIR" && tar -cf - --exclude .git .) | (cd "$SHADOW" && tar -xf -)
    # Homebrew installs GNU libtool's libtoolize as glibtoolize.
    if ! command -v libtoolize >/dev/null 2>&1 && command -v glibtoolize >/dev/null 2>&1; then
        export LIBTOOLIZE=glibtoolize
    fi
    (cd "$SHADOW" && autoreconf -i)
fi

HOST_ARGS=""
if [ -n "$TVM_IOS_CROSS" ]; then
    HOST_ARGS="--host=aarch64-apple-darwin"
fi

mkdir -p "$BUILDDIR/build"
cd "$BUILDDIR/build"
if [ ! -f Makefile ]; then
    CC="$CC" CFLAGS="-g -gdwarf-4 -O2" \
    "$SHADOW/configure" $HOST_ARGS --prefix="$PREFIX" \
        --disable-shared --enable-static --with-pic \
        --disable-docs --disable-multi-os-directory
fi
make
make install
