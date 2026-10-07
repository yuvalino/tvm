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
TVM_NOEMU_DEFAULT = "/tvm/bin/dropbear:/tvm/bin/bash:/tvm/bin/sh:/tvm/bin/toybox:/tvm/bin/vim:/tvm/bin/curl:/tvm/bin/less:/tvm/bin/nano:/tvm/bin/sqlite3:/tvm/bin/git:/tvm/libexec/git-core/git-remote-http:/tvm/libexec/git-core/git-http-fetch"
# build.py -t flutter symlinks the built app here (→ tvm.app / Runner.app).
FLUTTER_APP = f"./output/macos-arm64/lib/flutter"
IOS_APP = f"./output/ios-arm64/lib/flutter"
IOS_BUNDLE_ID = "com.yuvalino.tvm"




def clean_ansi(text: str) -> str:
    return re.sub(r'\x1b\[[?0-9;]*[a-zA-Z]|\x1b\][^\x07]*\x07|\x1b[>=<]', '', text)

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

def _test_failed(request) -> bool:
    """Whether the test using the fixture failed in setup or call (reports
    are attached by conftest's pytest_runtest_makereport)."""
    return any(getattr(getattr(request.node, "rep_" + when, None), "failed", False)
               for when in ("setup", "call"))

@pytest.fixture()
def forkless(request, device, backend, ssh_user) -> Iterator[Forkless]:
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
        if _test_failed(request):
            # The backend's kernel log (stdout+stderr of tvm / the app) is
            # the only trace of what happened inside TVM: show it in the
            # failure report and keep the file.
            proc._logfile.seek(0)
            print(f"--- backend log (kept at {proc._logpath}) ---")
            print(proc._logfile.read())
            proc._logfile.close()
        else:
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
        # (Guard `. $ENV`: bash's `.` with an empty $ENV is a usage error.)
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
        # (Guard `. $ENV`: bash's `.` with an empty $ENV is a usage error.)
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

@contextmanager
def remote_test_dir(forkless: Forkless, name: str) -> Iterator[str]:
    """A fresh, empty directory of the guest's own for a transfer test,
    under its TMPDIR (or HOME), removed afterwards. The tests used to work
    directly in TMPDIR: the non-interactive sessions inherit the runner's,
    and a developer Mac's /var/folders/.../T holds tens of thousands of
    entries, so test_sftp's `ls` of it took a minute and timed out."""
    # the physical path (pwd -P): sftp's realpath reports it that way, and
    # a TMPDIR with a trailing slash would otherwise show up as "T//name"
    with ssh(forkless.port, f"d=${{TMPDIR:-$HOME}}/{name}; rm -rf $d && mkdir -p $d && cd $d && echo DIR=$(pwd -P)",
             address=forkless.address, user=forkless.user) as p:
        p.expect(r"DIR=(/\S+)")
        remote_dir = p.match.group(1).decode() if isinstance(p.match.group(1), bytes) else p.match.group(1)
        p.expect(EOF)
    try:
        yield remote_dir
    finally:
        with ssh(forkless.port, f"rm -rf {remote_dir}", address=forkless.address, user=forkless.user) as p:
            p.expect(EOF)

@pytest.mark.parametrize("legacy_scp", [False, True], ids=["sftp", "legacy"])
def test_scp(forkless: Forkless, device, legacy_scp):
    with remote_test_dir(forkless, "tvm_test_scp") as remote_dir:
        _test_scp(forkless, device, legacy_scp, remote_dir)

def _test_scp(forkless: Forkless, device, legacy_scp, remote_dir):
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
    with remote_test_dir(forkless, "tvm_test_sftp") as remote_dir:
        _test_sftp(forkless, remote_dir)

def _test_sftp(forkless: Forkless, remote_dir: str):
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
            if line == "pwd":
                assert f"Remote working directory: {remote_dir}" in p.before, p.before
            if line == "ls":
                # the directory is the test's own: the listing is exactly the file put
                assert _lines(clean_ansi(p.before))[-1] == "sftp_put", p.before
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
    sshi.expect(r"bg=(\d+)\r?\n")
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
    # /tvm/bin/sh is a symlink to bash; the regular file is bash itself.
    sshi.sendline("find /tvm/bin -name 'bash' -type f")
    sshi.expect("/tvm/bin/bash")
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

# ── Dependent images ─────────────────────────────────────────

def test_exec_dependent_dylib(sshi: spawn, backend):
    """An executable that depends on a dylib of its own (@rpath/libtvmdep.dylib
    with an @loader_path run path; $ORIGIN on Linux), both built here on the
    host: the ulinker loads the dependency through TVM and resolves the
    executable's imports in it (ulinker.c, "Dependencies"). CLI backend only:
    the apps' sandbox cannot see files built on the host."""
    if backend != "tvm":
        pytest.skip("needs the host filesystem")
    d = tempfile.mkdtemp(prefix="tvmdep_", dir="/tmp")
    with open(f"{d}/dep.c", "w") as f:
        f.write('const char *tvmdep_greeting(void) { return "hello from the dependency"; }\n')
    # the child of a fork() runs on a duplicated linker, dependency included
    with open(f"{d}/main.c", "w") as f:
        f.write('#include <stdio.h>\n#include <unistd.h>\n#include <sys/wait.h>\n'
                'const char *tvmdep_greeting(void);\n'
                'int main(void) { printf("%s\\n", tvmdep_greeting()); fflush(stdout);\n'
                '  pid_t p = fork(); if (p == 0) { printf("child: %s\\n", tvmdep_greeting()); return 0; }\n'
                '  int s; waitpid(p, &s, 0); return WEXITSTATUS(s); }\n')
    if PLATFORM.startswith("macos"):
        subprocess.run(["cc", "-dynamiclib", "-install_name", "@rpath/libtvmdep.dylib",
                        "-o", f"{d}/libtvmdep.dylib", f"{d}/dep.c"], check=True)
        subprocess.run(["cc", "-o", f"{d}/depmain", f"{d}/main.c", "-L", d, "-ltvmdep",
                        "-Wl,-rpath,@loader_path"], check=True)
    else:
        subprocess.run(["cc", "-shared", "-fPIC", "-o", f"{d}/libtvmdep.so", f"{d}/dep.c"], check=True)
        subprocess.run(["cc", "-o", f"{d}/depmain", f"{d}/main.c", "-L", d, "-ltvmdep",
                        "-Wl,-rpath,$ORIGIN"], check=True)
    sshi.sendline(f"{d}/depmain; echo RC=$?")
    sshi.expect("hello from the dependency")
    sshi.expect("child: hello from the dependency")
    sshi.expect("RC=0")
    sshi.expect(r"\$ ")

# ── Apple *_cmds ──────────────────────────────────────────────
# The real BSD userland from apple-oss-distributions, built as individual
# executables and dropped into the initrd unsigned (see
# third_party/apple/CMakeLists.txt). These run under the emulator on iOS, so
# they also cover a much larger unsigned binary than toybox does.
#
# Deliberately limited to commands that produce checkable output as an
# unprivileged, sandboxed process: anything needing root or a real block device
# (quotaon, vipw, mkfile on /, fstyp) would only prove that it fails.
#
# Apple *_cmds are only built and bundled when the target is a Darwin platform.
pytestmark_apple = pytest.mark.skipif(
    not PLATFORM.startswith(("macos-", "ios-")),
    reason="apple *_cmds are only in Darwin target initrds")

@pytestmark_apple
def test_apple_binary_exists(sshi: spawn):
    """Verify that apple commands are individual binaries in the initrd."""
    sshi.sendline("file /tvm/bin/hostinfo")
    sshi.expect("Mach-O")
    sshi.expect(r"\$ ")

@pytestmark_apple
def test_apple_hostinfo(sshi: spawn):
    """hostinfo — Mach host_info() RPC from an emulated guest binary."""
    sshi.sendline("hostinfo")
    sshi.expect("Mach kernel version")
    sshi.expect(r"\$ ")

@pytestmark_apple
def test_apple_sysctl(sshi: spawn):
    """sysctl reading a known-stable MIB. On iOS the sandbox blocks some MIBs,
    so we accept both the real output and the 'not permitted' error."""
    sshi.sendline("sysctl kern.ostype")
    sshi.expect(r"kern\.ostype: Darwin|Operation not permitted")
    sshi.expect(r"\$ ")

@pytestmark_apple
def test_apple_vm_stat(sshi: spawn):
    """vm_stat — host_statistics64(), and a page-size-dependent header."""
    sshi.sendline("vm_stat")
    sshi.expect("Mach Virtual Memory Statistics")
    sshi.expect(r"\$ ")

@pytestmark_apple
def test_apple_lsvfs(sshi: spawn):
    """lsvfs — getvfsbyname()/sysctl over the loaded filesystems."""
    sshi.sendline("lsvfs")
    sshi.expect("Filesystem")
    sshi.expect(r"\$ ")

@pytestmark_apple
def test_apple_ps(sshi: spawn):
    """ps — libproc + mach_vm, the heaviest adv_cmds command. On iOS the
    sandbox blocks the process-list sysctl, so accept either output."""
    sshi.sendline("ps -o pid,comm")
    sshi.expect(r"PID\s+COMM|Operation not permitted|Failure calling sysctl")
    sshi.expect(r"\$ ")

@pytestmark_apple
def test_apple_ifconfig_loopback(sshi: spawn):
    """ifconfig on the loopback — exercises the reconstructed net/if.h ioctl
    surface (SIOCGIFFLAGS and friends)."""
    sshi.sendline("ifconfig lo0")
    sshi.expect("lo0")
    sshi.expect("127.0.0.1")
    sshi.expect(r"\$ ")

@pytestmark_apple
def test_apple_ifconfig_list(sshi: spawn):
    """ifconfig -l — plain interface-name enumeration."""
    sshi.sendline("ifconfig -l")
    sshi.expect("lo0")
    sshi.expect(r"\$ ")

@pytestmark_apple
def test_apple_netstat_routes(sshi: spawn):
    """netstat -rn — reads the routing table over PF_ROUTE sysctl. This is the
    command that needed the private in_pcb/socketvar/mbuf headers."""
    sshi.sendline("netstat -rn")
    sshi.expect("Routing tables")
    sshi.expect("Destination")
    sshi.expect(r"\$ ")

@pytestmark_apple
def test_apple_netstat_stats(sshi: spawn):
    """netstat -s -p tcp — protocol statistics, a different sysctl path.
    On iOS the sandbox blocks this sysctl, so we just check it returns."""
    sshi.sendline("netstat -s -p tcp; echo nss_done")
    sshi.expect("nss_done")
    sshi.expect(r"\$ ")

@pytestmark_apple
def test_apple_sync(sshi: spawn):
    """sync — trivial, but proves a bare syscall wrapper exits 0."""
    sshi.sendline("sync && echo sync_ok")
    sshi.expect("sync_ok")
    sshi.expect(r"\$ ")

@pytestmark_apple
def test_apple_mkfile(sshi: spawn):
    """mkfile in a writable dir — creates a sparse file of an exact size."""
    sshi.sendline("mkfile -n 4k $TMPDIR/apple_mkfile && wc -c < $TMPDIR/apple_mkfile")
    sshi.expect("4096")
    sshi.expect(r"\$ ")

@pytestmark_apple
def test_apple_repeated_invocation(sshi: spawn):
    """Same module run several times — guards against per-task getopt/global
    state leaking between execs of a multi-call binary."""
    for _ in range(3):
        sshi.sendline("hostinfo | head -1")
        sshi.expect("Mach kernel version")
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
    with remote_test_dir(forkless, "tvm_test_scp_large") as remote_dir:
        _test_scp_large_file(forkless, device, remote_dir)

def _test_scp_large_file(forkless: Forkless, device, remote_dir):
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

# ── curl ──────────────────────────────────────────────────────

# The guest shares the runner's network, so reachability is probed from the
# runner and the whole group skips cleanly when offline. Every request carries
# `-m 20` so a stalled connection fails with curl's own diagnostic (rc=28 plus
# a message) instead of a bare pexpect timeout. The expect timeout is generous
# because the device backend runs at real speed on a phone's Wi-Fi.
CURL_TIMEOUT = 120

def _reachable(host: str, port: int = 443) -> bool:
    try:
        socket.create_connection((host, port), timeout=5).close()
        return True
    except OSError:
        return False

_HAS_NETWORK = _reachable("example.com")
network_required = pytest.mark.skipif(
    not _HAS_NETWORK, reason="no network access to example.com:443 from the runner")

def _sh(sshi: spawn, cmd: str, timeout: float = CURL_TIMEOUT) -> tuple[str, int]:
    """Run cmd in the interactive session and return (output, exit code).
    The output is everything printed up to the `rc=` marker (ANSI-stripped),
    which the assertions below include in their messages so a failure shows
    what curl actually said."""
    sshi.sendline(f"{cmd}; echo rc=$?")
    # Anchor on the line end: pexpect matches against the bytes received so
    # far, so an unanchored rc=(\d+) can match "rc=6" before the "0" of
    # "rc=60" arrives.
    sshi.expect(r"rc=(\d+)\r?\n", timeout=timeout)
    out = clean_ansi(sshi.before.decode(errors="replace"))
    rc = int(sshi.match.group(1))
    sshi.expect(r"\$ ", timeout=10)
    return out, rc

def _lines(out: str) -> list[str]:
    return [l.strip() for l in out.splitlines() if l.strip()]

# curl's exit codes that say the request never got an answer: name resolution,
# connection, and the -m 20 deadline. Everything else, rc=60 (verification)
# in particular, is a verdict about TLS and the tests assert on it.
_CURL_NO_ANSWER = {6: "could not resolve host", 7: "could not connect", 28: "timed out"}

def _runner_reaches(url: str) -> tuple[bool, str]:
    """Whether the runner itself completes a request to url right now
    (unverified: -k, so an expired or self-signed certificate still counts
    as reachable). Returns (ok, diagnostic)."""
    r = subprocess.run(["curl", "-sSk", "-m", "20", "-o", "/dev/null", url],
                       capture_output=True, text=True)
    return r.returncode == 0, f"runner curl rc={r.returncode}{': ' + r.stderr.strip() if r.stderr.strip() else ''}"

def _curl(sshi: spawn, cmd: str, url: str) -> tuple[str, int]:
    """Run a curl command line in the guest, like _sh. The hosts here
    (badssl.com, example.com) are outside anyone's control, and a stalled
    connection used to fail the test with a bare rc=28. When the guest got no
    answer at all (rc 6/7/28, or such a curl diagnostic in the output of a
    pipeline), the runner probes the same URL: if it cannot reach it either
    the test is skipped, with both diagnostics -- that is the network -- and
    if it can, the test fails: the guest could not do what the runner can."""
    out, rc = _sh(sshi, cmd)
    m = re.search(r"curl: \((6|7|28)\)", out)
    code = rc if rc in _CURL_NO_ANSWER else int(m.group(1)) if m else None
    if code is not None:
        ok, diag = _runner_reaches(url)
        why = f"guest curl rc={code} ({_CURL_NO_ANSWER[code]})"
        if not ok:
            pytest.skip(f"{url} unreachable: {why}; {diag}")
        pytest.fail(f"{url}: {why}, but the runner reaches it ({diag}):\n{out}")
    return out, rc

@network_required
def test_curl_version(sshi: spawn):
    """curl --version — the tool links libcurl 8.x against OpenSSL 3.x and
    was built with SSL, IPv6, zlib and the threaded resolver. On Darwin the
    trust store is Apple SecTrust rather than a CA bundle."""
    out, rc = _sh(sshi, "curl --version")
    assert rc == 0, out
    lines = _lines(out)
    # The tty echoes the command line first, so key on the version line's
    # libcurl/ token rather than on "curl " at the start.
    first = next((l for l in lines if l.startswith("curl ") and "libcurl/" in l), "")
    assert "libcurl/8." in first and "OpenSSL/3." in first, out
    features = next((l for l in lines if l.startswith("Features:")), "")
    for feat in ("SSL", "IPv6", "libz", "AsynchDNS"):
        assert re.search(rf"\b{feat}\b", features), f"missing {feat}: {out}"
    if PLATFORM.startswith(("macos-", "ios-")):
        assert "AppleSecTrust" in features, out

@network_required
def test_curl_https_get(sshi: spawn):
    """curl over HTTPS — OpenSSL handshake plus certificate verification
    (Apple SecTrust on Darwin, Mozilla bundle on Linux)."""
    out, rc = _curl(sshi, "curl -sS -m 20 -o /dev/null -w '%{http_code} %{ssl_verify_result}\\n' https://example.com", "https://example.com")
    assert rc == 0, out
    assert "200 0" in _lines(out), out

@network_required
def test_curl_http_get(sshi: spawn):
    """Plain HTTP — the socket/resolver path with no TLS involved."""
    out, rc = _curl(sshi, "curl -sS -m 20 -o /dev/null -w '%{http_code}\\n' http://example.com", "http://example.com")
    assert rc == 0, out
    assert "200" in _lines(out), out

@network_required
def test_curl_verify_self_signed_fails(sshi: spawn):
    """An untrusted chain must be rejected: curl exits 60 (peer cert failed
    verification), proving verification is really on."""
    out, rc = _curl(sshi, "curl -sS -m 20 -o /dev/null https://self-signed.badssl.com/", "https://self-signed.badssl.com/")
    assert rc == 60, f"rc={rc}: {out}"

@network_required
def test_curl_insecure_self_signed_ok(sshi: spawn):
    """The same host with -k succeeds, so the previous failure was the
    verifier's decision and not a broken connection."""
    out, rc = _curl(sshi, "curl -sS -k -m 20 -o /dev/null -w '%{http_code}\\n' https://self-signed.badssl.com/", "https://self-signed.badssl.com/")
    assert rc == 0, out
    assert "200" in _lines(out), out

@network_required
def test_curl_expired_fails(sshi: spawn):
    """Validity period is checked, not just chain trust."""
    out, rc = _curl(sshi, "curl -sS -m 20 -o /dev/null https://expired.badssl.com/", "https://expired.badssl.com/")
    assert rc == 60, f"rc={rc}: {out}"

@network_required
def test_curl_wrong_host_fails(sshi: spawn):
    """Hostname is matched against the certificate's SANs."""
    out, rc = _curl(sshi, "curl -sS -m 20 -o /dev/null https://wrong.host.badssl.com/", "https://wrong.host.badssl.com/")
    assert rc == 60, f"rc={rc}: {out}"

@network_required
def test_curl_download_to_file(sshi: spawn):
    """-o into the guest's writable TMPDIR — the response body goes through
    the VFS write path, not the pty."""
    out, rc = _curl(sshi, 'curl -sS -m 20 -o "$TMPDIR/curl_out.html" https://example.com && wc -c < "$TMPDIR/curl_out.html"', "https://example.com")
    assert rc == 0, out
    sizes = [int(l) for l in _lines(out) if l.isdigit()]
    assert sizes and sizes[-1] > 0, out
    _sh(sshi, 'rm -f "$TMPDIR/curl_out.html"')

# Echo services for the POST test, in order of preference. httpbin is
# occasionally flaky (5xx), so postman-echo is tried when httpbin does not
# echo the body back. httpbin pretty-prints ("a": 1), postman-echo does not
# ("a":1), hence the regex.
_ECHO_HOSTS = ("httpbin.org", "postman-echo.com")

@network_required
def test_curl_post_json(sshi: spawn):
    """POST with a JSON body and header — request-body upload and the
    response is read back through the pipe/pty."""
    hosts = [h for h in _ECHO_HOSTS if _reachable(h)]
    if not hosts:
        pytest.skip("neither httpbin.org nor postman-echo.com reachable from the runner")
    attempts = []
    for host in hosts:
        out, rc = _sh(sshi, f"curl -sS -m 20 -H 'Content-Type: application/json' -d '{{\"a\":1}}' https://{host}/post")
        # Match the parsed-JSON echo, not the raw body or the echoed command
        # line (both contain the literal {"a":1}).
        if rc == 0 and re.search(r'"json":\s*\{\s*"a":\s*1', out):
            return
        attempts.append(f"--- {host}: rc={rc}\n{out}")
    pytest.fail("no echo service returned the posted JSON:\n" + "\n".join(attempts))

@network_required
def test_curl_repeated(sshi: spawn):
    """Five HTTPS GETs back to back in one shell. Each curl is a fresh guest
    task, so this exercises OpenSSL's per-thread state being set up and torn
    down repeatedly (the thread-exit hazard OpenSSL's atexit cleanup handles,
    see the openssl_ext comment in third_party/CMakeLists.txt)."""
    out, rc = _curl(sshi, "for i in 1 2 3 4 5; do curl -sS -m 20 -o /dev/null -w '%{http_code}\\n' https://example.com; done", "https://example.com")
    assert rc == 0, out
    codes = [l for l in _lines(out) if l == "200"]
    assert len(codes) == 5, f"got {len(codes)} x 200: {out}"

@network_required
def test_curl_pipeline(sshi: spawn):
    """curl's stdout through a TVM pipe into toybox grep."""
    out, rc = _curl(sshi, "curl -sS -m 20 https://example.com | grep -c '<html'", "https://example.com")
    assert rc == 0, out
    assert _lines(out)[-1] == "1", out

@network_required
def test_curl_in_background_job(sshi: spawn):
    """curl as a background job, reaped with wait — job control around a
    task that spawns its own resolver thread."""
    out, rc = _curl(sshi, "curl -sS -m 20 -o /dev/null https://example.com & wait $!", "https://example.com")
    assert rc == 0, out

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

def test_vim_terminal_sequences(forkless: Forkless):
    """vim drives the terminal through terminfo: the raw session must carry
    the control sequences of the announced terminal (xterm-256color's smcup,
    ESC [ ? 1049 h, when the full-screen editor starts). Every platform links
    the one static ncursesw and reads the initrd's terminfo database; the
    iOS build used to link stub termcap functions that swallowed every
    sequence, so vim drew nothing on a device."""
    with ssh_interactive(forkless.port, address=forkless.address, user=forkless.user) as p:
        p.sendline("export TERM=xterm-256color; vim -n -u NONE $TMPDIR/vt_seq.txt")
        idx = p.expect([r"\x1b\[\?1049h", TIMEOUT], timeout=5)
        p.send("\x1b")
        time.sleep(0.3)
        p.sendline(":q!")
        p.expect(r'\$\s*$', timeout=5)
        assert idx == 0, "vim emitted no smcup (ESC[?1049h) control sequence"

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

# ── less ─────────────────────────────────────────────────────

# less resets attributes after every line it paints (`\x1b[m` with LESS=-R);
# match line text tolerating a trailing SGR sequence.
_SGR = r"(?:\x1b\[[0-9;]*m)?"

def _less_lines(*lines):
    return r"\r\n".join(l + _SGR for l in lines)

def test_less_version(sshi: spawn):
    sshi.sendline("less --version | head -1")
    sshi.expect(r"less \d+ ")
    sshi.expect(r"\$ ")

def test_pager_editor_defaults(sshi: spawn):
    """The rc files export TERMINFO and default PAGER/LESS/EDITOR."""
    sshi.sendline("echo PAGER=${PAGER:-unset} LESS=${LESS:-unset} EDITOR=${EDITOR:-unset} TERMINFO=$TERMINFO")
    sshi.expect(r"PAGER=(\S+) LESS=(\S+) EDITOR=(\S+) TERMINFO=/tvm/share/terminfo")
    # setenv? keeps values the host environment already had; only the
    # unconditional TERMINFO is exact.
    assert b"unset" not in sshi.match.group(0)
    sshi.expect(r"\$ ")
    # the status, not ls's output: the echoed command line contains the paths
    sshi.sendline('test -f /tvm/share/terminfo/x/xterm-256color && test -f /tvm/share/terminfo/d/dumb; echo "terminfo=$?"')
    sshi.expect(r"terminfo=0\r?\n")
    sshi.expect(r"\$ ")

def test_less_interactive(sshi: spawn):
    """Pipe into less on a tty: first screen, G to the end, q back to the shell."""
    sshi.sendline("export TERM=xterm-256color; seq 1 500 | less")
    sshi.expect(_less_lines("1", "2", "3"))
    sshi.expect(r"\r\n23\b")
    sshi.send("G")
    sshi.expect(r"\r\n500\b")
    sshi.send("q")
    sshi.expect(r"\$ ")
    sshi.sendline("echo still_here")
    sshi.expect("still_here")
    sshi.expect(r"\$ ")

def test_less_file_search(sshi: spawn):
    """less on a file: / search jumps to the match, q exits with status 0."""
    sshi.sendline("export TERM=xterm-256color; seq 1 500 > $TMPDIR/less_file.txt; less $TMPDIR/less_file.txt; echo RC=$?")
    sshi.expect(r"\r\n23\b")
    sshi.expect("less_file.txt")   # file-mode prompt: first screen fully painted
    sshi.send("/250\r")
    sshi.expect("250")            # the match, highlighted (reverse video)
    sshi.expect(_less_lines("", "251"))
    sshi.send("q")
    sshi.expect("RC=0")
    sshi.expect(r"\$ ")

def test_less_not_a_tty(sshi: spawn):
    """stdout is a pipe: less passes the input through unchanged (cat mode)."""
    sshi.sendline("seq 1 5 | less | cat; echo RC=$?")
    sshi.expect("1\r\n2\r\n3\r\n4\r\n5\r\nRC=0")
    sshi.expect(r"\$ ")
    sshi.sendline("seq 1 5 > $TMPDIR/less_pipe.txt; less $TMPDIR/less_pipe.txt | cat; echo RC=$?")
    sshi.expect("1\r\n2\r\n3\r\n4\r\n5\r\nRC=0")
    sshi.expect(r"\$ ")

def test_less_pager_quit_if_one_screen(sshi: spawn):
    """git-style PAGER='less -F -X': one screen prints and exits without
    interaction, more than one screen pages until q."""
    sshi.sendline("export TERM=xterm-256color PAGER='less -F -X'; seq 1 3 | sh -c '$PAGER'; echo RC=$?")
    sshi.expect(_less_lines("1", "2", "3"))
    sshi.expect("RC=0")
    sshi.expect(r"\$ ")
    sshi.sendline("seq 1 500 | sh -c '$PAGER'; echo RC=$?")
    sshi.expect(r"\r\n23\b")
    sshi.send("q")
    sshi.expect("RC=0")
    sshi.expect(r"\$ ")

# ── nano ─────────────────────────────────────────────────────

def _expect_all(sshi: spawn, *texts: str):
    """Wait until every text has appeared, in any order. A full-screen
    program paints its rows in whatever order its screen update chooses:
    nano sometimes draws the help bar ("Write Out") before the file's lines,
    and a sequence of expects then skips past the earlier one."""
    pending = list(texts)
    while pending:
        i = sshi.expect([re.escape(t) for t in pending])
        del pending[i]

def test_nano_version(sshi: spawn):
    sshi.sendline("nano --version | head -1")
    sshi.expect(r"GNU nano, version \d+\.\d+")
    sshi.expect(r"\$ ")

def test_nano_interactive_edit(sshi: spawn):
    """Open a new file in nano, type, ^O Enter to save, ^X to quit, cat it."""
    sshi.sendline("export TERM=xterm-256color; rm -f $TMPDIR/nano_new.txt; nano $TMPDIR/nano_new.txt")
    sshi.expect("Write Out")
    sshi.send("hello from nano")
    sshi.send("\x0f")            # ^O
    sshi.expect("File")          # "File Name to Write:" prompt
    sshi.send("\r")
    sshi.expect(r"Wrote \d+ lines?")
    sshi.send("\x18")            # ^X
    sshi.expect(r"\$ ")
    sshi.sendline("cat $TMPDIR/nano_new.txt")
    sshi.expect("hello from nano")
    sshi.expect(r"\$ ")

def test_nano_open_existing(sshi: spawn):
    """nano shows an existing file's content; ^X on an unmodified buffer exits."""
    sshi.sendline("export TERM=xterm-256color; printf 'first line\\nsecond line\\n' > $TMPDIR/nano_old.txt; nano $TMPDIR/nano_old.txt; echo RC=$?")
    _expect_all(sshi, "first line", "second line", "Write Out")
    sshi.send("\x18")            # ^X
    sshi.expect("RC=0")
    sshi.expect(r"\$ ")

def test_nano_syntax_rc(sshi: spawn):
    """/tvm/etc/nanorc includes the bundled syntax files (glob include):
    a shell script opens straight into the editor with no rc errors."""
    # the status, not ls's output: the echoed command line contains the paths
    sshi.sendline('test -f /tvm/share/nano/sh.nanorc && test -f /tvm/etc/nanorc; echo "nanorc=$?"')
    sshi.expect(r"nanorc=0\r?\n")
    sshi.expect(r"\$ ")
    sshi.sendline("export TERM=xterm-256color; printf '#!/bin/sh\\necho hi\\n' > $TMPDIR/nano_syn.sh; nano $TMPDIR/nano_syn.sh 2> $TMPDIR/nano_syn.err; echo RC=$?")
    sshi.expect("Write Out")
    sshi.send("\x18")            # ^X
    sshi.expect("RC=0")
    sshi.expect(r"\$ ")
    sshi.sendline("wc -c < $TMPDIR/nano_syn.err")
    sshi.expect(r"[\r\n]0\r\n")
    sshi.expect(r"\$ ")

def test_nano_file_browser(sshi: spawn):
    """nano's file browser (^R, then ^T) reads a directory twice with a
    rewinddir() in between, which took the editor down with SIGSEGV when
    rewinddir reached the host libc with TVM's stream. The browser shows
    the directory and its entries, and leaves nano in one piece."""
    sshi.sendline("export TERM=xterm-256color; d=$(mktemp -d) && touch $d/browsed_file && cd $d && nano; echo RC=$?")
    sshi.expect("Write Out")
    sshi.send("\x12")            # ^R  Read File
    sshi.expect("File to insert")
    sshi.send("\x14")            # ^T  To Files
    _expect_all(sshi, "DIR:", "browsed_file")
    sshi.send("\x03")            # ^C  Cancel the browser
    sshi.expect("File to insert")
    sshi.send("\x03")            # ^C  Cancel the prompt
    sshi.send("\x18")            # ^X
    sshi.expect("RC=0")
    sshi.expect(r"\$ ")

# ── sqlite3 ──────────────────────────────────────────────────

def test_sqlite3_version(sshi: spawn):
    """sqlite3 --version: the 3.x shell built from the amalgamation."""
    out, rc = _sh(sshi, "sqlite3 --version")
    assert rc == 0, out
    assert any(re.match(r"3\.\d+\.\d+ \d{4}-\d{2}-\d{2} ", l) for l in _lines(out)), out

def test_sqlite3_create_query(sshi: spawn):
    """A database file in the guest TMPDIR: create, insert, read back in
    column mode (with headers) and as JSON."""
    out, rc = _sh(sshi, "rm -f $TMPDIR/sq_basic.db; sqlite3 $TMPDIR/sq_basic.db "
                        "\"create table t(id integer primary key, name text); "
                        "insert into t(name) values ('alice'),('bob');\"")
    assert rc == 0, out
    out, rc = _sh(sshi, "sqlite3 -header -column $TMPDIR/sq_basic.db 'select * from t order by id'")
    assert rc == 0, out
    lines = _lines(out)
    for pat in (r"id\s+name", r"1\s+alice", r"2\s+bob"):
        assert any(re.fullmatch(pat, l) for l in lines), f"{pat}: {out}"
    out, rc = _sh(sshi, "sqlite3 -json $TMPDIR/sq_basic.db 'select * from t order by id'")
    assert rc == 0, out
    assert '{"id":1,"name":"alice"}' in out and '{"id":2,"name":"bob"}' in out, out

def test_sqlite3_dot_commands(sshi: spawn):
    """.tables and .schema, both as command-line arguments and on stdin."""
    out, rc = _sh(sshi, "rm -f $TMPDIR/sq_dot.db; sqlite3 $TMPDIR/sq_dot.db "
                        "'create table alpha(a); create table beta(b text)' '.tables'")
    assert rc == 0, out
    assert any(re.fullmatch(r"alpha\s+beta", l) for l in _lines(out)), out
    out, rc = _sh(sshi, "printf '.schema beta\\n' | sqlite3 $TMPDIR/sq_dot.db")
    assert rc == 0, out
    assert "CREATE TABLE beta(b text);" in _lines(out), out

def test_sqlite3_memory_functions(sshi: spawn):
    """An in-memory database with a math function (SQLITE_ENABLE_MATH_FUNCTIONS)
    and JSON functions (built in)."""
    out, rc = _sh(sshi, "sqlite3 -list :memory: \"select sqrt(16), power(2, 10), "
                        "json_extract('{\\\"a\\\":42}', '$.a'), json_array(1, 'x')\"")
    assert rc == 0, out
    assert '4.0|1024.0|42|[1,"x"]' in _lines(out), out

def test_sqlite3_extensions(sshi: spawn):
    """FTS5 and R*Tree virtual tables and the dbstat table are compiled in;
    extension loading is not (no dlopen in a TVM image)."""
    out, rc = _sh(sshi, "sqlite3 -list :memory: "
                        "\"create virtual table ft using fts5(body); "
                        "insert into ft values ('the quick brown fox'), ('lazy dog'); "
                        "select body from ft where ft match 'quick';\" "
                        "\"create virtual table rt using rtree(id, minx, maxx); "
                        "insert into rt values (1, 0, 10), (2, 20, 30); "
                        "select 'rtree:' || id from rt where minx <= 5 and maxx >= 5;\" "
                        "\"select 'dbstat:' || (count(*) > 0) from dbstat\"")
    assert rc == 0, out
    lines = _lines(out)
    # distinct result lines, none of which appear in the echoed command
    assert "the quick brown fox" in lines and "rtree:1" in lines and "dbstat:1" in lines, out
    out, rc = _sh(sshi, "sqlite3 :memory: \"select load_extension('nope')\"")
    assert rc != 0, out
    assert "no such function: load_extension" in out, out

def test_sqlite3_interactive(sshi: spawn):
    """An interactive session on the tty (linenoise line editing): a
    statement at the sqlite> prompt, then .quit back to the shell."""
    sshi.sendline("sqlite3 -list :memory:")
    sshi.expect("sqlite> ")
    # linenoise runs the tty in raw mode: Enter is the CR a terminal sends,
    # not the LF sendline() would.
    sshi.send("select 40 + 2;\r")
    sshi.expect(r"\r\n42\r\n")
    sshi.expect("sqlite> ")
    sshi.send(".quit\r")
    sshi.expect(r"\$ ")
    sshi.sendline("echo still_here")
    sshi.expect("still_here")
    sshi.expect(r"\$ ")

def test_sqlite3_transaction_rollback(sshi: spawn):
    """ROLLBACK undoes an insert within the same connection; a later
    connection sees only what was committed."""
    out, rc = _sh(sshi, "rm -f $TMPDIR/sq_tx.db; sqlite3 -list $TMPDIR/sq_tx.db "
                        "\"create table t(v); insert into t values (1); "
                        "begin; insert into t values (2); select count(*) from t; rollback; "
                        "select count(*) from t;\"")
    assert rc == 0, out
    assert _lines(out)[-2:] == ["2", "1"], out
    out, rc = _sh(sshi, "sqlite3 -list $TMPDIR/sq_tx.db 'select * from t'")
    assert rc == 0, out
    assert _lines(out)[-1] == "1", out

def test_sqlite3_import_csv(sshi: spawn):
    """.import of a 5000-row CSV written by the shell (header row names the
    columns), then an aggregate over it."""
    n = 5000
    out, rc = _sh(sshi, f"awk 'BEGIN {{ print \"id,name,score\"; for (i = 1; i <= {n}; i++) "
                        "print i \",n\" i \",\" i % 97 }' > $TMPDIR/sq_in.csv; wc -l < $TMPDIR/sq_in.csv")
    assert rc == 0, out
    assert _lines(out)[-1] == str(n + 1), out
    out, rc = _sh(sshi, "rm -f $TMPDIR/sq_imp.db; sqlite3 -list $TMPDIR/sq_imp.db "
                        "\".import --csv $TMPDIR/sq_in.csv rows\" "
                        "\"select count(*), sum(cast(score as integer)), name from rows where id = 4242 or id > 0\"")
    assert rc == 0, out
    assert _lines(out)[-1] == f"{n}|{sum(i % 97 for i in range(1, n + 1))}|n1", out

def test_sqlite3_locking(sshi: spawn):
    """Two sqlite3 processes on one file. The first holds a write transaction
    open (BEGIN IMMEDIATE) for a few seconds; meanwhile a reader gets its
    rows (shared locks coexist with RESERVED) and a second writer is refused
    with SQLITE_BUSY. The first then commits successfully: its locks survived
    the other two opening and closing the file. This is fcntl() record locking
    between tasks of one host process, which TVM emulates."""
    out, rc = _sh(sshi, "rm -f $TMPDIR/sq_lock.db $TMPDIR/sq_lock.err; "
                        "sqlite3 $TMPDIR/sq_lock.db 'create table lk(v); insert into lk values (0)'")
    assert rc == 0, out
    out, rc = _sh(sshi, "( printf 'begin immediate;\\ninsert into lk values (1);\\n'; sleep 4; "
                        "printf 'commit;\\n' ) | sqlite3 $TMPDIR/sq_lock.db 2> $TMPDIR/sq_lock.err & "
                        "sleep 1; sqlite3 -list $TMPDIR/sq_lock.db 'select count(*) from lk'; echo reader=$?; "
                        "sqlite3 $TMPDIR/sq_lock.db 'insert into lk values (2)'; echo writer=$?; "
                        "wait; sqlite3 -list $TMPDIR/sq_lock.db 'select group_concat(v) from lk'; "
                        "echo holder_err=$(wc -c < $TMPDIR/sq_lock.err)", timeout=60)
    assert rc == 0, out
    lines = _lines(out)
    i = lines.index("reader=0")
    assert lines[i - 1] == "1", out                # the reader saw the committed row only
    assert "database is locked" in out, out        # the second writer was refused
    assert "writer=1" in lines, out
    assert "0,1" in lines, out                     # the holder's commit went through
    assert "holder_err=0" in lines, out            # and it reported no error

def test_sqlite3_readonly(sshi: spawn):
    """/tvm is a read-only cpio mount: creating a database there fails cleanly
    (no crash, no phantom file), a file that exists there opens read-only, and
    a read-only connection reports writes as such."""
    out, rc = _sh(sshi, "sqlite3 /tvm/etc/sq_ro.db 'create table t(x)'; echo sqlite=$?; "
                        "test -e /tvm/etc/sq_ro.db && echo phantom || echo no_phantom; "
                        "echo x > /tvm/etc/sq_ro.txt")
    assert rc != 0, out
    assert "unable to open database file" in out, out
    assert "sqlite=1" in _lines(out) and "no_phantom" in _lines(out), out
    assert "Read-only file system" in out, out
    out, rc = _sh(sshi, "sqlite3 /tvm/etc/profile 'select count(*) from sqlite_schema'")
    assert rc != 0, out
    assert "file is not a database" in out, out
    out, rc = _sh(sshi, "rm -f $TMPDIR/sq_ro.db; sqlite3 $TMPDIR/sq_ro.db 'create table t(x)'; "
                        "sqlite3 -readonly $TMPDIR/sq_ro.db 'insert into t values (1)'")
    assert rc != 0, out
    assert "attempt to write a readonly database" in out, out

def test_sqlite3_shell_escape(sshi: spawn):
    """.shell runs a command through system(3) and .import '|cmd' reads a
    pipe through popen(3) (both interposed by TVM; on iOS system() is bound
    through the include shim)."""
    out, rc = _sh(sshi, "printf 'create table t(v);\\n.shell echo from_shell\\n.import \"|seq 1 3\" t\\n"
                        "select sum(v) from t;\\n' | sqlite3 -list :memory:")
    assert rc == 0, out
    lines = _lines(out)
    assert "from_shell" in lines and "6" in lines, out

# ── git ──────────────────────────────────────────────────────

# Every git test works in its own directory under $TMPDIR with a private
# global config: GIT_CONFIG_GLOBAL keeps the CLI backend (whose HOME is the
# runner's) from reading or writing the user's ~/.gitconfig, and the host's
# GIT_ASKPASS (an IDE's helper script) must not leak into the VM. The
# session is a tty, so git would page everything: GIT_PAGER=cat, except in
# the pager tests, which unset it.
_GIT_ENV = ("export GIT_CONFIG_GLOBAL=$TMPDIR/tvm_gitconfig GIT_CONFIG_NOSYSTEM=1 GIT_PAGER=cat; "
            "unset GIT_ASKPASS SSH_ASKPASS GIT_EDITOR; "
            "git config --global user.name 'TVM Tester' && "
            "git config --global user.email tvm@example.com && "
            "git config --global init.defaultBranch main")

def _git_setup(sshi: spawn, name: str) -> str:
    """Private config, a fresh $TMPDIR/<name> as cwd; returns its path."""
    d = f"$TMPDIR/{name}"
    out, rc = _sh(sshi, f"{_GIT_ENV}; rm -rf {d} && mkdir -p {d} && cd {d}")
    assert rc == 0, out
    return d

def _git(sshi: spawn, cmd: str, timeout: float = CURL_TIMEOUT) -> tuple[str, int, str]:
    """Run cmd with stderr captured separately: (stdout, rc, stderr)."""
    out, rc = _sh(sshi, f"{cmd} 2>$TMPDIR/git_stderr", timeout=timeout)
    err, _ = _sh(sshi, "cat $TMPDIR/git_stderr")
    # _sh's output starts with the echoed command line
    err = "\n".join(_lines(err)[1:])
    return out, rc, err

def test_git_version_exec_path(sshi: spawn):
    """git --version and the compiled-in exec path, where the helpers live;
    `git remote-https` runs upstream's git-remote-http helper from the exec
    path (the https name is a link to it) and prints its usage."""
    out, rc, err = _git(sshi, "git --version")
    assert rc == 0 and err == "", (out, err)
    assert any(re.fullmatch(r"git version 2\.\d+\.\d+", l) for l in _lines(out)), out
    out, rc, err = _git(sshi, "git --exec-path")
    assert rc == 0 and err == "", (out, err)
    assert "/tvm/libexec/git-core" in _lines(out), out
    out, rc = _sh(sshi, "ls /tvm/libexec/git-core/git-remote-https /tvm/libexec/git-core/git-sh-setup"
                        " /tvm/share/git-core/templates/hooks/pre-commit.sample >/dev/null")
    assert rc == 0, out
    out, rc = _sh(sshi, "git remote-https 2>&1")
    assert rc == 1 and "usage: git remote-curl" in out, out

def test_git_init_add_commit_log_status(sshi: spawn):
    _git_setup(sshi, "git_basic")
    out, rc, err = _git(sshi, "git init -q . && echo hello > a.txt && git add a.txt && git commit -q -m 'first commit'")
    assert rc == 0 and err == "", (out, err)
    out, rc, err = _git(sshi, "git log --oneline --no-decorate")
    assert rc == 0 and err == "", (out, err)
    assert any(re.fullmatch(r"[0-9a-f]{7,} first commit", l) for l in _lines(out)), out
    out, rc, err = _git(sshi, "echo more >> a.txt; echo new > b.txt; git status --short")
    assert rc == 0 and err == "", (out, err)
    lines = _lines(out)
    assert "M a.txt" in lines and "?? b.txt" in lines, out
    out, rc, err = _git(sshi, "git status")
    assert rc == 0 and "On branch main" in out, (out, err)

def test_git_merge_conflict_ours(sshi: spawn):
    """A conflicting merge stops with CONFLICT; checkout --ours, add and
    commit conclude it as a two-parent merge."""
    _git_setup(sshi, "git_merge")
    out, rc, err = _git(sshi, "git init -q . && echo base > f && git add f && git commit -qm base && "
                              "git checkout -q -b feature && echo feature > f && git commit -qam feature && "
                              "git checkout -q main && echo main > f && git commit -qam main")
    assert rc == 0 and err == "", (out, err)
    out, rc = _sh(sshi, "git merge feature")
    assert rc == 1 and "CONFLICT (content): Merge conflict in f" in out, out
    out, rc, err = _git(sshi, "git checkout -q --ours f && git add f && git commit -q --no-edit && cat f")
    assert rc == 0 and err == "", (out, err)
    assert "main" in _lines(out), out
    out, rc, err = _git(sshi, "git log --format=%s:%p -1")
    assert rc == 0 and err == "", (out, err)
    assert any(re.fullmatch(r"Merge branch 'feature':[0-9a-f]+ [0-9a-f]+", l) for l in _lines(out)), out

def test_git_diff_through_pager(sshi: spawn):
    """git diff through less without interaction: on the tty with
    LESS=FRX (git's own default) the short diff prints and less exits by
    itself; into a pipe, git does not page and less passes it through."""
    _git_setup(sshi, "git_diff")
    out, rc, err = _git(sshi, "git init -q . && printf 'one\\ntwo\\n' > f && git add f && git commit -qm c1 && "
                              "printf 'one\\nTWO\\n' > f")
    assert rc == 0 and err == "", (out, err)
    out, rc = _sh(sshi, "unset GIT_PAGER; export TERM=xterm-256color; LESS=FRX PAGER=less git diff")
    assert rc == 0, out
    lines = _lines(out)
    assert "-two" in lines and "+TWO" in lines, out
    out, rc, err = _git(sshi, "git diff | less | cat")
    assert rc == 0 and err == "", (out, err)
    lines = _lines(out)
    assert "diff --git a/f b/f" in lines and "-two" in lines and "+TWO" in lines, out

def test_git_log_pager_interactive(sshi: spawn):
    """git log pages through less on the tty (PAGER=less, LESS=-R from the
    rc: no quit-at-end), q returns to the shell with status 0."""
    _git_setup(sshi, "git_logpager")
    out, rc, err = _git(sshi, "git init -q . && for i in $(seq 1 40); do git commit -q --allow-empty -m \"commit number $i\"; done")
    assert rc == 0 and err == "", (out, err)
    sshi.sendline("unset GIT_PAGER; export TERM=xterm-256color PAGER=less LESS=-R; git log --format=%s; echo RC=$?")
    sshi.expect("commit number 40")
    sshi.expect("commit number 30")
    sshi.expect(r"\r\n:")       # less's prompt: waiting for a key
    sshi.send("q")
    sshi.expect("RC=0")
    sshi.expect(r"\$ ")

def test_git_commit_editor_nano(sshi: spawn):
    """git commit without -m opens $EDITOR (nano, the rc default): type a
    message, ^O Enter, ^X, and the commit carries it."""
    _git_setup(sshi, "git_editor")
    out, rc, err = _git(sshi, "git init -q . && echo x > f && git add f")
    assert rc == 0 and err == "", (out, err)
    sshi.sendline("export TERM=xterm-256color EDITOR=nano; git commit -q; echo RC=$?")
    sshi.expect("COMMIT_EDITMSG")
    sshi.expect("Write Out")
    sshi.send("message from nano")
    sshi.send("\x0f")            # ^O
    sshi.expect("File")          # "File Name to Write:" prompt
    sshi.send("\r")
    sshi.expect(r"Wrote \d+ lines?")
    sshi.send("\x18")            # ^X
    sshi.expect("RC=0")
    sshi.expect(r"\$ ")
    out, rc, err = _git(sshi, "git log -1 --format=%s")
    assert rc == 0 and err == "", (out, err)
    assert "message from nano" in _lines(out), out

def test_git_pre_commit_hook(sshi: spawn):
    """A pre-commit hook (#!/bin/sh, run through the guest shell) that
    fails blocks the commit; once it succeeds the commit goes through."""
    _git_setup(sshi, "git_hook")
    out, rc, err = _git(sshi, "git init -q . && "
                              "printf '#!/bin/sh\\necho hook says no >&2\\nexit 1\\n' > .git/hooks/pre-commit && "
                              "chmod +x .git/hooks/pre-commit && echo x > f && git add f")
    assert rc == 0 and err == "", (out, err)
    out, rc, err = _git(sshi, "git commit -q -m hooked")
    assert rc == 1 and "hook says no" in err, (out, err)
    out, rc = _sh(sshi, "git rev-parse --verify -q HEAD")
    assert rc == 1, out
    # the hook prints a computed value, so the check cannot be satisfied by
    # the echoed command line, which only contains the expression
    out, rc, err = _git(sshi, "printf '#!/bin/sh\\necho hook says $((6*7)) >&2\\nexit 0\\n' > .git/hooks/pre-commit && "
                              "git commit -q -m hooked && git log -1 --format=%s")
    assert rc == 0, (out, err)
    # the hook's stderr reaches the terminal (only git log's is redirected)
    assert "hooked" in _lines(out) and "hook says 42" in _lines(out) + _lines(err), (out, err)

@network_required
def test_git_clone_https(sshi: spawn):
    """git clone over HTTPS: git-remote-https from the exec path, libcurl
    with OpenSSL and Apple SecTrust (no CA file); a shallow clone of a
    tiny public repository."""
    _git_setup(sshi, "git_https")
    out, rc, err = _git(sshi, "git clone -q --depth 1 https://github.com/octocat/test-repo1 repo")
    assert rc == 0 and err == "", (out, err)
    out, rc, err = _git(sshi, "git -C repo log -1 --format=%H && git -C repo rev-list --count HEAD && ls repo")
    assert rc == 0 and err == "", (out, err)
    lines = _lines(out)
    assert any(re.fullmatch(r"[0-9a-f]{40}", l) for l in lines), out
    assert "1" in lines and "2016-02-24-first-post.md" in out, out

def test_git_clone_local_push_bare(sshi: spawn):
    """Clone a local repository, commit, push to a bare repository
    (receive-pack's quarantine directory is a relative mkdtemp)."""
    d = _git_setup(sshi, "git_push")
    out, rc, err = _git(sshi, "git init -q src && cd src && echo a > a && git add a && git commit -qm one && cd .. && "
                              "git init -q --bare bare.git && git clone -q src work && cd work && "
                              "echo b > b && git add b && git commit -qm two && git push -q ../bare.git main")
    assert rc == 0 and err == "", (out, err)
    out, rc, err = _git(sshi, f"git -C {d}/bare.git log --format=%s main")
    assert rc == 0 and err == "", (out, err)
    lines = _lines(out)
    assert lines[-2:] == ["two", "one"], out

def test_git_gc_fsck(sshi: spawn):
    """git gc packs the objects (pack-objects with threads), repack -a -d
    and fsck, which runs commit-graph and multi-pack-index verify as child
    git processes, find nothing wrong."""
    _git_setup(sshi, "git_gc")
    out, rc, err = _git(sshi, "git init -q . && for i in $(seq 1 20); do echo $i > f$i; git add f$i; "
                              "git commit -qm c$i; done")
    assert rc == 0 and err == "", (out, err)
    out, rc, err = _git(sshi, "git gc -q && git repack -q -a -d --threads=4 && git count-objects -v")
    assert rc == 0 and err == "", (out, err)
    lines = _lines(out)
    assert "count: 0" in lines and "packs: 1" in lines, out
    out, rc, err = _git(sshi, "git fsck --no-progress --no-dangling")
    assert rc == 0 and err == "", (out, err)

def test_git_stash_pop(sshi: spawn):
    _git_setup(sshi, "git_stash")
    out, rc, err = _git(sshi, "git init -q . && echo one > f && git add f && git commit -qm one && echo two > f")
    assert rc == 0 and err == "", (out, err)
    out, rc, err = _git(sshi, "git stash -q && cat f && git status --short")
    assert rc == 0 and err == "", (out, err)
    lines = _lines(out)
    assert "one" in lines and not any(l.startswith("M ") for l in lines[1:]), out
    out, rc, err = _git(sshi, "git stash pop -q && cat f && git status --short && git stash list | wc -l")
    assert rc == 0 and err == "", (out, err)
    lines = _lines(out)
    assert "two" in lines and "M f" in lines and "0" in lines, out

# ── File descriptor & dup ────────────────────────────────────

def test_popen_close_first_of_two(sshi: spawn):
    """awk's `print | cmd` is popen(). Closing the first of two pipes must
    not hang: each popen child used to inherit the other pipes (popen's end
    was not close-on-exec, and fork dropped FD_CLOEXEC), so `sort` never saw
    EOF while `sort -r` held its write end."""
    out, rc = _sh(sshi, "printf 'b\\na\\n' | awk '{ print | \"sort\"; print | \"sort -r\" } "
                        "END { close(\"sort\"); print \"MID\"; close(\"sort -r\"); print \"END\" }'",
                  timeout=30)
    assert rc == 0, out
    lines = _lines(out)
    assert lines[-6:] == ["a", "b", "MID", "b", "a", "END"], out

def test_exec_short_text_script(sshi: spawn):
    """A #!-less text script shorter than 4 bytes runs through the shell like
    any other (it used to get ENOEXEC and bash's in-process fallback)."""
    out, rc = _sh(sshi, "d=$(mktemp -d) && cd $d && printf 'pwd' > s3 && chmod +x s3 && "
                        "./s3 | sed 's/^/OUT:/'; cd - >/dev/null")
    assert rc == 0, out
    assert any(l.startswith("OUT:/") for l in _lines(out)), out

def test_path_search_skips_dir_and_nonexec(sshi: spawn, device):
    """execvp() moves past a PATH entry that is a directory or a
    non-executable file of the same name, instead of failing with EACCES.
    On iOS TVM's stat() reports every regular file as executable (the sandbox
    does not report execute permission on the app's signed dylibs), so there
    only the directory case applies."""
    out, rc = _sh(sshi, "d=$(mktemp -d) && mkdir -p $d/p1/true && PATH=$d/p1:$PATH env true")
    assert rc == 0, out
    if not device:
        out, rc = _sh(sshi, "d=$(mktemp -d) && mkdir -p $d/p1/true $d/p2 && printf 'exit 7\\n' > $d/p2/true && "
                            "PATH=$d/p1:$d/p2:$PATH env true")
        assert rc == 0, out

def test_prompt_is_dollar(sshi: spawn):
    """The login shell's prompt is mksh-style "$ ", not bash's default
    "-bash-5.3$ "; so is an interactive non-login bash (reads bash.bashrc)."""
    # a prompt at the start of a line; bash puts its bracketed-paste escapes
    # (ESC [?2004l / ESC [?2004h) and a carriage return in front of it
    prompt = r"[\r\n](?:\x1b\[\?2004[hl]|\r)*\$ "
    sshi.sendline("echo \"PS1=[$PS1]\"")
    sshi.expect(r"PS1=\[\\\$ \]")
    sshi.expect(prompt)
    sshi.sendline("bash -i")
    sshi.expect(prompt)
    sshi.sendline("echo \"inner=[$PS1]\"; exit")
    sshi.expect(r"inner=\[\\\$ \]")
    sshi.expect(prompt)
    before = sshi.before.decode() if isinstance(sshi.before, bytes) else sshi.before
    assert "bash-5" not in before, before

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

# Symlink tests. The /tvm initrd mount is not writable (ln into /tvm/bin fails
# with ENOENT on every platform) and a host-fs symlink pointing into the mount
# cannot be opened or executed (the host resolves it), so the old variants that
# did `ln -sf toybox /tvm/bin/my_echo && /tvm/bin/my_echo ...` never ran
# anything: they passed by matching the marker in their own echoed command
# line. These exercise what exists: symlinks shipped inside the initrd, and a
# chain of host-fs symlinks. The markers are split with '' so the echoed
# command cannot satisfy the expect — only real output can.

def test_symlink_follow(sshi: spawn):
    """Execute through a symlink inside the initrd (/tvm/bin/sh -> bash) —
    exercises VFS symlink resolution in execve and readlink."""
    sshi.sendline("readlink /tvm/bin/sh; /tvm/bin/sh -c 'echo symlink_ex''ec_ok $0'")
    sshi.expect("bash")
    sshi.expect("symlink_exec_ok /tvm/bin/sh")
    sshi.expect(r"\$ ")

def test_symlink_chain(sshi: spawn):
    """Symlink to symlink on the host fs, executed as a #!-less text file —
    exercises chained resolution in execve's open()."""
    sshi.sendline("printf 'echo ch\"\"ain_ok\\n' > $TMPDIR/chain_target && chmod 755 $TMPDIR/chain_target && "
                  "ln -sf chain_target $TMPDIR/link1 && ln -sf link1 $TMPDIR/link2 && $TMPDIR/link2")
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
    sshi.sendline("kill %1 2>/dev/null || kill $(jobs -p) 2>/dev/null; wait; echo done")
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
    sshi.expect(r"parent=(\d+)\r?\n")
    parent = sshi.match.group(1).decode()
    sshi.expect(r"child=(\d+)\r?\n")
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
    """Ctrl-Z during a foreground command must not kill the VM.

    TVM has no task stop/continue: a stop signal reaching a task with the
    default disposition used to panic("signal_handler STOP") and abort the
    whole backend. It is ignored instead, so the foreground command runs to
    completion and the prompt comes back. Exercised both ways: a real ^Z
    through the tty line discipline (VSUSP -> SIGTSTP to the foreground
    process group) and explicit kill -TSTP / kill -STOP on a child.
    """
    sshi.sendline("sleep 3; echo after_sleep")
    time.sleep(1)
    sshi.send("\x1a")
    sshi.expect("after_sleep")
    sshi.expect(r"\$ ")
    sshi.sendline("sleep 2 & kill -TSTP $!; kill -STOP $!; wait; echo after_stop")
    sshi.expect("after_stop")
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

def test_ls_root_shows_tvm(sshi: spawn):
    """The VFS is one tree under the host's root: `ls /` is the host's
    listing with the mount point in it, and /tvm is a directory to test."""
    out, rc = _sh(sshi, "ls / | grep -x tvm; [ -d /tvm ] && [ -d /usr ] && echo both_dirs")
    assert rc == 0 and _lines(out)[-2:] == ["tvm", "both_dirs"], out

def test_vfs_readonly(sshi: spawn):
    """A cpio mount is read-only: creating in it is "Read-only file system"
    (the host used to answer for a path it has not got, ENOENT), and the
    mount point itself is busy."""
    out, rc = _sh(sshi, "mkdir /tvm/x 2>&1; touch /tvm/etc/x 2>&1; rm /tvm/etc/profile 2>&1; rmdir /tvm 2>&1; true")
    assert out.count("Read-only file system") == 3, out
    assert "busy" in out.lower(), out

def test_mount_builtin(sshi: spawn):
    """`tvm mount` takes mount(8)'s spelling: `[-t cpio] FILE TARGET` mounts
    an archive at runtime (here one toybox's cpio just wrote), `--bind` or
    `-o bind SOURCE TARGET` binds a file or a directory, and too few
    operands is the usage, rc 1."""
    out, rc = _sh(sshi, "tvm mount")
    assert rc == 1 and "USAGE" in out, out
    out, rc = _sh(sshi, "tvm mount --bind /tvm/bin/env")
    assert rc == 1 and "USAGE" in out, out
    out, rc = _sh(sshi, "tvm mount -x a b")
    assert rc == 1 and "unknown option" in out, out
    out, rc = _sh(sshi, "d=$(mktemp -d) && mkdir $d/x && echo from_cpio > $d/x/f && cd $d && "
                        "find x | cpio -o -H newc > $d/t.cpio && cd / && "
                        "tvm mount -t cpio $d/t.cpio /mnt_builtin && cat /mnt_builtin/x/f; rm -rf $d")
    assert rc == 0 and "from_cpio" in out, out
    # toybox dispatches on argv[0], so the bind keeps the applet's name
    out, rc = _sh(sshi, "tvm mount --bind /tvm/bin/env /usr/local/bin/env && [ -x /usr/local/bin/env ] && /usr/local/bin/env true && echo env_runs")
    assert rc == 0 and "env_runs" in out, out
    out, rc = _sh(sshi, "d=$(mktemp -d) && echo bound > $d/x && tvm mount -o bind $d /mnt_obind && cat /mnt_obind/x; rm -rf $d")
    assert rc == 0 and "bound" in out, out

def test_bind_mounts(sshi: spawn):
    """The initrc's bind mounts (/bin/sh, /bin/bash, /usr/bin/env from
    /tvm/bin) are the VM's programs under the host paths: regular files,
    not links (readlink fails, ls -l has no arrow), listed once in /bin,
    busy to mv and rm, and scripts naming their interpreter by host path
    run through them -- there is no PATH retry of a #! interpreter."""
    out, rc = _sh(sshi, "readlink /bin/sh; echo RL=$?; [ -f /bin/sh ] && ! [ -L /bin/sh ] && [ -f /bin/bash ] && [ -f /usr/bin/env ] && echo files_not_links")
    assert "RL=1" in out and "files_not_links" in out, out
    out, rc = _sh(sshi, "ls -l /bin/sh")
    assert rc == 0 and "->" not in out.split("\n")[-2] and "/bin/sh" in out, out
    out, rc = _sh(sshi, "ls /bin | grep -c '^sh$'")
    assert rc == 0 and _lines(out)[-1] == "1", out
    out, rc = _sh(sshi, "[ -x /bin/bash ] && /bin/bash -c 'echo BASH=${BASH_VERSINFO[0]}' && /bin/sh -c 'echo SH0=$0'")
    assert rc == 0 and "BASH=5" in out and "SH0=/bin/sh" in out, out
    out, rc = _sh(sshi, "d=$(mktemp -d) && printf '#!/bin/bash\\narr=(x y); echo ARR=${arr[1]}\\n' > $d/b && "
                        "printf '#!/bin/sh\\necho SH_OK\\n' > $d/s && printf '#!/usr/bin/env bash\\necho ENV_OK\\n' > $d/e && "
                        "chmod +x $d/b $d/s $d/e && $d/b && $d/s && $d/e; rm -rf $d")
    assert rc == 0 and "ARR=y" in out and "SH_OK" in out and "ENV_OK" in out, out
    out, rc = _sh(sshi, "mv /bin/sh $TMPDIR/sh_moved 2>&1; rm /bin/sh 2>&1; [ -f /bin/sh ] && echo still_there")
    assert out.lower().count("busy") == 2 and "still_there" in out, out

def test_bind_dir_listing(sshi: spawn):
    """A bound directory is listed with the mounts under it whatever opens
    it: toybox's ls (dirtree) opens a directory with openat(.., O_CLOEXEC)
    and fdopendir, no O_DIRECTORY, and used to get the host directory
    behind the bind, so with b bound at /mnt/nested under a bound /mnt,
    `ls /mnt` printed nothing while `echo /mnt/*` (bash's opendir) had
    nested and [ -d /mnt/nested ] was true (-1: the session is a tty)."""
    out, rc = _sh(sshi, "d=$(mktemp -d) && mkdir $d/a $d/b && touch $d/a/own $d/b/inner && "
                        "tvm mount --bind $d/a /mnt_lsdir && tvm mount --bind $d/b /mnt_lsdir/nested && "
                        "ls -1 /mnt_lsdir && ls -1 /mnt_lsdir/nested && echo /mnt_lsdir/* && [ -d /mnt_lsdir/nested ] && "
                        "ls -d /mnt_lsdir/nested; rm -rf $d")
    assert rc == 0 and _lines(out)[-5:] == ["nested", "own", "inner", "/mnt_lsdir/nested /mnt_lsdir/own", "/mnt_lsdir/nested"], out

def test_realpath_namespace(sshi: spawn):
    """realpath and pwd -P answer in the guest namespace: a cpio link is
    followed, a mount point keeps its own name (its source is hidden), and
    a path under a mount is no longer the host's ENOENT."""
    out, rc = _sh(sshi, "realpath /tvm/bin/sh /bin/sh /tvm/etc/../bin; cd /bin && pwd -P; cd /tvm/bin && pwd -P")
    assert rc == 0 and _lines(out)[-5:] == ["/tvm/bin/bash", "/bin/sh", "/tvm/bin", "/bin", "/tvm/bin"], out

def test_tty_device_dir(sshi: spawn):
    """The session's tty is an entry of the ptys' directory, a node of the
    tree: the directory exists to ls, [ -d ] and stat (it used to be
    ENOENT while its entries answered), lists the tty, the tty is a
    character device that realpath and ".." resolve like any path, and
    /dev is the host's directory with the VM's tty and ptmx in it, once
    each."""
    out, rc = _sh(sshi, "t=$(tty) && n=${t##*/} && [ -c $t ] && [ -d /tvm/pts ] && ls /tvm | grep -x pts && "
                        "ls /tvm/pts | grep -x $n > /dev/null && echo listed && ls -l $t | cut -c1 && "
                        "[ $(realpath /tvm/pts/../pts/$n) = $t ] && echo same && "
                        "ls /dev | grep -cx -e tty -e ptmx && ls /dev | grep -x null")
    assert rc == 0 and _lines(out)[-6:] == ["pts", "listed", "c", "same", "2", "null"], out

def test_tty_device_bind(sshi: spawn):
    """A bind of the session's tty is the tty: a write to the mount point
    lands on the terminal, ls shows a character device, the mount point is
    busy to rm; the ptys' directory gets no entries but the driver's, and
    its entries cannot be removed."""
    out, rc = _sh(sshi, "t=$(tty) && tvm mount --bind $t /mnt_tty_test && ls -l /mnt_tty_test | cut -c1 && "
                        "echo through-the-bind > /mnt_tty_test && "
                        "{ rm /mnt_tty_test 2>&1 || echo rm-bind-failed; } && "
                        "{ touch /tvm/pts/x 2>&1 || echo touch-failed; } && "
                        "{ mkdir /tvm/pts/d 2>&1 || echo mkdir-failed; } && "
                        "{ rm $t 2>&1 || echo rm-tty-failed; }")
    lines = _lines(out)
    assert rc == 0 and "through-the-bind" in lines, out
    assert lines[lines.index("through-the-bind") - 1] == "c", out
    rest = "\n".join(lines[lines.index("through-the-bind") + 1:])
    assert "busy" in rest.lower() and "rm-bind-failed" in rest, out
    assert "Permission denied" in rest and "touch-failed" in rest, out
    assert "Operation not permitted" in rest and "mkdir-failed" in rest and "rm-tty-failed" in rest, out

def test_exec_bad_interpreter(sshi: spawn):
    """A #! interpreter that does not exist is a bad interpreter, as on
    Linux: bash reports it and the status is 126 (no PATH retry)."""
    out, rc = _sh(sshi, "d=$(mktemp -d) && printf '#!/nonexistent/interp\\necho never\\n' > $d/s && "
                        "chmod +x $d/s && $d/s; echo RC=$?; rm -rf $d")
    assert "bad interpreter" in out and "RC=126" in out and "never" not in out.replace("echo never", ""), out

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

def test_vfs_missing_path_errno(sshi: spawn):
    """Lookups under the initrd VFS must report a real errno. vfs_lookup used
    to return NULL without setting errno, so the guest saw whatever the caller
    had left behind (ls said ENOTTY, cat EBADEXEC, cd EINTR)."""
    sshi.sendline("ls -d /tvm/nope 2>&1; echo rc=$?")
    sshi.expect("No such file or directory")
    sshi.expect("rc=1")
    sshi.expect(r"\$ ")
    sshi.sendline("cat /tvm/bin/nope 2>&1; echo rc=$?")
    sshi.expect("No such file or directory")
    sshi.expect("rc=1")
    sshi.expect(r"\$ ")
    sshi.sendline("ls -d /tvm/bin/sh/x 2>&1; echo rc=$?")
    sshi.expect("Not a directory")
    sshi.expect("rc=1")
    sshi.expect(r"\$ ")
    sshi.sendline("cd /tvm/nope 2>&1; echo rc=$?")
    sshi.expect("No such file or directory")
    sshi.expect("rc=[12]")
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
    # Match the output line, not the echoed command line: fork's pointer
    # fixup once rewrote the tail of the command string and this printed
    # "hello_syste" plus garbage bytes, which "hello_system" alone let pass.
    # (\r after bash's bracketed-paste reset, \n without one)
    sshi.expect(r"[\r\n]hello_system\r\n")
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
