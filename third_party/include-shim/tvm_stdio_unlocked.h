/*
 * Force-included (-include) into every translation unit of git, bash, nano
 * and python (scripts/build_git.sh, build_bash.sh, build_nano.sh,
 * build_python.sh). Not a patch to any of them.
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
 * is interposed by TVM.) bash (include/unlocked-io.h maps putc()/getc()/
 * putchar()/feof()/... onto the _unlocked names wherever they are declared)
 * and nano (read_file() in src/files.c and gnulib's getdelim() read with
 * getc_unlocked() under flockfile()) have the same problem and get the same
 * header. The mapping is a preprocessor one, so a program that defines its
 * own getc()/getc_unlocked() macros on top of it (bash) still ends in a
 * call to the locking function: a macro is not re-expanded inside its own
 * expansion.
 */
#ifndef TVM_STDIO_UNLOCKED_H
#define TVM_STDIO_UNLOCKED_H

/* -include reaches assembler sources too (python's CFLAGS build its .S
 * files): C only. */
#ifndef __ASSEMBLER__

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

/*
 * An autotools build (bash, nano; git has none) compiles with
 * -DHAVE_CONFIG_H and every translation unit includes config.h first.
 * Keep that order: gnulib's replacement headers (nano's lib/stdio.h,
 * reached by the <stdio.h> below through -I) refuse to be included before
 * config.h, and config.h is what defines the feature macros those headers
 * were configured with.
 */
#if defined(HAVE_CONFIG_H)
#include <config.h>
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

#endif /* __ASSEMBLER__ */

#endif
