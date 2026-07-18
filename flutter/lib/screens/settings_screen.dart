import 'package:flutter/cupertino.dart';
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
    // Native iOS grouped-section header: small, gray, uppercase, and aligned
    // with the row title text (CupertinoListSection.insetGrouped otherwise
    // renders it 20pt bold at the card edge). The row title of a notched tile
    // without a leading widget sits 28pt in from the card edge, so pad the
    // header to match.
    final headerStyle = TextStyle(
      fontSize: 13,
      fontWeight: FontWeight.w400,
      color: CupertinoColors.secondaryLabel.resolveFrom(context),
    );
    Widget sectionHeader(String title) => Padding(
          padding: const EdgeInsetsDirectional.only(start: 28),
          child: Text(title.toUpperCase(), style: headerStyle),
        );
    return CupertinoPageScaffold(
      backgroundColor: CupertinoColors.black,
      child: CustomScrollView(
        slivers: [
          const CupertinoSliverNavigationBar(
            largeTitle: Text('Settings'),
            previousPageTitle: 'tvm',
          ),
          SliverToBoxAdapter(
            child: Column(
              children: [
                CupertinoListSection.insetGrouped(
                  header: sectionHeader('Console'),
                  children: [
                    CupertinoListTile.notched(
                      title: const Text('Font Size'),
                      additionalInfo:
                          Text('${_worker.consoleFontSize.toInt()}'),
                      trailing: const CupertinoListTileChevron(),
                      onTap: () => _pushNumberEditor(
                        title: 'Font Size',
                        value: _worker.consoleFontSize.toInt(),
                        range: (10, 30),
                        footer: 'Recommended range: 10–30',
                        onChanged: (v) => setState(
                            () => _worker.consoleFontSize = v.toDouble()),
                      ),
                    ),
                  ],
                ),
                CupertinoListSection.insetGrouped(
                  header: sectionHeader('Network'),
                  children: [
                    CupertinoListTile.notched(
                      title: const Text('SSH Port'),
                      additionalInfo: Text('${_worker.sshPort}'),
                      trailing: const CupertinoListTileChevron(),
                      onTap: () => _pushNumberEditor(
                        title: 'SSH Port',
                        value: _worker.sshPort,
                        range: (1, 65535),
                        footer: 'Enter a value between 1 and 65535.',
                        onChanged: (v) => setState(() => _worker.sshPort = v),
                      ),
                    ),
                    CupertinoListTile.notched(
                      title: const Text('SSH Pass'),
                      additionalInfo:
                          Text('*' * _worker.sshPass.length.clamp(1, 20)),
                      trailing: const CupertinoListTileChevron(),
                      onTap: () => Navigator.push(
                        context,
                        CupertinoPageRoute(
                          builder: (_) => PasswordEditorScreen(
                            title: 'SSH Pass',
                            initialValue: _worker.sshPass,
                            footer: 'Default password is alpine.',
                            onChanged: (v) =>
                                setState(() => _worker.sshPass = v),
                          ),
                        ),
                      ),
                    ),
                  ],
                ),
                CupertinoListSection.insetGrouped(
                  header: sectionHeader('About'),
                  footer: Center(
                    child: Text(
                      'tvm by yuvalino',
                      style: TextStyle(
                        color: CupertinoColors.secondaryLabel.resolveFrom(context),
                        fontSize: 13,
                      ),
                    ),
                  ),
                  children: [
                    CupertinoListTile.notched(
                      title: const Text('Version'),
                      additionalInfo: Text(_packageInfo?.version ?? '...'),
                    ),
                    CupertinoListTile.notched(
                      title: const Text('Build'),
                      additionalInfo: Text(_packageInfo?.buildNumber ?? '...'),
                    ),
                  ],
                ),
              ],
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
