import 'package:flutter/cupertino.dart';
import 'package:package_info_plus/package_info_plus.dart';

import '../services/worker_controller.dart';
import '../theme/tvm_colors.dart';
import 'number_editor_screen.dart';
import 'password_editor_screen.dart';
import 'tvm_icons.dart';

class SettingsView extends StatefulWidget {
  const SettingsView({super.key});

  @override
  State<SettingsView> createState() => _SettingsViewState();
}

class _SettingsViewState extends State<SettingsView> {
  final _worker = WorkerController.instance;
  PackageInfo? _packageInfo;

  @override
  void initState() {
    super.initState();
    PackageInfo.fromPlatform().then((info) {
      if (mounted) setState(() => _packageInfo = info);
    });
  }

  @override
  Widget build(BuildContext context) {
    final version = _packageInfo == null
        ? '...'
        : '${_packageInfo!.version} (${_packageInfo!.buildNumber})';
    return ListView(
      padding: const EdgeInsets.only(bottom: 24),
      children: [
        const _Header('TERMINAL'),
        _Group([
          _Row(
            icon: StrokeIcon.fontSize,
            tint: TvmColors.blue,
            title: 'Font Size',
            value: '${_worker.terminalFontSize.toInt()}',
            onTap: () => _pushNumberEditor(
              title: 'Font Size',
              value: _worker.terminalFontSize.toInt(),
              range: (8, 30),
              footer: 'Recommended range: 10–20',
              onChanged: (v) =>
                  setState(() => _worker.terminalFontSize = v.toDouble()),
            ),
          ),
        ]),
        const _Footer('The log uses it too.'),
        const _Header('SSH SERVER'),
        _Group([
          _Row(
            icon: StrokeIcon.globe,
            tint: TvmColors.yellow,
            title: 'Port',
            value: '${_worker.sshPort}',
            onTap: () => _pushNumberEditor(
              title: 'SSH Port',
              value: _worker.sshPort,
              range: (1, 65535),
              footer: 'Enter a value between 1 and 65535.',
              onChanged: (v) => setState(() => _worker.sshPort = v),
            ),
          ),
          _Row(
            icon: StrokeIcon.lock,
            tint: TvmColors.red,
            title: 'Password',
            value: '•' * _worker.sshPass.length.clamp(1, 12),
            onTap: () => Navigator.push(
              context,
              CupertinoPageRoute(
                builder: (_) => PasswordEditorScreen(
                  title: 'SSH Password',
                  initialValue: _worker.sshPass,
                  footer: 'Default password is alpine.',
                  onChanged: (v) => setState(() => _worker.sshPass = v),
                ),
              ),
            ),
          ),
        ]),
        const _Footer('Changes apply on next launch.'),
        const _Header('ABOUT'),
        _Group([_Row(title: 'Version', value: version)]),
        const SizedBox(height: 24),
        Row(
          mainAxisAlignment: MainAxisAlignment.center,
          children: [
            for (final c in TvmColors.blocks)
              Container(width: 22, height: 12, color: c),
          ],
        ),
        const SizedBox(height: 8),
        const Center(
          child: Text('tvm by yuvalino',
              style: TextStyle(
                  fontFamily: TvmColors.mono,
                  fontSize: 12,
                  color: TvmColors.dimmer)),
        ),
      ],
    );
  }

  void _pushNumberEditor({
    required String title,
    required int value,
    required (int, int) range,
    required String footer,
    required ValueChanged<int> onChanged,
  }) {
    Navigator.push(
      context,
      CupertinoPageRoute(
        builder: (_) => NumberEditorScreen(
          title: title,
          initialValue: value,
          min: range.$1,
          max: range.$2,
          footer: footer,
          onChanged: onChanged,
        ),
      ),
    );
  }
}

class _Header extends StatelessWidget {
  const _Header(this.text);

  final String text;

  @override
  Widget build(BuildContext context) {
    return Padding(
      padding: const EdgeInsets.fromLTRB(30, 14, 30, 8),
      child: Text(text,
          style: const TextStyle(
              fontFamily: TvmColors.mono,
              fontSize: 12,
              fontWeight: FontWeight.w700,
              letterSpacing: 0.5,
              color: TvmColors.yellow)),
    );
  }
}

class _Footer extends StatelessWidget {
  const _Footer(this.text);

  final String text;

  @override
  Widget build(BuildContext context) {
    return Padding(
      padding: const EdgeInsets.fromLTRB(30, 8, 30, 0),
      child: Text(text,
          style: const TextStyle(
              fontFamily: TvmColors.mono,
              fontSize: 12,
              color: TvmColors.dimmer)),
    );
  }
}

class _Group extends StatelessWidget {
  const _Group(this.rows);

  final List<Widget> rows;

  @override
  Widget build(BuildContext context) {
    return Container(
      margin: const EdgeInsets.symmetric(horizontal: 16),
      decoration: BoxDecoration(
        color: TvmColors.card,
        border: Border.all(color: TvmColors.line),
        borderRadius: BorderRadius.circular(14),
      ),
      clipBehavior: Clip.antiAlias,
      child: Column(
        children: [
          for (var i = 0; i < rows.length; i++) ...[
            if (i > 0) Container(height: 1, color: TvmColors.line),
            rows[i],
          ],
        ],
      ),
    );
  }
}

class _Row extends StatelessWidget {
  const _Row({
    required this.title,
    this.icon,
    this.tint,
    this.value,
    this.onTap,
  });

  final String title;
  final List<String>? icon;
  final Color? tint;
  final String? value;
  final VoidCallback? onTap;

  @override
  Widget build(BuildContext context) {
    return GestureDetector(
      behavior: HitTestBehavior.opaque,
      onTap: onTap,
      child: SizedBox(
        height: 50,
        child: Padding(
          padding: const EdgeInsets.symmetric(horizontal: 14),
          child: Row(
            children: [
              if (icon != null) ...[
                Container(
                  width: 30,
                  height: 30,
                  alignment: Alignment.center,
                  decoration: BoxDecoration(
                      color: tint, borderRadius: BorderRadius.circular(8)),
                  child: StrokeIcon(icon!,
                      size: 17, color: TvmColors.bg, strokeWidth: 2.2),
                ),
                const SizedBox(width: 12),
              ],
              Text(title,
                  style:
                      const TextStyle(fontSize: 16, color: TvmColors.bright)),
              const Spacer(),
              if (value != null)
                Text(value!,
                    style: const TextStyle(
                        fontFamily: TvmColors.mono,
                        fontSize: 15,
                        color: TvmColors.dim)),
              if (onTap != null) ...[
                const SizedBox(width: 6),
                const StrokeIcon(StrokeIcon.right,
                    size: 16, color: TvmColors.dimmer, strokeWidth: 2.4),
              ],
            ],
          ),
        ),
      ),
    );
  }
}
