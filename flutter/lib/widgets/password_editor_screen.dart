import 'package:flutter/cupertino.dart';

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
    return CupertinoPageScaffold(
      backgroundColor: CupertinoColors.black,
      navigationBar: CupertinoNavigationBar(
        middle: Text(widget.title),
        previousPageTitle: 'Settings',
      ),
      child: SafeArea(
        child: ListView(
          children: [
            CupertinoFormSection.insetGrouped(
              footer: widget.footer != null ? Text(widget.footer!) : null,
              children: [
                CupertinoTextField.borderless(
                  controller: _controller,
                  placeholder: widget.title,
                  obscureText: !_showPassword,
                  autocorrect: false,
                  enableSuggestions: false,
                  onChanged: widget.onChanged,
                  autofocus: true,
                  padding: const EdgeInsets.symmetric(
                      horizontal: 16, vertical: 12),
                  suffix: Padding(
                    padding: const EdgeInsets.only(right: 12),
                    child: GestureDetector(
                      onTap: () =>
                          setState(() => _showPassword = !_showPassword),
                      child: Icon(
                        _showPassword
                            ? CupertinoIcons.eye_slash_fill
                            : CupertinoIcons.eye_fill,
                        size: 22,
                        color: CupertinoColors.secondaryLabel.resolveFrom(
                            context),
                      ),
                    ),
                  ),
                ),
              ],
            ),
          ],
        ),
      ),
    );
  }
}
