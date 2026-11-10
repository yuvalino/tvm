import 'dart:async';

import 'package:flutter/cupertino.dart';
import 'package:xterm/xterm.dart';

import '../services/shell_session.dart';
import '../services/tabs_controller.dart';
import '../theme/tvm_colors.dart';
import 'tvm_icons.dart';

/// The bar at the bottom of every page. Closed (keyboard down) it is one
/// row: the tabs, "+", and a button that raises the keyboard. Open (keyboard
/// up) it is two: the tabs and "+", then the key bar and a button that
/// lowers the keyboard.
class BottomBar extends StatefulWidget {
  const BottomBar({
    super.key,
    required this.tabs,
    required this.keyboardOpen,
    required this.onSelect,
    required this.onAdd,
    required this.onKeyboard,
    required this.onTabMenu,
  });

  final TabsController tabs;
  final bool keyboardOpen;
  final ValueChanged<int> onSelect;
  final VoidCallback onAdd;
  final ValueChanged<bool> onKeyboard;
  final void Function(ShellSession shell, Rect tabRect) onTabMenu;

  @override
  State<BottomBar> createState() => _BottomBarState();
}

class _BottomBarState extends State<BottomBar> {
  final _strip = ScrollController();
  final _tabKeys = <ShellSession, GlobalKey>{};
  bool _collapsed = false;
  bool _atEnd = true;
  (int, int, bool)? _shown;

  @override
  void initState() {
    super.initState();
    _strip.addListener(_onStripScroll);
  }

  @override
  void dispose() {
    _strip.dispose();
    super.dispose();
  }

  // The TVM tab stays pinned at the left; once the strip is scrolled off its
  // start it drops the label and keeps only the logo.
  void _onStripScroll() {
    final collapsed = _strip.hasClients && _strip.offset > 2;
    final atEnd =
        !_strip.hasClients ||
        _strip.offset >= _strip.position.maxScrollExtent - 2;
    if (collapsed != _collapsed || atEnd != _atEnd) {
      setState(() {
        _collapsed = collapsed;
        _atEnd = atEnd;
      });
    }
  }

  // Bring the tab in front into view, scrolling the strip as little as
  // possible.
  void _revealActive() {
    final shell = widget.tabs.activeShell;
    if (shell == null) {
      if (_strip.hasClients && _strip.offset > 0) {
        _strip.animateTo(
          0,
          duration: const Duration(milliseconds: 200),
          curve: Curves.easeOut,
        );
      }
      return;
    }
    if (!_strip.hasClients) return;
    final ctx = _tabKeys[shell]?.currentContext;
    final tab = ctx?.findRenderObject() as RenderBox?;
    final strip =
        _strip.position.context.notificationContext?.findRenderObject()
            as RenderBox?;
    if (ctx == null || tab == null || strip == null) return;
    final left = tab.localToGlobal(Offset.zero).dx;
    final right = left + tab.size.width;
    final viewLeft = strip.localToGlobal(Offset.zero).dx;
    final viewRight = viewLeft + strip.size.width;
    // Scrolled at all, the folded TVM tab covers the strip's start.
    final coveredLeft = viewLeft + _tvmFoldedWidth;
    final double delta = right > viewRight
        ? right - viewRight
        : left < coveredLeft
        ? left - coveredLeft
        : 0;
    if (delta == 0) return;
    final p = _strip.position;
    var target = p.pixels + delta;
    // Close enough to the start that only the TVM tab's label would cover
    // it: go all the way, and the label comes back out.
    if (target <= _tvmWidth + _gap - _tvmFoldedWidth + 1) target = 0;
    _strip.animateTo(
      target.clamp(p.minScrollExtent, p.maxScrollExtent),
      duration: const Duration(milliseconds: 200),
      curve: Curves.easeOut,
    );
  }

  @override
  Widget build(BuildContext context) {
    final tabs = widget.tabs;
    WidgetsBinding.instance.addPostFrameCallback((_) {
      if (mounted) _onStripScroll();
    });
    // Reveal on a tab change, and when the strip changes width with the mode.
    final shown = (tabs.index, tabs.pageCount, widget.keyboardOpen);
    if (shown != _shown) {
      _shown = shown;
      WidgetsBinding.instance.addPostFrameCallback((_) => _revealActive());
    }
    _tabKeys.removeWhere((s, _) => !tabs.shells.contains(s));
    final shell = tabs.activeShell;
    final open = widget.keyboardOpen;

    final tabRow = Row(
      children: [
        // The strip runs under the TVM tab, which floats over its start:
        // folding the label away only uncovers strip, so the tabs never
        // shift under a finger that is scrolling them.
        Expanded(
          child: Stack(
            children: [
              Positioned.fill(child: _buildStrip(tabs)),
              Positioned(left: 0, top: 0, bottom: 0, child: _buildTvmTab(tabs)),
            ],
          ),
        ),
        const SizedBox(width: 6),
        _SquareButton(
          key: const ValueKey('add-tab'),
          dashed: true,
          onTap: widget.onAdd,
          child: const StrokeIcon(StrokeIcon.plus, color: TvmColors.green),
        ),
        if (!open) ...[
          const SizedBox(width: 6),
          _SquareButton(
            onTap: () => widget.onKeyboard(true),
            child: const StrokeIcon(
              StrokeIcon.keyboardUp,
              color: TvmColors.yellow,
              strokeWidth: 1.8,
            ),
          ),
        ],
      ],
    );

    return Container(
      decoration: const BoxDecoration(
        color: TvmColors.bar,
        border: Border(top: BorderSide(color: TvmColors.line)),
      ),
      // With the keyboard up the bar sits right on it; down, it clears the
      // home indicator.
      child: SafeArea(
        top: false,
        bottom: !open,
        child: Padding(
          padding: const EdgeInsets.fromLTRB(8, 8, 8, 8),
          child: Column(
            mainAxisSize: MainAxisSize.min,
            children: [
              SizedBox(height: 40, child: tabRow),
              if (open) ...[
                const SizedBox(height: 8),
                SizedBox(
                  height: 38,
                  child: Row(
                    children: [
                      Expanded(child: _KeyBar(shell: shell)),
                      const SizedBox(width: 6),
                      _SquareButton(
                        onTap: () => widget.onKeyboard(false),
                        child: const StrokeIcon(
                          StrokeIcon.keyboardDown,
                          color: TvmColors.yellow,
                          strokeWidth: 1.8,
                        ),
                      ),
                    ],
                  ),
                ),
              ],
            ],
          ),
        ),
      ),
    );
  }

  // The TVM tab's width with its label out, and the room it takes folded
  // (the logo, the divider and the gap after it).
  static const _gap = 6.0;
  static final double _tvmWidth = () {
    final label = TextPainter(
      text: const TextSpan(
        text: 'tvm',
        style: TextStyle(
          fontFamily: TvmColors.mono,
          fontSize: 13,
          fontWeight: FontWeight.w700,
        ),
      ),
      textDirection: TextDirection.ltr,
    )..layout();
    // border + padding + logo + gap + label + padding + border
    return 1 + 9 + 24 + 6 + label.width.ceilToDouble() + 9 + 1;
  }();
  static const _tvmFoldedWidth = 1 + 9 + 24 + 9 + 1 + _gap + 1 + _gap;

  Widget _buildTvmTab(TabsController tabs) {
    return Row(
      children: [
        Container(
          color: TvmColors.bar,
          child: Row(
            children: [
              _Chip(
                selected: tabs.index == 0,
                onTap: () => widget.onSelect(0),
                icon: const TvmLogo(size: 24),
                label: 'tvm',
                labelShown: !_collapsed,
                bold: true,
              ),
              AnimatedContainer(
                duration: _fold,
                curve: Curves.easeOut,
                width: _collapsed ? 1 : 0,
                height: 28,
                margin: EdgeInsets.only(left: _collapsed ? _gap : 0),
                color: _collapsed ? TvmColors.line : const Color(0x00000000),
              ),
              SizedBox(width: _collapsed ? _gap : 0),
            ],
          ),
        ),
        // Tabs passing under the folded tab fade out rather than cut off.
        IgnorePointer(
          child: AnimatedOpacity(
            duration: _fold,
            opacity: _collapsed ? 1 : 0,
            child: Container(
              width: 12,
              decoration: const BoxDecoration(
                gradient: LinearGradient(
                  colors: [TvmColors.bar, Color(0x00262624)],
                ),
              ),
            ),
          ),
        ),
      ],
    );
  }

  Widget _buildStrip(TabsController tabs) {
    return ShaderMask(
      // Tabs fade out under the right edge while there are more past it.
      shaderCallback: (rect) => LinearGradient(
        colors: [
          const Color(0xFF000000),
          const Color(0xFF000000),
          _atEnd ? const Color(0xFF000000) : const Color(0x00000000),
        ],
        stops: const [0, 0.92, 1],
      ).createShader(rect),
      blendMode: BlendMode.dstIn,
      // A plain row rather than a lazy list: there are never many tabs, and
      // revealing one needs it laid out even while it is off screen.
      child: SingleChildScrollView(
        controller: _strip,
        scrollDirection: Axis.horizontal,
        child: Row(
          children: [
            SizedBox(width: _tvmWidth + _gap),
            for (final (i, s) in tabs.shells.indexed) ...[
              if (i > 0) const SizedBox(width: _gap),
              _tabChip(tabs, i, s),
            ],
          ],
        ),
      ),
    );
  }

  Widget _tabChip(TabsController tabs, int i, ShellSession s) {
    final key = _tabKeys.putIfAbsent(s, GlobalKey.new);
    return _Chip(
      key: key,
      selected: tabs.index == i + 1,
      onTap: () => widget.onSelect(i + 1),
      onLongPress: () {
        final box = key.currentContext?.findRenderObject() as RenderBox?;
        if (box == null) return;
        widget.onTabMenu(s, box.localToGlobal(Offset.zero) & box.size);
      },
      icon: const StrokeIcon(StrokeIcon.shell, size: 18),
      label: s.title,
      dot: s.unseen,
    );
  }
}

const _fold = Duration(milliseconds: 220);

class _Chip extends StatelessWidget {
  const _Chip({
    super.key,
    required this.selected,
    required this.onTap,
    required this.icon,
    this.onLongPress,
    this.label,
    this.labelShown = true,
    this.dot = false,
    this.bold = false,
  });

  final bool selected;
  final VoidCallback onTap;
  final VoidCallback? onLongPress;
  final Widget icon;
  final String? label;

  /// Whether [label] is out; it folds away and back with a transition.
  final bool labelShown;
  final bool dot;
  final bool bold;

  @override
  Widget build(BuildContext context) {
    final color = selected
        ? TvmColors.bright
        : bold
        ? TvmColors.fg
        : TvmColors.dim;
    return GestureDetector(
      behavior: HitTestBehavior.opaque,
      onTap: onTap,
      onLongPress: onLongPress,
      child: AnimatedContainer(
        duration: const Duration(milliseconds: 150),
        height: 40,
        padding: const EdgeInsets.symmetric(horizontal: 9),
        decoration: BoxDecoration(
          color: selected ? TvmColors.card : const Color(0x00000000),
          border: Border.all(
            color: selected ? TvmColors.line : const Color(0x00000000),
          ),
          borderRadius: BorderRadius.circular(11),
        ),
        child: IconTheme(
          data: IconThemeData(color: color),
          child: Row(
            mainAxisSize: MainAxisSize.min,
            children: [
              icon,
              if (label != null)
                ClipRect(
                  child: AnimatedSize(
                    duration: _fold,
                    curve: Curves.easeOut,
                    alignment: Alignment.centerLeft,
                    child: AnimatedOpacity(
                      duration: _fold,
                      opacity: labelShown ? 1 : 0,
                      child: labelShown
                          ? Padding(
                              padding: const EdgeInsets.only(left: 6),
                              child: Text(
                                label!,
                                maxLines: 1,
                                softWrap: false,
                                style: TextStyle(
                                  fontFamily: TvmColors.mono,
                                  fontSize: 13,
                                  fontWeight: bold
                                      ? FontWeight.w700
                                      : FontWeight.w500,
                                  color: color,
                                ),
                              ),
                            )
                          : const SizedBox(height: 0),
                    ),
                  ),
                ),
              if (dot) ...[
                const SizedBox(width: 6),
                Container(
                  width: 6,
                  height: 6,
                  decoration: const BoxDecoration(
                    color: TvmColors.green,
                    shape: BoxShape.circle,
                  ),
                ),
              ],
            ],
          ),
        ),
      ),
    );
  }
}

class _SquareButton extends StatelessWidget {
  const _SquareButton({
    super.key,
    required this.onTap,
    required this.child,
    this.dashed = false,
  });

  final VoidCallback onTap;
  final Widget child;
  final bool dashed;

  @override
  Widget build(BuildContext context) {
    return GestureDetector(
      behavior: HitTestBehavior.opaque,
      onTap: onTap,
      child: CustomPaint(
        painter: dashed ? _DashedBorder() : null,
        child: Container(
          width: 40,
          height: 40,
          alignment: Alignment.center,
          decoration: dashed
              ? null
              : BoxDecoration(
                  color: TvmColors.card,
                  border: Border.all(color: TvmColors.line),
                  borderRadius: BorderRadius.circular(11),
                ),
          child: child,
        ),
      ),
    );
  }
}

class _DashedBorder extends CustomPainter {
  @override
  void paint(Canvas canvas, Size size) {
    final rrect = RRect.fromRectAndRadius(
      (Offset.zero & size).deflate(0.5),
      const Radius.circular(11),
    );
    final paint = Paint()
      ..color = TvmColors.line
      ..style = PaintingStyle.stroke;
    for (final metric in (Path()..addRRect(rrect)).computeMetrics()) {
      for (double d = 0; d < metric.length; d += 7) {
        canvas.drawPath(metric.extractPath(d, d + 4), paint);
      }
    }
  }

  @override
  bool shouldRepaint(_DashedBorder oldDelegate) => false;
}

/// The keys a phone keyboard lacks. ctrl and alt latch for the next key;
/// the arrows repeat while held.
class _KeyBar extends StatelessWidget {
  const _KeyBar({required this.shell});

  /// The shell in front; null on the TVM tab, where the keys are dimmed.
  final ShellSession? shell;

  @override
  Widget build(BuildContext context) {
    return ListenableBuilder(
      listenable: shell ?? Listenable.merge(const []),
      builder: (context, _) {
        final keys = <Widget>[
          _Key(label: 'esc', onTap: () => shell?.sendKey(TerminalKey.escape)),
          _Key(
            label: 'ctrl',
            modifier: true,
            latched: shell?.ctrl ?? false,
            onTap: () => shell?.toggleCtrl(),
          ),
          _Key(
            label: 'alt',
            modifier: true,
            latched: shell?.alt ?? false,
            onTap: () => shell?.toggleAlt(),
          ),
          _Key(label: 'tab', onTap: () => shell?.sendKey(TerminalKey.tab)),
          for (final (icon, key) in [
            (StrokeIcon.left, TerminalKey.arrowLeft),
            (StrokeIcon.up, TerminalKey.arrowUp),
            (StrokeIcon.down, TerminalKey.arrowDown),
            (StrokeIcon.right, TerminalKey.arrowRight),
          ])
            _Key(
              icon: StrokeIcon(icon, size: 15, strokeWidth: 2.2),
              repeat: true,
              onTap: () => shell?.sendKey(key),
            ),
          for (final c in ['|', '~', '/', '-'])
            _Key(label: c, onTap: () => shell?.sendText(c)),
        ];
        return IgnorePointer(
          ignoring: shell == null,
          child: AnimatedOpacity(
            duration: const Duration(milliseconds: 150),
            opacity: shell == null ? 0.35 : 1,
            child: ShaderMask(
              shaderCallback: (rect) => const LinearGradient(
                colors: [
                  Color(0xFF000000),
                  Color(0xFF000000),
                  Color(0x00000000),
                ],
                stops: [0, 0.9, 1],
              ).createShader(rect),
              blendMode: BlendMode.dstIn,
              child: ListView.separated(
                scrollDirection: Axis.horizontal,
                itemCount: keys.length,
                separatorBuilder: (_, _) => const SizedBox(width: 6),
                itemBuilder: (_, i) => keys[i],
              ),
            ),
          ),
        );
      },
    );
  }
}

class _Key extends StatefulWidget {
  const _Key({
    required this.onTap,
    this.label,
    this.icon,
    this.modifier = false,
    this.latched = false,
    this.repeat = false,
  });

  final VoidCallback onTap;
  final String? label;
  final Widget? icon;
  final bool modifier;
  final bool latched;
  final bool repeat;

  @override
  State<_Key> createState() => _KeyState();
}

class _KeyState extends State<_Key> {
  Timer? _repeat;
  bool _down = false;

  @override
  void dispose() {
    _repeat?.cancel();
    super.dispose();
  }

  void _stop() {
    _repeat?.cancel();
    _repeat = null;
    if (_down) setState(() => _down = false);
  }

  @override
  Widget build(BuildContext context) {
    final fg = widget.latched
        ? TvmColors.bg
        : widget.modifier
        ? TvmColors.yellow
        : TvmColors.fg;
    return GestureDetector(
      behavior: HitTestBehavior.opaque,
      onTapDown: (_) => setState(() => _down = true),
      onTapUp: (_) => _stop(),
      onTapCancel: _stop,
      onTap: widget.onTap,
      onLongPressStart: widget.repeat
          ? (_) {
              widget.onTap();
              _repeat = Timer.periodic(
                const Duration(milliseconds: 70),
                (_) => widget.onTap(),
              );
            }
          : null,
      onLongPressEnd: widget.repeat ? (_) => _stop() : null,
      child: Container(
        constraints: const BoxConstraints(minWidth: 40),
        padding: const EdgeInsets.symmetric(horizontal: 9),
        alignment: Alignment.center,
        decoration: BoxDecoration(
          color: widget.latched
              ? TvmColors.yellow
              : _down
              ? TvmColors.highlight
              : TvmColors.card,
          border: Border.all(
            color: widget.latched ? TvmColors.yellow : TvmColors.line,
          ),
          borderRadius: BorderRadius.circular(9),
        ),
        child: widget.icon != null
            ? IconTheme(
                data: IconThemeData(color: fg),
                child: widget.icon!,
              )
            : Text(
                widget.label!,
                style: TextStyle(
                  fontFamily: TvmColors.mono,
                  fontSize: 13,
                  fontWeight: FontWeight.w500,
                  color: fg,
                ),
              ),
      ),
    );
  }
}
