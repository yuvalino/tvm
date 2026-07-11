#!/usr/bin/env python3
"""Build script for TVM. Wraps CMake + Ninja."""

import argparse
import os
import platform
import shutil
import subprocess
import sys
from pathlib import Path

PLATFORMS = {"linux-x64", "macos-arm64", "ios-arm64"}


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
    cmake_args = [
        "cmake",
        "-G", "Ninja",
        "-S", str(project_root()),
        "-B", str(bd),
        "-DCMAKE_BUILD_TYPE=Debug",
        f"-DCMAKE_RUNTIME_OUTPUT_DIRECTORY={od}/bin",
        f"-DCMAKE_ARCHIVE_OUTPUT_DIRECTORY={od}/lib",
    ]

    if plat == "ios-arm64":
        os.environ["ARCHFLAGS"] = "-arch arm64"
        os.environ["TVM_IOS_CROSS"] = "1"
        cmake_args += [
            "-DCMAKE_SYSTEM_NAME=iOS",
            "-DCMAKE_OSX_ARCHITECTURES=arm64",
            "-DCMAKE_OSX_DEPLOYMENT_TARGET=15.0",
        ]

    run(cmake_args)

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


def list_targets(plat):
    bd = build_dir(plat)
    if not (bd / "build.ninja").exists():
        configure(plat)
    result = subprocess.run(
        ["ninja", "-C", str(bd), "-t", "targets", "all"],
        capture_output=True, text=True,
    )
    skip = {"all", "edit_cache", "rebuild_cache", "CMakeCache.txt", "bundling_target"}
    for line in sorted(result.stdout.splitlines()):
        name, _, rule = line.partition(":")
        if "/" not in name and rule.strip() == "phony" \
                and not name.startswith(("cmake_", "lib")) \
                and name not in skip:
            print(name)


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
    parser.add_argument("-l", action="store_true",
                        help="list available targets")
    args = parser.parse_args()

    if args.a is None:
        args.a = detect_platform()

    if args.l:
        list_targets(args.a)
        return

    if args.clean:
        clean(args.a)

    build(args.a, args.t, verbose=args.verbose)


if __name__ == "__main__":
    main()
