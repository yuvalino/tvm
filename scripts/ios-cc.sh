#!/bin/sh
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
STUBS_DIR="$SCRIPT_DIR/../stubs"
exec xcrun --sdk iphoneos cc -arch arm64 -miphoneos-version-min=15.0 -I"$STUBS_DIR" "$@"
