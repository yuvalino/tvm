#!/usr/bin/env python3
"""Write the library stubs (text-based .tbd files) of TVM's SDK.

usage: gen_tbd.py <macos|ios> <usr/lib dir> [<iOS symbols dir>]

A program links against a system library through its stub: the library's
install name and the symbols it exports, nothing else. The Apple SDK's own
stubs are not redistributable, so these are made here, from the libraries
themselves: for macOS, the running system's (its dyld shared cache, read by
`dyld_info -exports`); for iOS, the devices' as Xcode copies them off each when
it is first connected (~/Library/Developer/Xcode/iOS DeviceSupport/<device
and version>/Symbols, read by `nm -gm`): what all of those have, unless a
directory is given. A symbol only a newer iOS has would link, and the
program would then not load on the older one.

libSystem's stub lists, as its own, everything the libraries it re-exports
(libsystem_c, libsystem_kernel, libdispatch ...) export: a program binds to
libSystem, and the loader finds each symbol through its re-exports, as it
does for a program linked against the SDK's stub. The stubs only list
symbols and give no versions (a dependency's compatibility version is then
1.0, which every one of these libraries satisfies).
"""
import glob
import os
import re
import subprocess
import sys

# <install name>  <stub>  <aliases (-l names)>
LIBRARIES = [
    ("/usr/lib/libSystem.B.dylib", "libSystem.B.tbd",
     ["libSystem.tbd", "libc.tbd", "libm.tbd", "libpthread.tbd", "libdl.tbd", "libinfo.tbd",
      "libpoll.tbd", "libproc.tbd", "librpcsvc.tbd", "libgcc_s.1.tbd"]),
    ("/usr/lib/libz.1.dylib", "libz.1.tbd", ["libz.tbd"]),
    ("/usr/lib/libbz2.1.0.dylib", "libbz2.1.0.tbd", ["libbz2.tbd"]),
    ("/usr/lib/libiconv.2.dylib", "libiconv.2.tbd", ["libiconv.tbd"]),
    ("/usr/lib/libcharset.1.dylib", "libcharset.1.tbd", ["libcharset.tbd"]),
    ("/usr/lib/libresolv.9.dylib", "libresolv.9.tbd", ["libresolv.tbd"]),
    ("/usr/lib/libedit.3.dylib", "libedit.3.tbd", ["libedit.tbd", "libreadline.tbd"]),
    ("/usr/lib/libffi.dylib", "libffi.tbd", []),
    ("/usr/lib/libobjc.A.dylib", "libobjc.A.tbd", ["libobjc.tbd"]),
    ("/usr/lib/libncurses.5.4.dylib", "libncurses.5.4.tbd",
     ["libncurses.5.tbd", "libncurses.tbd", "libcurses.tbd", "libtermcap.tbd"]),
    ("/usr/lib/libform.5.4.dylib", "libform.5.4.tbd", ["libform.tbd"]),
    ("/usr/lib/libmenu.5.4.dylib", "libmenu.5.4.tbd", ["libmenu.tbd"]),
    ("/usr/lib/libpanel.5.4.dylib", "libpanel.5.4.tbd", ["libpanel.tbd"]),
    ("/usr/lib/libsqlite3.dylib", "libsqlite3.tbd", ["libsqlite3.0.tbd"]),
    ("/usr/lib/libexpat.1.dylib", "libexpat.1.tbd", ["libexpat.tbd"]),
    ("/usr/lib/libxml2.2.dylib", "libxml2.2.tbd", ["libxml2.tbd"]),
    ("/usr/lib/libcurl.4.dylib", "libcurl.4.tbd", ["libcurl.3.tbd", "libcurl.tbd"]),
    ("/usr/lib/libpcap.A.dylib", "libpcap.A.tbd", ["libpcap.tbd"]),
]


def run(cmd):
    return subprocess.run(cmd, check=True, capture_output=True, text=True).stdout


class MacOS:
    """The running system's libraries, from its shared cache."""

    def has(self, path):
        return subprocess.run(["dyld_info", "-dependents", path], capture_output=True).returncode == 0

    def reexports(self, path):
        out = run(["dyld_info", "-dependents", path])
        return [l.split()[-1] for l in out.splitlines() if l.strip().startswith("re-export")]

    def exports(self, path):
        strong, weak = set(), set()
        for line in run(["dyld_info", "-exports", path]).splitlines():
            line = line.strip()
            m = re.match(r"(?:0x[0-9A-Fa-f]+|\[re-export\])\s+(\S+)(.*)", line)
            if not m:
                continue
            (weak if "[weak-def]" in m.group(2) else strong).add(m.group(1))
        return strong, weak


class IOS:
    """A device's libraries, as Xcode copied them off it."""

    def __init__(self, root):
        self.root = root

    def file(self, path):
        return os.path.join(self.root, path.lstrip("/"))

    def has(self, path):
        return os.path.exists(self.file(path))

    def reexports(self, path):
        out = run(["otool", "-L", self.file(path)])
        return [l.split()[0] for l in out.splitlines()[1:] if "reexport)" in l]

    def exports(self, path):
        strong, weak = set(), set()
        for line in run(["nm", "-gm", self.file(path)]).splitlines():
            if "(undefined)" in line or " external " not in line:
                continue
            name = line.split(" external ", 1)[1].split()[0]
            (weak if "weak external" in line else strong).add(name)
        return strong, weak


class Common:
    """What every one of several sources has: a library all of them have, with
    the symbols all of them export."""

    def __init__(self, sources):
        self.sources = sources

    def has(self, path):
        return all(s.has(path) for s in self.sources)

    def reexports(self, path):
        subs = [set(s.reexports(path)) for s in self.sources]
        return sorted(set.intersection(*subs))

    def exports(self, path):
        each = [s.exports(path) for s in self.sources]
        every = set.intersection(*[st | wk for st, wk in each])
        strong = {n for n in every if all(n in st for st, _ in each)}
        return strong, every - strong


def quote(names):
    return "[ " + ", ".join("'%s'" % n for n in sorted(names)) + " ]"


def write_tbd(path, platform, install_name, strong, weak):
    targets = "[ arm64-%s, arm64e-%s ]" % (platform, platform)
    lines = [
        "--- !tapi-tbd",
        "tbd-version:     4",
        "targets:         %s" % targets,
        "install-name:    '%s'" % install_name,
        "exports:",
        "  - targets:         %s" % targets,
        "    symbols:         %s" % quote(strong),
    ]
    if weak:
        lines.append("    weak-symbols:    %s" % quote(weak))
    lines.append("...")
    with open(path, "w") as f:
        f.write("\n".join(lines) + "\n")


def main():
    if len(sys.argv) not in (3, 4) or sys.argv[1] not in ("macos", "ios"):
        sys.exit(__doc__.strip().splitlines()[2])
    platform, outdir = sys.argv[1], sys.argv[2]
    if platform == "macos":
        source = MacOS()
    else:
        if len(sys.argv) == 4:
            roots = [sys.argv[3]]
        else:
            roots = sorted(glob.glob(os.path.expanduser(
                "~/Library/Developer/Xcode/iOS DeviceSupport/*/Symbols")))
            if not roots:
                sys.exit("gen_tbd: no iOS device symbols (connect a device to Xcode once, "
                         "or pass the Symbols directory of one)")
        source = Common([IOS(r) for r in roots])
        for r in roots:
            print("gen_tbd: iOS libraries from %s" % r)

    os.makedirs(outdir, exist_ok=True)
    for install_name, stub, aliases in LIBRARIES:
        # a library the platform does not have (curses on iOS, say): no stub
        if not source.has(install_name):
            print("gen_tbd: %s: no such library here, no stub" % install_name)
            continue
        strong, weak = source.exports(install_name)
        for sub in source.reexports(install_name):
            s, w = source.exports(sub)
            strong |= s
            weak |= w
        weak -= strong
        write_tbd(os.path.join(outdir, stub), platform, install_name, strong, weak)
        for alias in aliases:
            link = os.path.join(outdir, alias)
            if os.path.lexists(link):
                os.remove(link)
            os.symlink(stub, link)
        print("gen_tbd: %s: %d symbols (%d weak)" % (stub, len(strong) + len(weak), len(weak)))


if __name__ == "__main__":
    main()
