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
        appBarTheme: const AppBarTheme(
          backgroundColor: Color(0xFF1C1C1E),
          elevation: 0,
          scrolledUnderElevation: 0,
          centerTitle: true,
          iconTheme: IconThemeData(color: Colors.white70),
          actionsIconTheme: IconThemeData(color: Colors.white70),
          titleTextStyle: TextStyle(
            color: Colors.white,
            fontSize: 18,
            fontWeight: FontWeight.w600,
          ),
        ),
        pageTransitionsTheme: const PageTransitionsTheme(
          builders: {
            TargetPlatform.android: CupertinoPageTransitionsBuilder(),
            TargetPlatform.iOS: CupertinoPageTransitionsBuilder(),
          },
        ),
        useMaterial3: true,
      ),
      initialRoute: '/',
      onGenerateRoute: (settings) {
        switch (settings.name) {
          case '/settings':
            return MaterialPageRoute(
              settings: settings,
              builder: (_) => const SettingsScreen(),
            );
          case '/':
          default:
            return MaterialPageRoute(
              settings: settings,
              builder: (_) => const ConsoleScreen(),
            );
        }
      },
    );
  }
}
