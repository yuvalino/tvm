<div align="center">

<picture>
  <source media="(prefers-color-scheme: dark)" srcset="rsc/logo.jpg" />
  <img alt="tvm" src="rsc/logo.jpg" width="96" height="96" />
</picture>

# tvm

**Bypass security sandbox restrictions without use of vulnerabilities.**

An open-source, userspace framework that allows running unsigned code and multi-process workflows in highly sandboxed environments (iOS).

[![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)
[![Release](https://img.shields.io/github/v/release/yuvalino/tvm?sort=semver)](https://github.com/yuvalino/tvm/releases)

[Website](https://yuvalino.com) ·
[Research paper](https://yuvalino.com/rage-against-the-sandbox)

<a href="https://x.com/jachashx"><img alt="Follow Yuval on X" src="https://img.shields.io/badge/X-000000?logo=x&amp;logoColor=white" /></a>

</div>

![tvm demo screenshot](rsc/tvm-demo-screenshot.jpg)

## What's TVM ?

Thread-based process virtualization framework. Lets sandboxed applications with no `fork()` permissions run POSIX-compliant and multi-process reliant software by implementing processes as thread groups.

With TVM, `fork()` creates a new thread group (task) that shares resources like file descriptors and signal handlers. Memory ranges are duplicated with pointer fixup to imitate copy-on-write. A dedicated heap (`rmem.c`) and linker (`ulinker.c`) optimize this to be as fast as possible.

Unsigned code execution is supported via the [Unicorn](https://www.unicorn-engine.org/) CPU emulator. When the emulator hits a signed region, it traps and lets the real CPU take over.

## Building

Requirements: CMake 3.16+, Ninja, Python 3, a C compiler.

On macOS, also install: `brew install gnu-sed openssl`

```sh
# Build TVM CLI executable (default)
python3 build.py

# Build TVM flutter app for iOS
python3 build.py -a ios-arm64 -t flutter

# Build tests
python3 build.py -t tests
```

Output goes to `output/<platform>/bin/` (auto-detected: `linux-x64` or `macos-arm64`).

## Running

```sh
# Run a program in TVM
./output/linux-x64/bin/tvm <program> [args...]

# Run with kernel log output
./output/macos-arm64/bin/tvm --verbose <program>

# Custom initrc
./output/linux-x64/bin/tvm -i path/to/init.rc <program>

# Mount a CPIO archive
./output/linux-x64/bin/tvm -v /tvm:path/to/initrd.cpio <program>
```

## Embedding

`libtvm` exposes one entry point, declared in `tvm/tvm.h`. It boots the VM on
the calling thread and execs the program inside it:

```c
#include "tvm.h"

struct tvm_mount initrd = { .path = "/tvm", .data = cpio_bytes, .size = cpio_len };
struct tvm_config cfg = {
    .log_fd  = STDOUT_FILENO,       // -1 disables kernel logging
    .cwd     = "/some/dir",         // NULL = host cwd
    .initrc  = "/tvm/etc/init.rc",  // NULL = none; may live inside a mount
    .mounts  = &initrd,
    .nmounts = 1,
};
char *argv[] = { "sh", "-l", NULL };
tvm_start("sh", argv, envp, &cfg);  // envp NULL = inherit; cfg NULL = all defaults
perror("tvm_start");                // only reached on a pre-boot failure
```

## Flutter app (iOS / macOS)

`flutter/` is a Flutter app that embeds TVM and runs an SSH server (dropbear)
on-device, letting you `ssh` into a full POSIX environment. The TVM core is
built as `libtvm.dylib`; dropbear/mksh/toybox/vim as dylibs; the
rootfs is a per-platform Flutter asset (`initrd.flutter.<arch>.cpio`). The
native glue (`flutter/tvm/tvm_bindings.c`) `dlopen`s libtvm, resolves
`tvm_start` from it (see [Embedding](#embedding)) and calls it on a worker
thread.

```sh
# Build the app (always release). Pick the platform with -a.
python3 build.py -a ios-arm64   -t flutter
python3 build.py -a macos-arm64 -t flutter
```

The build symlinks the app at `output/<arch>/lib/flutter` (→ `Runner.app` on
iOS, `tvm.app` on macOS) — use that path, consistent with the rest of the
`output/<arch>` layout.

### Running on an iOS device

The device must be paired/trusted (over USB once, then usable over the network
tunnel). Use `xcrun devicectl` with the device name:

```sh
xcrun devicectl device install app --device <device-name> output/ios-arm64/lib/flutter
xcrun devicectl device process launch --device <device-name> com.yuvalino.tvm
# then: ssh -p 2222 mobile@<device-name>.local   (default password: alpine)
```

Signing note: the Runner target is pinned to a development team, and
`iOS.entitlements` requests `increased-memory-limit` — that capability must be
enabled on the App ID in the Apple Developer portal, otherwise signing fails.

### Viewing logs on a device

The app writes its startup info and TVM kernel logs to its in-app console **and**
to real `stderr` (fd 2). To see them from your Mac, launch with
`--terminate-existing --console`:

```sh
xcrun devicectl device process launch --terminate-existing --console \
    --device <device-name> com.yuvalino.tvm
```

**`--terminate-existing` is required.** devicectl only connects the app's
stdout/stderr *"if the app is not already running"* — i.e. on a fresh launch.
A plain launch, or `--console` while the app is already up, shows nothing.

### Testing on a device

```sh
# Runs the integration suite against the app's SSH server on the device.
pytest tests/test_integration.py --device <device-name> --backend flutter
```

## Programs

### Builtin

`tvm` - diagnostics tool (registered at fake `/usr/bin/tvm` path):
- `tvm ps` - task list
- `tvm lsof` - file descriptor tables
- `tvm pwd` - working directory
- `tvm env` - environment variables
- `tvm sorunner` - run shared object
- `tvm cpio` - CPIO archive reader
- `tvm mount` - VFS mount
- `tvm shell` - execute command via `system()`

### Third-party (`third_party/`)

| Program | Description |
|---|---|
| [dropbear](https://github.com/mkj/dropbear) | SSH server, client, and `scp` |
| sftp-server | Standalone SFTPv3 server (exec'd by dropbear for sftp/scp subsystem) |
| [mksh](https://github.com/MirBSD/mksh) | MirBSD Korn Shell |
| [toybox](http://landley.net/toybox/) | Unix utilities (`ls`, `cat`, `grep`, `id`, etc.) |
| [vim](https://github.com/vim/vim) | Text editor |
| [unicorn](https://www.unicorn-engine.org/) | CPU emulator (TCI interpreter mode for iOS) |
| Apple *_cmds | BSD userland from apple-oss-distributions (`ifconfig`, `netstat`, `ps`, `sysctl`, etc.) |

### Examples (`examples/`)

Small test programs: `fork`, `snake`, `stopit`, `hello_jmpbuf`, `gdb_backtrace`.

Build with `python3 build.py -t examples`.

## initrc

TVM runs an init script during `tvm_start()` before the main program starts. The script path is the `initrc` field of `struct tvm_config` (NULL = none); a relative path resolves against the init task's cwd and may live inside a config mount.

The CLI launcher maps its flags onto that field:

| Launcher flags | Behavior |
|---|---|
| (none) | Use `./init.rc` if it exists, silently skip otherwise |
| `-i <path>` / `--initrc <path>` | Use the given path, fail if not found |
| `-n` / `--no-initrc` | No initrc |

Commands:

```sh
mount <hostpath>:<guestpath>   # Mount a CPIO archive into the VFS
setenv <name>=<value>          # Set environment variable (overwrites)
setenv? <name>=<value>         # Set only if not already set
mkdir <path>                   # Create directory (mkdir -p semantics)
umask <mode>                   # Set per-task umask (octal, e.g. 022)
echo <message>                 # Print to kernel log
```

All arguments support variable expansion (`$VAR` or `${VAR}`).

See [INITRC.md](INITRC.md) for the full specification.

## Tests

```sh
# Unit tests (rmem + tvm)
python3 build.py -t tests
./output/linux-x64/bin/tvm ./output/linux-x64/bin/test_tvm
./output/macos-arm64/bin/test_rmem

# Integration tests (pytest)
pip install pytest pexpect
python3 build.py -t initrd
pytest tests/test_integration.py
```

Test runner flags: `-l` list tests, `-k=TEST` filter, `-v` verbose, `-g` debug with gdb.

## Platforms

- Linux x86-64
- macOS arm64
- iOS arm64 (via the Flutter app)
