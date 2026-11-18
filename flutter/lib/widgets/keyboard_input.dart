import 'package:flutter/services.dart';
import 'package:flutter/widgets.dart';

/// The app's one keyboard: a text input client that lives as long as the
/// home screen and is open exactly while [focusNode] has focus. What is typed
/// goes to whichever tab is in front (through the callbacks), so moving
/// between tabs never closes and reopens the keyboard, as it would if each
/// terminal held a connection of its own.
///
/// Modelled on xterm's CustomTextEdit (not exported by the package).
class KeyboardInput extends StatefulWidget {
  const KeyboardInput({
    super.key,
    required this.focusNode,
    required this.onInsert,
    required this.onDelete,
    required this.onKeyEvent,
    required this.child,
  });

  final FocusNode focusNode;
  final ValueChanged<String> onInsert;
  final VoidCallback onDelete;
  final KeyEventResult Function(KeyEvent event) onKeyEvent;
  final Widget child;

  @override
  State<KeyboardInput> createState() => _KeyboardInputState();
}

class _KeyboardInputState extends State<KeyboardInput> with TextInputClient {
  // iOS sends no event for backspace in an empty field, so the field always
  // holds two spaces: a backspace shows up as the text getting shorter.
  static const _initial = TextEditingValue(
    text: '  ',
    selection: TextSelection.collapsed(offset: 2),
  );

  TextInputConnection? _connection;
  TextEditingValue _current = _initial;

  @override
  void initState() {
    super.initState();
    widget.focusNode.addListener(_onFocusChange);
  }

  @override
  void didUpdateWidget(KeyboardInput oldWidget) {
    super.didUpdateWidget(oldWidget);
    if (oldWidget.focusNode != widget.focusNode) {
      oldWidget.focusNode.removeListener(_onFocusChange);
      widget.focusNode.addListener(_onFocusChange);
    }
  }

  @override
  void dispose() {
    widget.focusNode.removeListener(_onFocusChange);
    _close();
    super.dispose();
  }

  void _onFocusChange() {
    widget.focusNode.hasFocus ? _open() : _close();
  }

  void _open() {
    if (_connection?.attached ?? false) {
      _connection!.show();
      return;
    }
    _connection = TextInput.attach(
      this,
      const TextInputConfiguration(
        // Not plain text: no autocapitalisation or smart punctuation.
        inputType: TextInputType.emailAddress,
        inputAction: TextInputAction.newline,
        keyboardAppearance: Brightness.dark,
        autocorrect: false,
        enableSuggestions: false,
        enableIMEPersonalizedLearning: false,
      ),
    )..show();
    _current = _initial;
    _connection!.setEditingState(_initial);
  }

  void _close() {
    if (_connection?.attached ?? false) _connection!.close();
    _connection = null;
  }

  @override
  TextEditingValue? get currentTextEditingValue => _current;

  @override
  AutofillScope? get currentAutofillScope => null;

  @override
  void updateEditingValue(TextEditingValue value) {
    _current = value;
    // An IME still composing: wait for the committed text.
    if (!value.composing.isCollapsed) return;
    if (value.text.length < _initial.text.length) {
      widget.onDelete();
    } else if (value.text.length > _initial.text.length) {
      widget.onInsert(value.text.substring(_initial.text.length));
    }
    // Put back after every edit, and after the caret is moved (the space
    // bar's trackpad), or the next character would land among the spaces.
    final caret = value.selection;
    if (value.text != _initial.text ||
        caret.baseOffset != caret.extentOffset ||
        caret.extentOffset != _initial.text.length) {
      _current = _initial;
      _connection?.setEditingState(_initial);
    }
  }

  @override
  void performAction(TextInputAction action) {
    if (action == TextInputAction.done) widget.onInsert('\r');
  }

  @override
  void connectionClosed() {
    // The system took the keyboard away; give up focus to match.
    _connection?.connectionClosedReceived();
    _connection = null;
    widget.focusNode.unfocus();
  }

  @override
  void updateFloatingCursor(RawFloatingCursorPoint point) {}

  @override
  void showAutocorrectionPromptRect(int start, int end) {}

  @override
  void performPrivateCommand(String action, Map<String, dynamic> data) {}

  @override
  void insertTextPlaceholder(Size size) {}

  @override
  void removeTextPlaceholder() {}

  @override
  void showToolbar() {}

  @override
  Widget build(BuildContext context) {
    return Focus(
      focusNode: widget.focusNode,
      onKeyEvent: (_, event) => _current.composing.isCollapsed
          ? widget.onKeyEvent(event)
          : KeyEventResult.skipRemainingHandlers,
      child: widget.child,
    );
  }
}
