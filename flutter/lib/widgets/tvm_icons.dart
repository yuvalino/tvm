import 'package:flutter/widgets.dart';
import 'package:flutter_svg/flutter_svg.dart';

import '../theme/tvm_colors.dart';

String _hex(Color c) =>
    '#${(c.toARGB32() & 0xFFFFFF).toRadixString(16).padLeft(6, '0')}';

/// The site's neofetch apple: a steel shackle hanging open off the left
/// (its left leg free beside the apple, its right one going into the top)
/// and the body striped green, yellow, red, magenta, blue. Marks the TVM tab.
class AppleIcon extends StatelessWidget {
  const AppleIcon({super.key, this.size = 24});

  final double size;

  static final String _svg = () {
    String stop(double at, Color c) =>
        '<stop offset="$at" stop-color="${_hex(c)}"/>';
    final stripes = [
      (0.0, 0.22, TvmColors.green),
      (0.22, 0.40, TvmColors.yellow),
      (0.40, 0.62, TvmColors.red),
      (0.62, 0.80, TvmColors.magenta),
      (0.80, 1.0, TvmColors.blue),
    ].map((s) => stop(s.$1, s.$3) + stop(s.$2, s.$3)).join();
    // The shackle is drawn first so the body covers the end of its right leg.
    // It rises 5/13 of the body's height above it, as on the site (5 rows of
    // shackle to 13 of apple), and the whole is shifted up to sit centred.
    return '''
<svg viewBox="0 0 24 24" xmlns="http://www.w3.org/2000/svg">
<defs><linearGradient id="a" x1="0" y1="0" x2="0" y2="1">$stripes</linearGradient></defs>
<g transform="translate(0 -0.9)">
<path d="M2.4 7.8V6.6a2.9 2.9 0 0 1 5.8 0V9.6" fill="none" stroke="${_hex(TvmColors.steel)}" stroke-width="2" stroke-linecap="round"/>
<path fill="url(#a)" d="M17.2 14.1c0-2.5 2-3.7 2.1-3.8-1.2-1.7-3-1.9-3.6-1.9-1.5-.2-3 .9-3.8.9-.8 0-2-.9-3.3-.8-1.7 0-3.2 1-4.1 2.5-1.8 3.1-.5 7.6 1.3 10.1.8 1.2 1.8 2.6 3.1 2.5 1.2 0 1.7-.8 3.2-.8s1.9.8 3.2.8c1.3 0 2.2-1.2 3-2.5.9-1.4 1.3-2.8 1.3-2.9 0 0-2.4-1-2.4-4.1z"/>
</g>
</svg>''';
  }();

  @override
  Widget build(BuildContext context) =>
      SvgPicture.string(_svg, width: size, height: size);
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
  static const copy = ['M9 9h11v11H9z', 'M5 15V5a2 2 0 0 1 2-2h8'];
  static const follow = ['M12 5v14M6 13l6 6 6-6'];
  static const fontSize = ['M4 7V4h16v3M9 20h6M12 4v16'];
  static const globe = [
    'M12 3a9 9 0 1 0 0 18a9 9 0 1 0 0-18z',
    'M3 12h18M12 3a14 14 0 0 1 0 18M12 3a14 14 0 0 0 0 18',
  ];
  static const lock = ['M5 11h14v10H5z', 'M8 11V7a4 4 0 0 1 8 0v4'];
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
