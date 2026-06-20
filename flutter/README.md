# TVM Flutter

Flutter frontend for TVM (TembleOS Virtual Machine). Provides a console UI with an embedded SSH server, matching the native iOS `devtool` app.

## Building

All builds use `ghcr.io/yuvalino/flutter-xc2-ndk:latest` — a self-contained image with Flutter 3.41.7, xcbuild2, Xcode SDK, LLVM toolchain, Swift, Android SDK/NDK, and signing credentials.

### iOS

```bash
# From WSL (copy to Linux filesystem for speed)
cp -r /mnt/c/Users/Yuval/Development/tvm-flutter ~/Development/tvm-flutter
cd ~/Development/tvm-flutter

docker run --rm --user "$(id -u):$(id -g)" \
  -v "$PWD":"$PWD" \
  -w "$PWD" \
  ghcr.io/yuvalino/flutter-xc2-ndk:latest \
  bash -c "flutter pub get && flutter build ios --release"
```

Output: `build/ios/Products/Release-iphoneos/Runner.app`

Add `--no-codesign` to skip signing (unsigned builds can't be installed on devices).

### Android

```bash
docker run --rm --user "$(id -u):$(id -g)" \
  -v "$PWD":"$PWD" \
  -w "$PWD" \
  ghcr.io/yuvalino/flutter-xc2-ndk:latest \
  bash -c "flutter pub get && flutter build apk --release"
```

Output: `build/app/outputs/flutter-apk/app-release.apk`

### Windows

Requires Flutter 3.41.7 installed natively (can't cross-compile from Linux Docker).

```bash
flutter pub get
flutter build windows --release
```

Output: `build/windows/x64/runner/Release/tvm.exe`

### Both iOS + Android (single container)

```bash
docker run --rm --user "$(id -u):$(id -g)" \
  -v "$PWD":"$PWD" \
  -w "$PWD" \
  ghcr.io/yuvalino/flutter-xc2-ndk:latest \
  bash -c "flutter pub get && flutter build ios --release && flutter build apk --release"
```

## TVM native library

The app calls TVM via `dart:ffi` through a thin C bridge (`native/tvm_bridge.c`). Without TVM libraries, the app builds and runs but prints `[WARN] TVM native library not loaded`.

To enable TVM, provide pre-built libraries and uncomment the linker flags:

**iOS** — place `.a` files under `tvm/lib/ios/` and uncomment `ios/Flutter/Tvm.xcconfig`.

**Android** — place `.a` files under `tvm/lib/android/<abi>/` (arm64-v8a, armeabi-v7a, x86_64) and uncomment the `target_link_libraries` block in `android/app/src/main/CMakeLists.txt`.

**Windows** — place `.lib` files under `tvm/lib/windows/x64/` and uncomment the `target_link_libraries` block in `windows/CMakeLists.txt`.

The bridge expects a single extern: `int tvm_main(int outfd, int port)` — provided by your TVM build (replaces `tvm_devtool` from devtool-ios).

## iOS project notes

- **Bundle ID:** `com.yuvalino.tvm`
- **No storyboards** — `AppDelegate.swift` creates `UIWindow` and `FlutterViewController` programmatically (xcbuild2 doesn't have `ibtool`).
- **No CocoaPods** — uses Flutter's SPM integration (`FlutterGeneratedPluginSwiftPackage`).
- **`PBXFileSystemSynchronizedRootGroup`** — xcbuild2 discovers sources by scanning `Runner/` on disk.
