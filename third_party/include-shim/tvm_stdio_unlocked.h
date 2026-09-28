/*
 * Force-included (-include) into every git translation unit
 * (scripts/build_git.sh). Not a git patch.
 *
 * A guest FILE is a TVM file_stream, not a libc FILE: stdio reaches the
 * task's descriptors only through the functions TVM interposes. Darwin's
 * <stdio.h> defines getc_unlocked()/putc_unlocked() and the getchar/putchar/
 * feof/ferror/clearerr/fileno _unlocked variants as inline macros
 * (__sgetc/__sputc/__sfeof/...) that read and write the FILE's buffer fields
 * directly, so on a TVM stream they read garbage and cannot be interposed.
 * git reads its config files and strbuf_getwholeline() input with
 * getc_unlocked() under flockfile() (config.c, strbuf.c); compat/posix.h
 * only maps those to getc()/no-ops where _POSIX_THREAD_SAFE_FUNCTIONS is
 * not defined, which Darwin defines. Map every _unlocked macro to the
 * locking function here, which TVM interposes per task. (flockfile() itself
 * is interposed by TVM.) The same problem, solved the same way, for bash:
 * third_party/configs/bash_stdio_unlocked.h; nano answers
 * ac_cv_func_flockfile=no instead.
 */
#ifndef TVM_STDIO_UNLOCKED_H
#define TVM_STDIO_UNLOCKED_H

#include <stdio.h>

#undef getc_unlocked
#undef putc_unlocked
#undef getchar_unlocked
#undef putchar_unlocked
#undef feof_unlocked
#undef ferror_unlocked
#undef clearerr_unlocked
#undef fileno_unlocked
#define getc_unlocked(fp) getc(fp)
#define putc_unlocked(c, fp) putc(c, fp)
#define getchar_unlocked() getchar()
#define putchar_unlocked(c) putchar(c)
#define feof_unlocked(fp) feof(fp)
#define ferror_unlocked(fp) ferror(fp)
#define clearerr_unlocked(fp) clearerr(fp)
#define fileno_unlocked(fp) fileno(fp)

#endif
