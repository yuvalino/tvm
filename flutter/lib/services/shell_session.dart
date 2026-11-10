import 'dart:async';
import 'dart:convert';

import 'package:dartssh2/dartssh2.dart';
import 'package:flutter/services.dart';
import 'package:flutter/widgets.dart';
// The same key tables the TerminalView uses for its own input, which the
// package does not export.
// ignore: implementation_imports
import 'package:xterm/src/ui/input_map.dart';
import 'package:xterm/xterm.dart';

import 'worker_controller.dart';

enum ShellState { connecting, connected, ended }

/// One shell tab: an SSH session to the app's own dropbear on loopback,
/// rendered by an xterm [Terminal]. The shell is exactly what an outside
/// ssh client gets. Tabs live only in memory.
class ShellSession extends ChangeNotifier {
  ShellSession(this._title) {
    terminal.onOutput = _send;
    terminal.onResize = _onResize;
  }

  final terminal = Terminal(maxLines: 10000);

  /// The terminal view's scroll position, brought to the end on input.
  final scroll = ScrollController();

  /// Focused while the keyboard is up and this tab is in front: the view
  /// draws a solid cursor then, and a hollow one otherwise. It sits under
  /// the app's one keyboard (KeyboardInput), which stays up for it. The home
  /// screen makes it focusable only while the keyboard is up, so a touch on
  /// the terminal (xterm focuses its view on touch down, long presses too)
  /// cannot raise the keyboard by itself.
  final viewFocus = FocusNode(
    debugLabel: 'shell view',
    skipTraversal: true,
    canRequestFocus: false,
  );

  String _title;
  String get title => _title;
  set title(String value) {
    _title = value;
    notifyListeners();
  }

  ShellState _state = ShellState.connecting;
  ShellState get state => _state;

  /// Why the session ended: the exit status, or the connection error.
  String? endReason;

  /// Output arrived while the tab was not in front.
  bool unseen = false;

  /// In front. Only then does the view follow the screen's size (see
  /// [ShellView]): a tab out of sight has no real size, and resizing it
  /// makes its shell redraw for nobody, lighting up [unseen].
  bool get active => _active;
  bool _active = false;
  set active(bool value) {
    if (value == _active) return;
    _active = value;
    notifyListeners();
  }

  /// One-shot modifiers latched from the key bar; the next key takes them.
  bool ctrl = false;
  bool alt = false;

  late final _filter = _PrivateSgrFilter(onLate: _write);
  SSHClient? _client;
  SSHSession? _session;
  final _subs = <StreamSubscription>[];
  bool _disposed = false;

  // Counts connect() calls. A reconnect starts a new one while the old may
  // still be waiting; the old one finds the count moved on and leaves the
  // new connection alone.
  int _generation = 0;

  Future<void> connect() async {
    final worker = WorkerController.instance;
    final generation = ++_generation;
    bool stale() => _disposed || generation != _generation;
    _setState(ShellState.connecting);
    endReason = null;
    try {
      final socket = await SSHSocket.connect(
        '127.0.0.1',
        worker.serverPort,
        timeout: const Duration(seconds: 5),
      );
      if (stale()) {
        socket.destroy();
        return;
      }
      // The host key is our own server's, over loopback: nothing to verify.
      final client = SSHClient(
        socket,
        username: worker.username,
        onPasswordRequest: () => worker.serverPass,
      );
      _client = client;
      final width = terminal.viewWidth, height = terminal.viewHeight;
      final session = await client.shell(
        pty: SSHPtyConfig(type: 'xterm-256color', width: width, height: height),
      );
      if (stale()) {
        client.close();
        return;
      }
      _session = session;
      // A resize while the shell was being opened found no session to
      // tell: catch up on it, unless one is still on its way.
      if (_resizeTimer?.isActive != true &&
          (terminal.viewWidth != width || terminal.viewHeight != height)) {
        session.resizeTerminal(terminal.viewWidth, terminal.viewHeight);
      }
      _setState(ShellState.connected);
      _subs.add(
        session.stdout
            .cast<List<int>>()
            .transform(const Utf8Decoder(allowMalformed: true))
            .listen(_onData),
      );
      _subs.add(
        session.stderr
            .cast<List<int>>()
            .transform(const Utf8Decoder(allowMalformed: true))
            .listen(_onData),
      );
      await session.done;
      if (stale()) return;
      final code = session.exitCode;
      final signal = session.exitSignal?.signalName;
      _end(
        signal != null
            ? 'killed by SIG$signal'
            : code != null
            ? 'exit $code'
            : 'connection closed',
      );
    } catch (e) {
      if (!stale()) _end(e.toString());
    }
  }

  /// Closes what is left of the old connection and starts a new one in the
  /// same terminal, under the old output.
  Future<void> reconnect() async {
    _teardown();
    terminal.write('\r\n');
    await connect();
  }

  void _onData(String data) {
    data = _filter.filter(data);
    if (data.isEmpty) return;
    _write(data);
    if (!active && !unseen) {
      unseen = true;
      notifyListeners();
    }
  }

  void _end(String reason) {
    if (_disposed || _state == ShellState.ended) return;
    endReason = reason;
    _teardown();
    _setState(ShellState.ended);
  }

  // Output from the shell. The terminal answers some of it (device
  // attributes, cursor position, size reports) through [_send] while it is
  // being written; those answers are not typed and leave the latch alone.
  bool _writing = false;
  void _write(String data) {
    _writing = true;
    try {
      terminal.write(data);
    } finally {
      _writing = false;
    }
  }

  void _setState(ShellState s) {
    if (_disposed) return;
    _state = s;
    notifyListeners();
  }

  // Everything typed reaches the shell through here: the software keyboard
  // (via the TerminalView), the key bar and pastes alike, so a latched
  // modifier applies to whichever key comes next. The terminal's own
  // answers come this way too, and go as they are.
  void _send(String data) {
    // The iOS keyboard's return key arrives as typed text, "\n", but a
    // terminal's Return is CR. Programs that read the tty raw (Claude Code)
    // take LF as Ctrl+J, a newline in the line being edited, not Enter.
    // Pastes go the same way, as a terminal sends them. A Ctrl+J from the
    // key bar is made below, after this, and stays LF.
    data = data.replaceAll('\n', '\r');
    if ((ctrl || alt) && !_writing) {
      if (data.length == 1) {
        if (ctrl) data = _ctrlChar(data);
        if (alt) data = '\x1b$data';
      }
      ctrl = alt = false;
      notifyListeners();
    }
    _session?.write(utf8.encode(data));
  }

  // The control character a terminal sends for Ctrl+<c>, as xterm maps it.
  static String _ctrlChar(String c) {
    final code = c.codeUnitAt(0);
    if (code >= 0x40 && code <= 0x7e) return String.fromCharCode(code & 0x1f);
    switch (c) {
      case ' ':
      case '2':
        return '\x00';
      case '3':
        return '\x1b';
      case '4':
        return '\x1c';
      case '5':
        return '\x1d';
      case '6':
        return '\x1e';
      case '7':
      case '/':
      case '-':
        return '\x1f';
      case '8':
      case '?':
        return '\x7f';
    }
    return c;
  }

  /// A key from the key bar, taking any latched modifiers with it.
  void sendKey(TerminalKey key) {
    final c = ctrl, a = alt;
    if (c || a) {
      ctrl = alt = false;
      notifyListeners();
    }
    terminal.keyInput(key, ctrl: c, alt: a);
    _scrollToEnd();
  }

  void sendText(String text) => terminal.textInput(text);

  /// Text from the software keyboard, handled as xterm's TerminalView
  /// handles its own: a single character a key stands for goes as that key
  /// (so the program's key modes apply), anything else as text.
  void typeText(String text) {
    final key = charToTerminalKey(text.trim());
    if (key == null || !terminal.keyInput(key)) terminal.textInput(text);
    _scrollToEnd();
  }

  /// A hardware key, as TerminalView handles one. Printable keys are left to
  /// the text input that follows them.
  KeyEventResult handleKeyEvent(KeyEvent event) {
    if (event is KeyUpEvent) return KeyEventResult.ignored;
    final key = keyToTerminalKey(event.logicalKey);
    if (key == null) return KeyEventResult.ignored;
    final keys = HardwareKeyboard.instance;
    final handled = terminal.keyInput(
      key,
      ctrl: keys.isControlPressed,
      alt: keys.isAltPressed,
      shift: keys.isShiftPressed,
    );
    if (handled) _scrollToEnd();
    return handled ? KeyEventResult.handled : KeyEventResult.ignored;
  }

  void _scrollToEnd() {
    if (scroll.hasClients) scroll.jumpTo(scroll.position.maxScrollExtent);
  }

  void toggleCtrl() {
    ctrl = !ctrl;
    notifyListeners();
  }

  void toggleAlt() {
    alt = !alt;
    notifyListeners();
  }

  // The view resizes the terminal on every frame of an animation (the
  // keyboard sliding takes a dozen); the program hears of it once, when the
  // size holds still. Each window change is a SIGWINCH, and a full-screen
  // program redraws all of itself on every one.
  Timer? _resizeTimer;
  void _onResize(int w, int h, int pw, int ph) {
    _resizeTimer?.cancel();
    _resizeTimer = Timer(const Duration(milliseconds: 100), () {
      _session?.resizeTerminal(w, h, pw, ph);
    });
  }

  void _teardown() {
    _resizeTimer?.cancel();
    _filter.flush();
    for (final s in _subs) {
      s.cancel();
    }
    _subs.clear();
    _session?.close();
    _session = null;
    _client?.close();
    _client = null;
  }

  /// Ends the session now and frees it after the frame that takes its view
  /// out of the tree, which still holds the focus node, scroll controller
  /// and a listener on this.
  void close() {
    _disposed = true;
    _teardown();
    WidgetsBinding.instance.addPostFrameCallback((_) => dispose());
  }

  @override
  void dispose() {
    _disposed = true;
    _teardown();
    viewFocus.dispose();
    scroll.dispose();
    super.dispose();
  }
}

/// Drops `CSI <prefix> ... m` sequences (prefix one of `< = > ?`) from the
/// output before the terminal parses it. xterm.dart's parser ignores the
/// prefix and runs them as SGR, but they are other things: Claude Code sets
/// xterm's modifyOtherKeys with `CSI > 4 ; 2 m` and resets it on exit with
/// `CSI > 4 m`, which xterm.dart took as "underline" (and "faint") and left
/// on for everything after. Nothing it would draw is lost: none of these
/// change what is on screen in a terminal that understands them.
class _PrivateSgrFilter {
  _PrivateSgrFilter({required this.onLate});

  /// Receives a held-back tail that turned out not to be one of these.
  final void Function(String) onLate;

  static final _sequence = RegExp(r'\x1b\[[<=>?][0-9;:]*m');
  // What could still grow into one by the next chunk.
  static final _partial = RegExp(r'\x1b(\[([<=>?][0-9;:]*)?)?$');

  String _held = '';
  Timer? _timer;

  String filter(String data) {
    _timer?.cancel();
    data = _held + data;
    _held = '';
    final tail = _partial.firstMatch(data);
    if (tail != null) {
      _held = data.substring(tail.start);
      data = data.substring(0, tail.start);
      // Output that stops on a bare ESC or CSI is still shown, a moment late.
      _timer = Timer(const Duration(milliseconds: 50), flush);
    }
    return data.replaceAll(_sequence, '');
  }

  void flush() {
    _timer?.cancel();
    if (_held.isEmpty) return;
    final held = _held;
    _held = '';
    onLate(held);
  }
}
