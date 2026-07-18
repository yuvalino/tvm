import 'dart:async';
import 'dart:io';

import 'package:flutter/foundation.dart';
import 'package:shared_preferences/shared_preferences.dart';

import 'tvm_bindings.dart';


class WorkerController {
  WorkerController._();
  static final WorkerController instance = WorkerController._();

  late SharedPreferences _prefs;

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

  double get consoleFontSize => _prefs.getDouble('consoleFontSize') ?? 14.0;
  set consoleFontSize(double value) {
    _prefs.setDouble('consoleFontSize', value);
    fontSizeNotifier.value = value;
  }

  void start() {
    if (_started) return;
    _started = true;

    _addLog('os: ${Platform.operatingSystem} ${Platform.operatingSystemVersion}');
    _addLog('pid: $pid');
    _addLog('hostname: ${Platform.localHostname}');
    _addLog('ssh port: $sshPort');

    if (!TvmBindings.instance.isAvailable) {
      _addLog('[WARN] TVM native library not loaded');
      _addLog('[WARN] exe: ${Platform.resolvedExecutable}');
      if (TvmBindings.instance.loadError != null) {
        _addLog('[WARN] ${TvmBindings.instance.loadError}');
      }
      _started = false;
      return;
    }

    final libtvmPath = _libtvmPath();
    final assets = _flutterAssetsDir();

    _addLog('[INFO] Starting tvm');
    final result = TvmBindings.instance.start(
      libtvmPath: libtvmPath,
      port: sshPort,
      password: sshPass,
      initrcPath: '$assets/assets/init.flutter.rc',
      initrdPath: '$assets/assets/initrd.flutter.cpio',
    );
    if (result != 0) {
      _addLog('[ERROR] Failed to start TVM');
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
      if (line.isNotEmpty) _addLog(line);
    }
  }

  static String _appDir() {
    final exe = Platform.resolvedExecutable;
    return exe.substring(0, exe.lastIndexOf('/'));
  }

  static String _libtvmPath() {
    final appDir = _appDir();
    if (Platform.isMacOS) return '$appDir/../Frameworks/libtvm.dylib';
    return '$appDir/Frameworks/libtvm.dylib'; // iOS
  }

  static String _flutterAssetsDir() {
    final appDir = _appDir();
    if (Platform.isMacOS) {
      return '$appDir/../Frameworks/App.framework/Resources/flutter_assets';
    }
    return '$appDir/Frameworks/App.framework/flutter_assets'; // iOS
  }

  void _addLog(String line) {
    logs.value = [...logs.value, line];
    debugPrint('[TVM] $line');
  }
}
