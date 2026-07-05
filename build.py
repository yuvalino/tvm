#!/usr/bin/env python3
"""Build script for TVM. Wraps CMake + Ninja."""

import argparse
import platform
import shutil
import subprocess
import sys
from pathlib import Path

PLATFORMS = {"linux-x64", "macos-arm64"}


def detect_platform():
    s = platform.system()
    if s == "Darwin":
        return "macos-arm64"
    elif s == "Linux":
        return "linux-x64"
    else:
        print(f"error: unsupported platform '{s}'", file=sys.stderr)
        sys.exit(1)


def project_root():
    return Path(__file__).resolve().parent


def output_dir(plat):
    return project_root() / "output" / plat


def build_dir(plat):
    return output_dir(plat) / "build"


def run(cmd, **kwargs):
    print(f"+ {' '.join(cmd)}", file=sys.stderr)
    subprocess.check_call(cmd, **kwargs)


def configure(plat):
    bd = build_dir(plat)
    bd.mkdir(parents=True, exist_ok=True)

    od = output_dir(plat)
    run([
        "cmake",
        "-G", "Ninja",
        "-S", str(project_root()),
        "-B", str(bd),
        "-DCMAKE_BUILD_TYPE=Debug",
        f"-DCMAKE_RUNTIME_OUTPUT_DIRECTORY={od}/bin",
        f"-DCMAKE_ARCHIVE_OUTPUT_DIRECTORY={od}/lib",
    ])

    cc_json = bd / "compile_commands.json"
    cc_link = project_root() / "compile_commands.json"
    if cc_json.exists():
        cc_link.unlink(missing_ok=True)
        cc_link.symlink_to(cc_json)


def build(plat, targets, verbose=False):
    bd = build_dir(plat)
    if not (bd / "build.ninja").exists():
        configure(plat)

    ninja_args = ["ninja", "-C", str(bd)]
    if verbose:
        ninja_args.append("-v")
    ninja_args.extend(targets)

    run(ninja_args)


def clean(plat):
    od = output_dir(plat)
    if od.exists():
        shutil.rmtree(od)
        print(f"removed {od}", file=sys.stderr)


def main():
    parser = argparse.ArgumentParser(description="Build TVM")
    parser.add_argument("-a", default=None, choices=sorted(PLATFORMS),
                        help="platform (default: auto-detect)")
    parser.add_argument("-t", nargs="+", default=["tvm"],
                        help="cmake/ninja target(s) (default: tvm)")
    parser.add_argument("--clean", action="store_true",
                        help="remove build directory before building")
    parser.add_argument("--verbose", action="store_true",
                        help="show full build commands")
    args = parser.parse_args()

    if args.a is None:
        args.a = detect_platform()

    if args.clean:
        clean(args.a)

    build(args.a, args.t, verbose=args.verbose)


if __name__ == "__main__":
    main()
