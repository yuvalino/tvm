/*
 * Forced include (-include) for the TVM bash build. Not a bash patch.
 *
 * Darwin's <_stdio.h> defines putc_unlocked()/getc_unlocked() and the
 * putchar/getchar/feof/ferror/clearerr/fileno _unlocked variants as inline
 * macros (__sputc/__sgetc/__sfeof/...) that read and write the libc FILE
 * buffer directly. bash's include/unlocked-io.h then maps putc()/putchar()/
 * getc()/... onto them whenever the *_unlocked names are declared or defined,
 * so the shell's own output bypasses TVM's stdio interposition: the bytes go
 * into the process-global stdout FILE instead of the task's stream and are
 * lost on pipes and files (echo separators, the printf builtin, ...).
 *
 * Undefining the macros here, together with ac_cv_have_decl_*_unlocked=no in
 * scripts/build_bash.sh, makes unlocked-io.h fall back to the plain stdio
 * functions, which TVM interposes per task.
 */
#include <stdio.h>
#undef getc_unlocked
#undef putc_unlocked
#undef getchar_unlocked
#undef putchar_unlocked
#undef feof_unlocked
#undef ferror_unlocked
#undef clearerr_unlocked
#undef fileno_unlocked
