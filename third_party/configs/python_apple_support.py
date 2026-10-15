# TVM's _apple_support: installed by scripts/build_python.sh over CPython's
# Lib/_apple_support.py in the staged standard library.
#
# The iOS interpreter is compiled with the iPhoneOS SDK, so it defaults to
# PyConfig.use_system_logger (TARGET_OS_IPHONE, Python/initconfig.c) and at
# startup calls init_streams() below, whose upstream version replaces
# sys.stdout and sys.stderr with writers to the Apple system log: made for an
# app embedding Python, which has no terminal. Under TVM a program's stdout
# and stderr are its terminal, a pipe or a file, so the streams stay as they
# are. (The setting has no environment variable or command-line option; only
# an embedding application's PyConfig can turn it off.)


def init_streams(log_write, stdout_level, stderr_level):
    pass
