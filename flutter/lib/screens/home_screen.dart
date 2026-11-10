import 'package:flutter/cupertino.dart';
import 'package:flutter/material.dart' show Scaffold;
import 'package:flutter/services.dart';
import 'package:xterm/xterm.dart' show TerminalKey;

import '../services/shell_session.dart';
import '../services/tabs_controller.dart';
import '../services/worker_controller.dart';
import '../theme/tvm_colors.dart';
import '../widgets/bottom_bar.dart';
import '../widgets/keyboard_input.dart';
import '../widgets/shell_view.dart';
import '../widgets/tvm_icons.dart';
import 'tvm_tab.dart';

/// The whole app: the open tabs as pages (swipe between neighbours) over
/// the bottom bar.
class HomeScreen extends StatefulWidget {
  const HomeScreen({super.key});

  @override
  State<HomeScreen> createState() => _HomeScreenState();
}

class _HomeScreenState extends State<HomeScreen> {
  final _tabs = TabsController();
  final _pages = PageController();

  /// The app's one keyboard (KeyboardInput): up exactly while this or a
  /// node under it has focus. With a shell in front that node is the
  /// shell's view (for its solid cursor); on the TVM tab it is this one.
  final _keyboard = FocusNode(debugLabel: 'keyboard');

  @override
  void initState() {
    super.initState();
    WorkerController.instance.start();
    _tabs.addListener(_rebuild);
    _keyboard.addListener(_rebuild);
  }

  @override
  void dispose() {
    _keyboard.removeListener(_rebuild);
    _keyboard.dispose();
    _tabs.removeListener(_rebuild);
    for (final s in [..._tabs.shells]) {
      _tabs.close(s);
    }
    _pages.dispose();
    super.dispose();
  }

  void _rebuild() {
    if (mounted) setState(() {});
  }

  bool get _keyboardOpen => _keyboard.hasFocus;

  // What holds focus for the tab in front while the keyboard is up.
  FocusNode get _focusTarget {
    final shell = _tabs.activeShell;
    if (shell == null) return _keyboard;
    shell.viewFocus.canRequestFocus = true;
    return shell.viewFocus;
  }

  // Focus moves between nodes under the keyboard's, so the keyboard itself
  // never goes down while tabs change.
  void _select(int page, {bool fromSwipe = false}) {
    _tabs.select(page);
    if (!fromSwipe && _pages.hasClients && _pages.page?.round() != page) {
      _pages.jumpToPage(page);
    }
    if (_keyboardOpen) _focusTarget.requestFocus();
  }

  // A new shell is for typing into: it takes the keyboard, raising it if
  // it was down. (Its view exists from the next frame; until then the old
  // focus holds, so an open keyboard stays up.)
  void _add() {
    final page = _tabs.add();
    WidgetsBinding.instance.addPostFrameCallback((_) {
      if (_pages.hasClients) _pages.jumpToPage(page);
      _focusTarget.requestFocus();
    });
  }

  Future<void> _close(Iterable<ShellSession> shells) async {
    // Hand focus up to the keyboard's own node before a focused view leaves
    // the tree, or the keyboard would go down with it.
    final open = _keyboardOpen;
    if (open) {
      _keyboard.requestFocus();
      await Future<void>.delayed(Duration.zero);
    }
    for (final s in [...shells]) {
      _tabs.close(s);
    }
    WidgetsBinding.instance.addPostFrameCallback((_) {
      if (_pages.hasClients) _pages.jumpToPage(_tabs.index);
      if (open) _focusTarget.requestFocus();
    });
  }

  void _setKeyboard(bool open) {
    if (open) {
      _focusTarget.requestFocus();
      return;
    }
    if (_keyboardOpen) FocusManager.instance.primaryFocus?.unfocus();
    for (final s in _tabs.shells) {
      s.viewFocus.canRequestFocus = false;
    }
  }

  // Cmd+V (a hardware keyboard, the Mac) pastes; other keys go to the shell
  // in front. On the TVM tab there is nothing to type into.
  KeyEventResult _onKeyEvent(KeyEvent event) {
    final shell = _tabs.activeShell;
    if (shell == null) return KeyEventResult.ignored;
    if (event is KeyDownEvent &&
        event.logicalKey == LogicalKeyboardKey.keyV &&
        HardwareKeyboard.instance.isMetaPressed) {
      Clipboard.getData(Clipboard.kTextPlain).then((data) {
        final text = data?.text;
        if (text != null && text.isNotEmpty) shell.terminal.paste(text);
      }, onError: (_) {});
      return KeyEventResult.handled;
    }
    return shell.handleKeyEvent(event);
  }

  @override
  Widget build(BuildContext context) {
    return AnnotatedRegion<SystemUiOverlayStyle>(
      value: SystemUiOverlayStyle.light,
      child: Scaffold(
        backgroundColor: TvmColors.bg,
        resizeToAvoidBottomInset: true,
        body: KeyboardInput(
          focusNode: _keyboard,
          onInsert: (text) => _tabs.activeShell?.typeText(text),
          onDelete: () => _tabs.activeShell?.sendKey(TerminalKey.backspace),
          onKeyEvent: _onKeyEvent,
          child: Column(
            children: [
              Expanded(
                child: PageView.custom(
                  controller: _pages,
                  onPageChanged: (page) => _select(page, fromSwipe: true),
                  childrenDelegate: SliverChildBuilderDelegate(
                    (context, i) => i == 0
                        // Nothing on it takes focus, which would raise the
                        // keyboard (the segmented control focuses itself on
                        // a tap).
                        ? const ExcludeFocus(
                            key: ValueKey('tvm'),
                            child: TvmTab(),
                          )
                        : ShellView(
                            key: ObjectKey(_tabs.shells[i - 1]),
                            shell: _tabs.shells[i - 1],
                            onClose: () => _close([_tabs.shells[i - 1]]),
                            onTap: () => _setKeyboard(true),
                          ),
                    childCount: _tabs.pageCount,
                    // Pages are found again by key after a tab closes, so the
                    // others keep their state.
                    findChildIndexCallback: (key) {
                      if (key == const ValueKey('tvm')) return 0;
                      if (key is ObjectKey) {
                        final i =
                            _tabs.shells.indexOf(key.value as ShellSession);
                        return i < 0 ? null : i + 1;
                      }
                      return null;
                    },
                  ),
                ),
              ),
              BottomBar(
                tabs: _tabs,
                keyboardOpen: _keyboardOpen,
                onSelect: _select,
                onAdd: _add,
                onKeyboard: _setKeyboard,
                onTabMenu: _showTabMenu,
              ),
            ],
          ),
        ),
      ),
    );
  }

  Future<void> _showTabMenu(ShellSession shell, Rect tab) async {
    final action = await showGeneralDialog<_TabAction>(
      context: context,
      barrierDismissible: true,
      barrierLabel: 'Dismiss',
      barrierColor: const Color(0x80000000),
      // The menu has nothing to type into: leave the keyboard as it is.
      requestFocus: false,
      transitionDuration: const Duration(milliseconds: 150),
      pageBuilder: (context, _, _) => _TabMenu(tab: tab, title: shell.title),
      transitionBuilder: (context, anim, _, child) =>
          FadeTransition(opacity: anim, child: child),
    );
    if (!mounted || action == null) return;
    switch (action) {
      case _TabAction.rename:
        _rename(shell);
      case _TabAction.newShell:
        _add();
      case _TabAction.reconnect:
        shell.reconnect();
      case _TabAction.closeOthers:
        await _close(_tabs.shells.where((s) => s != shell));
        _select(_tabs.shells.indexOf(shell) + 1);
      case _TabAction.close:
        _close([shell]);
    }
  }

  Future<void> _rename(ShellSession shell) async {
    final name = await showCupertinoDialog<String>(
      context: context,
      barrierDismissible: true,
      builder: (context) => _RenameDialog(title: shell.title),
    );
    if (name != null && name.trim().isNotEmpty) shell.title = name.trim();
  }
}

/// Owns its text controller, so the controller lives until the dialog has
/// faded out, not just until it is popped.
class _RenameDialog extends StatefulWidget {
  const _RenameDialog({required this.title});

  final String title;

  @override
  State<_RenameDialog> createState() => _RenameDialogState();
}

class _RenameDialogState extends State<_RenameDialog> {
  late final _controller = TextEditingController(text: widget.title);

  @override
  void dispose() {
    _controller.dispose();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    return CupertinoAlertDialog(
      title: const Text('Rename tab'),
      content: Padding(
        padding: const EdgeInsets.only(top: 12),
        child: CupertinoTextField(
          controller: _controller,
          autofocus: true,
          autocorrect: false,
          style: const TextStyle(fontFamily: TvmColors.mono),
          onSubmitted: (v) => Navigator.pop(context, v),
        ),
      ),
      actions: [
        CupertinoDialogAction(
          onPressed: () => Navigator.pop(context),
          child: const Text('Cancel'),
        ),
        CupertinoDialogAction(
          isDefaultAction: true,
          onPressed: () => Navigator.pop(context, _controller.text),
          child: const Text('Rename'),
        ),
      ],
    );
  }
}

enum _TabAction { rename, newShell, reconnect, closeOthers, close }

/// A shell tab's long-press menu, opening upward from the tab, with a copy
/// of the tab lifted above the dimmed screen.
class _TabMenu extends StatelessWidget {
  const _TabMenu({required this.tab, required this.title});

  final Rect tab;
  final String title;

  @override
  Widget build(BuildContext context) {
    const width = 230.0;
    final screen = MediaQuery.sizeOf(context);
    final left = tab.left.clamp(12.0, screen.width - width - 12);
    Widget item(_TabAction a, String label, List<String> icon,
            {bool destructive = false}) {
      final color = destructive ? TvmColors.red : TvmColors.bright;
      return GestureDetector(
        behavior: HitTestBehavior.opaque,
        onTap: () => Navigator.pop(context, a),
        child: Container(
          height: 46,
          padding: const EdgeInsets.symmetric(horizontal: 16),
          child: Row(
            children: [
              Expanded(
                child: Text(label,
                    style: TextStyle(fontSize: 16, color: color)),
              ),
              StrokeIcon(icon, size: 18, color: color),
            ],
          ),
        ),
      );
    }

    const divider = SizedBox(
        height: 1, child: ColoredBox(color: TvmColors.line));
    return Stack(
      children: [
        Positioned(
          left: tab.left,
          top: tab.top,
          child: Container(
            height: tab.height,
            padding: const EdgeInsets.symmetric(horizontal: 9),
            decoration: BoxDecoration(
              color: TvmColors.card,
              border: Border.all(color: TvmColors.line),
              borderRadius: BorderRadius.circular(11),
              boxShadow: const [
                BoxShadow(color: Color(0x80000000), blurRadius: 24),
              ],
            ),
            child: Row(
              mainAxisSize: MainAxisSize.min,
              children: [
                const StrokeIcon(StrokeIcon.shell,
                    size: 18, color: TvmColors.bright),
                const SizedBox(width: 6),
                Text(title,
                    style: const TextStyle(
                        fontFamily: TvmColors.mono,
                        fontSize: 13,
                        fontWeight: FontWeight.w500,
                        color: TvmColors.bright)),
              ],
            ),
          ),
        ),
        Positioned(
          left: left,
          bottom: screen.height - tab.top + 10,
          width: width,
          child: Container(
            decoration: BoxDecoration(
              color: TvmColors.card,
              border: Border.all(color: TvmColors.line),
              borderRadius: BorderRadius.circular(14),
              boxShadow: const [
                BoxShadow(
                    color: Color(0x99000000),
                    blurRadius: 50,
                    offset: Offset(0, 20)),
              ],
            ),
            clipBehavior: Clip.antiAlias,
            child: Column(
              mainAxisSize: MainAxisSize.min,
              children: [
                item(_TabAction.rename, 'Rename', StrokeIcon.rename),
                divider,
                item(_TabAction.newShell, 'New shell', StrokeIcon.shell),
                divider,
                item(_TabAction.reconnect, 'Reconnect', StrokeIcon.reconnect),
                const SizedBox(
                    height: 7, child: ColoredBox(color: TvmColors.bg)),
                item(_TabAction.closeOthers, 'Close others',
                    StrokeIcon.closeOthers),
                divider,
                item(_TabAction.close, 'Close tab', StrokeIcon.close,
                    destructive: true),
              ],
            ),
          ),
        ),
      ],
    );
  }
}
