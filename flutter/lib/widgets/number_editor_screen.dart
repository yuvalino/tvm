import 'package:flutter/cupertino.dart';
import 'package:flutter/services.dart';

import '../theme/tvm_colors.dart';
import 'editor_page.dart';

class NumberEditorScreen extends StatefulWidget {
  const NumberEditorScreen({
    super.key,
    required this.title,
    required this.initialValue,
    required this.min,
    required this.max,
    this.footer,
    required this.onChanged,
  });

  final String title;
  final int initialValue;
  final int min;
  final int max;
  final String? footer;
  final ValueChanged<int> onChanged;

  @override
  State<NumberEditorScreen> createState() => _NumberEditorScreenState();
}

class _NumberEditorScreenState extends State<NumberEditorScreen> {
  late final TextEditingController _controller;

  @override
  void initState() {
    super.initState();
    _controller = TextEditingController(text: '${widget.initialValue}');
  }

  @override
  void dispose() {
    _controller.dispose();
    super.dispose();
  }

  void _onChanged(String text) {
    final parsed = int.tryParse(text);
    if (parsed != null) {
      widget.onChanged(parsed.clamp(widget.min, widget.max));
    }
  }

  @override
  Widget build(BuildContext context) {
    return EditorPage(
      title: widget.title,
      footer: widget.footer,
      field: CupertinoTextField.borderless(
        controller: _controller,
        placeholder: widget.title,
        keyboardType: TextInputType.number,
        inputFormatters: [FilteringTextInputFormatter.digitsOnly],
        onChanged: _onChanged,
        autofocus: true,
        style: EditorPage.valueStyle,
        placeholderStyle: EditorPage.placeholderStyle,
        cursorColor: TvmColors.green,
        keyboardAppearance: Brightness.dark,
        padding: const EdgeInsets.symmetric(horizontal: 16, vertical: 14),
      ),
    );
  }
}
