import 'dart:ffi';
import 'dart:io' show Platform;

import 'package:ffi/ffi.dart';

typedef _LoadNative = Int32 Function(Pointer<Utf8> path);
typedef _LoadDart = int Function(Pointer<Utf8> path);

typedef _StartNative = Int32 Function(Int32 port, Pointer<Utf8> password);
typedef _StartDart = int Function(int port, Pointer<Utf8> password);

typedef _PollNative = Int32 Function(Pointer<Uint8> buf, Int32 bufSize);
typedef _PollDart = int Function(Pointer<Uint8> buf, int bufSize);

typedef _StopNative = Void Function();
typedef _StopDart = void Function();

class TvmBindings {
  TvmBindings._();
  static final TvmBindings instance = TvmBindings._();

  DynamicLibrary? _lib;
  _LoadDart? _load;
  _StartDart? _start;
  _PollDart? _poll;
  _StopDart? _stop;

  bool get isAvailable => _lib != null;

  String? loadError;

  void init() {
    try {
      _lib = _openLibrary();
      _load = _lib!.lookupFunction<_LoadNative, _LoadDart>('tvm_load');
      _start = _lib!.lookupFunction<_StartNative, _StartDart>('tvm_start');
      _poll = _lib!.lookupFunction<_PollNative, _PollDart>('tvm_poll');
      _stop = _lib!.lookupFunction<_StopNative, _StopDart>('tvm_stop');

      final tvmPath = _tvmLibraryPath();
      if (tvmPath != null) {
        final ptr = tvmPath.toNativeUtf8();
        try {
          if (_load!(ptr) != 0) {
            loadError = 'tvm_load failed for $tvmPath';
            _lib = null;
          }
        } finally {
          malloc.free(ptr);
        }
      }
    } catch (e) {
      loadError = e.toString();
      _lib = null;
    }
  }

  int start(int port, String password) {
    final ptr = password.toNativeUtf8();
    try {
      return _start!(port, ptr);
    } finally {
      malloc.free(ptr);
    }
  }

  String? poll() {
    final buf = malloc<Uint8>(65536);
    try {
      final n = _poll!(buf, 65536);
      if (n <= 0) return null;
      return String.fromCharCodes(buf.asTypedList(n));
    } finally {
      malloc.free(buf);
    }
  }

  void stop() => _stop!();

  static DynamicLibrary _openLibrary() {
    if (Platform.isIOS || Platform.isMacOS) {
      final lib = DynamicLibrary.process();
      // Verify tvm_load is findable
      if (!lib.providesSymbol('tvm_load')) {
        throw Exception('tvm_load not found in process — tvm_bridge.c may not be linked');
      }
      return lib;
    }
    if (Platform.isAndroid || Platform.isLinux) {
      return DynamicLibrary.open('libtvm_bridge.so');
    }
    if (Platform.isWindows) {
      return DynamicLibrary.open('tvm_bridge.dll');
    }
    throw UnsupportedError('Unsupported platform: ${Platform.operatingSystem}');
  }

  static String? _tvmLibraryPath() {
    if (Platform.isMacOS) {
      final exe = Platform.resolvedExecutable;
      final appDir = exe.substring(0, exe.lastIndexOf('/'));
      return '$appDir/../Frameworks/libtvm.dylib';
    }
    if (Platform.isIOS) {
      final exe = Platform.resolvedExecutable;
      final appDir = exe.substring(0, exe.lastIndexOf('/'));
      return '$appDir/Frameworks/libtvm.dylib';
    }
    // Linux/Android: libtvm.so is loaded via libtvm_bridge.so linkage
    // iOS: statically linked
    return null;
  }
}
