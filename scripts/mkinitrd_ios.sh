#!/bin/sh
set -e

ROOTFS="$1"
OUTDIR="$2"
SRCDIR="$3"

rm -rf "$ROOTFS" && mkdir -p "$ROOTFS/bin"

# vim runtime only — binaries are loaded from Frameworks/ dylibs on iOS
mkdir -p "$ROOTFS/share/vim/syntax"
cp "$SRCDIR/third_party/vim/runtime/defaults.vim"  "$ROOTFS/share/vim/"
cp "$SRCDIR/third_party/vim/runtime/filetype.vim"  "$ROOTFS/share/vim/"
cp "$SRCDIR/third_party/vim/runtime/ftoff.vim"     "$ROOTFS/share/vim/"
cp "$SRCDIR/third_party/vim/runtime/ftplugin.vim"  "$ROOTFS/share/vim/"
cp "$SRCDIR/third_party/vim/runtime/ftplugof.vim"  "$ROOTFS/share/vim/"
cp "$SRCDIR/third_party/vim/runtime/indent.vim"    "$ROOTFS/share/vim/"
cp "$SRCDIR/third_party/vim/runtime/indoff.vim"    "$ROOTFS/share/vim/"
for f in syntax synload syncolor nosyntax c cpp python sh bash vim conf json yaml xml html css javascript markdown make diff help; do
    [ -f "$SRCDIR/third_party/vim/runtime/syntax/$f.vim" ] && \
        cp "$SRCDIR/third_party/vim/runtime/syntax/$f.vim" "$ROOTFS/share/vim/syntax/"
done
mkdir -p "$ROOTFS/share/vim/autoload/dist"
cp "$SRCDIR/third_party/vim/runtime/autoload/dist/ft.vim" "$ROOTFS/share/vim/autoload/dist/"
mkdir -p "$ROOTFS/share/vim/colors/lists"
cp "$SRCDIR/third_party/vim/runtime/colors/lists/default.vim" "$ROOTFS/share/vim/colors/lists/"

# toybox applet list — get from the macOS-built toybox
TOYBOX="$SRCDIR/third_party/toybox/toybox"
if [ -x "$TOYBOX" ]; then
    for cmd in $("$TOYBOX"); do
        [ ! -e "$ROOTFS/bin/$cmd" ] && touch "$ROOTFS/bin/$cmd"
    done
fi

# create cpio archive using the host cpio (no toybox needed in the initrd)
cd "$ROOTFS" && find . | cpio -o -H newc > "$OUTDIR/initrd.cpio"
