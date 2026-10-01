#!/bin/sh
# Build GNU bash for TVM (unpatched upstream, out-of-tree autotools build).
#
# usage: build_bash.sh <srcdir> <builddir>
# env:   CC (the target compiler, e.g. ios-cc.sh when cross-compiling),
#        TVM_IOS_CROSS=1 when building for iOS
#
# Everything TVM-specific is a configure cache variable or a -D on CFLAGS;
# bash's sources are not touched. After `make`, the exact objects of bash's
# own link line are exported to $BUILDDIR/objects/ so third_party/CMakeLists.txt
# can relink them (with bash's static sub-libraries) as libbash.dylib for the
# Flutter initrd.
set -e
SRCDIR="$1"
BUILDDIR="$2"
CONFIGS_DIR="$(cd "$(dirname "$0")/.." && pwd)/third_party/configs"
. "$(cd "$(dirname "$0")" && pwd)/tvm_stamp.sh"
tvm_stamp_begin "$BUILDDIR" "$0" "$SRCDIR" "$CONFIGS_DIR/bash_stdio_unlocked.h"

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
# make-time only (Darwin): -include bash_stdio_unlocked.h strips the SDK's
# inline *_unlocked stdio macros so bash's I/O goes through the real, TVM-
# interposed stdio functions (see the header and the ac_cv_have_decl_* cache
# variables below). Not passed to configure: a forced <stdio.h> makes
# AC_CHECK_FUNCS' dummy prototypes (`char dprintf (void);`) conflict and
# every such probe would report the function missing.
TVM_MAKE_CFLAGS="$TVM_CFLAGS"
case "$(uname)" in
    Darwin) TVM_MAKE_CFLAGS="$TVM_CFLAGS -include $CONFIGS_DIR/bash_stdio_unlocked.h" ;;
esac

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
#  ac_cv_have_decl_{putc,putchar,getc,getchar,feof,ferror,clearerr}_unlocked=no:
#     bash's include/unlocked-io.h maps putchar()/putc()/getc()... onto the
#     *_unlocked variants when they are declared or defined as macros. On
#     Darwin they are inline macros (__sputc/__sgetc/__sfeof in <_stdio.h>)
#     that poke the libc FILE buffer directly, bypassing TVM's stdio
#     interposition: bytes written by `echo` between words and the trailing
#     newline (and everything written by the printf builtin) landed in the
#     process-global stdout FILE instead of the task's stream and were lost
#     on pipes and files. With these answered `no` and the macros removed by
#     the forced include above, bash calls the real putc()/getc()/...
#     functions, which TVM interposes.
CACHE_VARS="bash_cv_job_control_missing=present bash_cv_sys_named_pipes=present \
bash_cv_dev_fd=absent bash_cv_dev_stdin=absent bash_cv_func_sigsetjmp=present \
bash_cv_unusable_rtsigs=yes bash_cv_wcontinued_broken=no bash_cv_pgrp_pipe=no \
bash_cv_must_reinstall_sighandlers=no bash_cv_getcwd_malloc=yes \
bash_cv_ulimit_maxfds=no bash_cv_func_sbrk=no bash_cv_func_brk=no \
ac_cv_func_killpg=no ac_cv_func_pselect=no ac_cv_func_shm_open=no \
ac_cv_func_dlopen=no ac_cv_func_dlsym=no \
ac_cv_func_dlclose=no ac_cv_lib_dl_dlopen=no am_cv_func_iconv=no \
ac_cv_have_decl_putc_unlocked=no ac_cv_have_decl_putchar_unlocked=no \
ac_cv_have_decl_getc_unlocked=no ac_cv_have_decl_getchar_unlocked=no \
ac_cv_have_decl_feof_unlocked=no ac_cv_have_decl_ferror_unlocked=no \
ac_cv_have_decl_clearerr_unlocked=no"

CROSS_FLAGS=""
CC_FLAG=""
if [ -n "$TVM_IOS_CROSS" ]; then
    # CC_FOR_BUILD: mksyntax/mksignames/buildversion run on the build host.
    CROSS_FLAGS="--host=aarch64-apple-darwin CC_FOR_BUILD=cc"
    CC_FLAG="${CC:+CC=$CC}"
    # Termcap: the iPhoneOS SDK has libtermcap.tbd (-> libncurses.5.4.dylib,
    # present in the dyld shared cache) but the device has no terminfo
    # database, so tgetent() fails and readline's first tputs() into ncurses
    # faults (SIGBUS in tputs from _rl_cr, seen as "crash in signal handler"
    # panics on pan1c). Use the GNU termcap bundled with bash instead and
    # point it at a termcap file shipped in the initrd (TERMCAP_FILE is
    # lib/termcap/termcap.c's #ifndef'd default, /etc/termcap). The dylib link
    # in third_party/CMakeLists.txt mirrors this with lib/termcap/libtermcap.a.
    CROSS_FLAGS="$CROSS_FLAGS bash_cv_termcap_lib=gnutermcap"
    TVM_CFLAGS="$TVM_CFLAGS -DTERMCAP_FILE=\\\"/tvm/etc/termcap\\\""
    TVM_MAKE_CFLAGS="$TVM_MAKE_CFLAGS -DTERMCAP_FILE=\\\"/tvm/etc/termcap\\\""
else
    case "$(uname)" in
        # macOS: the SDK's libtermcap.tbd (-> libncurses.5.4.dylib) with the
        # host terminfo database; pinned so the dylib link can list -ltermcap.
        Darwin) CROSS_FLAGS="$CROSS_FLAGS bash_cv_termcap_lib=libtermcap" ;;
    esac
fi
# ac_cv_func_getentropy=no: the iPhoneOS SDK exports the symbol but has no
# <sys/random.h> declaring it, so lib/sh/random.c fails to compile with
# HAVE_GETENTROPY; bash's fallback reads /dev/urandom through the interposed
# open(). Pinned on macOS too so both shells behave the same.
case "$(uname)" in
    Darwin) CROSS_FLAGS="$CROSS_FLAGS ac_cv_func_getentropy=no" ;;
esac

if [ ! -f Makefile ]; then
    "$SRCDIR/configure" \
        --without-bash-malloc \
        --disable-nls \
        $CROSS_FLAGS \
        $CC_FLAG \
        $CACHE_VARS \
        CFLAGS="$TVM_CFLAGS"
fi
make CFLAGS="$TVM_MAKE_CFLAGS"

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

# Export bash's own link inputs for the libbash.dylib relink: the top-level
# directory also holds host-built helper objects (mksignames.o, buildversion.o,
# buildsignames.o — each with a main(), and built with CC_FOR_BUILD when
# cross-compiling), so a *.o glob cannot be used. Ask the Makefile instead.
printf 'include Makefile\ntvm-print-objects:\n\t@echo $(OBJECTS)\n' > tvm_link.mk
OBJECTS="$(make -s -f tvm_link.mk tvm-print-objects)"
rm -rf objects
mkdir -p objects
for o in $OBJECTS; do
    ln -sf "$BUILDDIR/$o" "objects/$o"
done
