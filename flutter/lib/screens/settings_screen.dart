import 'package:flutter/material.dart';
import 'package:package_info_plus/package_info_plus.dart';

import '../services/worker_controller.dart';
import '../widgets/number_editor_screen.dart';
import '../widgets/password_editor_screen.dart';

class SettingsScreen extends StatefulWidget {
  const SettingsScreen({super.key});

  @override
  State<SettingsScreen> createState() => _SettingsScreenState();
}

class _SettingsScreenState extends State<SettingsScreen> {
  final _worker = WorkerController.instance;
  PackageInfo? _packageInfo;

  @override
  void initState() {
    super.initState();
    PackageInfo.fromPlatform().then((info) {
      setState(() => _packageInfo = info);
    });
  }

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      backgroundColor: const Color(0xFF1C1C1E),
      appBar: AppBar(title: const Text('Settings')),
      body: ListView(
        children: [
          _SectionHeader(title: 'Console'),
          _SettingsRow(
            title: 'Font Size',
            value: '${_worker.consoleFontSize.toInt()}',
            onTap: () => _pushNumberEditor(
              title: 'Font Size',
              value: _worker.consoleFontSize.toInt(),
              range: (10, 30),
              footer: 'Recommended range: 10–30',
              onChanged: (v) =>
                  setState(() => _worker.consoleFontSize = v.toDouble()),
            ),
          ),
          _SectionHeader(title: 'Network'),
          _SettingsRow(
            title: 'SSH Port',
            value: '${_worker.sshPort}',
            onTap: () => _pushNumberEditor(
              title: 'SSH Port',
              value: _worker.sshPort,
              range: (1, 65535),
              footer: 'Enter a value between 1 and 65535.',
              onChanged: (v) => setState(() => _worker.sshPort = v),
            ),
          ),
          _SettingsRow(
            title: 'SSH Pass',
            value: '*' * _worker.sshPass.length.clamp(1, 20),
            onTap: () async {
              final result = await Navigator.push<String>(
                context,
                MaterialPageRoute(
                  builder: (_) => PasswordEditorScreen(
                    title: 'SSH Pass',
                    initialValue: _worker.sshPass,
                    footer: 'Default password is alpine.',
                  ),
                ),
              );
              if (result != null) {
                setState(() => _worker.sshPass = result);
              }
            },
          ),
          _SectionHeader(title: 'About'),
          _InfoRow(
            title: 'Version',
            value: _packageInfo?.version ?? '...',
          ),
          _InfoRow(
            title: 'Build',
            value: _packageInfo?.buildNumber ?? '...',
          ),
          const Padding(
            padding: EdgeInsets.symmetric(vertical: 16),
            child: Center(
              child: Text(
                'tvm by yuvalino',
                style: TextStyle(color: Colors.white38, fontSize: 13),
              ),
            ),
          ),
        ],
      ),
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
      MaterialPageRoute(
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

class _SectionHeader extends StatelessWidget {
  const _SectionHeader({required this.title});
  final String title;

  @override
  Widget build(BuildContext context) {
    return Padding(
      padding: const EdgeInsets.fromLTRB(16, 24, 16, 8),
      child: Text(
        title.toUpperCase(),
        style: const TextStyle(
          color: Colors.white38,
          fontSize: 13,
          fontWeight: FontWeight.w600,
          letterSpacing: 0.5,
        ),
      ),
    );
  }
}

class _SettingsRow extends StatelessWidget {
  const _SettingsRow({
    required this.title,
    required this.value,
    required this.onTap,
  });

  final String title;
  final String value;
  final VoidCallback onTap;

  @override
  Widget build(BuildContext context) {
    return ListTile(
      tileColor: const Color(0xFF2C2C2E),
      title: Text(title, style: const TextStyle(color: Colors.white)),
      trailing: Row(
        mainAxisSize: MainAxisSize.min,
        children: [
          Text(value, style: const TextStyle(color: Colors.white38)),
          const SizedBox(width: 4),
          const Icon(Icons.chevron_right, color: Colors.white24, size: 20),
        ],
      ),
      onTap: onTap,
    );
  }
}

class _InfoRow extends StatelessWidget {
  const _InfoRow({required this.title, required this.value});

  final String title;
  final String value;

  @override
  Widget build(BuildContext context) {
    return ListTile(
      tileColor: const Color(0xFF2C2C2E),
      title: Text(title, style: const TextStyle(color: Colors.white)),
      trailing: Text(value, style: const TextStyle(color: Colors.white38)),
    );
  }
}
