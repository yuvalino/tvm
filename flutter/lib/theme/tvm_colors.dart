import 'package:flutter/widgets.dart';
import 'package:xterm/xterm.dart';

/// The palette of tvm.yuvalino.com (its style.css), plus the few surface
/// shades the app needs on top of the page background.
class TvmColors {
  static const bg = Color(0xFF1F1F1E);
  static const bar = Color(0xFF262624);
  static const card = Color(0xFF2B2A28);
  static const line = Color(0xFF3A3834);
  static const highlight = Color(0xFF33322F);

  static const fg = Color(0xFFD6D0C4);
  static const bright = Color(0xFFF2ECE0);
  static const dim = Color(0xFF8A8376);
  static const dimmer = Color(0xFF625C52);

  static const green = Color(0xFFB2C364);
  static const yellow = Color(0xFFE0B352);
  static const red = Color(0xFFEA6962);
  static const magenta = Color(0xFFD3869B);
  static const blue = Color(0xFF83B6AC);
  static const cyan = Color(0xFF8FC8B4);
  static const steel = Color(0xFFC3BCAE);

  /// The colour blocks neofetch prints, as the site shows them.
  static const blocks = [
    Color(0xFF4A443C), red, green, yellow, blue, magenta, cyan,
    Color(0xFFB5AC9C),
  ];

  static const mono = 'JetBrains Mono';
}

/// xterm's default theme with only the background swapped for the site's:
/// the shell's own colours are left as they are.
const tvmTerminalTheme = TerminalTheme(
  cursor: Color(0XAAAEAFAD),
  selection: Color(0XAAAEAFAD),
  foreground: Color(0XFFCCCCCC),
  background: TvmColors.bg,
  black: Color(0XFF000000),
  red: Color(0XFFCD3131),
  green: Color(0XFF0DBC79),
  yellow: Color(0XFFE5E510),
  blue: Color(0XFF2472C8),
  magenta: Color(0XFFBC3FBC),
  cyan: Color(0XFF11A8CD),
  white: Color(0XFFE5E5E5),
  brightBlack: Color(0XFF666666),
  brightRed: Color(0XFFF14C4C),
  brightGreen: Color(0XFF23D18B),
  brightYellow: Color(0XFFF5F543),
  brightBlue: Color(0XFF3B8EEA),
  brightMagenta: Color(0XFFD670D6),
  brightCyan: Color(0XFF29B8DB),
  brightWhite: Color(0XFFFFFFFF),
  searchHitBackground: Color(0XFFFFFF2B),
  searchHitBackgroundCurrent: Color(0XFF31FF26),
  searchHitForeground: Color(0XFF000000),
);
