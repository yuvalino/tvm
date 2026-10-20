#!/bin/sh
# Build CPython for TVM (unpatched upstream, out-of-tree autotools build).
#
# usage: build_python.sh <srcdir> <builddir> <libffi-prefix> <openssl-prefix> <ncurses-prefix>
# env:   CC (the target compiler, e.g. ios-cc.sh when cross-compiling),
#        TVM_IOS_CROSS=1 when building for iOS
#
# Output, installed under <builddir>/stage/tvm (configure --prefix=/tvm):
#   bin/python3.X                 the interpreter (converted to
#                                 libpython.framework for the Flutter apps)
#   lib/python3.X                 the standard library, byte-compiled,
#                                 with pip in site-packages
#
# The iOS build is a Darwin build (--host=aarch64-apple-darwin with the
# iPhoneOS compiler), not CPython's own iOS platform (*-apple-ios): that one
# is embedding-only by design -- sys.platform "ios" turns subprocess off in
# the stdlib, there is no REPL and no pip -- and TVM provides exactly what it
# leaves out (fork/exec, a terminal). A cross build needs a Python of the
# same version on the build machine (--with-build-python): it is built from
# the same sources with the host compiler first.
#
# Every extension module is built into the interpreter (MODULE_BUILDTYPE=
# static, the setting CPython's WASI build uses): a TVM image has no dlopen,
# so lib-dynload stays empty. Dependencies:
#   _ssl, _hashlib   the static OpenSSL curl links (third_party/openssl)
#   _ctypes          the static libffi (third_party/libffi) on iOS, whose
#                    SDK has none; on macOS configure prefers the SDK's
#                    libffi (/usr/lib/libffi.dylib, a system library)
#   _curses, _curses_panel  the static ncursesw bash, vim, less and nano link
#   _sqlite3, zlib, _bz2, pyexpat  the SDK's libsqlite3/libz/libbz2 (pyexpat
#                    and _decimal use CPython's bundled expat and libmpdec)
# readline is left out: the REPL (_pyrepl) does its own line editing and
# history, reading /tvm/share/terminfo itself. _lzma is missing because
# neither SDK has lzma.h.
set -e
SRCDIR="$1"
BUILDDIR="$2"
FFI="$3"
OPENSSL="$4"
NCURSES="$5"
. "$(cd "$(dirname "$0")" && pwd)/tvm_stamp.sh"
SHIM="$(cd "$(dirname "$0")/.." && pwd)/third_party/include-shim"
CONFIGS="$(cd "$(dirname "$0")/.." && pwd)/third_party/configs"
tvm_stamp_begin "$BUILDDIR" "$0" "$SRCDIR" "$FFI/lib/libffi.a" "$CONFIGS/python_apple_support.py" "$CONFIGS/python_scproxy.py" \
    "$OPENSSL/lib/libssl.a" "$OPENSSL/lib/libcrypto.a" "$NCURSES/lib/libncursesw.a" \
    "$SHIM/tvm_ios_libc.h" "$SHIM/tvm_stdio_unlocked.h"
CC="${CC:-cc}"
NPROC="$(sysctl -n hw.ncpu 2>/dev/null || nproc)"

PYVER="$(sed -n 's/^#define PY_MAJOR_VERSION[[:space:]]*\([0-9]*\).*/\1/p' "$SRCDIR/Include/patchlevel.h").$(sed -n 's/^#define PY_MINOR_VERSION[[:space:]]*\([0-9]*\).*/\1/p' "$SRCDIR/Include/patchlevel.h")"
STAGE="$BUILDDIR/stage"
LIBDEST="$STAGE/tvm/lib/python$PYVER"

# ── 1. build Python (cross builds only) ──
CROSS_ARGS=""
if [ -n "$TVM_IOS_CROSS" ]; then
    mkdir -p "$BUILDDIR/host"
    cd "$BUILDDIR/host"
    if [ ! -f Makefile ]; then
        CC=cc CFLAGS= LDFLAGS= "$SRCDIR/configure" --without-ensurepip --disable-test-modules
    fi
    make -j"$NPROC"
    # python.exe on a case-insensitive file system (macOS), python elsewhere
    BUILD_PYTHON="$BUILDDIR/host/python.exe"
    [ -x "$BUILD_PYTHON" ] || BUILD_PYTHON="$BUILDDIR/host/python"
    CROSS_ARGS="--host=aarch64-apple-darwin --build=$("$SRCDIR/config.guess") --with-build-python=$BUILD_PYTHON"
    # Answers configure cannot probe on the build machine (it would have to
    # run a target program): Darwin has /dev/ptmx, and its getaddrinfo is
    # not the buggy one the IPv6 check guards against.
    export ac_cv_file__dev_ptmx=yes ac_cv_file__dev_ptc=no ac_cv_buggy_getaddrinfo=no
    # The iPhoneOS SDK exports getentropy but no header declares it (as
    # scripts/build_bash.sh notes): without it os.urandom and the hash seed
    # read /dev/urandom, through TVM's open().
    export ac_cv_func_getentropy=no
    # Likewise macOS-only: sendfile has no iOS declaration, clock_settime is
    # marked unavailable on iOS.
    export ac_cv_func_sendfile=no ac_cv_func_clock_settime=no
    # What configure finds out by running a test program, which a cross
    # build cannot (it then assumes the worst): the arm64 Darwin answers, as
    # the native macOS build gets them -- a tzset() that works (else no
    # time.tzset), computed gotos (the faster interpreter loop), no aligned
    # access required.
    export ac_cv_working_tzset=yes ac_cv_computed_gotos=yes ac_cv_aligned_required=no
    # macOS-only APIs: _scproxy reads the system proxy settings through
    # SCDynamicStore, which the iPhoneOS SDK does not have.
    export py_cv_module__scproxy=n/a
fi

# ── 2. the target interpreter ──
# tvm_stdio_unlocked.h: Darwin's getc_unlocked() & co. are macros over the
# libc FILE's fields, and a guest FILE is a TVM stream (see the header): the
# tokenizer reads script files with getc_unlocked() under flockfile()
# (Py_UniversalNewlineFgetsWithSize), which faulted on the first line.
#
# WITH_PYMALLOC_RADIX_TREE=0: pymalloc tells its own blocks from others
# (address_in_range) by the arena table, not by a radix tree indexed by
# address bits. TVM's fork moves a task's heap, the arenas with it, and
# relocates every pointer into it, which keeps the table right but not the
# tree: a forked child took its own objects for malloc()'s and handed them to
# free(). (The table check reads the 16 KB pool header below an address; on
# arm64 Apple platforms that is the address's own page.)
#
# The force-included headers go to make only (EXTRA_CFLAGS, which the
# Makefile adds to every compile), not to configure: there they reached the
# function checks too, whose test programs declare a function with a dummy
# prototype (char waitpid(void);) that then clashed with the real one from
# the header's <stdlib.h>, and configure took waitpid, wait, system & co.
# for missing on iOS -- an os module without waitpid, so no subprocess.
TARGET_CFLAGS="-g -gdwarf-4 -DWITH_PYMALLOC_RADIX_TREE=0"
TARGET_EXTRA_CFLAGS="-include $SHIM/tvm_stdio_unlocked.h"
TARGET_LDFLAGS=""
if [ -z "$TVM_IOS_CROSS" ] && [ "$(uname)" = Darwin ]; then
    # An explicit SDK root: without one, Apple's clang also searches
    # /usr/local/include and /usr/local/lib, and configure would link the
    # build machine's libraries there (Homebrew's liblzma, zstd, libintl)
    # into the interpreter. (ios-cc.sh gets the iPhoneOS SDK from xcrun.)
    SDKROOT_FLAG="-isysroot $(xcrun --sdk macosx --show-sdk-path)"
    TARGET_CFLAGS="$TARGET_CFLAGS $SDKROOT_FLAG"
    TARGET_LDFLAGS="$SDKROOT_FLAG"
fi
if [ -n "$TVM_IOS_CROSS" ]; then
    # system(3) is unavailable at compile time in the iPhoneOS SDK (os.system
    # calls it); TVM interposes it. See the header.
    TARGET_EXTRA_CFLAGS="$TARGET_EXTRA_CFLAGS -include $SHIM/tvm_ios_libc.h"
fi

mkdir -p "$BUILDDIR/target"
cd "$BUILDDIR/target"
if [ ! -f Makefile ]; then
    # pkg-config sees nothing: the build machine's libraries (Homebrew's
    # liblzma, zstd, mpdecimal, ...) would end up linked into the target.
    # The dependencies are the ones passed here, and the SDK's, which
    # configure finds by header and library checks.
    # ac_cv_enable_visibility=no: no -fvisibility=hidden. TVM starts a
    # program at its exported main (as for every tool, see exe2lib.py), and
    # hidden visibility leaves python's main a private symbol.
    # ac_cv_func_mmap=no: pymalloc takes its arenas from malloc instead of
    # anonymous mmap (HAVE_MMAP is read in Objects/obmalloc.c only; the mmap
    # module does not depend on it). TVM's fork copies a task's heap and its
    # images' data, relocating pointers into them, but not anonymous
    # mappings: a forked child would keep running on the parent's objects.
    PKG_CONFIG_LIBDIR=/nonexistent PKG_CONFIG_PATH= \
    ac_cv_enable_visibility=no ac_cv_func_mmap=no \
    CC="$CC" CFLAGS="$TARGET_CFLAGS" LDFLAGS="$TARGET_LDFLAGS" \
    MODULE_BUILDTYPE=static \
    LIBFFI_CFLAGS="-I$FFI/include" LIBFFI_LIBS="$FFI/lib/libffi.a" \
    CURSES_CFLAGS="-I$NCURSES/include/ncursesw -I$NCURSES/include" \
    CURSES_LIBS="$NCURSES/lib/libncursesw.a" \
    PANEL_CFLAGS="-I$NCURSES/include/ncursesw -I$NCURSES/include" \
    PANEL_LIBS="$NCURSES/lib/libpanelw.a" \
    "$SRCDIR/configure" $CROSS_ARGS --prefix=/tvm \
        --disable-shared --without-ensurepip --disable-test-modules \
        --with-openssl="$OPENSSL" --with-openssl-rpath=no \
        --with-system-libmpdec=no --with-readline=no
fi
# pymalloc hands out zeroed blocks: Objects/obmalloc.c is compiled from a copy
# here, which make's VPATH search finds before the source tree's.
# TVM's fork copies a task's heap and relocates every word in it whose value
# lies in a copied range, pointer or not, so a data word must not look like an
# address (TVM's malloc zeroes its blocks for this). pymalloc reuses its freed
# blocks as they are: a string shorter than its block left the rest of its
# last word holding the upper bytes of a pointer the block had before, and
# that word was "relocated". subprocess's argv copies
# (_PySequence_BytesToCharpArray, which the forked child execs) reached the
# new program as "print(6*7)~e\x01". (Building --without-pymalloc fixes it
# too, but every allocation through TVM's malloc made Python 7x slower.)
# (Its one include relative to its own directory gets the source tree's path.)
mkdir -p Objects
sed -e 's|^    return (void \*)bp;$|    memset(bp, 0, INDEX2SIZE(size));  /* TVM: see build_python.sh */\
    return (void *)bp;|' \
    -e "s|^#  include \"mimalloc/static.c\"$|#  include \"$SRCDIR/Objects/mimalloc/static.c\"|" \
    "$SRCDIR/Objects/obmalloc.c" > Objects/obmalloc.c.new
if [ "$(grep -c 'TVM: see build_python.sh' Objects/obmalloc.c.new)" != 1 ] ||
   [ "$(grep -c "$SRCDIR/Objects/mimalloc/static.c" Objects/obmalloc.c.new)" != 1 ]; then
    echo "error: Objects/obmalloc.c: pymalloc_alloc() not patched" >&2
    exit 1
fi
if ! cmp -s Objects/obmalloc.c.new Objects/obmalloc.c; then
    mv Objects/obmalloc.c.new Objects/obmalloc.c
else
    rm Objects/obmalloc.c.new
fi

make -j"$NPROC" EXTRA_CFLAGS="$TARGET_EXTRA_CFLAGS"

# TVM finds the entry point by its exported symbol.
if ! nm -g python* 2>/dev/null | grep -Eq ' T _?main$'; then
    echo "error: the interpreter does not export main" >&2
    exit 1
fi

# No dlopen in a TVM image: every module must be built in.
if ls Modules/*.so >/dev/null 2>&1 || ls build/lib.*/*.so >/dev/null 2>&1; then
    echo "error: shared extension modules were built (MODULE_BUILDTYPE=static not in effect)" >&2
    exit 1
fi

# ── 3. stage the install ──
rm -rf "$STAGE"
make install DESTDIR="$STAGE" EXTRA_CFLAGS="$TARGET_EXTRA_CFLAGS"

# Not shipped: the test suite, the Tk/IDLE family (no _tkinter), the static
# libpython and its build configuration (nothing links against it in TVM),
# and the optimized bytecode (-O/-OO) next to the plain one.
rm -rf "$LIBDEST/test" "$LIBDEST/idlelib" "$LIBDEST/tkinter" "$LIBDEST/turtledemo" \
       "$LIBDEST/turtle.py" "$LIBDEST"/config-* "$STAGE/tvm/share" "$STAGE/tvm/include" \
       "$STAGE/tvm/lib/pkgconfig" "$STAGE/tvm/lib/libpython$PYVER.a"
find "$LIBDEST" -name '*.opt-1.pyc' -delete
find "$LIBDEST" -name '*.opt-2.pyc' -delete

# The interpreter's stdout and stderr stay the program's: see the file.
cp "$CONFIGS/python_apple_support.py" "$LIBDEST/_apple_support.py"
# iOS has no _scproxy (see the configure call above), which urllib.request
# imports on Darwin: a Python one, no system proxies (see the file).
if [ -n "$TVM_IOS_CROSS" ]; then
    cp "$CONFIGS/python_scproxy.py" "$LIBDEST/_scproxy.py"
fi

# ── 4. pip, and the bytecode ──
# The wheel ensurepip bundles, unpacked into site-packages (a wheel is a
# zip of the installed files) and byte-compiled with the build Python, which
# is the same version. The /tvm/bin/pip scripts come from the initrd.
COMPILE_PYTHON="${BUILD_PYTHON:-$BUILDDIR/target/python.exe}"
[ -x "$COMPILE_PYTHON" ] || COMPILE_PYTHON="$BUILDDIR/target/python"
PIP_WHEEL="$(ls "$SRCDIR"/Lib/ensurepip/_bundled/pip-*.whl)"
mkdir -p "$LIBDEST/site-packages"
(cd "$LIBDEST/site-packages" && unzip -q -o "$PIP_WHEEL")
# Bytecode for the whole library, pip included, checked against nothing
# (unchecked-hash): a .pyc says whether it is stale by its source's mtime,
# which copying the library into the initrd or the app bundle does not keep,
# and the library is read-only there, so stale bytecode would be recompiled
# on every import and never written.
# (-x: the files make install skips too, deliberately broken sources.)
"$COMPILE_PYTHON" -Wi -m compileall -q -f -o 0 --invalidation-mode unchecked-hash \
    -x 'bad_coding|badsyntax' -d "/tvm/lib/python$PYVER" "$LIBDEST"

echo "$PYVER" > "$BUILDDIR/version"
