#!/bin/sh
set -e

ROOTFS="$1"
OUTDIR="$2"
SRCDIR="$3"

rm -rf "$ROOTFS" && mkdir -p "$ROOTFS/bin"

# copy binaries
cp "$SRCDIR/third_party/dropbear/dropbearmulti" "$ROOTFS/bin/dropbear"
cp "$SRCDIR/third_party/mksh/mksh"              "$ROOTFS/bin/sh"
cp "$SRCDIR/third_party/toybox/toybox"           "$ROOTFS/bin/toybox"
cp "$SRCDIR/third_party/vim/src/vim"             "$ROOTFS/bin/vim"
ln -sf vim "$ROOTFS/bin/vi"

# vim runtime
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

# toybox applet symlinks
cd "$ROOTFS"
for cmd in $(./bin/toybox); do
    [ ! -e "bin/$cmd" ] && ln -sf toybox "bin/$cmd"
done

# create cpio archive
cd "$ROOTFS" && find . | ./bin/toybox cpio -o -H newc > "$OUTDIR/initrd.cpio"
