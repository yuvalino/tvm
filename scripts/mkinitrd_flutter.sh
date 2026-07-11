#!/bin/sh
set -e

ROOTFS="$1"
OUTDIR="$2"
SRCDIR="$3"
BINDIR="$4"       # path to bin/ dir with pre-built launchers
HOST_TOYBOX="$5"  # path to host-built toybox (for applet list)

rm -rf "$ROOTFS" && mkdir -p "$ROOTFS/bin"

# Copy pre-built launchers, renaming from x_launcher to the initrd name
cp "$BINDIR/dropbear_launcher" "$ROOTFS/bin/dropbear"
cp "$BINDIR/mksh_launcher"    "$ROOTFS/bin/sh"
cp "$BINDIR/toybox_launcher"  "$ROOTFS/bin/toybox"
cp "$BINDIR/vim_launcher"     "$ROOTFS/bin/vim"
ln -sf vim "$ROOTFS/bin/vi"

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
if [ -x "$HOST_TOYBOX" ]; then
    for cmd in $("$HOST_TOYBOX"); do
        [ ! -e "bin/$cmd" ] && ln -sf toybox "bin/$cmd"
    done
fi

# vim runtime
cd "$SRCDIR"
mkdir -p "$ROOTFS/share/vim/syntax"
cp third_party/vim/runtime/defaults.vim  "$ROOTFS/share/vim/"
cp third_party/vim/runtime/filetype.vim  "$ROOTFS/share/vim/"
cp third_party/vim/runtime/ftoff.vim     "$ROOTFS/share/vim/"
cp third_party/vim/runtime/ftplugin.vim  "$ROOTFS/share/vim/"
cp third_party/vim/runtime/ftplugof.vim  "$ROOTFS/share/vim/"
cp third_party/vim/runtime/indent.vim    "$ROOTFS/share/vim/"
cp third_party/vim/runtime/indoff.vim    "$ROOTFS/share/vim/"
for f in syntax synload syncolor nosyntax c cpp python sh bash vim conf json yaml xml html css javascript markdown make diff help; do
    [ -f "third_party/vim/runtime/syntax/$f.vim" ] && \
        cp "third_party/vim/runtime/syntax/$f.vim" "$ROOTFS/share/vim/syntax/"
done
mkdir -p "$ROOTFS/share/vim/autoload/dist"
cp third_party/vim/runtime/autoload/dist/ft.vim "$ROOTFS/share/vim/autoload/dist/"
mkdir -p "$ROOTFS/share/vim/colors/lists"
cp third_party/vim/runtime/colors/lists/default.vim "$ROOTFS/share/vim/colors/lists/"

# create cpio archive
cd "$ROOTFS" && find . | cpio -o -H newc > "$OUTDIR/initrd.cpio"
