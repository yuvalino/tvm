import 'package:flutter/cupertino.dart';
import 'package:flutter/rendering.dart' show ScrollDirection;
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

class _LogViewState extends State<LogView> with AutomaticKeepAliveClientMixin {
  final _scroll = ScrollController();
  final _worker = WorkerController.instance;

  @override
  bool get wantKeepAlive => true;

  @override
  void initState() {
    super.initState();
    _worker.logs.addListener(_onLogs);
    _scroll.addListener(_onScroll);
    _jumpToEnd();
  }

  @override
  void dispose() {
    _worker.logs.removeListener(_onLogs);
    _scroll.dispose();
    super.dispose();
  }

  void _onLogs() {
    if (_worker.autoScroll.value) _jumpToEnd(animate: true);
  }

  // Scrolling up by hand stops following; Follow (or reaching the end
  // again) resumes it.
  void _onScroll() {
    if (!_scroll.hasClients) return;
    final p = _scroll.position;
    if (p.userScrollDirection == ScrollDirection.forward &&
        p.pixels < p.maxScrollExtent - 20) {
      _worker.autoScroll.value = false;
    }
  }

  void _jumpToEnd({bool animate = false}) {
    WidgetsBinding.instance.addPostFrameCallback((_) {
      if (!_scroll.hasClients) return;
      final end = _scroll.position.maxScrollExtent;
      animate
          ? _scroll.animateTo(end,
              duration: const Duration(milliseconds: 100), curve: Curves.easeOut)
          : _scroll.jumpTo(end);
    });
  }

  @override
  Widget build(BuildContext context) {
    super.build(context);
    return Stack(
      children: [
        ValueListenableBuilder<List<String>>(
          valueListenable: _worker.logs,
          builder: (context, logs, _) => ValueListenableBuilder<double>(
            valueListenable: _worker.terminalFontSizeNotifier,
            builder: (context, fontSize, _) => ListView.builder(
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
        Positioned(
          right: 14,
          bottom: 12,
          child: Row(
            children: [
              _FloatButton(
                icon: StrokeIcon.copy,
                label: 'Copy',
                onTap: () => Clipboard.setData(
                    ClipboardData(text: _worker.logs.value.join('\n'))),
              ),
              const SizedBox(width: 8),
              ValueListenableBuilder<bool>(
                valueListenable: _worker.autoScroll,
                builder: (context, follow, _) => _FloatButton(
                  icon: StrokeIcon.follow,
                  label: 'Follow',
                  on: follow,
                  onTap: () {
                    _worker.autoScroll.value = !follow;
                    if (!follow) _jumpToEnd(animate: true);
                  },
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
    final cmd = 'ssh -p ${w.sshPort} ${w.username}@${host ?? 'localhost'}';
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
    required this.icon,
    required this.label,
    required this.onTap,
    this.on = false,
  });

  final List<String> icon;
  final String label;
  final VoidCallback onTap;
  final bool on;

  @override
  Widget build(BuildContext context) {
    final fg = on ? TvmColors.bg : TvmColors.fg;
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
