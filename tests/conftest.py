import os
import pwd
import pytest


def pytest_addoption(parser):
    parser.addoption("--backend", default="tvm",
                     choices=["tvm", "flutter", "adb", "adb-flutter"],
                     help="TVM backend: 'tvm' (standalone), 'flutter' (macOS/iOS "
                          "app), 'adb' (CLI on an android device over adb), or "
                          "'adb-flutter' (android flutter app over adb).")
    parser.addoption("--device", default=None,
                     help="Target device hostname (e.g. pan1c.local). "
                          "If set, defaults backend to 'flutter' and deploys via devicectl.")
    parser.addoption("--adb-serial", default=None,
                     help="adb device serial (passed as `adb -s <serial>`); "
                          "defaults to the single attached device.")


@pytest.fixture(scope="session")
def device(request):
    return request.config.getoption("--device")


@pytest.fixture(scope="session")
def adb_serial(request):
    return request.config.getoption("--adb-serial")


@pytest.fixture(scope="session")
def backend(request, device):
    b = request.config.getoption("--backend")
    if device and b == "tvm":
        return "flutter"
    return b


@pytest.fixture(scope="session")
def ssh_user(device, backend):
    if backend in ("adb", "adb-flutter"):
        # dropbear-in-TVM on an android device runs as uid 2000 ("shell", the
        # adb shell user); single-user mode only permits logging in as it.
        return "shell"
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
