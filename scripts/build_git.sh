#!/bin/sh
# Build git for TVM (unpatched upstream, built in a shadow tree).
#
# usage: build_git.sh <srcdir> <builddir> <curl-srcdir> <curl-builddir> <openssl-prefix>
# env:   CC (the target compiler, e.g. ios-cc.sh when cross-compiling),
#        TVM_IOS_CROSS=1 when building for iOS
#
# git's Makefile builds in the source tree (objects next to the sources,
# generated headers and scripts in the top directory), so the build runs in
# a shadow tree under $BUILDDIR/src -- directories mirrored, files symlinked,
# as build_nano.sh does -- and the submodule stays untouched. Everything
# TVM-specific is a make variable: the static knobs live in
# third_party/configs/git_config.mak, which becomes the tree's config.mak
# (git's own mechanism; it is included after config.mak.uname and can undo
# that file's Darwin choices), and the per-build ones -- CC, CFLAGS, the
# libcurl link line, the guest shell (see gitmake below) -- go on the make
# command line here. git has no configure step to cross-compile around: the
# Makefile only probes the compiler (`cc -MMD` support) and never runs a
# compiled program, so the iOS build differs from the macOS one in CC alone.
#
# HTTPS is the static libcurl + OpenSSL the curl tool is built with
# (CURL_CFLAGS/CURL_LDFLAGS below, so curl-config is not consulted); on Apple
# the certificate verification is SecTrust, no CA file.
#
# Output: upstream's own programs in $BUILDDIR/src: `git`, and the two curl
# helpers git execs from its exec path, git-remote-http (remote-curl.c; the
# https/ftp/ftps names are links to it, as upstream installs them) and
# git-http-fetch. Only the helpers link libcurl and OpenSSL, so plain git
# commands do not map them. scripts/CMakeLists.txt lays them out in the
# initrd; the Flutter apps get each converted to a dylib
# (third_party/CMakeLists.txt).
set -e
SRCDIR="$1"
BUILDDIR="$2"
CURL_SRC="$3"
CURL_BUILD="$4"
OPENSSL_PREFIX="$5"
CC="${CC:-cc}"

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
CONFIGS_DIR="$ROOT/third_party/configs"
INCLUDE_SHIM="$ROOT/third_party/include-shim"
SHADOW="$BUILDDIR/src"
. "$(cd "$(dirname "$0")" && pwd)/tvm_stamp.sh"
tvm_stamp_begin "$BUILDDIR" "$0" "$SRCDIR" "$CONFIGS_DIR/git_config.mak" "$INCLUDE_SHIM/tvm_stdio_unlocked.h" \
    "$CURL_BUILD/lib/libcurl.a" "$OPENSSL_PREFIX/lib/libssl.a" "$OPENSSL_PREFIX/lib/libcrypto.a"
TVMDIR="$BUILDDIR/tvm"   # exec-path name lists for the initrd

# libcurl's link line: the static libraries plus what they need from the
# system.
CURL_LDFLAGS="$CURL_BUILD/lib/libcurl.a $OPENSSL_PREFIX/lib/libssl.a $OPENSSL_PREFIX/lib/libcrypto.a -lz"
case "$(uname)" in
    Darwin)
        CURL_LDFLAGS="$CURL_LDFLAGS -framework Security -framework CoreFoundation -framework CoreServices"
        # curl's CMake links SystemConfiguration on macOS only (lib/macos.c
        # SCDynamicStoreCopyProxies); iOS does not compile that call.
        if [ -z "$TVM_IOS_CROSS" ]; then
            CURL_LDFLAGS="$CURL_LDFLAGS -framework SystemConfiguration"
        fi
        ;;
    *)
        CURL_LDFLAGS="$CURL_LDFLAGS -lpthread -ldl"
        ;;
esac

# make with the TVM variable set.
#  SHELL_PATH: the Makefile runs its recipes (SHELL = $(SHELL_PATH)) and its
#     generator scripts ($(SHELL_PATH) ./tools/generate-*.sh) with it, on the
#     build host, so it stays /bin/sh. What the artifacts carry is read from
#     the two derived variables instead, which the command line pins to the
#     guest shell: SHELL_PATH_SQ (GIT-BUILD-OPTIONS, hence the #! line of
#     every generated script) and SHELL_PATH_CQ_SQ (the -DSHELL_PATH git's
#     run-command execs for `sh -c`). See git_config.mak.
#  CFLAGS replaces git's "-g -O2 -Wall" (TVM's debug info flags; -O2 kept,
#     git's hashing and delta code are hot) and force-includes
#     tvm_stdio_unlocked.h: Darwin's getc_unlocked() is an inline macro over
#     the libc FILE's fields, which a TVM stream does not have; git reads
#     config files with it under flockfile() (see the header).
#  CURL_CONFIG points at the cmake-generated script of our own libcurl so the
#     version check does not read the host's.
GUEST_SH=/tvm/bin/sh
JOBS="$(sysctl -n hw.ncpu 2>/dev/null || nproc 2>/dev/null || echo 4)"
gitmake() {
    make -C "$SHADOW" -j"$JOBS" \
        SHELL=/bin/sh SHELL_PATH=/bin/sh \
        SHELL_PATH_SQ="$GUEST_SH" SHELL_PATH_CQ_SQ="\"$GUEST_SH\"" \
        CC="$CC" \
        CFLAGS="-g -gdwarf-4 -O2 -Wall -include $INCLUDE_SHIM/tvm_stdio_unlocked.h" \
        CURL_CONFIG="$CURL_BUILD/curl-config" \
        CURL_CFLAGS="-I$CURL_SRC/include" \
        CURL_LDFLAGS="$CURL_LDFLAGS" \
        "$@"
}

# Print a make variable of git's Makefile as configured above.
gitvar() {
    gitmake -s -f "$SHADOW/tvm_print.mk" "tvm-print-$1"
}

# ── shadow tree ──
if [ ! -f "$SHADOW/Makefile" ]; then
    rm -rf "$SHADOW"
    mkdir -p "$SHADOW"
    (cd "$SRCDIR" && find . -path ./.git -prune -o -type d -print) | while read -r d; do
        mkdir -p "$SHADOW/$d"
    done
    (cd "$SRCDIR" && find . -path ./.git -prune -o -type f -print) | while read -r f; do
        ln -s "$SRCDIR/$f" "$SHADOW/$f"
    done
fi
cp "$CONFIGS_DIR/git_config.mak" "$SHADOW/config.mak"
printf 'include Makefile\ntvm-print-%%:\n\t@echo $($*)\n' > "$SHADOW/tvm_print.mk"

# ── upstream programs, scripts, templates ──
# git and the two curl helpers as upstream links them, the shell scripts
# (SCRIPT_SH with their #! rewritten to SHELL_PATH, and the git-sh-setup
# family they source) and the `git init` templates. Not `all`: that also
# builds the test helpers and, without NO_TCLTK, gitk and git-gui.
gitmake git git-remote-http git-http-fetch build-sh-script $(gitvar SCRIPT_LIB)
# templates/Makefile runs no generator through SHELL_PATH; it only writes it
# into the hook samples' #! line.
make -C "$SHADOW/templates" SHELL=/bin/sh SHELL_PATH="$GUEST_SH" PERL_PATH=perl

# Build-time assertions: the exec path the initrd populates (compiled into
# exec-cmd.o only), the guest shell, and no dynamic dependency outside the
# SDK's own libraries (nothing from Homebrew).
if ! strings "$SHADOW/exec-cmd.o" | grep -qx "/tvm/libexec/git-core"; then
    echo "error: git was not built with GIT_EXEC_PATH=/tvm/libexec/git-core" >&2
    exit 1
fi
if ! grep -q -- "-DSHELL_PATH='\"/tvm/bin/sh\"'" "$SHADOW/GIT-CFLAGS"; then
    echo "error: git was not built with SHELL_PATH=/tvm/bin/sh" >&2
    exit 1
fi
if [ "$(uname)" = Darwin ]; then
    if otool -L "$SHADOW/git-remote-http" | sed 1d | grep -v "^	/usr/lib/\|^	/System/Library/Frameworks/" | grep -q .; then
        echo "error: git-remote-http depends on a library outside the SDK:" >&2
        otool -L "$SHADOW/git-remote-http" >&2
        exit 1
    fi
fi

rm -rf "$TVMDIR"
mkdir -p "$TVMDIR"

# What the initrd links into the exec path (scripts/CMakeLists.txt):
# upstream's dashed built-in names and the generated scripts.
gitvar BUILT_INS | tr ' ' '\n' | sed '/^$/d' > "$TVMDIR/built_ins"
{ gitvar SCRIPT_SH_GEN; gitvar SCRIPT_LIB; } | tr ' ' '\n' | sed '/^$/d' > "$TVMDIR/scripts"
