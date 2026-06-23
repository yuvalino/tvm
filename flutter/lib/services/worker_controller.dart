import 'dart:async';
import 'dart:io';

import 'package:flutter/foundation.dart';
import 'package:shared_preferences/shared_preferences.dart';

import 'tvm_bindings.dart';


class WorkerController {
  WorkerController._();
  static final WorkerController instance = WorkerController._();

  late SharedPreferences _prefs;
  Timer? _pollTimer;

  final ValueNotifier<List<String>> logs =
      ValueNotifier(const ['[INFO] App started']);
  final ValueNotifier<bool> autoScroll = ValueNotifier(true);
  final ValueNotifier<double> fontSizeNotifier = ValueNotifier(14.0);

  bool _started = false;

  Future<void> init() async {
    _prefs = await SharedPreferences.getInstance();
    autoScroll.value = _prefs.getBool('autoScroll') ?? true;
    fontSizeNotifier.value = consoleFontSize;
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

    _addLog(
        'os: ${Platform.operatingSystem} ${Platform.operatingSystemVersion}');
    _addLog('pid: $pid');
    _addLog('hostname: ${Platform.localHostname}');
    _addLog('ssh port: $sshPort');
    _addLog('cwd: ${Directory.current.path}');
    _addLog(
        'home: ${Platform.environment['HOME'] ?? Platform.environment['USERPROFILE'] ?? ''}');

    if (!TvmBindings.instance.isAvailable) {
      _addLog('[WARN] TVM native library not loaded');
      if (TvmBindings.instance.loadError != null) {
        _addLog('[WARN] ${TvmBindings.instance.loadError}');
      }
      return;
    }

    _addLog('[INFO] Starting tvm');
    final result = TvmBindings.instance.start(sshPort, sshPass);
    if (result != 0) {
      _addLog('[ERROR] Failed to start TVM');
      _started = false;
      return;
    }

    _pollTimer = Timer.periodic(const Duration(milliseconds: 100), (_) {
      final output = TvmBindings.instance.poll();
      if (output != null && output.isNotEmpty) {
        for (final line in output.split('\n')) {
          if (line.isNotEmpty) _addLog(line);
        }
      }
    });
  }

  void stop() {
    if (!_started) return;
    _started = false;
    _pollTimer?.cancel();
    _pollTimer = null;
    if (TvmBindings.instance.isAvailable) {
      TvmBindings.instance.stop();
    }
  }

  void _addLog(String line) {
    logs.value = [...logs.value, line];
    debugPrint('[TVM] $line');
  }
}
