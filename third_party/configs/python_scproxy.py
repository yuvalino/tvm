# TVM's _scproxy for iOS: installed by scripts/build_python.sh into the iOS
# build's standard library, in place of CPython's C module.
#
# urllib.request imports _scproxy on every Darwin platform (sys.platform is
# "darwin" under TVM, iOS included) to read the system's proxy settings, and
# the C module reads them through SCDynamicStore, which the iPhoneOS SDK does
# not have, so it is not built for iOS. Without a module of that name
# urllib.request, and with it pip, failed to import. This one reports no
# system proxies: the *_proxy / no_proxy environment variables, which
# urllib consults first, still work.


def _get_proxy_settings():
    return {"exclude_simple": False, "exceptions": []}


def _get_proxies():
    return {}
