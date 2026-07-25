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
import subprocess
import time

import pytest

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
    deadline = time.time() + timeout
    while time.time() < deadline:
        r = _run(
            ["xcrun", "devicectl", "device", "info", "details",
             "--device", device],
            timeout=15,
        )
        if r.returncode == 0 and "iPhone" in (r.stdout or ""):
            return True
        time.sleep(3)
    return False


def _device_reboot(device, timeout=120):
    """Restart device for clean kernel state."""
    logger.info("Rebooting device %s for clean state...", device)
    _run(
        ["xcrun", "devicectl", "device", "process", "launch",
         "--terminate-existing", "--device", device, IOS_BUNDLE_ID],
        timeout=15,
    )
    # There's no clean reboot command via devicectl, but we can
    # terminate all apps and let the device settle
    time.sleep(5)
    assert _wait_device(device, timeout), f"Device {device} did not come back"
    logger.info("Device %s ready", device)


def _launch_app(device):
    """Launch TVM app and wait for SSH."""
    _run(
        ["xcrun", "devicectl", "device", "process", "launch",
         "--terminate-existing", "--device", device, IOS_BUNDLE_ID],
        timeout=15,
    )
    assert _wait_ssh(device, timeout=30), "SSH did not come up after app launch"


def _run_darksword(device, timeout=90):
    """Run darksword and stream output line by line. Returns (lines, alive)."""
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
        deadline = time.time() + timeout
        for line in proc.stdout:
            line = line.rstrip()
            if not line:
                continue
            # skip SSH warnings
            if "WARNING" in line or "quantum" in line or "upgraded" in line or "openssh" in line:
                continue
            lines.append(line)
            logger.info("darksword: %s", line)
            if time.time() > deadline:
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

    # check filesystem access
    assert "opendir(/var/mobile/Library/AddressBook): OK" in output, \
        f"Filesystem access not verified. Output:\n{output}"

    # check device survived (no kernel panic)
    if not alive:
        # device panicked — get crash log for diagnostics
        _wait_device(ios_device, timeout=120)
        try:
            r = _run(
                ["xcrun", "devicectl", "device", "info", "files",
                 "--device", ios_device, "--domain-type", "systemCrashLogs"],
                timeout=30,
            )
            panics = [l for l in (r.stdout or "").splitlines() if "panic-full" in l]
            if panics:
                latest = panics[-1].split()[0]
                logger.error("Latest panic log: %s", latest)
        except Exception:
            pass
        pytest.fail(f"Device panicked after exploit. Output:\n{output}")

    assert "DONE" in output, f"Exploit did not complete. Output:\n{output}"
