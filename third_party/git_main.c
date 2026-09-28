// TVM multi-call entry point for git.
//
// git execs some of its transport helpers as separate programs from its exec
// path (/tvm/libexec/git-core): `git clone https://...` runs
// git-remote-https, which upstream links from remote-curl.o as its own
// executable, and git-http-fetch (dumb-HTTP fetch) is another. Each of them,
// like git itself, defines cmd_main() and is linked with common-main.o, whose
// main() sets git up and calls it.
//
// TVM ships them as one image: the `git` executable in the CLI initrd and
// libgit.dylib in the Flutter initrd, with the helper names as links to
// /tvm/bin/git (the launcher stub, in the Flutter case) in the exec path.
// This file is that image's cmd_main(): it picks the program by the name git
// invoked it as -- argv[0] is "git-remote-https" when git execs the helper
// through PATH, exactly as with upstream's separate binaries -- and calls the
// original cmd_main() of git.o, remote-curl.o or http-fetch.o, which
// scripts/build_git.sh renamed at link time (ld -alias / objcopy
// --redefine-sym, as scripts/build_curl_tool.sh does for curl's main). No git
// source is patched, no static library is linked twice, and the Flutter side
// needs one dylib and one launcher instead of one per helper.
//
// The two curl helpers also tear OpenSSL down at exit, for the reason
// third_party/curl_main.c gives: OpenSSL keeps per-thread state behind a
// pthread_key whose destructor frees it when the thread exits, and under TVM
// that destructor fires during the task thread's teardown, after TVM's own
// state is gone -- libmalloc then aborts on a pointer it never allocated and
// the task never finishes exiting, so `git clone https://...` hung after the
// transfer, with its parent waiting for the helper forever.
// OPENSSL_cleanup() releases the state and deletes the key. It is registered
// with atexit() rather than called after the helper returns because git
// leaves through exit() on every path, including die() (common-main.c,
// usage.c), and TVM runs a guest's atexit handlers from exit().
#include <stdlib.h>
#include <string.h>
#include <openssl/crypto.h>

static void tvm_openssl_cleanup(void)
{
	OPENSSL_cleanup();
}

int git_cmd_main(int argc, const char **argv);
int remote_curl_cmd_main(int argc, const char **argv);
int http_fetch_cmd_main(int argc, const char **argv);

int cmd_main(int argc, const char **argv)
{
	const char *name = argc > 0 && argv[0] ? argv[0] : "git";
	const char *slash = strrchr(name, '/');
	if (slash)
		name = slash + 1;

	// upstream: git-remote-http plus the git-remote-{https,ftp,ftps} links
	if (!strcmp(name, "git-remote-http") || !strcmp(name, "git-remote-https") ||
	    !strcmp(name, "git-remote-ftp") || !strcmp(name, "git-remote-ftps")) {
		atexit(tvm_openssl_cleanup);
		return remote_curl_cmd_main(argc, argv);
	}
	if (!strcmp(name, "git-http-fetch")) {
		atexit(tvm_openssl_cleanup);
		return http_fetch_cmd_main(argc, argv);
	}
	// "git" and the dashed built-in names (git-add, ...), which git.c's
	// cmd_main resolves itself
	return git_cmd_main(argc, argv);
}
