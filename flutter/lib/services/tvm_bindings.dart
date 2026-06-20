import 'dart:ffi';
import 'dart:io' show Platform;

import 'package:ffi/ffi.dart';

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
  _StartDart? _start;
  _PollDart? _poll;
  _StopDart? _stop;

  bool get isAvailable => _lib != null;

  void init() {
    try {
      _lib = _openLibrary();
      _start = _lib!.lookupFunction<_StartNative, _StartDart>('tvm_start');
      _poll = _lib!.lookupFunction<_PollNative, _PollDart>('tvm_poll');
      _stop = _lib!.lookupFunction<_StopNative, _StopDart>('tvm_stop');
    } catch (_) {
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
      return DynamicLibrary.process();
    }
    if (Platform.isAndroid || Platform.isLinux) {
      return DynamicLibrary.open('libtvm_bridge.so');
    }
    if (Platform.isWindows) {
      return DynamicLibrary.open('tvm_bridge.dll');
    }
    throw UnsupportedError('Unsupported platform: ${Platform.operatingSystem}');
  }
}
