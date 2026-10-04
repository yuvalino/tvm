# TVM build configuration for git.
#
# scripts/build_git.sh copies this file into the shadow build tree as
# config.mak; git's Makefile includes it right after config.mak.uname, so the
# knobs below both configure the build and undo what config.mak.uname decided
# for a Darwin host where that does not fit a TVM image. The per-build values
# (CC, CFLAGS, the static libcurl/OpenSSL link line, CURL_CONFIG) come from
# the make command line in build_git.sh. No upstream source is patched.

# ── Layout ──
# Compiled-in paths (no RUNTIME_PREFIX): git --exec-path is gitexecdir, where
# the initrd puts the programs git execs (git-remote-https, the shell script
# helpers, the dashed built-in links; see scripts/CMakeLists.txt); `git init`
# copies template_dir; the system config is $(sysconfdir)/gitconfig.
prefix = /tvm
gitexecdir = /tvm/libexec/git-core
template_dir = /tvm/share/git-core/templates
sysconfdir = /tvm/etc

# The shell git execs for `sh -c` commands (hooks configured as one-liners,
# `!` aliases, the pager, GIT_SSH_COMMAND, ...) and the #! line of the
# shipped shell scripts is /tvm/bin/sh (bash invoked as sh): the host's
# /bin/sh is a fat binary TVM cannot load. That is not set here as SHELL_PATH
# because the Makefile also runs its generator scripts with $(SHELL_PATH)
# and its recipes with SHELL = $(SHELL_PATH), on the build host, where
# /tvm/bin/sh does not exist. build_git.sh therefore keeps SHELL_PATH at
# /bin/sh for make and passes the guest shell through the two derived
# variables everything artifact-bound reads (SHELL_PATH_SQ: GIT-BUILD-OPTIONS
# and the script #! lines; SHELL_PATH_CQ_SQ: the compiled-in SHELL_PATH).

# ── Dependencies ──
# A TVM image may depend on the SDK's own libraries (TVM's ulinker loads
# them through the host: libiconv is such a dylib on macOS and iOS, part of
# libc on Linux), and on nothing from Homebrew. There is no gettext, expat
# (drops git-http-push, the WebDAV push helper -- smart HTTP push does not
# need it) or pcre. Perl, Python and Tcl/Tk programs are not shipped.
NO_GETTEXT = 1
NO_EXPAT = 1
NO_PERL = 1
NO_PYTHON = 1
NO_TCLTK = 1
NO_GITWEB = 1
# git 2.55 builds a Rust static library (src/*.rs: varint, hash and loose
# object helpers) into libgit by default; that would need a cargo cross-build
# for the iPhoneOS target. The C implementations are used instead.
NO_RUST = 1
# config.mak.uname adds Homebrew's gettext and libiconv include/library dirs
# on Darwin; nothing from Homebrew may reach a TVM image.
NO_HOMEBREW = 1
# OpenSSL reaches git only through the static libcurl; git's own hashing is
# its sha1dc / block SHA-256 code (config.mak.uname already sets NO_OPENSSL
# together with APPLE_COMMON_CRYPTO on Darwin; this keeps Linux the same).
NO_OPENSSL = 1

# ── Darwin defaults that do not fit ──
# The builtin fsmonitor daemon listens through FSEvents (CoreServices), which
# the iPhoneOS SDK does not have, and a resident daemon behind a unix socket
# has no place inside the VM anyway. Without a backend `git fsmonitor--daemon`
# reports itself unsupported and core.fsmonitor falls back to the hook.
FSMONITOR_DAEMON_BACKEND =
FSMONITOR_OS_SETTINGS =
