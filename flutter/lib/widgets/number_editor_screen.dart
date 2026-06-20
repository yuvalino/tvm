import 'package:flutter/material.dart';
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
      final clamped = parsed.clamp(widget.min, widget.max);
      widget.onChanged(clamped);
    }
  }

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      backgroundColor: const Color(0xFF1C1C1E),
      appBar: AppBar(title: Text(widget.title)),
      body: Padding(
        padding: const EdgeInsets.all(16),
        child: Column(
          crossAxisAlignment: CrossAxisAlignment.start,
          children: [
            TextField(
              controller: _controller,
              keyboardType: TextInputType.number,
              inputFormatters: [FilteringTextInputFormatter.digitsOnly],
              style: const TextStyle(color: Colors.white, fontSize: 16),
              decoration: InputDecoration(
                filled: true,
                fillColor: const Color(0xFF2C2C2E),
                border: OutlineInputBorder(
                  borderRadius: BorderRadius.circular(8),
                  borderSide: BorderSide.none,
                ),
                hintText: widget.title,
                hintStyle: const TextStyle(color: Colors.white24),
              ),
              onChanged: _onChanged,
              autofocus: true,
            ),
            if (widget.footer != null)
              Padding(
                padding: const EdgeInsets.only(top: 8),
                child: Text(
                  widget.footer!,
                  style: const TextStyle(color: Colors.white38, fontSize: 13),
                ),
              ),
          ],
        ),
      ),
    );
  }
}
