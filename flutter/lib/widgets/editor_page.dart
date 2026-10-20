import 'package:flutter/cupertino.dart';

import '../theme/tvm_colors.dart';

/// A settings value's own page, styled like the settings it comes from: the
/// site's background, one card holding [field], a quiet footer under it.
/// (Cupertino's grouped form section draws a black band and grey card of
/// its own, out of place on these colours.)
class EditorPage extends StatelessWidget {
  const EditorPage({
    super.key,
    required this.title,
    required this.field,
    this.footer,
  });

  final String title;
  final Widget field;
  final String? footer;

  /// The text style for the value being edited.
  static const valueStyle = TextStyle(
    fontFamily: TvmColors.mono,
    fontSize: 16,
    color: TvmColors.bright,
  );

  static const placeholderStyle = TextStyle(
    fontFamily: TvmColors.mono,
    fontSize: 16,
    color: TvmColors.dimmer,
  );

  @override
  Widget build(BuildContext context) {
    return CupertinoPageScaffold(
      backgroundColor: TvmColors.bg,
      navigationBar: CupertinoNavigationBar(
        backgroundColor: TvmColors.bg,
        border: const Border(bottom: BorderSide(color: TvmColors.line)),
        previousPageTitle: 'TVM',
        middle: Text(
          title,
          style: const TextStyle(
            fontFamily: TvmColors.mono,
            fontSize: 16,
            fontWeight: FontWeight.w700,
            color: TvmColors.bright,
          ),
        ),
      ),
      child: SafeArea(
        child: ListView(
          padding: const EdgeInsets.only(top: 20),
          children: [
            Container(
              margin: const EdgeInsets.symmetric(horizontal: 16),
              decoration: BoxDecoration(
                color: TvmColors.card,
                border: Border.all(color: TvmColors.line),
                borderRadius: BorderRadius.circular(14),
              ),
              child: field,
            ),
            if (footer != null)
              Padding(
                padding: const EdgeInsets.fromLTRB(30, 8, 30, 0),
                child: Text(
                  footer!,
                  style: const TextStyle(
                    fontFamily: TvmColors.mono,
                    fontSize: 12,
                    color: TvmColors.dimmer,
                  ),
                ),
              ),
          ],
        ),
      ),
    );
  }
}
