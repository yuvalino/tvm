import 'package:flutter/cupertino.dart';
import 'package:flutter/material.dart';

import 'screens/console_screen.dart';
import 'screens/settings_screen.dart';
import 'services/worker_controller.dart';

Future<void> main() async {
  WidgetsFlutterBinding.ensureInitialized();
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
          surface: Color(0xFF1E1E1E),
          primary: Color(0xFF34C759),
        ),
        scaffoldBackgroundColor: Colors.black,
        fontFamily: 'SF Pro',
        useMaterial3: true,
      ),
      // iOS chrome (nav bars, back buttons, form rows) inherits this — dark,
      // with green as the tint for back buttons and controls. The
      // DefaultTextStyle keeps raw Text under CupertinoPageScaffold from
      // falling back to WidgetsApp's error style (yellow underlines).
      builder: (context, child) => CupertinoTheme(
        data: const CupertinoThemeData(
          brightness: Brightness.dark,
          primaryColor: Color(0xFF34C759),
          scaffoldBackgroundColor: Colors.black,
        ),
        child: Builder(
          builder: (context) => DefaultTextStyle(
            style: CupertinoTheme.of(context).textTheme.textStyle,
            child: child!,
          ),
        ),
      ),
      initialRoute: '/',
      onGenerateRoute: (settings) {
        switch (settings.name) {
          case '/settings':
            return CupertinoPageRoute(
              settings: settings,
              builder: (_) => const SettingsScreen(),
            );
          case '/':
          default:
            return CupertinoPageRoute(
              settings: settings,
              builder: (_) => const ConsoleScreen(),
            );
        }
      },
    );
  }
}
