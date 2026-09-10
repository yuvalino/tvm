# TVM - Thread Virtual Machine

![TembleOS](./rsc/logo.jpg)

Thread-based process virtualization framework. Lets sandboxed applications with no `fork()` permissions run POSIX-compliant programs by implementing processes as thread groups.

`fork()` creates a new thread group (task) that shares resources like file descriptors and signal handlers. Memory ranges are duplicated with pointer fixup to imitate copy-on-write. A dedicated heap (`rmem.c`) and linker (`ulinker.c`) optimize this to be as fast as possible.

Unsigned code execution is supported via the [Unicorn](https://www.unicorn-engine.org/) CPU emulator. When the emulator hits a signed region, it traps and lets the real CPU take over. The `TVM_NOEMU` environment variable controls which programs skip emulation.

## Building

Requirements: CMake 3.16+, Ninja, Python 3, a C compiler.

On macOS, also install: `brew install gnu-sed openssl`

```sh
# Build TVM executable (default)
python3 build.py

# Build specific targets
python3 build.py -t dropbear mksh toybox vim

# Build the initrd (rootfs CPIO archive with all third-party programs)
python3 build.py -t initrd

# Build tests
python3 build.py -t tests

# Build examples
python3 build.py -t examples

# Clean and rebuild
python3 build.py --clean
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

# Skip initrc
./output/macos-arm64/bin/tvm -n <program>

# Mount a CPIO archive
./output/linux-x64/bin/tvm -v /tvm:path/to/initrd.cpio <program>
```

## Flutter app (iOS / macOS)

`flutter/` is a Flutter app that embeds TVM and runs an SSH server (dropbear)
on-device, letting you `ssh` into a full POSIX environment. The TVM core is
built as `libtvm.dylib`; dropbear/mksh/toybox/vim ship as sidecar dylibs; the
rootfs is a per-platform Flutter asset (`initrd.flutter.<arch>.cpio`). The
native glue (`flutter/tvm/tvm_bindings.c`) `dlopen`s libtvm and runs it on a
worker thread.

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

### Cross-platform Docker builds

All platforms can be built from Linux using `ghcr.io/yuvalino/flutter-xc2-ndk:latest` — a self-contained image with Flutter, xcbuild2, Xcode SDK, LLVM, Swift, and Android SDK/NDK.

```sh
docker run --rm --user "$(id -u):$(id -g)" \
  -v "$PWD":"$PWD" -w "$PWD" \
  ghcr.io/yuvalino/flutter-xc2-ndk:latest \
  bash -c "flutter pub get && flutter build ios --release"
# Android: flutter build apk --release
# Both:    flutter build ios --release && flutter build apk --release
```

### iOS project notes

- **Bundle ID:** `com.yuvalino.tvm`
- **No storyboards** — `AppDelegate.swift` creates `UIWindow` and `FlutterViewController` programmatically (xcbuild2 doesn't have `ibtool`).
- **No CocoaPods** — uses Flutter's SPM integration (`FlutterGeneratedPluginSwiftPackage`).
- **`PBXFileSystemSynchronizedRootGroup`** — xcbuild2 discovers sources by scanning `Runner/` on disk.

## Programs

### Builtin

`tvm` - diagnostics tool (registered at `/usr/bin/tvm`):
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

TVM runs an init script during `tvm_init()` before the main program starts. By default it looks for `./init.rc` in the current directory.

| Environment variable | Behavior |
|---|---|
| (neither set) | Try `./init.rc`, silently skip if not found |
| `TVM_INITRC=<path>` | Load from given path, error if not found |
| `TVM_NOINITRC=1` | Skip initrc entirely |

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
