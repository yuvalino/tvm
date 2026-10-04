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
# HTTPS is the static libcurl + OpenSSL that scripts/build_curl_tool.sh links
# the curl tool with (CURL_CFLAGS/CURL_LDFLAGS below, so curl-config is not
# consulted); on Apple the certificate verification is SecTrust, no CA file.
#
# Output: git dispatches most commands in-process but execs a few programs
# from its exec path, above all git-remote-https (remote-curl.c, a separate
# executable upstream). Instead of one image per program, git.o,
# remote-curl.o and http-fetch.o have their cmd_main() renamed at link time
# (ld -alias / objcopy --redefine-sym, as build_curl_tool.sh does for curl's
# main) and third_party/git_main.c dispatches on argv[0]; the helper names
# in the exec path are links to /tvm/bin/git (scripts/CMakeLists.txt).
# The build makes upstream's own git, git-remote-http and git-http-fetch too
# (the link that validates the object set) and then links the multi-call
# `git` (the Flutter apps get it converted to libgit.dylib,
# third_party/CMakeLists.txt).
set -e
SRCDIR="$1"
BUILDDIR="$2"
CURL_SRC="$3"
CURL_BUILD="$4"
OPENSSL_PREFIX="$5"
CC="${CC:-cc}"

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
CONFIGS_DIR="$ROOT/third_party/configs"
WRAP_SRC="$ROOT/third_party/git_main.c"
INCLUDE_SHIM="$ROOT/third_party/include-shim"
SHADOW="$BUILDDIR/src"
. "$(cd "$(dirname "$0")" && pwd)/tvm_stamp.sh"
tvm_stamp_begin "$BUILDDIR" "$0" "$SRCDIR" "$CONFIGS_DIR/git_config.mak" "$INCLUDE_SHIM/tvm_stdio_unlocked.h" \
    "$ROOT/third_party/git_main.c" "$CURL_BUILD/lib/libcurl.a" "$OPENSSL_PREFIX/lib/libssl.a" "$OPENSSL_PREFIX/lib/libcrypto.a"
TVMDIR="$BUILDDIR/tvm"   # renamed objects, wrapper, exec-path name lists

# The libcurl link line of the curl tool (build_curl_tool.sh): the static
# libraries plus what they need from the system.
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

# ── the multi-call image ──
rm -rf "$TVMDIR"
mkdir -p "$TVMDIR"
# The wrapper's OPENSSL_cleanup() comes from OpenSSL's headers.
"$CC" -g -gdwarf-4 -I"$OPENSSL_PREFIX/include" -c "$WRAP_SRC" -o "$TVMDIR/git_main.o"

# Rename each program's cmd_main -> <program>_cmd_main without touching the
# sources: ld -r with an alias, then the original made private so the
# wrapper's cmd_main is the only global one (objcopy on Linux).
rename_cmd_main() {
    case "$(uname)" in
        Darwin)
            "$CC" -nostdlib -r \
                -Wl,-alias,_cmd_main,"_$2" -Wl,-unexported_symbol,_cmd_main \
                "$SHADOW/$1" -o "$TVMDIR/$1"
            ;;
        *)
            objcopy --redefine-sym cmd_main="$2" "$SHADOW/$1" "$TVMDIR/$1"
            ;;
    esac
}
rename_cmd_main git.o git_cmd_main
rename_cmd_main remote-curl.o remote_curl_cmd_main
rename_cmd_main http-fetch.o http_fetch_cmd_main

# The link inputs, in the order of git's own link rules: the program objects,
# the built-in commands, the http objects git-remote-http and git-http-fetch
# share, common-main.o (main), then libcurl and git's libraries + EXTLIBS.
OBJS="$TVMDIR/git_main.o $TVMDIR/git.o $TVMDIR/remote-curl.o $TVMDIR/http-fetch.o"
for o in $(gitvar BUILTIN_OBJS) http.o http-walker.o common-main.o; do
    OBJS="$OBJS $SHADOW/$o"
done
LIBS="$(gitvar ALL_LDFLAGS) $CURL_LDFLAGS"
for l in $(gitvar LIBS); do
    case "$l" in
        -*) LIBS="$LIBS $l" ;;
        *)  LIBS="$LIBS $SHADOW/$l" ;;
    esac
done
# The executable.
"$CC" -g -o "$BUILDDIR/git" $OBJS $LIBS

# What the initrd links into the exec path (scripts/CMakeLists.txt):
# upstream's dashed built-in names and the generated scripts.
gitvar BUILT_INS | tr ' ' '\n' | sed '/^$/d' > "$TVMDIR/built_ins"
{ gitvar SCRIPT_SH_GEN; gitvar SCRIPT_LIB; } | tr ' ' '\n' | sed '/^$/d' > "$TVMDIR/scripts"
