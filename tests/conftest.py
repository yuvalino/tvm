import os
import pwd
import subprocess
import pytest


def pytest_addoption(parser):
    parser.addoption("--backend", default="tvm",
                     choices=["tvm", "flutter", "adb", "adb-flutter"],
                     help="TVM backend: 'tvm' (standalone), 'flutter' (macOS/iOS "
                          "app), 'adb' (CLI on an android device over adb), or "
                          "'adb-flutter' (android flutter app over adb).")
    parser.addoption("--device", default=None,
                     help="Target device. Auto-detected as android (adb serial) "
                          "or iOS (devicectl hostname): if the value matches an "
                          "attached adb device it runs the android flutter app "
                          "over adb, otherwise it deploys the iOS app via "
                          "devicectl. Pair with --backend adb for the android "
                          "CLI backend instead of the flutter app.")
    parser.addoption("--adb-serial", default=None,
                     help="adb device serial (passed as `adb -s <serial>`); "
                          "defaults to the single attached device. Implied by "
                          "--device when that names an adb device.")


def _is_adb_device(name):
    """True if `name` is an attached adb device serial (→ android)."""
    if name is None:
        return False
    try:
        out = subprocess.run(["adb", "devices"], capture_output=True,
                             text=True, timeout=10).stdout
    except Exception:
        return False
    serials = [ln.split("\t")[0] for ln in out.splitlines()[1:]
               if "\tdevice" in ln]
    return name in serials


@pytest.fixture(scope="session")
def device(request):
    return request.config.getoption("--device")


@pytest.fixture(scope="session")
def is_android(request, device, backend):
    """The target is an android device (adb serial), whether CLI or flutter."""
    return backend in ("adb", "adb-flutter") or _is_adb_device(device)


@pytest.fixture(scope="session")
def adb_serial(request, device):
    # An explicit --adb-serial wins; otherwise, if --device names an adb
    # device, use it as the serial.
    explicit = request.config.getoption("--adb-serial")
    if explicit:
        return explicit
    if _is_adb_device(device):
        return device
    return None


@pytest.fixture(scope="session")
def backend(request, device):
    b = request.config.getoption("--backend")
    if b != "tvm":
        return b
    # --device with the default backend: pick android-flutter-over-adb vs
    # iOS-flutter-over-devicectl by probing adb.
    if device:
        return "adb-flutter" if _is_adb_device(device) else "flutter"
    return b


@pytest.fixture(scope="session")
def remote_fs(device, backend):
    """True when the guest's filesystem is NOT shared with the host running
    pytest — i.e. a real device (iOS over devicectl, android over adb). Tests
    that move files between host and guest (scp) must then use a genuine local
    path on one side instead of assuming both sides see the same file."""
    return bool(device) or backend in ("adb", "adb-flutter")


def _android_app_user(serial, pkg="com.yuvalino.tvm"):
    """The bionic username of an installed app's uid (e.g. 10234 → 'u0_a234').
    dropbear-in-TVM runs as the app's uid, and single-user mode only permits
    logging in as that user."""
    adb = ["adb"] + (["-s", serial] if serial else [])
    try:
        out = subprocess.run(adb + ["shell", "dumpsys", "package", pkg],
                             capture_output=True, text=True, timeout=15).stdout
    except Exception:
        return None
    import re
    m = re.search(r"userId=(\d+)", out)
    if not m:
        return None
    uid = int(m.group(1))
    if 10000 <= uid <= 19999:
        return f"u0_a{uid - 10000}"
    return None


@pytest.fixture(scope="session")
def ssh_user(device, backend, adb_serial):
    if backend == "adb":
        # CLI backend: tvm runs under `adb shell`, i.e. uid 2000 = "shell".
        return "shell"
    if backend == "adb-flutter":
        # The flutter app runs as its own app uid; log in as that user.
        return _android_app_user(adb_serial) or "shell"
    if device:
        return "mobile"
    # Resolve from the process's real uid, NOT os.getlogin(): dropbear-in-TVM
    # runs as this process's uid and (single-user mode) only permits logging in
    # as that same user. os.getlogin() reads the controlling-terminal login name,
    # which in a headless daemon context (e.g. the CI runner) returns "root"
    # even though the process runs as an ordinary user — the SSH login is then
    # rejected as "wrong user" and every test hangs. getpwuid(getuid()) always
    # matches what dropbear runs as.
    return pwd.getpwuid(os.getuid()).pw_name
