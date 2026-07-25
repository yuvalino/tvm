import 'dart:async';
import 'dart:ffi';
import 'dart:io';

import 'package:ffi/ffi.dart';
import 'package:flutter/foundation.dart';
import 'package:flutter/services.dart';
import 'package:shared_preferences/shared_preferences.dart';

import 'tvm_bindings.dart';


class WorkerController {
  WorkerController._();
  static final WorkerController instance = WorkerController._();

  late SharedPreferences _prefs;

  // Android-only: resolved at init() via the platform channel + asset staging,
  // because android has no filesystem path for bundled .so's or flutter assets
  // (unlike the Apple app bundle). Null on iOS/macOS.
  static const _paths = MethodChannel('com.yuvalino.tvm/paths');
  String? _androidLibDir; // nativeLibraryDir: libtvm.so + sidecars live here
  String? _androidAssets; // dir the initrd/initrc were staged into (= filesDir)

  final ValueNotifier<List<String>> logs =
      ValueNotifier(const ['[INFO] App started']);
  final ValueNotifier<bool> autoScroll = ValueNotifier(true);
  final ValueNotifier<double> fontSizeNotifier = ValueNotifier(14.0);

  bool _started = false;
  String _lineBuf = '';

  Future<void> init() async {
    _prefs = await SharedPreferences.getInstance();
    autoScroll.value = _prefs.getBool('autoScroll') ?? true;
    fontSizeNotifier.value = consoleFontSize;
    if (Platform.isAndroid) {
      await _prepareAndroid();
    }
    TvmBindings.instance.onOutput = _onOutput;
    TvmBindings.instance.init();
    autoScroll.addListener(() {
      _prefs.setBool('autoScroll', autoScroll.value);
    });
  }

  // Android: fetch the two dirs Dart can't derive, stage the flutter assets
  // (which are inside the APK, not on disk) into the writable files dir, and
  // set HOME so tvm_bindings.c's getenv("HOME") lands in a writable place —
  // matching how the OS sets HOME to the sandbox on iOS.
  Future<void> _prepareAndroid() async {
    _androidLibDir = await _paths.invokeMethod<String>('nativeLibraryDir');
    final filesDir = await _paths.invokeMethod<String>('filesDir');
    _androidAssets = filesDir;
    final arch = _archTag();
    for (final asset in ['init.flutter.rc', 'initrd.flutter.$arch.cpio']) {
      final data = await rootBundle.load('assets/$asset');
      final out = File('$filesDir/$asset');
      await out.writeAsBytes(data.buffer.asUint8List(
          data.offsetInBytes, data.lengthInBytes));
    }
    _NativeInfo.setenv('HOME', filesDir!);
  }

  int get sshPort => _prefs.getInt('sshPort') ?? 2222;
  set sshPort(int value) => _prefs.setInt('sshPort', value);

  String get sshPass => _prefs.getString('sshPass') ?? 'alpine';
  set sshPass(String value) => _prefs.setString('sshPass', value);

  double get consoleFontSize => _prefs.getDouble('consoleFontSize') ?? 14.0;
  set consoleFontSize(double value) {
    _prefs.setDouble('consoleFontSize', value);
    fontSizeNotifier.value = value;
  }

  void start() {
    if (_started) return;
    _started = true;

    _log('device: ${_NativeInfo.device()}');
    _log('pid: $pid');
    _log('uid: ${_NativeInfo.uid()}');
    _log('username: ${_NativeInfo.username()}');
    _log('hostname: ${Platform.localHostname}');
    _log('ssh port: $sshPort');
    _log('cwd: ${Directory.current.path}');
    _log('home: ${_NativeInfo.home()}');

    if (!TvmBindings.instance.isAvailable) {
      _log('[WARN] TVM native library not loaded');
      _log('[WARN] exe: ${Platform.resolvedExecutable}');
      if (TvmBindings.instance.loadError != null) {
        _log('[WARN] ${TvmBindings.instance.loadError}');
      }
      _started = false;
      return;
    }

    final libtvmPath = _libtvmPath();
    final assets = _flutterAssetsDir();
    final arch = _archTag();

    _log('[INFO] Starting tvm');
    final result = TvmBindings.instance.start(
      libtvmPath: libtvmPath,
      port: sshPort,
      password: sshPass,
      initrcPath: '$assets/init.flutter.rc',
      initrdPath: '$assets/initrd.flutter.$arch.cpio',
    );
    if (result != 0) {
      _log('[ERROR] Failed to start TVM');
      _started = false;
    }
  }

  // TVM output arrives here (on the main isolate). Buffer partial lines so a
  // chunk split mid-line doesn't produce a broken log entry.
  void _onOutput(String text) {
    _lineBuf += text;
    int nl;
    while ((nl = _lineBuf.indexOf('\n')) >= 0) {
      final line = _lineBuf.substring(0, nl);
      _lineBuf = _lineBuf.substring(nl + 1);
      if (line.isNotEmpty) _log(line);
    }
  }

  static String _appDir() {
    final exe = Platform.resolvedExecutable;
    return exe.substring(0, exe.lastIndexOf('/'));
  }

  String _libtvmPath() {
    if (Platform.isAndroid) return '$_androidLibDir/libtvm.so';
    final appDir = _appDir();
    if (Platform.isMacOS) return '$appDir/../Frameworks/libtvm.dylib';
    return '$appDir/Frameworks/libtvm.dylib'; // iOS
  }

  // On Apple the initrc/initrd are Resources in the app bundle; on android they
  // were staged into the files dir by _prepareAndroid(). Note the Apple paths
  // include a trailing "/assets"; android's do not (see start()).
  String _flutterAssetsDir() {
    if (Platform.isAndroid) return _androidAssets!;
    final appDir = _appDir();
    if (Platform.isMacOS) {
      return '$appDir/../Frameworks/App.framework/Resources/flutter_assets/assets';
    }
    return '$appDir/Frameworks/App.framework/flutter_assets/assets'; // iOS
  }

  // Matches build.py platform names / the per-arch initrd asset suffix.
  static String _archTag() {
    final abi = Abi.current();
    if (abi == Abi.iosArm64) return 'ios-arm64';
    if (abi == Abi.macosArm64) return 'macos-arm64';
    if (abi == Abi.androidArm64) return 'android-arm64';
    if (abi == Abi.linuxX64) return 'linux-x64';
    throw UnsupportedError('unsupported abi: $abi');
  }

  // Single log sink: the in-app console UI, plus real stderr (fd 2). On Apple
  // `devicectl --console` captures fd 2 on a fresh launch; on android the same
  // stderr shows in logcat, which the adb-flutter test backend tails.
  void _log(String line) {
    logs.value = [...logs.value, line];
    try {
      stderr.writeln(line);
    } catch (_) {}
  }
}

// Startup info gathered via libc. On Apple, mirrors the original devtool app
// (sysctl device string, getpwuid name, NSHomeDirectory). On android those
// symbols don't exist, so device/home degrade gracefully and only the portable
// bionic calls (getuid/getpwuid/setenv) are used.
class _NativeInfo {
  static final DynamicLibrary _libc = DynamicLibrary.process();

  static final int Function() _getuid =
      _libc.lookupFunction<Uint32 Function(), int Function()>('getuid');

  static final Pointer<Void> Function(int) _getpwuid = _libc.lookupFunction<
      Pointer<Void> Function(Uint32), Pointer<Void> Function(int)>('getpwuid');

  static final int Function(Pointer<Utf8>, Pointer<Utf8>, int) _setenv =
      _libc.lookupFunction<Int32 Function(Pointer<Utf8>, Pointer<Utf8>, Int32),
          int Function(Pointer<Utf8>, Pointer<Utf8>, int)>('setenv');

  static void setenv(String name, String value) {
    final n = name.toNativeUtf8();
    final v = value.toNativeUtf8();
    try {
      _setenv(n, v, 1);
    } catch (_) {
    } finally {
      malloc.free(n);
      malloc.free(v);
    }
  }

  static int uid() {
    try {
      return _getuid();
    } catch (_) {
      return -1;
    }
  }

  static String username() {
    try {
      final pw = _getpwuid(_getuid());
      if (pw == nullptr) return '';
      // pw_name is the first field of struct passwd (a char *).
      final namePtr = pw.cast<Pointer<Utf8>>().value;
      return namePtr == nullptr ? '' : namePtr.toDartString();
    } catch (_) {
      return '';
    }
  }

  static String device() {
    if (Platform.isAndroid) {
      return '${Platform.operatingSystem}_${Platform.operatingSystemVersion}';
    }
    return '${_sysctl('hw.machine')}_${_sysctl('kern.osproductversion')}'
        '_${_sysctl('kern.osversion')}';
  }

  // ── Apple-only helpers below (sysctl / Foundation). Not called on android. ──
  static final int Function(
    Pointer<Utf8>,
    Pointer<Void>,
    Pointer<IntPtr>,
    Pointer<Void>,
    int,
  ) _sysctlbyname = _libc.lookupFunction<
      Int32 Function(
          Pointer<Utf8>, Pointer<Void>, Pointer<IntPtr>, Pointer<Void>, IntPtr),
      int Function(Pointer<Utf8>, Pointer<Void>, Pointer<IntPtr>, Pointer<Void>,
          int)>('sysctlbyname');

  // NSHomeDirectory()/Documents — the writable home used by the TVM shell
  // (the sandbox root itself isn't writable on iOS). NSHomeDirectory is a
  // Foundation C function; its NSString is read via -[NSString UTF8String].
  static final Pointer<Void> Function() _nsHomeDirectory =
      _libc.lookupFunction<Pointer<Void> Function(), Pointer<Void> Function()>(
          'NSHomeDirectory');
  static final Pointer<Void> Function(Pointer<Utf8>) _selRegisterName =
      _libc.lookupFunction<Pointer<Void> Function(Pointer<Utf8>),
          Pointer<Void> Function(Pointer<Utf8>)>('sel_registerName');
  static final Pointer<Utf8> Function(Pointer<Void>, Pointer<Void>) _msgSendStr =
      _libc.lookupFunction<
          Pointer<Utf8> Function(Pointer<Void>, Pointer<Void>),
          Pointer<Utf8> Function(Pointer<Void>, Pointer<Void>)>('objc_msgSend');

  static String home() {
    if (Platform.isAndroid) return WorkerController.instance._androidAssets ?? '';
    try {
      final nsstr = _nsHomeDirectory();
      if (nsstr == nullptr) return '';
      final sel = 'UTF8String'.toNativeUtf8();
      try {
        final cstr = _msgSendStr(nsstr, _selRegisterName(sel));
        if (cstr == nullptr) return '';
        return '${cstr.toDartString()}/Documents';
      } finally {
        calloc.free(sel);
      }
    } catch (_) {
      return '';
    }
  }

  static String _sysctl(String name) {
    final namePtr = name.toNativeUtf8();
    final lenPtr = calloc<IntPtr>();
    try {
      if (_sysctlbyname(namePtr, nullptr, lenPtr, nullptr, 0) != 0) return '';
      final len = lenPtr.value;
      if (len <= 0) return '';
      final buf = calloc<Uint8>(len);
      try {
        if (_sysctlbyname(namePtr, buf.cast(), lenPtr, nullptr, 0) != 0) {
          return '';
        }
        return buf.cast<Utf8>().toDartString();
      } finally {
        calloc.free(buf);
      }
    } catch (_) {
      return '';
    } finally {
      calloc.free(namePtr);
      calloc.free(lenPtr);
    }
  }
}
