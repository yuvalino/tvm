import 'package:flutter/cupertino.dart';
import 'package:flutter/services.dart';

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
                CupertinoTextFormFieldRow(
                  controller: _controller,
                  placeholder: widget.title,
                  keyboardType: TextInputType.number,
                  inputFormatters: [FilteringTextInputFormatter.digitsOnly],
                  onChanged: _onChanged,
                  autofocus: true,
                ),
              ],
            ),
          ],
        ),
      ),
    );
  }
}
