import 'package:flutter/cupertino.dart';
import 'package:flutter/gestures.dart' show kLongPressTimeout, kTouchSlop;
import 'package:xterm/xterm.dart';

import '../services/shell_session.dart';
import '../services/worker_controller.dart';
import '../theme/tvm_colors.dart';

/// A shell tab's page: the terminal, edge to edge, from under the status
/// bar down to the bottom bar.
class ShellView extends StatefulWidget {
  const ShellView({
    super.key,
    required this.shell,
    required this.onClose,
    required this.onTap,
  });

  final ShellSession shell;
  final VoidCallback onClose;

  /// A tap on the terminal: raises the keyboard.
  final VoidCallback onTap;

  @override
  State<ShellView> createState() => _ShellViewState();
}

// Kept alive off screen, so a tab swiped away keeps its view (scroll
// position, selection) for when it comes back.
class _ShellViewState extends State<ShellView>
    with AutomaticKeepAliveClientMixin {
  // Where a touch went down: a tap is one that comes up soon after, close
  // by. xterm's own tap callback (onTapUp) is never called in 4.0.0, and a
  // raw pointer listener stays out of the gesture arena, so the terminal's
  // selection and the page swipe work as before.
  Offset? _downAt;
  Duration? _downTime;

  @override
  bool get wantKeepAlive => true;

  @override
  Widget build(BuildContext context) {
    super.build(context);
    final shell = widget.shell;
    return ColoredBox(
      color: TvmColors.bg,
      child: SafeArea(
        bottom: false,
        child: Stack(
          children: [
            Listener(
              onPointerDown: (e) {
                _downAt = e.position;
                _downTime = e.timeStamp;
              },
              onPointerUp: (e) {
                final at = _downAt, time = _downTime;
                _downAt = _downTime = null;
                if (at == null || time == null) return;
                if ((e.position - at).distance <= kTouchSlop &&
                    e.timeStamp - time < kLongPressTimeout) {
                  widget.onTap();
                }
              },
              onPointerCancel: (_) => _downAt = _downTime = null,
              // The view only draws and takes focus (for its cursor): typing
              // reaches the shell through the app's one keyboard.
              child: Focus(
                focusNode: shell.viewFocus,
                child: ListenableBuilder(
                  listenable: Listenable.merge([
                    shell,
                    WorkerController.instance.terminalFontSizeNotifier,
                  ]),
                  // TerminalView insets its rows by the MediaQuery padding.
                  // The home indicator's bottom inset is the bottom bar's,
                  // below this; left in, the terminal would lose rows to it
                  // with the keyboard down and get them back with it up.
                  builder: (context, _) => MediaQuery.removePadding(
                    context: context,
                    removeBottom: true,
                    child: TerminalView(
                      shell.terminal,
                      // Only the tab in front follows the screen's size; one
                      // coming to the front catches up on its first layout.
                      autoResize: shell.active,
                      focusNode: shell.viewFocus,
                      scrollController: shell.scroll,
                      hardwareKeyboardOnly: true,
                      readOnly: true,
                      theme: tvmTerminalTheme,
                      textStyle: TerminalStyle(
                        fontSize: WorkerController
                            .instance
                            .terminalFontSizeNotifier
                            .value,
                      ),
                      padding: const EdgeInsets.symmetric(
                        horizontal: 6,
                        vertical: 4,
                      ),
                    ),
                  ),
                ),
              ),
            ),
            ListenableBuilder(
              listenable: shell,
              builder: (context, _) => shell.state == ShellState.ended
                  ? Positioned(
                      left: 12,
                      right: 12,
                      bottom: 12,
                      child: _EndedBanner(
                        reason: shell.endReason ?? '',
                        onReconnect: shell.reconnect,
                        onClose: widget.onClose,
                      ),
                    )
                  : const SizedBox.shrink(),
            ),
          ],
        ),
      ),
    );
  }
}

class _EndedBanner extends StatelessWidget {
  const _EndedBanner({
    required this.reason,
    required this.onReconnect,
    required this.onClose,
  });

  final String reason;
  final VoidCallback onReconnect;
  final VoidCallback onClose;

  @override
  Widget build(BuildContext context) {
    return Container(
      padding: const EdgeInsets.fromLTRB(14, 12, 12, 12),
      decoration: BoxDecoration(
        color: TvmColors.card,
        border: Border.all(color: TvmColors.line),
        borderRadius: BorderRadius.circular(14),
      ),
      child: Row(
        children: [
          Expanded(
            child: Column(
              crossAxisAlignment: CrossAxisAlignment.start,
              mainAxisSize: MainAxisSize.min,
              children: [
                const Text(
                  'Session ended',
                  style: TextStyle(
                    color: TvmColors.bright,
                    fontSize: 14,
                    fontWeight: FontWeight.w500,
                  ),
                ),
                const SizedBox(height: 3),
                Text(
                  reason,
                  maxLines: 2,
                  overflow: TextOverflow.ellipsis,
                  style: const TextStyle(
                    color: TvmColors.dim,
                    fontSize: 12,
                    fontFamily: TvmColors.mono,
                  ),
                ),
              ],
            ),
          ),
          CupertinoButton(
            padding: const EdgeInsets.symmetric(horizontal: 12),
            minimumSize: const Size(0, 36),
            onPressed: onClose,
            child: const Text(
              'Close',
              style: TextStyle(color: TvmColors.dim, fontSize: 14),
            ),
          ),
          CupertinoButton(
            padding: const EdgeInsets.symmetric(horizontal: 14),
            minimumSize: const Size(0, 36),
            color: TvmColors.green,
            borderRadius: BorderRadius.circular(10),
            onPressed: onReconnect,
            child: const Text(
              'Reconnect',
              style: TextStyle(
                color: TvmColors.bg,
                fontSize: 14,
                fontWeight: FontWeight.w600,
              ),
            ),
          ),
        ],
      ),
    );
  }
}
