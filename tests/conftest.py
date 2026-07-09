import sys

def pytest_addoption(parser):
    parser.addoption("--backend", default="tvm", choices=["tvm", "flutter"],
                     help="TVM backend: 'tvm' (standalone) or 'flutter' (app)")
    parser.addoption("--device", default=None,
                     help="Target device hostname (e.g. pan1c.local). "
                          "If set, defaults backend to 'flutter' and deploys via devicectl.")

def pytest_collection_modifyitems(config, items):
    device = config.getoption("--device", None)
    backend = config.getoption("--backend", "tvm")
    if device and backend == "tvm":
        backend = "flutter"
    for name, mod in sys.modules.items():
        if name.endswith("test_integration") and hasattr(mod, "BACKEND"):
            mod.DEVICE = device
            mod.BACKEND = backend
