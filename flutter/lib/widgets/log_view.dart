import 'package:flutter/cupertino.dart';
import 'package:flutter/services.dart';

import '../services/worker_controller.dart';
import '../theme/tvm_colors.dart';
import 'tvm_icons.dart';

/// The app log: how to reach the SSH server, then TVM's output, coloured the
/// way the site colours its neofetch.
class LogView extends StatefulWidget {
  const LogView({super.key});

  @override
  State<LogView> createState() => _LogViewState();
}

class _LogViewState extends State<LogView> {
  final _scroll = ScrollController();
  final _worker = WorkerController.instance;
  // Whether the view shows the log's end; Bottom has nothing to do then.
  final _atBottom = ValueNotifier(true);

  @override
  void initState() {
    super.initState();
    _worker.logs.addListener(_onLogs);
    _scroll.addListener(_onScroll);
    _toEnd();
  }

  @override
  void dispose() {
    _worker.logs.removeListener(_onLogs);
    _scroll.dispose();
    _atBottom.dispose();
    super.dispose();
  }

  void _onLogs() {
    if (_worker.autoScroll.value) _toEnd(animate: true);
  }

  // On a scroll, and on a new line (the end moves away, the view does not).
  void _onScroll() {
    if (!_scroll.hasClients) return;
    final p = _scroll.position;
    _atBottom.value = p.pixels >= p.maxScrollExtent - 20;
  }

  // The list builds lazily: the end it knows of is an estimate until the
  // last lines are laid out, which going there does. So it goes again, a
  // frame later, until the view stays at the end.
  void _toEnd({bool animate = false, int tries = 10}) {
    WidgetsBinding.instance.addPostFrameCallback((_) async {
      if (!mounted || !_scroll.hasClients) return;
      final p = _scroll.position;
      if (p.pixels >= p.maxScrollExtent) return;
      if (animate) {
        await _scroll.animateTo(p.maxScrollExtent,
            duration: const Duration(milliseconds: 200), curve: Curves.easeOut);
      } else {
        _scroll.jumpTo(p.maxScrollExtent);
      }
      if (tries > 0) _toEnd(tries: tries - 1);
    });
    WidgetsBinding.instance.scheduleFrame();
  }

  @override
  Widget build(BuildContext context) {
    return Stack(
      children: [
        ValueListenableBuilder<List<String>>(
          valueListenable: _worker.logs,
          builder: (context, logs, _) => ValueListenableBuilder<double>(
            valueListenable: _worker.terminalFontSizeNotifier,
            builder: (context, fontSize, _) =>
                NotificationListener<ScrollMetricsNotification>(
              onNotification: (_) {
                _onScroll();
                return false;
              },
              child: ListView.builder(
              key: const ValueKey('log-list'),
              controller: _scroll,
              padding: const EdgeInsets.fromLTRB(16, 0, 16, 64),
              itemCount: logs.length + 1,
              itemBuilder: (context, i) => i == 0
                  ? const _ConnectCard()
                  : Padding(
                      padding: const EdgeInsets.symmetric(vertical: 1),
                      child: Text.rich(
                        _colorize(logs[i - 1]),
                        style: TextStyle(
                          fontFamily: TvmColors.mono,
                          fontSize: fontSize,
                          height: 1.45,
                          color: TvmColors.fg,
                        ),
                      ),
                    ),
              ),
            ),
          ),
        ),
        Positioned(
          right: 14,
          bottom: 12,
          child: Row(
            children: [
              // Auto-scroll: each new line brings the view to the end.
              // Bottom: to the end once; nothing to do at the end. Each one's
              // state is in the key around it too, for the UI tests.
              ValueListenableBuilder<bool>(
                valueListenable: _worker.autoScroll,
                builder: (context, on, _) => KeyedSubtree(
                  key: ValueKey('log-auto-scroll-${on ? 'on' : 'off'}'),
                  child: _FloatButton(
                    key: const ValueKey('log-auto-scroll'),
                    icon: StrokeIcon.lock,
                    label: 'Auto-scroll',
                    on: on,
                    onTap: () => _worker.autoScroll.value = !on,
                  ),
                ),
              ),
              const SizedBox(width: 8),
              ValueListenableBuilder<bool>(
                valueListenable: _atBottom,
                builder: (context, atBottom, _) => KeyedSubtree(
                  key: ValueKey('log-bottom-${atBottom ? 'off' : 'on'}'),
                  child: IgnorePointer(
                    ignoring: atBottom,
                    child: _FloatButton(
                      key: const ValueKey('log-bottom'),
                      icon: StrokeIcon.bottom,
                      label: 'Bottom',
                      enabled: !atBottom,
                      onTap: () => _toEnd(animate: true),
                    ),
                  ),
                ),
              ),
            ],
          ),
        ),
      ],
    );
  }
}

const _bold = TextStyle(fontWeight: FontWeight.w700);
final _levels = {
  '[INFO]': _bold.copyWith(color: TvmColors.green),
  '[WARN]': _bold.copyWith(color: TvmColors.yellow),
  '[ERROR]': _bold.copyWith(color: TvmColors.red),
};
// The startup fields WorkerController.start logs as "key: value".
final _field = RegExp(r'^(device|pid|uid|username|hostname|ssh port|cwd|home|mdns name): ');
// The TVM kernel's "yyyy-mm-dd hh:mm:ss.mmm Kernel(tid): " prefix, dropbear's
// "[pid] Mon dd hh:mm:ss ", and the kernel glue's "tvm_app_start: ".
final _prefix = RegExp(r'^(\d{4}-\d\d-\d\d \d\d:\d\d:\d\d\.\d+ Kernel\(\d+\): '
    r'|\[\d+\] \w{3} [ \d]\d \d\d:\d\d:\d\d |tvm_app_start: )');
final _ip = RegExp(r'\b\d{1,3}(?:\.\d{1,3}){3}\b');

TextSpan _colorize(String line) {
  final spans = <TextSpan>[];
  var rest = line;
  for (final e in _levels.entries) {
    if (rest.startsWith(e.key)) {
      spans.add(TextSpan(text: e.key, style: e.value));
      rest = rest.substring(e.key.length);
      break;
    }
  }
  final field = _field.firstMatch(rest);
  final prefix = _prefix.firstMatch(rest);
  if (field != null) {
    spans.add(TextSpan(
        text: field.group(1),
        style: _bold.copyWith(color: TvmColors.yellow)));
    spans.add(TextSpan(text: rest.substring(field.group(1)!.length)));
    return TextSpan(children: spans);
  }
  if (prefix != null) {
    spans.add(TextSpan(
        text: prefix.group(0), style: const TextStyle(color: TvmColors.dim)));
    rest = rest.substring(prefix.end);
  }
  var at = 0;
  for (final m in _ip.allMatches(rest)) {
    spans.add(TextSpan(text: rest.substring(at, m.start)));
    spans.add(TextSpan(
        text: m.group(0), style: const TextStyle(color: TvmColors.blue)));
    at = m.end;
  }
  spans.add(TextSpan(text: rest.substring(at)));
  return TextSpan(children: spans);
}

/// The ssh command for reaching this device, with a copy button. It names
/// the host by its mDNS name, or by its Wi-Fi address until (or unless) the
/// name is known.
class _ConnectCard extends StatelessWidget {
  const _ConnectCard();

  @override
  Widget build(BuildContext context) {
    final w = WorkerController.instance;
    return ValueListenableBuilder(
      valueListenable: w.network,
      builder: (context, net, _) => _build(net.name ?? net.address),
    );
  }

  Widget _build(String? host) {
    final w = WorkerController.instance;
    final cmd = 'ssh -p ${w.serverPort} ${w.username}@${host ?? 'localhost'}';
    return Container(
      margin: const EdgeInsets.only(bottom: 10),
      padding: const EdgeInsets.fromLTRB(12, 10, 4, 4),
      decoration: BoxDecoration(
        color: TvmColors.card,
        border: Border.all(color: TvmColors.line),
        borderRadius: BorderRadius.circular(12),
      ),
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          const Text(
            'CONNECT FROM ANOTHER MACHINE',
            style: TextStyle(
                color: TvmColors.dim,
                fontSize: 11,
                fontWeight: FontWeight.w600,
                letterSpacing: 0.6),
          ),
          Row(
            children: [
              Expanded(
                child: Text(cmd,
                    key: const ValueKey('connect-command'),
                    style: const TextStyle(
                        fontFamily: TvmColors.mono,
                        fontSize: 13,
                        fontWeight: FontWeight.w500,
                        color: TvmColors.bright)),
              ),
              CupertinoButton(
                padding: const EdgeInsets.symmetric(horizontal: 8),
                minimumSize: const Size(0, 34),
                onPressed: () => Clipboard.setData(ClipboardData(text: cmd)),
                child: const Text('Copy',
                    style: TextStyle(
                        color: TvmColors.green,
                        fontSize: 13,
                        fontWeight: FontWeight.w600)),
              ),
            ],
          ),
        ],
      ),
    );
  }
}

class _FloatButton extends StatelessWidget {
  const _FloatButton({
    super.key,
    required this.icon,
    required this.label,
    required this.onTap,
    this.on = false,
    this.enabled = true,
  });

  final List<String> icon;
  final String label;
  final VoidCallback onTap;
  final bool on;
  final bool enabled;

  @override
  Widget build(BuildContext context) {
    final fg = on
        ? TvmColors.bg
        : enabled
            ? TvmColors.fg
            : TvmColors.dimmer;
    return GestureDetector(
      onTap: onTap,
      child: Container(
        height: 36,
        padding: const EdgeInsets.symmetric(horizontal: 12),
        decoration: BoxDecoration(
          color: on ? TvmColors.green : TvmColors.card,
          border: Border.all(color: on ? TvmColors.green : TvmColors.line),
          borderRadius: BorderRadius.circular(10),
          boxShadow: const [
            BoxShadow(
                color: Color(0x66000000), blurRadius: 18, offset: Offset(0, 6)),
          ],
        ),
        child: Row(
          children: [
            StrokeIcon(icon, size: 15, color: fg),
            const SizedBox(width: 6),
            Text(label,
                style: TextStyle(
                    color: fg, fontSize: 12, fontWeight: FontWeight.w500)),
          ],
        ),
      ),
    );
  }
}
