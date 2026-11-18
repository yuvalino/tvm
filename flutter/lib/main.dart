import 'dart:ffi';
import 'dart:io';

import 'package:ffi/ffi.dart';
import 'package:flutter/cupertino.dart';
import 'package:flutter/material.dart';
import 'package:frt/frt.dart';

import 'screens/home_screen.dart';
import 'services/worker_controller.dart';
import 'theme/tvm_colors.dart';

/// The process's environment variable [name]. On iOS the Dart VM leaves
/// Platform.environment empty (runtime/bin/platform_macos.cc), so the
/// variable is read from libc there.
String? _getenv(String name) {
  if (!Platform.isIOS) return Platform.environment[name];
  final getenv = DynamicLibrary.process().lookupFunction<
      Pointer<Utf8> Function(Pointer<Utf8>),
      Pointer<Utf8> Function(Pointer<Utf8>)>('getenv');
  final cname = name.toNativeUtf8();
  try {
    final value = getenv(cname);
    return value == nullptr ? null : value.toDartString();
  } finally {
    malloc.free(cname);
  }
}

Future<void> main() async {
  WidgetsFlutterBinding.ensureInitialized();
  // Tests launch the app with FRT_PORT (and FRT_HOST to be reached from
  // another machine) to drive it over HTTP; launched normally, no socket is
  // opened. See tests/test_flutter.py. They're passed to initFrt, which
  // reads Platform.environment and would not see them on iOS.
  final frtPort = _getenv('FRT_PORT');
  if (frtPort != null) {
    try {
      final frtHost = _getenv('FRT_HOST');
      await initFrt(
        port: int.parse(frtPort),
        address: frtHost == null || frtHost.isEmpty
            ? null
            : InternetAddress(frtHost),
      );
    } catch (e) {
      stderr.writeln('[ERROR] frt: $e');
    }
  }
  await WorkerController.instance.init();
  runApp(const DevtoolApp());
}

class DevtoolApp extends StatelessWidget {
  const DevtoolApp({super.key});

  @override
  Widget build(BuildContext context) {
    return MaterialApp(
      title: 'tvm',
      debugShowCheckedModeBanner: false,
      theme: ThemeData(
        colorScheme: const ColorScheme.dark(
          surface: TvmColors.bg,
          primary: TvmColors.green,
        ),
        scaffoldBackgroundColor: TvmColors.bg,
        useMaterial3: true,
      ),
      // iOS chrome (nav bars, back buttons, form rows, dialogs) inherits
      // this: the site's colours, with its green as the tint. The
      // DefaultTextStyle keeps raw Text outside a Material from falling back
      // to WidgetsApp's error style (yellow underlines).
      builder: (context, child) => CupertinoTheme(
        data: const CupertinoThemeData(
          brightness: Brightness.dark,
          primaryColor: TvmColors.green,
          scaffoldBackgroundColor: TvmColors.bg,
          barBackgroundColor: TvmColors.bar,
        ),
        child: Builder(
          builder: (context) => DefaultTextStyle(
            style: CupertinoTheme.of(context)
                .textTheme
                .textStyle
                .copyWith(color: TvmColors.fg),
            child: child!,
          ),
        ),
      ),
      home: const HomeScreen(),
    );
  }
}
