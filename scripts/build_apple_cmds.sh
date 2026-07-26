#!/bin/bash
# Build one apple-oss-distributions *_cmds repo as a multi-call module.
#
# Each command in these repos is an independent program with its own main() and
# its own file-scope globals, so they cannot simply be linked together (see the
# duplicate `usage`/`flags` symbols all over these trees). For each command we
# therefore:
#   1. compile its sources with -Dmain=<cmd>_main
#   2. `ld -r` the objects into one relocatable object, exporting *only* that
#      renamed main — every other global is demoted to a private extern, so
#      commands can no longer collide with each other
#   3. link all the per-command objects plus a generated dispatcher into a
#      single module (dylib for the flutter build, executable for the CLI one),
#      busybox-style: the dispatcher picks the command from basename(argv[0]).
#
# Usage: build_apple_cmds.sh <module> <repo-src> <build-dir> <manifest>
#
# Manifest lines are '|'-separated:
#   <cmd>|<subdir>|<sources>|<extra-cflags>|<platform>
# <platform> is blank for both, or 'macos'/'ios' to restrict the command.
# ('|' rather than tab because tab is IFS whitespace, so bash's read would
#  collapse the empty fields that most rows have.)
# In the cflags column, @SRC@ expands to the repo root and @APPLE@ to
# third_party/apple, so include paths do not depend on the working directory.
# ("#"-prefixed lines and blank lines are ignored).
set -e

MODULE="$1"
SRCDIR="$(cd "$2" && pwd)"
BUILDDIR="$(mkdir -p "$3" && cd "$3" && pwd)"
MANIFEST="$4"

[ -n "$MODULE" ] && [ -d "$SRCDIR" ] && [ -f "$MANIFEST" ] || {
    echo "usage: build_apple_cmds.sh <module> <repo-src> <build-dir> <manifest>" >&2
    exit 1
}

CC="${CC:-cc}"

APPLE_DIR="$(cd "$(dirname "$0")/../third_party/apple" && pwd)"
GEN_INC="$BUILDDIR/gen_include"

if [ "${TVM_IOS_CROSS:-}" = "1" ]; then
    LD="xcrun --sdk iphoneos ld -arch arm64"
    # The iOS SDK ships the same libSystem as macOS but strips a number of
    # public headers from usr/include (libproc.h, utmp.h, nlist.h, sys/disk.h,
    # get_compat.h, ...). The symbols are all still there at runtime, so fall
    # back to the macOS headers for anything the iOS SDK does not declare.
    # -idirafter keeps the iOS SDK authoritative wherever it does have the
    # header. Anything that genuinely is not on iOS fails at dylib link time,
    # since undefined symbols are an error for Mach-O dylibs.
    MACINC="$(xcrun --sdk macosx --show-sdk-path)/usr/include"
    SDK_FALLBACK="-idirafter $MACINC"

    # The iOS SDK ships mach/mach_vm.h as a single `#error mach_vm.h
    # unsupported.` line, even though mach_vm_region/mach_vm_read/... are all
    # exported by libSystem on iOS exactly as they are on macOS. -idirafter
    # cannot get past it (the SDK copy wins), so point at the macOS SDK's real
    # header from a directory that comes first. Generated rather than vendored
    # so it always tracks whatever SDK is installed.
    TARGET_PLATFORM=ios
    mkdir -p "$GEN_INC/mach"
    printf '#include "%s/mach/mach_vm.h"\n' "$MACINC" > "$GEN_INC/mach/mach_vm.h"
else
    LD="ld -arch $(uname -m)"
    SDK_FALLBACK=""
    TARGET_PLATFORM=macos
    mkdir -p "$GEN_INC"
fi

# Search order, highest priority first:
#   gen_include      — generated per-build shims (iOS mach_vm.h)
#   include-override — full xnu copies of headers the SDK ships stripped
#   <the SDK>
#   include          — xnu/Libc/... headers the SDK omits entirely
#   include-shim     — hand-written, no open-source original exists
#   macOS SDK        — iOS only; headers dropped from the iOS SDK
APPLE_INCLUDES="-I$GEN_INC -I$APPLE_DIR/include-override"
APPLE_FALLBACK="-idirafter $APPLE_DIR/include -idirafter $APPLE_DIR/include-shim $SDK_FALLBACK"

# K&R definitions are still all over these trees (`main(argc, argv)`), and the
# repos are third-party sources we do not want to patch, so quiet the noise.
# These trees predate clang turning several legacy-C patterns into hard errors
# (ndp.c passes NULL for a size_t, various files call functions with no visible
# prototype). They are third-party sources we do not want to patch, and Apple's
# own toolchain accepted them, so demote that class back to warnings.
LEGACY_C_FLAGS="-Wno-error=int-conversion -Wno-error=implicit-function-declaration \
-Wno-error=incompatible-function-pointer-types -Wno-error=incompatible-pointer-types \
-Wno-error=return-type -Wno-error=deprecated-non-prototype"

BASE_CFLAGS="-g -gdwarf-4 -fno-common -w -DPRIVATE=1 -DAPPLE_CMDS_MULTICALL=1 $LEGACY_C_FLAGS $APPLE_INCLUDES $APPLE_FALLBACK"

# Apply any patches from third_party/apple/patches/<module>-*.patch.
# Patches are applied with -N (skip if already applied) so re-builds are safe.
PATCHES_DIR="$APPLE_DIR/patches"
if [ -d "$PATCHES_DIR" ]; then
    for p in "$PATCHES_DIR/${MODULE}"-*.patch; do
        [ -f "$p" ] || continue
        patch -d "$SRCDIR" -p1 -N --silent < "$p" 2>/dev/null || true
    done
fi

OBJDIR="$BUILDDIR/obj"
TMPDIR_="$BUILDDIR/tmp"
rm -rf "$OBJDIR" "$TMPDIR_"
mkdir -p "$OBJDIR" "$TMPDIR_"

# C identifiers, not command names: wordexp-helper -> wordexp_helper_main.
# printf, not echo: echo's trailing newline is non-alphanumeric and tr would
# turn it into a trailing '_', giving tabs__main instead of tabs_main.
symbolize() { printf '%s' "$1" | tr -c 'A-Za-z0-9_' '_'; }

CMDS=""
COMMON_SRCS=""
MODULE_CFLAGS=""
MODULE_LIBS=""

while IFS='|' read -r cmd subdir sources cflags platforms; do
    # Module-wide directives. `#common` lists support sources (repo-relative)
    # linked into every command — network_cmds_lib, disklib and friends. Each
    # command gets its own private copy, which is fine: the ld -r step localises
    # them, so two commands can hold different copies without colliding, and
    # -dead_strip drops whatever a given command did not use.
    case "$cmd" in
        '#common') COMMON_SRCS="$subdir"; continue ;;
        '#cflags') MODULE_CFLAGS="$subdir"; continue ;;
        '#libs')   MODULE_LIBS="$subdir"; continue ;;
        ''|'#'*) continue ;;
    esac

    # Optional 5th column restricts a command to one platform. quotacheck is
    # the live case: its blockcheck() helper sits inside #if TARGET_OS_OSX in
    # Apple's own disklib, so the command simply does not exist on iOS.
    platforms="${platforms%%[[:space:]]*}"
    if [ -n "$platforms" ] && [ "$platforms" != "$TARGET_PLATFORM" ]; then
        echo "  [$MODULE] $cmd — skipped ($platforms only)"
        continue
    fi

    sym="$(symbolize "$cmd")"
    cmddir="$SRCDIR/$subdir"
    work="$TMPDIR_/$cmd"
    mkdir -p "$work"

    # Empty source list means "everything in the directory".
    if [ -z "$sources" ]; then
        srcfiles=$(ls "$cmddir"/*.c)
    else
        srcfiles=""
        for s in $sources; do srcfiles="$srcfiles $cmddir/$s"; done
    fi
    for s in $COMMON_SRCS; do srcfiles="$srcfiles $SRCDIR/$s"; done

    # @SRC@/@APPLE@ keep manifest include paths absolute.
    mod_cflags="${MODULE_CFLAGS//@SRC@/$SRCDIR}"; mod_cflags="${mod_cflags//@APPLE@/$APPLE_DIR}"
    cmd_cflags="${cflags//@SRC@/$SRCDIR}"; cmd_cflags="${cmd_cflags//@APPLE@/$APPLE_DIR}"

    echo "  [$MODULE] $cmd"
    objs=""
    for src in $srcfiles; do
        obj="$work/$(basename "$src" .c).o"
        # shellcheck disable=SC2086
        $CC $BASE_CFLAGS $mod_cflags -I"$cmddir" $cmd_cflags -Dmain="${sym}_main" -c "$src" -o "$obj"
        objs="$objs $obj"
    done

    # Collapse to a single object that exports nothing but the renamed main.
    # shellcheck disable=SC2086
    $LD -r -o "$OBJDIR/$cmd.o" $objs -exported_symbol "_${sym}_main"

    CMDS="$CMDS $cmd"
done < "$MANIFEST"

[ -n "$CMDS" ] || { echo "error: no commands built for $MODULE" >&2; exit 1; }

# ── Dispatcher ────────────────────────────────────────────────────────────
DISPATCH="$BUILDDIR/${MODULE}_dispatch.c"
{
    echo "/* Generated by build_apple_cmds.sh — do not edit. */"
    echo '#include <libgen.h>'
    echo '#include <stdio.h>'
    echo '#include <string.h>'
    echo
    for cmd in $CMDS; do
        echo "extern int $(symbolize "$cmd")_main(int, char **);"
    done
    echo
    echo 'static const struct { const char *name; int (*fn)(int, char **); } cmds[] = {'
    for cmd in $CMDS; do
        echo "    { \"$cmd\", $(symbolize "$cmd")_main },"
    done
    echo '};'
    cat <<'EOF'

static int dispatch(const char *name, int argc, char **argv)
{
    for (size_t i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++)
        if (!strcmp(cmds[i].name, name))
            return cmds[i].fn(argc, argv);
    return -1;
}

int main(int argc, char **argv)
{
    int rc;

    /* Normal path: invoked through a symlink named after the command. */
    if (argc > 0 && (rc = dispatch(basename(argv[0]), argc, argv)) != -1)
        return rc;

    /* Fallback, for poking at the module directly: <module> <cmd> [args] */
    if (argc > 1 && (rc = dispatch(argv[1], argc - 1, argv + 1)) != -1)
        return rc;

    fprintf(stderr, "%s: commands:", argc > 0 ? basename(argv[0]) : "?");
    for (size_t i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++)
        fprintf(stderr, " %s", cmds[i].name);
    fprintf(stderr, "\n");
    return 127;
}
EOF
} > "$DISPATCH"

# shellcheck disable=SC2086
$CC $BASE_CFLAGS -c "$DISPATCH" -o "$OBJDIR/00_dispatch.o"

# Executable for the CLI initrd. The flutter initrd instead gets a dylib,
# linked from these same objects by the _shared cmake target.
# shellcheck disable=SC2086
$CC -g -gdwarf-4 -Wl,-dead_strip -o "$BUILDDIR/$MODULE" "$OBJDIR"/*.o $MODULE_LIBS $APPLE_CMDS_LIBS

echo "  [$MODULE] linked $(echo $CMDS | wc -w | tr -d ' ') commands"
