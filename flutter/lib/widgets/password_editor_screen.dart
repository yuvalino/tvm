import 'package:flutter/cupertino.dart';

import '../theme/tvm_colors.dart';
import 'editor_page.dart';

class PasswordEditorScreen extends StatefulWidget {
  const PasswordEditorScreen({
    super.key,
    required this.title,
    required this.initialValue,
    required this.onChanged,
    this.footer,
  });

  final String title;
  final String initialValue;
  final ValueChanged<String> onChanged;
  final String? footer;

  @override
  State<PasswordEditorScreen> createState() => _PasswordEditorScreenState();
}

class _PasswordEditorScreenState extends State<PasswordEditorScreen> {
  late final TextEditingController _controller;
  bool _showPassword = false;

  @override
  void initState() {
    super.initState();
    _controller = TextEditingController(text: widget.initialValue);
  }

  @override
  void dispose() {
    _controller.dispose();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    return EditorPage(
      title: widget.title,
      footer: widget.footer,
      field: CupertinoTextField.borderless(
        key: const ValueKey('editor-field'),
        controller: _controller,
        placeholder: widget.title,
        obscureText: !_showPassword,
        autocorrect: false,
        enableSuggestions: false,
        onChanged: widget.onChanged,
        autofocus: true,
        style: EditorPage.valueStyle,
        placeholderStyle: EditorPage.placeholderStyle,
        cursorColor: TvmColors.green,
        keyboardAppearance: Brightness.dark,
        padding: const EdgeInsets.symmetric(horizontal: 16, vertical: 14),
        suffix: GestureDetector(
          key: const ValueKey('password-reveal'),
          behavior: HitTestBehavior.opaque,
          onTap: () => setState(() => _showPassword = !_showPassword),
          child: Padding(
            padding: const EdgeInsets.symmetric(horizontal: 14, vertical: 10),
            child: Icon(
              _showPassword
                  ? CupertinoIcons.eye_slash_fill
                  : CupertinoIcons.eye_fill,
              size: 20,
              color: TvmColors.dim,
            ),
          ),
        ),
      ),
    );
  }
}
