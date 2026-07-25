import 'dart:convert';
import 'dart:ffi';
import 'dart:io' show Platform;

import 'package:ffi/ffi.dart';

typedef _OutputCbNative = Void Function(Pointer<Uint8> data, Int32 len);

typedef _StartNative = Int32 Function(
  Pointer<Utf8> libtvmPath,
  Int32 port,
  Pointer<Utf8> password,
  Pointer<Utf8> initrcPath,
  Pointer<Utf8> initrdPath,
  Pointer<NativeFunction<_OutputCbNative>> onOutput,
);
typedef _StartDart = int Function(
  Pointer<Utf8> libtvmPath,
  int port,
  Pointer<Utf8> password,
  Pointer<Utf8> initrcPath,
  Pointer<Utf8> initrdPath,
  Pointer<NativeFunction<_OutputCbNative>> onOutput,
);

class TvmBindings {
  TvmBindings._();
  static final TvmBindings instance = TvmBindings._();

  DynamicLibrary? _lib;
  _StartDart? _start;
  NativeCallable<_OutputCbNative>? _callback;

  String? loadError;

  bool get isAvailable => _lib != null;

  /// Sink for TVM output. Set by the caller before [start].
  void Function(String)? onOutput;

  void init() {
    try {
      final DynamicLibrary lib;
      if (Platform.isIOS || Platform.isMacOS) {
        // tvm_bindings.c is compiled into the app binary, so tvm_start lives
        // in the process image.
        lib = DynamicLibrary.process();
      } else if (Platform.isAndroid) {
        // tvm_bindings.c is compiled into a standalone libtvm_bindings.so
        // (gradle externalNativeBuild); load it by name.
        lib = DynamicLibrary.open('libtvm_bindings.so');
      } else {
        throw UnsupportedError('Unsupported platform: ${Platform.operatingSystem}');
      }
      if (!lib.providesSymbol('tvm_start')) {
        throw Exception('tvm_start not found — tvm_bindings not linked');
      }
      _lib = lib;
      _start = lib.lookupFunction<_StartNative, _StartDart>('tvm_start');
      // .listener runs the callback on this isolate's event loop, so it is
      // safe to touch Dart state / the UI from it.
      _callback = NativeCallable<_OutputCbNative>.listener(_onNativeOutput);
    } catch (e) {
      loadError = e.toString();
      _lib = null;
    }
  }

  // Called (asynchronously) from the native reader thread. `data` is a
  // malloc'd buffer of `len` bytes that we own and must free.
  void _onNativeOutput(Pointer<Uint8> data, int len) {
    try {
      final bytes = data.asTypedList(len);
      final text = utf8.decode(bytes, allowMalformed: true);
      onOutput?.call(text);
    } finally {
      malloc.free(data);
    }
  }

  /// Starts TVM. Returns 0 on success.
  int start({
    required String libtvmPath,
    required int port,
    required String password,
    required String initrcPath,
    required String initrdPath,
  }) {
    if (_start == null || _callback == null) return -1;

    final pLib = libtvmPath.toNativeUtf8();
    final pPass = password.toNativeUtf8();
    final pInitrc = initrcPath.toNativeUtf8();
    final pInitrd = initrdPath.toNativeUtf8();
    try {
      return _start!(
        pLib,
        port,
        pPass,
        pInitrc,
        pInitrd,
        _callback!.nativeFunction,
      );
    } finally {
      malloc.free(pLib);
      malloc.free(pPass);
      malloc.free(pInitrc);
      malloc.free(pInitrd);
    }
  }
}
