#!/bin/sh
exec xcrun --sdk iphoneos cc -arch arm64 -miphoneos-version-min=15.0 "$@"
