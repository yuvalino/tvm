// TVM wrapper around the curl tool's main().
//
// curl's own main() (src/tool_main.c) is renamed to curl_tool_main at link
// time by scripts/build_curl_tool.sh (ld -alias/-unexported_symbol on Apple,
// objcopy --redefine-sym on Linux) -- no curl or OpenSSL sources are patched.
//
// Why the wrapper exists: OpenSSL keeps per-thread state behind a pthread_key
// whose destructor frees it (via OPENSSL_free) when the thread exits. Under
// TVM a guest runs on a task thread, and that destructor can fire during the
// thread's teardown after TVM's own TLS is gone, which is a crash. OpenSSL is
// built `no-atexit`, and a guest's atexit() is not a reliable hook either, so
// nothing else tears OpenSSL down. OPENSSL_cleanup() releases the current
// thread's OpenSSL state and deletes that key, so no destructor is left to
// run at thread exit.
#include <openssl/crypto.h>

int curl_tool_main(int argc, char **argv);

int main(int argc, char **argv)
{
    int rc = curl_tool_main(argc, argv);
    OPENSSL_cleanup();
    return rc;
}
