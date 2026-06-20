import 'package:flutter/material.dart';

class PasswordEditorScreen extends StatefulWidget {
  const PasswordEditorScreen({
    super.key,
    required this.title,
    required this.initialValue,
    this.footer,
  });

  final String title;
  final String initialValue;
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
    return Scaffold(
      backgroundColor: const Color(0xFF1C1C1E),
      appBar: AppBar(
        title: Text(widget.title),
        actions: [
          TextButton(
            onPressed: () => Navigator.pop(context, _controller.text),
            child: const Text('Save'),
          ),
        ],
      ),
      body: Padding(
        padding: const EdgeInsets.all(16),
        child: Column(
          crossAxisAlignment: CrossAxisAlignment.start,
          children: [
            TextField(
              controller: _controller,
              obscureText: !_showPassword,
              autocorrect: false,
              enableSuggestions: false,
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
                suffixIcon: IconButton(
                  icon: Icon(
                    _showPassword ? Icons.visibility_off : Icons.visibility,
                    color: Colors.white38,
                  ),
                  onPressed: () =>
                      setState(() => _showPassword = !_showPassword),
                ),
              ),
              autofocus: true,
            ),
            const SizedBox(height: 12),
            SwitchListTile(
              contentPadding: EdgeInsets.zero,
              title: const Text(
                'Show Password',
                style: TextStyle(color: Colors.white),
              ),
              value: _showPassword,
              onChanged: (v) => setState(() => _showPassword = v),
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
