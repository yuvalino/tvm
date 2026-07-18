import 'package:flutter/cupertino.dart';
import 'package:flutter/material.dart';

import '../services/worker_controller.dart';

class ConsoleScreen extends StatefulWidget {
  const ConsoleScreen({super.key});

  @override
  State<ConsoleScreen> createState() => _ConsoleScreenState();
}

class _ConsoleScreenState extends State<ConsoleScreen> {
  final _scrollController = ScrollController();
  final _worker = WorkerController.instance;

  bool _isAtBottom = true;

  @override
  void initState() {
    super.initState();
    _scrollController.addListener(_onScroll);
    _worker.logs.addListener(_onLogsChanged);
    _worker.start();
  }

  @override
  void dispose() {
    _scrollController.removeListener(_onScroll);
    _worker.logs.removeListener(_onLogsChanged);
    _scrollController.dispose();
    super.dispose();
  }

  void _onScroll() {
    final atBottom = _scrollController.position.pixels >=
        _scrollController.position.maxScrollExtent - 20;
    if (atBottom != _isAtBottom) setState(() => _isAtBottom = atBottom);
  }

  void _onLogsChanged() {
    if (_worker.autoScroll.value) {
      WidgetsBinding.instance.addPostFrameCallback((_) {
        if (_scrollController.hasClients) {
          _scrollController.animateTo(
            _scrollController.position.maxScrollExtent,
            duration: const Duration(milliseconds: 100),
            curve: Curves.easeOut,
          );
        }
      });
    }
    setState(() {});
  }

  void _scrollToBottom() {
    _scrollController.animateTo(
      _scrollController.position.maxScrollExtent,
      duration: const Duration(milliseconds: 200),
      curve: Curves.easeOut,
    );
  }

  @override
  Widget build(BuildContext context) {
    return CupertinoPageScaffold(
      backgroundColor: Colors.black,
      child: Column(
        children: [
          Expanded(
            child: ValueListenableBuilder<double>(
              valueListenable: _worker.fontSizeNotifier,
              builder: (context, fontSize, _) {
                final logs = _worker.logs.value;
                return CustomScrollView(
                  controller: _scrollController,
                  slivers: [
                    const CupertinoSliverNavigationBar(
                      largeTitle: Text('tvm'),
                    ),
                    SliverPadding(
                      padding: const EdgeInsets.all(12),
                      sliver: SliverList.builder(
                        itemCount: logs.length,
                        itemBuilder: (context, index) {
                          return Padding(
                            padding: const EdgeInsets.symmetric(vertical: 2),
                            child: Text(
                              logs[index],
                              style: TextStyle(
                                fontFamily: 'monospace',
                                fontSize: fontSize,
                                color: Colors.green,
                                height: 1.3,
                                decoration: TextDecoration.none,
                              ),
                            ),
                          );
                        },
                      ),
                    ),
                  ],
                );
              },
            ),
          ),
          const Divider(height: 1, color: Colors.white24),
          _buildFooter(),
        ],
      ),
    );
  }

  Widget _buildFooter() {
    return ValueListenableBuilder<bool>(
      valueListenable: _worker.autoScroll,
      builder: (context, autoScroll, _) {
        return Container(
          color: const Color(0xFF1C1C1E),
          padding: const EdgeInsets.symmetric(horizontal: 12, vertical: 8),
          child: SafeArea(
            top: false,
            child: Row(
              children: [
                _FooterButton(
                  icon: autoScroll
                      ? Icons.lock_rounded
                      : Icons.lock_open_rounded,
                  filled: autoScroll,
                  onPressed: () =>
                      _worker.autoScroll.value = !_worker.autoScroll.value,
                ),
                const SizedBox(width: 8),
                _FooterButton(
                  icon: Icons.arrow_downward_rounded,
                  filled: false,
                  enabled: !_isAtBottom,
                  onPressed: _scrollToBottom,
                ),
                const Spacer(),
                _FooterButton(
                  icon: Icons.settings_rounded,
                  filled: false,
                  onPressed: () async {
                    await Navigator.pushNamed(context, '/settings');
                    setState(() {});
                  },
                ),
              ],
            ),
          ),
        );
      },
    );
  }
}

class _FooterButton extends StatelessWidget {
  const _FooterButton({
    required this.icon,
    required this.filled,
    required this.onPressed,
    this.enabled = true,
  });

  final IconData icon;
  final bool filled;
  final bool enabled;
  final VoidCallback onPressed;

  @override
  Widget build(BuildContext context) {
    final color = Theme.of(context).colorScheme.primary;
    return GestureDetector(
      onTap: enabled ? onPressed : null,
      child: AnimatedContainer(
        duration: const Duration(milliseconds: 200),
        width: 44,
        height: 44,
        decoration: BoxDecoration(
          color: filled ? color : Colors.transparent,
          borderRadius: BorderRadius.circular(8),
        ),
        child: Icon(
          icon,
          color: filled
              ? Colors.white
              : enabled
                  ? color
                  : color.withValues(alpha: 0.3),
          size: 22,
        ),
      ),
    );
  }
}
