import 'package:flutter/foundation.dart';

import 'shell_session.dart';

/// The open tabs: page 0 is always the TVM tab, page i (i >= 1) is
/// [shells][i - 1]. Nothing here persists across launches.
class TabsController extends ChangeNotifier {
  final List<ShellSession> shells = [];

  int _index = 0;
  int get index => _index;
  int get pageCount => shells.length + 1;

  /// The shell in front, or null on the TVM tab.
  ShellSession? get activeShell => _index == 0 ? null : shells[_index - 1];

  void select(int index) {
    if (index == _index || index < 0 || index >= pageCount) return;
    _index = index;
    for (var i = 0; i < shells.length; i++) {
      shells[i].active = i == _index - 1;
    }
    activeShell?.unseen = false;
    notifyListeners();
  }

  /// Opens a new shell as the last tab and returns its page index.
  int add() {
    final shell = ShellSession(_nextTitle());
    shell.addListener(notifyListeners);
    shells.add(shell);
    shell.connect();
    final page = shells.length;
    select(page);
    return page;
  }

  void close(ShellSession shell) {
    final i = shells.indexOf(shell);
    if (i < 0) return;
    shells.removeAt(i);
    shell.removeListener(notifyListeners);
    shell.close();
    // The tab to the left takes the front if the closed one had it; tabs
    // to the right of the closed one move down a page.
    if (_index >= i + 1) _index--;
    for (var j = 0; j < shells.length; j++) {
      shells[j].active = j == _index - 1;
    }
    activeShell?.unseen = false;
    notifyListeners();
  }

  // "bash", then the lowest free "bash N".
  String _nextTitle() {
    final used = shells.map((s) => s.title).toSet();
    if (!used.contains('bash')) return 'bash';
    var n = 2;
    while (used.contains('bash $n')) {
      n++;
    }
    return 'bash $n';
  }
}
