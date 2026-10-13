#!/bin/sh
# Wrap one image of the Flutter apps in a framework bundle.
#
# usage: mkframework.sh <ios|macos> <exe|dylib> <name> <input> <outdir> [<min-os>]
#
# An App Store bundle may hold no code outside the app's own executable and
# its frameworks (ITMS-90171: no standalone executable or .dylib), so each
# tool and TVM itself is <outdir>/<name>.framework (libbash.framework, ...),
# its binary named <name>:
#
#   ios     <name>.framework/<name>, Info.plist            (a shallow bundle)
#   macos   <name>.framework/Versions/A/<name>, Versions/A/Resources/Info.plist,
#           with the Versions/Current, <name> and Resources links
#
# An `exe` input is a tool's upstream executable, converted by exe2lib.py; a
# `dylib` input (libtvm) is copied. Either way the install name is the
# binary's place in the framework. Unsigned: the app's build phase signs each
# framework as it embeds it (scripts/embed_frameworks.sh).
set -e

[ $# -ge 5 ] || { echo "usage: mkframework.sh <ios|macos> <exe|dylib> <name> <input> <outdir> [<min-os>]" >&2; exit 1; }
PLATFORM="$1"
KIND="$2"
NAME="$3"
SRC="$4"
OUTDIR="$5"
MINOS="${6:-}"

HERE="$(cd "$(dirname "$0")" && pwd)"
FW="$OUTDIR/$NAME.framework"

case "$PLATFORM" in
    ios)
        BINDIR="$FW"
        PLISTDIR="$FW"
        ID="@rpath/$NAME.framework/$NAME"
        SDK=iPhoneOS
        MINOS_KEY=MinimumOSVersion
        ;;
    macos)
        BINDIR="$FW/Versions/A"
        PLISTDIR="$BINDIR/Resources"
        ID="@rpath/$NAME.framework/Versions/A/$NAME"
        SDK=MacOSX
        MINOS_KEY=LSMinimumSystemVersion
        ;;
    *)
        echo "mkframework: unknown platform '$PLATFORM'" >&2
        exit 1
        ;;
esac

rm -rf "$FW"
mkdir -p "$BINDIR" "$PLISTDIR"

case "$KIND" in
    exe)
        "${PYTHON3:-python3}" "$HERE/exe2lib.py" "$SRC" "$BINDIR/$NAME" "$ID"
        ;;
    dylib)
        cp "$SRC" "$BINDIR/$NAME"
        chmod 755 "$BINDIR/$NAME"
        # drops the linker's ad-hoc signature, as exe2lib.py does
        install_name_tool -id "$ID" "$BINDIR/$NAME" 2>/dev/null
        ;;
    *)
        echo "mkframework: unknown input kind '$KIND'" >&2
        exit 1
        ;;
esac

if [ "$PLATFORM" = macos ]; then
    ln -s A "$FW/Versions/Current"
    ln -s "Versions/Current/$NAME" "$FW/$NAME"
    ln -s Versions/Current/Resources "$FW/Resources"
fi

MINOS_ENTRY=""
if [ -n "$MINOS" ]; then
    MINOS_ENTRY="	<key>$MINOS_KEY</key>
	<string>$MINOS</string>
"
fi

# The versions are placeholders: embed_frameworks.sh sets the app's.
cat > "$PLISTDIR/Info.plist" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
	<key>CFBundleDevelopmentRegion</key>
	<string>en</string>
	<key>CFBundleExecutable</key>
	<string>$NAME</string>
	<key>CFBundleIdentifier</key>
	<string>com.yuvalino.tvm.framework.$NAME</string>
	<key>CFBundleInfoDictionaryVersion</key>
	<string>6.0</string>
	<key>CFBundleName</key>
	<string>$NAME</string>
	<key>CFBundlePackageType</key>
	<string>FMWK</string>
	<key>CFBundleShortVersionString</key>
	<string>1.0</string>
	<key>CFBundleVersion</key>
	<string>1</string>
	<key>CFBundleSupportedPlatforms</key>
	<array>
		<string>$SDK</string>
	</array>
$MINOS_ENTRY</dict>
</plist>
EOF
