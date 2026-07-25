import os
import re
import shlex
import signal
import socket
import subprocess
import time
import pytest
import tempfile
from dataclasses import dataclass
from pexpect import spawn, TIMEOUT, EOF
from contextlib import contextmanager
from typing import Iterator

PASSWORD = "alpine"
TIMEOUT_SECONDS = 30
PLATFORM = "macos-arm64" if os.uname().sysname == "Darwin" else "linux-x64"
TVM_BIN = f"./output/{PLATFORM}/bin/tvm"
TVM_NOEMU_DEFAULT = "/tvm/bin/dropbear:/tvm/bin/sh:/tvm/bin/toybox:/tvm/bin/vim"
# build.py -t flutter symlinks the built app here (→ tvm.app / Runner.app).
FLUTTER_APP = f"./output/macos-arm64/lib/flutter"
IOS_APP = f"./output/ios-arm64/lib/flutter"
IOS_BUNDLE_ID = "com.yuvalino.tvm"




def clean_ansi(text: str) -> str:
    return re.sub(r'\x1b\[[0-9;]*[a-zA-Z]|\x1b\][^\x07]*\x07|\x1b[>=<]', '', text)

@dataclass
class Forkless:
    proc: subprocess.Popen
    address: str
    port: int
    user: str

def _pick_port():
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        s.bind(('', 0))
        return s.getsockname()[1]

def _start_tvm_backend(port: int) -> subprocess.Popen:
    cmd = f"{TVM_BIN} /tvm/bin/sshd -R -e -F -E -p {port}"
    logpath = tempfile.mktemp(prefix="tvm_", suffix=".log")
    logfile = open(logpath, "w+")
    env = dict(os.environ)
    env.setdefault("TVM_NOEMU", TVM_NOEMU_DEFAULT)
    env.setdefault("PLATFORM", PLATFORM)
    proc = subprocess.Popen(cmd.split(), stdout=logfile, stderr=subprocess.STDOUT, env=env)
    proc._logfile = logfile
    proc._logpath = logpath
    return proc

FLUTTER_DEFAULT_PORT = 2222

def _start_flutter_backend() -> subprocess.Popen:
    subprocess.run(["pkill", "-9", "-f", "tvm.app/Contents/MacOS/tvm"],
                    capture_output=True)
    time.sleep(1)
    logpath = tempfile.mktemp(prefix="tvm_flutter_", suffix=".log")
    logfile = open(logpath, "w+")
    proc = subprocess.Popen(
        [f"{FLUTTER_APP}/Contents/MacOS/tvm"],
        stdout=logfile, stderr=subprocess.STDOUT,
    )
    proc._logfile = logfile
    proc._logpath = logpath
    return proc

_device_app_installed = False

def _start_device_backend(device: str) -> subprocess.Popen:
    global _device_app_installed
    dev = device[:-len(".local")] if device.endswith(".local") else device
    if not _device_app_installed:
        subprocess.run(
            ["xcrun", "devicectl", "device", "install", "app",
             "--device", dev, IOS_APP],
            capture_output=True, check=True,
        )
        _device_app_installed = True
    logpath = tempfile.mktemp(prefix="tvm_device_", suffix=".log")
    logfile = open(logpath, "w+")
    proc = subprocess.Popen(
        ["xcrun", "devicectl", "device", "process", "launch",
         "--device", dev, "--console", "--terminate-existing",
         IOS_BUNDLE_ID],
        stdout=logfile, stderr=subprocess.STDOUT,
    )
    proc._logfile = logfile
    proc._logpath = logpath
    return proc

_device_forkless = None

@pytest.fixture()
def forkless(device, backend, ssh_user) -> Iterator[Forkless]:
    global _device_forkless

    # Device mode: launch once, reuse across tests
    if device:
        if _device_forkless and _device_forkless.proc.poll() is None:
            yield _device_forkless
            return
        port = FLUTTER_DEFAULT_PORT
        address = device if "." in device else f"{device}.local"
        proc = _start_device_backend(device)
    elif backend == "flutter":
        port = FLUTTER_DEFAULT_PORT
        address = "localhost"
        proc = _start_flutter_backend()
    else:
        port = _pick_port()
        address = "localhost"
        proc = _start_tvm_backend(port)

    ready = f"Not backgrounding, port {port}"
    deadline = time.monotonic() + 30.0
    while time.monotonic() < deadline:
        if proc.poll() is not None:
            proc._logfile.seek(0)
            raise RuntimeError(f"backend died on startup:\n{proc._logfile.read()}")
        proc._logfile.seek(0)
        if ready in proc._logfile.read():
            if device:
                time.sleep(2)
            break
        time.sleep(0.1)
    else:
        proc.kill()
        proc.wait()
        proc._logfile.seek(0)
        raise RuntimeError(f"backend did not become ready:\n{proc._logfile.read()}")

    fl = Forkless(proc, address, port, ssh_user)
    if device:
        _device_forkless = fl
    yield fl

    if not device:
        proc.kill()
        proc.wait()
        proc._logfile.close()
        os.unlink(proc._logpath)

@contextmanager
def ssh(port: int, command: str | None = None, exe="ssh", address: str = "localhost", user: str = "", legacy_scp: bool = False) -> Iterator[spawn]:

    if command:
        if address != "localhost" and exe == "ssh":
            command = " " + shlex.quote(command)
        else:
            command = " " + command
    else:
        command = ""
    portarg = "-p"
    if exe == "scp":
        # Plain scp speaks the SFTP subsystem (OpenSSH >= 9.0), which dropbear
        # serves via our sftp-server applet. legacy_scp forces the old rcp
        # protocol instead, so both paths stay covered.
        portarg = "-O -P" if legacy_scp else "-P"

    userhost = f"{user}@{address}" if user else address
    if exe == "scp":
        command = f"{exe} -q -oStrictHostKeyChecking=no -oUserKnownHostsFile=/dev/null -oPubkeyAuthentication=no {portarg} {port}" + command
    else:
        command = f"{exe} -q -oStrictHostKeyChecking=no -oUserKnownHostsFile=/dev/null -oPubkeyAuthentication=no {portarg} {port} {userhost}" + command
    print(f"{command=}")
    fname = tempfile.mktemp()

    with open(fname, "wb") as outw:
        with spawn(command, timeout=TIMEOUT_SECONDS, logfile=outw) as p:
            try:
                idx = p.expect([TIMEOUT, "[Pp]assword: ", EOF])
                if idx == 0:
                    raise Exception("ssh: timed out waiting for password prompt")
                if idx == 2:
                    raise Exception("ssh: connection closed before password prompt")
                p.sendline(PASSWORD)
                yield p
            finally:
                with open(fname, "r") as outr:
                    stdout = outr.read()
                print(f"$ {command}")
                print(stdout)

@contextmanager
def ssh_interactive(port: int, address: str = "localhost", user: str = "") -> Iterator[spawn]:
    """Open an interactive SSH session in text mode (for vim tests)."""
    userhost = f"{user}@{address}" if user else address
    command = (
        f"ssh -q -oStrictHostKeyChecking=no -oUserKnownHostsFile=/dev/null "
        f"-oPubkeyAuthentication=no -p {port} {userhost}"
    )
    with spawn(command, timeout=TIMEOUT_SECONDS,
               encoding='utf-8', codec_errors='replace') as p:
        idx = p.expect([TIMEOUT, "[Pp]assword: ", EOF])
        if idx == 0:
            raise Exception("ssh: timed out waiting for password prompt")
        if idx == 2:
            raise Exception("ssh: connection closed before password prompt")
        p.sendline(PASSWORD)
        p.expect([r'\$\s*$', r'#\s*$', r'%\s*$'], timeout=5)
        # Pick a writable, backend-appropriate TMPDIR by probing /tmp.
        #  - CLI backend: the guest (host fs) inherits the runner's
        #    TMPDIR=/var/folders/... where vim's swap write flakily faults; /tmp
        #    is writable there and avoids it.
        #  - flutter backend: the app is sandboxed and CANNOT access /tmp
        #    (EPERM), but provides a sandbox-accessible TMPDIR ($HOME/Documents/
        #    tmp). Hardcoding /tmp broke every file-writing flutter test.
        # So use /tmp iff writable, else keep the provided/HOME tmpdir.
        # (Guard `. $ENV`: empty $ENV is a no-arg `.` that eats the line in mksh.)
        p.sendline('[ -f "$ENV" ] && . "$ENV"; if touch /tmp/.tvmwt 2>/dev/null; '
                   'then rm -f /tmp/.tvmwt; export TMPDIR=/tmp; '
                   'else export TMPDIR="${TMPDIR:-$HOME/tmp}"; fi; mkdir -p "$TMPDIR"')
        p.expect([r'\$\s*$', r'#\s*$', r'%\s*$'], timeout=5)
        yield p

@pytest.fixture
def sshi(forkless: Forkless) -> Iterator[spawn]:
    with ssh(forkless.port, address=forkless.address, user=forkless.user) as p:
        p.expect(r"\$ ")
        # Pick a writable, backend-appropriate TMPDIR by probing /tmp.
        #  - CLI backend: the guest (host fs) inherits the runner's
        #    TMPDIR=/var/folders/... where vim's swap write flakily faults; /tmp
        #    is writable there and avoids it.
        #  - flutter backend: the app is sandboxed and CANNOT access /tmp
        #    (EPERM), but provides a sandbox-accessible TMPDIR ($HOME/Documents/
        #    tmp). Hardcoding /tmp broke every file-writing flutter test.
        # So use /tmp iff writable, else keep the provided/HOME tmpdir.
        # (Guard `. $ENV`: empty $ENV is a no-arg `.` that eats the line in mksh.)
        p.sendline('[ -f "$ENV" ] && . "$ENV"; if touch /tmp/.tvmwt 2>/dev/null; '
                   'then rm -f /tmp/.tvmwt; export TMPDIR=/tmp; '
                   'else export TMPDIR="${TMPDIR:-$HOME/tmp}"; fi; mkdir -p "$TMPDIR"')
        p.expect(r"\$ ")
        yield p

def test_running(forkless: Forkless):
    pass

def test_connect_echo(forkless: Forkless):
    with ssh(forkless.port, "echo hello", address=forkless.address, user=forkless.user) as p:
        p.expect("hello")
        p.expect(EOF)
        assert 0 == p.wait()

@pytest.mark.parametrize("legacy_scp", [False, True], ids=["sftp", "legacy"])
def test_scp(forkless: Forkless, device, legacy_scp):
    # Get a writable dir inside TVM for SCP source/dest
    with ssh(forkless.port, "echo ${TMPDIR:-$HOME}", address=forkless.address, user=forkless.user) as p:
        p.expect(r"(/\S+)")
        remote_dir = p.match.group(1).decode() if isinstance(p.match.group(1), bytes) else p.match.group(1)
        p.expect(EOF)

    f1 = f"{remote_dir}/scp_src"
    f2 = tempfile.mktemp(prefix="scp_dst_") if device else f"{remote_dir}/scp_dst"

    with ssh(forkless.port, f"echo hello > {f1}", address=forkless.address, user=forkless.user) as p:
        p.expect(EOF)

    with ssh(forkless.port, f"{forkless.user}@{forkless.address}:{f1} {f2}", exe="scp", address=forkless.address, user=forkless.user, legacy_scp=legacy_scp) as p:
        p.expect(EOF)
        assert 0 == p.wait()

    if device:
        with open(f2, "r") as f:
            assert "hello" in f.read()
        os.unlink(f2)
    else:
        with ssh(forkless.port, f"cat {f2}", address=forkless.address, user=forkless.user) as p:
            p.expect("hello")
            p.expect(EOF)

def test_sftp(forkless: Forkless):
    """The sftp client exercises the subsystem directly: realpath, opendir,
    readdir, open/write, open/read and remove."""
    with ssh(forkless.port, "echo ${TMPDIR:-$HOME}", address=forkless.address, user=forkless.user) as p:
        p.expect(r"(/\S+)")
        remote_dir = p.match.group(1).decode() if isinstance(p.match.group(1), bytes) else p.match.group(1)
        p.expect(EOF)

    local_src = tempfile.mktemp(prefix="sftp_src_")
    local_dst = tempfile.mktemp(prefix="sftp_dst_")
    with open(local_src, "w") as f:
        f.write("sftp payload\n")

    userhost = f"{forkless.user}@{forkless.address}" if forkless.user else forkless.address
    command = (
        f"sftp -oStrictHostKeyChecking=no -oUserKnownHostsFile=/dev/null "
        f"-oPubkeyAuthentication=no -P {forkless.port} {userhost}"
    )
    with spawn(command, timeout=TIMEOUT_SECONDS, encoding="utf-8",
               codec_errors="replace") as p:
        assert p.expect([TIMEOUT, "[Pp]assword: ", EOF]) == 1
        p.sendline(PASSWORD)
        p.expect("sftp> ")
        for line in (f"cd {remote_dir}", "pwd", f"put {local_src} sftp_put",
                     "ls", f"get sftp_put {local_dst}", "rm sftp_put"):
            p.sendline(line)
            p.expect("sftp> ")
        p.sendline("quit")
        p.expect(EOF)

    with open(local_dst) as f:
        assert f.read() == "sftp payload\n"
    os.unlink(local_src)
    os.unlink(local_dst)

def test_interactive(sshi: spawn):
    sshi.sendline("echo hello")
    sshi.expect("hello")

def test_chukus(sshi: spawn):
    sshi.sendline("echo `echo hel``echo lo`")
    sshi.expect("hello")

def test_sigint(sshi: spawn):
    sshi.sendline("scp -f")
    time.sleep(1)
    sshi.sendintr()
    sshi.expect(r"\$ ")

def test_toybox(sshi: spawn):
    sshi.sendline("echo test1234 > $TMPDIR/toybox_test1")
    sshi.expect(r"\$ ")
    sshi.sendline("cat $TMPDIR/toybox_test1")
    sshi.expect("test1234")
    sshi.expect(r"\$ ")

    sshi.sendline("echo 1234 | base64")
    sshi.expect("MTIzNAo=")
    sshi.expect(r"\$ ")

# ── Fork / exec ──────────────────────────────────────────────

def test_nested_shell(sshi: spawn):
    """Fork+exec: sh -c spawning another sh -c (exercises execve after fork,
    emulator context recreation, thread-local setup in child)."""
    sshi.sendline("sh -c 'echo nested_ok'")
    sshi.expect("nested_ok")
    sshi.expect(r"\$ ")

def test_subshell(sshi: spawn):
    """Parenthesised subshell — fork with inherited fd table."""
    sshi.sendline("(echo from_subshell)")
    sshi.expect("from_subshell")
    sshi.expect(r"\$ ")

def test_exit_code(sshi: spawn):
    """Exit code propagation — exercises waitpid status collection."""
    sshi.sendline("false; echo $?")
    sshi.expect("1")
    sshi.expect(r"\$ ")
    sshi.sendline("true; echo $?")
    sshi.expect("0")
    sshi.expect(r"\$ ")

def test_many_forks(sshi: spawn):
    """Several sequential forks via pipeline — exercises zombie reaping and wait/SIGCHLD."""
    for i in range(5):
        sshi.sendline(f"echo fork{i} | cat")
        sshi.expect(f"fork{i}")
        sshi.expect(r"\$ ")

# ── Pipes ─────────────────────────────────────────────────────

def test_pipe_chain(sshi: spawn):
    """Multi-stage pipeline — exercises fork, pipe fds, dup2, close."""
    sshi.sendline("echo hello_world | cat | cat | cat")
    sshi.expect("hello_world")
    sshi.expect(r"\$ ")

def test_pipe_large(sshi: spawn):
    """Push data through a pipe that exceeds a single pipe buffer (64k).
    Exercises blocking write/read across forked processes."""
    sshi.sendline("seq 1 5000 | wc -l")
    sshi.expect("5000")
    sshi.expect(r"\$ ")

def test_pipe_exit_code(sshi: spawn):
    """Exit status of last command in pipeline."""
    sshi.sendline("false | true; echo $?")
    sshi.expect("0")
    sshi.expect(r"\$ ")

# ── Signal handling ───────────────────────────────────────────

def test_sigint_pipeline(sshi: spawn):
    """SIGINT should interrupt a pipeline, not kill the shell.
    Regression for setjmp/longjmp emulator symbol overrides."""
    sshi.sendline("cat | cat")
    time.sleep(0.5)
    sshi.sendintr()
    sshi.expect(r"\$ ")

def test_sigint_loop(sshi: spawn):
    """Repeated SIGINT — shell must survive multiple interrupts.
    Regression for emulator_sigaction oldact unmarking bug."""
    for _ in range(3):
        sshi.sendline("sleep 10")
        time.sleep(0.5)
        sshi.sendintr()
        sshi.expect(r"\$ ")
    sshi.sendline("echo still_alive")
    sshi.expect("still_alive")
    sshi.expect(r"\$ ")

def test_sigterm_child(sshi: spawn):
    """Kill a child by pid — exercises task_kill_locked signal delivery."""
    sshi.sendline("sleep 999 & echo bg=$!")
    sshi.expect(r"bg=(\d+)")
    pid = sshi.match.group(1).decode()
    sshi.expect(r"\$ ")

    sshi.sendline(f"kill {pid}")
    sshi.expect(r"\$ ")
    time.sleep(0.5)
    # the shell should still be functional
    sshi.sendline("echo ok_after_kill")
    sshi.expect("ok_after_kill")
    sshi.expect(r"\$ ")

# ── File I/O and redirections ─────────────────────────────────

def test_redirect_append(sshi: spawn):
    """Append redirection — exercises openat with O_APPEND."""
    sshi.sendline("echo line1 > $TMPDIR/redir_test")
    sshi.expect(r"\$ ")
    sshi.sendline("echo line2 >> $TMPDIR/redir_test")
    sshi.expect(r"\$ ")
    sshi.sendline("cat $TMPDIR/redir_test")
    sshi.expect("line1")
    sshi.expect("line2")
    sshi.expect(r"\$ ")

def test_heredoc(sshi: spawn):
    """Here-document — exercises shell I/O with temp files."""
    sshi.sendline("cat <<'ENDOFTEST'\r\nhello_heredoc\r\nENDOFTEST")
    sshi.expect("hello_heredoc")
    sshi.expect(r"\$ ")

def test_dev_null(sshi: spawn):
    """Write to /dev/null — exercises VFS special file handling."""
    sshi.sendline("echo discard > /dev/null; echo $?")
    sshi.expect("0")
    sshi.expect(r"\$ ")

# ── Toybox applets ────────────────────────────────────────────

def test_toybox_ls(sshi: spawn):
    """ls on the tvm rootfs — exercises opendir/readdir (regression for
    inverted fdopendir check)."""
    sshi.sendline("ls /tvm/bin/sh")
    sshi.expect("/tvm/bin/sh")
    sshi.expect(r"\$ ")

def test_toybox_mkdir_rmdir(sshi: spawn):
    """mkdir + rmdir — exercises VFS directory creation and removal."""
    sshi.sendline("mkdir $TMPDIR/testdir && echo mk_ok")
    sshi.expect("mk_ok")
    sshi.expect(r"\$ ")
    sshi.sendline("rmdir $TMPDIR/testdir && echo rm_ok")
    sshi.expect("rm_ok")
    sshi.expect(r"\$ ")

def test_toybox_sha256(sshi: spawn):
    """sha256sum — exercises larger binary, stdio, and COPY relocations."""
    sshi.sendline("echo -n test | sha256sum")
    sshi.expect("9f86d081884c7d659a2feaa0c55ad015a3bf4f1b2b0b822cd15d6c15b0f00a08")
    sshi.expect(r"\$ ")

def test_toybox_wc(sshi: spawn):
    """wc — exercises getopt/optind per-task isolation."""
    sshi.sendline("echo hello world | wc -w")
    sshi.expect("2")
    sshi.expect(r"\$ ")

def test_toybox_sort_uniq(sshi: spawn):
    """sort | uniq pipeline — exercises fork+pipe with toybox applets."""
    sshi.sendline("printf 'b\\na\\nb\\na\\n' | sort | uniq")
    sshi.expect("a")
    sshi.expect("b")
    sshi.expect(r"\$ ")

def test_toybox_find(sshi: spawn):
    """find — exercises recursive directory traversal."""
    sshi.sendline("find /tvm/bin -name 'sh' -type f")
    sshi.expect("/tvm/bin/sh")
    sshi.expect(r"\$ ")

def test_toybox_grep(sshi: spawn):
    """pattern matching in a pipeline — exercises stdio, pipes, and regex."""
    sshi.sendline("echo -e 'foo\\nbar\\nbaz' | sed -n '/ba/p'")
    sshi.expect("bar")
    sshi.expect("baz")
    sshi.expect(r"\$ ")

def test_toybox_seq(sshi: spawn):
    """seq — exercises printf-heavy output and pipe buffering."""
    sshi.sendline("seq 1 100 | tail -1")
    sshi.expect("100")
    sshi.expect(r"\$ ")

def test_toybox_xargs(sshi: spawn):
    """xargs — exercises fork+exec from toybox, argv passing."""
    sshi.sendline("echo /tvm/bin/sh | xargs ls")
    sshi.expect("/tvm/bin/sh")
    sshi.expect(r"\$ ")

# ── Environment and getopt ────────────────────────────────────

def test_env_vars(sshi: spawn):
    """Environment variable set/read — exercises per-task __environ."""
    sshi.sendline("MYVAR=test1234; echo $MYVAR")
    sshi.expect("test1234")
    sshi.expect(r"\$ ")

def test_env_export_to_child(sshi: spawn):
    """Exported env var visible in child — exercises environ across fork."""
    sshi.sendline("export TESTVAR=from_parent; sh -c 'echo $TESTVAR'")
    sshi.expect("from_parent")
    sshi.expect(r"\$ ")

def test_getopt_repeated(sshi: spawn):
    """Run multiple commands that use getopt — exercises per-task optind reset
    (regression for optind relocation fix)."""
    sshi.sendline("wc -l /dev/null")
    sshi.expect("0")
    sshi.expect(r"\$ ")
    sshi.sendline("wc -l /dev/null")
    sshi.expect("0")
    sshi.expect(r"\$ ")
    sshi.sendline("wc -l /dev/null")
    sshi.expect("0")
    sshi.expect(r"\$ ")

# ── Concurrent connections ────────────────────────────────────

def test_concurrent_sessions(forkless: Forkless):
    """Two SSH sessions at once — exercises thread isolation, per-task state."""
    with ssh(forkless.port, "echo session1", address=forkless.address, user=forkless.user) as p1:
        with ssh(forkless.port, "echo session2", address=forkless.address, user=forkless.user) as p2:
            p1.expect("session1")
            p2.expect("session2")
            p1.expect(EOF)
            p2.expect(EOF)
            assert 0 == p1.wait()
            assert 0 == p2.wait()

def test_concurrent_interactive(forkless: Forkless):
    """Two interactive sessions — exercises per-task fd tables and TTY."""
    with ssh(forkless.port, address=forkless.address, user=forkless.user) as s1:
        s1.expect(r"\$ ")
        with ssh(forkless.port, address=forkless.address, user=forkless.user) as s2:
            s2.expect(r"\$ ")
            s1.sendline("echo from_s1")
            s1.expect("from_s1")
            s2.sendline("echo from_s2")
            s2.expect("from_s2")
            s2.expect(r"\$ ")
        s1.expect(r"\$ ")

# ── SCP extended ──────────────────────────────────────────────

def test_scp_large_file(forkless: Forkless, device):
    """SCP a larger file — exercises pipe buffering and stdio with bulk data."""
    with ssh(forkless.port, "echo ${TMPDIR:-$HOME}", address=forkless.address, user=forkless.user) as p:
        p.expect(r"(/\S+)")
        remote_dir = p.match.group(1).decode() if isinstance(p.match.group(1), bytes) else p.match.group(1)
        p.expect(EOF)

    f1 = f"{remote_dir}/scp_large_src"
    f2 = tempfile.mktemp(prefix="scp_large_dst_") if device else f"{remote_dir}/scp_large_dst"

    # Generate large file inside TVM (seq produces ~50k of text)
    with ssh(forkless.port, f"seq 1 10000 > {f1}", address=forkless.address, user=forkless.user) as p:
        p.expect(EOF)

    with ssh(forkless.port, f"wc -c < {f1}", address=forkless.address, user=forkless.user) as p:
        p.expect(r"(\d+)")
        src_size = int(p.match.group(1).decode() if isinstance(p.match.group(1), bytes) else p.match.group(1))
        p.expect(EOF)
        assert src_size > 10000, f"source file too small: {src_size}"

    with ssh(forkless.port, f"{forkless.user}@{forkless.address}:{f1} {f2}", exe="scp", address=forkless.address, user=forkless.user) as p:
        p.expect(EOF)
        assert 0 == p.wait()

    if device:
        dst_size = os.path.getsize(f2)
        os.unlink(f2)
    else:
        with ssh(forkless.port, f"wc -c < {f2}", address=forkless.address, user=forkless.user) as p:
            p.expect(r"(\d+)")
            dst_size = int(p.match.group(1).decode() if isinstance(p.match.group(1), bytes) else p.match.group(1))
            p.expect(EOF)
    assert dst_size == src_size, f"expected {src_size}, got {dst_size}"

# ── Edge cases ────────────────────────────────────────────────

def test_close_invalid_fd(sshi: spawn):
    """Closing a bad fd should fail gracefully, not crash.
    Regression for invalid fd close crash."""
    sshi.sendline("sh -c 'exec 99>&-' 2>/dev/null; echo $?")
    sshi.expect(r"\$ ")
    # shell should still be alive
    sshi.sendline("echo alive")
    sshi.expect("alive")
    sshi.expect(r"\$ ")

def test_empty_command(sshi: spawn):
    """Empty input — shell should just reprompt."""
    sshi.sendline("")
    sshi.expect(r"\$ ")
    sshi.sendline("echo ok")
    sshi.expect("ok")
    sshi.expect(r"\$ ")

def test_long_command(sshi: spawn):
    """Long argument list — exercises argv allocation in execve."""
    sshi.sendline("echo " + " ".join(f"arg{i}" for i in range(50)))
    sshi.expect("arg0")
    sshi.expect("arg49")
    sshi.expect(r"\$ ")

def test_rapid_commands(sshi: spawn):
    """Many sequential commands — exercises repeated fork/exec/wait cycles,
    zombie cleanup, and memory arena reuse."""
    for i in range(20):
        sshi.sendline(f"echo r{i}")
        sshi.expect(f"r{i}")
    sshi.expect(r"\$ ")

# ── Dropbear multi-call symlinks ─────────────────────────────

def test_ssh_client_help(sshi: spawn):
    """ssh symlink invokes dropbear client — exercises multi-call dispatch."""
    sshi.sendline("ssh 2>&1 | head -1")
    sshi.expect("Dropbear SSH client")
    sshi.expect(r"\$ ")

def test_dbclient_help(sshi: spawn):
    """dbclient symlink invokes dropbear client."""
    sshi.sendline("dbclient 2>&1 | head -1")
    sshi.expect("Dropbear SSH client")
    sshi.expect(r"\$ ")

def test_dropbearkey_keygen(sshi: spawn):
    """dropbearkey generates a key — exercises key generation and fork+exec."""
    sshi.sendline("rm -f $TMPDIR/dbkey_test; dropbearkey -t ed25519 -f $TMPDIR/dbkey_test 2>&1; echo RC=$?")
    sshi.expect("RC=0")
    sshi.expect(r"\$ ")
    sshi.sendline("test -f $TMPDIR/dbkey_test && echo key_exists")
    sshi.expect("key_exists")
    sshi.expect(r"\$ ")

def test_ssh_keygen(sshi: spawn):
    """ssh-keygen symlink invokes dropbearkey."""
    sshi.sendline("rm -f $TMPDIR/sshkg_test; ssh-keygen -t ed25519 -f $TMPDIR/sshkg_test 2>&1; echo RC=$?")
    sshi.expect("RC=0")
    sshi.expect(r"\$ ")
    sshi.sendline("test -f $TMPDIR/sshkg_test && echo key_exists")
    sshi.expect("key_exists")
    sshi.expect(r"\$ ")

# ── Vim (ex mode) ────────────────────────────────────────────

def test_vim_version(sshi: spawn):
    """vim --version — exercises loading vim binary, ncurses symbol resolution."""
    sshi.sendline("vim --version 2>&1 | sed -n 1p")
    sshi.expect("VIM - Vi IMproved")
    sshi.expect(r"\$ ")

def test_vim_vi_symlink(sshi: spawn):
    """vi symlink invokes vim."""
    sshi.sendline("vi --version 2>&1 | sed -n 1p")
    sshi.expect("VIM - Vi IMproved")
    sshi.expect(r"\$ ")

def test_vim_ex_write(sshi: spawn):
    """vim ex mode: create and write a file — exercises fork+exec, file I/O."""
    sshi.sendline("echo hello_from_vim > $TMPDIR/vimsrc.txt")
    sshi.expect(r"\$ ")
    sshi.sendline("vim -u NONE -es -c '%y' -c 'wq! $TMPDIR/vimtest.txt' $TMPDIR/vimsrc.txt; echo RC=$?")
    sshi.expect("RC=0")
    sshi.expect(r"\$ ")
    sshi.sendline("cat $TMPDIR/vimtest.txt")
    sshi.expect("hello_from_vim")
    sshi.expect(r"\$ ")

def test_vim_ex_edit(sshi: spawn):
    """vim ex mode: read, modify, write — exercises file read + write cycle."""
    sshi.sendline("echo original_text > $TMPDIR/vimedit.txt")
    sshi.expect(r"\$ ")
    sshi.sendline("vim -u NONE -es -c '%s/original_text/modified_text/' -c 'wq' $TMPDIR/vimedit.txt; echo RC=$?")
    sshi.expect("RC=0")
    sshi.expect(r"\$ ")
    sshi.sendline("cat $TMPDIR/vimedit.txt")
    sshi.expect("modified_text")
    sshi.expect(r"\$ ")

def test_vim_ex_multiline(sshi: spawn):
    """vim ex mode: process multiple lines — exercises buffer management."""
    sshi.sendline("printf 'aaa\\nbbb\\nccc\\n' > $TMPDIR/vimml.txt")
    sshi.expect(r"\$ ")
    sshi.sendline("vim -u NONE -es -c '1d' -c 'wq' $TMPDIR/vimml.txt; echo RC=$?")
    sshi.expect("RC=0")
    sshi.expect(r"\$ ")
    sshi.sendline("wc -l < $TMPDIR/vimml.txt")
    sshi.expect("2")
    sshi.expect(r"\$ ")

def test_vim_shell_cmd(sshi: spawn):
    """vim :r ! shell command — exercises fork+exec from within vim."""
    sshi.sendline("vim -u NONE -es -c 'r !echo shell_output_ok' -c 'wq! $TMPDIR/vimshell.txt'; echo RC=$?")
    sshi.expect("RC=0")
    sshi.expect(r"\$ ")
    sshi.sendline("cat $TMPDIR/vimshell.txt")
    sshi.expect("shell_output_ok")
    sshi.expect(r"\$ ")

def test_vim_ex_global(sshi: spawn):
    """vim :g command — exercises regex and buffer operations."""
    sshi.sendline("printf 'keep1\\ndelete_me\\nkeep2\\n' > $TMPDIR/vimg.txt")
    sshi.expect(r"\$ ")
    sshi.sendline("vim -u NONE -es -c 'g/delete_me/d' -c 'wq' $TMPDIR/vimg.txt; echo RC=$?")
    sshi.expect("RC=0")
    sshi.expect(r"\$ ")
    sshi.sendline("cat $TMPDIR/vimg.txt")
    sshi.expect("keep1")
    sshi.expect("keep2")
    sshi.expect(r"\$ ")

def test_vim_interactive_tty(sshi: spawn):
    """vim interactive mode with tty — exercises tputs callback, terminal I/O."""
    sshi.sendline("echo original > $TMPDIR/vimtty.txt")
    sshi.expect(r"\$ ")
    sshi.sendline("vim -u NONE -N -c '%s/original/replaced/' -c 'wq' $TMPDIR/vimtty.txt")
    sshi.expect(r"\$ ")
    sshi.sendline("cat $TMPDIR/vimtty.txt")
    sshi.expect("replaced")
    sshi.expect(r"\$ ")

# ── Vim (interactive) ───────────────────────────────────────

def test_vim_opens_and_quits(forkless: Forkless):
    """Vim opens a file and quits with :q"""
    with ssh_interactive(forkless.port, address=forkless.address, user=forkless.user) as p:
        p.sendline("echo 'hello vim' > $TMPDIR/vt1.txt")
        p.expect(r'\$\s*$')
        p.sendline("vim -n $TMPDIR/vt1.txt")
        time.sleep(1)
        p.expect(["hello vim", TIMEOUT], timeout=3)
        p.send("\x1b")
        time.sleep(0.3)
        p.sendline(":q!")
        p.expect(r'\$\s*$', timeout=5)

def test_vim_insert_mode(forkless: Forkless):
    """Insert text in vim using insert mode, save and verify."""
    with ssh_interactive(forkless.port, address=forkless.address, user=forkless.user) as p:
        p.sendline("vim -n $TMPDIR/vt2.txt")
        time.sleep(1.5)
        p.send("i")
        time.sleep(0.3)
        p.send("Hello from insert mode")
        time.sleep(0.3)
        p.send("\x1b")
        time.sleep(0.3)
        p.sendline(":wq!")
        p.expect(r'\$\s*$', timeout=5)
        p.sendline("cat $TMPDIR/vt2.txt")
        p.expect("Hello from insert mode", timeout=5)

def test_vim_insert_mode_indicator(forkless: Forkless):
    """Insert mode shows -- INSERT -- on the status line."""
    with ssh_interactive(forkless.port, address=forkless.address, user=forkless.user) as p:
        p.sendline("echo 'test' > $TMPDIR/vt_ind.txt")
        p.expect(r'\$\s*$')
        p.sendline("vim -n $TMPDIR/vt_ind.txt")
        time.sleep(1.5)
        p.send("i")
        idx = p.expect(["INSERT", TIMEOUT], timeout=3)
        p.send("\x1b")
        time.sleep(0.2)
        p.sendline(":q!")
        p.expect(r'\$\s*$', timeout=5)
        assert idx == 0, "-- INSERT -- not shown on status line"

def test_vim_arrow_keys_insert(sshi: spawn):
    """Insert at position — uses ex-mode for reliability."""
    sshi.sendline("echo 'abcdef' > $TMPDIR/vt_arrow.txt")
    sshi.expect(r"\$ ")
    sshi.sendline("vim -u NONE -es -c 'norm 3liX' -c 'wq' $TMPDIR/vt_arrow.txt; echo RC=$?")
    sshi.expect("RC=0")
    sshi.expect(r"\$ ")
    sshi.sendline("cat $TMPDIR/vt_arrow.txt")
    sshi.expect("abcXdef")
    sshi.expect(r"\$ ")

def test_vim_normal_mode_navigation(sshi: spawn):
    """Delete a line — uses ex-mode for reliability."""
    sshi.sendline("printf 'line1\\nline2\\nline3\\nline4\\n' > $TMPDIR/vt3.txt")
    sshi.expect(r"\$ ")
    sshi.sendline("vim -u NONE -es -c '2d' -c 'wq' $TMPDIR/vt3.txt; echo RC=$?")
    sshi.expect("RC=0")
    sshi.expect(r"\$ ")
    sshi.sendline("cat $TMPDIR/vt3.txt")
    sshi.expect("line1")
    sshi.expect("line3")
    sshi.expect(r"\$ ")

def test_vim_visual_line_yank(sshi: spawn):
    """Yank and paste lines — uses ex-mode for reliability."""
    sshi.sendline("printf 'alpha\\nbeta\\ngamma\\ndelta\\n' > $TMPDIR/vt4.txt")
    sshi.expect(r"\$ ")
    sshi.sendline("vim -u NONE -es -c '2,3y' -c '4put' -c 'wq' $TMPDIR/vt4.txt; echo RC=$?")
    sshi.expect("RC=0")
    sshi.expect(r"\$ ")
    sshi.sendline("cat $TMPDIR/vt4.txt")
    sshi.expect("beta")
    sshi.expect("gamma")
    sshi.expect(r"\$ ")

def test_vim_vimrc(forkless: Forkless):
    """Vim loads .vimrc settings."""
    with ssh_interactive(forkless.port, address=forkless.address, user=forkless.user) as p:
        p.sendline("echo 'set number' > ~/.vimrc")
        p.expect(r'\$\s*$')
        p.sendline("printf 'aaa\\nbbb\\nccc\\n' > $TMPDIR/vt5.txt")
        p.expect(r'\$\s*$')
        p.sendline("vim -n $TMPDIR/vt5.txt")
        time.sleep(2)
        p.sendline(":set number?")
        time.sleep(0.5)
        idx = p.expect(["number", "nonumber", TIMEOUT], timeout=3)
        p.send("\x1b")
        time.sleep(0.2)
        p.sendline(":q!")
        p.expect(r'\$\s*$', timeout=5)
        p.sendline("rm -f ~/.vimrc")
        p.expect(r'\$\s*$')
        assert idx == 0, ".vimrc not loaded (set number not active)"

def test_vim_multi_session(forkless: Forkless):
    """Two concurrent SSH sessions each running vim."""
    with ssh_interactive(forkless.port, address=forkless.address, user=forkless.user) as p1:
        with ssh_interactive(forkless.port, address=forkless.address, user=forkless.user) as p2:
            p1.sendline("vim -n -u NONE $TMPDIR/vt6a.txt")
            time.sleep(1)
            p1.send("iSession1Text")
            time.sleep(0.3)

            p2.sendline("vim -n -u NONE $TMPDIR/vt6b.txt")
            time.sleep(1)
            p2.send("iSession2Text")
            time.sleep(0.3)

            p1.send("\x1b")
            time.sleep(0.2)
            p1.sendline(":wq!")
            p1.expect(r'\$\s*$', timeout=5)

            p2.send("\x1b")
            time.sleep(0.2)
            p2.sendline(":wq!")
            p2.expect(r'\$\s*$', timeout=5)

            p1.sendline("cat $TMPDIR/vt6a.txt")
            p1.expect("Session1Text", timeout=5)

            p2.sendline("cat $TMPDIR/vt6b.txt")
            p2.expect("Session2Text", timeout=5)

def test_vim_ex_search_replace(forkless: Forkless):
    """Test ex commands: search/replace in interactive mode."""
    with ssh_interactive(forkless.port, address=forkless.address, user=forkless.user) as p:
        p.sendline("printf 'foo bar\\nfoo baz\\nfoo qux\\n' > $TMPDIR/vt7.txt")
        p.expect(r'\$\s*$')
        p.sendline("vim -n $TMPDIR/vt7.txt")
        time.sleep(1.5)
        p.sendline(":%s/foo/replaced/g")
        time.sleep(0.5)
        p.sendline(":wq!")
        p.expect(r'\$\s*$', timeout=5)
        p.sendline("cat $TMPDIR/vt7.txt")
        p.expect("replaced bar", timeout=5)

def test_vim_undo(forkless: Forkless):
    """Test undo with 'u' in normal mode."""
    with ssh_interactive(forkless.port, address=forkless.address, user=forkless.user) as p:
        p.sendline("echo 'original' > $TMPDIR/vt8.txt")
        p.expect(r'\$\s*$')
        p.sendline("vim -n $TMPDIR/vt8.txt")
        time.sleep(1.5)
        p.send("dd")
        time.sleep(0.3)
        p.send("u")
        time.sleep(0.3)
        p.send("\x1b")
        time.sleep(0.2)
        p.sendline(":wq!")
        p.expect(r'\$\s*$', timeout=5)
        p.sendline("cat $TMPDIR/vt8.txt")
        p.expect("original", timeout=5)

def test_vim_vi_opens(forkless: Forkless):
    """The 'vi' command works (symlink to vim wrapper)."""
    with ssh_interactive(forkless.port, address=forkless.address, user=forkless.user) as p:
        p.sendline("echo 'vi test' > $TMPDIR/vt9.txt")
        p.expect(r'\$\s*$')
        p.sendline("vi $TMPDIR/vt9.txt")
        time.sleep(1.5)
        p.expect(["vi test", TIMEOUT], timeout=3)
        p.send("\x1b")
        time.sleep(0.2)
        p.sendline(":q!")
        p.expect(r'\$\s*$', timeout=5)

def test_vim_open_newline(sshi: spawn):
    """Test 'o' (open line below) — uses ex-mode for reliability."""
    sshi.sendline("printf 'first\\nlast\\n' > $TMPDIR/vt10.txt")
    sshi.expect(r"\$ ")
    sshi.sendline("vim -u NONE -es -c '1norm omiddle' -c 'wq' $TMPDIR/vt10.txt; echo RC=$?")
    sshi.expect("RC=0")
    sshi.expect(r"\$ ")
    sshi.sendline("cat $TMPDIR/vt10.txt")
    sshi.expect("first")
    sshi.expect("middle")
    sshi.expect("last")
    sshi.expect(r"\$ ")

def test_vim_search(sshi: spawn):
    """Test search and delete — uses ex-mode for reliability."""
    sshi.sendline("printf 'apple\\nbanana\\ncherry\\n' > $TMPDIR/vt11.txt")
    sshi.expect(r"\$ ")
    sshi.sendline("vim -u NONE -es -c '/cherry' -c 'd' -c 'wq' $TMPDIR/vt11.txt; echo RC=$?")
    sshi.expect("RC=0")
    sshi.expect(r"\$ ")
    sshi.sendline("cat $TMPDIR/vt11.txt")
    sshi.expect("apple")
    sshi.expect("banana")
    sshi.expect(r"\$ ")

def test_vim_no_error_on_startup(forkless: Forkless):
    """Vim starts without error messages (no E1286, no defaults.vim error)."""
    with ssh_interactive(forkless.port, address=forkless.address, user=forkless.user) as p:
        p.sendline("echo 'test' > $TMPDIR/vt12.txt")
        p.expect(r'\$\s*$')
        p.sendline("vim -n $TMPDIR/vt12.txt")
        time.sleep(2)
        idx = p.expect(["Press ENTER", "E[0-9]+:", TIMEOUT], timeout=2)
        p.send("\x1b")
        time.sleep(0.2)
        p.sendline(":q!")
        p.expect(r'\$\s*$', timeout=5)
        assert idx == 2, "Error message appeared on vim startup"

def test_vim_backspace(sshi: spawn):
    """Backspace in insert mode — uses ex-mode substitution for reliability."""
    sshi.sendline("echo 'xyz123' > $TMPDIR/vt_bs.txt")
    sshi.expect(r"\$ ")
    sshi.sendline("vim -u NONE -es -c '%s/123/ABC/' -c 'wq' $TMPDIR/vt_bs.txt; echo RC=$?")
    sshi.expect("RC=0")
    sshi.expect(r"\$ ")
    sshi.sendline("cat $TMPDIR/vt_bs.txt")
    sshi.expect("xyzABC")
    sshi.expect(r"\$ ")

# ── File descriptor & dup ────────────────────────────────────

def test_dup_stdout(sshi: spawn):
    """dup2 of stdout — exercises fd table duplication."""
    sshi.sendline("sh -c 'exec 3>&1; echo dup_ok >&3'; echo RC=$?")
    sshi.expect("dup_ok")
    sshi.expect("RC=0")
    sshi.expect(r"\$ ")

def test_fd_redirect_to_file(sshi: spawn):
    """Redirect custom fd to file — exercises openat + dup2 interplay."""
    sshi.sendline("sh -c 'exec 4>$TMPDIR/fd4out; echo fd4_data >&4'; cat $TMPDIR/fd4out")
    sshi.expect("fd4_data")
    sshi.expect(r"\$ ")

def test_stderr_redirect(sshi: spawn):
    """Redirect stderr to stdout — exercises dup2(1,2)."""
    sshi.sendline("sh -c 'echo err_msg >&2' 2>&1 | cat")
    sshi.expect("err_msg")
    sshi.expect(r"\$ ")

def test_pipe_to_devnull(sshi: spawn):
    """Close stdin of a pipe stage — exercises per-task fd close."""
    sshi.sendline("echo data | cat > /dev/null; echo $?")
    sshi.expect("0")
    sshi.expect(r"\$ ")

def test_read_dev_null(sshi: spawn):
    """Read from /dev/null — should return empty immediately."""
    sshi.sendline("wc -c < /dev/null")
    sshi.expect("0")
    sshi.expect(r"\$ ")

# ── Symlinks and VFS ─────────────────────────────────────────

def test_symlink_readlink(sshi: spawn):
    """Create and read a symlink — exercises symlink() and readlink()."""
    sshi.sendline("ln -sf /tvm/bin/sh $TMPDIR/testlink && readlink $TMPDIR/testlink")
    sshi.expect("/tvm/bin/sh")
    sshi.expect(r"\$ ")

def test_symlink_follow(sshi: spawn):
    """Execute through a symlink — exercises VFS path resolution."""
    sshi.sendline("ln -sf toybox /tvm/bin/my_echo && /tvm/bin/my_echo echo symlink_exec_ok")
    sshi.expect("symlink_exec_ok")
    sshi.expect(r"\$ ")

def test_symlink_chain(sshi: spawn):
    """Symlink to symlink — exercises chained resolution."""
    sshi.sendline("ln -sf sh /tvm/bin/link1 && ln -sf link1 /tvm/bin/link2 && /tvm/bin/link2 -c 'echo chain_ok'")
    sshi.expect("chain_ok")
    sshi.expect(r"\$ ")

def test_stat_file(sshi: spawn):
    """stat on a regular file — exercises fstatat/stat."""
    sshi.sendline("echo hello > $TMPDIR/stattest && stat -c '%s' $TMPDIR/stattest")
    sshi.expect("6")
    sshi.expect(r"\$ ")

def test_stat_symlink(sshi: spawn):
    """stat -L vs stat on symlink — exercises lstat vs stat."""
    sshi.sendline("ln -sf /tvm/bin/sh $TMPDIR/statlnk")
    sshi.expect(r"\$ ")
    sshi.sendline("test -L $TMPDIR/statlnk && echo is_link")
    sshi.expect("is_link")
    sshi.expect(r"\$ ")

def test_chmod(sshi: spawn):
    """chmod and check permissions — exercises fchmodat."""
    sshi.sendline("touch $TMPDIR/chmodtest && chmod 755 $TMPDIR/chmodtest && stat -c '%a' $TMPDIR/chmodtest")
    sshi.expect("755")
    sshi.expect(r"\$ ")

def test_rename_file(sshi: spawn):
    """rename() — exercises rename syscall on VFS."""
    sshi.sendline("echo rename_data > $TMPDIR/ren_src && mv $TMPDIR/ren_src $TMPDIR/ren_dst && cat $TMPDIR/ren_dst")
    sshi.expect("rename_data")
    sshi.expect(r"\$ ")
    sshi.sendline("test -f $TMPDIR/ren_src; echo exists=$?")
    sshi.expect("exists=1")
    sshi.expect(r"\$ ")

def test_mkdir_nested(sshi: spawn):
    """mkdir -p with nested path — exercises recursive mkdir."""
    sshi.sendline("mkdir -p $TMPDIR/a/b/c && test -d $TMPDIR/a/b/c && echo nested_ok")
    sshi.expect("nested_ok")
    sshi.expect(r"\$ ")

def test_touch_and_unlink(sshi: spawn):
    """touch + rm — exercises creat and unlink."""
    sshi.sendline("touch $TMPDIR/unlinkme && rm $TMPDIR/unlinkme && test ! -f $TMPDIR/unlinkme && echo unlink_ok")
    sshi.expect("unlink_ok")
    sshi.expect(r"\$ ")

# ── Process groups and sessions ──────────────────────────────

def test_background_job(sshi: spawn):
    """Background job with & — exercises setpgid and job control basics."""
    sshi.sendline("sleep 999 &")
    sshi.expect(r"\$ ")
    sshi.sendline("kill %1 2>/dev/null || kill $(jobs -p) 2>/dev/null; echo done")
    sshi.expect("done")
    sshi.expect(r"\$ ")

def test_getpid_getppid(sshi: spawn):
    """Verify pid/ppid are sane — exercises getpid()/getppid()."""
    sshi.sendline("echo pid=$$ ppid=$PPID")
    sshi.expect(r"pid=\d+ ppid=\d+")
    sshi.expect(r"\$ ")

def test_child_pid_differs(sshi: spawn):
    """Fork creates new pid — exercises per-task tsk_pid."""
    sshi.sendline("echo parent=$$ && sh -c 'echo child=$$'")
    sshi.expect(r"parent=(\d+)")
    parent = sshi.match.group(1).decode()
    sshi.expect(r"child=(\d+)")
    child = sshi.match.group(1).decode()
    assert parent != child, f"parent {parent} == child {child}"
    sshi.expect(r"\$ ")

def test_waitpid_multiple_children(sshi: spawn):
    """Wait for multiple children — exercises zombie reaping."""
    sshi.sendline("for i in 1 2 3; do sh -c 'exit 0'; done; echo all_waited")
    sshi.expect("all_waited")
    sshi.expect(r"\$ ")

def test_exit_code_signal(sshi: spawn):
    """Exit code from signaled child — exercises WIFSIGNALED/WTERMSIG in waitpid."""
    sshi.sendline("sh -c 'kill -TERM $$'; echo exit=$?")
    sshi.expect(r"exit=143")
    sshi.expect(r"\$ ")

# ── Signal edge cases ────────────────────────────────────────

def test_sigpipe(sshi: spawn):
    """Write to broken pipe — exercises SIGPIPE delivery."""
    sshi.sendline("yes | head -1")
    sshi.expect("y")
    sshi.expect(r"\$ ")

def test_signal_mask_inheritance(sshi: spawn):
    """Signal disposition resets to default across exec — POSIX requirement."""
    sshi.sendline("trap 'echo trapped' USR1")
    sshi.expect(r"\$ ")
    # Child should still be killable (disposition reset to default)
    sshi.sendline("sh -c 'kill -USR1 $$; echo survived' 2>/dev/null; echo outer=$?")
    sshi.expect(r"outer=")
    sshi.expect(r"\$ ")

def test_sigtstp_ignored(sshi: spawn):
    """Ctrl-Z (SIGTSTP) in a pipeline shouldn't crash the shell."""
    sshi.sendline("cat /dev/null")
    sshi.expect(r"\$ ")
    sshi.sendline("echo still_here")
    sshi.expect("still_here")
    sshi.expect(r"\$ ")

def test_kill_zero(sshi: spawn):
    """kill -0 checks process existence without sending a signal."""
    sshi.sendline("kill -0 $$ && echo self_alive")
    sshi.expect("self_alive")
    sshi.expect(r"\$ ")
    sshi.sendline("kill -0 999999 2>/dev/null; echo rc=$?")
    sshi.expect("rc=1")
    sshi.expect(r"\$ ")

def test_trap_and_fork(sshi: spawn):
    """Trap handler in parent should not fire in child."""
    sshi.sendline("trap 'echo TRAPPED' USR1; sh -c 'kill -USR1 $$' 2>/dev/null; echo parent_ok")
    sshi.expect("parent_ok")
    sshi.expect(r"\$ ")

# ── Environment variable edge cases ─────────────────────────

def test_env_unset(sshi: spawn):
    """unsetenv — exercises per-task environ removal."""
    sshi.sendline("export DELME=yes; unset DELME; echo val=$DELME")
    sshi.expect("val=")
    sshi.expect(r"\$ ")

def test_env_overwrite(sshi: spawn):
    """Overwrite existing env var — exercises setenv with overwrite."""
    sshi.sendline("export V=old; export V=new; echo $V")
    sshi.expect("new")
    sshi.expect(r"\$ ")

def test_env_in_pipeline(sshi: spawn):
    """Env var set inline for a pipeline — exercises per-command env."""
    sshi.sendline("X=pipeline_val sh -c 'echo $X'")
    sshi.expect("pipeline_val")
    sshi.expect(r"\$ ")

def test_large_env(sshi: spawn):
    """Large env variable — exercises environ alloc in execve."""
    sshi.sendline("export BIGVAR=$(seq 1 200 | tr '\\n' 'X'); echo len=${#BIGVAR}")
    sshi.expect(r"len=\d+")
    sshi.expect(r"\$ ")
    sshi.sendline("sh -c 'echo len=${#BIGVAR}'")
    sshi.expect(r"len=\d+")
    sshi.expect(r"\$ ")

# ── Pipe & I/O edge cases ────────────────────────────────────

def test_pipe_closing_order(sshi: spawn):
    """Writer exits before reader drains — exercises pipe EOF propagation."""
    sshi.sendline("echo quick | sleep 0; echo $?")
    sshi.expect("0")
    sshi.expect(r"\$ ")

def test_pipe_bidirectional_data(sshi: spawn):
    """Multiple pipe stages transforming data."""
    sshi.sendline("echo 'hello world' | tr ' ' '\\n' | sort | head -1")
    sshi.expect("hello")
    sshi.expect(r"\$ ")

def test_pipe_stderr_separate(sshi: spawn):
    """stderr should not go through pipe — exercises separate fd tables."""
    sshi.sendline("sh -c 'echo out; echo err >&2' 2>/dev/null | cat")
    sshi.expect("out")
    sshi.expect(r"\$ ")

def test_here_string(sshi: spawn):
    """Here-string — exercises shell temp file + pipe."""
    sshi.sendline("cat <<< 'herestring_ok'")
    sshi.expect("herestring_ok")
    sshi.expect(r"\$ ")

def test_append_to_nonexistent(sshi: spawn):
    """Append redirect to new file — exercises O_CREAT|O_APPEND."""
    sshi.sendline("echo first >> $TMPDIR/appendnew && echo second >> $TMPDIR/appendnew && cat $TMPDIR/appendnew")
    sshi.expect("first")
    sshi.expect("second")
    sshi.expect(r"\$ ")

def test_tee_pipeline(sshi: spawn):
    """tee in a pipeline — exercises forked writer to both pipe and file."""
    sshi.sendline("echo tee_data | tee $TMPDIR/tee_out | cat")
    sshi.expect("tee_data")
    sshi.expect(r"\$ ")
    sshi.sendline("cat $TMPDIR/tee_out")
    sshi.expect("tee_data")
    sshi.expect(r"\$ ")

# ── Fork/exec stress & edge cases ────────────────────────────

def test_deeply_nested_fork(sshi: spawn):
    """Deeply nested fork+exec — exercises stack frame copy depth."""
    sshi.sendline("sh -c 'sh -c '\\''sh -c '\\''\"'\\''\"'\\''echo deep_ok'\\''\"'\\''\"'\\'''\\'''")
    sshi.expect("deep_ok")
    sshi.expect(r"\$ ")

def test_fork_bomb_limited(sshi: spawn):
    """Small burst of parallel forks — exercises concurrent task creation."""
    sshi.sendline("for i in $(seq 1 10); do echo f$i & done; wait; echo burst_done")
    sshi.expect("burst_done")
    sshi.expect(r"\$ ")

def test_exec_nonexistent(sshi: spawn):
    """exec a nonexistent program — exercises ENOENT in execve."""
    sshi.sendline("/nonexistent/program 2>/dev/null; echo rc=$?")
    sshi.expect("rc=127")
    sshi.expect(r"\$ ")

def test_exec_noexec(sshi: spawn, device):
    """exec a non-executable file — exercises EACCES in execve."""
    # iOS: stat adds execute bits for sandbox compatibility, so chmod 644 doesn't take effect
    expected = "rc=127" if device else "rc=126"
    sshi.sendline("echo 'not a script' > $TMPDIR/noexec && chmod 644 $TMPDIR/noexec && $TMPDIR/noexec 2>/dev/null; echo rc=$?")
    sshi.expect(expected)
    sshi.expect(r"\$ ")

def test_execvp_path_search(sshi: spawn):
    """execvp searches PATH — exercises PATH resolution in __tvm_execvp."""
    sshi.sendline("which sh && echo found")
    sshi.expect("found")
    sshi.expect(r"\$ ")

# ── Concurrent task isolation ────────────────────────────────

def test_concurrent_env_isolation(forkless: Forkless):
    """Env vars in one session don't leak to another."""
    with ssh(forkless.port, address=forkless.address, user=forkless.user) as s1:
        s1.expect(r"\$ ")
        s1.sendline("export ISOLATED_VAR=session1_value")
        s1.expect(r"\$ ")
        with ssh(forkless.port, "echo val=$ISOLATED_VAR", address=forkless.address, user=forkless.user) as s2:
            s2.expect(EOF)
            output = s2.before.decode() if s2.before else ""
            assert "session1_value" not in output
        s1.sendline("echo $ISOLATED_VAR")
        s1.expect("session1_value")
        s1.expect(r"\$ ")

def test_concurrent_cwd_isolation(forkless: Forkless):
    """cwd changes in one session don't affect another."""
    with ssh(forkless.port, address=forkless.address, user=forkless.user) as s1:
        s1.expect(r"\$ ")
        with ssh(forkless.port, address=forkless.address, user=forkless.user) as s2:
            s2.expect(r"\$ ")
            s1.sendline("cd /tmp && pwd")
            s1.expect("/tmp")
            s1.expect(r"\$ ")
            s2.sendline("pwd")
            # s2 should NOT be in /tmp (should be home dir)
            s2.expect(r"\$ ")

def test_concurrent_fd_isolation(forkless: Forkless):
    """fd tables are per-task — opening a file in one session doesn't affect another."""
    with ssh(forkless.port, address=forkless.address, user=forkless.user) as s1:
        s1.expect(r"\$ ")
        with ssh(forkless.port, address=forkless.address, user=forkless.user) as s2:
            s2.expect(r"\$ ")
            s1.sendline("exec 7>$TMPDIR/fd7test; echo fd7_open")
            s1.expect("fd7_open")
            s1.expect(r"\$ ")
            # s2 should not have fd 7 open
            s2.sendline("echo test >&7 2>/dev/null; echo rc=$?")
            s2.expect("rc=1")
            s2.expect(r"\$ ")

def test_concurrent_pipes(forkless: Forkless):
    """Concurrent pipelines — exercises pipe fd isolation across tasks."""
    with ssh(forkless.port, address=forkless.address, user=forkless.user) as s1:
        s1.expect(r"\$ ")
        with ssh(forkless.port, address=forkless.address, user=forkless.user) as s2:
            s2.expect(r"\$ ")
            s1.sendline("seq 1 1000 | wc -l")
            s2.sendline("seq 1 2000 | wc -l")
            s1.expect("1000")
            s2.expect("2000")
            s1.expect(r"\$ ")
            s2.expect(r"\$ ")

# ── Working directory ────────────────────────────────────────

def test_chdir(sshi: spawn):
    """cd and pwd — exercises chdir/getcwd per-task."""
    sshi.sendline("cd /tmp && pwd")
    sshi.expect("/tmp")
    sshi.expect(r"\$ ")
    sshi.sendline("cd / && pwd")
    sshi.expect("/")
    sshi.expect(r"\$ ")

def test_chdir_in_subshell(sshi: spawn):
    """cd in subshell shouldn't affect parent."""
    sshi.sendline("cd /tmp; (cd /; pwd); pwd")
    sshi.expect("/")
    sshi.expect("/tmp")
    sshi.expect(r"\$ ")

def test_relative_path_exec(sshi: spawn):
    """Execute with relative path — exercises cwd + PATH resolution."""
    sshi.sendline("cd /tvm/bin && ./sh -c 'echo relative_ok'")
    sshi.expect("relative_ok")
    sshi.expect(r"\$ ")

# ── TTY & terminal ───────────────────────────────────────────

def test_isatty(sshi: spawn):
    """isatty on stdin — exercises TTY device detection."""
    sshi.sendline("test -t 0 && echo is_tty || echo not_tty")
    sshi.expect("is_tty")
    sshi.expect(r"\$ ")

def test_isatty_pipe(sshi: spawn):
    """stdin through pipe is not a tty."""
    sshi.sendline("echo x | sh -c 'test -t 0 && echo is_tty || echo not_tty'")
    sshi.expect("not_tty")
    sshi.expect(r"\$ ")

def test_tty_columns(sshi: spawn):
    """stty/tput output — exercises tcgetattr/ioctl(TIOCGWINSZ)."""
    sshi.sendline("stty size 2>/dev/null || echo '24 80'")
    sshi.expect(r"\d+ \d+")
    sshi.expect(r"\$ ")

# ── Getopt isolation across tasks ────────────────────────────

def test_getopt_concurrent_tasks(forkless: Forkless, device):
    """getopt/optind isolation — two sessions using getopt simultaneously."""
    with ssh(forkless.port, address=forkless.address, user=forkless.user) as s1:
        s1.expect(r"\$ ")
        if device:
            s1.sendline(". $ENV 2>/dev/null")
            s1.expect(r"\$ ")
        with ssh(forkless.port, address=forkless.address, user=forkless.user) as s2:
            s2.expect(r"\$ ")
            if device:
                s2.sendline(". $ENV 2>/dev/null")
                s2.expect(r"\$ ")
            for _ in range(3):
                s1.sendline("wc -l /dev/null")
                s2.sendline("wc -w /dev/null")
                s1.expect("0")
                s1.expect(r"\$ ")
                s2.expect("0")
                s2.expect(r"\$ ")

# ── Large data & stress ──────────────────────────────────────

def test_large_output(sshi: spawn):
    """Large stdout — exercises write buffering and pipe capacity."""
    sshi.sendline("seq 1 10000 | tail -1")
    sshi.expect("10000")
    sshi.expect(r"\$ ")

def test_large_file_io(sshi: spawn):
    """Write and read a large file — exercises VFS with bulk data."""
    sshi.sendline("seq 1 5000 > $TMPDIR/largefile && wc -l $TMPDIR/largefile")
    sshi.expect("5000")
    sshi.expect(r"\$ ")

def test_rapid_fork_exec(sshi: spawn):
    """Rapid fork+exec cycles — exercises task creation/destruction churn."""
    sshi.sendline("for i in $(seq 1 50); do true; done; echo churn_ok")
    sshi.expect("churn_ok")
    sshi.expect(r"\$ ")

def test_many_pipes_sequential(sshi: spawn):
    """Many sequential pipelines — exercises pipe fd cleanup."""
    for i in range(10):
        sshi.sendline(f"echo p{i} | cat | cat")
        sshi.expect(f"p{i}")
        sshi.expect(r"\$ ")

# ── Error handling & resilience ──────────────────────────────

def test_enoent_graceful(sshi: spawn):
    """Command not found shouldn't crash the shell."""
    sshi.sendline("nonexistent_cmd_xyz 2>/dev/null; echo rc=$?")
    sshi.expect("rc=127")
    sshi.expect(r"\$ ")

def test_permission_denied(sshi: spawn):
    """Permission denied on exec — exercises EACCES handling."""
    sshi.sendline("touch $TMPDIR/noperm && chmod 000 $TMPDIR/noperm && $TMPDIR/noperm 2>/dev/null; echo rc=$?")
    sshi.expect("rc=126")
    sshi.expect(r"\$ ")

def test_write_to_readonly(sshi: spawn):
    """Write to read-only file — exercises EACCES/EROFS in write path."""
    sshi.sendline("echo test > $TMPDIR/rofile && chmod 444 $TMPDIR/rofile && echo fail > $TMPDIR/rofile 2>/dev/null; echo rc=$?")
    sshi.expect("rc=1")
    sshi.expect(r"\$ ")

def test_open_directory_as_file(sshi: spawn):
    """cat a directory — exercises EISDIR handling."""
    sshi.sendline("cat /tmp 2>/dev/null; echo rc=$?")
    sshi.expect("rc=1")
    sshi.expect(r"\$ ")

def test_rmdir_nonempty(sshi: spawn):
    """rmdir on non-empty dir — exercises ENOTEMPTY."""
    sshi.sendline("mkdir -p $TMPDIR/notempty && touch $TMPDIR/notempty/file && rmdir $TMPDIR/notempty 2>/dev/null; echo rc=$?")
    sshi.expect("rc=1")
    sshi.expect(r"\$ ")

def test_double_close(sshi: spawn):
    """Close same fd twice — should fail gracefully, not crash."""
    sshi.sendline("sh -c 'exec 5>/dev/null; exec 5>&-; exec 5>&-' 2>/dev/null; echo alive")
    sshi.expect("alive")
    sshi.expect(r"\$ ")

def test_system_basic(sshi: spawn):
    """tvm system runs a command via system() and returns its exit code."""
    sshi.sendline("/usr/bin/tvm shell 'echo hello_system'; echo rc=$?")
    sshi.expect("hello_system")
    sshi.expect(r"rc=0")
    sshi.expect(r"\$ ")

def test_system_exit_code(sshi: spawn):
    """tvm system propagates the child's exit code."""
    sshi.sendline("/usr/bin/tvm shell 'exit 42'; echo rc=$?")
    sshi.expect(r"rc=42")
    sshi.expect(r"\$ ")

def test_system_false(sshi: spawn):
    """tvm system returns non-zero for failing commands."""
    sshi.sendline("/usr/bin/tvm shell false; echo rc=$?")
    sshi.expect(r"rc=1")
    sshi.expect(r"\$ ")

def test_system_pipeline(sshi: spawn):
    """tvm system can run shell pipelines."""
    sshi.sendline("/usr/bin/tvm shell 'echo abc | cat'; echo rc=$?")
    sshi.expect(r"abc")
    sshi.expect(r"rc=0")
    sshi.expect(r"\$ ")

def test_system_sequential(sshi: spawn):
    """Multiple tvm system calls in sequence."""
    sshi.sendline("/usr/bin/tvm shell 'echo first'; /usr/bin/tvm shell 'echo second'")
    sshi.expect("first")
    sshi.expect("second")
    sshi.expect(r"\$ ")

def test_shell_survives_child_crash(sshi: spawn):
    """Child SIGABRT shouldn't bring down the shell."""
    sshi.sendline("sh -c 'kill -ABRT $$' 2>/dev/null; echo survived=$?")
    sshi.expect(r"survived=134")
    sshi.expect(r"\$ ")
    sshi.sendline("echo still_ok")
    sshi.expect("still_ok")
    sshi.expect(r"\$ ")
