import 'package:flutter/cupertino.dart';
import 'package:flutter/material.dart';

import 'screens/home_screen.dart';
import 'services/worker_controller.dart';
import 'theme/tvm_colors.dart';

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
