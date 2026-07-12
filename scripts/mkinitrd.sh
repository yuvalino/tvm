#!/bin/sh
set -e

ROOTFS="$1"
OUTDIR="$2"
SRCDIR="$3"
BUILDDIR="$4"  # third_party build dir (e.g. output/<platform>/build/third_party)

rm -rf "$ROOTFS" && mkdir -p "$ROOTFS/bin"

# copy binaries from build dir
cp "$BUILDDIR/dropbear/dropbearmulti" "$ROOTFS/bin/dropbear"
cp "$BUILDDIR/mksh/mksh"             "$ROOTFS/bin/sh"
cp "$BUILDDIR/toybox/toybox"         "$ROOTFS/bin/toybox"
cp "$BUILDDIR/vim/vim"               "$ROOTFS/bin/vim"
ln -sf vim "$ROOTFS/bin/vi"

# vim runtime (from source)
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

# dropbear multi-call symlinks
cd "$ROOTFS/bin"
ln -sf dropbear scp
ln -sf dropbear dbclient
ln -sf dropbear ssh
ln -sf dropbear dropbearkey
ln -sf dropbear ssh-keygen
ln -sf dropbear sshd

# toybox applet symlinks (from static list)
APPLETS="$SRCDIR/scripts/toybox_applets.list"
cd "$ROOTFS"
while read -r cmd; do
    [ -n "$cmd" ] && [ ! -e "bin/$cmd" ] && ln -sf toybox "bin/$cmd"
done < "$APPLETS"

# create cpio archive
cd "$ROOTFS" && find . | cpio -o -H newc > "$OUTDIR/initrd-cli.cpio"
