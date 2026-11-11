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
import signal
import socket
import subprocess
import tempfile
import time
import unicodedata

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

    def __init__(self, proc: subprocess.Popen, log, host: str, port: int):
        self.proc = proc
        self._log = log
        self.log_path = log.name
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
        if self.proc.poll() is None:
            self.proc.kill()
            self.proc.wait()
        try:
            self._loop.run_until_complete(self._client.close())
        finally:
            self._loop.close()
            self._log.close()
            os.unlink(self.log_path)


def _kill_mac_app():
    subprocess.run(["pkill", "-9", "-f", "tvm.app/Contents/MacOS/tvm"],
                   capture_output=True)
    deadline = time.monotonic() + 10
    while time.monotonic() < deadline:
        if subprocess.run(["pgrep", "-f", "tvm.app/Contents/MacOS/tvm"],
                          capture_output=True).returncode != 0:
            return
        time.sleep(0.1)
    raise RuntimeError("a running tvm.app did not quit")


def _launch(device: str | None, ssh: bool = True) -> App:
    """The app, driven by frt, once its first screen is up; with ssh, once
    its SSH server listens too."""
    frt_port = _free_port() if device is None else 4800
    if device is None:
        if not os.path.exists(APP_EXE):
            pytest.exit(f"{APP_EXE} not found: python3 build.py -a macos-arm64 -t flutter",
                        returncode=1)
        _kill_mac_app()
    log = tempfile.NamedTemporaryFile("w", prefix="tvm_flutter_ui_", suffix=".log",
                                      delete=False)
    if device is None:
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
    app = App(proc, log, host, frt_port)
    try:
        app.connect(timeout=60)
        # frt answers before the app's first frame
        app.wait_for("key", "tab-tvm", timeout=30000)
        deadline = time.monotonic() + 30
        while ssh and READY not in app.log():
            if proc.poll() is not None or time.monotonic() > deadline:
                raise RuntimeError("the app's SSH server did not start")
            time.sleep(0.1)
    except Exception as e:
        log = app.log()
        app.stop()
        raise RuntimeError(f"{e}\nThe app's log:\n{log}") from e
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
    # The editor page slides in: until it is in place, a tap on the field
    # lands on the page below (409 not_hittable). Entering text replaces
    # the field's text, so it is tried again until it lands.
    deadline = time.monotonic() + 5
    while True:
        try:
            app.enter_text("key", "editor-field", value, settle=EDITOR_SETTLE)
            break
        except FrtError as e:
            if e.error != "not_hittable" or time.monotonic() > deadline:
                raise
            time.sleep(0.1)
    _back(app)


FONT_SIZE = "13"  # the tests' font size; they put it back to this


def _settings(app: App, values: dict[str, str]):
    """Set each row, all of them even when one fails (the first failure is
    raised after)."""
    failure = None
    for row, value in values.items():
        try:
            _set_number(app, row, value)
            assert app.text(f"setting-{row}-value") == value
        except Exception as e:
            failure = failure or e
            try:    # out of an editor left open, for the next row
                if app.exists("editor-field"):
                    _back(app)
            except Exception:
                pass
    if failure:
        raise failure


@pytest.fixture(scope="module")
def app(device):
    """The app, its SSH server on a port of its own for the run and the
    terminal at FONT_SIZE. The user's settings are put back afterwards, by
    an app launched for that, whatever state the run left the last in.
    Settings need no SSH server: those launches don't wait for one (the
    user's port may be taken). SIGTERM and SIGHUP end the run through the
    same restore, as Ctrl-C does: Python runs no finally for them by
    itself, and a KeyboardInterrupt is the one exception that no except
    Exception (here, asyncio's, aiohttp's) takes for an ordinary error."""
    handlers = {sig: signal.getsignal(sig)
                for sig in (signal.SIGINT, signal.SIGTERM, signal.SIGHUP)}
    signal.signal(signal.SIGTERM, signal.default_int_handler)
    signal.signal(signal.SIGHUP, signal.default_int_handler)
    try:
        yield from _app(device)
    finally:
        for sig, handler in handlers.items():
            signal.signal(sig, handler)


def _app(device):
    first = _launch(device, ssh=False)
    try:
        original = {row: first.text(f"setting-{row}-value")
                    for row in ("port", "font-size")}
    except BaseException:
        first.stop()
        raise
    try:
        ssh_port = str(_free_port())
        try:
            _settings(first, {"port": ssh_port, "font-size": FONT_SIZE})
        finally:
            first.stop()
        app = _launch(device)
        try:
            # The port applies on the next launch.
            assert f"ssh port: {ssh_port}" in app.log()
            app.ssh_port = int(ssh_port)
            yield app
        finally:
            app.stop()
    finally:
        # not interrupted itself: a second signal would leave a row unset
        for sig in (signal.SIGINT, signal.SIGTERM, signal.SIGHUP):
            signal.signal(sig, signal.SIG_IGN)
        last = _launch(device, ssh=False)
        try:
            _settings(last, original)
        finally:
            last.stop()


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

    def wait(self, text=None, *, pattern=None, start=0, timeout=10000,
             screen=False) -> dict:
        """terminal_wait; on a timeout, the failure shows the screen."""
        try:
            return self.app.terminal_wait(text, self.key, pattern=pattern,
                                          start=start, timeout=timeout,
                                          screen=screen)
        except FrtError as e:
            if e.status != 408:
                raise
            shown = "\n".join(f"|{row}|" for row in self.buffer(screen=True)["lines"])
            raise AssertionError(f"{e}\nThe screen:\n{shown}") from None

    def wait_alt(self, on: bool, timeout: float = 10):
        """Wait for a full-screen program to switch to (on) or back from the
        alternate screen. Returns the buffer that showed it."""
        deadline = time.monotonic() + timeout
        while (buf := self.buffer())["altBuffer"] != on:
            assert time.monotonic() < deadline, f"altBuffer never became {on}"
            time.sleep(0.1)
        return buf

    def wait_size(self, timeout: float = 5) -> tuple[int, int]:
        """Wait until the shell is told the terminal's size (resizes reach it
        a moment late: they are debounced); return (rows, columns)."""
        deadline = time.monotonic() + timeout
        while True:
            buf = self.buffer()
            want = f"{buf['height']} {buf['width']}"
            out, rc = self.run("stty size")
            if (out, rc) == (want, 0):
                return buf["height"], buf["width"]
            assert time.monotonic() < deadline, f"stty size {out!r}, screen {want}"
            time.sleep(0.2)

    def scratch_dir(self):
        """cd into a new empty directory."""
        assert self.run('cd "$(mktemp -d)"')[1] == 0

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


def keyboard_down(app: App):
    app.tap("key", "keyboard-down")
    app.wait_gone("key", "key-ctrl")


def keyboard_up(app: App):
    """Raise the app's keyboard, unless it is up (a new tab raises it)."""
    if app.exists("keyboard-up"):
        app.tap("key", "keyboard-up")
    app.wait_for("key", "key-ctrl")


# --- The TVM tab ---


def _network_line(app: App, timeout: float = 10) -> str:
    """The network line, once the app's lookup at start has filled it in."""
    deadline = time.monotonic() + timeout
    while True:
        line = app.text("network-line")
        if line or time.monotonic() >= deadline:
            return line
        time.sleep(0.1)


def test_network_line(app: App):
    line = _network_line(app)
    # the Wi-Fi address and the host's name; either alone when it's all
    assert re.match(r"(\d{1,3}(\.\d{1,3}){3}( · \S+)?|\S+)$", line), line


def test_connect_command(app: App):
    line = _network_line(app)
    host = line.split(" · ")[-1]
    user = re.search(r"^username: (.*)$", app.log(), re.M).group(1)
    app.tap("key", "tab-tvm")
    app.tap("key", "section-log")
    # The command is the log's first entry, which a log following its end
    # has long scrolled away (and the lazy list no longer builds): scroll
    # back up to it, then follow again.
    scrolled = False
    try:
        for _ in range(50):
            if app.exists("connect-command"):
                break
            app.scroll("key", "log-list", dy=600)
            scrolled = True     # which stops following
        assert app.text("connect-command") == f"ssh -p {app.ssh_port} {user}@{host}"
    finally:
        if scrolled:
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
        assert shell.wait_size()[1] == new
    finally:
        _set_number(app, "font-size", FONT_SIZE)
    assert app.text("setting-font-size-value") == FONT_SIZE


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
        _set_number(app, "font-size", FONT_SIZE)


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
    assert (out, rc) == ("z" * (width + 7), 0)   # soft wraps undone
    buf = shell.buffer()
    i = len(buf["lines"]) - 1 - buf["lines"][::-1].index("z" * width)
    assert buf["lines"][i + 1] == "z" * 7 and buf["wrapped"][i + 1]


def test_wide_characters(shell: Shell):
    assert shell.run("echo 漢字x") == ("漢字x", 0)


@pytest.mark.xfail(strict=True, reason=(
    "xterm 4.0.0 Buffer.writeChar wraps only once the cursor is past the "
    "last column, so a wide character in the last column is cut in two: "
    "its first half ends the row, the empty second half starts the next"))
def test_wide_character_at_the_edge(shell: Shell):
    """A wide character with one column left starts the next row."""
    width = shell.buffer()["width"]
    at = shell.cursor_line()
    out, rc = shell.run(f"printf -v s '%*s' {width - 1} ''; echo \"${{s// /x}}漢字\"")
    assert rc == 0
    buf = shell.buffer(start=at)
    i = buf["lines"].index("x" * (width - 1))
    assert buf["lines"][i + 1] == "漢字" and buf["wrapped"][i + 1]
    assert out == "x" * (width - 1) + "漢字"


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
    shell.wait_alt(True)
    shell.send("q")
    shell.wait_alt(False)
    assert shell.run("echo after") == ("after", 0)


def test_leaving_alt_screen_shows_prompt(shell: Shell):
    # Scrollback first: the main screen has more lines than the alternate.
    assert shell.run("seq 1 200")[1] == 0
    # Into the alternate screen: it is in place at once, not sliding in.
    at = shell.cursor_line()
    shell.send("printf '\\e[?1049h'; sleep 0.5; printf '\\e[?1049l'; echo out\r")
    buf = shell.wait_alt(True)
    assert buf["view"] == {"first": 0, "last": buf["lineCount"] - 1}, buf["view"]
    # And out of it while the spring would still run, as a program that
    # quits at once does.
    shell.wait_alt(False)
    shell.wait(pattern=r"^out$", start=at)
    assert shell.run("echo after") == ("after", 0)
    # The view is back at the end, the prompt on its last row.
    buf = shell.buffer()
    assert buf["view"]["last"] == buf["cursor"]["line"] == buf["lineCount"] - 1, buf["view"]


def test_tab_completion(shell: Shell):
    shell.scratch_dir()
    assert shell.run("touch alpha_one alpha_two beta_file") == ("", 0)
    # One match: Tab completes it, and a space after.
    at = shell.cursor_line()
    shell.send("echo bet\t")
    shell.wait(pattern=r"^\$ echo beta_file $", start=at)
    assert shell.run("") == ("beta_file", 0)  # runs the completed line
    # A command name.
    at = shell.cursor_line()
    shell.send("ech\t")
    shell.wait(pattern=r"^\$ echo $", start=at)
    shell.send("\x15")  # Ctrl-U: the line is dropped
    # Two matches: Tab Tab lists them, then the line is back as it was.
    at = shell.cursor_line()
    shell.send("echo alpha_\t\t")
    # (readline pads the list with spaces)
    shell.wait(pattern=r"^alpha_one +alpha_two *\n\$ echo alpha_$", start=at)
    shell.send("t\t")
    shell.wait(pattern=r"^\$ echo alpha_two $", start=at)
    assert shell.run("") == ("alpha_two", 0)


def test_vim_cut_and_paste_lines(shell: Shell):
    shell.scratch_dir()
    assert shell.run(r"printf 'one\ntwo\nthree\nfour\nfive\n' > f") == ("", 0)
    shell.send("vim f\r")
    shell.wait(pattern=r"^one\ntwo\nthree\nfour\nfive\n~", screen=True)
    assert shell.buffer()["altBuffer"]
    # Line 2; Shift-V selects it, j takes line 3 too.
    shell.send("jVj")
    shell.wait(pattern=r"-- VISUAL LINE --", screen=True)
    shell.send("d")  # cut both
    shell.wait(pattern=r"^one\nfour\nfive\n~", screen=True)
    shell.send("p")  # paste them after the line the cursor is on, "four"
    shell.wait(pattern=r"^one\nfour\ntwo\nthree\nfive\n~", screen=True)
    shell.send(":wq\r")
    shell.wait_alt(False)
    assert shell.run("cat f") == ("one\nfour\ntwo\nthree\nfive", 0)


def _mixed_lines(width: int) -> list[str]:
    """Lines of every length that matters to wrapping: empty, short, one
    short of the width, exactly it, one over, two and three rows' worth,
    each in a letter of its own so a line moved or cut short shows; and one
    with wide characters, at its start (one cut by the edge is
    test_wide_character_at_the_edge's)."""
    lens = [0, 1, width - 1, width, width + 1, 2 * width, 2 * width + 1,
            3 * width + 5, 7, width // 2] * 5
    abc = "abcdefghijklmnopqrstuvwxyz"
    lines = [abc[i % 26] * n for i, n in enumerate(lens)]
    return lines + ["漢字" + "x" * (width - 2)]


def _print_lines(lines: list[str]) -> str:
    """A command that prints what _mixed_lines() gave."""
    *same, wide = lines
    lens = " ".join(str(len(line)) for line in same)
    n = len(wide) - 2
    return ("a=abcdefghijklmnopqrstuvwxyz; i=0; "
            f"for n in {lens}; do printf -v s '%*s' $n ''; "
            'echo "${s// /${a:i%26:1}}"; i=$((i+1)); done; '
            f"printf -v s '%*s' {n} ''; " 'echo "漢字${s// /x}"')


def _rows(lines: list[str], width: int) -> tuple[list[str], list[bool]]:
    """The rows lines take in a terminal this wide, and which of them
    continue the row before. A wide character that doesn't fit at the end
    of a row starts the next one (as in xterm and VTE; not yet in the xterm
    package, see test_wide_character_at_the_edge)."""
    rows, wrapped = [], []
    for line in lines:
        row, col, cont = "", 0, False
        for ch in line:
            w = 2 if unicodedata.east_asian_width(ch) in "WF" else 1
            if col + w > width:
                rows.append(row)
                wrapped.append(cont)
                row, col, cont = "", 0, True
            row += ch
            col += w
        rows.append(row)
        wrapped.append(cont)
    return rows, wrapped


def _find_rows(shell: Shell, lines: list[str]) -> dict:
    """That the buffer holds the lines, whole, once: their text, and the
    rows they take at the terminal's width, wrapped where they should be.
    Returns the buffer."""
    buf = shell.buffer()
    assert "\n" + "\n".join(lines) + "\n" in buf["text"]
    rows, wrapped = _rows(lines, buf["width"])
    have = buf["lines"]
    starts = [i for i in range(len(have) - len(rows) + 1)
              if have[i:i + len(rows)] == rows]
    assert len(starts) == 1, f"the rows are there {len(starts)} times"
    (first,) = starts
    assert buf["wrapped"][first:first + len(rows)] == wrapped
    return buf


def test_scrollback_while_scrolling(app: App, shell: Shell):
    """Scrolled up, down and up again, the view moves a screenful's worth of
    lines at a time over a buffer that holds the lines printed, and that
    scrolling changes nothing in."""
    lines = _mixed_lines(shell.buffer()["width"])
    assert shell.run(_print_lines(lines)) == ("\n".join(lines), 0)
    buf = _find_rows(shell, lines)
    view = buf["view"]
    assert view["last"] == buf["cursor"]["line"] == buf["lineCount"] - 1
    in_view = view["last"] - view["first"] + 1
    assert in_view == buf["height"]

    def check(moved, up: bool):
        b = shell.buffer()
        assert b["lines"] == buf["lines"] and b["wrapped"] == buf["wrapped"]
        v = b["view"]
        assert v["last"] - v["first"] + 1 in (in_view - 1, in_view)
        assert (v["first"] < moved["first"]) if up else (v["first"] > moved["first"])
        return v

    # Each drag is a fling too: let it run out.
    app.scroll("key", shell.key, dy=400, settle=3000)
    up = check(view, up=True)
    assert up["last"] < view["last"]
    app.scroll("key", shell.key, dy=-150, settle=3000)
    down = check(up, up=False)
    app.scroll("key", shell.key, dy=400, settle=3000)
    check(down, up=True)
    app.scroll("key", shell.key, dy=-5000, settle=3000)
    assert shell.buffer()["view"] == view
    assert shell.run("echo after") == ("after", 0)


def test_resize_keeps_scrollback(app: App, shell: Shell):
    """The keyboard going down and up resizes the terminal (rows), the font
    size too (columns: lines are wrapped anew). The shell is told, and the
    lines printed before are all there, wrapped at the new width."""
    keyboard_up(app)
    width = shell.buffer()["width"]
    lines = _mixed_lines(width)
    assert shell.run(_print_lines(lines)) == ("\n".join(lines), 0)
    _find_rows(shell, lines)
    rows, _ = shell.wait_size()

    keyboard_down(app)
    taller, cols = shell.wait_size()
    assert taller > rows and cols == width
    _find_rows(shell, lines)

    keyboard_up(app)
    assert shell.wait_size() == (rows, width)
    buf = _find_rows(shell, lines)
    assert buf["view"]["last"] == buf["cursor"]["line"]

    try:
        _set_number(app, "font-size", "17")
        app.tap("key", f"tab-{shell.id}")
        keyboard_up(app)
        _, narrow = shell.wait_size()
        assert narrow < width
        buf = _find_rows(shell, lines)
        assert buf["view"]["last"] == buf["cursor"]["line"]
    finally:
        _set_number(app, "font-size", FONT_SIZE)
    app.tap("key", f"tab-{shell.id}")
    keyboard_up(app)
    assert shell.wait_size() == (rows, width)
    _find_rows(shell, lines)


def test_vim_redraws_on_resize(app: App, shell: Shell):
    """vim fills the screen it is given, again when the keyboard resizes
    it."""
    keyboard_up(app)
    shell.scratch_dir()
    assert shell.run("seq 1 200 > f") == ("", 0)

    def shows(rows: int):
        # The file from its first line down to the last row but one; the
        # last is vim's own, not the file's next line (as it would be on a
        # shrink vim wasn't told of).
        nums = "\\n".join(str(i) for i in range(1, rows))
        shell.wait(pattern=f"^{nums}\\n(?!{rows}$)", screen=True)

    rows = shell.buffer()["height"]
    shell.send("vim f\r")
    shell.wait_alt(True)
    shows(rows)
    keyboard_down(app)
    taller = shell.buffer()["height"]
    assert taller > rows
    shows(taller)
    keyboard_up(app)
    assert shell.buffer()["height"] == rows
    shows(rows)
    shell.send(":q\r")
    shell.wait_alt(False)
    assert shell.wait_size()[0] == rows


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


def test_key_bar_tab(app: App, shell: Shell):
    keyboard_up(app)
    shell.scratch_dir()
    assert shell.run("touch beta_file") == ("", 0)
    at = shell.cursor_line()
    app.type("key", "keyboard-input", "echo bet")
    app.tap("key", "key-tab")
    shell.wait(pattern=r"^\$ echo beta_file $", start=at)
    app.type("key", "keyboard-input", "\n")
    shell.wait(pattern=r"^beta_file$", start=at + 1)


def test_key_bar_esc_in_vim(app: App, shell: Shell):
    keyboard_up(app)
    shell.scratch_dir()
    shell.send("vim g\r")
    shell.wait_alt(True)
    shell.wait(pattern=r"^~ *$", screen=True)  # vim pads rows with spaces
    app.type("key", "keyboard-input", "ihello")
    shell.wait(pattern=r"-- INSERT --", screen=True)
    app.tap("key", "key-esc")
    # Out of insert mode: the command is a command, not text.
    app.type("key", "keyboard-input", ":wq\n")
    shell.wait_alt(False)
    assert shell.run("cat g") == ("hello", 0)


def test_keyboard_down_hides_key_bar(app: App, shell: Shell):
    keyboard_up(app)
    keyboard_down(app)
    assert app.exists("keyboard-up")
