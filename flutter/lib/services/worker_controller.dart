import 'dart:async';
import 'dart:ffi';
import 'dart:io';

import 'package:ffi/ffi.dart';
import 'package:flutter/foundation.dart';
import 'package:shared_preferences/shared_preferences.dart';

import 'mdns.dart';
import 'tvm_bindings.dart';


class WorkerController {
  WorkerController._();
  static final WorkerController instance = WorkerController._();

  late SharedPreferences _prefs;

  final ValueNotifier<List<String>> logs =
      ValueNotifier(const ['[INFO] App started']);
  final ValueNotifier<bool> autoScroll = ValueNotifier(true);
  final ValueNotifier<double> terminalFontSizeNotifier = ValueNotifier(13.0);

  /// The login name the shell tabs authenticate as (getpwuid(getuid())).
  final String username = _NativeInfo.username();

  bool _started = false;
  String _lineBuf = '';

  Future<void> init() async {
    _prefs = await SharedPreferences.getInstance();
    autoScroll.value = _prefs.getBool('autoScroll') ?? true;
    terminalFontSizeNotifier.value = terminalFontSize;
    TvmBindings.instance.onOutput = _onOutput;
    TvmBindings.instance.init();
    autoScroll.addListener(() {
      _prefs.setBool('autoScroll', autoScroll.value);
    });
  }

  int get sshPort => _prefs.getInt('sshPort') ?? 2222;
  set sshPort(int value) => _prefs.setInt('sshPort', value);

  String get sshPass => _prefs.getString('sshPass') ?? 'alpine';
  set sshPass(String value) => _prefs.setString('sshPass', value);

  /// The port and password the server was started with. The settings above
  /// apply on next launch; until then the shell tabs and the shown ssh
  /// command go by these.
  int get serverPort => _serverPort ?? sshPort;
  String get serverPass => _serverPass ?? sshPass;
  int? _serverPort;
  String? _serverPass;

  double get terminalFontSize => _prefs.getDouble('terminalFontSize') ?? 13.0;
  set terminalFontSize(double value) {
    _prefs.setDouble('terminalFontSize', value);
    terminalFontSizeNotifier.value = value;
  }

  /// How other machines on the Wi-Fi reach this one: its `.local` name and
  /// its address, each null until known (or when there is none). Looked up
  /// once, at start.
  final ValueNotifier<({String? name, String? address})> network =
      ValueNotifier((name: null, address: null));

  Future<void> _lookUpNetwork() async {
    final address = await _lanAddress();
    network.value = (name: null, address: address);
    // gethostname gives the mDNS name on macOS; iOS answers "localhost", so
    // there the phone's own mDNS responder is asked instead.
    final h = Platform.localHostname;
    final name = h.isNotEmpty && h != 'localhost'
        ? (h.contains('.') ? h : '$h.local')
        : address == null
            ? null
            : await mdnsReverseLookup(address);
    network.value = (name: name, address: address);
    if (name != null) _log('mdns name: $name');
  }

  /// The Wi-Fi (en0) IPv4 address, or null when there is none.
  Future<String?> _lanAddress() async {
    try {
      final ifaces = await NetworkInterface.list(type: InternetAddressType.IPv4);
      for (final i in ifaces) {
        if (i.name == 'en0' && i.addresses.isNotEmpty) {
          return i.addresses.first.address;
        }
      }
    } catch (_) {}
    return null;
  }

  void start() {
    if (_started) return;
    _started = true;
    _serverPort = sshPort;
    _serverPass = sshPass;

    _log('device: ${_NativeInfo.device()}');
    _log('pid: $pid');
    _log('uid: ${_NativeInfo.uid()}');
    _log('username: $username');
    _log('hostname: ${Platform.localHostname}');
    _log('ssh port: $serverPort');
    _log('cwd: ${Directory.current.path}');
    _log('home: ${_NativeInfo.home()}');
    _lookUpNetwork();

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
      port: serverPort,
      password: serverPass,
      initrcPath: '$assets/assets/init.flutter.rc',
      initrdPath: '$assets/assets/initrd.flutter.$arch.cpio',
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

  static String _libtvmPath() {
    final appDir = _appDir();
    if (Platform.isMacOS) return '$appDir/../Frameworks/libtvm.framework/libtvm';
    return '$appDir/Frameworks/libtvm.framework/libtvm'; // iOS
  }

  static String _flutterAssetsDir() {
    final appDir = _appDir();
    if (Platform.isMacOS) {
      return '$appDir/../Frameworks/App.framework/Resources/flutter_assets';
    }
    return '$appDir/Frameworks/App.framework/flutter_assets'; // iOS
  }

  // Matches build.py platform names / the per-arch initrd asset suffix.
  static String _archTag() {
    final abi = Abi.current();
    if (abi == Abi.iosArm64) return 'ios-arm64';
    if (abi == Abi.macosArm64) return 'macos-arm64';
    if (abi == Abi.linuxX64) return 'linux-x64';
    throw UnsupportedError('unsupported abi: $abi');
  }

  // Single log sink: the in-app console UI, plus real stderr (fd 2) so
  // `devicectl --console` surfaces everything on a fresh (--terminate-existing)
  // launch. Flutter routes print/debugPrint to os_log rather than stdio, but
  // dart:io's stderr writes to the real fd that --console captures.
  void _log(String line) {
    logs.value = [...logs.value, line];
    try {
      stderr.writeln(line);
    } catch (_) {}
  }
}

// Startup info gathered via libc, matching the original devtool app:
//   device = hw.machine _ kern.osproductversion _ kern.osversion
//   uid    = getuid()
//   name   = getpwuid(getuid())->pw_name
class _NativeInfo {
  static final DynamicLibrary _libc = DynamicLibrary.process();

  static final int Function() _getuid =
      _libc.lookupFunction<Uint32 Function(), int Function()>('getuid');

  static final Pointer<Void> Function(int) _getpwuid = _libc.lookupFunction<
      Pointer<Void> Function(Uint32), Pointer<Void> Function(int)>('getpwuid');

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

  static int uid() {
    try {
      return _getuid();
    } catch (_) {
      return -1;
    }
  }

  static final Pointer<Utf8> Function(Pointer<Utf8>) _getenv =
      _libc.lookupFunction<Pointer<Utf8> Function(Pointer<Utf8>),
          Pointer<Utf8> Function(Pointer<Utf8>)>('getenv');

  /// TVM_USER when set (the guest's user then, see tvm.c), else the host's
  /// name for the app's uid.
  static String username() {
    try {
      final key = 'TVM_USER'.toNativeUtf8();
      final override = _getenv(key);
      malloc.free(key);
      if (override != nullptr && override.toDartString().isNotEmpty) {
        return override.toDartString();
      }
      final pw = _getpwuid(_getuid());
      if (pw == nullptr) return '';
      // pw_name is the first field of struct passwd (a char *).
      final namePtr = pw.cast<Pointer<Utf8>>().value;
      return namePtr == nullptr ? '' : namePtr.toDartString();
    } catch (_) {
      return '';
    }
  }

  static String device() =>
      '${_sysctl('hw.machine')}_${_sysctl('kern.osproductversion')}'
      '_${_sysctl('kern.osversion')}';

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
