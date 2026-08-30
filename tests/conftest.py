import os
import pwd
import pytest


def pytest_addoption(parser):
    parser.addoption("--backend", default="tvm", choices=["tvm", "flutter"],
                     help="TVM backend: 'tvm' (standalone) or 'flutter' (app)")
    parser.addoption("--device", default=None,
                     help="Target device hostname (e.g. my-device.local). "
                          "If set, defaults backend to 'flutter' and deploys via devicectl.")


@pytest.fixture(scope="session")
def device(request):
    return request.config.getoption("--device")


@pytest.fixture(scope="session")
def backend(request, device):
    b = request.config.getoption("--backend")
    if device and b == "tvm":
        return "flutter"
    return b


@pytest.fixture(scope="session")
def ssh_user(device):
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
