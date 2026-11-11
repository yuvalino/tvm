"""UI tests for the Flutter app, driven over HTTP by frt
(https://github.com/yuvalino/frt): tabs, settings, the log, and the shell
tabs' terminals (typing in, reading out).

The app runs frt only when launched with FRT_PORT set, so these tests drive
the same build that ships. On macOS:

    python3 build.py -a macos-arm64 -t flutter
    pip install pytest aiohttp
    pytest tests/test_flutter.py

The app's SSH server is moved to a free port for the run (through the
Settings page, then a relaunch, as a user would) and put back afterwards, so
the tests never meet whatever else listens on the usual port. Like
test_integration.py's flutter backend, the run kills a tvm.app already
running.

With --device the app is launched on that device through devicectl with
FRT_HOST=0.0.0.0, and driven over Wi-Fi. (Not run yet.)
"""

import asyncio
import itertools
import os
import re
import socket
import subprocess
import tempfile
import time

import pytest

from frt_client import FrtClient, FrtError

FLUTTER_APP = "./flutter/build/macos/Build/Products/Release/tvm.app"
APP_EXE = f"{FLUTTER_APP}/Contents/MacOS/tvm"
IOS_BUNDLE_ID = "com.yuvalino.tvm"
READY = "Not backgrounding"  # dropbear's, once it listens

# Editor pages hold a focused CupertinoTextField whose cursor fades for as
# long as it is focused, so frames never settle there: don't wait for them.
EDITOR_SETTLE = 300


def _free_port() -> int:
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


class App:
    """The running app: frt's client calls, made synchronous, plus the log."""

    def __init__(self, proc: subprocess.Popen, log_path: str, host: str, port: int):
        self.proc = proc
        self.log_path = log_path
        self._loop = asyncio.new_event_loop()
        self._client = FrtClient(port_file=os.devnull, host=host, port=port)

    def __getattr__(self, name):
        attr = getattr(self._client, name)
        if not asyncio.iscoroutinefunction(attr):
            return attr
        return lambda *a, **kw: self._loop.run_until_complete(attr(*a, **kw))

    def log(self) -> str:
        with open(self.log_path, errors="replace") as f:
            return f.read()

    def text(self, key: str) -> str:
        matches = self.find("key", key)
        assert len(matches) == 1, f"{len(matches)} widgets keyed {key!r}"
        return matches[0].get("text", "")

    def exists(self, key: str) -> bool:
        return bool(self.find("key", key))

    def stop(self):
        try:
            self._loop.run_until_complete(self._client.close())
        finally:
            self._loop.close()
        if self.proc.poll() is None:
            self.proc.kill()
            self.proc.wait()


def _kill_mac_app():
    subprocess.run(["pkill", "-9", "-f", "tvm.app/Contents/MacOS/tvm"],
                   capture_output=True)
    deadline = time.monotonic() + 10
    while time.monotonic() < deadline:
        if subprocess.run(["pgrep", "-f", "tvm.app/Contents/MacOS/tvm"],
                          capture_output=True).returncode != 0:
            return
        time.sleep(0.1)


def _launch(device: str | None) -> App:
    frt_port = _free_port() if device is None else 4800
    log_path = tempfile.mktemp(prefix="tvm_flutter_ui_", suffix=".log")
    log = open(log_path, "w")
    if device is None:
        if not os.path.exists(APP_EXE):
            pytest.exit(f"{APP_EXE} not found: python3 build.py -a macos-arm64 -t flutter",
                        returncode=1)
        _kill_mac_app()
        proc = subprocess.Popen([APP_EXE], stdout=log, stderr=subprocess.STDOUT,
                                env={**os.environ, "FRT_PORT": str(frt_port)})
        host = "127.0.0.1"
    else:
        dev = device.removesuffix(".local")
        env = f'{{"FRT_PORT": "{frt_port}", "FRT_HOST": "0.0.0.0"}}'
        proc = subprocess.Popen(
            ["xcrun", "devicectl", "device", "process", "launch", "--device", dev,
             "--console", "--terminate-existing", "-e", env, IOS_BUNDLE_ID],
            stdout=log, stderr=subprocess.STDOUT)
        host = socket.gethostbyname(f"{dev}.local")
    app = App(proc, log_path, host, frt_port)
    try:
        app.connect(timeout=60)
        deadline = time.monotonic() + 30
        while READY not in app.log():
            if proc.poll() is not None or time.monotonic() > deadline:
                raise RuntimeError(f"the app's SSH server did not start:\n{app.log()}")
            time.sleep(0.1)
    except BaseException:
        app.stop()
        raise
    return app


def _open_settings(app: App):
    app.tap("key", "tab-tvm")
    app.tap("key", "section-settings")


def _back(app: App):
    app.tap("type", "CupertinoNavigationBarBackButton", settle=EDITOR_SETTLE)
    app.wait_gone("key", "editor-field")


def _set_number(app: App, row: str, value: str):
    """Open a number setting's editor, type value, and go back."""
    _open_settings(app)
    app.tap("key", f"setting-{row}")
    app.wait_for("key", "editor-field")
    app.enter_text("key", "editor-field", value, settle=EDITOR_SETTLE)
    _back(app)


@pytest.fixture(scope="module")
def app(device):
    """The app, its SSH server on a port of its own for the run."""
    first = _launch(device)
    try:
        original = first.text("setting-port-value")
        ssh_port = str(_free_port())
        _set_number(first, "port", ssh_port)
        assert first.text("setting-port-value") == ssh_port
    finally:
        first.stop()

    app = _launch(device)
    try:
        # The port applies on the next launch.
        assert f"ssh port: {ssh_port}" in app.log()
        app.ssh_port = int(ssh_port)
        yield app
    finally:
        try:
            _set_number(app, "port", original)
        finally:
            app.stop()


_tokens = itertools.count(1)


class Shell:
    """A shell tab: run commands in it and read what they print."""

    def __init__(self, app: App, id: int):
        self.app = app
        self.id = id
        self.key = f"terminal-{id}"

    def buffer(self, **kw) -> dict:
        return self.app.terminal_buffer(self.key, **kw)

    def cursor_line(self) -> int:
        return self.buffer()["cursor"]["line"]

    def send(self, text: str):
        self.app.terminal_input(text, self.key)

    def wait(self, text=None, *, pattern=None, start=0, timeout=10000) -> dict:
        return self.app.terminal_wait(text, self.key, pattern=pattern,
                                      start=start, timeout=timeout)

    def run(self, cmd: str, timeout: int = 10000) -> tuple[str, int]:
        """Run cmd; return its output (the rows after the command line, soft
        wraps undone, trailing newlines dropped) and exit status. A marker
        printed after it, with its status, says it is done: `_m <token>`
        (see setup()) prints `<token:status>` on a line of its own, which
        the typed line itself never shows.

        A line typed while the shell is still busy shows twice: echoed by
        the tty as it is typed, then again after the prompt when the shell
        reads it. The output starts after the last of them."""
        token = f"m{next(_tokens)}"
        at = self.cursor_line()
        self.send(f"{cmd}; _m {token}\r")
        result = self.wait(pattern=rf"^<{token}:(\d+)>$", start=at, timeout=timeout)
        text = self.buffer(start=at)["text"]
        marker = text.index(f"\n<{token}:")
        typed = text.rindex(f"_m {token}", 0, marker) + len(f"_m {token}")
        out = text[text.index("\n", typed) + 1:marker]
        return out.rstrip("\n"), int(result["groups"][0])

    def setup(self, start: int = 0):
        """Wait for the shell (its first output from buffer line start on),
        then make it predictable: a short prompt, no history file, and the
        marker function run() uses (it reports the status of the command
        before it)."""
        self.wait(pattern=r"\S", start=start, timeout=30000)
        self.send("PS1='$ '; unset HISTFILE; _m() { printf '\\n<%s:%d>\\n' \"$1\" $?; }\r")
        assert self.run("true") == ("", 0)


def _terminal_ids(app: App) -> set[int]:
    ids = set()
    for match in app.find("type", "TerminalView"):
        m = re.fullmatch(r"terminal-(\d+)", match.get("key", ""))
        if m:
            ids.add(int(m.group(1)))
    return ids


def open_tab(app: App) -> Shell:
    before = _terminal_ids(app)
    app.tap("key", "add-tab")
    deadline = time.monotonic() + 10
    while not (new := _terminal_ids(app) - before):
        assert time.monotonic() < deadline, "no new terminal after tapping +"
        time.sleep(0.1)
    (id,) = new
    shell = Shell(app, id)
    shell.setup()
    return shell


def close_tab(app: App, id: int):
    app.long_press("key", f"tab-{id}")
    app.tap("key", "menu-close")
    app.wait_gone("key", f"tab-{id}")


@pytest.fixture
def shell(app):
    """A fresh shell tab, closed afterwards (with any others the test left)."""
    before = _terminal_ids(app)
    sh = open_tab(app)
    yield sh
    if app.exists("keyboard-down"):
        app.tap("key", "keyboard-down")
    for id in sorted(_terminal_ids(app) - before):
        if app.exists(f"tab-{id}"):
            close_tab(app, id)


def _on_screen(app: App, key: str) -> bool:
    """Whether the page with this widget is the one in front (pages to
    either side are laid out off screen, others not at all)."""
    (match,) = app.find("key", key)
    return "bounds" in match and abs(match["bounds"]["x"]) < 1


def keyboard_up(app: App):
    """Raise the app's keyboard, unless it is up (a new tab raises it)."""
    if app.exists("keyboard-up"):
        app.tap("key", "keyboard-up")
    app.wait_for("key", "key-ctrl")


# --- The TVM tab ---


def test_network_line(app: App):
    line = app.text("network-line")
    assert re.match(r"\d{1,3}(\.\d{1,3}){3}( · \S+)?$", line), line


def test_connect_command(app: App):
    line = app.text("network-line")
    host = line.split(" · ")[-1]
    user = re.search(r"^username: (.*)$", app.log(), re.M).group(1)
    app.tap("key", "tab-tvm")
    app.tap("key", "section-log")
    # The command is the log's first entry, which a log following its end
    # has long scrolled away (and the lazy list no longer builds): scroll
    # back up to it, then follow again.
    try:
        for _ in range(50):
            if app.exists("connect-command"):
                break
            app.scroll("key", "log-list", dy=600)
        assert app.text("connect-command") == f"ssh -p {app.ssh_port} {user}@{host}"
    finally:
        app.tap("key", "log-follow")


def test_sections(app: App):
    """The log and the settings share the page; only one takes touches."""
    app.tap("key", "tab-tvm")
    app.tap("key", "section-log")
    with pytest.raises(FrtError) as exc_info:
        app.tap("key", "setting-font-size")
    assert exc_info.value.error == "not_hittable"
    app.tap("key", "section-settings")
    with pytest.raises(FrtError) as exc_info:
        app.tap("key", "log-follow")
    assert exc_info.value.error == "not_hittable"


# --- Settings ---


def test_font_size_resizes_terminal(app: App, shell: Shell):
    width = shell.buffer()["width"]
    try:
        _set_number(app, "font-size", "20")
        assert app.text("setting-font-size-value") == "20"
        app.tap("key", f"tab-{shell.id}")
        new = shell.buffer()["width"]
        assert new < width
        # The shell is told the new size (a moment later: resizes are
        # debounced).
        deadline = time.monotonic() + 5
        while True:
            out, rc = shell.run("stty size")
            if out == f"{shell.buffer()['height']} {new}" or time.monotonic() > deadline:
                break
            time.sleep(0.2)
        assert rc == 0 and out == f"{shell.buffer()['height']} {new}"
    finally:
        _set_number(app, "font-size", "13")
    assert app.text("setting-font-size-value") == "13"


def test_number_editor_clamps_and_filters(app: App):
    try:
        _set_number(app, "font-size", "99")
        assert app.text("setting-font-size-value") == "30"
        _set_number(app, "font-size", "x9y")
        # Digits only: "9".
        assert app.text("setting-font-size-value") == "9"
        _set_number(app, "font-size", "3")
        assert app.text("setting-font-size-value") == "8"
    finally:
        _set_number(app, "font-size", "13")


def test_password_row(app: App):
    _open_settings(app)
    assert re.fullmatch("•+", app.text("setting-password-value"))
    app.tap("key", "setting-password")
    app.wait_for("key", "password-reveal")
    _back(app)


# --- Tabs ---


def test_open_tab(app: App, shell: Shell):
    assert app.text(f"tab-{shell.id}").startswith("bash")
    assert _on_screen(app, shell.key)


def test_switch_tabs(app: App, shell: Shell):
    other = open_tab(app)
    assert _on_screen(app, other.key) and not _on_screen(app, shell.key)
    app.tap("key", f"tab-{shell.id}")
    assert _on_screen(app, shell.key) and not _on_screen(app, other.key)
    app.tap("key", "tab-tvm")
    assert not _on_screen(app, shell.key)


def test_unseen_output_dot(app: App, shell: Shell):
    other = open_tab(app)
    app.tap("key", f"tab-{shell.id}")
    shell.send("sleep 1; echo late\r")
    app.tap("key", f"tab-{other.id}")
    app.wait_for("key", f"tab-dot-{shell.id}", timeout=5000)
    assert not app.exists(f"tab-dot-{other.id}")
    app.tap("key", f"tab-{shell.id}")
    app.wait_gone("key", f"tab-dot-{shell.id}")


def test_rename_tab(app: App, shell: Shell):
    app.long_press("key", f"tab-{shell.id}")
    app.tap("key", "menu-rename")
    app.wait_for("key", "rename-field")
    app.enter_text("key", "rename-field", "work", settle=EDITOR_SETTLE)
    app.tap("key", "rename-ok", settle=EDITOR_SETTLE)
    app.wait_gone("key", "rename-field")
    assert app.text(f"tab-{shell.id}") == "work"


def test_close_tab_from_menu(app: App):
    sh = open_tab(app)
    close_tab(app, sh.id)
    assert not app.exists(sh.key)


def test_exit_ends_session(app: App, shell: Shell):
    shell.send("exit\r")
    app.wait_for("key", f"session-ended-{shell.id}", timeout=10000)
    assert "exit 0" in app.text(f"session-ended-{shell.id}")
    # Reconnecting starts a line below the old output: the new shell's
    # prompt comes after that.
    fresh = shell.cursor_line() + 1
    app.tap("key", "session-reconnect")
    app.wait_gone("key", f"session-ended-{shell.id}", timeout=10000)
    shell.setup(start=fresh)
    assert shell.run("echo back") == ("back", 0)


# --- Terminal ---


def test_echo(shell: Shell):
    assert shell.run("echo hi") == ("hi", 0)


def test_exit_status(shell: Shell):
    assert shell.run("false")[1] == 1
    assert shell.run("(exit 7)")[1] == 7


def test_multiline_output(shell: Shell):
    out, rc = shell.run("seq 1 5")
    assert (out, rc) == ("1\n2\n3\n4\n5", 0)


def test_long_line_wraps(shell: Shell):
    width = shell.buffer()["width"]
    out, rc = shell.run(f"printf 'z%.0s' $(seq 1 {width + 7}); echo")
    assert (out, rc) == ("z" * (width + 7), 0)


def test_wide_characters(shell: Shell):
    assert shell.run("echo 漢字x") == ("漢字x", 0)


def test_stty_size_matches_screen(shell: Shell):
    buf = shell.buffer()
    assert shell.run("stty size") == (f"{buf['height']} {buf['width']}", 0)


def test_scrollback(shell: Shell):
    out, rc = shell.run("seq 1 200")
    assert rc == 0 and out.split("\n") == [str(i) for i in range(1, 201)]
    assert shell.buffer()["scrollBack"] > 0


def test_ctrl_c(shell: Shell):
    at = shell.cursor_line()
    shell.send("sleep 30\r")
    shell.wait("sleep 30\n", start=at)
    time.sleep(0.3)
    shell.send("\x03")
    assert shell.run("echo $?", timeout=5000) == ("130", 0)


def test_alt_screen(shell: Shell):
    shell.send("less /tvm/etc/ssl/cacert.pem\r")
    deadline = time.monotonic() + 10
    while not shell.buffer()["altBuffer"]:
        assert time.monotonic() < deadline, "less did not switch screens"
        time.sleep(0.1)
    shell.send("q")
    deadline = time.monotonic() + 10
    while shell.buffer()["altBuffer"]:
        assert time.monotonic() < deadline, "less did not switch back"
        time.sleep(0.1)
    assert shell.run("echo after") == ("after", 0)


# --- The keyboard ---


def test_keyboard_typing(app: App, shell: Shell):
    """Typing through the app's own keyboard (KeyboardInput), a character at
    a time, as the platform keyboard sends it; Return arrives as "\\n"."""
    keyboard_up(app)
    at = shell.cursor_line()
    app.type("key", "keyboard-input", "echo typed\n")
    shell.wait(pattern=r"^typed$", start=at + 1)


def test_keyboard_backspace(app: App, shell: Shell):
    keyboard_up(app)
    at = shell.cursor_line()
    app.type("key", "keyboard-input", "echo abX")
    app.type("key", "keyboard-input", backspace=1)
    app.type("key", "keyboard-input", "c\n")
    shell.wait(pattern=r"^abc$", start=at + 1)


def test_key_bar_ctrl_latch(app: App, shell: Shell):
    keyboard_up(app)
    at = shell.cursor_line()
    shell.send("sleep 30\r")
    shell.wait("sleep 30\n", start=at)
    time.sleep(0.3)
    # ctrl latches; the next key typed takes it: Ctrl-C.
    app.tap("key", "key-ctrl")
    app.type("key", "keyboard-input", "c")
    assert shell.run("echo $?", timeout=5000) == ("130", 0)


def test_key_bar_keys(app: App, shell: Shell):
    keyboard_up(app)
    at = shell.cursor_line()
    app.type("key", "keyboard-input", "echo ab")
    # Left twice, then type there: "echo " + "X" + "ab".
    app.tap("key", "key-left")
    app.tap("key", "key-left")
    app.type("key", "keyboard-input", "X\n")
    shell.wait(pattern=r"^Xab$", start=at + 1)


def test_keyboard_down_hides_key_bar(app: App, shell: Shell):
    keyboard_up(app)
    app.tap("key", "keyboard-down")
    app.wait_gone("key", "key-ctrl")
    assert app.exists("keyboard-up")
