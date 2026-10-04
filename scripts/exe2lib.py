#!/usr/bin/env python3
"""Turn a thin arm64 Mach-O executable into a dylib, keeping its code.

usage: exe2lib.py <executable> <output.dylib> [<install-name>]

The Flutter apps load each tool as lib<tool>.dylib from the app bundle's
Frameworks directory (an App Store bundle may contain no standalone
executables, ITMS-90171), while the tools are built the upstream way as
executables. This rewrites only the executable's metadata so the result is
a regular dylib, as ld would have written it:

- the header's file type becomes MH_DYLIB; MH_PIE (executables only) is
  cleared and MH_NO_REEXPORTED_DYLIBS set, as ld does for a dylib that
  re-exports nothing;
- __PAGEZERO is removed (dyld refuses a dylib that maps page zero), and with
  it the chained-fixup starts entry of that segment, which LIEF leaves in
  place (`dyld_info -fixups`: "seg_count exceeds number of segments");
- LC_MAIN and LC_LOAD_DYLINKER (executables only) are removed and an
  LC_ID_DYLIB is added right after the segments, where ld puts it;
- the old code signature is removed; sign the result afterwards
  (codesign --force), as the app's build phase does for every dylib.

Segment addresses are left alone (__TEXT stays at 0x100000000, a valid
preferred load address for a slid dylib), so code, data, symbols and fixup
targets are byte-for-byte the executable's. TVM finds the entry point by its
symbol (`main`), not through LC_MAIN, so the tool runs as before.

Needs LIEF (pip install lief).
"""
import os
import struct
import sys

import lief

LC_SEGMENT_64 = 0x19
LC_DYLD_CHAINED_FIXUPS = 0x80000034


def die(msg):
    sys.exit(f"exe2lib: {msg}")


def fix_chained_starts(path, removed_index):
    """Drop the starts entry of segment `removed_index` from the chained
    fixups. The per-segment entries are offsets from the starts structure to
    each segment's starts, so the remaining ones keep their values; only
    their positions shift down by one."""
    with open(path, "r+b") as f:
        data = bytearray(f.read())
        ncmds, _ = struct.unpack_from("<II", data, 16)
        off, nsegs, fixups = 32, 0, None
        for _ in range(ncmds):
            cmd, size = struct.unpack_from("<II", data, off)
            if cmd == LC_SEGMENT_64:
                nsegs += 1
            elif cmd == LC_DYLD_CHAINED_FIXUPS:
                fixups = struct.unpack_from("<I", data, off + 8)[0]
            off += size
        if fixups is None:
            return
        starts = fixups + struct.unpack_from("<I", data, fixups + 4)[0]
        seg_count = struct.unpack_from("<I", data, starts)[0]
        if seg_count == nsegs:
            return  # already consistent (LIEF fixed it itself)
        offsets = list(struct.unpack_from(f"<{seg_count}I", data, starts + 4))
        if seg_count != nsegs + 1 or offsets[removed_index] != 0:
            die(f"{path}: unexpected chained fixups (seg_count {seg_count}, "
                f"{nsegs} segments, removed entry {offsets[removed_index]:#x})")
        del offsets[removed_index]
        struct.pack_into(f"<I{len(offsets)}I", data, starts, len(offsets), *offsets)
        f.seek(0)
        f.write(data)


def main():
    if len(sys.argv) not in (3, 4):
        die("usage: exe2lib.py <executable> <output.dylib> [<install-name>]")
    src, dst = sys.argv[1], sys.argv[2]
    install_name = sys.argv[3] if len(sys.argv) == 4 else "@rpath/" + os.path.basename(dst)

    M = lief.MachO
    fat = M.parse(src)
    if fat is None or fat.size != 1:
        die(f"{src}: not a thin Mach-O")
    b = fat.at(0)
    if b.header.file_type != M.Header.FILE_TYPE.EXECUTE:
        die(f"{src}: not an executable ({b.header.file_type})")
    if not b.header.has(M.Header.FLAGS.PIE):
        die(f"{src}: not position independent (link with -pie)")

    for c in list(b.commands):
        if c.command in (M.LoadCommand.TYPE.MAIN, M.LoadCommand.TYPE.LOAD_DYLINKER):
            b.remove(c)

    removed_index = None
    segs = list(b.segments)
    for i, seg in enumerate(segs):
        if seg.name == "__PAGEZERO":
            removed_index = i
            if not b.remove(seg):
                die(f"{src}: could not remove __PAGEZERO")
            break

    if b.has_code_signature:
        b.remove_signature()

    b.header.file_type = M.Header.FILE_TYPE.DYLIB
    b.header.remove(M.Header.FLAGS.PIE)
    b.header.add(M.Header.FLAGS.NO_REEXPORTED_DYLIBS)

    nsegs = len([c for c in b.commands if c.command == M.LoadCommand.TYPE.SEGMENT_64])
    b.add(M.DylibCommand.id_dylib(install_name, 2, 0x10000, 0x10000), nsegs)

    if os.path.lexists(dst):
        os.remove(dst)  # a previous output may be read-only
    b.write(dst)
    if removed_index is not None:
        fix_chained_starts(dst, removed_index)
    # LIEF writes 0644; ld gives dylibs 0755, and TVM's execve wants the x
    # bit (not the source's mode: toybox's executable is read-only)
    os.chmod(dst, 0o755)


if __name__ == "__main__":
    main()
