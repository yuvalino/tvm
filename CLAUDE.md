This is TVM: Thread-based process virtualization framework that lets sandboxed applicatiosn with no fork() permissions to POSIX compliant programs.

The basic idea here is that fork() creates a new thread group (struct task) that shares resources such as file descriptors and signal handlers. To imitate Copy-On-Write, on fork() memory ranges are duplicated and pointers to old ranges are fixed up to point to the equivilant new ranges.

Dedicated heap and linker were written in tvm/rmem.c and tvm/ulinker.c to optimize the fork() process to be as fast as possible.

The entire VM is implemented in tvm/tvm.c .

In addition, unsigned code execution is made possible when code signing is validated by running the unsigned code parts using Unicorn emulator engine. When the emulator hits a signed region, it traps and lets the real CPU execute that code. The emulator integration too is implemented in tvm/tvm.c .

## Flutter app (iOS / macOS)

`flutter/` is a Flutter app that embeds TVM and runs an SSH server on-device.
- TVM core builds as `libtvm.dylib` (`tvm/CMakeLists.txt`, `tvm_shared` = `$<TARGET_OBJECTS:tvm_static>`). dropbear/mksh/toybox/vim are sidecar dylibs; the rootfs is a per-platform Flutter asset `initrd.flutter.<arch>.cpio` (pubspec `platforms:` bundles only the matching one), built by `scripts/CMakeLists.txt`.
- Native glue: `flutter/tvm/tvm_bindings.c`, compiled into the app binary. Both Xcode projects reference it explicitly via `../tvm/tvm_bindings.c` (macOS `../../tvm/...`); no symlinks. It `dlopen`s libtvm and `dlsym`s `tvm_init_with_outfd`/`tvm_mount_cpio`/`execvp`/`chdir`, then runs the worker on a thread. A single FFI export `tvm_start(...)` takes a `NativeCallable` output callback; there is no `tvm_stop`.
- Dart: `flutter/lib/services/tvm_bindings.dart` (FFI) and `worker_controller.dart` (orchestration, startup info via libc FFI, `_log` → UI + `dart:io` stderr).
- Entitlements: macOS `macOS.entitlements` / `macOS-debug.entitlements` (needs `network.server`); iOS `iOS.entitlements` (`increased-memory-limit`, must be enabled on the App ID). Runner target pinned to team NQ5VVK576N.

### Build / deploy / test on device (pan1c)
The build symlinks the app at `output/<arch>/lib/flutter` — use that (our
`output/<arch>` layout), not the raw `flutter/build/...` path.
```sh
python3 build.py -a ios-arm64 -t flutter
xcrun devicectl device install app --device pan1c output/ios-arm64/lib/flutter
xcrun devicectl device process launch --device pan1c com.yuvalino.tvm
pytest tests/test_integration.py --device pan1c --backend flutter
```

### Seeing device logs — IMPORTANT
Use `xcrun devicectl device process launch --terminate-existing --console --device pan1c com.yuvalino.tvm`. `--terminate-existing` is REQUIRED: devicectl connects the app's stdout/stderr only on a fresh launch ("if the app is not already running"). Plain launch or `--console` on a running app shows nothing. Reason: iOS GUI apps have fd 1/2 → /dev/null and Flutter routes print/debugPrint to os_log, not stdio — so the app deliberately mirrors logs to `dart:io` stderr (the real fd), which `--console` captures. `devicectl` has no unified-log subcommand; for os_log use Console.app / `log stream` / `idevicesyslog` (network-capable after pairing, not just USB). See README "Viewing logs on a device".

