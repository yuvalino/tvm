#!/usr/bin/env python3
"""Build script for TVM. Wraps CMake + Ninja."""

import argparse
import os
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

TARGET_MAP = {
    "tvm":           "tvm",
    "libtvm":        "tvm_static",
    "test_tvm":      "test_tvm",
    "test_rmem":     "test_rmem",
    "tests":         "tests",
    "dropbear":      "dropbear",
    "mksh":          "mksh",
    "toybox":        "toybox",
    "vim":           "vim",
    "initrd":        "initrd",
    "snake":         "example_snake",
    "fork":          "example_fork",
    "stopit":        "example_stopit",
    "hello_jmpbuf":  "example_hello_jmpbuf",
    "gdb_backtrace": "example_gdb_backtrace",
    "examples":      "examples",
}


def project_root():
    return Path(__file__).resolve().parent


def output_dir(platform):
    return project_root() / "output" / platform


def build_dir(platform):
    return output_dir(platform) / "build"


def run(cmd, **kwargs):
    print(f"+ {' '.join(cmd)}", file=sys.stderr)
    subprocess.check_call(cmd, **kwargs)


def configure(platform):
    bd = build_dir(platform)
    bd.mkdir(parents=True, exist_ok=True)

    od = output_dir(platform)
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


def build(platform, targets, verbose=False):
    bd = build_dir(platform)
    if not (bd / "build.ninja").exists():
        configure(platform)

    cmake_targets = []
    for t in targets:
        if t not in TARGET_MAP:
            print(f"error: unknown target '{t}'", file=sys.stderr)
            print(f"available targets: {', '.join(sorted(TARGET_MAP))}", file=sys.stderr)
            sys.exit(1)
        cmake_targets.append(TARGET_MAP[t])

    ninja_args = ["ninja", "-C", str(bd)]
    if verbose:
        ninja_args.append("-v")
    ninja_args.extend(cmake_targets)

    run(ninja_args)


def clean(platform):
    od = output_dir(platform)
    if od.exists():
        shutil.rmtree(od)
        print(f"removed {od}", file=sys.stderr)


def main():
    parser = argparse.ArgumentParser(description="Build TVM")
    parser.add_argument("-a", default=None, choices=sorted(PLATFORMS),
                        help="platform (default: auto-detect)")
    parser.add_argument("-t", nargs="+", default=["tvm"],
                        help="build target(s) (default: tvm)")
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
