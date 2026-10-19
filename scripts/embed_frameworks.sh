#!/bin/sh
# Xcode build phase of the Flutter apps: embed TVM and the tools' frameworks
# (output/<platform>/lib/<name>.framework, see mkframework.sh) in the app
# bundle's Frameworks directory, stamped with the app's version and signed
# with the app's identity; and copy the Python standard library
# (output/<platform>/lib/python, the python_stdlib target) and clang's
# toolchain bundle (output/<platform>/lib/llvm, llvm_bundle) into the bundle's
# resources, where init.flutter.rc binds them at /tvm/lib and /tvm/llvm.
#
# usage: embed_frameworks.sh <ios-arm64|macos-arm64>   (from the Runner target)
set -e

PLATFORM="$1"
HERE="$(cd "$(dirname "$0")" && pwd)"
TVM_LIB="$HERE/../output/$PLATFORM/lib"
DEST="${BUILT_PRODUCTS_DIR}/${FRAMEWORKS_FOLDER_PATH}"
SIGN_ID="${EXPANDED_CODE_SIGN_IDENTITY:--}"

mkdir -p "$DEST"
for name in libtvm libdropbear libsftpserver libbash libtoybox libvim libcurl libless libnano libsqlite libgit libgit-remote-http libgit-http-fetch libpython libllvm libnode librg libbun; do
    SRC="$TVM_LIB/$name.framework"
    # the loose dylib an earlier build of this tree embedded
    rm -f "$DEST/$name.dylib"
    [ -d "$SRC" ] || continue

    FW="$DEST/$name.framework"
    rm -rf "$FW"
    cp -R "$SRC" "$FW"

    PLIST="$FW/Info.plist"
    [ -f "$PLIST" ] || PLIST="$FW/Versions/A/Resources/Info.plist"
    [ -z "${FLUTTER_BUILD_NAME:-}" ] || plutil -replace CFBundleShortVersionString -string "$FLUTTER_BUILD_NAME" "$PLIST"
    [ -z "${FLUTTER_BUILD_NUMBER:-}" ] || plutil -replace CFBundleVersion -string "$FLUTTER_BUILD_NUMBER" "$PLIST"

    codesign --force --sign "$SIGN_ID" "$FW"
done

# The Python standard library and clang's toolchain bundle (its resource
# directory and the SDK): plain files, sealed by the app's own signature like
# any other resource.
for res in python llvm node; do
    if [ -d "$TVM_LIB/$res" ]; then
        RES="${BUILT_PRODUCTS_DIR}/${UNLOCALIZED_RESOURCES_FOLDER_PATH}/$res"
        rm -rf "$RES"
        cp -R "$TVM_LIB/$res" "$RES"
    fi
done
