#!/bin/sh
# Build clang and lld for TVM, the C/C++ runtimes their programs link, and
# the toolchain bundle a TVM guest compiles against (unpatched upstream).
#
# usage: build_llvm.sh <srcdir> <builddir> [<host-bindir>]
# env:   TVM_IOS_CROSS=1 when building for iOS
#
# The toolchain TVM runs is compiled by a clang of the same version, against
# the libc++ built here, linked in statically. Built against the system's
# libc++.dylib (as Xcode's compiler would build it), its operator new would be
# the host's, and clang's whole heap the host's malloc, which a task's exit
# does not free: every compile leaked its memory into the process (9.5 MB a
# small C++ file, measured), and a fork's child shared the parent's live
# objects. The same goes for its locks and waits. Linked with this libc++,
# its allocations are malloc()s from the task's heap, and the rest goes
# through TVM's functions too. So there are two builds of LLVM:
#
#   llvm/      (macOS only) the build machine's: clang and lld compiled by
#              Xcode (whose clang cannot compile this libc++), which compile
#              everything below, and the tablegen programs
#   target/    the toolchain TVM runs, compiled by the build machine's clang
#
# <host-bindir> (iOS only) is the macOS build's llvm/bin
# (output/macos-arm64/build/third_party/llvm/llvm/bin), which plays that part
# for the iOS build.
#
# Output, under <builddir>:
#   target/bin/llvm    the toolchain, one multicall program (LLVM's
#                      LLVM_TOOL_LLVM_DRIVER_BUILD): clang, clang++, ld64.lld,
#                      llvm-ar & co. are names of it, chosen by argv[0]
#                      (converted to libllvm.framework for the Flutter apps)
#   bundle/            what the guest sees at /tvm/llvm (bound there by
#                      init.rc / init.flutter.rc; the apps carry it in their
#                      resources, outside the initrd, which is read into
#                      memory whole):
#     lib/clang/<N>/   clang's resource directory: its own headers, and the
#                      compiler-rt builtins (lib/darwin/libclang_rt.<os>.a)
#     sdk/             the SDK, trimmed to what C and C++ need: usr/include,
#                      the usr/lib stubs and the SDK settings, with libc++
#                      (headers and libc++.1.dylib) built here in place of
#                      the SDK's
#
# The guest's clang finds the bundle through the configuration files in
# /tvm/etc/clang (CLANG_CONFIG_FILE_SYSTEM_DIR; third_party/configs/clang.cfg):
# the resource directory (the default, next to the executable, is the app's
# Frameworks directory in the Flutter apps), the SDK, lld, and a deployment
# target old enough for the device's libSystem. On the build machine there is
# no /tvm/etc/clang, and the same program is a stock clang.
#
# Programs compiled under TVM link this build's libc++, a dylib with
# libc++abi and libunwind in it, installed as
# /tvm/llvm/sdk/usr/lib/libc++.1.dylib: TVM loads it with the program, as
# the program's own image. A program compiled on the device is not signed,
# so TVM emulates it, and C++ exceptions must be unwound by a libunwind that
# walks the emulated frames (the system's, native, cannot); this one finds
# the images' unwind tables through TVM's _dyld_find_unwind_sections. Not a
# static libc++.a: a Mach-O program runs its static initializers in link
# order, which put a program's own before libc++'s (init_priority has no
# meaning in Mach-O), so a static constructor writing to std::cout found it
# not yet constructed; a dylib dependency is initialized first, as the
# system's libc++ is. The SDK keeps no libc++.tbd, so -lc++ (which the
# driver passes) finds this one.
#
# The driver runs the linker with posix_spawn, which TVM implements: a fork
# would not do, as LLVM frees the child's argv right after it (see TVM's
# posix_spawn).
#
# Unlike the other third-party builds (scripts/tvm_stamp.sh), an edit of this
# script does not start everything over, which for LLVM is the better part of
# an hour: each CMake build tree starts over when its configure command
# changes, and is otherwise rebuilt by ninja, which follows the sources.
set -e
SRCDIR="$1"
BUILDDIR="$2"
HOSTBIN="$3"
NPROC="$(sysctl -n hw.ncpu 2>/dev/null || nproc)"

# configure DIR CMAKE-ARG...: configure the build tree DIR, from scratch
# unless it was configured, successfully, with these arguments
# (DIR/.tvm-configure, written when cmake succeeds; a failed configure leaves
# a CMakeCache.txt behind, whose values must not carry over).
configure() {
    _dir="$1"; shift
    _sig="$(printf '%s\n' "$@")"
    if [ -f "$_dir/build.ninja" ] && [ -f "$_dir/.tvm-configure" ] &&
       [ "$(cat "$_dir/.tvm-configure")" = "$_sig" ]; then
        return 0
    fi
    if [ -e "$_dir" ]; then
        echo "build_llvm: $_dir not configured with these arguments, starting it over" >&2
        rm -rf "$_dir"
    fi
    mkdir -p "$_dir"
    (cd "$_dir" && cmake -G Ninja -B . "$@")
    printf '%s' "$_sig" > "$_dir/.tvm-configure"
}

LLVM_MAJOR="$(sed -n 's/^ *set(LLVM_VERSION_MAJOR \([0-9]*\)).*/\1/p' "$SRCDIR/cmake/Modules/LLVMVersion.cmake")"
if [ -z "$LLVM_MAJOR" ]; then
    echo "error: no LLVM_VERSION_MAJOR in $SRCDIR/cmake/Modules/LLVMVersion.cmake" >&2
    exit 1
fi

if [ -n "$TVM_IOS_CROSS" ]; then
    if [ ! -x "$HOSTBIN/llvm" ] || [ ! -x "$HOSTBIN/llvm-tblgen" ]; then
        echo "error: the iOS build needs the macOS one's tools in '$HOSTBIN'" >&2
        echo "       (python3 build.py -a macos-arm64 -t llvm_exe)" >&2
        exit 1
    fi
    SDK="$(xcrun --sdk iphoneos --show-sdk-path)"
    OS=ios
    MIN_VERSION=15.0
    MIN_VERSION_FLAG=-miphoneos-version-min=$MIN_VERSION
    PLATFORM_ARGS="-DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_ARCHITECTURES=arm64
        -DCMAKE_OSX_DEPLOYMENT_TARGET=$MIN_VERSION"
else
    SDK="$(xcrun --sdk macosx --show-sdk-path)"
    OS=osx
    # lld writes chained fixups (which TVM's linker reads) from macOS 13 on;
    # older targets get rebase/bind opcodes, which it does not
    MIN_VERSION=13.0
    MIN_VERSION_FLAG=-mmacosx-version-min=$MIN_VERSION
    PLATFORM_ARGS="-DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_OSX_DEPLOYMENT_TARGET=$MIN_VERSION"
fi

# What both builds of LLVM are: clang and lld as one multicall program,
# AArch64 only, no plugins. Build machine libraries stay out (Homebrew's
# zstd, libxml2, libedit would be linked in); zlib is the SDK's. One link at
# a time: a static clang link takes several GB.
LLVM_ARGS="-DCMAKE_BUILD_TYPE=Release
        -DLLVM_ENABLE_PROJECTS=clang;lld
        -DLLVM_TARGETS_TO_BUILD=AArch64
        -DLLVM_TOOL_LLVM_DRIVER_BUILD=ON
        -DLLVM_ENABLE_ASSERTIONS=OFF
        -DLLVM_ENABLE_PLUGINS=OFF -DCLANG_PLUGIN_SUPPORT=OFF
        -DLLVM_ENABLE_ZSTD=OFF -DLLVM_ENABLE_LIBXML2=OFF -DLLVM_ENABLE_LIBEDIT=OFF
        -DLLVM_ENABLE_LIBPFM=OFF -DLLVM_ENABLE_CURL=OFF -DLLVM_ENABLE_HTTPLIB=OFF
        -DLLVM_ENABLE_BINDINGS=OFF
        -DLLVM_INCLUDE_TESTS=OFF -DLLVM_INCLUDE_EXAMPLES=OFF
        -DLLVM_INCLUDE_BENCHMARKS=OFF -DLLVM_INCLUDE_DOCS=OFF
        -DCLANG_INCLUDE_TESTS=OFF -DCLANG_INCLUDE_DOCS=OFF
        -DCLANG_ENABLE_STATIC_ANALYZER=OFF
        -DCLANG_CONFIG_FILE_SYSTEM_DIR=/tvm/etc/clang
        -DLLVM_PARALLEL_LINK_JOBS=1"

# TVM starts a program at its exported main (see exe2lib.py).
check_exports_main() {
    if ! nm -g "$1" | grep -Eq ' T _?main$'; then
        echo "error: $1 does not export main" >&2
        exit 1
    fi
}

# ── 1. the build machine's clang and lld (macOS) ──
if [ -z "$TVM_IOS_CROSS" ]; then
    # shellcheck disable=SC2086
    configure "$BUILDDIR/llvm" -S "$SRCDIR/llvm" \
            -DCMAKE_BUILD_TYPE=Release \
            -DCMAKE_C_COMPILER=cc -DCMAKE_CXX_COMPILER=c++ \
            -DCMAKE_OSX_SYSROOT="$SDK" $PLATFORM_ARGS \
            $(echo "$LLVM_ARGS" | sed 1d)
    cd "$BUILDDIR/llvm"
    ninja -j"$NPROC" llvm-driver
    # the names the builds below call it by
    for tool in clang clang++ ld64.lld llvm-ar llvm-ranlib llvm-nm; do
        ln -sf llvm "bin/$tool"
    done
    TOOLBIN="$BUILDDIR/llvm/bin"
else
    TOOLBIN="$HOSTBIN"
fi

# ── 2. libc++, libc++abi and libunwind ──
# libc++.a, with libc++abi and libunwind merged in, and libc++.1.dylib (see
# above), linked here from all of it. Built with the clang of this version:
# libc++ supports only the latest compilers. (Not by the runtimes' own shared
# library build: for CMAKE_SYSTEM_NAME iOS, LLVM's HandleLLVMOptions.cmake
# adds the ELF linker's -z defs to every shared library link, which ld
# rejects.)
# (The compiler's size and time stand for its version in the signature:
# ninja does not rebuild for a new compiler.)
TOOLSIG="$(stat -f '%m %z' "$TOOLBIN/llvm" 2>/dev/null || stat -c '%Y %s' "$TOOLBIN/llvm")"
# shellcheck disable=SC2086
configure "$BUILDDIR/runtimes" -S "$SRCDIR/runtimes" -DTVM_COMPILER="$TOOLSIG" \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_C_COMPILER="$TOOLBIN/clang" -DCMAKE_CXX_COMPILER="$TOOLBIN/clang++" \
        -DCMAKE_OSX_SYSROOT="$SDK" $PLATFORM_ARGS \
        -DCMAKE_INSTALL_PREFIX="$BUILDDIR/runtimes/install" \
        -DLLVM_ENABLE_RUNTIMES="libcxx;libcxxabi;libunwind" \
        -DLIBCXX_ENABLE_SHARED=OFF -DLIBCXXABI_ENABLE_SHARED=OFF -DLIBUNWIND_ENABLE_SHARED=OFF \
        -DLIBCXX_ENABLE_STATIC_ABI_LIBRARY=ON \
        -DLIBCXXABI_USE_LLVM_UNWINDER=ON -DLIBCXXABI_ENABLE_STATIC_UNWINDER=ON \
        -DLLVM_INCLUDE_TESTS=OFF -DLIBCXX_INCLUDE_TESTS=OFF -DLIBCXX_INCLUDE_BENCHMARKS=OFF \
        -DLIBCXXABI_INCLUDE_TESTS=OFF -DLIBUNWIND_INCLUDE_TESTS=OFF
cd "$BUILDDIR/runtimes"
ninja -j"$NPROC"
# not emptied first: the toolchain below is compiled against these headers,
# and new copies of them (new times) made ninja rebuild all of it every run;
# install leaves a file that is up to date alone
ninja install
"$TOOLBIN/clang++" -dynamiclib -arch arm64 -isysroot "$SDK" "$MIN_VERSION_FLAG" -nostdlib++ \
    -install_name /tvm/llvm/sdk/usr/lib/libc++.1.dylib -compatibility_version 1.0 -current_version 1.0 \
    -o install/lib/libc++.1.dylib -Wl,-all_load install/lib/libc++.a

# ── 3. compiler-rt's builtins ──
# libclang_rt.<os>.a, which the driver links into every program when it is
# there (__isPlatformVersionAtLeast for @available, and the like). Its Darwin
# build makes the library of every platform whose SDK is installed, each with
# the architectures the compiler can target (arm64 only); only this
# platform's is kept.
configure "$BUILDDIR/builtins" -S "$SRCDIR/runtimes" -DTVM_COMPILER="$TOOLSIG" \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_C_COMPILER="$TOOLBIN/clang" -DCMAKE_CXX_COMPILER="$TOOLBIN/clang++" \
        -DCMAKE_INSTALL_PREFIX="$BUILDDIR/builtins/install" \
        -DLLVM_ENABLE_RUNTIMES=compiler-rt \
        -DCOMPILER_RT_BUILD_BUILTINS=ON -DCOMPILER_RT_BUILD_SANITIZERS=OFF \
        -DCOMPILER_RT_BUILD_XRAY=OFF -DCOMPILER_RT_BUILD_LIBFUZZER=OFF \
        -DCOMPILER_RT_BUILD_PROFILE=OFF -DCOMPILER_RT_BUILD_MEMPROF=OFF \
        -DCOMPILER_RT_BUILD_ORC=OFF -DCOMPILER_RT_BUILD_CTX_PROFILE=OFF \
        -DCOMPILER_RT_BUILD_GWP_ASAN=OFF -DCOMPILER_RT_ENABLE_IOS=ON \
        -DCOMPILER_RT_INCLUDE_TESTS=OFF -DLLVM_INCLUDE_TESTS=OFF
cd "$BUILDDIR/builtins"
ninja -j"$NPROC"
rm -rf install
ninja install
BUILTINS="$(find install -name "libclang_rt.$OS.a" | head -n 1)"
if [ -z "$BUILTINS" ]; then
    echo "error: compiler-rt built no libclang_rt.$OS.a" >&2
    exit 1
fi

# ── 4. the toolchain TVM runs ──
# Compiled by the build machine's clang, against this libc++ (-nostdinc++,
# its headers; -nostdlib++, its static library, with libc++abi and libunwind
# in it), with the build machine's tablegen programs.
RUNTIMES="$BUILDDIR/runtimes/install"
TARGET_PLATFORM_ARGS="$PLATFORM_ARGS"
CROSS_ARGS=""
CROSS_FLAGS=""
if [ -n "$TVM_IOS_CROSS" ]; then
    # A Darwin cross build, with the iPhoneOS SDK and deployment target: LLVM's
    # CMake knows the Darwin linker by CMAKE_SYSTEM_NAME, and for iOS gave it
    # the ELF one's options (-z defs, --gc-sections). The default target is
    # the device.
    TARGET_PLATFORM_ARGS="-DCMAKE_SYSTEM_NAME=Darwin -DCMAKE_CROSSCOMPILING=ON -DCMAKE_OSX_ARCHITECTURES=arm64"
    CROSS_ARGS="-DLLVM_HOST_TRIPLE=arm64-apple-ios$MIN_VERSION -DLLVM_DEFAULT_TARGET_TRIPLE=arm64-apple-ios
        -DCMAKE_C_FLAGS=$MIN_VERSION_FLAG"
    CROSS_FLAGS=" $MIN_VERSION_FLAG"
fi
# shellcheck disable=SC2086
configure "$BUILDDIR/target" -S "$SRCDIR/llvm" -DTVM_COMPILER="$TOOLSIG" \
        -DCMAKE_C_COMPILER="$TOOLBIN/clang" -DCMAKE_CXX_COMPILER="$TOOLBIN/clang++" \
        -DCMAKE_OSX_SYSROOT="$SDK" $TARGET_PLATFORM_ARGS $CROSS_ARGS \
        -DCMAKE_CXX_FLAGS="-nostdinc++ -isystem $RUNTIMES/include/c++/v1$CROSS_FLAGS" \
        -DCMAKE_EXE_LINKER_FLAGS="-nostdlib++ $RUNTIMES/lib/libc++.a$CROSS_FLAGS" \
        -DLLVM_NATIVE_TOOL_DIR="$TOOLBIN" \
        $LLVM_ARGS
cd "$BUILDDIR/target"
ninja -j"$NPROC" llvm-driver clang-resource-headers
check_exports_main bin/llvm
# the system's libc++ must not be in it (see above)
if otool -L bin/llvm | grep -q 'libc++'; then
    echo "error: bin/llvm links a libc++ dylib" >&2
    exit 1
fi

# ── 5. the bundle ──
BUNDLE="$BUILDDIR/bundle"
RESOURCE="$BUNDLE/lib/clang/$LLVM_MAJOR"
rm -rf "$BUNDLE"
mkdir -p "$RESOURCE/lib/darwin" "$BUNDLE/sdk/usr/lib"
cp -R "$BUILDDIR/target/lib/clang/$LLVM_MAJOR/include" "$RESOURCE/"
cp "$BUILDDIR/builtins/$BUILTINS" "$RESOURCE/lib/darwin/"

# The SDK: its settings (the driver reads the SDK's version and platform from
# SDKSettings.json), the C headers, and the library stubs -- not the
# frameworks, nor Swift. libc++ is this build's, headers and library, in
# place of the SDK's.
cp "$SDK/SDKSettings.json" "$SDK/SDKSettings.plist" "$BUNDLE/sdk/"
mkdir -p "$BUNDLE/sdk/usr/include"
(cd "$SDK/usr/include" && tar cf - --exclude ./c++ .) | (cd "$BUNDLE/sdk/usr/include" && tar xf -)
(cd "$SDK/usr/lib" && tar cf - --exclude ./swift --exclude './libc++*' --exclude './libstdc++*' .) | \
    (cd "$BUNDLE/sdk/usr/lib" && tar xf -)
# Links into the frameworks left out (Tcl, Kerberos, vecLib's headers ...)
find "$BUNDLE/sdk" -type l ! -exec test -e {} \; -exec rm {} +
cp -R "$BUILDDIR/runtimes/install/include/c++" "$BUNDLE/sdk/usr/include/"
cp "$BUILDDIR/runtimes/install/lib/libc++.1.dylib" "$BUNDLE/sdk/usr/lib/"
ln -s libc++.1.dylib "$BUNDLE/sdk/usr/lib/libc++.dylib"
if [ "$(otool -D "$BUNDLE/sdk/usr/lib/libc++.1.dylib" | tail -n 1)" != /tvm/llvm/sdk/usr/lib/libc++.1.dylib ]; then
    echo "error: libc++.1.dylib's install name is not /tvm/llvm/sdk/usr/lib/libc++.1.dylib" >&2
    exit 1
fi
for sym in ___cxa_throw __Unwind_RaiseException; do
    if ! nm -g "$BUNDLE/sdk/usr/lib/libc++.1.dylib" 2>/dev/null | grep -q " T $sym\$"; then
        echo "error: libc++.1.dylib does not define $sym (libc++abi / libunwind not merged)" >&2
        exit 1
    fi
done
