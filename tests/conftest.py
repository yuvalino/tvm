import os
import pytest


def pytest_addoption(parser):
    parser.addoption("--backend", default="tvm", choices=["tvm", "flutter"],
                     help="TVM backend: 'tvm' (standalone) or 'flutter' (app)")
    parser.addoption("--device", default=None,
                     help="Target device hostname (e.g. pan1c.local). "
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
    return "mobile" if device else os.getlogin()
