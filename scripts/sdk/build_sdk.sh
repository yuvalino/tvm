#!/bin/sh
# Make the SDK of TVM's clang toolchain bundle (scripts/build_llvm.sh): the
# C headers and the library stubs a guest's clang compiles and links
# against, at /tvm/llvm/sdk.
#
# usage: build_sdk.sh <macosx|iphoneos> <workdir> <outdir>
#
# Nothing is copied from Xcode's SDK, whose license allows no redistribution
# of any part of it (Xcode and Apple SDKs Agreement, 2.7). Instead:
#
#   usr/include   Apple's open-source releases (sources.txt, the projects
#                 and versions of macOS 26.2), each installed the way the
#                 project installs it where that can run here:
#                   AvailabilityVersions  its CMake install, which generates
#                                         Availability.h & co. from its
#                                         version list; the published list
#                                         ends in 2023, the versions since
#                                         are added (availability-versions.dsl)
#                   xnu                   make installhdrs
#                   xnu's libsyscall      xcodescripts/mach_install_mig.sh:
#                                         the MIG interfaces (mach/task.h ...)
#                   Libc                  xcodescripts/headers.sh
#                   ncurses               curses.h and term.h generated, as
#                                         its xcodescripts do
#                 and the other projects' public headers as the macOS SDK
#                 places them (headers.txt), its symbolic links among them
#                 (readline/history.h ...); libxml2's xmlversion.h is the
#                 project's pregenerated one, sqlite3.h SQLite's own (public
#                 domain: Apple publishes no SQLite). The SDK's
#                 TargetConditionals.h, math.h, fenv.h and complex.h have no
#                 open-source release (Libm's is from 2002): those are TVM's
#                 own, written for it (scripts/sdk/*.h).
#   usr/lib       stubs (.tbd) of libSystem and the other libraries those
#                 headers are of, generated from the libraries themselves
#                 (gen_tbd.py)
#   SDKSettings.json   what clang reads of an SDK: its platform and version
#   NOTICE.txt    the licenses of the open-source projects (APSL 2.0 and
#                 others), which their redistribution asks for
#
# The headers are the projects' as published (but for the "fixes" below);
# Apple's SDK build also rewrites them ("bleaching": its internal platforms
# and SPI markings made public ones), so a few declarations differ, e.g. an
# iOS SPI that the SDK marks unavailable is declared available here.
#
# The build machine's Xcode is used as a tool only: its mig and cc (for the
# MIG interfaces and xnu's build tools), unifdef.
set -e

[ $# -eq 3 ] || { echo "usage: build_sdk.sh <macosx|iphoneos> <workdir> <outdir>" >&2; exit 1; }
PLATFORM="$1"
WORK="$2"
OUT="$3"
HERE="$(cd "$(dirname "$0")" && pwd)"
NPROC="$(sysctl -n hw.ncpu 2>/dev/null || nproc)"

case "$PLATFORM" in
    macosx)   STUBS_OS=macos ;;
    iphoneos) STUBS_OS=ios ;;
    *) echo "build_sdk: unknown platform '$PLATFORM'" >&2; exit 1 ;;
esac
# The release the sources are (macOS 26.2, iOS 26.2: Darwin 25.2)
SDK_VERSION=26.2
DARWIN_VERSION=25.2.0

mkdir -p "$WORK/dl" "$WORK/src"
WORK="$(cd "$WORK" && pwd)"

# ── the sources ──
# src <tag>: the extracted project
src() { echo "$WORK/src/$1"; }
grep -v '^#' "$HERE/sources.txt" | while read -r project tag sha url; do
    [ -n "$project" ] || continue
    [ -n "$url" ] || url="https://github.com/apple-oss-distributions/$project/archive/refs/tags/$tag.tar.gz"
    if [ ! -f "$WORK/src/$tag/.tvm-extracted" ]; then
        tarball="$WORK/dl/$tag.tar.gz"
        if [ ! -f "$tarball" ] || [ "$(shasum -a 256 "$tarball" | cut -d' ' -f1)" != "$sha" ]; then
            curl -fsSL -o "$tarball.part" "$url"
            mv "$tarball.part" "$tarball"
        fi
        got="$(shasum -a 256 "$tarball" | cut -d' ' -f1)"
        if [ "$got" != "$sha" ]; then
            echo "build_sdk: $tag.tar.gz: sha256 $got, expected $sha" >&2
            exit 1
        fi
        rm -rf "$WORK/src/$tag"
        mkdir -p "$WORK/src/$tag"
        tar xzf "$tarball" -C "$WORK/src/$tag" --strip-components 1
        touch "$WORK/src/$tag/.tvm-extracted"
    fi
done
tag_of() { grep "^$1 " "$HERE/sources.txt" | awk '{print $2}'; }
XNU="$(src "$(tag_of xnu)")"
LIBC="$(src "$(tag_of Libc)")"
AV="$(src "$(tag_of AvailabilityVersions)")"

STAGE="$WORK/stage-$PLATFORM"
OBJ="$WORK/obj-$PLATFORM"
rm -rf "$STAGE" "$OBJ"
mkdir -p "$STAGE" "$OBJ"

# ── AvailabilityVersions ──
# (also installs usr/local/libexec/availability.pl, which xnu's build runs)
cp -R "$AV" "$OBJ/av-src"
grep -v '^#' "$HERE/availability-versions.dsl" >> "$OBJ/av-src/availability.dsl"
# The projects' headers also name visionOS and Mac Catalyst by their
# internal names, xros and iosmac (API_AVAILABLE(xros(1.1)), in
# dispatch/queue.h, ffi/ffi.h ...), which the SDK's build renames: aliases
# of the two, so that availability markup with them compiles
sed -i '' -e '/^platform  *visionos /s/$/ availability_aliases=xros/' \
    -e '/^variant  *ios  *macCatalyst /s/$/ availability_aliases=iosmac/' "$OBJ/av-src/availability.dsl"
cmake -G Ninja -S "$OBJ/av-src" -B "$OBJ/av" \
    -DSRCROOT="$OBJ/av-src" -DOBJROOT="$OBJ/av" -DSYMROOT="$OBJ/av/sym" -DDSTROOT="$STAGE" \
    -DDRIVERKIT=0 -DSYSTEM_PREFIX= -DCONFIG_EXCLAVEKIT=0 -DCONFIG_EXCLAVECORE=0 \
    -DCONFIG_KERNELKIT=0 -DINSTALL_KERNEL_HEADERS=0 -DAV_VERSION="$(tag_of AvailabilityVersions)" >/dev/null
ninja -C "$OBJ/av" install >/dev/null

# ── xnu ──
# Its sys/_symbol_aliasing.h is generated by availability.pl, which it looks
# for in the SDK it builds with: the one just installed instead.
cat > "$OBJ/make_symbol_aliasing.sh" <<EOF
#!/bin/bash
exec "$XNU/bsd/sys/make_symbol_aliasing.sh" "$STAGE" "\$2"
EOF
chmod +x "$OBJ/make_symbol_aliasing.sh"
(cd "$XNU" && make -j"$NPROC" installhdrs SDKROOT="$PLATFORM" ARCH_CONFIGS=ARM64 KERNEL_CONFIGS=RELEASE \
    RC_DARWIN_KERNEL_VERSION="$DARWIN_VERSION" MAKE_SYMBOL_ALIASING="$OBJ/make_symbol_aliasing.sh" \
    SRCROOT="$XNU" OBJROOT="$OBJ/xnu" DSTROOT="$STAGE" >"$OBJ/xnu.log" 2>&1) || {
    cat "$OBJ/xnu.log" >&2
    exit 1
}

# ── xnu's libsyscall: the MIG interfaces and libsystem_kernel's headers ──
mkdir -p "$OBJ/mig/out"
(cd "$XNU/libsyscall" && env SRCROOT="$XNU/libsyscall" OBJROOT="$OBJ/mig" \
    BUILT_PRODUCTS_DIR="$OBJ/mig/out" SDKROOT="$PLATFORM" ARCHS=arm64 PLATFORM_NAME="$PLATFORM" \
    sh xcodescripts/mach_install_mig.sh >"$OBJ/mig.log" 2>&1) || {
    cat "$OBJ/mig.log" >&2
    exit 1
}
# The script copies its headers as they are, kernel and private parts
# included, which xnu's own install takes out (and the SDK's build does for
# these): a program's own PRIVATE or KERNEL macro must not reach them
find "$OBJ/mig/out/mig_hdr/include" -name '*.h' | while read -r h; do
    unifdef -UKERNEL -UKERNEL_PRIVATE -UPRIVATE -UMACH_KERNEL_PRIVATE -UBSD_KERNEL_PRIVATE \
        -o "$h" "$h" || [ $? -eq 1 ]
done
cp -R "$OBJ/mig/out/mig_hdr/include/." "$STAGE/usr/include/"

# ── Libc ──
(cd "$LIBC" && env SRCROOT="$LIBC" DSTROOT="$STAGE" DEPLOYMENT_LOCATION=YES \
    SDK_INSTALL_HEADERS_ROOT= SDK_INSTALL_ROOT= PLATFORM_NAME="$PLATFORM" \
    VARIANT_PLATFORM_NAME="$PLATFORM" ARCHS=arm64 DERIVED_FILES_DIR="$OBJ/libc" BASH=/bin/bash \
    /bin/bash -e xcodescripts/headers.sh >"$OBJ/libc.log" 2>&1) || {
    cat "$OBJ/libc.log" >&2
    exit 1
}

# ── the other projects' public headers ──
grep -v '^#' "$HERE/headers.txt" | while read -r tag file dest; do
    [ -n "$tag" ] || continue
    if [ "$tag" = "=" ]; then
        mkdir -p "$STAGE/usr/include/$(dirname "$dest")"
        ln -sfn "$file" "$STAGE/usr/include/$dest"
        continue
    fi
    if [ ! -f "$WORK/src/$tag/$file" ]; then
        echo "build_sdk: $tag has no $file" >&2
        exit 1
    fi
    mkdir -p "$STAGE/usr/include/$(dirname "$dest")"
    cp "$WORK/src/$tag/$file" "$STAGE/usr/include/$dest"
done

# libxml2's xmlversion.h, which its configure makes of xmlversion.h.in: the
# project's own, generated for the system's build
cp "$(src "$(tag_of libxml2)")/Pregenerated Files/include/libxml/xmlversion.h" "$STAGE/usr/include/libxml/"

# ── ncurses ──
# curses.h and term.h, generated from the terminal capabilities as the
# project's own scripts do (xcodescripts/derived_headers.sh), and the headers
# its install_headers.sh installs, with ncurses.h a link to curses.h. The
# project's sources come configured (configure.sh: wide characters, ABI 5.4)
NC="$(src "$(tag_of ncurses)")"
mkdir -p "$OBJ/ncurses"
(
    cd "$NC" && export AWK=awk && CAPS=ncurses/include/Caps &&
    { cat ncurses/include/curses.head; sh ncurses/include/MKkey_defs.sh "$CAPS";
      cat ncurses/include/curses.wide ncurses/include/curses.tail; } > "$OBJ/ncurses/curses.h" &&
    awk -f ncurses/include/MKterm.h.awk "$CAPS" > "$OBJ/ncurses/term.h" &&
    sh ncurses/include/edit_cfg.sh ncurses/include/ncurses_cfg.h "$OBJ/ncurses/term.h"
) >"$OBJ/ncurses.log" 2>&1 || {
    cat "$OBJ/ncurses.log" >&2
    exit 1
}
for h in include/tic.h menu/eti.h panel/panel.h include/ncurses_dll.h include/unctrl.h include/nc_tparm.h \
         form/form.h include/termcap.h menu/menu.h include/term_entry.h; do
    cp "$NC/ncurses/$h" "$STAGE/usr/include/"
done
cp "$OBJ/ncurses/curses.h" "$OBJ/ncurses/term.h" "$STAGE/usr/include/"
ln -sfn curses.h "$STAGE/usr/include/ncurses.h"

# ── fixes ──
# dyld.h marks its NSObjectFileImage functions DYLD_EXCLAVEKIT_UNAVAILABLE,
# which only Apple's own build defines (the SDK's dyld.h defines it, empty):
# any program including <mach-o/dyld.h> failed to compile. Defined empty
# where the file defines its DriverKit counterpart, with a note in the file
# saying so (APSL 2.0, 2.2(a): a modified file says what changed, and when).
python3 - "$STAGE/usr/include/mach-o/dyld.h" <<'EOF'
import sys
path = sys.argv[1]
s = open(path).read()
anchor = "#ifdef __DRIVERKIT_19_0\n #define DYLD_DRIVERKIT_UNAVAILABLE __API_UNAVAILABLE(driverkit)\n#else\n #define DYLD_DRIVERKIT_UNAVAILABLE\n#endif\n"
if s.count(anchor) != 1:
    sys.exit("build_sdk: dyld.h: DYLD_DRIVERKIT_UNAVAILABLE not where expected")
s = s.replace(anchor, anchor + "\n/* Modified for TVM's SDK (2026-10-18): DYLD_EXCLAVEKIT_UNAVAILABLE, used below\n"
              " * and defined only by Apple's own build, defined empty. */\n"
              "#ifndef DYLD_EXCLAVEKIT_UNAVAILABLE\n #define DYLD_EXCLAVEKIT_UNAVAILABLE\n#endif\n")
open(path, "w").write(s)
EOF

# ── TVM's own ──
for h in TargetConditionals.h math.h fenv.h complex.h; do
    cp "$HERE/$h" "$STAGE/usr/include/$h"
done

# ── the SDK ──
rm -rf "$OUT"
mkdir -p "$OUT/usr/lib"
cp -R "$STAGE/usr/include" "$OUT/usr/"
chmod -R u+w "$OUT"
case "$PLATFORM" in
    macosx)   TRIPLE_SYS=macos; DISPLAY="macOS $SDK_VERSION" ;;
    iphoneos) TRIPLE_SYS=ios;   DISPLAY="iOS $SDK_VERSION" ;;
esac
cat > "$OUT/SDKSettings.json" <<EOF
{
  "CanonicalName": "$PLATFORM$SDK_VERSION",
  "DisplayName": "$DISPLAY (TVM)",
  "Version": "$SDK_VERSION",
  "MaximumDeploymentTarget": "$SDK_VERSION.99",
  "SupportedTargets": {
    "$PLATFORM": {
      "Archs": ["arm64", "arm64e"],
      "LLVMTargetTripleVendor": "apple",
      "LLVMTargetTripleSys": "$TRIPLE_SYS"
    }
  }
}
EOF
python3 "$HERE/gen_tbd.py" "$STUBS_OS" "$OUT/usr/lib"

# ── the notices ──
{
    echo "The headers in usr/include are Apple's open-source releases, unmodified,"
    echo "but for TargetConditionals.h, math.h, fenv.h and complex.h, which were"
    echo "written for TVM (scripts/sdk in TVM's source); the Availability headers,"
    echo "generated by AvailabilityVersions from its version list with the versions"
    echo "since 2023 and two platform aliases added; and mach-o/dyld.h, which says"
    echo "what was changed in it. curses.h and term.h are generated by ncurses' own"
    echo "scripts. The stubs in usr/lib were generated from the system's libraries."
    echo "The projects (https://github.com/apple-oss-distributions; sqlite-autoconf"
    echo "is SQLite's own release, https://www.sqlite.org):"
    echo
    grep -v '^#' "$HERE/sources.txt" | awk 'NF {print "    " $2}'
    echo
    echo "Their licenses follow: the Apple Public Source License 2.0, which most of"
    echo "the headers say they are under, and the license files of the projects"
    echo "that have others. sqlite3.h and sqlite3ext.h are SQLite's"
    echo "(https://www.sqlite.org), which is in the public domain."
    grep -v '^#' "$HERE/sources.txt" | while read -r project tag sha; do
        [ -n "$project" ] || continue
        for f in APPLE_LICENSE LICENSE COPYING bzip2/LICENSE ncurses/COPYING expat/COPYING \
                 libxml2/Copyright libpcap/LICENSE curl/COPYING; do
            [ -f "$WORK/src/$tag/$f" ] || continue
            sum="$(shasum "$WORK/src/$tag/$f" | cut -d' ' -f1)"
            [ -f "$OBJ/notice.$sum" ] && continue
            touch "$OBJ/notice.$sum"
            echo
            echo "======== $tag: $f ========"
            echo
            cat "$WORK/src/$tag/$f"
        done
    done
} > "$OUT/NOTICE.txt"
