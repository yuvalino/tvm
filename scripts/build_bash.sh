#!/bin/sh
# Build GNU bash for TVM (unpatched upstream, out-of-tree autotools build).
#
# usage: build_bash.sh <srcdir> <builddir> <ncurses-prefix>
# env:   CC (the target compiler, e.g. ios-cc.sh when cross-compiling),
#        TVM_IOS_CROSS=1 when building for iOS
#
# Everything TVM-specific is a configure cache variable or a -D on CFLAGS;
# bash's sources are not touched. The Flutter apps get the same `bash`,
# converted to libbash.dylib (third_party/CMakeLists.txt, add_shared_lib).
#
# Terminal library: readline links the static ncursesw of build_ncurses.sh,
# with the terminfo database shipped in the initrd (/tvm/share/terminfo) --
# the same library and database as vim, less and nano, on every platform.
# Nothing depends on a host curses library or terminfo (absent on iOS, where
# a tgetent() failure used to make readline's first tputs() fault).
set -e
SRCDIR="$1"
BUILDDIR="$2"
NCURSES_PREFIX="$3"
INCLUDE_SHIM="$(cd "$(dirname "$0")/.." && pwd)/third_party/include-shim"
. "$(cd "$(dirname "$0")" && pwd)/tvm_stamp.sh"
tvm_stamp_begin "$BUILDDIR" "$0" "$SRCDIR" "$INCLUDE_SHIM/tvm_stdio_unlocked.h" \
    "$NCURSES_PREFIX/lib/libncursesw.a"

mkdir -p "$BUILDDIR"
cd "$BUILDDIR"

# Compile-time path overrides (no patches):
#  -D_PATHNAMES_H_: pre-defines pathnames.h's include guard so its three
#     unconditional #defines (SYS_PROFILE, DEFAULT_HOSTS_FILE,
#     DEBUGGER_START_FILE) are skipped and ours take effect. SYS_PROFILE is
#     otherwise hard-wired to the host's /etc/profile, which a TVM login shell
#     must not read. The `strings` assertion below catches a future release
#     renaming that guard.
#  -DSYS_BASHRC: system-wide rc for interactive shells (config-top.h leaves it
#     commented out).
#  -DDEFAULT_PATH_VALUE: PATH when the environment has none (config-top.h
#     guards it with #ifndef); the initrc normally sets PATH.
TVM_CFLAGS='-g -gdwarf-4 -D_PATHNAMES_H_ -DSYS_PROFILE=\"/tvm/etc/profile\" -DDEFAULT_HOSTS_FILE=\"/etc/hosts\" -DDEBUGGER_START_FILE=\"/tvm/share/bashdb/bashdb-main.inc\" -DSYS_BASHRC=\"/tvm/etc/bash.bashrc\" -DDEFAULT_PATH_VALUE=\"/tvm/bin:/usr/bin:/bin\"'
# make-time only: -include tvm_stdio_unlocked.h maps the *_unlocked stdio
# accessors back to the locking functions, which TVM interposes (see the
# header; git and nano are built with it the same way). bash's
# include/unlocked-io.h turns putc()/getc()/putchar()/feof()/... into their
# _unlocked variants wherever those are declared, and on Darwin the variants
# are inline macros over the libc FILE's buffer: the shell's own output
# bypassed TVM's streams and bytes written by `echo` between words, and
# everything the printf builtin wrote, were lost on pipes and files. Not
# passed to configure: a forced <stdio.h> makes AC_CHECK_FUNCS' dummy
# prototypes (`char dprintf (void);`) conflict and every such probe would
# report the function missing.
TVM_MAKE_CFLAGS="$TVM_CFLAGS -include $INCLUDE_SHIM/tvm_stdio_unlocked.h"

# Configure cache variables. The bash_cv_* ones are run-time probes that
# cannot execute when cross-compiling (iOS) and would otherwise default to
# their pessimistic values; they are pinned on every platform so the macOS
# and iOS shells behave identically (and the probes are answered for the TVM
# guest, not for the build host):
#  job_control_missing=present, sys_named_pipes=present, func_sigsetjmp=present,
#  wcontinued_broken=no, pgrp_pipe=no, must_reinstall_sighandlers=no:
#     the host answers, TVM interposes all of it (setpgid/tcsetpgrp/waitpid/
#     mkfifo/sigaction).
#  dev_fd=absent, dev_stdin=absent: TVM's VFS has no /dev/fd, so process
#     substitution must use FIFOs (mkfifo is interposed) instead of /dev/fd/N.
#  getcwd_malloc=yes: getcwd(0,0) mallocs — TVM's interposed getcwd does. `no`
#     would make bash use its own lib/sh/getcwd.c (opendir("..") walk), which
#     bypasses TVM's virtual cwd and cpio mounts.
#  unusable_rtsigs=yes: no real-time signals on Darwin.
#  ulimit_maxfds=no, func_sbrk=no, func_brk=no: only affect bash's own malloc
#     (disabled with --without-bash-malloc) and `ulimit -n` fallbacks.
#  ac_cv_func_killpg=no: REQUIRED. TVM does not interpose killpg(), so bash
#     must use its kill(-pgrp) fallback (lib/sh/oslib.c), which TVM does route.
#  ac_cv_func_pselect=no: TVM interposes select() but not pselect(); a guest
#     pselect() reaches the host with guest fd numbers, so `read -t`, `read -t
#     0` and readline timeouts waited on the wrong fd (read -t 1 </dev/null
#     timed out instead of hitting EOF). bash then uses select() everywhere
#     (lib/sh/timers.c, input_avail.c, ufuncs.c, lib/readline/input.c).
#  ac_cv_func_shm_open=no: same class of problem for lib/sh/anonfile.c (used
#     by ${ command; } nofork substitutions): a host shm_open() fd is not a
#     guest fd. bash falls back to sh_mktmpfd(), i.e. an interposed open().
#  ac_cv_func_dlopen/dlsym/dlclose=no, ac_cv_lib_dl_dlopen=no: no loadable
#     builtins (guest dlopen is not interposed); `enable -f` reports them
#     unsupported. dlclose must go with them: enable.def compiles its
#     dlclose/dlerror helpers under HAVE_DLCLOSE alone but only includes
#     <dlfcn.h> under HAVE_DLOPEN && HAVE_DLSYM.
#  am_cv_func_iconv=no: no libiconv dependency on any platform.
CACHE_VARS="bash_cv_job_control_missing=present bash_cv_sys_named_pipes=present \
bash_cv_dev_fd=absent bash_cv_dev_stdin=absent bash_cv_func_sigsetjmp=present \
bash_cv_unusable_rtsigs=yes bash_cv_wcontinued_broken=no bash_cv_pgrp_pipe=no \
bash_cv_must_reinstall_sighandlers=no bash_cv_getcwd_malloc=yes \
bash_cv_ulimit_maxfds=no bash_cv_func_sbrk=no bash_cv_func_brk=no \
ac_cv_func_killpg=no ac_cv_func_pselect=no ac_cv_func_shm_open=no \
ac_cv_func_dlopen=no ac_cv_func_dlsym=no \
ac_cv_func_dlclose=no ac_cv_lib_dl_dlopen=no am_cv_func_iconv=no"

CROSS_FLAGS=""
CC_FLAG=""
if [ -n "$TVM_IOS_CROSS" ]; then
    # CC_FOR_BUILD: mksyntax/mksignames/buildversion run on the build host.
    CROSS_FLAGS="--host=aarch64-apple-darwin CC_FOR_BUILD=cc"
    CC_FLAG="${CC:+CC=$CC}"
fi
# ac_cv_func_getentropy=no: the iPhoneOS SDK exports the symbol but has no
# <sys/random.h> declaring it, so lib/sh/random.c fails to compile with
# HAVE_GETENTROPY; bash's fallback reads /dev/urandom through the interposed
# open(). Pinned on macOS too so both shells behave the same.
case "$(uname)" in
    Darwin) CROSS_FLAGS="$CROSS_FLAGS ac_cv_func_getentropy=no" ;;
esac

# Configure:
#  bash_cv_termcap_lib=libncursesw: TERMCAP_LIB=-lncursesw (aclocal.m4,
#     BASH_CHECK_LIB_TERMCAP), found through LDFLAGS; the probe would
#     otherwise take the first host library with tgetent(). CPPFLAGS puts
#     our <termcap.h> ahead of the SDK's (the iPhoneOS SDK has none).
if [ ! -f Makefile ]; then
    "$SRCDIR/configure" \
        --without-bash-malloc \
        --disable-nls \
        $CROSS_FLAGS \
        $CC_FLAG \
        $CACHE_VARS \
        bash_cv_termcap_lib=libncursesw \
        CPPFLAGS="-I$NCURSES_PREFIX/include -I$NCURSES_PREFIX/include/ncursesw" \
        LDFLAGS="-L$NCURSES_PREFIX/lib" \
        CFLAGS="$TVM_CFLAGS"
fi
# Build-time assertion: the terminal library must be our ncursesw, not a host
# library (which the iOS device has no database for).
if ! grep -q '^TERMCAP_LIB = -lncursesw' Makefile; then
    echo "error: bash was not configured against libncursesw" >&2
    exit 1
fi
make CFLAGS="$TVM_MAKE_CFLAGS"
if nm -u bash | grep -q 'tputs'; then
    echo "error: bash imports tputs from a host library" >&2
    exit 1
fi

# Build-time assertion: the login-shell profile path must have made it into
# the binary. If a future bash release renames pathnames.h's include guard,
# -D_PATHNAMES_H_ silently stops working and the shell would read the host's
# /etc/profile — fail loudly instead.
if ! strings bash | grep -q '^/tvm/etc/profile$'; then
    echo "error: bash does not contain SYS_PROFILE=/tvm/etc/profile (pathnames.h guard changed?)" >&2
    exit 1
fi
if strings bash | grep -q '^/etc/profile$'; then
    echo "error: bash still contains the host /etc/profile path" >&2
    exit 1
fi
