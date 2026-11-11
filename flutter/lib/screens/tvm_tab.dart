import 'package:flutter/cupertino.dart';

import '../services/worker_controller.dart';
import '../theme/tvm_colors.dart';
import '../widgets/log_view.dart';
import '../widgets/settings_view.dart';

/// The first tab, always open: the app's identity and server status, then
/// the log or the settings.
class TvmTab extends StatefulWidget {
  const TvmTab({super.key});

  @override
  State<TvmTab> createState() => _TvmTabState();
}

class _TvmTabState extends State<TvmTab> with AutomaticKeepAliveClientMixin {
  int _section = 0;

  @override
  bool get wantKeepAlive => true;

  @override
  Widget build(BuildContext context) {
    super.build(context);
    final w = WorkerController.instance;
    return ColoredBox(
      color: TvmColors.bg,
      child: SafeArea(
        bottom: false,
        child: Column(
          children: [
            Padding(
              padding: const EdgeInsets.fromLTRB(16, 6, 16, 12),
              child: Row(
                children: [
                  ClipRRect(
                    borderRadius: BorderRadius.circular(11),
                    child: Image.asset('assets/logo.png',
                        width: 46, height: 46, filterQuality: FilterQuality.none),
                  ),
                  const SizedBox(width: 12),
                  Expanded(
                    child: Column(
                      crossAxisAlignment: CrossAxisAlignment.start,
                      children: [
                        const _Prompt(),
                        const SizedBox(height: 2),
                        ValueListenableBuilder(
                          valueListenable: w.network,
                          // Shrinks to fit rather than cut the name short.
                          builder: (context, net, _) => FittedBox(
                            fit: BoxFit.scaleDown,
                            alignment: Alignment.centerLeft,
                            child: Text(
                              [?net.address, ?net.name].join(' · '),
                              key: const ValueKey('network-line'),
                              maxLines: 1,
                              style: const TextStyle(
                                  fontFamily: TvmColors.mono,
                                  fontSize: 12,
                                  color: TvmColors.dim),
                            ),
                          ),
                        ),
                      ],
                    ),
                  ),
                ],
              ),
            ),
            Padding(
              padding: const EdgeInsets.fromLTRB(16, 0, 16, 12),
              child: SizedBox(
                width: double.infinity,
                child: CupertinoSlidingSegmentedControl<int>(
                  groupValue: _section,
                  backgroundColor: TvmColors.card,
                  thumbColor: TvmColors.highlight,
                  onValueChanged: (v) => setState(() => _section = v ?? 0),
                  children: {
                    for (final (i, label) in [(0, 'log'), (1, 'settings')])
                      i: Padding(
                        key: ValueKey('section-$label'),
                        padding: const EdgeInsets.symmetric(vertical: 6),
                        child: Text(label,
                            style: TextStyle(
                                fontFamily: TvmColors.mono,
                                fontSize: 13,
                                fontWeight: FontWeight.w500,
                                color: _section == i
                                    ? TvmColors.bright
                                    : TvmColors.dim)),
                      ),
                  },
                ),
              ),
            ),
            Expanded(
              child: IndexedStack(
                index: _section,
                children: const [LogView(), SettingsView()],
              ),
            ),
          ],
        ),
      ),
    );
  }
}

/// The title, written the way tvm.yuvalino.com writes its prompt: `tvm:~\$`
/// in the site's colours (.ps green, .pd dim, .pw blue).
class _Prompt extends StatelessWidget {
  const _Prompt();

  @override
  Widget build(BuildContext context) {
    const bold = FontWeight.w700;
    return const Text.rich(
      TextSpan(
        children: [
          TextSpan(
            text: 'tvm',
            style: TextStyle(color: TvmColors.green, fontWeight: bold),
          ),
          TextSpan(text: ':', style: TextStyle(color: TvmColors.dim)),
          TextSpan(
            text: '~',
            style: TextStyle(color: TvmColors.blue, fontWeight: bold),
          ),
          TextSpan(text: r'$', style: TextStyle(color: TvmColors.dim)),
        ],
      ),
      style: TextStyle(fontFamily: TvmColors.mono, fontSize: 20),
    );
  }
}
