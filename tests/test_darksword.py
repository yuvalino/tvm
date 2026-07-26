"""
Darksword kernel exploit integration test.

Runs only on iOS devices. The exploit runs as a signed dylib
(libdarksword.signed.dylib) in the app bundle via TVM_NOEMU —
no transfer needed, native execution, code-signed.

Orchestrates:
  1. Restart device for clean kernel state
  2. Install + launch TVM app
  3. Execute darksword.signed and stream output
  4. Verify sandbox escape succeeded + clean exit

Usage:
    pytest tests/test_darksword.py --device pan1c
"""

import logging
import select
import subprocess
import tempfile
import time

import pytest

from panic_parser import parse_panic_ips, format_panic_summary

logger = logging.getLogger(__name__)

IOS_APP = "./output/ios-arm64/lib/flutter"
IOS_BUNDLE_ID = "com.yuvalino.tvm"
SSH_PORT = 2222
SSH_PASS = "alpine"
DARKSWORD_CMD = "darksword.signed"


def _run(cmd, **kwargs):
    kwargs.setdefault("capture_output", True)
    kwargs.setdefault("text", True)
    kwargs.setdefault("timeout", 30)
    logger.debug("$ %s", " ".join(cmd) if isinstance(cmd, list) else cmd)
    return subprocess.run(cmd, **kwargs)


def _ssh(device, cmd, timeout=30):
    addr = device if "." in device else f"{device}.local"
    return _run(
        ["sshpass", "-p", SSH_PASS, "ssh",
         "-o", "StrictHostKeyChecking=no",
         "-o", f"ConnectTimeout={min(timeout, 10)}",
         "-p", str(SSH_PORT),
         f"mobile@{addr}", cmd],
        timeout=timeout,
    )


def _wait_ssh(device, timeout=30):
    addr = device if "." in device else f"{device}.local"
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            r = _ssh(device, "echo ok", timeout=5)
            if r.returncode == 0:
                return True
        except (subprocess.TimeoutExpired, Exception):
            pass
        time.sleep(1)
    return False


def _wait_device(device, timeout=120):
    """Wait until the device is up, the app launches, and SSH is reachable."""
    deadline = time.time() + timeout
    # phase 1: wait for device to appear
    while time.time() < deadline:
        try:
            r = _run(
                ["xcrun", "devicectl", "device", "info", "details",
                 "--device", device],
                timeout=10,
            )
            if r.returncode == 0 and "iPhone" in (r.stdout or ""):
                break
        except subprocess.TimeoutExpired:
            pass
        time.sleep(3)
    else:
        return False
    # phase 2: launch app and wait for SSH
    while time.time() < deadline:
        try:
            r = _run(
                ["xcrun", "devicectl", "device", "process", "launch",
                 "--terminate-existing", "--device", device, IOS_BUNDLE_ID],
                timeout=30,
            )
            if r.returncode == 0 and _wait_ssh(device, timeout=min(15, deadline - time.time())):
                return True
        except subprocess.TimeoutExpired:
            pass
        time.sleep(5)
    return False


def _device_reboot(device, timeout=240):
    """Restart app for clean state."""
    logger.info("Restarting app on %s...", device)
    assert _wait_device(device, timeout), f"Device {device} not reachable"
    logger.info("Device %s ready", device)


def _launch_app(device):
    """Launch TVM app and wait for SSH."""
    _run(
        ["xcrun", "devicectl", "device", "process", "launch",
         "--terminate-existing", "--device", device, IOS_BUNDLE_ID],
        timeout=30,
    )
    assert _wait_ssh(device, timeout=30), "SSH did not come up after app launch"


DARKSWORD_SILENCE_TIMEOUT = 30

def _run_darksword(device, timeout=90):
    """Run darksword and stream output line by line. Returns (lines, alive).

    Detects likely panic/crash if no output arrives for DARKSWORD_SILENCE_TIMEOUT
    seconds — the exploit prints progress continuously, so silence means the
    device died or the process exited.
    """
    addr = device if "." in device else f"{device}.local"
    proc = subprocess.Popen(
        ["sshpass", "-p", SSH_PASS, "ssh",
         "-o", "StrictHostKeyChecking=no",
         "-o", "ConnectTimeout=10",
         "-p", str(SSH_PORT),
         f"mobile@{addr}", DARKSWORD_CMD],
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
        text=True,
    )

    lines = []
    try:
        last_line_time = time.time()
        deadline = time.time() + timeout
        while time.time() < deadline:
            ready, _, _ = select.select([proc.stdout], [], [], 1.0)
            if ready:
                line = proc.stdout.readline()
                if not line:
                    break
                last_line_time = time.time()
                line = line.rstrip()
                if not line:
                    continue
                if "WARNING" in line or "quantum" in line or "upgraded" in line or "openssh" in line:
                    continue
                lines.append(line)
                logger.info("darksword: %s", line)
                if "DONE" in line:
                    proc.kill()
                    break
            elif time.time() - last_line_time >= DARKSWORD_SILENCE_TIMEOUT:
                logger.warning("No output for %ds — likely panic or crash",
                               int(time.time() - last_line_time))
                proc.kill()
                break
    except Exception:
        proc.kill()
    finally:
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait()

    # check device is still alive
    time.sleep(2)
    alive = False
    try:
        r = _ssh(device, "echo alive", timeout=5)
        alive = r.returncode == 0
    except Exception:
        pass

    return lines, alive


def _pull_latest_panic(device, timeout=60):
    """Wait for panic log to appear after reboot, pull and parse it.

    Panic logs can take up to ~60s to be written after the device reboots.
    Returns parsed panic dict or None.
    """
    logger.info("Waiting for panic log on device (up to %ds)...", timeout)
    if not _wait_device(device, timeout=90):
        logger.warning("Device did not come back")
        return None

    # poll for new panic log (may take a while after reboot)
    latest_name = None
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            r = _run(
                ["xcrun", "devicectl", "device", "info", "files",
                 "--device", device, "--domain-type", "systemCrashLogs"],
                timeout=30,
            )
            if r.returncode == 0 and r.stdout:
                panics = [l.split()[0] for l in r.stdout.splitlines()
                          if "panic-full" in l]
                if panics:
                    latest_name = panics[-1]
                    break
        except (subprocess.TimeoutExpired, Exception):
            pass
        time.sleep(5)

    if not latest_name:
        logger.warning("No panic log found")
        return None

    # pull the file
    with tempfile.NamedTemporaryFile(suffix=".ips", delete=False) as tmp:
        dst = tmp.name
    try:
        r = _run(
            ["xcrun", "devicectl", "device", "copy", "from",
             "--device", device, "--domain-type", "systemCrashLogs",
             "--source", latest_name, "--destination", dst],
            timeout=30,
        )
        if r.returncode != 0:
            logger.warning("Failed to pull panic log: %s", r.stderr)
            return None

        parsed = parse_panic_ips(dst)
        return parsed
    except Exception as e:
        logger.warning("Failed to parse panic log: %s", e)
        return None


# ── Fixtures ──────────────────────────────────────────────────

@pytest.fixture(scope="module")
def ios_device(request):
    device = request.config.getoption("--device")
    if not device:
        pytest.skip("--device required for darksword tests")
    return device


@pytest.fixture(scope="module", autouse=True)
def clean_device_state(ios_device):
    """Restart device before the test module for clean kernel state."""
    _device_reboot(ios_device)


@pytest.fixture(autouse=True)
def ensure_app_running(ios_device):
    """Ensure TVM app is launched and SSH is accessible before each test."""
    if not _wait_ssh(ios_device, timeout=5):
        _launch_app(ios_device)


# ── Tests ─────────────────────────────────────────────────────

def test_darksword_sandbox_escape(ios_device):
    """Run darksword exploit and verify sandbox escape + clean exit."""
    lines, alive = _run_darksword(ios_device, timeout=90)

    output = "\n".join(lines)

    # check exploit stages completed
    assert "controlSocket" in output, "Exploit did not find control socket"
    assert "controlSocketPcb" in output, "KRW not established"

    # check sandbox escape
    assert "SANDBOX ESCAPED" in output, f"Sandbox escape failed. Output:\n{output}"

    # check filesystem access beyond app container
    for path in ["/var/mobile/Library/AddressBook", "/var/root", "/var/db"]:
        assert f"opendir({path}): OK" in output, \
            f"Filesystem access denied for {path}. Output:\n{output}"

    # SMS and Safari are TCC-protected — extension zeroing can't reach them
    for path in ["/var/mobile/Library/SMS", "/var/mobile/Library/Safari"]:
        if f"opendir({path}): OK" in output:
            logger.info("Bonus: %s also unlocked", path)

    # check device survived (no kernel panic)
    if not alive:
        panic_info = _pull_latest_panic(ios_device, timeout=60)
        panic_summary = ""
        if panic_info:
            panic_summary = format_panic_summary(panic_info)
            for line in panic_summary.splitlines():
                logger.error("PANIC: %s", line)
        pytest.fail(
            f"Device panicked after exploit.\n"
            f"--- Exploit output ---\n{output}\n"
            f"--- Panic log ---\n{panic_summary or '(not available)'}"
        )

    assert "DONE" in output, f"Exploit did not complete. Output:\n{output}"
