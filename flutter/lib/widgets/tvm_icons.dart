import 'package:flutter/widgets.dart';
import 'package:flutter_svg/flutter_svg.dart';

import '../theme/tvm_colors.dart';

String _hex(Color c) =>
    '#${(c.toARGB32() & 0xFFFFFF).toRadixString(16).padLeft(6, '0')}';

/// The site's favicon (tvm.yuvalino.com/favicon-32.png): the yellow pixel-art
/// hard hat, stored at 4x with each pixel kept square. Marks the TVM tab.
class TvmLogo extends StatelessWidget {
  const TvmLogo({super.key, this.size = 24});

  final double size;

  @override
  Widget build(BuildContext context) => Image.asset(
    'assets/favicon.png',
    width: size,
    height: size,
    filterQuality: FilterQuality.medium,
  );
}

/// A stroked 24x24 icon (paths in SVG syntax) drawn in the ambient icon
/// colour, so it follows the tab or button it sits in.
class StrokeIcon extends StatelessWidget {
  const StrokeIcon(this.paths,
      {super.key, this.size = 20, this.color, this.strokeWidth = 2});

  final List<String> paths;
  final double size;
  final Color? color;
  final double strokeWidth;

  // Shell tabs: a ">_" prompt in a rounded box.
  static const shell = [
    'M5 3.5h14a2.5 2.5 0 0 1 2.5 2.5v12a2.5 2.5 0 0 1-2.5 2.5H5a2.5 2.5 0 0 1-2.5-2.5V6A2.5 2.5 0 0 1 5 3.5z',
    'M7 9.5l3 2.5-3 2.5M12.5 15h4.5',
  ];
  static const plus = ['M12 5v14M5 12h14'];
  static const keyboardUp = [
    'M4.5 5h15a2 2 0 0 1 2 2v7a2 2 0 0 1-2 2h-15a2 2 0 0 1-2-2V7a2 2 0 0 1 2-2z',
    'M6 8.5h1M9.5 8.5h1M13 8.5h1M16.5 8.5h1M8 12.5h8',
    'M9 21l3-2.5 3 2.5',
  ];
  static const keyboardDown = [
    'M4.5 3h15a2 2 0 0 1 2 2v7a2 2 0 0 1-2 2h-15a2 2 0 0 1-2-2V5a2 2 0 0 1 2-2z',
    'M6 6.5h1M9.5 6.5h1M13 6.5h1M16.5 6.5h1M8 10.5h8',
    'M9 18.5l3 2.5 3-2.5',
  ];
  static const left = ['M15 6l-6 6 6 6'];
  static const up = ['M6 15l6-6 6 6'];
  static const down = ['M6 9l6 6 6-6'];
  static const right = ['M9 6l6 6-6 6'];
  static const bottom = ['M12 5v14M6 13l6 6 6-6'];
  static const fontSize = ['M4 7V4h16v3M9 20h6M12 4v16'];
  static const globe = [
    'M12 3a9 9 0 1 0 0 18a9 9 0 1 0 0-18z',
    'M3 12h18M12 3a14 14 0 0 1 0 18M12 3a14 14 0 0 0 0 18',
  ];
  static const lock = ['M5 11h14v10H5z', 'M8 11V7a4 4 0 0 1 8 0v4'];
  static const unlock = ['M5 11h14v10H5z', 'M8 11V7a4 4 0 0 1 7.9-.9'];
  static const rename = ['M12 20h9M16.5 3.5a2.1 2.1 0 0 1 3 3L7 19l-4 1 1-4z'];
  static const reconnect = ['M21 12a9 9 0 1 1-3-6.7L21 8M21 3v5h-5'];
  static const closeOthers = ['M4 4h16v16H4z', 'M9 9l6 6M15 9l-6 6'];
  static const close = ['M6 6l12 12M18 6L6 18'];

  @override
  Widget build(BuildContext context) {
    final c = color ?? IconTheme.of(context).color ?? TvmColors.fg;
    final body = paths.map((d) => '<path d="$d"/>').join();
    return SvgPicture.string(
      '<svg viewBox="0 0 24 24" xmlns="http://www.w3.org/2000/svg" fill="none" '
      'stroke="${_hex(c)}" stroke-width="$strokeWidth" stroke-linecap="round" '
      'stroke-linejoin="round">$body</svg>',
      width: size,
      height: size,
    );
  }
}
