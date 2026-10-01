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

/*
 * glibc: the same holds there (getc_unlocked() & co. are extern inlines
 * over the glibc FILE's _IO_read_ptr/_IO_write_ptr at -O2), but the
 * <stdio.h> below is then the translation unit's first system header, and
 * glibc's <features.h> settles the feature set once, before git's
 * compat/posix.h defines _GNU_SOURCE: <fcntl.h> then declared no
 * sync_file_range() and wrapper.c (HAVE_SYNC_FILE_RANGE on Linux) failed to
 * compile. Ask for what compat/posix.h asks for, with its exact values so
 * its own definitions later repeat them harmlessly; on glibc _GNU_SOURCE
 * implies the _XOPEN_SOURCE/_DEFAULT_SOURCE/... it also sets.
 */
#if !defined(__APPLE__)
#define _FILE_OFFSET_BITS 64
#define _GNU_SOURCE 1
#endif

#include <stdio.h>

#if !defined(__APPLE__)
/* <features.h> (done by now) set these to 700 / 1 for _GNU_SOURCE; drop
 * them so compat/posix.h's 600 is not a redefinition in every file. */
#undef _XOPEN_SOURCE
#undef _XOPEN_SOURCE_EXTENDED
#endif

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
