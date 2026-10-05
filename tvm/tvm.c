#define _GNU_SOURCE

// TVM reimplements system() using fork+exec. iOS marks the libc
// system() as __unavailable, so undef that attribute via macro before
// headers are included.
#ifdef __APPLE__
#include <TargetConditionals.h>
#include <libkern/OSCacheControl.h>
#if TARGET_OS_IOS
#define __IOS_UNAVAILABLE
#define __IOS_PROHIBITED
#endif
#endif

#include <dlfcn.h>

#include <sys/uio.h>
#include <sys/types.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <poll.h>
#include <unistd.h>
#include <err.h>
#include <sys/resource.h>
#ifdef __APPLE__
#include <mach/mach_init.h>
#include <mach/mach_host.h>
#include <mach/mach_port.h>
#include <mach/thread_act.h>
#include <mach/thread_info.h>
#endif
#include <errno.h>
#include <stdio.h>
#include <pthread.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <wchar.h>
#include <setjmp.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <sys/file.h>
#include <sys/time.h>

#include <limits.h>
#include <termios.h>
#include <getopt.h>
#include <grp.h>
#include <pwd.h>
#include <signal.h>
#include <dirent.h>

#if defined(__APPLE__)
#include <util.h>
#include <sys/param.h>
#include <sys/mount.h>
#include <sys/fileport.h>
#include <mach-o/getsect.h>
#include <CoreFoundation/CoreFoundation.h>
#include <sys/sysctl.h>
#elif defined(__linux__)
#include <pty.h>
#include <sys/vfs.h>
#include <sys/epoll.h>
#include <sys/timerfd.h>
#include <asm/prctl.h>
#include <link.h>
#include <malloc.h>   // memalign, pvalloc
#include <elf.h>
#include <sys/eventfd.h>
#include <sys/inotify.h>
#include <sys/statvfs.h>
#include <sys/xattr.h>
#include <stdio_ext.h>  // __fpurge
#endif

#if defined(__linux__)
#define ITIMER_WHICH_T __itimer_which_t
#else
#define ITIMER_WHICH_T int
#endif

#if defined(__APPLE__)
typedef int timer_t;
struct itimerspec {
    struct timespec it_interval;
    struct timespec it_value;
};
#define TIMER_ABSTIME   1
#define SIGEV_THREAD_ID 4
int timer_create(clockid_t, struct sigevent *restrict, timer_t *restrict);
int timer_settime(timer_t, int, const struct itimerspec *restrict, struct itimerspec *restrict);
int timer_gettime(timer_t, struct itimerspec *restrict);
int timer_delete(timer_t);
int timer_getoverrun(timer_t);
int sigwaitinfo(const sigset_t *restrict, siginfo_t *restrict);
int sigtimedwait(const sigset_t *restrict, siginfo_t *restrict, const struct timespec *restrict);
#endif

#include "tvm.h"
#include "rmem.h"
#include "ulinker.h"
#include "unicorn/unicorn.h"
#include "unicorn/x86.h"
#include "unicorn/arm64.h"

/**
 * Instructions unclear, ended up implementing a kernel.
 *    (all I wanted was to have ssh server in iOS)
 * 
 * Well I haven't figured out entirely why, but this bad idea seems to work okay-ish.
 * 
 * TODO:
 * - Multi-threading (with COW)
 * - SIGSTOP/SIGTTIN/SIGTTOU without deadlocking the entire TVM
 */

// TODOLIST
// 2. taskfreelastref taskslock deadlock
// 4. waitpid signal delivery
// 5. race for signal delivery before blocking IO operation
// 6. external signal delivery may be received in TVM threads, needs to be handled outside
// 7. support co-existing with external sigaction handlers

// LOCK ORDERING

// tasks
// tty | vfs_node
// timers
// task
// file
// file_stream
// vfs_file

/////////////
// MARK: Macros
/////////////

/**
 * "the things you need the most are the things you never have" - T.S.
 */

#define panic(Msg, ...) do { char __MSG[0x1000] = {0}; snprintf(__MSG, sizeof(__MSG), Msg, ##__VA_ARGS__); __panic(__MSG, __FILE__, __LINE__); } while (0)
__attribute__((noreturn)) static void __panic(const char *m, const char *f, int l);

#define VERIFY(Expression) do { if (!(Expression)) { panic("VERIFY( " #Expression " )"); } } while (0)

#ifdef __O_TMPFILE
#define __OPEN_NEEDS_MODE(oflag) \
  (((oflag) & O_CREAT) != 0 || ((oflag) & __O_TMPFILE) == __O_TMPFILE)
#else
# define __OPEN_NEEDS_MODE(oflag) (((oflag) & O_CREAT) != 0)
#endif

#define WRITE_ONCE(x, val) x=(val)
#define READ_ONCE(x) (x)

#define container_of(ptr, type, member) ({                      \
        const typeof( ((type *)0)->member ) *__mptr = (ptr);    \
        (type *)((char *)__mptr - offsetof(type,member));})

static long g_tz_offset; // seconds east of UTC, cached at startup

static void format_time(char *buf, size_t buflen)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);

    // apply cached tz offset to avoid localtime_r which takes glibc's
    // tzset_lock and deadlocks when called from malloc/strdup hooks
    long long t = tv.tv_sec + g_tz_offset;
    int ms = tv.tv_usec / 1000;

    int sec = t % 60; t /= 60;
    int min = t % 60; t /= 60;
    int hour = t % 24; t /= 24;

    // days since epoch -> y/m/d  (civil_from_days, Howard Hinnant)
    long long z = t + 719468;
    long long era = (z >= 0 ? z : z - 146096) / 146097;
    long long doe = z - era * 146097;
    long long yoe = (doe - doe/1460 + doe/36524 - doe/146096) / 365;
    long long y = yoe + era * 400;
    long long doy = doe - (365*yoe + yoe/4 - yoe/100);
    long long mp = (5*doy + 2) / 153;
    int day = doy - (153*mp + 2)/5 + 1;
    int mon = mp + (mp < 10 ? 3 : -9);
    if (mon <= 2) y++;

    snprintf(buf, buflen,
        "%04d-%02d-%02d %02d:%02d:%02d.%03d",
        (int)y, mon, day, hour, min, sec, ms);
}

static int g_outfd = STDOUT_FILENO;  // kernel log fd; -1 = logging disabled
static void log_write(const char *buf, int n);

#define INFO(Msg, ...) do { \
    char __ts[32]; \
    format_time(__ts, sizeof(__ts)); \
    char __msgbuf[0x1000]; \
    int __num = snprintf(__msgbuf, sizeof(__msgbuf), "%s Kernel(%d): %s: " Msg "\n", __ts, __gettid(), __FUNCTION__, ##__VA_ARGS__); \
    log_write(__msgbuf, __num); \
} while (0)

#define __ALIGN_MASK(x,mask)    (((x)+(mask))&~(mask))

/////////////
// MARK: List
/////////////

/**
 * Courtesy of the Linux Kernel (in XNU its a shitshow).
 */

#define LIST_POISON1  ((void *) 0x100)
#define LIST_POISON2  ((void *) 0x122)

#define LIST_HEAD_INIT(name) { &(name), &(name) }

#undef LIST_HEAD
#define LIST_HEAD(name) \
	struct list_head name = LIST_HEAD_INIT(name)

struct list_head {
    struct list_head *next, *prev;
};

static inline void INIT_LIST_HEAD(struct list_head *list)
{
	WRITE_ONCE(list->next, list);
	WRITE_ONCE(list->prev, list);
}

static inline void __list_add(struct list_head *new,
			      struct list_head *prev,
			      struct list_head *next)
{
	next->prev = new;
	new->next = next;
	new->prev = prev;
	WRITE_ONCE(prev->next, new);
}

static inline void list_add(struct list_head *new, struct list_head *head)
{
	__list_add(new, head, head->next);
}

static inline void list_add_tail(struct list_head *new, struct list_head *head)
{
	__list_add(new, head->prev, head);
}

static inline void list_del(struct list_head *entry)
{
    entry->next->prev = entry->prev;
	WRITE_ONCE(entry->prev->next, entry->next);
    
	entry->next = LIST_POISON1;
	entry->prev = LIST_POISON2;
}

static inline int list_empty(const struct list_head *head)
{
	return READ_ONCE(head->next) == head;
}

#define list_entry(ptr, type, member) \
	container_of(ptr, type, member)

#define list_first_entry(ptr, type, member) \
	list_entry((ptr)->next, type, member)

#define list_next_entry(pos, member) \
	list_entry((pos)->member.next, typeof(*(pos)), member)

static inline int list_is_head(const struct list_head *list, const struct list_head *head)
{
	return list == head;
}

#define list_entry_is_head(pos, head, member)				\
	list_is_head(&pos->member, (head))

#define list_for_each_entry(pos, head, member)				\
	for (pos = list_first_entry(head, typeof(*pos), member);	\
	     !list_entry_is_head(pos, head, member);			\
	     pos = list_next_entry(pos, member))

/////////////
// MARK: Functions
/////////////

/**
 * Get pointers to POSIX API functions so we can call them from their hook.
 * 
 * Basically, DECL_FUNC() just declares a global variable for that function's original pointer
 *  and populates it on init.
 * 
 * Once a function is declared, we can provide CALL_FUNC(name, args...) and have the unhooked function called.
 * 
 * TODO:
 * - On older ubuntu (20.04 for sure) some basic functions are missing from libc (like stat()), need to deal with that.
 */

#define LOCAL_SYM(Name) o_##Name
#define DECL_FUNC_EX(Name, Opt) \
    static typeof(Name) *LOCAL_SYM(Name) = NULL; \
    static struct original_func_t _of_ ## Name = { .ptrdest = (void *)&LOCAL_SYM(Name), .name = #Name, .opt = (Opt) }; \
    __attribute__((constructor)) \
    static void _ofc_ ## Name () { list_add_tail( & (_of_ ## Name).list, &original_functions); }
#define DECL_FUNC(Name)     DECL_FUNC_EX(Name, 0)
#define DECL_FUNC_OPT(Name) DECL_FUNC_EX(Name, 1)
#define CALL_FUNC(Name, ...) (LOCAL_SYM(Name) (__VA_ARGS__))

struct original_func_t {
    struct list_head list;
    void **ptrdest;
    const char *name;
    int opt;
};

static LIST_HEAD(original_functions);

DECL_FUNC(malloc);
DECL_FUNC(calloc);
DECL_FUNC(realloc);
DECL_FUNC(free);
DECL_FUNC(posix_memalign);
DECL_FUNC(aligned_alloc);
DECL_FUNC(valloc);

DECL_FUNC(opendir)
DECL_FUNC(fdopendir);
DECL_FUNC(closedir);
DECL_FUNC(readdir);
DECL_FUNC(dirfd);
// rewinddir, seekdir, telldir


DECL_FUNC(fopen);
DECL_FUNC(fdopen);
DECL_FUNC(popen);
DECL_FUNC(pclose);
DECL_FUNC(wait4);
DECL_FUNC(wait3);
DECL_FUNC(vwarn);
DECL_FUNC(vwarnx);
#if defined(__APPLE__)
// BSD-only: glibc has none of these, and an unguarded DECL_FUNC both fails to
// compile there and panics at startup when dlsym cannot find the original.
DECL_FUNC(vwarnc);
DECL_FUNC(verrc);
DECL_FUNC(getprogname);
DECL_FUNC(setprogname);
#endif
DECL_FUNC(freopen);
DECL_FUNC(fclose);
DECL_FUNC(fwrite);
DECL_FUNC(fread);
DECL_FUNC(fseek);
DECL_FUNC(fseeko);
DECL_FUNC(ftell);
DECL_FUNC(ftello);
DECL_FUNC(rewind);
DECL_FUNC(flockfile);
DECL_FUNC(ftrylockfile);
DECL_FUNC(funlockfile);
DECL_FUNC(fflush);
DECL_FUNC(setbuf);
DECL_FUNC(setbuffer);
DECL_FUNC(setlinebuf);
DECL_FUNC(setvbuf);
DECL_FUNC(fputc);
DECL_FUNC(fputs);
DECL_FUNC(putc);
DECL_FUNC(putchar);
DECL_FUNC(puts);
DECL_FUNC(fgetc);
DECL_FUNC(fgets);
DECL_FUNC(getc);
DECL_FUNC(getchar);
DECL_FUNC(ungetc);
DECL_FUNC(clearerr);
DECL_FUNC(feof);
DECL_FUNC(ferror);
DECL_FUNC(fileno);

DECL_FUNC(printf);
DECL_FUNC(fprintf);
DECL_FUNC(dprintf);
DECL_FUNC(vprintf);
DECL_FUNC(vfprintf);
DECL_FUNC(vdprintf);
DECL_FUNC(scanf);
DECL_FUNC(fscanf);
DECL_FUNC(vscanf);
DECL_FUNC(vfscanf);
DECL_FUNC(perror);

DECL_FUNC(open);
DECL_FUNC(creat);
DECL_FUNC(openat);
DECL_FUNC(read);
DECL_FUNC(write);
DECL_FUNC(close);
DECL_FUNC(readv);
DECL_FUNC(writev);
DECL_FUNC(pread);
DECL_FUNC(pwrite);
DECL_FUNC(preadv);
DECL_FUNC(pwritev);
DECL_FUNC(lseek);
DECL_FUNC(fsync);
DECL_FUNC(dup);
DECL_FUNC(dup2);
DECL_FUNC(pipe);
DECL_FUNC(select);
DECL_FUNC(poll);
DECL_FUNC(pselect);
#if defined(__linux__)
DECL_FUNC(ppoll);
#endif
DECL_FUNC(shm_open);
DECL_FUNC(shm_unlink);
DECL_FUNC(fcntl);
DECL_FUNC(ftruncate);
DECL_FUNC(stat);
DECL_FUNC(lstat);
DECL_FUNC(fstat);
DECL_FUNC(fstatat);
DECL_FUNC(statfs);
DECL_FUNC(fstatfs);
DECL_FUNC(access);
DECL_FUNC(faccessat);
DECL_FUNC(chmod);
DECL_FUNC(lchmod);
DECL_FUNC(fchmod);
DECL_FUNC(fchmodat);
DECL_FUNC(chown);
DECL_FUNC(lchown);
DECL_FUNC(fchown);
DECL_FUNC(fchownat);
DECL_FUNC(flock);
DECL_FUNC(lockf);
DECL_FUNC(readlink);
DECL_FUNC(readlinkat);
DECL_FUNC(symlink);
DECL_FUNC(symlinkat);
DECL_FUNC(link);
DECL_FUNC(linkat);
DECL_FUNC(rename);
DECL_FUNC(renameat);
DECL_FUNC(unlink);
DECL_FUNC(unlinkat);
DECL_FUNC(mkdir);
DECL_FUNC(mkdirat);
DECL_FUNC(rmdir);
DECL_FUNC(mkfifo);
DECL_FUNC(mkfifoat);
DECL_FUNC(mkstemp);
DECL_FUNC(mkdtemp);

DECL_FUNC(socket);
DECL_FUNC(accept);
DECL_FUNC(bind);
DECL_FUNC(listen);
DECL_FUNC(connect);
DECL_FUNC(getpeername);
DECL_FUNC(getsockname);
DECL_FUNC(getsockopt);
DECL_FUNC(setsockopt);
DECL_FUNC(send);
DECL_FUNC(sendto);
DECL_FUNC(sendmsg);
DECL_FUNC(recv);
DECL_FUNC(recvfrom);
DECL_FUNC(recvmsg);
DECL_FUNC(shutdown);
DECL_FUNC(socketpair);
DECL_FUNC(ioctl);

DECL_FUNC(pthread_create);
DECL_FUNC(pthread_exit);

DECL_FUNC(exit);
DECL_FUNC(_exit);
DECL_FUNC(_Exit);
DECL_FUNC(abort);

DECL_FUNC(verr);
DECL_FUNC(verrx);

DECL_FUNC(getpid);
DECL_FUNC(getppid);
DECL_FUNC(umask);

DECL_FUNC(fork);
DECL_FUNC(wait);
DECL_FUNC(waitpid);

DECL_FUNC(setsid);
DECL_FUNC(getsid);
DECL_FUNC(setpgid);
DECL_FUNC(getpgid);
DECL_FUNC(getpgrp);
DECL_FUNC(setpgrp);

DECL_FUNC(signal);
DECL_FUNC(sigaction);
DECL_FUNC(raise);
DECL_FUNC(kill);
DECL_FUNC(killpg);
#if defined(__APPLE__)
DECL_FUNC(sysctl);
#endif
DECL_FUNC(sigprocmask);
DECL_FUNC(pthread_sigmask);
DECL_FUNC(sigpending);
DECL_FUNC(sigsuspend);
DECL_FUNC(pause);
DECL_FUNC(sigwait);
DECL_FUNC_OPT(sigwaitinfo);
DECL_FUNC_OPT(sigtimedwait);

DECL_FUNC(getenv);
DECL_FUNC(putenv);
DECL_FUNC(setenv);
DECL_FUNC(unsetenv);

DECL_FUNC(execve);
DECL_FUNC(execl);
DECL_FUNC(execlp);
DECL_FUNC(execle);
DECL_FUNC(execv);
DECL_FUNC(execvp);
DECL_FUNC(system);

DECL_FUNC(openpty);

DECL_FUNC(tcgetpgrp);
DECL_FUNC(tcsetpgrp);
DECL_FUNC(isatty);
DECL_FUNC(ttyname);
DECL_FUNC(ttyname_r);
DECL_FUNC(ptsname);
DECL_FUNC(ptsname_r);

DECL_FUNC(tcgetattr);
DECL_FUNC(tcsetattr);
DECL_FUNC(tcsendbreak);
DECL_FUNC(tcdrain);
DECL_FUNC(tcflush);
DECL_FUNC(tcflow);

DECL_FUNC(strdup);
DECL_FUNC(strndup);
DECL_FUNC(wcsdup);
DECL_FUNC(asprintf);
DECL_FUNC(vasprintf);
DECL_FUNC(getline);
DECL_FUNC(getdelim);

DECL_FUNC(mmap);
DECL_FUNC(munmap);
DECL_FUNC(mprotect);

DECL_FUNC(getopt);
DECL_FUNC(getopt_long);
DECL_FUNC(getopt_long_only);

// never decl those
// DECL_FUNC(dlopen);
// DECL_FUNC(dlclose);
// DECL_FUNC(dlsym);
// DECL_FUNC(dlerror);
// DECL_FUNC(dladdr);

DECL_FUNC(realpath);
DECL_FUNC(chdir);
DECL_FUNC(fchdir);
DECL_FUNC(getcwd);
DECL_FUNC(getwd);

DECL_FUNC(bsearch);
DECL_FUNC(qsort);

DECL_FUNC(setitimer);
DECL_FUNC(getitimer);

DECL_FUNC_OPT(timer_create);
DECL_FUNC_OPT(timer_settime);
DECL_FUNC_OPT(timer_gettime);
DECL_FUNC_OPT(timer_delete);
DECL_FUNC_OPT(timer_getoverrun);

#if defined(__linux__)
DECL_FUNC(memalign);
DECL_FUNC(pvalloc);
DECL_FUNC(pread64);
DECL_FUNC(pwrite64);
DECL_FUNC(preadv64);
DECL_FUNC(pwritev64);
DECL_FUNC(fdatasync);
DECL_FUNC(mmap64);
DECL_FUNC(statfs64);
DECL_FUNC(mkstemp64);
DECL_FUNC(fseeko64);
DECL_FUNC(ftello64);
DECL_FUNC(posix_fallocate);
DECL_FUNC(posix_fallocate64);
// glibc entry points that only its own headers' redirections name (fortify,
// C23 scanf): not declared for this translation unit.
extern int __open_2(const char *, int);
extern int __open64_2(const char *, int);
extern int __vprintf_chk(int, const char *, va_list);
extern int __vfprintf_chk(FILE *, int, const char *, va_list);
extern int __vdprintf_chk(int, int, const char *, va_list);
extern size_t __fread_chk(void *, size_t, size_t, size_t, FILE *);
extern size_t __fread_unlocked_chk(void *, size_t, size_t, size_t, FILE *);
extern char *__fgets_chk(char *, size_t, int, FILE *);
extern char *__fgets_unlocked_chk(char *, size_t, int, FILE *);
DECL_FUNC(__open_2);
DECL_FUNC(__open64_2);
DECL_FUNC(__vprintf_chk);
DECL_FUNC(__vfprintf_chk);
DECL_FUNC(__vdprintf_chk);
DECL_FUNC(__fread_chk);
DECL_FUNC(__fread_unlocked_chk);
DECL_FUNC(__fgets_chk);
DECL_FUNC(__fgets_unlocked_chk);
DECL_FUNC(fread_unlocked);
DECL_FUNC(fwrite_unlocked);
DECL_FUNC(fgets_unlocked);
DECL_FUNC(fputs_unlocked);
DECL_FUNC(fputc_unlocked);
DECL_FUNC(fflush_unlocked);
DECL_FUNC(__fpurge);
DECL_FUNC(accept4);
DECL_FUNC(recvmmsg);
DECL_FUNC(sendmmsg);
DECL_FUNC(memfd_create);
DECL_FUNC(inotify_init);
DECL_FUNC(inotify_init1);
DECL_FUNC(inotify_add_watch);
DECL_FUNC(inotify_rm_watch);
DECL_FUNC(sync_file_range);
DECL_FUNC(fpathconf);
DECL_FUNC(futimens);
DECL_FUNC(utimensat);
DECL_FUNC(futimesat);
DECL_FUNC(fgetxattr);
DECL_FUNC(fsetxattr);
DECL_FUNC(flistxattr);
DECL_FUNC(fremovexattr);
DECL_FUNC(getxattr);
DECL_FUNC(lgetxattr);
DECL_FUNC(statvfs);
DECL_FUNC(statvfs64);
DECL_FUNC(fstatvfs);
DECL_FUNC(fstatvfs64);
DECL_FUNC(eaccess);
DECL_FUNC(euidaccess);
DECL_FUNC(mknod);
DECL_FUNC(mknodat);
DECL_FUNC(clearenv);
DECL_FUNC(execvpe);
DECL_FUNC(epoll_create);
DECL_FUNC(epoll_create1);
DECL_FUNC(timerfd_create);
DECL_FUNC(eventfd);
DECL_FUNC(eventfd_read);
DECL_FUNC(eventfd_write);
DECL_FUNC(pipe2);
DECL_FUNC(fstatfs64);
DECL_FUNC(open64);
DECL_FUNC(openat64);
DECL_FUNC(fstatat64);
DECL_FUNC(lseek64);
DECL_FUNC(ftruncate64);
DECL_FUNC(readdir64);
DECL_FUNC(fopen64);
DECL_FUNC(freopen64);
DECL_FUNC(stat64);
DECL_FUNC(lstat64);
DECL_FUNC(fstat64);
DECL_FUNC(get_current_dir_name);
#elif defined(__APPLE__)
DECL_FUNC_OPT(CFDictionaryApplyFunction);
DECL_FUNC(fileport_makeport);
DECL_FUNC(fileport_makefd);
#endif

DECL_FUNC(setusershell);
DECL_FUNC(getusershell);
DECL_FUNC(endusershell);
DECL_FUNC(getpwnam);
DECL_FUNC(getpwuid);
DECL_FUNC(getgrnam);
DECL_FUNC(getgrgid);
DECL_FUNC(localtime);
DECL_FUNC(gmtime);
DECL_FUNC(ctime);
DECL_FUNC(strerror);

static void log_write(const char *buf, int n)
{
    if (g_outfd >= 0)
        CALL_FUNC(write, g_outfd, buf, n);
}

__attribute__((constructor))
static void init_funcs()
{
    struct original_func_t *curr_func;
    list_for_each_entry(curr_func, &original_functions, list) {
        *curr_func->ptrdest = dlsym(RTLD_NEXT, curr_func->name);
        if (NULL == *curr_func->ptrdest) {
            if (!curr_func->opt)
                panic("function '%s' not found", curr_func->name);
            *curr_func->ptrdest = (void *)0x4141DEAD;
        }
    }
};

__attribute__((constructor))
static void format_time_init(void) {
    tzset();
    g_tz_offset = -timezone + (daylight ? 3600 : 0);
}

/////////////
// MARK: Declarations
/////////////

/**
 * Just needed this for code in `Task` section.
 */

struct tty;
struct fops;
struct task;
struct file;
struct file_stream;

void task_filestreamdealloc_list(struct task *t, struct list_head *head);

void *kalloc(size_t size);
void kfree(void *p);
void *krealloc(void *ptr, size_t size);
char *kstrdup(const char *s);

int can_enter_tvm(void *lr);

static int task_assign_file_locked(struct task *t, struct file *file, int min_fd);
static void task_unassign_file_locked(struct task *t, struct file *file);

static struct tty *tty_for_file_locked(struct file *file, int *out_ttymode);

struct emulator_context;
struct emulator_context *emulator_context_create(struct ulinker *ulinker);
void                     emulator_context_destroy(struct emulator_context *ec);

void call_sighandler(uint64_t handler_pc, uint64_t arg0, uint64_t arg1, uint64_t arg2);

static inline void tvm_sig_enter(void);
static inline void tvm_sig_leave(void);

typedef void *(*start_routine_fn)(void *);

static pthread_mutex_t vtimers_lock = PTHREAD_MUTEX_INITIALIZER;
static void vtimer_destroy_locked(int global_idx);

/////////////
// MARK: Helpers
/////////////

static pid_t __gettid()
{
#if defined(__linux__)
    return syscall(SYS_gettid);
#elif defined(__APPLE__)
    uint64_t thread_id;
    if (0 != pthread_threadid_np(pthread_self(), &thread_id))
        panic("pthread_threadid_np");
    // 31-bit thread id with 31th bit on
    // its 31-bit to ensure its positive
    return (pid_t)((thread_id & ((1ULL << 31)-1)) | (1ULL << 30));
#else
#error platform
#endif
}

#define	__W_EXITCODE(ret, sig)	((ret) << 8 | (sig))

static int mkwstatus(int is_exit, int status_or_signal)
{
    if (is_exit) {
        return __W_EXITCODE(status_or_signal, 0);
    }

    return __W_EXITCODE(0, status_or_signal);
}

static int intparse(const char *s, long long *out, int base)
{
    char *endp;

    errno = 0;
    long long r = strtoll(s, &endp, base);
    if (0 != errno)
        return -1;
    
    // not entire string is valid
    if (*endp)
        return -1;
    
    *out = r;
    return 0;
}

struct fdentry {
    int fd;
};

#if defined(__linux__)
static int read_fdtable(struct fdentry **out_fdt, size_t *out_fdn)
{
    int ret = -1;
    DIR *dirp = NULL;
    struct dirent *dent = NULL;

    struct fdentry *fdt = NULL;
    size_t fdn = 0;

    if (!(dirp = opendir("/proc/self/fd")))
        goto err;
    
    while (1) {
        errno = 0;
        if (!(dent = readdir(dirp))) {
            if (errno)
                goto err;
            break;
        }

        if (0 == strcmp(dent->d_name, ".") || 0 == strcmp(dent->d_name, ".."))
            continue;

        long long fd;
        if (intparse(dent->d_name, &fd, 10))
            goto err;
        
        if (dirfd(dirp) == fd)
            continue;

        struct fdentry fde = {.fd = (int)fd};
        if (!(fdt = realloc(fdt, sizeof(*fdt) * (fdn + 1))))
            goto err;

        memcpy(fdt + fdn, &fde, sizeof(*fdt));
        fdn++;
    }

    *out_fdt = fdt;
    *out_fdn = fdn;    
    ret = 0;
err:
    if (ret)
        free(fdt);
    if (dirp)
        closedir(dirp);
    return ret;
}
#elif defined(__APPLE__)

// libproc.h not in iPhoneOS sdk
#define PROC_PIDLISTFDS                 1
int proc_pidinfo(int pid, int flavor, uint64_t arg, void *buffer, int buffersize);

struct proc_fdinfo {
	int32_t                 proc_fd;
	uint32_t                proc_fdtype;
};

static int read_fdtable(struct fdentry **out_fdt, size_t *out_fdn)
{
    int ret = -1;
    struct proc_fdinfo *fdinfos = NULL;
    struct fdentry *fdt = NULL;
    size_t fdn = 0;
    int size = -1;
    
    size = proc_pidinfo(CALL_FUNC(getpid), PROC_PIDLISTFDS, 0, 0, 0);
    if (size < 0)
        goto err;
    
    if (!(fdinfos = malloc(size)))
        goto err;

    size = proc_pidinfo(CALL_FUNC(getpid), PROC_PIDLISTFDS, 0, fdinfos, size);
    if (size < 0)
        goto err;

    for (int i = 0; i < (size / sizeof(*fdinfos)); i++) {
        struct fdentry fde = {.fd = fdinfos[i].proc_fd};
        if (!(fdt = realloc(fdt, sizeof(*fdt) * (fdn + 1))))
            goto err;

        memcpy(fdt + fdn, &fde, sizeof(*fdt));
        fdn++;
    }

    *out_fdt = fdt;
    *out_fdn = fdn;    
    ret = 0;
err:
    if (ret)
        free(fdt);
    if (fdinfos)
        free(fdinfos);
    return ret;
}
#endif

ssize_t fd_getpath(int fd, char *out, size_t outsize)
{
#if defined(__APPLE__)
    char filepath[PATH_MAX];
    filepath[0] = 0;

    if (-1 == CALL_FUNC(fcntl, F_GETPATH, (uint64_t)&filepath))
        return -1;

    size_t len = strlen(filepath);
    if (len + 1 > outsize) {
        errno = ENAMETOOLONG;
        return -1;
    }

    memcpy(out, filepath, len + 1);
    return len;
#elif defined(__linux__)
        if (!out || outsize == 0) {
        errno = EINVAL;
        return -1;
    }

    char link[64];
    snprintf(link, sizeof(link), "/proc/self/fd/%d", fd);

    ssize_t n = CALL_FUNC(readlink, link, out, outsize - 1);
    if (n < 0)
        return -1;

    if ((size_t)n >= outsize) {
        errno = ENAMETOOLONG;
        return -1;
    }

    out[n] = '\0';
    return n;
#endif

    errno = ENOSYS;
    return -1;
}

static uint32_t hex32(const char *s) {
    uint32_t v = 0;
    for (int i = 0; i < 8; i++) {
        char c = s[i];
        v <<= 4;
        if (c >= '0' && c <= '9') v |= (c - '0');
        else if (c >= 'a' && c <= 'f') v |= (c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') v |= (c - 'A' + 10);
        else return 0;
    }
    return v;
}

static size_t align4(size_t n) {
    return (n + 3) & ~3;
}

static void *read_all(int fd, size_t *out_size) {
#define READ_CHUNK 4096
    if (!out_size)
        return NULL;

    *out_size = 0;

    size_t cap = READ_CHUNK;
    char *buf = malloc(cap);
    if (!buf)
        return NULL;

    size_t total = 0;

    while (1) {
        if (total == cap) {
            size_t new_cap = cap * 2;
            char *new_buf = realloc(buf, new_cap);
            if (!new_buf) {
                free(buf);
                return NULL;
            }
            buf = new_buf;
            cap = new_cap;
        }

        ssize_t r = read(fd, buf + total, cap - total);
        if (r < 0) {
            if (errno == EINTR)
                continue;

            free(buf);
            return NULL;
        }

        if (r == 0)
            break;

        total += (size_t)r;
    }

    *out_size = total;
    return buf;
#undef READ_CHUNK
}


/////////////
// MARK: CPIO
/////////////

typedef struct {
    /* Header fields (as in the cpio "newc" format) */
    uint32_t c_magic;
    uint32_t c_ino;
    uint32_t c_mode;
    uint32_t c_uid;
    uint32_t c_gid;
    uint32_t c_nlink;
    uint32_t c_mtime;
    uint64_t c_filesize;
    uint32_t c_devmajor;
    uint32_t c_devminor;
    uint32_t c_rdevmajor;
    uint32_t c_rdevminor;
    uint32_t c_namesize;
    uint32_t c_check;

    /* Derived fields */
    const char *name;     /* pointer inside the buffer */
    const void *data;     /* pointer inside the buffer */
} cpio_t;

#define CPIO_NEWC_HEADERSIZE 110

typedef int (*cpio_cb)(void *arg, const cpio_t *entry);

static int cpio_parse(const void *buf, size_t len, cpio_cb cb, void *arg) {
    const unsigned char *p = buf;
    const unsigned char *end = p + len;

    while (1) {
        if ((size_t)(end - p) < CPIO_NEWC_HEADERSIZE)
            return -1;

        if (memcmp(p, "070701", 6) != 0)
            return -1;

        cpio_t cpio;
        cpio.c_magic = 0x070701;
        p += 6;

        cpio.c_ino = hex32((const char*)p); p += 8;
        cpio.c_mode = hex32((const char*)p); p += 8;
        cpio.c_uid = hex32((const char*)p); p += 8;
        cpio.c_gid = hex32((const char*)p); p += 8;
        cpio.c_nlink = hex32((const char*)p); p += 8;
        cpio.c_mtime = hex32((const char*)p); p += 8;

        cpio.c_filesize = hex32((const char*)p); p += 8;

        cpio.c_devmajor = hex32((const char*)p); p += 8;
        cpio.c_devminor = hex32((const char*)p); p += 8;
        cpio.c_rdevmajor = hex32((const char*)p); p += 8;
        cpio.c_rdevminor = hex32((const char*)p); p += 8;

        cpio.c_namesize = hex32((const char*)p); p += 8;
        cpio.c_check = hex32((const char*)p); p += 8;

        if (cpio.c_namesize == 0 || cpio.c_namesize > 1 << 20)
            return -1;

        if (cpio.c_filesize > (uint64_t)1 << 40)
            return -1;

        if ((size_t)(end - p) < cpio.c_namesize)
            return -1;

        cpio.name = (const char*)p;
        if (cpio.name[cpio.c_namesize - 1] != '\0')
            return -1;

        size_t name_pad = align4(CPIO_NEWC_HEADERSIZE + cpio.c_namesize) - (CPIO_NEWC_HEADERSIZE + cpio.c_namesize);
        p += cpio.c_namesize + name_pad;

        if ((uint64_t)(end - p) < cpio.c_filesize)
            return -1;

        cpio.data = p;

        if (strcmp(cpio.name, "TRAILER!!!") == 0)
            return 0;

        if (cb) {
            int r = cb(arg, &cpio);
            if (r)
                return r;
        }

        p += align4((size_t)cpio.c_filesize);
    }
}

/////////////
// MARK: Jmpbuf
/////////////

/**
 * if you port to a new OS/architecture, good luck with this.
 * 
 * In order to properly have a `fork()` function that actually creates a thread behind the scenes,
 * we need to copy stack frames from the parent's stack to the child's stack, so all the local
 * variables will work on the child when `fork()` returns. To actually set the stack pointer
 * we use `setjmp` and `longjmp`.
 * 
 * The tricky parts are:
 * - Stack data copied from parent must be scanned for pointers pointing to the parent stack and fixed up to child stack.
 * - Saved SP/LR/FP/PC registers are obfuscated with a thread-specific token and must be unobfuscated (mangle/demangle).
 * - On arm64, pointer-authentication encrypts LR (return addresses).
 * 
 * TODO:
 * - MacOS is compiled for arm64 meaning only APPLE library code has PAC for its LRs, but on iOS its arm64e which is for every program.
 */

#if defined(__linux__)
#if defined(__x86_64__)

#define JB_RBX	0
#define JB_RBP	1
#define JB_R12	2
#define JB_R13	3
#define JB_R14	4
#define JB_R15	5
#define JB_RSP	6
#define JB_PC	7
#define JB_SIZE (8*8)

#define PTR_MANGLE(var)	asm ("xor %%fs:0x30, %0\n"		      \
				     "rol $2*8+1, %0"		      \
				     : "=r" (var)			      \
				     : "0" (var))
#define PTR_DEMANGLE(var) asm ("ror $2*8+1, %0\n"	      \
				     "xor %%fs:0x30, %0"			      \
				     : "=r" (var) \
				     : "r" (var))

static void jmpbuf_mangle(jmp_buf *jmpbuf)
{
    void **jb = (void **) (jmpbuf);
    PTR_MANGLE(jb[JB_RBP]);
    PTR_MANGLE(jb[JB_RSP]);
    PTR_MANGLE(jb[JB_PC]);
}

static void jmpbuf_demangle(jmp_buf *jmpbuf)
{
    void **jb = (void **) (jmpbuf);
    PTR_DEMANGLE(jb[JB_RBP]);
    PTR_DEMANGLE(jb[JB_RSP]);
    PTR_DEMANGLE(jb[JB_PC]);
}

static void *jmpbuf_getstack(jmp_buf *jmpbuf)
{
    void **jb = (void **) (jmpbuf);
    return jb[JB_RSP];
}

#undef PTR_MANGLE
#undef PTR_DEMANGLE

#endif
#elif defined(__APPLE__)
#if defined(__aarch64__)

#define JMP_FP 10
#define JMP_LR 11
#define JMP_SP 12

#define PTR_MANGLE(var) do { \
        uint64_t __TOKEN; \
        asm ("mrs %0, TPIDRRO_EL0\n" : "=r" (__TOKEN)); \
        *((uint64_t *) &var) ^= ((uint64_t *) (__TOKEN & ~0x7ULL))[7]; \
    } while (0)

#define PTR_DEMANGLE(var) do { \
        uint64_t __TOKEN; \
        asm ("mrs %0, TPIDRRO_EL0\n" : "=r" (__TOKEN)); \
        *((uint64_t *) &var) ^= ((uint64_t *) (__TOKEN & ~0x7ULL))[7]; \
    } while (0)

static uint64_t
xpaci(uint64_t pointer) {
	//asm("xpaci %[value]\n" : [value] "+r"(pointer));
	return pointer;
}

static uint64_t
pacibsp(uint64_t pointer, uint64_t context) {
	asm(
        "mov x2, sp\n"
        "mov x3, lr\n"
        "mov sp, %[context]\n"
        "mov lr, %[pointer]\n"
        //"pacibsp\n"
        "mov %[pointer], lr\n"
        "mov sp, x2\n"
        "mov lr, x3\n"
        :
        [pointer] "+r"(pointer), [context] "+r"(context)
    );
	return pointer;
}

static uint64_t
xpacd(uint64_t pointer) {
	//asm("xpacd %[value]\n" : [value] "+r"(pointer));
	return pointer;
}

static void jmpbuf_mangle(jmp_buf *jmpbuf)
{
    uint64_t *jb = (uint64_t *) (jmpbuf);

    jb[JMP_LR] = pacibsp(jb[JMP_LR], jb[JMP_SP]);

    PTR_MANGLE(jb[JMP_FP]);
    PTR_MANGLE(jb[JMP_LR]);
    PTR_MANGLE(jb[JMP_SP]);
}

static void jmpbuf_demangle(jmp_buf *jmpbuf)
{
    uint64_t *jb = (uint64_t *) (jmpbuf);
    PTR_DEMANGLE(jb[JMP_FP]);
    PTR_DEMANGLE(jb[JMP_LR]);
    PTR_DEMANGLE(jb[JMP_SP]);

    jb[JMP_LR] = xpaci(jb[JMP_LR]);
}

static void *jmpbuf_getstack(jmp_buf *jmpbuf)
{
    void **jb = (void **) (jmpbuf);
    return jb[JMP_SP];
}

#undef PTR_MANGLE
#undef PTR_DEMANGLE

#endif
#endif

static void jmpbuf_setstack(jmp_buf *jmpbuf, uintptr_t new_stack, uintptr_t old_stack, size_t stack_size)
{
    uintptr_t *jb = (uintptr_t*) (jmpbuf);
    uintptr_t diff = old_stack - new_stack;
 
    for (int i = 0; i < (sizeof(jmp_buf)/8); i++)
    {
        if (jb[i] >= old_stack && jb[i] < (old_stack + stack_size))
            jb[i] -= diff;
    }
}

static void jmpbuf_dupstack(jmp_buf *jmpbuf, uintptr_t new_stack, uintptr_t old_stack, size_t stack_size)
{
    uintptr_t x;

    uintptr_t jmpbuf_old_sp = (uintptr_t) jmpbuf_getstack(jmpbuf);

    if (!(jmpbuf_old_sp >= old_stack && jmpbuf_old_sp < (old_stack + stack_size)))
        panic("jmpbuf's sp not in old_stack");

    memcpy(
        (void *) new_stack,
        (void *) old_stack,
        stack_size - 0x1000
    );

    jmpbuf_setstack(jmpbuf, new_stack, old_stack, stack_size);
}

/////////////
// MARK: Environment
/////////////

/**
 * Every process has its own environment data.
 * 
 * The POSIX design of environment is kind of bad.
 * Due to older functions like `putenv()`, environment is assumed
 * to have user pointers in it, so all environment data is "leaked"
 * and never freed, relying on the OS to free all memory on exit.
 * 
 * Well, if you ever want to solve memory leaks, start with a per-task allocator arena
 * and free all the deep-copied environment on `exit()`.
 */

extern char **environ;

/**
 * Excluding last NULL byte.
 */
static size_t envsize(char **env)
{
    int sz = 0;
    if (env) {
        for (char **e = env; *e != NULL; e++)
            sz++;
    }
    return sz;
}

static char **copyenv(struct rmem_arena *ar, char **env)
{
    if (!env)
        return NULL;
    
    int ok = 0;
    int sz = envsize(env);

    char **r = rmem_arena_alloc(ar, sizeof(char *) * (sz+1));
    if (!r)
        goto out;
    memset(r, 0, sizeof(char *) * (sz+1));
    
    for (int i = 0; i < sz; i++) {
        int len = strlen(env[i]);
        r[i] = rmem_arena_alloc(ar, len + 1);
        if (!r[i]) {
            goto out;
        }
        memcpy(r[i], env[i], len + 1);
    }

    ok = 1;

out:

    if (!ok) {
        if (r) {
            for (char **e = r; *e != NULL; e++)
                rmem_arena_free(ar, e);
            rmem_arena_free(ar, r);
        }
        return NULL;
    }

    return r;
}

static char **addenv(char **env, char *string)
{
    int sz = envsize(env);
    char **newenv = malloc(sizeof(char *) * (sz+2));
    if (!newenv)
        return NULL;

    newenv[sz] = string;
    newenv[sz + 1] = 0;

    if (env)
        memcpy(newenv, env, sizeof(char *) * sz);

    return newenv;
}

static char **findenv(char **env, const char *name, size_t name_len)
{
    if (env) {
        for (char **e = env; *e != NULL; e++) {
            if (0 == strncmp(*e, name, name_len)) {
                if ((*e)[name_len] == '=') {
                    return e;
                }
                if ((*e)[name_len] == 0) {
                    return e;
                }
            }
        }
    }

    return NULL;
}

/////////////
// MARK: File
/////////////

/**
 * Yeah when I say in `Task` section we create a little kernel,
 * this section really got me to realize it.
 * 
 * We can't really use OS-level PTY/TTYs so we need to create our own
 * userspace device drivers. This is our framework to do so.
 * 
 * Since `int` is very large for file-descriptor representation,
 * we use the higher bits (but non-MSB, cause signess) to mark special devices
 * in the file-descriptors table. We route IO operations on these FDs to the functions
 * defined in `fops` structure, and every device driver and use REGISTER_DEV to register it.
 */

#define DEV_REG 0
#define DEV_TTY 1
#define DEV_VFS 2

struct fops {
    /**
     * open and initialize `out` (modify f_data).
     * errno set to ENOTSUP if pathname not relevant to this device.
     */
    int (*f_open)(const char *pathname, int flags, mode_t mode, uid_t uid, gid_t gid, struct file *out);
    /**
     * dup `oldf` to `newf` and initialize `newf` (modify f_data).
     * both locked
     */
    int (*f_dup)(struct file *oldfile, struct file *newfile);

    int (*f_fstat)(struct file *file, struct stat *st);
    int (*f_fchown)(struct file *file, uid_t owner, gid_t group);
    int (*f_fchmod)(struct file *file, mode_t mode);
    /**
     * errno set to ENOTSUP if pathname not relevant to this device.
     */
    int (*f_stat)(const char *pathname, struct stat *st, int flag);
    /**
     * errno set to ENOTSUP if pathname not relevant to this device.
     */
    int (*f_chown)(const char *pathname, uid_t owner, gid_t group, int flag);
    /**
     * errno set to ENOTSUP if pathname not relevant to this device.
     */
    int (*f_chmod)(const char *pathname, mode_t mode, int flag);
    /**
     * errno set to ENOTSUP if pathname not relevant to this device.
     */
    ssize_t (*f_readlink)(const char *pathname, char *buf, size_t bufsiz);
    int (*f_ioctl)(struct file *file, unsigned long request, void *arg);
    ssize_t (*f_read)(struct file *file, void *buf, size_t count, int offset);
    int (*f_readdir)(struct file *file, struct dirent *out);
    ssize_t (*f_write)(struct file *file, const void *buf, size_t count, int offset);
    off_t (*f_lseek)(struct file *file, off_t offset, int whence);
    void (*f_close)(struct file *file);

    void *(*f_mmap)(void *hint, size_t size, int prot, int flags, struct file *file, off_t offset);
};

struct device {
    struct list_head dev_list;
    int dev_type;
    struct fops dev_fops;
};

static LIST_HEAD(devices);
#define REGISTER_DEV(Dev) __attribute__((constructor)) \
    static void _tvm_register_dev_ ## Dev () { list_add_tail(&((Dev).dev_list), &devices); }

struct file {
    uint64_t f_gencnt;
    int f_refcnt;
    pthread_mutex_t f_lock;

    int f_rfd; // real
    int f_type;
    void *f_data;
    struct fops *f_ops;

    int f_nfd;// not real, in task
    struct task *f_task; // back pointer to owning task

    char *f_pathname; // optionally available pathname for open file

    // The program's own no-SIGPIPE choice (SO_NOSIGPIPE, F_SETNOSIGPIPE),
    // kept here because the host descriptor always has it on (rfd_adopt).
    int f_nosigpipe;
};

uint64_t next_gencnt = 0;
pthread_mutex_t next_gentcnt_lock = PTHREAD_MUTEX_INITIALIZER;

static uint64_t get_gencnt()
{
    pthread_mutex_lock(&next_gentcnt_lock);
    uint64_t gencnt = next_gencnt++;
    pthread_mutex_unlock(&next_gentcnt_lock);
    return gencnt;
}

/**
 * must be locked
 */
void filedealloc_locked(struct file *file) {
    if (file->f_ops && file->f_ops->f_close)
        file->f_ops->f_close(file);

    if (file->f_pathname) {
        kfree(file->f_pathname);
        file->f_pathname = NULL;
    }

    file->f_ops = NULL;
    file->f_data = NULL;
    file->f_type = DEV_REG;

    if (file->f_rfd >= 0) {
        if (0 != CALL_FUNC(close, file->f_rfd))
            panic("filedealloc close %d", file->f_rfd);
    }

    file->f_rfd = -1;
    
    pthread_mutex_unlock(&file->f_lock);
    pthread_mutex_destroy(&file->f_lock);
    kfree(file);
}

struct file *filealloc() {

    struct file *file = kalloc(sizeof(struct file));
    if (!file)
        return NULL;
    memset(file, 0, sizeof(*file));

    file->f_rfd = file->f_nfd = -1;
    file->f_gencnt = get_gencnt();

    pthread_mutex_init(&file->f_lock, NULL);

    return file;
}

/**
 * Take ownership of a real fd on behalf of a task.
 *
 * On darwin a write to a broken pipe raises SIGPIPE process-wide: xnu's
 * dofilewrite() calls plain psignal(), and get_signalthread() hands the
 * signal to the first thread in the process whose mask allows it, not to the
 * thread that wrote. Every task shares this one process, so the signal lands
 * on whichever task owns the oldest unmasked thread (the init task), which
 * then dies of another task's broken pipe. Seen as `vm_stat | head -2`
 * killing the parent shell with status 141 once vm_stat was slow enough
 * (emulated) for head to exit between its writes.
 *
 * So the kernel is told never to raise SIGPIPE for a task-owned fd
 * (F_SETNOSIGPIPE, which is SO_NOSIGPIPE for sockets), and epipe_signal()
 * raises it on the writing thread itself when a write fails with EPIPE, where
 * signal_handler maps it to the writing task. Linux delivers SIGPIPE to the
 * writing thread already, so neither half exists there.
 */
static int rfd_adopt(int rfd)
{
#ifdef F_SETNOSIGPIPE
    if (rfd >= 0)
        (void)CALL_FUNC(fcntl, rfd, F_SETNOSIGPIPE, 1);
#endif
    return rfd;
}

/**
 * A write on the calling task's fd just failed; if with EPIPE, deliver the
 * SIGPIPE the kernel was told not to raise (see rfd_adopt). Thread-directed,
 * so it reaches signal_handler on this thread; inside a tvm_sig_enter()
 * section it is deferred to the matching tvm_sig_leave() like any other.
 * errno is preserved for the caller.
 */
static void epipe_signal_fd(int nfd, int send_flags);
#ifdef F_SETNOSIGPIPE
static int current_signal_ignored(int signum);
#endif

static void epipe_signal(void)
{
#ifdef F_SETNOSIGPIPE
    if (errno == EPIPE) {
        int last_errno = errno;
        // An ignored SIGPIPE is discarded here, as task_kill_locked discards
        // any ignored signal, rather than raised and dispatched to SAD_IGN. A
        // program that ignores SIGPIPE and keeps writing to a broken pipe
        // (every write fails with EPIPE) otherwise took a real signal per
        // write, and OS handlers nesting on the thread at that rate ran into
        // non-reentrant code: the dispatcher's log formatting recursed on
        // localeconv's os_unfair_lock (libplatform abort, SIGKILL), and a
        // signal landing inside call_sighandler's ulinker_dladdr took its
        // mutex again (deadlock).
        if (!current_signal_ignored(SIGPIPE))
            pthread_kill(pthread_self(), SIGPIPE);
        errno = last_errno;
    }
#endif
}

static int task_signal_ignored_locked(struct task *t, int signum);

// epipe_signal() for a caller that holds the lock of t (the calling task):
// current_signal_ignored() would take it again.
static void epipe_signal_locked(struct task *t)
{
#ifdef F_SETNOSIGPIPE
    if (errno == EPIPE) {
        int last_errno = errno;
        if (!task_signal_ignored_locked(t, SIGPIPE))
            pthread_kill(pthread_self(), SIGPIPE);
        errno = last_errno;
    }
#else
    (void)t;
#endif
}

struct file *filealloc_from_rfd(int rfd) {
    struct file *file = filealloc();
    if (!file)
        return NULL;

    file->f_rfd = rfd_adopt(rfd);
    return file;
}

void file_lock(struct file *file, int refcount) {
    pthread_mutex_lock(&file->f_lock);
    file->f_refcnt += refcount;
}

/**
 * no refcount taken
 */
void files_lock(struct file *file1, struct file *file2) {
    if (file1 == file2) {
        file_lock(file1, 0);
    }
    else if (file1 < file2) {
        file_lock(file1, 0);
        file_lock(file2, 0);
    }
    else {
        file_lock(file2, 0);
        file_lock(file1, 0);
    }
}

void file_unlock(struct file *file, int refcount) {
    // very important that refcount == 0 wouldn't trigger free
    if (refcount && 0 == (file->f_refcnt -= refcount)) {

        if (file->f_task)
            panic("file_unlock destruct has task");
        
        filedealloc_locked(file);
        return;
    }
    pthread_mutex_unlock(&file->f_lock);
}

char *normalize_path(const char *dirname, const char *file_name, char *out_path);

/**
 * returns locked with refcnt
 */
struct file *file_open(int dirfd, const char *dirpath, const char *pathname, int flags, mode_t mode, uid_t uid, gid_t gid)
{
    struct file *file = filealloc();
    if (!file)
        return NULL;

    file->f_refcnt++;
    file_lock(file, 0);

    // The pathname the file is known by, recorded in f_pathname for the
    // *at() calls that TVM answers by path (dirfd_normalize_path). An open
    // relative to a directory descriptor used to record nothing, so a
    // directory reached through openat(dirfd, "sub") had no name and every
    // later fstatat(fd, "entry")/faccessat(...) on it resolved "entry"
    // against the task's cwd instead (see dirfd_normalize_path). toybox's
    // dirtree walks a tree exactly that way -- openat(parentfd, name) per
    // directory, fstatat(dirfd, entry) per entry -- so `rm -rf dir` failed on
    // anything two levels deep ("rm: dir/a/f: No such file or directory",
    // then "Directory not empty"), which every git working tree is. The
    // name is now the directory's recorded name joined with the relative
    // one (normalized, so ".." components fold), and that absolute name is
    // also what the devices (tty, cpio VFS) are asked about, so a
    // dirfd-relative open of a /tvm entry finds it too. The host open keeps
    // the (dirfd, relative) form, so the kernel's own semantics -- following
    // a directory across a rename, say -- are unchanged.
    char pathname_abs[PATH_MAX];
    if (pathname[0] != '/' && dirfd != AT_FDCWD && dirpath) {
        if (!normalize_path(dirpath, pathname, pathname_abs)) {
            filedealloc_locked(file);
            return NULL;
        }
    }
    const char *devpath = (pathname[0] != '/' && dirfd != AT_FDCWD && dirpath) ? pathname_abs : pathname;

    if (devpath[0] == '/') {
        file->f_pathname = kstrdup(devpath);
        if (!file->f_pathname) {
            filedealloc_locked(file);
            return NULL;
        }
    }

    struct device *dev;
    list_for_each_entry(dev, &devices, dev_list) {
        int last_errno = errno;
        if (0 == dev->dev_fops.f_open(devpath, flags, mode, uid, gid, file)) {
            return file;
        }

        if (errno != ENOTSUP) {
            filedealloc_locked(file);
            return NULL;
        }

        errno = last_errno;
    };

    int rfd = CALL_FUNC(openat, dirfd, pathname, flags, mode);
    if (rfd == -1) {
        filedealloc_locked(file);
        return NULL;
    }

    file->f_rfd = rfd_adopt(rfd);
    return file;
}

/**
 * returns with locked and refcnt
 */
struct file *file_dup_locked(struct file *oldfile) {
    // we dup the rfd outside f_dup() and call it with the rfd already duped in f_rfd
    int newfd = CALL_FUNC(dup, oldfile->f_rfd);
    if (newfd == -1)
        return NULL;

    struct file *newfile = filealloc_from_rfd(newfd);
    if (!newfile) {
        if (0 != CALL_FUNC(close, newfd))
            panic("file_dup_locked close");
        return NULL;
    }

    newfile->f_refcnt++;
    newfile->f_type = oldfile->f_type;
    newfile->f_ops = oldfile->f_ops;
    // The name goes with the duplicate: fdopendir(dup(dirfd)) followed by
    // fstatat(dirfd(dir), entry) is the portable way to walk a directory,
    // and the duplicate must answer by path like the original.
    if (oldfile->f_pathname)
        newfile->f_pathname = kstrdup(oldfile->f_pathname);
    newfile->f_nosigpipe = oldfile->f_nosigpipe;

    // we can blatantly lock although oldfile is locked, as it was just created
    file_lock(newfile, 0);

    if (oldfile->f_ops && oldfile->f_ops->f_dup && 0 != oldfile->f_ops->f_dup(oldfile, newfile)) {
        newfile->f_ops = NULL;
        filedealloc_locked(newfile);
        return NULL;
    }

    return newfile;
}

int file_access(const char *pathname, int mode, int flags) {
    struct stat st;
    struct device *dev;
    list_for_each_entry(dev, &devices, dev_list) {
        int last_errno = errno;
        if (0 == dev->dev_fops.f_stat(pathname, &st, flags)) {
            if (mode == 0 || mode == F_OK)
                return 0;
            if ((mode & X_OK) && (st.st_mode & (S_IXUSR | S_IXGRP | S_IXOTH)))
                return 0;
            if ((mode & W_OK) && (st.st_mode & (S_IWUSR | S_IWGRP | S_IWOTH)))
                return 0;
            if ((mode & R_OK) && (st.st_mode & (S_IRUSR | S_IRGRP | S_IROTH)))
                return 0;
            errno = EACCES;
            return -1;
        }

        if (errno != ENOTSUP) {
            return -1;
        }
        
        errno = last_errno;
    };

    return CALL_FUNC(faccessat, AT_FDCWD, pathname, mode, flags);
}

int file_stat(const char *pathname, struct stat *statbuf, int flag)
{
    struct device *dev;
    list_for_each_entry(dev, &devices, dev_list) {
        int last_errno = errno;
        if (0 == dev->dev_fops.f_stat(pathname, statbuf, flag)) {
            return 0;
        }

        if (errno != ENOTSUP) {
            return -1;
        }
        
        errno = last_errno;
    };

    return CALL_FUNC(fstatat, AT_FDCWD, pathname, statbuf, flag);
}

int file_chmod(const char *pathname, mode_t mode, int flag)
{
    struct device *dev;
    list_for_each_entry(dev, &devices, dev_list) {
        int last_errno = errno;
        if (0 == dev->dev_fops.f_chmod(pathname, mode, flag)) {
            return 0;
        }

        if (errno != ENOTSUP) {
            return -1;
        }
        
        errno = last_errno;
    };

    return CALL_FUNC(fchmodat, AT_FDCWD, pathname, mode, flag);
}

int file_chown(const char *pathname, uid_t owner, gid_t group, int flag)
{
    struct device *dev;
    list_for_each_entry(dev, &devices, dev_list) {
        int last_errno = errno;
        if (0 == dev->dev_fops.f_chown(pathname, owner, group, flag)) {
            return 0;
        }

        if (errno != ENOTSUP) {
            return -1;
        }
        
        errno = last_errno;
    };

    return CALL_FUNC(fchownat, AT_FDCWD, pathname, owner, group, flag);
}

ssize_t file_readlink(const char *pathname, char *buf, size_t bufsiz)
{
    struct device *dev;
    list_for_each_entry(dev, &devices, dev_list) {
        if (!dev->dev_fops.f_readlink)
            continue;
        
        int last_errno = errno;
        ssize_t ret = dev->dev_fops.f_readlink(pathname, buf, bufsiz);
        if (ret >= 0)
            return ret;

        if (errno != ENOTSUP)
            return -1;

        errno = last_errno;
    };

    return CALL_FUNC(readlink, pathname, buf, bufsiz);
}

/////////////
// MARK: Task Data
/////////////

/**
 * This is TVM's "data section" for each program. 
 */
struct task_data {
    char **__environ;

    char  *optarg;
    int    optind, opterr, optopt, optpos, optreset;

    // Per-task buffers for non-reentrant libc wrappers.
    // String backing data (pw_strings, gr_strings) is malloc'd
    // from the task arena at the right size.
    struct passwd  pw_buf;
    char          *pw_strings;
    struct group   gr_buf;
    char          *gr_strings;
    struct tm      tm_buf;
    char           ctime_buf[26];
    char           strerror_buf[256];
    int            usershell_idx;
};

/**
 * Executables may override storage for these in their own data segment.
 * This is an indirection struct that imitates relocations, we access variables here rather than from task_data.
 */
struct task_relocations {
    char ***__environ;

    char **optarg;
    int *optind, *opterr, *optopt;
};

/////////////
// MARK: Task
/////////////

/**
 * this is the fulcrum of tvm
 * 
 * This section basically implements our little kernel.
 * Here we do resource management at the task ("process") level.
 * 
 * Every process created with `fork()` is represented as a task,
 * with its own set of file-descriptors, signal handlers and other info.
 * 
 * On `fork()`, we inherit much of the information from our parent,
 * except that we `dup()` all file-descriptors.
 * 
 * In addition, we need to manage links between parents, children and sessions/process groups
 * so we could implement proper `kill(2)`/`waitpid(2)` semantics.
 * 
 * TODO:
 * - Locks in this section were implemented very badly
 * - `setrlimit()`/`getrlimit()`
 */

#define MAX_FILES 1024
#define MAX_SIGNALS 32
#define MAX_TIMERS_PER_TASK 8
#define MAX_VIRTUAL_TIMERS 256

#define TS_ZOMBIE  0x1
#define TS_CTTY    0x2
#define TS_EXITING 0x4  // a thread is tearing this task down; racing terminators must not re-enter the drain loop
#define TS_REAPED  0x8  // a waitpid() has claimed this zombie; task_next_zombie_child must skip it

typedef void (*exit_t)(void);

struct task {
    struct list_head tsk_list;
    struct task *tsk_parent;
    unsigned tsk_refcount;
    pid_t tsk_pid;
    int tsk_result;  // wstatus as returned from `waitpid()`
    struct rusage tsk_rusage;       // CPU time of the exiting thread, for wait4()/wait3()
    struct rusage tsk_rusage_base;  // that thread's CPU time when the task started on it
    pthread_mutex_t tsk_lock;
    struct file *tsk_files[MAX_FILES];
    struct list_head tsk_filestreams;  // file_stream_container
    struct file_stream *tsk_io_filestreams[3];
    int tsk_state;
    pthread_cond_t tsk_wait_cond;
    struct sigaction tsk_sighandlers[MAX_SIGNALS];
    unsigned tsk_threads_cnt;
    struct list_head tsk_threads;  // thread
    pid_t tsk_sid;
    pid_t tsk_pgid;
    char tsk_comm[16];
    struct rmem_arena *tsk_ar;
    struct ulinker *tsk_linker;
    char tsk_cwd[PATH_MAX];
    exit_t tsk_exits[32];
    void *tsk_exit_args[32];  // argument for each tsk_exits entry (__cxa_atexit), NULL for atexit
    uint32_t tsk_pending_sigs;  // bitmask of deferred signals (atomic via __atomic builtins)
    siginfo_t tsk_pending_siginfo[MAX_SIGNALS];
    ucontext_t tsk_pending_ucontext[MAX_SIGNALS];
    struct task_data *tsk_data; // malloc()ed from tsk_ar
    struct task_relocations tsk_relocs; // oh my god
    int tsk_timers[MAX_TIMERS_PER_TASK]; // indices into vtimers[], -1 = unused
    int tsk_itimer_real; // tsk_timers[] index for ITIMER_REAL, -1 = unset
    struct thread *tsk_exiting_thread; // thread running terminate_current_locked; written under tasks_lock+tsk_lock, read with __atomic (lock-free checks in tvm_sig_leave/waitpid)
    mode_t tsk_umask; // per-task umask (real umask is process-wide, can't share across tasks)
    unsigned tsk_rlocks; // POSIX record locks held (entries in rlocks owned by this task, see fcntl_rlock)
};

struct thread {
    struct list_head thr_list;
    struct task *thr_task;  // back pointer to owning task
    pthread_t thr_pthread;
    pthread_attr_t thr_pthread_attr;
    uint64_t thr_refcount;
    struct emulator_context *thr_emulator;
    void *thr_fork_fp;  // copy frames from sp to this on fork()
    int thr_sig_blocked;  // nonzero when signals are deferred on this thread (atomic via __atomic builtins)
    uint32_t thr_pending_sigs;  // thread-directed deferred signals (atomic via __atomic builtins)
    siginfo_t thr_pending_siginfo[MAX_SIGNALS];
    ucontext_t thr_pending_ucontext[MAX_SIGNALS];
    sigset_t thr_sigmask;  // virtual signal mask (sigprocmask)
};

static struct rmem_arena *global_arena = NULL;
static struct task *main_task = NULL;
static pthread_mutex_t tasks_lock = PTHREAD_MUTEX_INITIALIZER;
struct tvm_call;
typedef int (*compar_t)(const void *, const void *);

struct tvm_tls {
    struct task *tls_current;
    struct thread *tls_current_thread;

    int tls_thread_tvm_disallow;
    int tls_tvm_sig_blocked;
    int tls_tvm_sig_draining;
    int tls_tvm_sig_interrupted;
    int tls_tvm_sig_depth;
    int tls_tvm_sig_synchronous_depth;
    int tls_tvm_in_emu;
    int tls_tvm_emu_call_depth; // >0 while inside emulated execution in emulator_call_ex; signals defer and drain only at the emulator_step safe point
    int tls_sigshutup;
    int tls_tasks_lock_held; // set during task teardown when tasks_lock is held

    struct tvm_call *tls_call;

    compar_t tls_bsearch_compar;
    compar_t tls_qsort_compar;
#if defined(__APPLE__)
    CFDictionaryApplierFunction tls_CFDictionaryApplyFunction_function;
    // sysctl(): the last name-to-mib translation, see the interposer
    int    tls_sysctl_mib[CTL_MAXNAME];
    u_int  tls_sysctl_miblen;
    char   tls_sysctl_name[256];
#endif

    char tls_tty_name[PATH_MAX];
    char tls_pts_name[PATH_MAX];
};

static pthread_key_t tls_key;
static int tls_initialized = 0;

static void taskdealloc(struct task *t);
void threaddealloc(struct thread *thr);

static void tvm_thread_destructor(void *arg) {
    struct tvm_tls *tls = arg;
    // glibc clears the key value BEFORE invoking the destructor, but the
    // teardown below is VM code (emulator/task destruction reads TLS, e.g.
    // TVM_OFF).  Chokepoint rule: re-install the TLS for the teardown's
    // duration, and clear it at the end so glibc doesn't re-run us (it loops
    // while any key value is non-NULL).
    pthread_setspecific(tls_key, tls);
    if (tls->tls_current_thread)
        threaddealloc(tls->tls_current_thread);
    if (tls->tls_current)
        taskdealloc(tls->tls_current);
    pthread_setspecific(tls_key, NULL);
    kfree(tls);
}

// Peek at this thread's TLS without asserting it exists. ONLY for the VM-enter
// chokepoints (FUNCTION_DEFINE wrappers, signal_handler, can_enter_tvm, panic):
// they must gate on "no TLS -> host behavior" before any code that calls
// tvm_tls(). Everything else calls tvm_tls(), which treats a missing TLS as a
// fatal bug (a code path that slipped past every chokepoint).
#define TVM_TLS_PEEK() (tls_initialized ? (struct tvm_tls *)pthread_getspecific(tls_key) : NULL)

static inline struct tvm_tls *tvm_tls(void) {
    struct tvm_tls *t = TVM_TLS_PEEK();
    if (!t) {
        // No per-thread TLS and somebody still reached VM state: missed
        // chokepoint. Raw report + abort — panic()/INFO read TLS via this
        // very function and would recurse.
        static const char __msg[] = "tvm_tls: no thread TLS (code path missed a VM-enter chokepoint)\n";
        CALL_FUNC(write, 2, __msg, sizeof(__msg) - 1);
        CALL_FUNC(abort);
    }
    return t;
}

#define current                     (tvm_tls()->tls_current)
#define current_thread              (tvm_tls()->tls_current_thread)
#define thread_tvm_disallow         (tvm_tls()->tls_thread_tvm_disallow)
#define tvm_sig_blocked             (tvm_tls()->tls_tvm_sig_blocked)
#define tvm_sig_draining            (tvm_tls()->tls_tvm_sig_draining)
#define tvm_sig_interrupted         (tvm_tls()->tls_tvm_sig_interrupted)
#define tvm_sig_depth               (tvm_tls()->tls_tvm_sig_depth)
#define tvm_sig_synchronous_depth   (tvm_tls()->tls_tvm_sig_synchronous_depth)
#define tvm_in_emu                  (tvm_tls()->tls_tvm_in_emu)
#define tvm_emu_call_depth          (tvm_tls()->tls_tvm_emu_call_depth)
#define tasks_lock_held             (tvm_tls()->tls_tasks_lock_held)
#define sigshutup                   (tvm_tls()->tls_sigshutup)
#define call                        (tvm_tls()->tls_call)
#define bsearch_compar              (tvm_tls()->tls_bsearch_compar)
#define qsort_compar                (tvm_tls()->tls_qsort_compar)
#if defined(__APPLE__)
#define CFDictionaryApplyFunction_function (tvm_tls()->tls_CFDictionaryApplyFunction_function)
#endif
#define tty_name                    (tvm_tls()->tls_tty_name)
#define pts_name                    (tvm_tls()->tls_pts_name)

#define TVM_ON  do { thread_tvm_disallow = 0; } while (0)
#define TVM_OFF ({ int __val = thread_tvm_disallow; thread_tvm_disallow = 1; __val; })
#define TVM_RESTORE(_val) do { thread_tvm_disallow = (_val); } while (0)

static void task_lock(struct task *t)
{
    pthread_mutex_lock(&t->tsk_lock);
}
//#define task_lock(t) ({ INFO("task_lock %p (" __FILE__ ":%d)", t, __LINE__); _task_lock(t); })

static void task_unlock(struct task *t)
{
    pthread_mutex_unlock(&t->tsk_lock);
}

/**
 * returns locked and refcnt
 */
static struct file *file_for_task_locked(struct task *t, int nfd) {
    if (nfd < 0 || nfd >= MAX_FILES)
        return NULL;

    if (!t->tsk_files[nfd])
        return NULL;

    file_lock(t->tsk_files[nfd], 1);

    return t->tsk_files[nfd];
}

/**
 * returns locked and refcnt
 */
static struct file *file_for_task(struct task *t, int nfd) {
    task_lock(t);
    struct file *file = file_for_task_locked(t, nfd);
    task_unlock(t);
    return file;
}

/**
 * Execute `Expression` with refcount on `Task`'s `NFD` refcounted
 *   and with reference to real file-descriptor `__rfd`.
 * If `NFD` is invalid, `Expression` is executed with `__rfd = -1`.
 */
#define WITH_RFD(Task, NFD, Expression) ({                          \
    long __RET;                                                     \
    tvm_sig_enter();                                                \
    struct file *__FILE = file_for_task((Task), (NFD));             \
    int __rfd = (__FILE ? __FILE->f_rfd : -1);                      \
    if ((NFD) < 0) { __rfd = (NFD); }  /*AT_FDCWD*/                 \
    if (__FILE) file_unlock(__FILE, 0);                             \
    __RET = (Expression);                                           \
    if (__FILE) { file_lock(__FILE, 0); file_unlock(__FILE, 1); }   \
    tvm_sig_leave();                                                \
    __RET;                                                          \
})

/**
 * Execute `Expression` with refcount on current task's `NFD` refcounted
 *   and with reference to real file-dwescriptor `__rfd`.
 * If `NFD` is invalid, `Expression` is executed with `__rfd = -1`.
 */
#define CURRENT_RFD(NFD, Expression) WITH_RFD(current, NFD, Expression)

/**
 * Execute `Expression` with refcount on `Task`'s `NFD` refcounted
 *   and with reference to real file struct `__file`.
 * If `NFD` is invalid, `Expression` is not executed and instead -1 with EBADF is returned.
 */
#define WITH_FILE(Task, NFD, Expression) ({                         \
    int __RET = -1;                                                 \
    tvm_sig_enter();                                                \
    struct file *__file = file_for_task((Task), (NFD));             \
    if (!__file) { errno = EBADF; }                                 \
    else {                                                          \
        __RET = (Expression);                                       \
        file_unlock(__file, 1);                                     \
    }                                                               \
    tvm_sig_leave();                                                \
    __RET;                                                          \
})

/**
 * Execute `Expression` with refcount on current task's `NFD` refcounted
 *   and with reference to real file struct `__file`.
 * If `NFD` is invalid, `Expression` is not executed and instead -1 with EBADF is returned.
 */
#define CURRENT_FILE(NFD, Expression) WITH_FILE(current, NFD, Expression)

/**
 * look for rfd in task and return nfd if found or -1 if not
 */
static int task_fd_for_rfd(struct task *t, int fd, int lock)
{
    if (!main_task)
        return fd;
    
    if (lock)
        task_lock(t);
    
    if (fd >= 0) {
        for (int i = 0; i < MAX_FILES; i++) {
            if (!t->tsk_files[i])
                continue;

            if (fd == t->tsk_files[i]->f_rfd) {
                if (lock)
                    task_unlock(t);
                return i;
            }
        }
    }

    if (lock)
        task_unlock(t);
    return -1;
}

static int t_fdr(int fd, int lock)
{
    return task_fd_for_rfd(current, fd, lock);
}

static struct task *taskalloc_common()
{
    struct task *ret = NULL;
    struct task *t = kalloc(sizeof(struct task));
    if (!t)
        goto out;
    
    memset(t, 0, sizeof(*t));

    for (int i = 0; i < MAX_TIMERS_PER_TASK; i++)
        t->tsk_timers[i] = -1;
    t->tsk_itimer_real = -1;

    INIT_LIST_HEAD(&t->tsk_list);
    INIT_LIST_HEAD(&t->tsk_threads);
    INIT_LIST_HEAD(&t->tsk_filestreams);

    t->tsk_refcount = 1;

    t->tsk_ar = rmem_arena_create(t);
    if (!t->tsk_ar)
        goto out;

    t->tsk_linker = ulinker_create((void *)t);
    if (!t->tsk_linker)
        goto out;

    if (pthread_mutex_init(&t->tsk_lock, NULL) != 0) {
        goto out;
    }
    if (pthread_cond_init(&t->tsk_wait_cond, NULL) != 0) {
        goto out;
    }

    ret = t;

out:
    if (!ret) {
        if (t) {
            kfree(t);
            t = NULL;
        }
    }
    return ret;
}

static void taskdealloc(struct task *t)
{
    if (t->tsk_threads_cnt != 0)
        panic("taskdealloc tsk_threads_cnt %u", t->tsk_threads_cnt);

    if (t->tsk_linker) {
        ulinker_destroy(t->tsk_linker);
        t->tsk_linker = NULL;
    }

    rmem_arena_destroy(t->tsk_ar);
    t->tsk_ar = NULL;
    t->tsk_data = NULL; // allocated in the task arena

    // tsk_lock must be unheld here: unlocking it unconditionally used to
    // release a mutex this thread didn't own in the waitpid-reap path (UB that
    // could break a concurrent holder's critical section). The auto-reap path
    // unlocks it explicitly in terminate_current_locked before pthread_exit.
    pthread_cond_destroy(&t->tsk_wait_cond);
    pthread_mutex_destroy(&t->tsk_lock);

    kfree(t);
}

/////////////
// MARK: POSIX record locks
/////////////

// fcntl() byte-range locks (F_SETLK/F_SETLKW/F_GETLK) are owned by a process:
// they conflict only between processes, and a process loses every lock it
// holds on a file when it closes *any* descriptor of that file. All TVM tasks
// live in the one host process, so passing the calls straight to the kernel
// made every task a single owner: no request of one task ever conflicted with
// a lock another task held, and task B merely opening and closing a file
// dropped task A's locks on it. sqlite3 is built on these locks (SHARED /
// RESERVED / PENDING / EXCLUSIVE are byte ranges of the database file): a
// second sqlite3 inserting into a database on which a first one held `BEGIN
// IMMEDIATE` was not told SQLITE_BUSY, its write went through, and the first
// one's COMMIT then failed with "disk I/O error" because its kernel locks had
// gone away with the second one's close().
//
// The table below gives each task its own owner identity: a request is
// checked against, and recorded in, the table first, and only then handed to
// the kernel, so processes outside TVM still see it. Entries are keyed by
// (st_dev, st_ino), so every descriptor of a file, dup'd or reopened, shares
// them; a task's locks on a file go when it closes any descriptor of the
// file (POSIX) or exits. The kernel side keeps its per-process semantics --
// an unlock or close by one task also drops what the kernel held for another
// -- which only a process outside TVM can observe. Host-backed regular files
// only: the read-only cpio VFS, ttys, pipes and sockets pass through as
// before.

struct rlock {
    struct list_head rl_list;
    dev_t rl_dev;
    ino_t rl_ino;
    struct task *rl_owner;
    short rl_type;      // F_RDLCK or F_WRLCK
    off_t rl_start;
    off_t rl_end;       // inclusive; RLOCK_EOF for "to the end of the file and beyond"
};

#define RLOCK_EOF ((off_t)INT64_MAX)

static LIST_HEAD(rlocks);
static pthread_mutex_t rlocks_lock = PTHREAD_MUTEX_INITIALIZER; // leaf: never held while taking another lock
static pthread_cond_t rlocks_cond = PTHREAD_COND_INITIALIZER;   // broadcast on every release, for F_SETLKW waiters

// F_SETLKW waiters, each "waiter is blocked on a lock owner holds", for
// deadlock detection. Entries live on the waiting thread's stack and are
// linked only while it sleeps, under rlocks_lock.
struct rlock_waiter {
    struct list_head rw_list;
    struct task *rw_waiter;
    struct task *rw_owner;
};
static LIST_HEAD(rlock_waiters);

// Would `self` blocking on a lock held by `owner` close a cycle? Follows
// owner -> the owner it is itself waiting for -> ... A kernel refuses such a
// wait with EDEADLK; without this, two tasks that each held a lock the other
// wanted slept forever. Pointers are only compared, never dereferenced, so a
// stale entry for a task that has just exited cannot fault. Called with
// rlocks_lock held.
static int rlock_would_deadlock_locked(struct task *self, struct task *owner)
{
    for (int hops = 0; owner && hops < 1024; hops++) {
        if (owner == self)
            return 1;
        struct task *next = NULL;
        struct rlock_waiter *w;
        list_for_each_entry(w, &rlock_waiters, rw_list) {
            if (w->rw_waiter == owner) {
                next = w->rw_owner;
                break;
            }
        }
        owner = next;
    }
    return 0;
}

// The byte range [*start, *end] a struct flock denotes, SEEK_CUR/SEEK_END
// resolved against the real descriptor. -1 with errno on a bad range.
static int rlock_range(int rfd, const struct flock *fl, off_t *start, off_t *end)
{
    off_t base;
    switch (fl->l_whence) {
    case SEEK_SET:
        base = 0;
        break;
    case SEEK_CUR:
        if (-1 == (base = CALL_FUNC(lseek, rfd, 0, SEEK_CUR)))
            return -1;
        break;
    case SEEK_END: {
        struct stat st;
        if (-1 == CALL_FUNC(fstat, rfd, &st))
            return -1;
        base = st.st_size;
        break;
    }
    default:
        errno = EINVAL;
        return -1;
    }

    off_t s = base + fl->l_start, e;
    if (fl->l_len > 0)
        e = s + fl->l_len - 1;
    else if (fl->l_len == 0)
        e = RLOCK_EOF;
    else {
        e = s - 1;
        s += fl->l_len;
    }
    if (s < 0 || e < s) {
        errno = EINVAL;
        return -1;
    }
    *start = s;
    *end = e;
    return 0;
}

// A lock of another task that `type` on [start, end] of the file cannot
// coexist with (two read locks can, anything with a write lock cannot).
static struct rlock *rlock_conflict_locked(dev_t dev, ino_t ino, struct task *owner, short type, off_t start, off_t end)
{
    struct rlock *rl;
    list_for_each_entry(rl, &rlocks, rl_list) {
        if (rl->rl_dev != dev || rl->rl_ino != ino || rl->rl_owner == owner)
            continue;
        if (rl->rl_end < start || rl->rl_start > end)
            continue;
        if (type == F_WRLCK || rl->rl_type == F_WRLCK)
            return rl;
    }
    return NULL;
}

// Replace whatever `owner` holds on [start, end] of the file by `type`
// (F_UNLCK: by nothing), splitting an entry that sticks out on both sides.
static int rlock_apply_locked(dev_t dev, ino_t ino, struct task *owner, short type, off_t start, off_t end)
{
    struct rlock *rl, *next;
    for (rl = list_first_entry(&rlocks, struct rlock, rl_list);
         !list_entry_is_head(rl, &rlocks, rl_list); rl = next) {
        next = list_next_entry(rl, rl_list);
        if (rl->rl_dev != dev || rl->rl_ino != ino || rl->rl_owner != owner)
            continue;
        if (rl->rl_end < start || rl->rl_start > end)
            continue;
        if (rl->rl_start < start && rl->rl_end > end) {
            struct rlock *tail = kalloc(sizeof(*tail));
            if (!tail) {
                errno = ENOLCK;
                return -1;
            }
            *tail = *rl;
            tail->rl_start = end + 1;
            rl->rl_end = start - 1;
            list_add(&tail->rl_list, &rl->rl_list);
            owner->tsk_rlocks++;
        }
        else if (rl->rl_start < start)
            rl->rl_end = start - 1;
        else if (rl->rl_end > end)
            rl->rl_start = end + 1;
        else {
            list_del(&rl->rl_list);
            kfree(rl);
            owner->tsk_rlocks--;
        }
    }

    if (type == F_UNLCK)
        return 0;

    rl = kalloc(sizeof(*rl));
    if (!rl) {
        errno = ENOLCK;
        return -1;
    }
    rl->rl_dev = dev;
    rl->rl_ino = ino;
    rl->rl_owner = owner;
    rl->rl_type = type;
    rl->rl_start = start;
    rl->rl_end = end;
    list_add(&rl->rl_list, &rlocks);
    owner->tsk_rlocks++;
    return 0;
}

// Drop every lock `t` holds on the file behind `rfd` (a close of any of the
// file's descriptors, POSIX) or, with rfd -1, on every file (task exit).
static void rlock_release(struct task *t, int rfd)
{
    struct stat st;
    if (rfd >= 0 && (-1 == CALL_FUNC(fstat, rfd, &st) || !S_ISREG(st.st_mode)))
        return;

    pthread_mutex_lock(&rlocks_lock);
    struct rlock *rl, *next;
    for (rl = list_first_entry(&rlocks, struct rlock, rl_list);
         !list_entry_is_head(rl, &rlocks, rl_list); rl = next) {
        next = list_next_entry(rl, rl_list);
        if (rl->rl_owner != t)
            continue;
        if (rfd >= 0 && (rl->rl_dev != st.st_dev || rl->rl_ino != st.st_ino))
            continue;
        list_del(&rl->rl_list);
        kfree(rl);
        t->tsk_rlocks--;
    }
    pthread_cond_broadcast(&rlocks_cond);
    pthread_mutex_unlock(&rlocks_lock);
}

/**
 * tasks_lock must either be locked or dolocktasks=1
 * locks children
 */
static void taskfreelastref(struct task *t, int dolocktasks, int dealloc)
{
    if (dolocktasks)
        pthread_mutex_lock(&tasks_lock);

    // tasks_lock is held for the whole body (either we locked it or the caller
    // did). Closing this task's files below can reach ttyop_close, which would
    // otherwise re-acquire tasks_lock (directly or via kill()) and self-deadlock.
    int prev_tasks_lock_held = tasks_lock_held;
    tasks_lock_held = 1;

    pthread_mutex_lock(&vtimers_lock);
    for (int i = 0; i < MAX_TIMERS_PER_TASK; i++) {
        if (t->tsk_timers[i] >= 0) {
            vtimer_destroy_locked(t->tsk_timers[i]);
            t->tsk_timers[i] = -1;
        }
    }
    t->tsk_itimer_real = -1;
    pthread_mutex_unlock(&vtimers_lock);

    task_filestreamdealloc_list(t, &t->tsk_filestreams);

    for (int i = 0; i < 3; i++)
        t->tsk_io_filestreams[i] = NULL;

    // POSIX record locks die with the task (the files below are released
    // without going through close(), which is where they would otherwise go).
    if (t->tsk_rlocks)
        rlock_release(t, -1);

    for (int i = 0; i < MAX_FILES; i++) {
        struct file *file = file_for_task_locked(t, i);
        if (!file)
            continue;
        task_unassign_file_locked(t, file);
        file_unlock(file, 1);
    }

    struct task *child;
    list_for_each_entry(child, &main_task->tsk_list, tsk_list) {
        if (child->tsk_parent == t) {
            task_lock(child);
            child->tsk_parent = NULL;
            task_unlock(child);
        }
    }

    if (dealloc) {
        list_del(&t->tsk_list);
        taskdealloc(t);
    }

    tasks_lock_held = prev_tasks_lock_held;

    if (dolocktasks)
        pthread_mutex_unlock(&tasks_lock);
}

/**
 * locks tasks_lock and current.
 */
static struct task *taskalloc()
{
    struct task *ret = NULL;

    struct task *t = taskalloc_common();
    if (!t)
        return NULL;
    
    task_lock(current);
    task_lock(t);

    t->tsk_parent = current;
    t->tsk_pid = -1;
    t->tsk_sid = current->tsk_sid;
    t->tsk_pgid = current->tsk_pgid;
    t->tsk_state = current->tsk_state & TS_CTTY;
    memcpy(t->tsk_comm, current->tsk_comm, sizeof(t->tsk_comm));
    memcpy(&t->tsk_exits, &current->tsk_exits, sizeof(t->tsk_exits));
    memcpy(&t->tsk_exit_args, &current->tsk_exit_args, sizeof(t->tsk_exit_args));

    // dup all files (file streams fixed up by memory duplication)
    for (int i = 0; i < MAX_FILES; i++) {
        if (!current->tsk_files[i])
            continue;
        
        // will never dealloc because current is locked and has ref on file
        file_lock(current->tsk_files[i], 1);
        struct file *file = file_dup_locked(current->tsk_files[i]);
        // The child's descriptor is a host dup(), and dup() clears
        // FD_CLOEXEC; a fork must keep it. Without this every descriptor a
        // program marked close-on-exec lost the mark in its fork children and
        // leaked into whatever they exec'd -- e.g. popen()'s pipes stayed open
        // in later popen() children, so pclose() of the first could hang.
        if (file) {
            int fdflags = CALL_FUNC(fcntl, current->tsk_files[i]->f_rfd, F_GETFD);
            if (fdflags != -1 && (fdflags & FD_CLOEXEC))
                (void)CALL_FUNC(fcntl, file->f_rfd, F_SETFD, FD_CLOEXEC);
        }
        file_unlock(current->tsk_files[i], 1);
        if (!file)
            goto out;

        if (i != task_assign_file_locked(t, file, i))
            panic("taskalloc task_assign_file_locked %d", i);
        
        // t has ref, will never dealloc
        file_unlock(file, 1);
    }

    // do not duplicate file-streams just yet

    memcpy(t->tsk_sighandlers, t->tsk_parent->tsk_sighandlers, sizeof(t->tsk_sighandlers));

    t->tsk_cwd[0] = 0;
    strncpy(t->tsk_cwd, current->tsk_cwd, sizeof(t->tsk_cwd));
    t->tsk_umask = current->tsk_umask;

    ret = t;

out:

    task_unlock(t);
    task_unlock(current);
    if (!ret) {
        if (t) {
            taskfreelastref(t, 1, 1);
            t = NULL;
        }
    }

    return ret;
}

/**
 * refcnt
 */
static int task_assign_file_locked(struct task *t, struct file *file, int min_fd)
{
    if (min_fd < 0 || min_fd >= MAX_FILES)
        panic("task_assign_file_locked %d", min_fd);
    
    for (int i = min_fd; i < MAX_FILES; i++) {
        if (!t->tsk_files[i]) {
            file->f_refcnt++;
            file->f_nfd = i;
            file->f_task = t;
            t->tsk_files[i] = file;
            return i;
        }
    }
    
    return -1;
}

/**
 * both locked
 */
static void task_unassign_file_locked(struct task *t, struct file *file) {
    VERIFY(file->f_task == t);
    int fd = file->f_nfd;
    VERIFY(fd >= 0 && fd < MAX_FILES);
    VERIFY(file == t->tsk_files[fd]);

    file->f_task = NULL;
    file->f_nfd = -1;
    file->f_refcnt--; // cannot be 0, because file_for_task_locked() refcnt
    t->tsk_files[fd] = NULL;
}

/**
 * returns locked without refcnt
 */
static struct file *task_unassign_fd_locked(struct task *t, int fd) {
    struct file *file = file_for_task_locked(current, fd);
    if (!file)
        return NULL;

    VERIFY(file->f_nfd == fd);
    task_unassign_file_locked(t, file);
    return file;
}

/**
 * returns locked and refcnt
 */
static struct file *task_new_regular_file_locked(struct task *t, int rfd) {
    struct file *file = filealloc();
    if (!file)
        return NULL;

    file->f_refcnt++;
    file_lock(file, 0);

    if (-1 == task_assign_file_locked(t, file, 0)) {
        // no ref back to task, okay to have task locked
        filedealloc_locked(file);
        return NULL;   
    }

    file->f_rfd = rfd_adopt(rfd);

    return file;
}

static int task_new_fd(struct task *t, int rfd) {
    tvm_sig_enter();
    task_lock(t);
    struct file *file = task_new_regular_file_locked(t, rfd);
    if (!file) {
        task_unlock(t);
        tvm_sig_leave();
        return -1;
    }

    int fd = file->f_nfd;
    task_unlock(t);
    file_unlock(file, 1);
    tvm_sig_leave();
    return fd;
}

/**
 * since lock ordering is task > file
 * lock current and keep lock order semantics
 */
static void task_lock_with_file_locked(struct task *task, struct file *file)
{
    VERIFY(file->f_task == task);
    file_unlock(file, 0);
    task_lock(task);
    file_lock(file, 0);
}

/**
 * tasks_lock must be locked (or dolocktasks=1)
 * returns locked task
 */
static struct task *task_lookup_pid(pid_t pid, int dolocktasks) {
    if (main_task->tsk_pid == pid) {
        // callers task_unlock() the result — returning it unlocked would make
        // them release a mutex they don't own (possibly held by another thread)
        task_lock(main_task);
        return main_task;
    }

    if (dolocktasks)
        pthread_mutex_lock(&tasks_lock);
    
    struct task *t;
    list_for_each_entry(t, &main_task->tsk_list, tsk_list) {
        if (t->tsk_pid == pid) {
            break;
        }
    }

    // TODO is this okay?
    if (t == main_task) {
        if (dolocktasks)
            pthread_mutex_unlock(&tasks_lock);
        return NULL;
    }

    task_lock(t);
    if (dolocktasks)
        pthread_mutex_unlock(&tasks_lock);

    return t;
}

/**
 * locks tasks_lock
 * returns locked task
 */
static struct task *task_for_rfd(int rfd, int *out_nfd) {
    if (rfd < 0)
        return NULL;
    
    pthread_mutex_lock(&tasks_lock);
    struct task *t = main_task;
    do {
        task_lock(t);
        int nfd = task_fd_for_rfd(t, rfd, 0);
        if (nfd != -1) {
            if (out_nfd)
                *out_nfd = nfd;
            pthread_mutex_unlock(&tasks_lock);
            return t;
        }

        task_unlock(t);
    }
    while ( ({ t = list_next_entry(t, tsk_list); !list_entry_is_head(t, &main_task->tsk_list, tsk_list); }) );
    pthread_mutex_unlock(&tasks_lock);
    return NULL;
}

/**
 * both tasks_lock and t locked
 * posix API with errno
 */
static int task_set_pgid(struct task *t, int pgid)
{
    if (t->tsk_pid == t->tsk_sid) {
        errno = EPERM;
        return -1;
    }

    // Already a member of that group: success, nothing to do. Job-control
    // shells (bash) call setpgid(child, pgid) from both the parent and the
    // child to close the fork race, so whichever runs second must see a
    // no-op, not EPERM ("child setpgid: Operation not permitted" on every
    // command, seen on iOS where the parent usually wins the race).
    if (t->tsk_pgid == pgid)
        return 0;

    struct task *ct;
    list_for_each_entry(ct, &main_task->tsk_list, tsk_list)
    {
        // TODO lock ct?
        if (ct != t && pgid == ct->tsk_pgid)
        {
            // Joining an existing group: it must belong to our session.
            if (t->tsk_sid != ct->tsk_sid) {
                errno = EPERM;
                return -1;
            }

            t->tsk_pgid = ct->tsk_pgid;
            return 0;
        }
    }

    if (t->tsk_pid != pgid) {
        errno = ESRCH;
        return -1;
    }

    t->tsk_pgid = pgid;
    return 0;
}

/**
 * needs tasks_lock and t locked.
 * Prefers delivering to a thread that isn't in a signal-deferred section.
 */
// SIGSTOP cannot be caught, so pthread_kill()ing it never reaches
// signal_handler: the kernel stops the whole host process — every task, every
// session, the embedding app's UI — and only an outside SIGCONT resumes it.
// Task stop/continue is not implemented (see SAD_STOP in signal_dispatch, which
// ignores the catchable stop signals), so drop it here for the same reason.
static int stop_signal_dropped(int signum, pid_t pid)
{
    if (signum != SIGSTOP)
        return 0;
    static int warned = 0;
    if (!warned) {
        warned = 1;
        INFO("SIGSTOP for pid %d dropped: task stop/continue not implemented", pid);
    }
    return 1;
}

static int task_signal_ignored_locked(struct task *t, int signum);

static int task_kill_locked(struct task *t, int signum) {
    int r;

    if (list_empty(&t->tsk_threads))
        return ESRCH;

    if (stop_signal_dropped(signum, t->tsk_pid))
        return 0;

    // An ignored signal is discarded (POSIX), and must not interrupt
    // anything: pthread_kill made every blocking host call in the target
    // fail with EINTR, including reads inside the host libc's stdio, which
    // report it as EOF or an error. The common case was SIGCHLD, default-
    // ignored, sent to a parent reading a pipe from its child (awk's
    // `"cmd" | getline`, sqlite3's `.import "|cmd"`, fscanf loops); SIGWINCH
    // on a terminal resize likewise. Still delivered when some thread blocks
    // it, where it stays pending (sigwait). waitpid sleepers are woken as
    // before.
    if (task_signal_ignored_locked(t, signum)) {
        pthread_cond_broadcast(&t->tsk_wait_cond);
        return 0;
    }

    // Try to find a thread that isn't blocking signals
    struct thread *fallback = NULL;
    struct thread *thr;
    list_for_each_entry(thr, &t->tsk_threads, thr_list) {
        if (!fallback)
            fallback = thr;
        if (!__atomic_load_n(&thr->thr_sig_blocked, __ATOMIC_ACQUIRE)) {
            r = pthread_kill(thr->thr_pthread, signum);
            goto delivered;
        }
    }

    // All threads blocked — still deliver via pthread_kill so that any
    // blocking syscall (read/write/poll/…) gets interrupted with EINTR.
    // signal_handler will see tvm_sig_blocked and defer the signal; it
    // gets drained on the next tvm_sig_leave().
    if (!fallback)
        panic("fallback thread NULL");
    r = pthread_kill(fallback->thr_pthread, signum);

delivered:
    // pthread_cond_wait is not EINTR-interruptible, so a thread sleeping in
    // waitpid would absorb the pthread_kill (deferred) and keep sleeping —
    // even for SIGKILL. Wake the task's waitpid sleepers so they drain the
    // now-pending signal. Broadcast after pthread_kill: the OS delivers the
    // signal (setting the pending bit via signal_handler) before cond_wait can
    // return to userspace, so a woken waiter always observes the pending bit.
    pthread_cond_broadcast(&t->tsk_wait_cond);
    return r;
}

/**
 * `tasks_lock` held
 */
static struct task *task_next_zombie_child(struct task *t, pid_t pid, int *found) {
    *found = 0;

    struct task *childt;
    list_for_each_entry(childt, &main_task->tsk_list, tsk_list) {
        if (childt->tsk_parent != t)
            continue;

        // already claimed by another thread's waitpid — invisible to this one
        if (childt->tsk_state & TS_REAPED)
            continue;

        if (pid == -1)
            *found = 1;
        else if (pid > 0 && pid == childt->tsk_pid)
            *found = 1;
        else if (pid < -1 && (-pid) == childt->tsk_pgid)
            *found = 1;

        if ((childt->tsk_state & TS_ZOMBIE) == 0)
            continue;

        if (pid > 0 && childt->tsk_pid != pid)
            continue;

        if (pid < -1 && (-pid) != childt->tsk_pgid)
            continue;
        
        break;
    }

    if (childt == main_task)
        childt = NULL;
    
    return childt;
}

/**
 * both tasks_lock and current are locked
 * both are unlocked before returning
 */
/**
 * Detach the calling (non-last) thread from its task and terminate it: remove
 * it from the thread list, drop the task's thread/ref counts, and pthread_exit.
 * The current lock must be held (NOT tasks_lock); it is released here.
 * Shared by pthread_exit and the task-teardown self-reap paths.
 */
__attribute__((noreturn))
static void detach_current_thread_and_exit(void *retval)
{
    list_del(&current_thread->thr_list);
    current->tsk_threads_cnt--;
    current->tsk_refcount--;
    task_unlock(current);

    sigset_t all_sigs;
    sigfillset(&all_sigs);
    CALL_FUNC(pthread_sigmask, SIG_BLOCK, &all_sigs, NULL);

    // current_thread left set → tvm_thread_destructor calls threaddealloc
    current = NULL;
    CALL_FUNC(pthread_exit, retval);
    __builtin_unreachable();
}

/**
 * Remove the calling thread from its (exiting) task like a normal non-last
 * pthread_exit and die, letting the task's terminator finish its drain loop.
 * tasks_lock and current must be locked; both are released. (Unlock order is
 * irrelevant to correctness — only lock-acquisition order matters — and the
 * thread list / counts are protected by the current lock, not tasks_lock.)
 */
__attribute__((noreturn))
static void thread_exit_for_task_exit_locked(void)
{
    pthread_mutex_unlock(&tasks_lock);
    detach_current_thread_and_exit(NULL);
}

// CPU time consumed so far by the calling thread, as rusage user/system time.
static void thread_cputime(struct rusage *ru)
{
    memset(ru, 0, sizeof(*ru));
#ifdef __APPLE__
    thread_basic_info_data_t bi;
    mach_msg_type_number_t cnt = THREAD_BASIC_INFO_COUNT;
    mach_port_t self = mach_thread_self();
    if (thread_info(self, THREAD_BASIC_INFO, (thread_info_t)&bi, &cnt) == KERN_SUCCESS) {
        ru->ru_utime.tv_sec  = bi.user_time.seconds;
        ru->ru_utime.tv_usec = bi.user_time.microseconds;
        ru->ru_stime.tv_sec  = bi.system_time.seconds;
        ru->ru_stime.tv_usec = bi.system_time.microseconds;
    }
    mach_port_deallocate(mach_task_self(), self);
#else
    struct timespec ts;
    if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts) == 0) {
        ru->ru_utime.tv_sec  = ts.tv_sec;
        ru->ru_utime.tv_usec = ts.tv_nsec / 1000;
    }
#endif
}

static void timeval_sub(struct timeval *a, const struct timeval *b)
{
    a->tv_sec -= b->tv_sec;
    a->tv_usec -= b->tv_usec;
    if (a->tv_usec < 0) {
        a->tv_sec--;
        a->tv_usec += 1000000;
    }
    if (a->tv_sec < 0)
        a->tv_sec = a->tv_usec = 0;
}

// Called on the thread a task starts on (fork child, init task): the CPU
// time the thread had already used is not the task's.
static void task_rusage_start(struct task *t)
{
    thread_cputime(&t->tsk_rusage_base);
}

// The CPU time this (exiting) thread consumed since the task started on it,
// as the task's rusage. Tasks are threads, so there is no kernel per-process
// accounting to read; the exiting thread is the task's main thread for every
// single-threaded tool.
static void task_record_rusage(struct task *t)
{
    thread_cputime(&t->tsk_rusage);
    timeval_sub(&t->tsk_rusage.ru_utime, &t->tsk_rusage_base.ru_utime);
    timeval_sub(&t->tsk_rusage.ru_stime, &t->tsk_rusage_base.ru_stime);
}

__attribute__((noreturn))
static void terminate_current_locked(int result)
{
    // Another thread of this task is already tearing it down (e.g. this thread
    // took a fatal signal while a sibling is in exit()/_exit()). Two threads
    // both running the tsk_threads_cnt drain loop below would each wait for the
    // other to decrement and neither ever would — a permanent livelock. So a
    // racing terminator just removes itself like a normal non-last thread
    // (mirrors pthread_exit) and lets the first terminator's loop converge.
    if (current->tsk_state & TS_EXITING)
        thread_exit_for_task_exit_locked();
    current->tsk_state |= TS_EXITING;
    __atomic_store_n(&current->tsk_exiting_thread, current_thread, __ATOMIC_RELEASE);

    // Block OS-level signals for the entire teardown. tasks_lock and the task
    // lock are held from here on; an OS signal landing on this thread would
    // re-enter signal_handler -> signal_dispatch, which takes tasks_lock
    // again and deadlocks the thread against itself (seen as a hang with
    // signal_dispatch stuck in pthread_mutex_lock under rmem_arena_free
    // inside taskfreelastref). This thread never returns, so the mask is
    // never restored; the init-task exit path unblocks only the signal it
    // re-raises.
    sigset_t all_sigs;
    sigfillset(&all_sigs);
    CALL_FUNC(pthread_sigmask, SIG_BLOCK, &all_sigs, NULL);

    // Decide now; `current` may be cleared before the exit below.
    int is_init = (current == main_task);

    // Destroy this task's timers so the vtimer manager spawns no new callbacks.
    // Any already-running SIGEV_THREAD callbacks are regular task threads and
    // will be drained by the tsk_threads_cnt loop below.
    pthread_mutex_lock(&vtimers_lock);
    for (int i = 0; i < MAX_TIMERS_PER_TASK; i++) {
        if (current->tsk_timers[i] >= 0) {
            vtimer_destroy_locked(current->tsk_timers[i]);
            current->tsk_timers[i] = -1;
        }
    }
    pthread_mutex_unlock(&vtimers_lock);

    // Drain sibling threads. Poke each one so blocked threads exit too: the
    // pthread_kill interrupts blocking syscalls and (via TS_EXITING checks in
    // signal_dispatch / tvm_sig_leave / waitpid) makes the sibling reap itself
    // at its next safe point; SIGCONT is used as the carrier since its default
    // action is harmless if it gets dispatched normally. The broadcast wakes
    // siblings sleeping in waitpid's cond wait. Then release both locks so
    // siblings can take task_lock(current) to decrement the count.
    while (current->tsk_threads_cnt != 1) {
        struct thread *sib;
        list_for_each_entry(sib, &current->tsk_threads, thr_list) {
            if (sib != current_thread)
                pthread_kill(sib->thr_pthread, SIGCONT);
        }
        pthread_cond_broadcast(&current->tsk_wait_cond);
        task_unlock(current);
        pthread_mutex_unlock(&tasks_lock);
        usleep(500);
        pthread_mutex_lock(&tasks_lock);
        task_lock(current);
    }
    --current->tsk_refcount;
    --current->tsk_threads_cnt;

    // SIGCHLD special cases
    struct task *parent = current->tsk_parent;
    int notify_parent = 0;
    int auto_reap = 1;

    if (parent) {

        // lock order: tasks_lock -> parent -> child
        // tasks_lock is already held
        task_unlock(current);
        task_lock(parent);
        task_lock(current);

        auto_reap = 0;
        notify_parent = 1;

        struct sigaction *parent_act = &parent->tsk_sighandlers[SIGCHLD];
        void *handler = ((parent_act->sa_flags & SA_SIGINFO) ? ((void *) parent_act->sa_sigaction) : ((void *) parent_act->sa_handler));
        
        if (handler == ((void *) SIG_IGN))
            notify_parent = 0;
    
        if ((parent_act->sa_flags & SA_NOCLDWAIT) || handler == ((void *) SIG_IGN))
            auto_reap = 1;

        // unlock parent before taskfreelastref — closing files (ttyop_close)
        // iterates all tasks and would deadlock trying to re-lock parent
        task_unlock(parent);
    }

    if (auto_reap) {
        taskfreelastref(current, 0, 0);
        list_del(&current->tsk_list);
        pthread_detach(pthread_self());
    }
    else {
        if (!parent)
            panic("auto_reap=0 but parent is NULL");


        taskfreelastref(current, 0, 0);

        current->tsk_state |= TS_ZOMBIE;
        current->tsk_result = result;
        task_record_rusage(current);

        task_unlock(current);
    }

    // re-lock parent for SIGCHLD notification and waitpid wakeup below
    if (parent)
        task_lock(parent);

    // OS-level signals have been blocked since entry (see above), so nothing
    // can arrive on this thread once current is cleared below.

    // zombie: clear current/current_thread so tvm_thread_destructor skips
    //         teardown — parent's waitpid handles it via taskfreelastref.
    // auto_reap: leave them set so tvm_thread_destructor calls
    //            threaddealloc + taskdealloc after stack unwind.
    if (!auto_reap) {
        current_thread = NULL;
        current = NULL;
    }

    if (notify_parent)
        task_kill_locked(parent, SIGCHLD);  // best-effort; parent may have already exited

    // parent may be blocked on waitpid but no more pids, wake him up!
    // broadcast, not signal: multiple parent threads can wait for different
    // specific pids — a single wakeup could go to the wrong waiter and leave
    // the right one asleep with its zombie unreaped.
    if (parent) {
        pthread_cond_broadcast(&parent->tsk_wait_cond);
        task_unlock(parent);
    }

    // if this is before the pthread_cond_broadcast, a not-that-rare deadlock occurs in stress tests on unlocked mutex of parent.
    // not sure why, nor gonna figure this out now
    pthread_mutex_unlock(&tasks_lock);

    // auto_reap: tsk_lock is still held from entry; release it before the TLS
    // destructor's taskdealloc destroys the mutex (destroying a locked mutex is
    // UB). The task is already list_del'd, so nothing can re-lock it.
    if (auto_reap)
        task_unlock(current);

    // The init task is the host process. Once it is torn down nothing is left
    // to run, so leave the process with the matching status — the same way
    // the interposed _exit() does for main_task — instead of pthread_exit'ing
    // the main thread and leaving the process alive on the vtimer thread.
    // This is also how panic() on the init thread ends: its SIGABRT lands
    // here via signal_dispatch.
    if (is_init) {
        if (WIFSIGNALED(result)) {
            int sig = WTERMSIG(result);
            struct sigaction dfl = { .sa_handler = SIG_DFL };
            CALL_FUNC(sigaction, sig, &dfl, NULL);
            sigset_t unblk;
            sigemptyset(&unblk);
            sigaddset(&unblk, sig);
            CALL_FUNC(pthread_sigmask, SIG_UNBLOCK, &unblk, NULL);
            CALL_FUNC(raise, sig);
            CALL_FUNC(_exit, 128 + sig);
        }
        CALL_FUNC(_exit, WEXITSTATUS(result));
    }

    CALL_FUNC(pthread_exit, 0);
    __builtin_unreachable();
}

/**
 * normalize `file_name` path, with an optional dirname for relative values.
 */
char *normalize_path(const char *dirname, const char *file_name, char *out_path)
{
    char tmp[PATH_MAX];
    int c = 0;

    if (!dirname)
        task_lock(current);
    if (file_name[0] == '/')
        c = snprintf(tmp, PATH_MAX, "%s", file_name);
    else if (file_name[0] == 0)
        c = snprintf(tmp, PATH_MAX, "%s", (dirname ?: current->tsk_cwd));
    else
        c = snprintf(tmp, PATH_MAX, "%s/%s", (dirname ?: current->tsk_cwd), file_name);
    if (!dirname)
        task_unlock(current);

    if (c < 0 || c >= PATH_MAX) {
        errno = ENAMETOOLONG;
        return NULL;
    }

    /* Normalize path: handle ., .., and // */
    char *stack[PATH_MAX / 2];
    int top = 0;

    char *p = tmp;
    while (*p) {
        while (*p == '/') p++;
        if (!*p)
            break;

        char *start = p;
        while (*p && *p != '/') p++;
        if (*p)
            *p++ = '\0';

        if (strcmp(start, ".") == 0) {
            continue;
        } else if (strcmp(start, "..") == 0) {
            if (top > 0)
                top--;
        } else {
            stack[top++] = start;
        }
    }

    /* Rebuild normalized path */
    char *out = out_path;
    size_t left = PATH_MAX;

    if (left < 2) {
        errno = ENAMETOOLONG;
        return NULL;
    }

    *out++ = '/';
    left--;

    for (int i = 0; i < top; i++) {
        size_t len = strlen(stack[i]);
        if (len + 1 > left) {
            errno = ENAMETOOLONG;
            return NULL;
        }
        memcpy(out, stack[i], len);
        out += len;
        left -= len;

        if (i != top - 1) {
            *out++ = '/';
            left--;
        }
    }

    if (out == out_path + 1)  /* root */
        *out = '\0';
    else
        *out = '\0';

    return out_path;
}

char *dirfd_normalize_path(int dirnfd, const char *file_name, char *out_path)
{
    struct file *dir = NULL;
    const char *dirname = NULL;
    if (dirnfd != AT_FDCWD) {
        dir = file_for_task(current, dirnfd);
        if (!dir)
            return NULL;

        if (!dir->f_pathname) {
            // A directory descriptor with no recorded name (opened before
            // file_open recorded relative opens, or adopted from the host).
            // This used to hand back the bare relative name, which the
            // path-based callers (fstatat, faccessat, fchmodat, fchownat,
            // readlinkat) then resolved against the task's cwd -- silently
            // the wrong directory. Ask the host for the descriptor's path
            // instead, and fail rather than guess when it has none.
            char dirbuf[PATH_MAX];
#if defined(__APPLE__)
            if (-1 == CALL_FUNC(fcntl, dir->f_rfd, F_GETPATH, dirbuf)) {
                file_unlock(dir, 1);
                errno = EBADF;
                return NULL;
            }
#else
            char proc[64];
            snprintf(proc, sizeof(proc), "/proc/self/fd/%d", dir->f_rfd);
            ssize_t n = CALL_FUNC(readlink, proc, dirbuf, sizeof(dirbuf) - 1);
            if (n <= 0) {
                file_unlock(dir, 1);
                errno = EBADF;
                return NULL;
            }
            dirbuf[n] = 0;
#endif
            file_unlock(dir, 1);
            return normalize_path(dirbuf, file_name, out_path);
        }

        dirname = dir->f_pathname;
        file_unlock(dir, 0);
    }

    char *ret = normalize_path(dirname, file_name, out_path);

    if (dir) {
        file_lock(dir, 0);
        file_unlock(dir, 1);
    }

    return ret;
}

int copy_pthread_attr(const pthread_attr_t *src, pthread_attr_t *dest)
{
    int ret;
    int detachstate, inheritsched, scope;
    size_t stacksize;
    struct sched_param schedparam;
    int schedpolicy;

    // Get attributes from the source
    if ((ret = pthread_attr_getdetachstate(src, &detachstate))) goto out;
    if ((ret = pthread_attr_getinheritsched(src, &inheritsched))) goto out;
    if ((ret = pthread_attr_getscope(src, &scope))) goto out;
    if ((ret = pthread_attr_getstacksize(src, &stacksize))) goto out;
    if ((ret = pthread_attr_getschedparam(src, &schedparam))) goto out;
    if ((ret = pthread_attr_getschedpolicy(src, &schedpolicy))) goto out;

    // Set attributes in the destination
    if ((ret = pthread_attr_setdetachstate(dest, detachstate))) goto out;
    if ((ret = pthread_attr_setinheritsched(dest, inheritsched))) goto out;
    if ((ret = pthread_attr_setscope(dest, scope))) goto out;
    if ((ret = pthread_attr_setstacksize(dest, stacksize))) goto out;
    if ((ret = pthread_attr_setschedparam(dest, &schedparam))) goto out;
    if ((ret = pthread_attr_setschedpolicy(dest, schedpolicy))) goto out;

out:

    return ret;
}

void threaddealloc(struct thread *thr)
{
    if (thr->thr_emulator) {
        emulator_context_destroy(thr->thr_emulator);
        thr->thr_emulator = NULL;
    }

    pthread_attr_destroy(&thr->thr_pthread_attr);

    kfree(thr);
}

struct thread *threadalloc(struct task *parent)
{
    struct thread *ret = NULL;
    struct thread *thr = kalloc(sizeof(struct thread));
    if (!thr)
        return NULL;

    memset(thr, 0, sizeof(*thr));

    INIT_LIST_HEAD(&thr->thr_list);
    thr->thr_refcount = 1;
    thr->thr_task = parent;

    pthread_attr_init(&thr->thr_pthread_attr);

    thr->thr_emulator = emulator_context_create(parent->tsk_linker);
    if (!thr->thr_emulator)
        goto out;

    ret = thr;

out:
    if (!ret) {
        if (thr) {
            threaddealloc(thr);
            thr = NULL;
        }
    }

    return ret;
}

/////////////
// MARK: VTimers
/////////////

struct virtual_timer {
    int vt_active;
    struct task *vt_task;
    int vt_signo;           // signal to deliver to TVM task
    union sigval vt_sigval; // caller's si_value
    int vt_notify;          // SIGEV_SIGNAL, SIGEV_THREAD, SIGEV_NONE
    int vt_task_slot;       // index in task's tsk_timers[]
    int vt_clockid;                // CLOCK_MONOTONIC or CLOCK_REALTIME
    struct timespec vt_deadline;   // absolute next-fire time (CLOCK_MONOTONIC)
    struct itimerspec vt_interval; // original interval (for repeating)
    start_routine_fn vt_thread_func;           // SIGEV_THREAD: pre-wrapped start routine
    void (*vt_notify_function)(union sigval);  // original sigev_notify_function
};

static struct virtual_timer vtimers[MAX_VIRTUAL_TIMERS];
static pthread_cond_t vtimers_cond;

static void vtimer_destroy_locked(int global_idx)
{
    struct virtual_timer *vt = &vtimers[global_idx];
    if (vt->vt_active) {
        vt->vt_active = 0;
        vt->vt_task = NULL;
        memset(&vt->vt_deadline, 0, sizeof(vt->vt_deadline));
        pthread_cond_signal(&vtimers_cond);
    }
}

static inline int ts_is_zero(const struct timespec *ts)
{
    return ts->tv_sec == 0 && ts->tv_nsec == 0;
}

static inline int ts_le(const struct timespec *a, const struct timespec *b)
{
    if (a->tv_sec != b->tv_sec) return a->tv_sec < b->tv_sec;
    return a->tv_nsec <= b->tv_nsec;
}

static inline struct timespec ts_add(const struct timespec *a, const struct timespec *b)
{
    struct timespec r;
    r.tv_sec = a->tv_sec + b->tv_sec;
    r.tv_nsec = a->tv_nsec + b->tv_nsec;
    if (r.tv_nsec >= 1000000000L) {
        r.tv_sec++;
        r.tv_nsec -= 1000000000L;
    }
    return r;
}

static int task_create_thread_locked(struct task *tsk, start_routine_fn start_fn, void *arg,
                              pthread_t *out_thread, const pthread_attr_t *attr,
                              const sigset_t *sigmask);

static void pthread_cond_timedwait_mono_np(pthread_cond_t *cond, pthread_mutex_t *mutex, const struct timespec *mono_deadline)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    struct timespec rel;
    rel.tv_sec = mono_deadline->tv_sec - now.tv_sec;
    rel.tv_nsec = mono_deadline->tv_nsec - now.tv_nsec;
    if (rel.tv_nsec < 0) { rel.tv_sec--; rel.tv_nsec += 1000000000L; }
    if (rel.tv_sec < 0)
        return;
#if defined(__APPLE__)
    pthread_cond_timedwait_relative_np(cond, mutex, &rel);
#else
    struct timespec abstime;
    clock_gettime(CLOCK_REALTIME, &abstime);
    abstime.tv_sec += rel.tv_sec;
    abstime.tv_nsec += rel.tv_nsec;
    if (abstime.tv_nsec >= 1000000000L) { abstime.tv_sec++; abstime.tv_nsec -= 1000000000L; }
    pthread_cond_timedwait(cond, mutex, &abstime);
#endif
}

// SIGEV_THREAD arg: allocated from tsk_ar by vtimer manager, freed inside thread.
struct vtimer_thread_arg {
    void (*func)(union sigval);
    union sigval val;
};

static void *vtimer_thread(void *arg)
{
    (void)arg;

    // block all signals — manager thread must not receive TVM signals
    sigset_t all;
    sigfillset(&all);
    CALL_FUNC(pthread_sigmask, SIG_SETMASK, &all, NULL);

    pthread_mutex_lock(&vtimers_lock);

    for (;;) {
        // find earliest deadline among active timers
        struct timespec earliest = {0, 0};
        int have_deadline = 0;
        for (int i = 0; i < MAX_VIRTUAL_TIMERS; i++) {
            struct virtual_timer *vt = &vtimers[i];
            if (vt->vt_active && !ts_is_zero(&vt->vt_deadline)) {
                if (!have_deadline || ts_le(&vt->vt_deadline, &earliest)) {
                    earliest = vt->vt_deadline;
                    have_deadline = 1;
                }
            }
        }

        if (!have_deadline) {
            pthread_cond_wait(&vtimers_cond, &vtimers_lock);
            continue;
        }

        pthread_cond_timedwait_mono_np(&vtimers_cond, &vtimers_lock, &earliest);

        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);

        // collect fired timers
        for (int i = 0; i < MAX_VIRTUAL_TIMERS; i++) {
            struct virtual_timer *vt = &vtimers[i];
            if (!vt->vt_active || ts_is_zero(&vt->vt_deadline))
                continue;
            if (!ts_le(&vt->vt_deadline, &now))
                continue;

            // timer fired — update deadline
            if (!ts_is_zero(&vt->vt_interval.it_interval)) {
                // repeating: advance deadline past now
                while (ts_le(&vt->vt_deadline, &now))
                    vt->vt_deadline = ts_add(&vt->vt_deadline, &vt->vt_interval.it_interval);
            } else {
                memset(&vt->vt_deadline, 0, sizeof(vt->vt_deadline));
            }

            // save delivery info
            struct task *target = vt->vt_task;
            int signo = vt->vt_signo;
            int notify = vt->vt_notify;
            start_routine_fn thread_func = vt->vt_thread_func;
            void (*notify_function)(union sigval) = vt->vt_notify_function;
            union sigval sigval = vt->vt_sigval;

            // unlock vtimers, acquire tasks_lock (lock ordering: tasks -> vtimers_lock -> task)
            pthread_mutex_unlock(&vtimers_lock);

            if (target && notify == SIGEV_THREAD && thread_func) {
                // SIGEV_THREAD: spawn callback thread
                pthread_mutex_lock(&tasks_lock);
                pthread_mutex_lock(&vtimers_lock);
                int still_valid = vt->vt_active && vt->vt_task == target;
                pthread_mutex_unlock(&vtimers_lock);

                if (still_valid) {
                    // allocate arg from task's arena (freed by wrapper via kfree)
                    struct vtimer_thread_arg *a = rmem_arena_alloc(target->tsk_ar, sizeof(*a));
                    VERIFY(a);

                    task_lock(target);

                    a->func = notify_function;
                    a->val = sigval;

                    pthread_t thr;
                    pthread_attr_t attr;
                    pthread_attr_init(&attr);
                    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);

                    VERIFY(0 == task_create_thread_locked(target, thread_func, a, &thr, &attr, NULL));

                    pthread_attr_destroy(&attr);

                    task_unlock(target);
                }
                pthread_mutex_unlock(&tasks_lock);
            } else if (target && notify != SIGEV_NONE) {
                // SIGEV_SIGNAL: deliver signal
                pthread_mutex_lock(&tasks_lock);
                pthread_mutex_lock(&vtimers_lock);
                int still_valid = vt->vt_active && vt->vt_task == target;
                pthread_mutex_unlock(&vtimers_lock);

                if (still_valid) {
                    task_lock(target);
                    task_kill_locked(target, signo);
                    task_unlock(target);
                }
                pthread_mutex_unlock(&tasks_lock);
            }

            pthread_mutex_lock(&vtimers_lock);
        }
    }

    return NULL;
}

static void vtimer_init(void)
{
    memset(vtimers, 0, sizeof(vtimers));

    pthread_cond_init(&vtimers_cond, NULL);

    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_t thr;
    if (0 != CALL_FUNC(pthread_create, &thr, &attr, vtimer_thread, NULL))
        panic("vtimer_init: pthread_create failed");
    pthread_attr_destroy(&attr);
}

/////////////
// MARK: Allocator
/////////////

static struct rmem_arena *arena_for_task(struct task *task)
{
    return (task ? task->tsk_ar : global_arena);
}

void *kalloc(size_t size)
{
    return rmem_arena_alloc(global_arena, size);
}

void kfree(void *p)
{
    rmem_arena_free(global_arena, p);
}

void *krealloc(void *ptr, size_t size)
{
    return rmem_arena_realloc(global_arena, ptr, size);
}

char *kstrdup(const char *s)
{
    int len = strlen(s);
    char *p = kalloc(len + 1);
    if (!p)
        return NULL;
    memcpy(p, s, len + 1);
    return p;
}

/////////////
// MARK: File Streams
/////////////

/**
 * the fact `FILE *` and `DIR *` manage file-descriptors under the hood makes our lives harder.
 * we need to "invalidate" these once the fd is closed or bad programs might close FDs of other tasks.
 *
 * what we do here is a dirty lil' trick of xref'ing the fd field of the FILE * and DIR * at init, so we can modify the underlying FD.
 * so when an fd closes, we can invalidate streams (setting their fd to -1).
 *
 * this is because POSIX offers no way to release the ownership of a file-descriptor from `FILE *` and `DIR *`.
 */

#if defined(__linux__)
#define FILE_FD_TYPE int
#elif defined (__APPLE__)
#define FILE_FD_TYPE short
#endif

int filep_fd_offset = -1;

void fp_setfileno(FILE *fp, int fd) {
    VERIFY(filep_fd_offset != -1);
    *(FILE_FD_TYPE *)((uintptr_t)fp + filep_fd_offset) = (FILE_FD_TYPE)fd;
}

void init_file_stream_fp()
{
    int devnull = CALL_FUNC(open, "/dev/null", O_RDONLY);
    VERIFY(devnull != -1);

    int fd123;
    VERIFY(123 == (fd123 = CALL_FUNC(fcntl, devnull, F_DUPFD, 123)));

    FILE *fp123;
    VERIFY(NULL != (fp123 = CALL_FUNC(fdopen, fd123, "r")));
    VERIFY(CALL_FUNC(fileno, fp123) == 123);

    for (int i = 0; i < 128; i += sizeof(FILE_FD_TYPE)) {
        if (*(FILE_FD_TYPE *)((uintptr_t)fp123 + i) == 123) {
            filep_fd_offset = i;
            break;
        }
    }

    //INFO("filep_fd_offset %d (size %zu)", filep_fd_offset, sizeof(FILE_FD_TYPE));
    // make sure we found offset of FILE->fd
    VERIFY(filep_fd_offset != -1);
    fp_setfileno(fp123, 21235);
    VERIFY(CALL_FUNC(fileno, fp123) == 21235);
    fp_setfileno(fp123, 123);
    VERIFY(CALL_FUNC(fileno, fp123) == 123);

    VERIFY(0 == CALL_FUNC(fclose, fp123));
    VERIFY(0 == CALL_FUNC(close, devnull));
}

#define DIR_FD_TYPE int

int dirp_fd_offset = -1;

void dp_setfileno(DIR *dp, int fd) {
    VERIFY(dirp_fd_offset != -1);
    *(DIR_FD_TYPE *)((uintptr_t)dp + dirp_fd_offset) = (DIR_FD_TYPE)fd;
}

void init_file_stream_dp()
{
    int dev = CALL_FUNC(open, "/", O_RDONLY | O_DIRECTORY);
    VERIFY(dev != -1);

    int fd123;
    VERIFY(123 == (fd123 = CALL_FUNC(fcntl, dev, F_DUPFD, 123)));

    DIR *dp123;
    VERIFY(NULL != (dp123 = CALL_FUNC(fdopendir, fd123)));
    VERIFY(CALL_FUNC(dirfd, dp123) == 123);

    for (int i = 0; i < 128; i += sizeof(DIR_FD_TYPE)) {
        if (*(FILE_FD_TYPE *)((uintptr_t)dp123 + i) == 123) {
            dirp_fd_offset = i;
            break;
        }
    }

    //INFO("dirp_fd_offset %d (size %zu)", dirp_fd_offset, sizeof(DIR_FD_TYPE));
    // make sure we found offset of DIR->fd
    VERIFY(dirp_fd_offset != -1);
    dp_setfileno(dp123, 21235);
    VERIFY(CALL_FUNC(dirfd, dp123) == 21235);
    dp_setfileno(dp123, 123);
    VERIFY(CALL_FUNC(dirfd, dp123) == 123);

    VERIFY(0 == CALL_FUNC(closedir, dp123));
    VERIFY(0 == CALL_FUNC(close, dev));
}

void init_file_stream()
{
    init_file_stream_fp();
    init_file_stream_dp();
}

enum {
    FS_UNSPEC = 0,
    FS_FILE,
    FS_DIR
};

/**
 * file stream is a "user space" struct.
 * should not include pointers to tvm data
 */
struct file_stream {
    int fs_nfd;
    uint64_t fs_gencnt;

    int fs_type;
    union {
        FILE *fs_fp;
        DIR  *fs_dp;
    };

    char fs_mode[5]; // for files only
    int fs_flush_pending; // fflush(NULL): lock was busy, retry this stream
    int fs_discard;       // deallocate without flushing (execve)
    pid_t fs_pid;    // popen(): the child running the command, reaped by pclose()

    struct dirent fs_dirent; // for dirs only
    char fs_dirent_name[NAME_MAX+1]; // incase dirent->d_name is defined as zero-length array
};

struct file_stream_list_entry {
    struct list_head fsle_list;
    struct file_stream *fsle_stream;
};

static struct file *file_for_task_stream_locked(struct task *t, struct file_stream *fs, int fs_type);

// Returns EOF, with errno set, when the fclose-time flush of a FILE stream
// failed (fclose() reports it, as libc's does); 0 otherwise.
int task_filestreamdealloc_locked(struct task *t, struct file_stream *fs)
{
    int ret = 0, flush_errno = 0;
    struct file_stream_list_entry *fsle;
    list_for_each_entry(fsle, &t->tsk_filestreams, fsle_list) {
        if (fsle->fsle_stream == fs)
            break;
    }

    if (!list_entry_is_head(fsle, &t->tsk_filestreams, fsle_list)) {
        list_del(&fsle->fsle_list);
        kfree(fsle);
    }

    if (fs->fs_type == FS_FILE) {
        if (fs->fs_fp) {
            int last_errno = errno;
            // fclose(): flush while the fd is still attached, or the real
            // fclose() below would flush to fd -1 and drop the buffer; a
            // broken pipe here is SIGPIPE for the closing task, as in
            // fclose(3). The flush goes through file_for_task_stream_locked,
            // which re-syncs the libc FILE's descriptor (it can be stale after
            // a dup2 over it) and skips a stream whose descriptor is gone,
            // instead of writing into a host descriptor number that may now
            // belong to another task.
            //
            // Task teardown (_exit(), death by a signal, or execve dropping
            // the old image's streams) does not flush: POSIX discards
            // unflushed stdio buffers there, exit() has already flushed, and
            // a write that blocked on a full pipe here would do so holding
            // tasks_lock, stopping every task in the VM.
            if (t == current && !(t->tsk_state & TS_EXITING) && !fs->fs_discard && fs->fs_nfd >= 0) {
                struct file *file = file_for_task_stream_locked(t, fs, FS_FILE);
                if (file) {
                    // A failed flush is fclose()'s failure: the buffered
                    // output is lost, and the caller's check of fclose() is
                    // how it learns of EPIPE/ENOSPC/EIO on the final write.
                    if (CALL_FUNC(fflush, fs->fs_fp) == EOF) {
                        ret = EOF;
                        flush_errno = errno;
                        epipe_signal_locked(t);  // t is locked here
                    }
                    file_unlock(file, 1);
                }
            }
            fp_setfileno(fs->fs_fp, -1);
            (void)CALL_FUNC(fclose, fs->fs_fp);
            errno = last_errno;
            fs->fs_fp = NULL;
        }
    }
    else if (fs->fs_type == FS_DIR) {
        if (fs->fs_dp) {
            int last_errno = errno;
            dp_setfileno(fs->fs_dp, -1);
            CALL_FUNC(closedir, fs->fs_dp);
            errno = last_errno;
            fs->fs_dp = NULL;
        }
    }

    fs->fs_type = FS_UNSPEC;
    rmem_arena_free(t->tsk_ar, fs);
    if (ret == EOF)
        errno = flush_errno;
    return ret;
}

struct file_stream *task_filestreamalloc_locked(struct task *t) {
    struct file_stream *fs = rmem_arena_alloc(t->tsk_ar, sizeof(struct file_stream));
    if (!fs)
        return NULL;

    struct file_stream_list_entry *fsle = kalloc(sizeof(struct file_stream_list_entry));
    if (!fsle) {
        rmem_arena_free(t->tsk_ar, fs);
        return NULL;
    }
    
    memset(fs, 0, sizeof(*fs));
    fs->fs_nfd = -1;

    list_add(&fsle->fsle_list, &t->tsk_filestreams);
    fsle->fsle_stream = fs;

    return fs;
}

void task_filestreamdealloc_list(struct task *t, struct list_head *head) {
    while (!list_empty(head)) {
        struct file_stream_list_entry *fsle = list_first_entry(head, struct file_stream_list_entry, fsle_list);
        task_filestreamdealloc_locked(t, fsle->fsle_stream);
    }
}

// A libc FILE attached to no descriptor (fileno -1): what a standard stream
// becomes after fclose() (see there). Operations on it fail with EBADF.
static FILE *fp_detached(const char *mode)
{
    int fd = CALL_FUNC(open, "/dev/null", O_RDWR);
    if (fd == -1)
        return NULL;
    FILE *fp = CALL_FUNC(fdopen, fd, mode);
    if (!fp) {
        CALL_FUNC(close, fd);
        return NULL;
    }
    fp_setfileno(fp, -1);
    CALL_FUNC(close, fd);
    return fp;
}

/*
 * execve replaces the program image, and with it the stdio state: the old
 * image's streams and their unflushed buffers are gone, and the new image's
 * stdin/stdout/stderr are bound to descriptors 0-2 as they are now. TVM kept
 * the old streams, so the new program's fflush(NULL)/exit() flushed the old
 * image's buffers a second time, and a standard stream the old image had
 * fclose()d stayed detached (EBADF) even when descriptor 1 was open again.
 * Called with t locked.
 */
static void task_exec_reset_streams_locked(struct task *t)
{
    while (!list_empty(&t->tsk_filestreams)) {
        struct file_stream_list_entry *fsle =
            list_first_entry(&t->tsk_filestreams, struct file_stream_list_entry, fsle_list);
        fsle->fsle_stream->fs_discard = 1;
        task_filestreamdealloc_locked(t, fsle->fsle_stream);
    }

    for (int i = 0; i < 3; i++) {
        t->tsk_io_filestreams[i] = NULL;
        struct file_stream *fs = task_filestreamalloc_locked(t);
        if (!fs)
            panic("execve: std stream %d", i);
        fs->fs_type = FS_FILE;
        memset(fs->fs_mode, 0, sizeof(fs->fs_mode));
        strncpy(fs->fs_mode, (i ? "w" : "r"), sizeof(fs->fs_mode) - 1);

        struct file *file = file_for_task_locked(t, i);
        if (file) {
            fs->fs_nfd = i;
            fs->fs_gencnt = file->f_gencnt;
            fs->fs_fp = CALL_FUNC(fdopen, file->f_rfd, fs->fs_mode);
            file_unlock(file, 1);
        }
        if (!fs->fs_fp) {
            fs->fs_nfd = -1;
            if (!(fs->fs_fp = fp_detached(fs->fs_mode)))
                panic("execve: detached std stream %d", i);
        }
        else
            (void)CALL_FUNC(setvbuf, fs->fs_fp, NULL, _IOLBF, 0);
        t->tsk_io_filestreams[i] = fs;
    }
}

int task_filestreamrealloc_locked(struct task *t, struct file_stream *fs) {
    if (fs->fs_nfd < 0) {
        // a standard stream the parent fclose()d: the copy still names the
        // parent's libc FILE, give the child a detached one of its own
        if (fs->fs_type == FS_FILE && !(fs->fs_fp = fp_detached(fs->fs_mode[0] ? fs->fs_mode : "r")))
            return -1;
        return 0;
    }

    struct file *file = file_for_task_locked(t, fs->fs_nfd);
    if (!file)
        return -1;

    fs->fs_gencnt = file->f_gencnt;
    if (fs->fs_type == FS_DIR) {
        if (!(fs->fs_dp = CALL_FUNC(fdopendir, file->f_rfd))) {
            file_unlock(file, 1);
            return -1;
        }
    }
    else if (fs->fs_type == FS_FILE) {
        if (!(fs->fs_fp = CALL_FUNC(fdopen, file->f_rfd, fs->fs_mode))) {
            file_unlock(file, 1);
            return -1;
        }

        VERIFY(0 == CALL_FUNC(setvbuf, fs->fs_fp, NULL, _IOLBF, 0));
    }

    file_unlock(file, 1);
    return 0;
}

DIR *real_dp(DIR *dp) {
    return ((struct file_stream *)dp)->fs_dp;
}

struct file_stream *file_stream_fp(FILE *fp) {
    struct file_stream *fs = (struct file_stream *)fp;
    tvm_sig_enter();
    task_lock(current);
    if (fp == stdin)
        fs = current->tsk_io_filestreams[0];
    else if (fp == stdout)
        fs = current->tsk_io_filestreams[1];
    else if (fp == stderr)
        fs = current->tsk_io_filestreams[2];
    task_unlock(current);
    tvm_sig_leave();
    return fs;
}

FILE *real_fp(FILE *fp) {
    return file_stream_fp(fp)->fs_fp;
}

/**
 * returns locked and refcnt
 */
static struct file *file_for_task_stream_locked(struct task *t, struct file_stream *fs, int fs_type)
{
    if ((FILE *)fs == stdin)
        fs = t->tsk_io_filestreams[0];
    else if ((FILE *)fs == stdout)
        fs = t->tsk_io_filestreams[1];
    else if ((FILE *)fs == stderr)
        fs = t->tsk_io_filestreams[2];

    if (!fs)
        return NULL;

    if (fs_type != fs->fs_type)
        return NULL;

    struct file *file = file_for_task_locked(t, fs->fs_nfd);
    if (!file)
        return NULL;

    /**
     * If a `file`'s NFD was dupped recently, it updated the underlying fd -> update info
     */
    if (file->f_gencnt != fs->fs_gencnt) {
        if (fs->fs_type == FS_FILE)
            fp_setfileno(fs->fs_fp, file->f_rfd);
        else if (fs->fs_type == FS_DIR)
            dp_setfileno(fs->fs_dp, file->f_rfd);
        else {
            // unknown type?
            file_unlock(file, 1);
            errno = EINVAL;
            return NULL;
        }
        fs->fs_gencnt = file->f_gencnt;
    }

    return file;
}

static struct file *file_for_task_dp_locked(struct task *t, DIR *dp) {
    return file_for_task_stream_locked(t, (struct file_stream *)dp, FS_DIR);
}

static struct file *file_for_task_fp_locked(struct task *t, FILE *fp) {
    return file_for_task_stream_locked(t, (struct file_stream *)fp, FS_FILE);
}

/**
 * returns locked and refcnt
 */
static struct file *file_for_task_dp(struct task *t, DIR *dp)
{
    task_lock(t);
    struct file *file = file_for_task_dp_locked(t, dp);
    task_unlock(t);
    return file;
}

/**
 * returns locked and refcnt
 */
static struct file *file_for_task_fp(struct task *t, FILE *fp)
{
    task_lock(t);
    struct file *file = file_for_task_fp_locked(t, fp);
    task_unlock(t);
    return file;
}

/////////////
// MARK: Signals
/////////////

/**
 * you have no idea how fun it was creating `default_signal_actions` for each operating system /s
 */

#define SAD_TERM 0
#define SAD_IGN  1
#define SAD_CORE 2
#define SAD_STOP 3
#define SAD_CONT 4
#define SAD_CTCH 5

static const int default_signal_actions[MAX_SIGNALS] = {
#if defined(__linux__)
    [SIGHUP]    = SAD_TERM, // 1
    [SIGINT]    = SAD_TERM, // 2
    [SIGQUIT]   = SAD_CORE, // 3
    [SIGILL]    = SAD_CORE, // 4
    [SIGTRAP]   = SAD_CORE, // 5
    [SIGABRT]   = SAD_CORE, // 6
    [SIGBUS]    = SAD_CORE, // 7
    [SIGFPE]    = SAD_CORE, // 8
    [SIGKILL]   = SAD_TERM, // 9
    [SIGUSR1]   = SAD_TERM, // 10
    [SIGSEGV]   = SAD_CORE, // 11
    [SIGUSR2]   = SAD_TERM, // 12
    [SIGPIPE]   = SAD_TERM, // 13
    [SIGALRM]   = SAD_TERM, // 14
    [SIGTERM]   = SAD_TERM, // 15
    [SIGSTKFLT] = SAD_TERM, // 16
    [SIGCHLD]   = SAD_IGN,  // 17
    [SIGCONT]   = SAD_CONT, // 18
    [SIGSTOP]   = SAD_STOP, // 19
    [SIGTSTP]   = SAD_STOP, // 20
    [SIGTTIN]   = SAD_STOP, // 21
    [SIGTTOU]   = SAD_STOP, // 22
    [SIGURG]    = SAD_IGN,  // 23
    [SIGXCPU]   = SAD_CORE, // 24
    [SIGXFSZ]   = SAD_CORE, // 25
    [SIGVTALRM] = SAD_TERM, // 26
    [SIGPROF]   = SAD_TERM, // 27
    [SIGWINCH]  = SAD_IGN,  // 28
    [SIGIO]     = SAD_TERM, // 29
    [SIGPWR]    = SAD_TERM, // 30
    [SIGSYS]    = SAD_CORE, // 31
#elif defined(__APPLE__)
    [SIGHUP]    = SAD_TERM, // 1
    [SIGINT]    = SAD_TERM, // 2
    [SIGQUIT]   = SAD_CORE, // 3
    [SIGILL]    = SAD_CORE, // 4
    [SIGTRAP]   = SAD_CORE, // 5
    [SIGABRT]   = SAD_CORE, // 6
    [SIGEMT]    = SAD_CORE, // 7
    [SIGFPE]    = SAD_CORE, // 8
    [SIGKILL]   = SAD_TERM, // 9
    [SIGBUS]    = SAD_CORE, // 10
    [SIGSEGV]   = SAD_CORE, // 11
    [SIGSYS]    = SAD_CORE, // 12
    [SIGPIPE]   = SAD_TERM, // 13
    [SIGALRM]   = SAD_TERM, // 14
    [SIGTERM]   = SAD_TERM, // 15
    [SIGURG]    = SAD_IGN,  // 16
    [SIGSTOP]   = SAD_STOP, // 17
    [SIGTSTP]   = SAD_STOP, // 18
    [SIGCONT]   = SAD_IGN,  // 19
    [SIGCHLD]   = SAD_IGN,  // 20
    [SIGTTIN]   = SAD_STOP, // 21
    [SIGTTOU]   = SAD_STOP, // 22
    [SIGIO]     = SAD_IGN,  // 23
    [SIGXCPU]   = SAD_TERM, // 24
    [SIGXFSZ]   = SAD_TERM, // 25
    [SIGVTALRM] = SAD_TERM, // 26
    [SIGPROF]   = SAD_TERM, // 27
    [SIGWINCH]  = SAD_IGN,  // 28
    [SIGINFO]   = SAD_IGN,  // 29
    [SIGUSR1]   = SAD_TERM, // 30
    [SIGUSR2]   = SAD_TERM, // 31
#endif
};

// signum is ignored by t: SIG_IGN, or SIG_DFL whose default action is to
// ignore, and no thread of t blocks it (a blocked signal stays pending).
// Called with t locked.
static int task_signal_ignored_locked(struct task *t, int signum)
{
    if (signum <= 0 || signum >= MAX_SIGNALS)
        return 0;
    struct sigaction *act = &t->tsk_sighandlers[signum];
    if (act->sa_flags & SA_SIGINFO)
        return 0;
    if (!(act->sa_handler == SIG_IGN ||
          (act->sa_handler == SIG_DFL && default_signal_actions[signum] == SAD_IGN)))
        return 0;
    struct thread *thr;
    list_for_each_entry(thr, &t->tsk_threads, thr_list) {
        if (sigismember(&thr->thr_sigmask, signum))
            return 0;
    }
    return 1;
}

#ifdef F_SETNOSIGPIPE
// task_signal_ignored_locked() for the calling task, taking the locks itself.
static int current_signal_ignored(int signum)
{
    tvm_sig_enter();
    task_lock(current);
    int ignored = task_signal_ignored_locked(current, signum);
    task_unlock(current);
    tvm_sig_leave();
    return ignored;
}
#endif

struct safeio_ctx
{
    int p[2];
};

#define SAFEIO_NULL {-1,-1}

static int safeio_open(struct safeio_ctx *out)
{
    if (-1 == CALL_FUNC(pipe, out->p))
        return -1;
    return 0;
}

static ssize_t safeio_copy(struct safeio_ctx *ctx, void *dst, void *src, size_t size)
{
    if (size != CALL_FUNC(write, ctx->p[1], src, size))
        return -1;

    if (size != CALL_FUNC(read, ctx->p[0], dst, size))
        return -1;

    return size;
}

static void safeio_close(struct safeio_ctx *ctx)
{
    if (-1 != ctx->p[0]) {
        CALL_FUNC(close, ctx->p[0]);
        ctx->p[0] = -1;
    }

    if (-1 != ctx->p[1]) {
        CALL_FUNC(close, ctx->p[1]);
        ctx->p[1] = -1;
    }
}

static size_t get_stacktrace(void **out_buf, size_t size)
{
    size_t ret = 0;
    struct safeio_ctx safeio = SAFEIO_NULL;
    void **frame = __builtin_frame_address(0);

    if (0 != safeio_open(&safeio))
        goto out;

    for (int i = 0; i < size; i++) {
        if (sizeof(void *) != safeio_copy(&safeio, &out_buf[i], (frame + 1), sizeof(void *)))
            goto out;
        ret++;

        if (sizeof(void *) != safeio_copy(&safeio, &frame, frame, sizeof(void *)))
            goto out;
    }

out:
    safeio_close(&safeio);
    return ret;
}

static void dump_stacktrace()
{
    void *stack[16];
    size_t nstack = get_stacktrace(stack, sizeof(stack)/sizeof(*stack));

    struct tvm_tls *__tls = TVM_TLS_PEEK();
    struct task *cur = __tls ? __tls->tls_current : NULL;
    struct ulinker *linker = cur ? cur->tsk_linker : NULL;

    for (int i = 0; i < nstack; i++) {
        struct ulinker_dladdr_info uldli;
        if (linker && ulinker_dladdr(linker, stack[i], &uldli, 0)) {
            if (uldli.udli_sname)
                INFO("  stacktrace[% 2d] = %s : %s + %lx", i, uldli.udli_fname, uldli.udli_sname, (long)stack[i] - (long)uldli.udli_saddr);
            else
                INFO("  stacktrace[% 2d] = %s + %lx", i, uldli.udli_fname, (long)stack[i] - (long)(uintptr_t)uldli.udli_fbase);
            continue;
        }

        Dl_info dlinfo;
        memset(&dlinfo, 0, sizeof(dlinfo));
        if (0 == dladdr(stack[i], &dlinfo) || (!dlinfo.dli_fname)) {
            INFO("  stacktrace[% 2d] = %08lx", i, (long)stack[i]);
            continue;
        }

        if (dlinfo.dli_sname) {
            INFO("  stacktrace[% 2d] = %s : %s + %lx", i, dlinfo.dli_fname, dlinfo.dli_sname, (long)stack[i] - (long)dlinfo.dli_saddr);
            continue;
        }

        INFO("  stacktrace[% 2d] = %s + %lx", i, dlinfo.dli_fname, (long)stack[i] - (long)dlinfo.dli_fbase);
    }
}

/**
 * Dispatch a signal on the current thread.  Must NOT be called while
 * tvm_sig_blocked — callers are responsible for checking.
 *
 * This is the shared core used by both the OS signal handler (immediate
 * delivery) and tvm_sig_drain (deferred delivery).
 */
static void signal_dispatch(int signum, siginfo_t *siginfo, ucontext_t *ucontext)
{
    if (!sigshutup) {
        if (signum == SIGSEGV || signum == SIGBUS || signum == SIGILL) {
            unsigned long fault_pc = 0, fault_lr = 0;
#if defined(__APPLE__) && defined(__aarch64__)
            if (ucontext && ucontext->uc_mcontext) {
                fault_pc = (unsigned long)__darwin_arm_thread_state64_get_pc(ucontext->uc_mcontext->__ss);
                fault_lr = (unsigned long)__darwin_arm_thread_state64_get_lr(ucontext->uc_mcontext->__ss);
            }
#endif
            INFO("signum=%d at %p pc=0x%lx lr=0x%lx si_code=%d in_emu=%d sigblk=%d depth=%d", signum, siginfo->si_addr, fault_pc, fault_lr, siginfo->si_code, tvm_in_emu, tvm_sig_blocked, tvm_sig_depth);
            dump_stacktrace();
        }
        else if (signum != SIGCHLD) {
            INFO("signum=%d", signum);
            if (signum == SIGABRT)
                dump_stacktrace();
        }
    }

    // `tasks_lock` required for `terminate_current_locked()`
    pthread_mutex_lock(&tasks_lock);
    task_lock(current);

    struct sigaction *act = current->tsk_sighandlers + signum;

    // need to figure out what to do first
    int whatdo = SAD_CTCH;
    if ((act->sa_flags & SA_SIGINFO) == 0) {
        if (act->sa_handler == SIG_IGN)
            whatdo = SAD_IGN;
        else if (act->sa_handler == SIG_DFL)
            whatdo = default_signal_actions[signum];
    }

    if (whatdo == SAD_TERM || whatdo == SAD_CORE) {
        tvm_sig_depth--;
        terminate_current_locked(mkwstatus(0, signum));
    }
    pthread_mutex_unlock(&tasks_lock);

    if (whatdo == SAD_IGN) {
        task_unlock(current);
        tvm_sig_depth--;
        return;
    }
    if (whatdo == SAD_STOP) {
        // Stopping a task (SIGSTOP/SIGTSTP/SIGTTIN/SIGTTOU with the default
        // disposition) is not implemented: tasks are threads of one host
        // process and there is no stop/continue machinery yet. Ignore the
        // signal like SAD_IGN. This used to panic(), which took the whole VM
        // (and the embedding app) down on any Ctrl-Z or `kill -STOP` that
        // reached a task with the default disposition — routine with bash's
        // job control. Logged once so a silently-ignored stop is still visible.
        static int stop_warned = 0;
        if (!stop_warned) {
            stop_warned = 1;
            INFO("stop signal %d for pid %d ignored: task stop/continue not implemented", signum, current->tsk_pid);
        }
        task_unlock(current);
        tvm_sig_depth--;
        return;
    }
    if (whatdo == SAD_CONT) {
        task_unlock(current);
        tvm_sig_depth--;
        // TODO anything else we need to do?
        return;
    }

    tvm_sig_interrupted = signum;

    if ((act->sa_flags & SA_SIGINFO) == 0) {
        task_unlock(current);
        tvm_sig_depth--;
        call_sighandler((uint64_t)act->sa_handler, signum, 0, 0);
        return;
    }

    task_unlock(current);
    tvm_sig_depth--;
    call_sighandler((uint64_t)act->sa_sigaction, signum, (uint64_t)siginfo, (uint64_t)ucontext);
    return;
}

/**
 * Drain all pending (deferred) signals on the current task/thread.
 * Called when tvm_sig_blocked transitions from 1 → 0.
 *
 * Thread-level pending is drained first (thread-directed signals),
 * then task-level pending (process-directed signals).
 */
static void tvm_sig_drain(void)
{
    if (!current)
        return;

    // Prevent recursive drain (signal_dispatch may re-enter)
    if (tvm_sig_draining)
        return;
    tvm_sig_draining = 1;


    uint32_t pending;

    // Drain thread-directed pending signals
    struct thread *thr = current_thread;
    if (thr) {
        while ((pending = __atomic_exchange_n(&thr->thr_pending_sigs, 0, __ATOMIC_ACQ_REL)) != 0) {
            uint32_t reblock = 0;
            for (int sig = 1; sig < MAX_SIGNALS; sig++) {
                if (!(pending & (1u << sig)))
                    continue;
                if (sigismember(&current_thread->thr_sigmask, sig)) {
                    reblock |= (1u << sig);
                    continue;
                }
                signal_dispatch(sig, &thr->thr_pending_siginfo[sig], &thr->thr_pending_ucontext[sig]);
            }
            if (reblock)
                __atomic_fetch_or(&thr->thr_pending_sigs, reblock, __ATOMIC_RELEASE);
            if (reblock && !(pending & ~reblock))
                break; // only blocked signals remain
        }
    }

    // Drain process-directed pending signals
    struct task *t = current;
    while ((pending = __atomic_exchange_n(&t->tsk_pending_sigs, 0, __ATOMIC_ACQ_REL)) != 0) {
        uint32_t reblock = 0;
        for (int sig = 1; sig < MAX_SIGNALS; sig++) {
            if (!(pending & (1u << sig)))
                continue;
            if (sigismember(&current_thread->thr_sigmask, sig)) {
                reblock |= (1u << sig);
                continue;
            }
            signal_dispatch(sig, &t->tsk_pending_siginfo[sig], &t->tsk_pending_ucontext[sig]);
        }
        if (reblock)
            __atomic_fetch_or(&t->tsk_pending_sigs, reblock, __ATOMIC_RELEASE);
        if (reblock && !(pending & ~reblock))
            break; // only blocked signals remain
    }

    tvm_sig_draining = 0;
}

static inline void tvm_sig_enter(void)
{
    tvm_sig_blocked++;
    if (tvm_sig_blocked == 1 && current_thread)
        __atomic_store_n(&current_thread->thr_sig_blocked, 1, __ATOMIC_RELEASE);
}

static inline void tvm_sig_leave(void)
{
    VERIFY(tvm_sig_blocked > 0);
    if (!--tvm_sig_blocked) {
        if (current_thread)
            __atomic_store_n(&current_thread->thr_sig_blocked, 0, __ATOMIC_RELEASE);
        tvm_sig_drain();
    }
}

/**
 * OS-level signal handler installed via sigaction.
 *
 * If the current thread has signals blocked (inside a critical section),
 * the signal is deferred into the task's pending bitmask.  Otherwise it
 * is dispatched immediately.
 */
static void signal_handler(int signum, siginfo_t *siginfo, void *ucontext)
{
    if (signum < 1 || signum >= MAX_SIGNALS)
        return;

    // VM-enter chokepoint: a thread with no VM identity (no TLS: newborn,
    // dying, or TVM-internal; or TLS but no task yet) has no task to
    // virtualize the signal into.
    struct tvm_tls *tls = TVM_TLS_PEEK();
    if (!tls || !tls->tls_current) {
        int hwfault = siginfo->si_code > 0 &&
                      (signum == SIGSEGV || signum == SIGBUS ||
                       signum == SIGILL || signum == SIGFPE);
        if (hwfault) {
            // Synchronous kernel fault: returning would re-execute the
            // instruction and loop forever, so die deterministically inside
            // the handler, by the kernel's hand (correct termination status +
            // core dump).  Never return-and-reexecute — racy (the address may
            // have become mapped meanwhile), and surviving would leave this
            // handler uninstalled process-wide.
            struct sigaction dfl = { .sa_handler = SIG_DFL };
            CALL_FUNC(sigaction, signum, &dfl, NULL);
            sigset_t unblk;
            sigemptyset(&unblk);
            sigaddset(&unblk, signum);
            CALL_FUNC(pthread_sigmask, SIG_UNBLOCK, &unblk, NULL);
            CALL_FUNC(raise, signum);
            CALL_FUNC(_exit, 128 + signum);
        }
        // Everything else: DROP, even fatal-by-default signals.  These threads
        // exist inside an embedding app (flutter UI/Dart threads never have a
        // VM identity), and darwin delivers e.g. SIGPIPE process-directed (BSD
        // semantics) — a guest's broken-pipe write lands on an arbitrary
        // task-less thread, and applying the host default there kills the
        // WHOLE app (seen as flutter test_sigpipe killing the backend).  Only
        // the fault-loop class above must die; nothing else can loop, since a
        // syscall-generated signal completes the syscall (e.g. EPIPE) after
        // the handler returns.
        return;
    }

    int hardware_fault = siginfo->si_code > 0 && (signum == SIGSEGV || signum == SIGBUS || signum == SIGILL || signum == SIGFPE);

    if (hardware_fault && tvm_in_emu)
        panic("crash in emulator");

    if (tvm_sig_depth && hardware_fault) {
        INFO("fault in signal handler (signum %d)", signum);
        tvm_sig_synchronous_depth++;
    }
    tvm_sig_depth++;

    // crash in signal handler
    if (tvm_sig_synchronous_depth) {
        
        // recursive crash in panic, gotta go with the nuclear option
        if (tvm_sig_synchronous_depth > 3) {
            INFO("RECURSIVE SIGNAL DEPTH (signum %d)", signum);
            CALL_FUNC(abort);
            __builtin_unreachable();
        }
        
        panic("crash in signal handler (signum %d)", signum);
    }

    // tvm_in_emu:        the thread is executing guest code inside uc_emu_start.
    // tvm_emu_call_depth: the thread is anywhere inside emulator_call_ex's
    //                     emulated path — engine setup/teardown, the step loop,
    //                     and the between-steps intr-processing that mutates
    //                     ec->ec_ss / the engine and allocates saved states.
    // Dispatching a signal in either window runs the guest handler reentrantly
    // (via call_sighandler -> emulator_call) on top of an inconsistent emulator
    // context, corrupting it — the outer emulation then faults on clobbered
    // near-NULL state (seen in emulated code, __tvm_pause, and TCG helpers).
    // Defer; emulator_step drains deferred signals at its one safe point, right
    // after uc_emu_start returns with the guest state saved and the engine idle.
    // Hardware faults are handled above.
    if (tvm_sig_blocked || tvm_in_emu || tvm_emu_call_depth || sigismember(&current_thread->thr_sigmask, signum)) {
        // A real hardware fault (kernel-generated: si_code > 0) is synchronous and
        // CANNOT be deferred — returning just re-executes the faulting instruction,
        // which re-faults forever. In any critical section (signals blocked, inside
        // the emulator, or on the emulated call path) it means TVM/host code itself
        // faulted, which is an implementation bug. Dump the stack and panic rather
        // than silently record it as pending (which loops or dies with no trace).
        if (siginfo->si_code > 0 &&
            (signum == SIGSEGV || signum == SIGBUS || signum == SIGILL || signum == SIGFPE)) {
            // Lock-free crash report: dump_stacktrace()/panic() can hang here
            // because the faulting context may hold a lock (dl lock via dladdr,
            // or glibc's stack_cache_lock). Print the faulting instruction
            // pointer + a few caller return addresses read straight from the
            // trapped frame (via safeio, fault-safe) and _exit immediately.
            unsigned long fault_pc = 0, fault_bp = 0, fault_sp = 0;
#if defined(__linux__) && defined(__x86_64__)
            if (ucontext) {
                // glibc x86_64 gregs indices (avoid REG_* macros / greg_t which
                // need _GNU_SOURCE): RBP=10, RSP=15, RIP=16.
                long long *gr = (long long *)((ucontext_t *)ucontext)->uc_mcontext.gregs;
                fault_bp = (unsigned long)gr[10];
                fault_sp = (unsigned long)gr[15];
                fault_pc = (unsigned long)gr[16];
            }
#endif
            (void)fault_sp;
            INFO("HWFAULT signum=%d at %p pc=0x%lx bp=0x%lx sp=0x%lx si_code=%d in_emu=%d sigblk=%d emu_depth=%d",
                 signum, siginfo->si_addr, fault_pc, fault_bp, fault_sp, siginfo->si_code,
                 tvm_in_emu, tvm_sig_blocked, tvm_emu_call_depth);
            CALL_FUNC(_exit, 139);
            __builtin_unreachable();
        }

        // Thread-directed: defer at thread level
        struct thread *thr = current_thread;
        thr->thr_pending_siginfo[signum] = *siginfo;
        if (ucontext)
            thr->thr_pending_ucontext[signum] = *(ucontext_t *)ucontext;
        __atomic_fetch_or(&thr->thr_pending_sigs, 1u << signum, __ATOMIC_RELEASE);
        tvm_sig_depth--;
        return;
    }

    signal_dispatch(signum, siginfo, (ucontext_t *)ucontext);
}

/////////////
// MARK: PTY/TTY
/////////////

/**
 * Yeah so we kinda can't really use OS-level PTY/TTY drivers for a number of reasons:
 * - Controlling TTY is a process-specific attribute and we need multiple processes with controlling TTYs.
 * - Because this concept is legacy and bug shitfest in every OS, they tend to limit who can use TTYs.
 * 
 * So there you go, our own TTY driver in userspace.
 * In reality, we use a `socketpair()` behind the scenes with one side being the master and another the slave.
 * This saves us a ton of code to implement (buffering, poll/select).
 * Each character sent via each side is parsed individually in `tty_input` (master->slave) and `tty_output` (slave->master).
 * 
 * We also need to make sure `/dev/tty` returns the tasks' tty and not the host process tty.
 * 
 * Input/output code is courtesy of the XNU kernel (in Linux its a shitshow).
 */

#define TTY_DIR "/tvm/pts"
#define PTMX_FILE "/dev/ptmx"
#define TTY_FILE "/dev/tty"

#define TTM_MASTER 1
#define TTM_SLAVE  2
#define TTM_ALL    (TTM_MASTER | TTM_SLAVE)

struct tty {
    struct list_head t_list;
    pthread_mutex_t t_lock;
    unsigned t_refcnt;
    unsigned t_master_refcnt;

    pid_t t_sid;
    pid_t t_pgid;

    uid_t t_uid;
    gid_t t_gid;
    mode_t t_mode;

    struct winsize t_winsize;
    struct termios t_termios;
#define t_iflag  t_termios.c_iflag
#define t_oflag  t_termios.c_oflag
#define t_cflag  t_termios.c_cflag
#define t_lflag  t_termios.c_lflag
#define t_cc     t_termios.c_cc
#define t_ispeed t_termios.c_ispeed
#define t_ospeed t_termios.c_ospeed

    int t_mfd;
    ino_t t_mfd_ino;
    int t_sfd;
    ino_t t_sfd_ino;

    int t_col;
};

LIST_HEAD(ttys);
pthread_mutex_t ttys_lock = PTHREAD_MUTEX_INITIALIZER;

static cc_t ttydefchars[NCCS] = {
	[VEOF]=CEOF,
    [VEOL]=CEOL,
    [VEOL2]=CEOL,
    [VERASE]=CERASE,
    [VWERASE]=CWERASE,
    [VKILL]=CKILL,
    [VREPRINT]=CREPRINT,
	[VINTR]=CINTR,
    [VQUIT]=CQUIT,
    [VSUSP]=CSUSP,
#ifdef VDSUSP
    [VDSUSP]=CDSUSP,
#endif
    [VSTART]=CSTART,
    [VSTOP]=CSTOP,
    [VLNEXT]=CLNEXT,
	[VDISCARD]=CDISCARD,
    [VMIN]=CMIN,
    [VTIME]=CTIME,
#ifdef VSTATUS
    [VSTATUS]=CSTATUS,
#endif
};

static struct tty *ttyalloc()
{
    int ends[2] = {-1, -1};
    
    if (-1 == CALL_FUNC(socketpair, AF_UNIX, SOCK_STREAM, 0, ends))
        return NULL;
    
    struct tty *tt = kalloc(sizeof(*tt));
    if (!tt) {
        CALL_FUNC(close, ends[0]);
        CALL_FUNC(close, ends[1]);
        return NULL;
    }

    tt->t_refcnt = 0;
    tt->t_master_refcnt = 0;
    tt->t_mfd = ends[0];
    tt->t_sfd = ends[1];

    tt->t_sid = -1;
    tt->t_pgid = -1;

    tt->t_mode = S_IRUSR | S_IWUSR | S_IWGRP;
    tt->t_uid = getuid();
    struct group *grp = getgrnam("tty");
    if (grp)
        tt->t_gid = grp->gr_gid;
    else
        tt->t_gid = getgid();

    memcpy(tt->t_termios.c_cc, ttydefchars, sizeof(ttydefchars));
    tt->t_termios.c_iflag = TTYDEF_IFLAG;
    tt->t_termios.c_oflag = TTYDEF_OFLAG;
    tt->t_termios.c_lflag = TTYDEF_LFLAG;
    tt->t_termios.c_cflag = TTYDEF_CFLAG;
    tt->t_termios.c_ispeed = tt->t_termios.c_ospeed = TTYDEF_SPEED;
    
    struct stat st;
    if (-1 == CALL_FUNC(fstat, tt->t_mfd, &st)) {
        goto fail;
    }

#if defined(__APPLE__)
    // XNU has a bug in their fstat implementation for UNIX sockets (as of Sep, 2026).
    // Their code allocates an inode when stat is run on the socket for the first time:
    // ``` bsd/kern/uipc_usrreq.c
    //   static int
    //   uipc_sense(struct socket *so, void *ub, int isstat64)
    //   {
    //       // ...
    //       if (unp->unp_ino == 0) {
    //           unp->unp_ino = unp_ino++;      // XXX: should be `++unp_ino`.
    //       }
    // ```
    // First socket gets `unp_ino`, then increments `unp_ino`. On next call to stat on the same socket, **THE INODE CHANGES**.
    // It only happens on the first socket after reboot, but its so odd to do it that it happened to me on every stage I demo'd it on a real iPhone.
    // So for now, we stat twice on Apple products if we get a zero inode and that should fix it.

    if (st.st_ino == 0 && -1 == CALL_FUNC(fstat, tt->t_mfd, &st)) {
        goto fail;
    }
#endif
    
    tt->t_mfd_ino = st.st_ino;

    if (-1 == CALL_FUNC(fstat, tt->t_sfd, &st)) {
        goto fail;
    }
    tt->t_sfd_ino = st.st_ino;

    if (pthread_mutex_init(&tt->t_lock, NULL) != 0) {
        goto fail;
    }

    return tt;

fail:

    CALL_FUNC(close, ends[0]);
    CALL_FUNC(close, ends[1]);
    kfree(tt);

    return NULL;
}

static void ttydealloc(struct tty *tt)
{
    if (tt->t_mfd != -1)
        CALL_FUNC(close, tt->t_mfd);
    if (tt->t_sfd != -1)
        CALL_FUNC(close, tt->t_sfd);

    pthread_mutex_destroy(&tt->t_lock);

    kfree(tt);
}

static void tty_lock(struct tty *tt, int refcount) {
    pthread_mutex_lock(&tt->t_lock);
    tt->t_refcnt += refcount;
}

static void tty_unlock(struct tty *tt, int refcount) {

    // very important that refcount == 0 wouldn't trigger free
    if (refcount && 0 == (tt->t_refcnt -= refcount)) {

        pthread_mutex_unlock(&tt->t_lock);

        // refcount is 0, no one will take references from tty list

        pthread_mutex_lock(&ttys_lock);
        list_del(&tt->t_list);
        pthread_mutex_unlock(&ttys_lock);

        ttydealloc(tt);
        return;
    }
    pthread_mutex_unlock(&tt->t_lock);
}

/**
 * locks and refcnt
 */
static struct tty *tty_for_nfd(int nfd, int *out_ttymode)
{
    struct file *file = file_for_task(current, nfd);
    if (!file)
        return NULL;
    
    struct tty *tt = tty_for_file_locked(file, out_ttymode);
    tty_unlock(tt, 0);
    file_unlock(file, 1);
    tty_lock(tt, 0);
    return tt;
}

/**
 * locks current
 * Returns 1 if local (tvm) tty, 0 if not.
 */
static int islocaltty(int fd)
{
    tvm_sig_enter();
    task_lock(current);
    struct file *file = file_for_task_locked(current, fd);
    if (!file) {
        task_unlock(current);
        tvm_sig_leave();
        errno = EBADF;
        return 0;
    }
    task_unlock(current);

    if (file->f_type != DEV_TTY) {
        file_unlock(file, 1);
        tvm_sig_leave();
        errno = ENOTTY;
        return 0;
    }

    int ttymode;
    struct tty *tt = tty_for_file_locked(file, &ttymode);
    if (!tt) {
        file_unlock(file, 1);
        tvm_sig_leave();
        errno = ENOTTY;
        return 0;
    }

    int ok = (ttymode == TTM_MASTER || tt->t_master_refcnt > 0);
    tty_unlock(tt, 1);
    file_unlock(file, 1);
    tvm_sig_leave();
    if (!ok)
        errno = ENOTTY;
    return ok;
}

/**
 * returns locked and refcnt
 */
static struct tty *tty_for_path(const char *pathname) {   
    // Open the controlling TTY
    if (0 == strcmp(pathname, TTY_FILE)) {

        // since lock order is tty > task
        //   we cannot check tsk_state & TS_CTTY first and then find the tty
        //   because we have to unlock the task between each tty lock and unlock.
        // so we lock on each TTY and re-check TS_CTTY and SID of tty
        // if non found, we return ENXIO and assume TS_CTTY was not set during the search.

        pthread_mutex_lock(&ttys_lock);
        struct tty *tt;
        list_for_each_entry(tt, &ttys, t_list) {
            // lock without ref is safe while ttys_lock is held
            tty_lock(tt, 0);

            // tty is being destroyed, leave it be
            if (!tt->t_refcnt) {
                tty_unlock(tt, 0);
                continue;
            }

            task_lock(current);
            if (tt->t_sid == current->tsk_sid && (current->tsk_state & TS_CTTY)) {
                tt->t_refcnt++;
                pthread_mutex_unlock(&ttys_lock);
                task_unlock(current);
                return tt;
            }
            tty_unlock(tt, 0);
            task_unlock(current);
        }
        pthread_mutex_unlock(&ttys_lock);

        // No TVM tty found — fall back to host /dev/tty
        errno = ENOTSUP;
        return NULL;
    }

    // Open some other TTY

    // not a relevant path, skip
    if (0 != strncmp(pathname, TTY_DIR "/", sizeof(TTY_DIR "/") - 1)) {
        errno = ENOTSUP;
        return NULL;
    }

    long long n;
    if (0 != intparse(pathname + sizeof(TTY_DIR "/") - 1, &n, 10)) {
        errno = ENOENT;
        return NULL;
    }
    
    pthread_mutex_lock(&ttys_lock);
    struct tty *tt;
    list_for_each_entry(tt, &ttys, t_list) {
        tty_lock(tt, 0);

        // tty is being destroyed, leave it be
        if (!tt->t_refcnt) {
            tty_unlock(tt, 0);
            continue;
        }
        
        if (((int) n) == tt->t_mfd) {
            tt->t_refcnt++;
            pthread_mutex_unlock(&ttys_lock);
            return tt;
        }
        tty_unlock(tt, 0);
    }
    pthread_mutex_unlock(&ttys_lock);

    errno = ENOENT;
    return NULL;
}


/**
 * returns locked and refcnt
 */
static struct tty *tty_for_rfd(int rfd, int *out_ttymode) {
    pthread_mutex_lock(&ttys_lock);
    struct tty *tt;
    list_for_each_entry(tt, &ttys, t_list) {
        tty_lock(tt, 0);

        // tty is being destroyed, leave it be
        if (!tt->t_refcnt) {
            tty_unlock(tt, 0);
            continue;
        }
        
        if (rfd == tt->t_mfd) {
            tt->t_refcnt++;
            if (out_ttymode)
                *out_ttymode = TTM_MASTER;
            pthread_mutex_unlock(&ttys_lock);
            return tt;
        }

        if (rfd == tt->t_mfd) {
            tt->t_refcnt++;
            if (out_ttymode)
                *out_ttymode = TTM_SLAVE;
            pthread_mutex_unlock(&ttys_lock);
            return tt;
        }
        tty_unlock(tt, 0);
    }
    pthread_mutex_unlock(&ttys_lock);
    errno = ENOENT;
    return NULL;
}

static struct fops *ttyops();

static int ttyop_open(const char *pathname, int flags, mode_t mode, uid_t uid, gid_t gid, struct file *out)
{
    // tty_for_path locks stuff
    file_unlock(out, 0);
    struct tty *tt = tty_for_path(pathname);
    if (!tt) {
        file_lock(out, 0);
        return -1; // errno set by tty_for_path, ENOTSUP for not relevant
    }
    
    // tt is locked and ref'd

    // if no O_NOCTTY, we gotta lock task
    if (0 == (flags & O_NOCTTY)) {
        task_lock(current);

        // gotta have no terminal and be session leader
        if (current->tsk_state & TS_CTTY || current->tsk_pid != current->tsk_sid) {
            task_unlock(current);
            flags |= O_NOCTTY;
        }
        else
        {
            // no stealing!
            if (tt->t_sid != current->tsk_sid && tt->t_sid != -1) {
                task_unlock(current);
                tty_unlock(tt, 1);
                file_lock(out, 0);
                errno = EIO;
                return -1;
            }

            // tt is still locked in this case!
        }
    }


    // at this point, task is locked if flags does not have O_NOCTTY

    if (tt->t_sfd == -1)
        panic("%s t_sfd invalid?", pathname);

    int r_sfd = CALL_FUNC(dup, tt->t_sfd);
    if (r_sfd == -1) {
        if (0 == (flags & O_NOCTTY))
            task_unlock(current);
        tty_unlock(tt, 1);
        file_lock(out, 0);
        return -1;
    }

    // if we didn't have O_NOCTTY, attach TTY to process
    if (0 == (flags & O_NOCTTY)) {
        current->tsk_state |= TS_CTTY;
        tt->t_sid = current->tsk_sid;
        tt->t_pgid = current->tsk_pgid;
        task_unlock(current);
    }

    // by this point, task is unlocked

    file_lock(out, 0);

    out->f_rfd = rfd_adopt(r_sfd);
    out->f_type = DEV_TTY;
    out->f_data = (void *)tt;
    out->f_ops = ttyops();
    tt->t_refcnt++;

    tty_unlock(tt, 1);

    return 0;
}

static int ttyop_dup(struct file *oldfile, struct file *newfile) {
    // tty_for_file_locked unlocks, so keep lock order between files
    file_unlock(newfile, 0);
    int ttymode;
    struct tty *tt = tty_for_file_locked(oldfile, &ttymode);
    file_unlock(oldfile, 0);
    files_lock(oldfile, newfile);

    if (!tt) {
        return -1;
    }

    tt->t_refcnt++;
    if (ttymode == TTM_MASTER)
        tt->t_master_refcnt++;
    newfile->f_data = (void *)tt;
    tty_unlock(tt, 1);
    return 0;
}

static int tty_stat_locked(struct tty *tt, struct stat *st) {
    if (!st) {
        return 0;
    }

    memset(st, 0, sizeof(*st));

    st->st_ino = tt->t_mfd_ino;
    st->st_uid = tt->t_uid;
    st->st_gid = tt->t_gid;
    st->st_mode = tt->t_mode;

    return 0;
}

static int ttyop_fstat(struct file *file, struct stat *st)
{
    struct tty *tt = tty_for_file_locked(file, NULL);
    if (!tt)
        return -1;

    int ret = tty_stat_locked(tt, st);
    tty_unlock(tt, 1);
    return ret;
}

static int ttyop_stat(const char *pathname, struct stat *st, int flag)
{

    struct tty *tt = tty_for_path(pathname);
    if (!tt)
        return -1;

    int ret = tty_stat_locked(tt, st);
    tty_unlock(tt, 1);
    return ret;
}

static int tty_chmod_locked(struct tty *tt, mode_t mode)
{
    tt->t_mode = mode;
    return 0;
}

static int ttyop_fchmod(struct file *file, mode_t mode)
{
    struct tty *tt = tty_for_file_locked(file, NULL);
    if (!tt)
        return -1;

    int ret = tty_chmod_locked(tt, mode);
    tty_unlock(tt, 1);
    return ret;
}

static int ttyop_chmod(const char *pathname, mode_t mode, int flag)
{
    struct tty *tt = tty_for_path(pathname);
    if (!tt)
        return -1;

    int ret = tty_chmod_locked(tt, mode);
    tty_unlock(tt, 1);
    return ret;
}

static int tty_chown_locked(struct tty *tt, uid_t owner, gid_t group)
{
    tt->t_uid = owner;
    tt->t_gid = group;
    return 0;
}

static int ttyop_fchown(struct file *file, uid_t owner, gid_t group)
{
    struct tty *tt = tty_for_file_locked(file, NULL);
    if (!tt)
        return -1;

    int ret = tty_chown_locked(tt, owner, group);
    tty_unlock(tt, 1);
    return ret;
}

static int ttyop_chown(const char *pathname, uid_t owner, gid_t group, int flag)
{
    struct tty *tt = tty_for_path(pathname);
    if (!tt)
        return -1;

    int ret = tty_chown_locked(tt, owner, group);
    tty_unlock(tt, 1);
    return ret;
}

static int ttyop_ioctl(struct file *file, unsigned long request, void *arg)
{
    int ttymode;
    struct tty *tt = tty_for_file_locked(file, &ttymode);
    if (!tt) {
        errno = ENOTTY;
        return -1;
    }

    pid_t pgid;
    struct task *t;

    int ret = -1;

    switch (request) {
#if defined(__linux__)
        case TCGETS:
#elif defined(__APPLE__)
        case TIOCGETA:
#endif
            memcpy(arg, &tt->t_termios, sizeof(tt->t_termios));
            ret = 0;
            break;
        
#if defined(__linux__)
        case TCSETS:
        case TCSETSW:
        case TCSETSF:
#elif defined(__APPLE__)
        case TIOCSETA:
        case TIOCSETAW:
        case TIOCSETAF:
#endif
            memcpy(&tt->t_termios, arg, sizeof(tt->t_termios));
            ret = 0;
            break;
        

        case TIOCGWINSZ:
            memcpy(arg, &tt->t_winsize, sizeof(tt->t_winsize));
            ret = 0;
            break;
        
        case TIOCSWINSZ:
            memcpy(&tt->t_winsize, arg, sizeof(tt->t_winsize));
            if (tt->t_sid == -1 || tt->t_pgid == -1) {
                ret = 0;
                break;
            }
            
            pgid = tt->t_pgid;

            tty_unlock(tt, 1);
            file_unlock(file, 0);

            kill(-pgid, SIGWINCH);

            file_lock(file, 0);

            ret = 0;
            goto out_unlocked;
        
        case TIOCSCTTY:
            task_lock_with_file_locked(current, file);

            if ((current->tsk_state & TS_CTTY) ||
                (current->tsk_sid != current->tsk_pid)) {
                task_unlock(current);
                errno = EINVAL;
                break;
            }

            // we don't allow stealing ttys
            if (tt->t_sid != -1 || 0 != (long)arg) {
                task_unlock(current);
                errno = EPERM;
                break;
            }

            current->tsk_state |= TS_CTTY;
            tt->t_sid = current->tsk_sid;
            tt->t_pgid = current->tsk_pgid;
            task_unlock(current);
            ret = 0;
            break;

        case TIOCNOTTY:
            task_lock_with_file_locked(current, file);

            if (current->tsk_sid != current->tsk_pid) {
                current->tsk_state &= ~TS_CTTY;
                task_unlock(current);
                ret = 0;
                break;
            }

            // oh my god..
            task_unlock(current);
            tty_unlock(tt, 0);
            file_unlock(file, 0);

            pthread_mutex_lock(&tasks_lock);
            tty_lock(tt, 0);

            list_for_each_entry(t, &main_task->tsk_list, tsk_list) {
                task_lock(t);
                if (t->tsk_sid == tt->t_sid)
                    t->tsk_state &= ~TS_CTTY;
                task_unlock(t);
            }

            pthread_mutex_unlock(&tasks_lock);
            
            pgid = tt->t_pgid;
            tty_unlock(tt, 1);

            kill(-pgid, SIGHUP);
            kill(-pgid, SIGCONT);

            file_lock(file, 0);
            ret = 0;
            goto out_unlocked;

        case TIOCGPGRP:
            task_lock_with_file_locked(current, file);

            if (ttymode == TTM_SLAVE &&
                (0 == (current->tsk_state & TS_CTTY) || tt->t_sid != current->tsk_sid)) {
                task_unlock(current);
                errno = ENOTTY;
                break;
            }

            task_unlock(current);

            *((pid_t *) arg) = tt->t_pgid;
            ret = 0;
            break;
        
        case TIOCSPGRP:
            task_lock_with_file_locked(current, file);

            if (ttymode == TTM_SLAVE &&
                (0 == (current->tsk_state & TS_CTTY) || tt->t_sid != current->tsk_sid)) {
                task_unlock(current);
                errno = ENOTTY;
                break;
            }

            // bruh..
            task_unlock(current);
            tty_unlock(tt, 0);
            file_unlock(file, 0);

            pthread_mutex_lock(&tasks_lock);
            tty_lock(tt, 0);
            file_lock(file, 0);

            list_for_each_entry(t, &main_task->tsk_list, tsk_list) {
                if (t->tsk_pgid == *((pid_t *)arg)) {
                    break;
                }
            }

            pthread_mutex_unlock(&tasks_lock);

            // group non-existed / empty
            if (t == main_task && main_task->tsk_pgid != *((pid_t *) arg)) {
                errno = -ESRCH;
                break;
            }

            tt->t_pgid = *((pid_t *) arg);
            ret = 0;
            break;

#if defined(__linux__)
        case TIOCGSID:
            task_lock_with_file_locked(current, file);

            if (ttymode == TTM_SLAVE &&
                (0 == (current->tsk_state & TS_CTTY) || tt->t_sid != current->tsk_sid)) {
                task_unlock(current);
                errno = ENOTTY;
                break;
            }
            task_unlock(current);

            *((pid_t *) arg) = tt->t_sid;
            ret = 0;
            break;
#endif

        default:
            errno = ENOTSUP;
    }

    tty_unlock(tt, 1);
out_unlocked:
    return ret;
}

static ssize_t ttyop_read(struct file *file, void *buf, size_t count, int offset)
{
    if (offset != -1) {
        errno = EINVAL;
        return -1;
    }

    struct tty *tt = tty_for_file_locked(file, NULL);
    if (!tt)
        return -1;

    struct termios termios;
    memcpy(&termios, &tt->t_termios, sizeof(termios));

    tty_unlock(tt, 0);
    file_unlock(file, 0);

    size_t total_read = 0;
    uint8_t *out = buf;
    struct pollfd pfd = { .fd = file->f_rfd, .events = POLLIN };

    int vmin = termios.c_cc[VMIN];
    int vtime = termios.c_cc[VTIME]; // in tenths of a second

    int timer_started = 0;
    struct timespec start_time;
    long remaining_ms = vtime * 100; // VTIME in 0.1s units → ms

    while (1) {
        // Check if we have enough bytes for VMIN
        if (vmin > 0 && total_read >= (size_t)vmin)
            break;

        // If VMIN == 0 and some bytes already read, return immediately
        if (vmin == 0 && total_read > 0)
            break;

        int poll_timeout;
        if (vmin == 0 && vtime == 0) {
            poll_timeout = 0;  // poll returns immediately
        } else if (vmin == 0 && vtime > 0) {
            poll_timeout = remaining_ms; // timer starts immediately
        } else if (vmin > 0 && vtime > 0 && timer_started) {
            poll_timeout = remaining_ms; // inter-byte timeout
        } else {
            poll_timeout = -1; // infinite wait for VMIN>0, VTIME=0
        }
        //INFO("ttyop_read VMIN %d VTIME %d poll_timeout %d", vmin, vtime, poll_timeout);

        int ret = CALL_FUNC(poll, &pfd, 1, poll_timeout);
        if (ret == 0) {
            // Timeout
            break;
        } else if (ret < 0) {
            tty_lock(tt, 0);
            tty_unlock(tt, 1);
            file_lock(file, 0);
            return -1;
        }

        // Data is available: read one byte at a time
        unsigned char c;
        ssize_t n = CALL_FUNC(read, file->f_rfd, &c, 1);
        if (n < 0) {
            tty_lock(tt, 0);
            tty_unlock(tt, 1);
            file_lock(file, 0);
            return -1;
        }
        if (n == 0)
            break; // EOF

        // Store the byte
        if (total_read < count)
            out[total_read++] = c;

        // Start timer after first byte if VMIN>0 and VTIME>0
        if (vmin > 0 && vtime > 0 && !timer_started) {
            clock_gettime(CLOCK_MONOTONIC, &start_time);
            timer_started = 1;
        }

        // Update remaining time if timer is running
        if (timer_started && vtime > 0) {
            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            long elapsed_ms = (now.tv_sec - start_time.tv_sec) * 1000 +
                              (now.tv_nsec - start_time.tv_nsec) / 1000000;
            remaining_ms = vtime * 100 - elapsed_ms;
            if (remaining_ms < 0)
                remaining_ms = 0;
        }
    }

    tty_lock(tt, 0);
    tty_unlock(tt, 1);
    file_lock(file, 0);
    return total_read;
}

#define TTY_CHARMASK    0x000000ff      /* Character mask */

#define E       0x00    /* Even parity. */
#define O       0x80    /* Odd parity. */

#define ALPHA   0x40    /* Alpha or underscore. */

#define CCLASSMASK      0x3f
#define CCLASS(c)       (char_type[c] & CCLASSMASK)

#define SET(T, F)       (T) |= (F)
#define CLR(T, F)       (T) &= ~(F)
#define ISSET(T, F)     ((T) & (F))

#define ORDINARY        0
#define CONTROL         1
#define BACKSPACE       2
#define NEWLINE         3
#define TAB             4
#define VTAB            5
#define RETURN          6

#define BS      BACKSPACE
#define CC      CONTROL
#define CR      RETURN
#define NA      ORDINARY | ALPHA
#define NL      NEWLINE
#define NO      ORDINARY
#define TB      TAB
#define VT      VTAB

static u_char const char_type[] = {
	E | CC, O | CC, O | CC, E | CC, O | CC, E | CC, E | CC, O | CC, /* nul - bel */
	O | BS, E | TB, E | NL, O | CC, E | VT, O | CR, O | CC, E | CC, /* bs - si */
	O | CC, E | CC, E | CC, O | CC, E | CC, O | CC, O | CC, E | CC, /* dle - etb */
	E | CC, O | CC, O | CC, E | CC, O | CC, E | CC, E | CC, O | CC, /* can - us */
	O | NO, E | NO, E | NO, O | NO, E | NO, O | NO, O | NO, E | NO, /* sp - ' */
	E | NO, O | NO, O | NO, E | NO, O | NO, E | NO, E | NO, O | NO, /* ( - / */
	E | NA, O | NA, O | NA, E | NA, O | NA, E | NA, E | NA, O | NA, /* 0 - 7 */
	O | NA, E | NA, E | NO, O | NO, E | NO, O | NO, O | NO, E | NO, /* 8 - ? */
	O | NO, E | NA, E | NA, O | NA, E | NA, O | NA, O | NA, E | NA, /* @ - G */
	E | NA, O | NA, O | NA, E | NA, O | NA, E | NA, E | NA, O | NA, /* H - O */
	E | NA, O | NA, O | NA, E | NA, O | NA, E | NA, E | NA, O | NA, /* P - W */
	O | NA, E | NA, E | NA, O | NO, E | NO, O | NO, O | NO, O | NA, /* X - _ */
	E | NO, O | NA, O | NA, E | NA, O | NA, E | NA, E | NA, O | NA, /* ` - g */
	O | NA, E | NA, E | NA, O | NA, E | NA, O | NA, O | NA, E | NA, /* h - o */
	O | NA, E | NA, E | NA, O | NA, E | NA, O | NA, O | NA, E | NA, /* p - w */
	E | NA, O | NA, O | NA, E | NO, O | NO, E | NO, E | NO, O | CC, /* x - del */
	/*
	 * Meta chars; should be settable per character set;
	 * for now, treat them all as normal characters.
	 */
	NA, NA, NA, NA, NA, NA, NA, NA,
	NA, NA, NA, NA, NA, NA, NA, NA,
	NA, NA, NA, NA, NA, NA, NA, NA,
	NA, NA, NA, NA, NA, NA, NA, NA,
	NA, NA, NA, NA, NA, NA, NA, NA,
	NA, NA, NA, NA, NA, NA, NA, NA,
	NA, NA, NA, NA, NA, NA, NA, NA,
	NA, NA, NA, NA, NA, NA, NA, NA,
	NA, NA, NA, NA, NA, NA, NA, NA,
	NA, NA, NA, NA, NA, NA, NA, NA,
	NA, NA, NA, NA, NA, NA, NA, NA,
	NA, NA, NA, NA, NA, NA, NA, NA,
	NA, NA, NA, NA, NA, NA, NA, NA,
	NA, NA, NA, NA, NA, NA, NA, NA,
	NA, NA, NA, NA, NA, NA, NA, NA,
	NA, NA, NA, NA, NA, NA, NA, NA,
};
#undef  BS
#undef  CC
#undef  CR
#undef  NA
#undef  NL
#undef  NO
#undef  TB
#undef  VT

static int tty_putc_locked(struct tty *tt, int fd, int c)
{
    // assumes fd is O_NONBLOCK


    ssize_t ret = CALL_FUNC(write, fd, &c, 1);
    if (ret == 1)
        return 0;
    if (ret == 0)
        panic("what?");
    if (errno != EWOULDBLOCK && errno != EAGAIN)
        return -1;
    
    // would block, try with blocking
    if (-1 == CALL_FUNC(fcntl, fd, F_SETFL, 0))
        return -1;
    
    tty_unlock(tt, 0);
    ret = CALL_FUNC(write, fd, &c, 1);
    tty_lock(tt, 0);

    if (-1 == CALL_FUNC(fcntl, fd, F_SETFL, O_NONBLOCK))
        return -1;

    if (ret == 0)
        panic("what?");
    if (ret == 1)
        return 0;
    return -1;
}

static int tty_output_locked(struct tty *tt, int fd, int c)
{
    int col;

    if (!ISSET(tt->t_oflag, OPOST)) {
        if (ISSET(tt->t_lflag, FLUSHO))
            return 0;
        return tty_putc_locked(tt, fd, c);
    }

    // expand tabs or smth

    // if ONLCR is set, translate newline into "\r\n"
    if (c == '\n' && ISSET(tt->t_oflag, ONLCR)) {
        if (tty_putc_locked(tt, fd, '\r'))
            return -1;
    }
    // if OCRNL is set, translate "\r" into "\n"
    else if (c == '\r' && ISSET(tt->t_oflag, OCRNL))
        c = '\n';
    // if ONOCR is set, don't transmit CRs when on column 0
    else if (c == '\r' && ISSET(tt->t_oflag, ONOCR) && tt->t_col == 0)
        return 0;

    if (!ISSET(tt->t_lflag, FLUSHO) && tty_putc_locked(tt, fd, c))
        return -1;
    
    col = tt->t_col;
    switch (CCLASS(c)) {
    case BACKSPACE:
        if (col > 0) {
            --col;
        }
        break;
    case CONTROL:
        break;
    case NEWLINE:
    case RETURN:
        col = 0;
        break;
    case ORDINARY:
        ++col;
        break;
    case TAB:
        col = (col + 8) & ~7;
        break;
    }
    tt->t_col = col;
    return 0;
}

static ssize_t tty_output_buffer_locked(struct tty *tt, struct file *file, const uint8_t *buf, size_t count)
{
    int fflags;
    ssize_t ret = -1;

    if (-1 == (fflags = CALL_FUNC(fcntl, file->f_rfd, F_GETFL)))
        return -1;

    if (-1 == CALL_FUNC(fcntl, file->f_rfd, F_SETFL, O_NONBLOCK))
        return -1;

    file_unlock(file, 0);
    size_t i;
    for (i = 0; i < count; i++)
        if (0 != tty_output_locked(tt, file->f_rfd, (int)buf[i]))
            break;
    file_lock(file, 0);
    
    ret = i;

    CALL_FUNC(fcntl, file->f_rfd, F_SETFL, fflags);

    return ret;
}

static void tty_echo_locked(struct tty *tt, int c)
{
	if ((!ISSET(tt->t_lflag, ECHO) &&
	    (c != '\n' || !ISSET(tt->t_lflag, ECHONL))) ||
	    ISSET(tt->t_lflag, EXTPROC)) {
		return;
	}

    int slave = CALL_FUNC(dup, tt->t_sfd);
    if (-1 == slave)
        return;
    
    if (-1 == CALL_FUNC(fcntl, slave, F_SETFL, O_NONBLOCK)) {
        CALL_FUNC(close, slave);
        return;
    }

	if (ISSET(tt->t_lflag, ECHOCTL) &&
	    ((ISSET(c, TTY_CHARMASK) <= 037 && c != '\t' && c != '\n') ||
	    ISSET(c, TTY_CHARMASK) == 0177)) {
		(void)tty_output_locked(tt, slave, '^');
		CLR(c, ~TTY_CHARMASK);
		if (c == 0177) {
			c = '?';
		} else {
			c += 'A' - 1;
		}
	}

	tty_output_locked(tt, slave, c);
    CALL_FUNC(close, slave);
}

static int tty_input_locked(struct tty *tt, int fd, int c)
{
    if (ISSET(tt->t_iflag, ISTRIP))
		CLR(c, 0x80);
    
    if (!ISSET(tt->t_lflag, EXTPROC)) {
        // Signals
        if (ISSET(tt->t_lflag, ISIG)) {
            if (CCEQ(tt->t_cc[VINTR], c) || CCEQ(tt->t_cc[VQUIT], c) || CCEQ(tt->t_cc[VSUSP], c)) {
                tty_echo_locked(tt, c);
                int pgid = tt->t_pgid;
                int sig = CCEQ(tt->t_cc[VINTR], c) ? SIGINT : (CCEQ(tt->t_cc[VQUIT], c) ? SIGQUIT : SIGTSTP);
                tty_unlock(tt, 0);
                kill(-pgid, sig); // TODO check TS_CTTY
                tty_lock(tt, 0);
                return 0;
            }
        }

        // IGNCR, ICRNL, & INLCR
		if (c == '\r') {
			if (ISSET(tt->t_iflag, IGNCR))
				return 0;
			if (ISSET(tt->t_iflag, ICRNL))
				c = '\n';
		} else if (c == '\n' && ISSET(tt->t_iflag, INLCR))
			c = '\r';
        
    }

    return tty_putc_locked(tt, fd, c);
}

static ssize_t tty_input_buffer_locked(struct tty *tt, struct file *file, const uint8_t *buf, size_t count)
{
    int fflags;
    ssize_t ret = -1;

    if (-1 == (fflags = CALL_FUNC(fcntl, file->f_rfd, F_GETFL)))
        return -1;

    if (-1 == CALL_FUNC(fcntl, file->f_rfd, F_SETFL, O_NONBLOCK))
        return -1;

    file_unlock(file, 0);
    size_t i;
    for (i = 0; i < count; i++)
        if (0 != tty_input_locked(tt, file->f_rfd, (int)buf[i]))
            break;
    file_lock(file, 0);
    
    ret = i;

    CALL_FUNC(fcntl, file->f_rfd, F_SETFL, fflags);

    return ret;
}

static ssize_t ttyop_write(struct file *file, const void *buf, size_t count, int offset)
{
    if (offset != -1) {
        errno = EINVAL;
        return -1;
    }

    int ttymode;
    struct tty *tt = tty_for_file_locked(file, &ttymode);
    if (!tt)
        return -1;

    ssize_t ret;
    if (ttymode == TTM_MASTER)
        ret = tty_input_buffer_locked(tt, file, buf, count);
    else
        ret = tty_output_buffer_locked(tt, file, buf, count);

    tty_unlock(tt, 1);

    return ret;
}

// Send sig to a target the way kill() does (pid>0: that pid; pid<0: process
// group -pid), but assuming tasks_lock is already held by the caller. Used from
// the tty-close path during task teardown, where re-entering kill() (which
// locks tasks_lock) would self-deadlock.
static void kill_target_tasks_locked(pid_t pid, int sig) {
    struct task *t;
    if ((main_task->tsk_pid == pid || main_task->tsk_pgid == -pid) && main_task != current) {
        task_lock(main_task);
        if (!(main_task->tsk_state & TS_ZOMBIE))
            task_kill_locked(main_task, sig);
        task_unlock(main_task);
    }
    list_for_each_entry(t, &main_task->tsk_list, tsk_list) {
        if ((t->tsk_pid == pid || t->tsk_pgid == -pid) && t != current) {
            task_lock(t);
            if (!(t->tsk_state & TS_ZOMBIE))
                task_kill_locked(t, sig);
            task_unlock(t);
        }
    }
}

static void ttyop_close(struct file *file) {
    int ttymode;
    struct tty *tt = tty_for_file_locked(file, &ttymode);
    if (!tt)
        return;

    // we took ref in tty_for_file_locked()
    tt->t_refcnt--;
    int master_closed = (ttymode == TTM_MASTER && --tt->t_master_refcnt == 0);
    int last_file = (tt->t_refcnt == 1); // only the temp ref from tty_for_file_locked remains

    if (!last_file && !master_closed) {
        tty_unlock(tt, 1);
        return;
    }

    int tty_sid = tt->t_sid;
    pid_t tty_pgid = tt->t_pgid;

    if (!last_file) {
        // master just closed but slave still open: signal session, don't destroy tty
        tty_unlock(tt, 1);
        file_unlock(file, 0);

        if (tty_sid != -1) {
            int held = tasks_lock_held;
            if (!held)
                pthread_mutex_lock(&tasks_lock);
            struct task *t;
            list_for_each_entry(t, &main_task->tsk_list, tsk_list) {
                // In teardown current's task_lock is already held by us — don't
                // re-lock it (recursive task_lock would self-deadlock).
                int self_held = (held && t == current);
                if (!self_held)
                    task_lock(t);
                if (t->tsk_sid == tty_sid)
                    t->tsk_state &= ~TS_CTTY;
                if (!self_held)
                    task_unlock(t);
            }
            if (held) {
                // teardown path: tasks_lock already held, signal without re-locking
                kill_target_tasks_locked(-tty_pgid, SIGHUP);
                kill_target_tasks_locked(-tty_pgid, SIGCONT);
            } else {
                pthread_mutex_unlock(&tasks_lock);
                kill(-tty_pgid, SIGHUP);
                kill(-tty_pgid, SIGCONT);
            }
        }

        file_lock(file, 0);
        return;
    }

    VERIFY(tt->t_refcnt == 1);

    // leave refcnt at 0, no one will take ref in global list after unlocking
    tt->t_refcnt--;
    tty_unlock(tt, 0);
    file_unlock(file, 0);

    // Disassociate the entire session

    int held = tasks_lock_held;
    if (master_closed && tty_sid != -1) {
        if (!held)
            pthread_mutex_lock(&tasks_lock);
        struct task *t;
        list_for_each_entry(t, &main_task->tsk_list, tsk_list) {
            // In teardown current's task_lock is already held by us — don't
            // re-lock it (recursive task_lock would self-deadlock).
            int self_held = (held && t == current);
            if (!self_held)
                task_lock(t);
            if (t->tsk_sid == tty_sid)
                t->tsk_state &= ~TS_CTTY;
            if (!self_held)
                task_unlock(t);
        }
        if (!held)
            pthread_mutex_unlock(&tasks_lock);
    }

    pthread_mutex_lock(&ttys_lock);
    list_del(&tt->t_list);
    pthread_mutex_unlock(&ttys_lock);

    ttydealloc(tt);

    // Signal after all locks released: session leader may be current task,
    // and signal_handler acquires tasks_lock.
    if (master_closed && tty_sid != -1) {
        if (held) {
            // teardown path: tasks_lock already held, signal without re-locking
            kill_target_tasks_locked(tty_sid, SIGHUP);
            kill_target_tasks_locked(tty_sid, SIGCONT);
        } else {
            kill(tty_sid, SIGHUP);
            kill(tty_sid, SIGCONT);
        }
    }

    file_lock(file, 0);
}

static struct device devtty = {
    .dev_type = DEV_TTY,
    .dev_fops = {
        .f_open = ttyop_open,
        .f_dup = ttyop_dup,

        .f_fstat = ttyop_fstat,
        .f_fchown = ttyop_fchown,
        .f_fchmod = ttyop_fchmod,

        .f_stat = ttyop_stat,
        .f_chmod = ttyop_chmod,
        .f_chown = ttyop_chown,

        .f_ioctl = ttyop_ioctl,
        .f_read = ttyop_read,
        .f_write = ttyop_write,
        .f_close = ttyop_close,
    }
};
REGISTER_DEV(devtty);

static struct fops *ttyops() {
    return &devtty.dev_fops;
}

static struct tty *tty_for_file_locked(struct file *file, int *out_ttymode) {
    if (file->f_type != DEV_TTY) {
        errno = ENOTTY;
        return NULL;
    }
    
    // tty > file
    // we have ref on file so tty is safe to access refcnt without lock for this simple VERIFY
    struct tty *tt = (struct tty *)file->f_data;
    VERIFY(tt->t_refcnt);

    file_unlock(file, 0);
    tty_lock(tt, 1);
    file_lock(file, 0);

    // no need to detect mode, bye
    if (!out_ttymode) {
        return tt;
    }

    struct stat st;
    if (-1 == CALL_FUNC(fstat, file->f_rfd, &st)) {
        tty_unlock(tt, 1);
        return NULL;
    }

    if (tt->t_mfd_ino == st.st_ino) {
        *out_ttymode = TTM_MASTER;
        return tt;
    }

    // or else it doesn't make sense
    VERIFY(tt->t_sfd_ino == st.st_ino);
    *out_ttymode = TTM_SLAVE;
    return tt;
}

/**
 * returns locked and refcnt
 */
static struct file *ptm_open()
{
    struct tty *tt = ttyalloc();
    if (!tt)
        return NULL;

    int mfd = CALL_FUNC(dup, tt->t_mfd);
    if (-1 == mfd) {
        ttydealloc(tt);
        return NULL;
    }

    struct file *file = filealloc_from_rfd(mfd);
    if (!file) {
        ttydealloc(tt);
        VERIFY(0 == CALL_FUNC(close, mfd));
        return NULL;
    }

    file_lock(file, 1);
    tt->t_refcnt++;
    tt->t_master_refcnt++;
    file->f_type = DEV_TTY;
    file->f_data = tt;
    file->f_ops = ttyops();
    pthread_mutex_lock(&ttys_lock);
    list_add_tail(&tt->t_list, &ttys);
    pthread_mutex_unlock(&ttys_lock);

    return file;
}

static struct file *pts_open_locked(struct tty *tt) {
    int sfd = CALL_FUNC(dup, tt->t_sfd);
    if (-1 == sfd)
        return NULL;

    struct file *file = filealloc_from_rfd(sfd);
    if (!file) {
        VERIFY(0 == CALL_FUNC(close, sfd));
        return NULL;
    }

    file_lock(file, 1);
    tt->t_refcnt++;
    file->f_type = DEV_TTY;
    file->f_data = tt;
    file->f_ops = ttyops();
    return file;
}

static int tty_slavename_locked(struct tty *tt, char *buf, size_t buflen)
{
    if (tt->t_mfd == -1) {
        errno = EINVAL;
        return -1;
    }

    snprintf(buf, buflen, TTY_DIR "/%d", tt->t_mfd);
    return 0;
}

/////////////
// MARK: VFS
/////////////

#define VFS_NAME_MAX 256

struct vfs_node {
    struct list_head vn_list;
    char vn_name[VFS_NAME_MAX];

    struct stat vn_st;          /* S_ISDIR / S_ISREG determine type */

    char *vn_data;
    size_t vn_size;

    struct vfs_node *vn_parent;
    struct list_head vn_children;

    size_t vn_refcnt;
    pthread_mutex_t vn_lock;
};

static LIST_HEAD(vfs_list);
static pthread_mutex_t vfs_lock = PTHREAD_MUTEX_INITIALIZER;

#define VFS_LOCK()   pthread_mutex_lock(&vfs_lock);
#define VFS_UNLOCK() pthread_mutex_unlock(&vfs_lock);

static ino_t vfs_next_inode_value = 4141000000;
static pthread_mutex_t vfs_next_inode_lock = PTHREAD_MUTEX_INITIALIZER;

static ino_t vfs_next_inode() {
    pthread_mutex_lock(&vfs_next_inode_lock);
    ino_t ret = vfs_next_inode_value++;
    pthread_mutex_unlock(&vfs_next_inode_lock);
    return ret;
}

static struct vfs_node * vnalloc(const char *name, mode_t mode, uid_t uid, gid_t gid)
{
    int namelen = strlen(name);
    if (namelen > VFS_NAME_MAX-1) {
        errno = ENAMETOOLONG;
        return NULL;
    }

    struct vfs_node *vn = kalloc(sizeof(*vn));
    if (!vn) {
        errno = ENOMEM;
        return NULL;
    }

    memcpy(vn->vn_name, name, namelen);
    vn->vn_name[namelen] = 0;

    INIT_LIST_HEAD(&vn->vn_children);
    INIT_LIST_HEAD(&vn->vn_list);

    vn->vn_st.st_mode = mode;
    vn->vn_st.st_uid  = uid;
    vn->vn_st.st_gid  = gid;
    vn->vn_st.st_nlink = 1;
    vn->vn_st.st_ino = vfs_next_inode();

    vn->vn_refcnt = 1;
    pthread_mutex_init(&vn->vn_lock, NULL);
    pthread_mutex_lock(&vn->vn_lock);

    return vn;
}

static void vndealloc_locked(struct vfs_node *vn)
{
    if (vn->vn_refcnt != 0) {
        panic("refcount != 0 (%zu)", vn->vn_refcnt);
    }

    /* Free file data if present */
    if (vn->vn_data) {
        kfree(vn->vn_data);
        vn->vn_data = NULL;
    }
    vn->vn_size = 0;

    pthread_mutex_unlock(&vn->vn_lock);
    pthread_mutex_destroy(&vn->vn_lock);
    kfree(vn);
}

void vn_lock(struct vfs_node *vn, int refcount) {
    pthread_mutex_lock(&vn->vn_lock);
    vn->vn_refcnt += refcount;
}

void vn_unlock(struct vfs_node *vn, int refcount) {
    // very important that refcount == 0 wouldn't trigger free
    if (refcount && 0 == (vn->vn_refcnt -= refcount)) {
        vndealloc_locked(vn);
        return;
    }
    pthread_mutex_unlock(&vn->vn_lock);
}

static void
vfs_add_child_locked(struct vfs_node *parent, struct vfs_node *child)
{
    if (!S_ISDIR(parent->vn_st.st_mode))
        panic("parent is not a directory");

    child->vn_parent = parent;
    child->vn_refcnt++;
    parent->vn_refcnt++;
    list_add_tail(&child->vn_list, &parent->vn_children);
}

static struct vfs_node *vfs_find_child_locked(struct vfs_node *dir, const char *name)
{
    struct vfs_node *vn;

    list_for_each_entry(vn, &dir->vn_children, vn_list) {
        if (0 == strcmp(vn->vn_name, name)) {
            vn_lock(vn, 1);
            return vn;
        }
    }

    return NULL;
}

/* build the path from root to vn by walking vn_parent pointers.
   root must be an ancestor of vn. result does NOT include root's name.
   vn must be locked. returns path in out (e.g. "bin" for /tvm/bin). */
static void vfs_build_path_locked(struct vfs_node *root, struct vfs_node *vn, char *out, size_t outlen)
{
    if (vn == root) { out[0] = 0; return; }

    int depth = 0;
    for (struct vfs_node *p = vn; p && p != root; p = p->vn_parent)
        depth++;

    const char **names = kalloc(depth * sizeof(*names));
    if (!names) { out[0] = 0; return; }

    int i = 0;
    for (struct vfs_node *p = vn; p && p != root && i < depth; p = p->vn_parent)
        names[i++] = p->vn_name;

    out[0] = 0;
    for (i = depth - 1; i >= 0; i--) {
        if (out[0]) strncat(out, "/", outlen - strlen(out) - 1);
        strncat(out, names[i], outlen - strlen(out) - 1);
    }

    kfree(names);
}

static struct vfs_node *vfs_lookup_locked(struct vfs_node *root, const char *relpath, int create, int nofollow, mode_t mode, uid_t uid, gid_t gid)
{
    char tmp[PATH_MAX];
    struct vfs_node *cur = root;

    strncpy(tmp, relpath, sizeof(tmp)-1);
    tmp[PATH_MAX-1] = 0;


    char *saveptr;
    char *tok = strtok_r(tmp, "/", &saveptr);
    char *next_tok;

    while (tok) {
        next_tok = strtok_r(NULL, "/", &saveptr);

        if (strcmp(tok, ".") == 0) {
            tok = next_tok;
            continue;
        }

        struct vfs_node *child = vfs_find_child_locked(cur, tok);

        if (!child) {
            if (create && next_tok == NULL) {
                /* Only create the leaf */
                child = vnalloc(tok, mode, uid, gid);
                if (!child) {
                    if (cur != root)
                        vn_unlock(cur, 1);
                    return NULL;
                }
                child->vn_parent = cur;
                vfs_add_child_locked(cur, child);
                if (cur != root)
                    vn_unlock(cur, 1);
                return child;
            } else {
                if (cur != root)
                    vn_unlock(cur, 1);
                // Set errno explicitly: every vfsop_* returns -1 straight from
                // this NULL, and the device dispatch loops (file_open/file_stat/
                // ...) treat any errno other than ENOTSUP as the VFS's verdict.
                // Leaving errno untouched leaked the caller's stale value (ls
                // reported ENOTTY, cat EBADEXEC, cd EINTR), and a stale ENOTSUP
                // would fall through to the host filesystem for a /tvm path.
                errno = ENOENT;
                return NULL;
            }
        }

        /* resolve symlinks for intermediate components, and for the
           final component when not creating */
        if (S_ISLNK(child->vn_st.st_mode) && (next_tok || (!create && !nofollow))) {
            if (!child->vn_data || child->vn_size >= PATH_MAX) {
                vn_unlock(child, 1);
                if (cur != root) vn_unlock(cur, 1);
                errno = EINVAL;
                return NULL;
            }

            char link_target[PATH_MAX];
            memcpy(link_target, child->vn_data, child->vn_size);
            link_target[child->vn_size] = 0;

            /* An absolute symlink used to be refused outright (ENOTSUP,
               "can't resolve within VFS root"), so a cpio entry such as
               /tvm/libexec/git-core/git-remote-https -> /tvm/bin/git was
               a dead link: stat() followed it to ENOENT and git could not
               exec its own helper. One that points back into this mount
               is resolved from the mount root, the way a relative one is
               resolved from its directory: the rest of the path is
               appended and the lookup restarts. A target outside the mount
               (a host path) still cannot be reached from here. */
            char newpath[PATH_MAX];
            char cur_path[PATH_MAX];
            if (link_target[0] == '/') {
                size_t vnlen = strlen(root->vn_name);
                if (strncmp(link_target, root->vn_name, vnlen) != 0 ||
                    (link_target[vnlen] != '/' && link_target[vnlen] != 0)) {
                    vn_unlock(child, 1);
                    if (cur != root) vn_unlock(cur, 1);
                    errno = ENOTSUP;
                    return NULL;
                }
                /* drop the mount prefix: the target is relative to root */
                memmove(link_target, link_target + vnlen + (link_target[vnlen] == '/' ? 1 : 0),
                        strlen(link_target + vnlen + (link_target[vnlen] == '/' ? 1 : 0)) + 1);
                cur_path[0] = 0;
            }
            else {
                /* relative symlink: resolve against cur (the parent dir)
                   build new path = path_to_cur/target[/remaining...] */
                vfs_build_path_locked(root, cur, cur_path, sizeof(cur_path));
            }

            vn_unlock(child, 1);

            /* reconstruct remaining components after the symlink */
            if (next_tok) {
                char rest[PATH_MAX];
                if (saveptr && saveptr[0])
                    snprintf(rest, sizeof(rest), "%s/%s", next_tok, saveptr);
                else
                    strncpy(rest, next_tok, sizeof(rest)-1);

                if (cur_path[0])
                    snprintf(newpath, sizeof(newpath), "%s/%s/%s", cur_path, link_target, rest);
                else
                    snprintf(newpath, sizeof(newpath), "%s/%s", link_target, rest);
            } else {
                if (cur_path[0])
                    snprintf(newpath, sizeof(newpath), "%s/%s", cur_path, link_target);
                else
                    strncpy(newpath, link_target, sizeof(newpath)-1);
            }

            if (cur != root)
                vn_unlock(cur, 1);

            return vfs_lookup_locked(root, newpath, 0, 0, 0, 0, 0);
        }

        if (next_tok && !S_ISDIR(child->vn_st.st_mode)) {
            vn_unlock(child, 1);
            if (cur != root)
                vn_unlock(cur, 1);
            errno = ENOTDIR;
            return NULL;
        }

        if (cur != root)
            vn_unlock(cur, 1);
        cur = child;
        tok = next_tok;
    }

    return cur;
}

static struct vfs_node *vfs_lookup(const char *abspath, int create, int nofollow, mode_t mode, uid_t uid, gid_t gid)
{
    int pathlen = strlen(abspath);

    VFS_LOCK();
    struct vfs_node *root;
    list_for_each_entry(root, &vfs_list, vn_list) {
        int vnlen = strlen(root->vn_name);
        if (pathlen < vnlen)
            continue;

        if (0 == strncmp(root->vn_name, abspath, vnlen) &&
            (abspath[vnlen] == 0 || abspath[vnlen] == '/')) {
            vn_lock(root, 1);
            VFS_UNLOCK();

            if (abspath[vnlen] == 0)
                return root;

            struct vfs_node *vn = vfs_lookup_locked(root, abspath + vnlen + 1, create, nofollow, mode, uid, gid);
            // vfs_lookup_locked returns the root itself, without a reference
            // of its own, when the path resolves to it ("/tvm/.", a symlink
            // to "/tvm"): hand over the reference taken above instead of
            // dropping it, or the caller's release is a second one (refcount
            // underflow on the mount root).
            if (vn != root)
                vn_unlock(root, 1);
            return vn;
        }
    }
    VFS_UNLOCK();

    // not root was found, not even in vfs
    errno = ENOTSUP;
    return NULL;
}

static void vfs_add_root(struct vfs_node *root) {
    VFS_LOCK();
    vn_lock(root, 1);
    VERIFY(!root->vn_parent);
    root->vn_refcnt++;
    list_add(&root->vn_list, &vfs_list);
    vn_unlock(root, 1);
    VFS_UNLOCK();
}

struct vfs_file {
    int vf_oflags;
    int vf_cursor;
    struct vfs_node *vf_vn;

    size_t vf_refcnt;
    pthread_mutex_t vf_lock;
};

static struct vfs_file * vfalloc()
{
    struct vfs_file *vf = kalloc(sizeof(*vf));
    if (!vf)
        return NULL;
    memset(vf, 0, sizeof(*vf));

    vf->vf_refcnt = 1;
    pthread_mutex_init(&vf->vf_lock, NULL);
    pthread_mutex_lock(&vf->vf_lock);

    return vf;
}

static void vfdealloc_locked(struct vfs_file *vf)
{
    VERIFY(!vf->vf_refcnt);
    VERIFY(!vf->vf_vn);

    pthread_mutex_unlock(&vf->vf_lock);
    pthread_mutex_destroy(&vf->vf_lock);
    kfree(vf);
}

void vf_lock(struct vfs_file *vf, int refcount) {
    pthread_mutex_lock(&vf->vf_lock);
    vf->vf_refcnt += refcount;
}

void vf_unlock(struct vfs_file *vf, int refcount) {
    // very important that refcount == 0 wouldn't trigger free
    if (refcount && 0 == (vf->vf_refcnt -= refcount)) {
        vfdealloc_locked(vf);
        return;
    }
    pthread_mutex_unlock(&vf->vf_lock);
}

static struct vfs_file *vf_for_file_locked(struct file *file) {
    VERIFY(file->f_type == DEV_VFS);
    VERIFY(file->f_data);

    struct vfs_file *vf = (struct vfs_file *)file->f_data;
    vf_lock(vf, 1);
    return vf;
}

static struct vfs_node *vn_for_vf_locked(struct vfs_file *vf) {
    struct vfs_node *vn = vf->vf_vn;
    vf_unlock(vf, 0);
    vn_lock(vn, 1);
    vf_lock(vf, 0);
    return vn;
}

static struct vfs_node *vn_for_file_locked(struct file *file) {
    struct vfs_file *vf = vf_for_file_locked(file);
    if (!vf)
        return NULL;

    file_unlock(file, 0);
    struct vfs_node *vn = vn_for_vf_locked(vf);
    file_lock(file, 0);
    vf_unlock(vf, 1);
    return vn;
}

static struct vfs_node *vn_for_pathname(const char *pathname) {
    return vfs_lookup(pathname, 0, 0, 0, 0, 0);
}

static struct vfs_node *vn_for_pathname_nofollow(const char *pathname) {
    return vfs_lookup(pathname, 0, 1, 0, 0, 0);
}

static struct fops *vfsops();

static int vfsop_open(const char *pathname, int flags, mode_t mode, uid_t uid, gid_t gid, struct file *out) {
    file_unlock(out, 0);

    // The VFS holds cpio mounts and is read-only: vfsop_write is unimplemented
    // (ENOTSUP) and nothing in it ever persists. Opens for writing used to
    // succeed anyway, and O_CREAT even minted a phantom in-memory node, so a
    // program that probes writability by opening was misled: sqlite3's
    // unixOpen() tries O_RDWR|O_CREAT first and only falls back to O_RDONLY
    // when that fails, so `sqlite3 /tvm/etc/x.db 'create table ...'` got an
    // empty node, took the database for writable and failed at its first
    // write with "disk I/O error" -- leaving /tvm/etc/x.db behind -- instead
    // of "unable to open database file"; `echo x > /tvm/etc/foo` likewise
    // created foo and then failed with "Not supported". Refuse write access
    // and creation with EROFS, as a read-only mount does, so callers take
    // their read-only paths (an existing file opens read-only, and a write to
    // it is then reported as such: "attempt to write a readonly database").
    // The lookup comes first: a path outside every mount is ENOTSUP, which
    // file_open() takes as "not ours" and hands to the host filesystem.
    int nofollow = !!(flags & O_NOFOLLOW);
    struct vfs_node *vn = vfs_lookup(pathname, 0, nofollow, mode, uid, gid);
    if (!vn) {
        if ((flags & O_CREAT) && errno == ENOENT)
            errno = EROFS;
        file_lock(out, 0);
        return -1;
    }
    if ((flags & O_ACCMODE) != O_RDONLY || (flags & (O_TRUNC | O_APPEND)) ||
        (flags & (O_CREAT | O_EXCL)) == (O_CREAT | O_EXCL)) {
        int last_errno = ((flags & (O_CREAT | O_EXCL)) == (O_CREAT | O_EXCL)) ? EEXIST : EROFS;
        vn_unlock(vn, 1);
        file_lock(out, 0);
        errno = last_errno;
        return -1;
    }

    if (nofollow && S_ISLNK(vn->vn_st.st_mode)) {
        vn_unlock(vn, 1);
        file_lock(out, 0);
        errno = ELOOP;
        return -1;
    }

    struct vfs_file *vf = vfalloc();
    if (!vf) {
        vn_unlock(vn, 1);
        file_lock(out, 0);
        return -1;
    }

    int rfd;
    if (S_ISDIR(vn->vn_st.st_mode))
        rfd = CALL_FUNC(open, "/", (flags & O_ACCMODE) | O_DIRECTORY);
    else
        rfd = CALL_FUNC(open, "/dev/null", (flags & O_ACCMODE));

    if (-1 == rfd) {
        vf_unlock(vf, 1);
        vn_unlock(vn, 1);
        file_lock(out, 0);
        return -1;
    }

    vf->vf_oflags = flags;
    vf->vf_vn = vn;
    vf->vf_refcnt++;
    vn->vn_refcnt++;

    vf_unlock(vf, 1);
    vn_unlock(vn, 1);

    file_lock(out, 0);
    out->f_rfd = rfd_adopt(rfd);
    out->f_type = DEV_VFS;
    out->f_data = (void *)vf;
    out->f_ops = vfsops();

    return 0;
}

static int vfsop_dup(struct file *oldfile, struct file *newfile) {

    struct vfs_file *vf = vf_for_file_locked(oldfile);
    if (!vf)
        return -1;

    vf->vf_refcnt++;
    newfile->f_data = (void *)vf;
    vf_unlock(vf, 1);
    return 0;
}

static void vfsop_close(struct file *file) {
    struct vfs_file *vf = vf_for_file_locked(file);
    if (!vf)
        return;

    file_unlock(file, 0);
    struct vfs_node *vn = vn_for_vf_locked(vf);
    file_lock(file, 0);
    if (!vn) {
        vf_unlock(vf, 1);
        return;
    }

    // we took ref in vf_for_file_locked()
    vf->vf_refcnt--;
    if (vf->vf_refcnt > 1) {
        vf_unlock(vf, 1);
        vn_unlock(vn, 1);
        return;
    }

    VERIFY(vf->vf_refcnt == 1);

    vn->vn_refcnt--;
    vf->vf_vn = NULL;
    vn_unlock(vn, 1);

    file->f_data = NULL;
    vf_unlock(vf, 1); // will dealloc
}

static int vn_stat_locked(struct vfs_node *vn, struct stat *st) {
    if (!st) {
        return 0;
    }

    memcpy(st, &vn->vn_st, sizeof(*st));
    st->st_size = vn->vn_size;

    return 0;
}

static int vfsop_fstat(struct file *file, struct stat *st)
{
    struct vfs_node *vn = vn_for_file_locked(file);
    if (!vn)
        return -1;

    int ret = vn_stat_locked(vn, st);
    vn_unlock(vn, 1);
    return ret;
}

static int vfsop_stat(const char *pathname, struct stat *st, int flag)
{
    struct vfs_node *vn = (flag & AT_SYMLINK_NOFOLLOW)
        ? vn_for_pathname_nofollow(pathname)
        : vn_for_pathname(pathname);
    if (!vn)
        return -1;

    int ret = vn_stat_locked(vn, st);
    vn_unlock(vn, 1);
    return ret;
}

static ssize_t vfsop_readlink(const char *pathname, char *buf, size_t bufsiz)
{
    struct vfs_node *vn = vn_for_pathname_nofollow(pathname);
    if (!vn)
        return -1;

    if (!S_ISLNK(vn->vn_st.st_mode)) {
        vn_unlock(vn, 1);
        errno = EINVAL;
        return -1;
    }

    size_t len = vn->vn_size;
    if (len > bufsiz) len = bufsiz;
    memcpy(buf, vn->vn_data, len);
    vn_unlock(vn, 1);
    return (ssize_t)len;
}

static int vn_chmod_locked(struct vfs_node *vn, mode_t mode)
{
    vn->vn_st.st_mode = (vn->vn_st.st_mode & ~07777) | (mode & 07777);
    return 0;
}

static int vfsop_fchmod(struct file *file, mode_t mode)
{
    struct vfs_node *vn = vn_for_file_locked(file);
    if (!vn)
        return -1;

    int ret = vn_chmod_locked(vn, mode);
    vn_unlock(vn, 1);
    return ret;
}

static int vfsop_chmod(const char *pathname, mode_t mode, int flag)
{
    struct vfs_node *vn = vn_for_pathname(pathname);
    if (!vn)
        return -1;

    int ret = vn_chmod_locked(vn, mode);
    vn_unlock(vn, 1);
    return ret;
}

static int vn_chown_locked(struct vfs_node *vn, uid_t owner, gid_t group)
{
    vn->vn_st.st_uid = owner;
    vn->vn_st.st_gid = group;
    return 0;
}

static int vfsop_fchown(struct file *file, uid_t owner, gid_t group)
{
    struct vfs_node *vn = vn_for_file_locked(file);
    if (!vn)
        return -1;

    int ret = vn_chown_locked(vn, owner, group);
    vn_unlock(vn, 1);
    return ret;
}

static int vfsop_chown(const char *pathname, uid_t owner, gid_t group, int flag)
{
    struct vfs_node *vn = vn_for_pathname(pathname);
    if (!vn)
        return -1;

    int ret = vn_chown_locked(vn, owner, group);
    vn_unlock(vn, 1);
    return ret;
}

static int vfsop_ioctl(struct file *file, unsigned long request, void *arg)
{
    errno = ENOTSUP;
    return -1;
}

static ssize_t vfsop_read(struct file *file, void *buf, size_t count, int offset)
{
    struct vfs_file *vf = vf_for_file_locked(file);
    if (!vf)
        return -1;

    file_unlock(file, 0);
    struct vfs_node *vn = vn_for_vf_locked(vf);
    file_lock(file, 0);
    if (!vn) {
        vf_unlock(vf, 1);
        return -1;
    }

    if (S_ISDIR(vn->vn_st.st_mode) || (vf->vf_oflags & O_DIRECTORY)) {
        vf_unlock(vf, 1);
        vn_unlock(vn, 1);
        errno = EISDIR;
        return -1;
    }

    if (O_RDONLY != (vf->vf_oflags & (O_ACCMODE)) &&
        O_RDWR   != (vf->vf_oflags & (O_ACCMODE))) {
        vf_unlock(vf, 1);
        vn_unlock(vn, 1);
        errno = EBADF;
        return -1;
    }

    size_t op_offset = offset;
    if (offset < 0)
        op_offset = vf->vf_cursor;

    // read after size return no data
    if (op_offset > vn->vn_size) {
        vf_unlock(vf, 1);
        vn_unlock(vn, 1);
        return 0;
    }

    size_t op_left = vn->vn_size - op_offset;

    size_t op_size = count;
    if (op_size > op_left)
        op_size = op_left;

    memcpy(buf, vn->vn_data + op_offset, op_size);

    if (offset == -1)
        vf->vf_cursor += op_size;

    vf_unlock(vf, 1);
    vn_unlock(vn, 1);

    return op_size;
}

static ssize_t vfsop_write(struct file *file, const void *buf, size_t count, int offset)
{
    // not implemented
    errno = ENOTSUP;
    return -1;
}

static off_t vfsop_lseek(struct file *file, off_t offset, int whence)
{
    struct vfs_file *vf = vf_for_file_locked(file);
    if (!vf)
        return -1;

    file_unlock(file, 0);
    struct vfs_node *vn = vn_for_vf_locked(vf);
    file_lock(file, 0);

    off_t new_pos;
    switch (whence) {
    case SEEK_SET: new_pos = offset; break;
    case SEEK_CUR: new_pos = vf->vf_cursor + offset; break;
    case SEEK_END: new_pos = (off_t)vn->vn_size + offset; break;
    default:
        vf_unlock(vf, 1);
        vn_unlock(vn, 1);
        errno = EINVAL;
        return -1;
    }

    vn_unlock(vn, 1);

    if (new_pos < 0) {
        vf_unlock(vf, 1);
        errno = EINVAL;
        return -1;
    }

    vf->vf_cursor = new_pos;
    vf_unlock(vf, 1);
    return new_pos;
}

static int vfsop_readdir(struct file *file, struct dirent *out)
{
    struct vfs_file *vf = vf_for_file_locked(file);
    if (!vf)
        return -1;

    file_unlock(file, 0);
    struct vfs_node *vn = vn_for_vf_locked(vf);
    file_lock(file, 0);
    if (!vn) {
        vf_unlock(vf, 1);
        return -1;
    }

    if (!S_ISDIR(vn->vn_st.st_mode)) {
        vf_unlock(vf, 1);
        vn_unlock(vn, 1);
        errno = ENOTDIR;
        return -1;
    }

    int cursor = vf->vf_cursor;
    file_unlock(file, 0);
    vf_unlock(vf, 0);

    int i = 0;
    struct vfs_node *child;
    list_for_each_entry(child, &vn->vn_children, vn_list) {
        if (i == cursor)
            break;
        i++;
    }

    // end of children
    if (list_is_head(&child->vn_list, &vn->vn_children)) {
        vn_unlock(vn, 1);
        file_lock(file, 0);
        vf_lock(vf, 0);
        vf_unlock(vf, 1);
        return -1;
    }

    // inc cursor and release vf
    vf_lock(vf, 0);
    vf->vf_cursor++;
    vf_unlock(vf, 1);

    vn_lock(child, 1);
    vn_unlock(vn, 1);

    int namelen = strlen(child->vn_name);
    if (namelen > NAME_MAX)
        namelen = NAME_MAX;

    // all other fields are not specified in POSIX readdir
    out->d_ino = child->vn_st.st_ino;
    memcpy(out->d_name, child->vn_name, namelen);
    out->d_name[namelen] = 0;

    vn_unlock(child, 1);
    file_lock(file, 0);

    return 0;
}

void *vfsop_mmap(void *hint, size_t size, int prot, int flags, struct file *file, off_t offset) {
    struct vfs_node *vn = vn_for_file_locked(file);
    if (!vn)
        return MAP_FAILED;

    if (!S_ISREG(vn->vn_st.st_mode)) {
        vn_unlock(vn, 1);
        errno = ENOTSUP;
        return MAP_FAILED;
    }

    void *ret = CALL_FUNC(mmap, hint, size, PROT_READ | PROT_WRITE, flags | MAP_ANONYMOUS, -1, 0);
    if (ret == MAP_FAILED) {
        vn_unlock(vn, 1);
        return MAP_FAILED;
    }

    size_t op_offset = offset;
    size_t op_left = 0;

    if (op_offset < vn->vn_size)
        op_left = vn->vn_size - op_offset;

    size_t op_size = size;
    if (op_size > op_left)
        op_size = op_left;

    memcpy(ret, vn->vn_data + op_offset, op_size);

    if (prot != (PROT_READ | PROT_WRITE)) {
        VERIFY(0 == CALL_FUNC(mprotect, ret, size, prot));
    }

    vn_unlock(vn, 1);

    return ret;
}

static struct device devvfs = {
    .dev_type = DEV_VFS,
    .dev_fops = {
        .f_open = vfsop_open,
        .f_dup = vfsop_dup,

        .f_fstat = vfsop_fstat,
        .f_fchown = vfsop_fchown,
        .f_fchmod = vfsop_fchmod,

        .f_stat = vfsop_stat,
        .f_chmod = vfsop_chmod,
        .f_chown = vfsop_chown,
        .f_readlink = vfsop_readlink,

        .f_ioctl = vfsop_ioctl,
        .f_read = vfsop_read,
        .f_write = vfsop_write,
        .f_lseek = vfsop_lseek,
        .f_close = vfsop_close,

        .f_readdir = vfsop_readdir,
        .f_mmap = vfsop_mmap,
    }
};

REGISTER_DEV(devvfs);

static struct fops *vfsops() {
    return &devvfs.dev_fops;
}

static int vfs_load_cpio_cb(struct vfs_node *vn, const cpio_t *cpio) {
    if (S_ISDIR(cpio->c_mode) ||
        S_ISREG(cpio->c_mode) ||
        S_ISLNK(cpio->c_mode)) {
        // cpio names are "./bin/sh" — strip "./" so VFS paths resolve correctly
        const char *name = cpio->name;
        while (name[0] == '.' && name[1] == '/') name += 2;
        while (name[0] == '/') name++;
        if (name[0] == '.' && name[1] == '\0') return 0;
        if (name[0] == '\0') return 0;
        struct vfs_node *child = vfs_lookup_locked(vn, name, 1, 0, cpio->c_mode, cpio->c_uid, cpio->c_gid);
        if (!child) {
            // TODO clear all children
            return -1;
        }

        if (!S_ISDIR(cpio->c_mode)) {
            if (cpio->c_filesize) {
                void *buf = kalloc(cpio->c_filesize + 1);
                if (!buf) {
                    // TODO clear all children
                    vn_unlock(child, 1);
                    return -1;
                }
                memcpy(buf, cpio->data, cpio->c_filesize);
                ((char *)buf)[cpio->c_filesize] = 0;

                child->vn_data = buf;
                child->vn_size = cpio->c_filesize;
            }
        }

        if (child != vn)
            vn_unlock(child, 1);
    }

    return 0;
}

static int vfs_load_cpio(struct vfs_node *vn, const void *buf, size_t len) {
    return cpio_parse(buf, len, (cpio_cb)vfs_load_cpio_cb, vn);
}

/////////////
// MARK: Copy-On-Write (COW) Fixup
/////////////

/*
 * alloca() for bytes that must survive a fork unchanged, e.g. strings.
 *
 * fork copies the stack between sp and thr_fork_fp and cow_fixup_process()
 * then rewrites every 8-byte word whose value falls inside a copied range: it
 * cannot tell pointers from data. alloca(n) leaves the bytes between n and
 * the next word boundary as they were, stale stack contents such as the high
 * bytes of an earlier stack address. The last word of a short string ("m\0"
 * plus those stale bytes) can then read as an address in the parent's stack
 * and get "relocated" into the child's: the final character and the NUL are
 * overwritten, and `tvm shell 'echo hello_system'` printed
 * "hello_syste}\xf2Lp\x01" (a 0x1xxxxxxxx thread-stack address). Rounding
 * up to whole words and zeroing keeps such a word below any mapped address.
 */
#define tvm_alloca_zero(n) ({ \
    size_t _az_n = ((size_t)(n) + sizeof(uintptr_t) - 1) & ~(sizeof(uintptr_t) - 1); \
    void *_az_p = alloca(_az_n); \
    memset(_az_p, 0, _az_n); \
    _az_p; })

struct cow_fixup_context {
    struct {
        void *new;
        void *old;
        size_t size;
        int fixup;
    } *ptrs;
    size_t count;

    // Lookup index over ptrs, rebuilt when ranges were added since the last
    // build (see cow_fixup_index).
    size_t *order;          // indices into ptrs, sorted by old address
    size_t indexed;         // count the index was built for
    uintptr_t lo, hi;       // lowest old start, highest old end
    int overlap;            // some old ranges overlap: use the linear scan
};

static int cow_fixup_ctx_add(struct cow_fixup_context *dctx, void *new, void *old, size_t size, int fixup)
{
    void *nptrs = krealloc(dctx->ptrs, sizeof(dctx->ptrs[0]) * (dctx->count + 1));
    if (!nptrs) {
        errno = ENOMEM;
        return -1;
    }

    dctx->ptrs = nptrs; 
    dctx->ptrs[dctx->count].new = new;
    dctx->ptrs[dctx->count].old = old;
    dctx->ptrs[dctx->count].size = size;
    dctx->ptrs[dctx->count].fixup = fixup;
    dctx->count++;

    return 0;
}

static void *cow_fixup_translate_linear(struct cow_fixup_context *dctx, void *p)
{
    for (int i = 0; i < dctx->count; i++)
    {
        if ((uintptr_t)p < (uintptr_t)dctx->ptrs[i].old || (uintptr_t)p >= (uintptr_t)dctx->ptrs[i].old + dctx->ptrs[i].size)
            continue;

        return (void *)((uintptr_t)p - (uintptr_t)dctx->ptrs[i].old + (uintptr_t)dctx->ptrs[i].new);
    }

    return p;
}

/*
 * Build the sorted index used by cow_fixup_translate.
 *
 * cow_fixup_process() translates every pointer-sized word of every copied
 * writable region, and it used to test each word against every range in
 * insertion order. A fork of a shell whose heap holds a large script (bash
 * running neofetch: several MB) did (heap words) x (ranges) comparisons:
 * ~10 ms per MB of used heap, 100+ ms per fork, which is where almost all of
 * neofetch's 11 s went (sampled: cow_fixup_translate was the hottest
 * on-CPU function by far). The index turns that into a min/max reject for
 * the common non-pointer word plus a binary search. With overlapping old
 * ranges the linear scan's "first added range wins" rule could differ from
 * a binary search, so that case keeps the linear scan.
 */
static void cow_fixup_index(struct cow_fixup_context *dctx)
{
    if (dctx->indexed == dctx->count && dctx->order)
        return;

    size_t *order = krealloc(dctx->order, sizeof(*order) * (dctx->count ? dctx->count : 1));
    if (!order) {
        // no index: everything goes through the linear scan
        dctx->overlap = 1;
        dctx->indexed = dctx->count;
        return;
    }
    dctx->order = order;

    // insertion sort by old start: counts are small (tens to hundreds)
    for (size_t i = 0; i < dctx->count; i++) {
        size_t j = i;
        while (j > 0 && (uintptr_t)dctx->ptrs[order[j - 1]].old > (uintptr_t)dctx->ptrs[i].old) {
            order[j] = order[j - 1];
            j--;
        }
        order[j] = i;
    }

    dctx->overlap = 0;
    dctx->lo = UINTPTR_MAX;
    dctx->hi = 0;
    for (size_t k = 0; k < dctx->count; k++) {
        uintptr_t start = (uintptr_t)dctx->ptrs[order[k]].old;
        uintptr_t end = start + dctx->ptrs[order[k]].size;
        if (start < dctx->lo)
            dctx->lo = start;
        if (end > dctx->hi)
            dctx->hi = end;
        if (k > 0) {
            uintptr_t prev_end = (uintptr_t)dctx->ptrs[order[k - 1]].old + dctx->ptrs[order[k - 1]].size;
            if (prev_end > start)
                dctx->overlap = 1;
        }
    }
    dctx->indexed = dctx->count;
}

// Index lookup; the caller has built the index and checked there is no
// overlap. Inlined into the per-word loop of cow_fixup_process.
static inline __attribute__((always_inline)) void *cow_fixup_translate_indexed(struct cow_fixup_context *dctx, void *p)
{
    uintptr_t v = (uintptr_t)p;
    if (v < dctx->lo || v >= dctx->hi)
        return p;

    size_t lo = 0, hi = dctx->count;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if ((uintptr_t)dctx->ptrs[dctx->order[mid]].old <= v)
            lo = mid + 1;
        else
            hi = mid;
    }
    if (lo == 0)
        return p;
    size_t i = dctx->order[lo - 1];
    if (v >= (uintptr_t)dctx->ptrs[i].old + dctx->ptrs[i].size)
        return p;
    return (void *)(v - (uintptr_t)dctx->ptrs[i].old + (uintptr_t)dctx->ptrs[i].new);
}

static void *cow_fixup_translate(struct cow_fixup_context *dctx, void *p)
{
    cow_fixup_index(dctx);
    if (dctx->overlap)
        return cow_fixup_translate_linear(dctx, p);

    uintptr_t v = (uintptr_t)p;
    if (v < dctx->lo || v >= dctx->hi)
        return p;

    // last range whose old start is <= v
    size_t lo = 0, hi = dctx->count;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if ((uintptr_t)dctx->ptrs[dctx->order[mid]].old <= v)
            lo = mid + 1;
        else
            hi = mid;
    }
    if (lo == 0)
        return p;
    size_t i = dctx->order[lo - 1];
    if (v >= (uintptr_t)dctx->ptrs[i].old + dctx->ptrs[i].size)
        return p;
    return (void *)(v - (uintptr_t)dctx->ptrs[i].old + (uintptr_t)dctx->ptrs[i].new);
}

static void cow_fixup_process(struct cow_fixup_context *dctx)
{
    // The ranges do not change during the scan: build the index once and use
    // the inlined lookup per word (this loop visits every word of every
    // copied writable region; a call plus an index check per word was a third
    // of what remained of fork's cost).
    cow_fixup_index(dctx);
    int indexed = !dctx->overlap;

    for (int i = 0; i < dctx->count; i++) {
        // only used for other fixups
        if (!dctx->ptrs[i].fixup)
            continue;

        void **end = (void **)((uintptr_t)dctx->ptrs[i].new + (dctx->ptrs[i].size & ~(sizeof(void *) - 1)));
        for (void **p = (void **)dctx->ptrs[i].new; p < end; p++) {
            if (indexed) {
                uintptr_t v = (uintptr_t)*p;
                if (v < dctx->lo || v >= dctx->hi)
                    continue;  // not a pointer into any copied range
                *p = cow_fixup_translate_indexed(dctx, *p);
            }
            else
                *p = cow_fixup_translate_linear(dctx, *p);
        }
    }
}

/////////////
// MARK: Exec
/////////////

/**
 * Honestly the nicest code I've written in this shithole of a project.
 * 
 * Behind the scenes, exec() can never really exec() because the OS will kill our virtual-memory address-space... and our threads.
 * So, we re-initialize all COW globals to their original values (or zero) and run a main function with the arguments.
 * 
 * New main functions can be registered in the host process by calling `tvm_register_program()` with its "virtual" pathnames.
 * Make sure these paths are in PATH environment of the calling process.
 */

typedef int (*main_func_t)(int, char * const*, char * const*);
struct exec_program {
    struct list_head list;
    const char *file;
    const char *pathname;
    main_func_t main_routine;
};
static LIST_HEAD(exec_programs);
static pthread_mutex_t programs_lock = PTHREAD_MUTEX_INITIALIZER;

void tvm_register_program(const char *pathname, main_func_t main_routine)
{
    if (!pathname || !pathname[0])
        panic("pathname");
    
    struct exec_program *prog;
    if (NULL == (prog = malloc(sizeof(*prog))))
        panic("prog");
    
    prog->pathname = strdup(pathname);
    if (!prog->pathname)
        panic("strdup");
    
    prog->main_routine = main_routine;
    prog->file = strrchr(prog->pathname, '/');
    if (prog->file)
        prog->file++;
    else
        prog->file = prog->pathname;
    
    if (!prog->file[0])
        panic("program file \"%s\"", prog->pathname);
    
    pthread_mutex_lock(&programs_lock);
    list_add_tail(&prog->list, &exec_programs);
    pthread_mutex_unlock(&programs_lock);
}

static main_func_t find_program(const char *pathname)
{
    pthread_mutex_lock(&programs_lock);
    struct exec_program *prog;
    list_for_each_entry(prog, &exec_programs, list) {
        if (0 == strcmp(pathname, prog->pathname)) {
            main_func_t f = prog->main_routine;
            pthread_mutex_unlock(&programs_lock);
            return f;
        }
    }
    pthread_mutex_unlock(&programs_lock);

    return NULL;
}

static const char *find_program_pathname(const char *file)
{
    pthread_mutex_lock(&programs_lock);
    struct exec_program *prog;
    list_for_each_entry(prog, &exec_programs, list) {
        INFO("%s %s", file, prog->file);
        if (0 == strcmp(file, prog->file)) {
            const char *p = prog->pathname;
            pthread_mutex_unlock(&programs_lock);
            return p;
        }
    }
    pthread_mutex_unlock(&programs_lock);

    return NULL;
}

/////////////
// MARK: Emulator
/////////////

struct emulator_map_entry {
    struct emulator_map_entry *eme_next;
    uint64_t eme_start;
    size_t   eme_size;
};

void emulator_map_entry_destroy(struct emulator_map_entry *eme) {
    kfree(eme);
}

struct emulator_map_entry *emulator_map_entry_create(uint64_t eme_start, size_t eme_size) {
    struct emulator_map_entry *eme = NULL;

    if (!(eme = kalloc(sizeof(*eme))))
        return NULL;

    eme->eme_start = eme_start;
    eme->eme_size = eme_size;
    return eme;
}

struct emulator_engine {
    struct emulator_engine *ee_next;
    struct emulator_context *ee_ec;
    uc_engine *ee_uc;
    int ee_arch;
    struct emulator_map_entry *ee_eme;
    int ee_refresh;
    int ee_inuse;
};

void emulator_engine_destroy(struct emulator_engine *ee) {
    VERIFY(!ee->ee_inuse);

    while (ee->ee_eme) {
        struct emulator_map_entry *eme = ee->ee_eme;
        ee->ee_eme = eme->eme_next;

        emulator_map_entry_destroy(eme);
    }

    if (ee->ee_uc) {
        uc_close(ee->ee_uc);
        ee->ee_uc = NULL;
    }

    kfree(ee);
}

uc_engine *uc_open_arch(int arch)
{
    uc_err err = UC_ERR_ARCH;
    uc_engine *engine = NULL;

    if (arch == ULARCH_ARM64) {
        err = uc_open(UC_ARCH_ARM64, UC_MODE_ARM, &engine);
        if (!err)
            uc_ctl_set_cpu_model(engine, UC_CPU_ARM64_MAX);
    }
    else if (arch == ULARCH_X86_64) {
        err = uc_open(UC_ARCH_X86, UC_MODE_64, &engine);
#if defined(__linux__) && defined(__x86_64__)
        uint64_t fs_base;
        if (0 != syscall(SYS_arch_prctl, ARCH_GET_FS, &fs_base)) {
            INFO("arch_prctl(ARCH_GET_FS) failed: %d (%s)", errno, strerror(errno));
            uc_close(engine);
            return NULL;
        }

        err = uc_reg_write(engine, UC_X86_REG_FS_BASE, &fs_base);
        if (err) {
            INFO("uc_reg_write(UC_X86_REG_FS_BASE) failed: %s", uc_strerror(err));
            uc_close(engine);
            return NULL;
        }
#endif
    }

    if (err) {
        INFO("uc_open(ess_arch=%d) failed: %s", arch, uc_strerror(err));
        return NULL;
    }
    uc_ctl_exits_enable(engine);

    return engine;
}

struct emulator_context;
bool emulator_hook_cb(uc_engine *uc, uc_mem_type type, uint64_t address, int size, int64_t value, struct emulator_context *ec);

static void emulator_debug_hook_cb(uc_engine *uc,
                         uint64_t address,
                         uint32_t size,
                         void *user_data)
{
    INFO("emulator_debug: PC = %p, size = %u", (void *)address, size);
}

#define EMULATOR_ADDRESS_MIN ((uint64_t)0x4000)
#define EMULATOR_ADDRESS_MAX ((uint64_t)0xFFFFFFFFFFFFC000)
// Page containing EMULATOR_CALL_END_LR (0xDEADBEEF4141) — must remain unmapped
// so unicorn fires UC_MEM_FETCH_UNMAPPED instead of trying to execute bytes there
#define EMULATOR_SENTINEL_PAGE ((uint64_t)0xDEADBEEF4000)
#define EMULATOR_SENTINEL_SIZE ((uint64_t)0x1000)

int emulator_engine_reset(struct emulator_engine *ee) {
    int ret = 1;
    uc_err err;
    uc_engine *engine = NULL;

    if (!(engine = uc_open_arch(ee->ee_arch)))
        goto out;

    uc_ctl(engine, UC_CTL_TLB_TYPE, UC_TLB_CPU);

    uc_hook hh;
    err = uc_hook_add(engine, &hh, UC_HOOK_MEM_INVALID, emulator_hook_cb, ee->ee_ec, 1, 0);
    if (err) {
        INFO("uc_hook_add(UC_HOOK_MEM_INVALID) failed: %s", uc_strerror(err));
        goto out;
    }

    // uc_hook_add(engine, &hh,
    //         UC_HOOK_CODE,
    //         emulator_debug_hook_cb,
    //         NULL,
    //         1, 0);

    // Map emulator map entries as RWX while the rest are mapped as RW
    // NOTE: This is to allow passthrough memory to host but allow us to tightly control which code is executed on guest and which on host
    // The sentinel page (containing EMULATOR_CALL_END_LR) is left unmapped so
    // unicorn fires UC_MEM_FETCH_UNMAPPED cleanly instead of trying to execute bytes there.

    #define EMU_MAP_RW(eng, start, size) do { \
        uint64_t _s = (start), _sz = (size); \
        uint64_t _sentinel = EMULATOR_SENTINEL_PAGE; \
        uint64_t _sentinel_end = _sentinel + EMULATOR_SENTINEL_SIZE; \
        if (_s < _sentinel && _s + _sz > _sentinel) { \
            if (_s < _sentinel) { \
                err = uc_mem_map_ptr(eng, _s, _sentinel - _s, UC_PROT_READ | UC_PROT_WRITE, (void *)_s); \
                if (err) { INFO("uc_mem_map_ptr failed: %s", uc_strerror(err)); goto out; } \
            } \
            if (_sentinel_end < _s + _sz) { \
                err = uc_mem_map_ptr(eng, _sentinel_end, (_s + _sz) - _sentinel_end, UC_PROT_READ | UC_PROT_WRITE, (void *)_sentinel_end); \
                if (err) { INFO("uc_mem_map_ptr failed: %s", uc_strerror(err)); goto out; } \
            } \
        } else if (_s >= _sentinel && _s < _sentinel_end) { \
            if (_sentinel_end < _s + _sz) { \
                err = uc_mem_map_ptr(eng, _sentinel_end, (_s + _sz) - _sentinel_end, UC_PROT_READ | UC_PROT_WRITE, (void *)_sentinel_end); \
                if (err) { INFO("uc_mem_map_ptr failed: %s", uc_strerror(err)); goto out; } \
            } \
        } else if (_sz > 0) { \
            err = uc_mem_map_ptr(eng, _s, _sz, UC_PROT_READ | UC_PROT_WRITE, (void *)_s); \
            if (err) { INFO("uc_mem_map_ptr failed: %s", uc_strerror(err)); goto out; } \
        } \
    } while (0)

    uint64_t last_entry_end = EMULATOR_ADDRESS_MIN; // incase there are no entries, map everything

    for (struct emulator_map_entry *eme = ee->ee_eme; eme; eme = eme->eme_next) {
        if (eme->eme_start > last_entry_end) {
            EMU_MAP_RW(engine, last_entry_end, eme->eme_start - last_entry_end);
        }

        err = uc_mem_map_ptr(engine, eme->eme_start, eme->eme_size, UC_PROT_READ | UC_PROT_WRITE | UC_PROT_EXEC, (void *)eme->eme_start);
        if (err) {
            INFO("uc_mem_map_ptr(addr=%p, size=%p, perms=UC_PROT_READ|UC_PROT_WRITE|UC_PROT_EXEC) failed: %s", (void *)eme->eme_start, (void *)eme->eme_size, uc_strerror(err));
            goto out;
        }

        last_entry_end = eme->eme_start + eme->eme_size;
    }

    if (last_entry_end < EMULATOR_ADDRESS_MAX) {
        EMU_MAP_RW(engine, last_entry_end, EMULATOR_ADDRESS_MAX - last_entry_end);
    }

    #undef EMU_MAP_RW

    if (ee->ee_uc) {
        uc_close(ee->ee_uc);
        ee->ee_uc = NULL;
    }

    ee->ee_uc = engine;
    ret = 0;

out:

    if (ret && engine) {
        uc_close(engine);
        engine = NULL;
    }
    return ret;
}

struct emulator_engine *emulator_engine_create(struct emulator_context *ec, int arch)
{
    struct emulator_engine *ee = NULL;
    struct emulator_engine *ret = NULL;

    if (!(ee = kalloc(sizeof(struct emulator_engine))))
        goto out;

    ee->ee_ec = ec;
    ee->ee_arch = arch;
    ee->ee_next = NULL;
    ee->ee_eme = NULL;
    ee->ee_uc = NULL;
    ee->ee_refresh = 0;
    ee->ee_inuse = 0;

    if (0 != emulator_engine_reset(ee))
        goto out;

    ret = ee;

out:
    if (!ret && ee) {
        emulator_engine_destroy(ee);
        ee = NULL;
    }
    return ret;
}

int emulator_engine_add_map_entry(struct emulator_engine *ee, uint64_t start, size_t size) {
    uint64_t end = start + size;

    // Keep the list sorted by address and merge overlapping/adjacent ranges.
    // The same range can be added twice when nested emulations fault it again
    // before a reset consumed the previous add (seen with back-to-back
    // signal-handler dispatches), and emulator_engine_reset's gap-walk assumes
    // an ascending, non-overlapping list — a duplicate or out-of-order entry
    // makes its uc_mem_map_ptr fail with UC_ERR_MAP, permanently wedging the
    // engine (ee_refresh stays set, every subsequent step fails).
    struct emulator_map_entry **link = &ee->ee_eme;
    while (*link && (*link)->eme_start + (*link)->eme_size < start)
        link = &(*link)->eme_next;

    if (*link && (*link)->eme_start <= end) {
        // overlaps or touches: extend this entry, then swallow any later
        // entries the merged range now covers
        struct emulator_map_entry *eme = *link;
        if (start < eme->eme_start)
            eme->eme_start = start;
        if (end > eme->eme_start + eme->eme_size)
            eme->eme_size = end - eme->eme_start;
        while (eme->eme_next && eme->eme_next->eme_start <= eme->eme_start + eme->eme_size) {
            struct emulator_map_entry *victim = eme->eme_next;
            uint64_t victim_end = victim->eme_start + victim->eme_size;
            if (victim_end > eme->eme_start + eme->eme_size)
                eme->eme_size = victim_end - eme->eme_start;
            eme->eme_next = victim->eme_next;
            emulator_map_entry_destroy(victim);
        }
    } else {
        struct emulator_map_entry *eme = emulator_map_entry_create(start, size);
        if (!eme)
            return 1;
        eme->eme_next = *link;
        *link = eme;
    }

    ee->ee_refresh = 1;
    return 0;
}

struct emulator_saved_state_arm64 {
    uint64_t x[29];
    uint64_t fp;
    uint64_t lr;
    uint64_t sp;
    uint64_t pc;
};

struct emulator_saved_state_x86_64 {
    uint64_t rax;
    uint64_t rbp;
    uint64_t rbx;
    uint64_t rcx;
    uint64_t rdi;
    uint64_t rdx;
    uint64_t rip;
    uint64_t rsi;
    uint64_t rsp;
    uint64_t r8;
    uint64_t r9;
    uint64_t r10;
    uint64_t r11;
    uint64_t r12;
    uint64_t r13;
    uint64_t r14;
    uint64_t r15;
};

#define EMULATOR_STACK_SIZE (0x1000 * 2000) // 8Mb

enum {
    ESS_INTR_UNSPEC = 0,
    ESS_INTR_MEM = 1,
};

struct emulator_saved_state {
    struct emulator_saved_state *ess_next;
    int ess_arch;
    int ess_abi;
    union {
        struct emulator_saved_state_arm64 arm64;
        struct emulator_saved_state_x86_64 x86_64;
    } ess_context;
    void   *ess_stack;
    size_t  ess_stack_size;

    int     ess_intr;

    uc_mem_type ess_intr_memtype;
    uint64_t    ess_intr_addr;
    uint64_t    ess_fault_lr;
};

enum {
    ESS_REG_PC,
    ESS_REG_SP,
    ESS_REG_RET,
};

void emulator_saved_state_destroy(struct emulator_saved_state *ss) {
    if (ss->ess_stack) {
#ifdef TRACE_MMAP
        INFO("munmap %p-%p (emu stack)", ss->ess_stack, (uintptr_t)ss->ess_stack + ss->ess_stack_size);
#endif
        CALL_FUNC(munmap, ss->ess_stack, ss->ess_stack_size);
        ss->ess_stack = NULL;
        ss->ess_stack_size = 0;
    }

    kfree(ss);
}

uint64_t emulator_saved_state_read_reg(struct emulator_saved_state *ss, int reg) {
    uc_err err = UC_ERR_ARCH;
    if (ss->ess_arch == ULARCH_ARM64) {
        if (reg == ESS_REG_PC)  return ss->ess_context.arm64.pc;
        if (reg == ESS_REG_SP)  return ss->ess_context.arm64.sp;
        if (reg == ESS_REG_RET) return ss->ess_context.arm64.x[0];
    }
    else if (ss->ess_arch == ULARCH_X86_64) {
        if (reg == ESS_REG_PC)  return ss->ess_context.x86_64.rip;
        if (reg == ESS_REG_SP)  return ss->ess_context.x86_64.rsp;
        if (reg == ESS_REG_RET) return ss->ess_context.x86_64.rax;
    }

    panic("emulator_saved_state_read_reg(arch=%d, reg=%d) unknown reg", ss->ess_arch, reg);
    __builtin_unreachable();
}

void emulator_saved_state_write_reg(struct emulator_saved_state *ss, int reg, uint64_t val) {
    uc_err err = UC_ERR_ARCH;
    if (ss->ess_arch == ULARCH_ARM64) {
        if (reg == ESS_REG_PC)  { ss->ess_context.arm64.pc   = val; return; }
        if (reg == ESS_REG_SP)  { ss->ess_context.arm64.sp   = val; return; }
        if (reg == ESS_REG_RET) { ss->ess_context.arm64.x[0] = val; return; }
    }
    else if (ss->ess_arch == ULARCH_X86_64) {
        if (reg == ESS_REG_PC)  { ss->ess_context.x86_64.rip = val; return; }
        if (reg == ESS_REG_SP)  { ss->ess_context.x86_64.rsp = val; return; }
        if (reg == ESS_REG_RET) { ss->ess_context.x86_64.rax = val; return; }
    }

    panic("emulator_saved_state_write_reg(arch=%d, reg=%d) unknown reg", ss->ess_arch, reg);
    __builtin_unreachable();
}

uc_context *uc_context_alloc_arch(uc_engine *engine, int arch)
{
    // TODO cache this

    uc_context *context = NULL;
    uc_context *ret = NULL;

    uc_err err = uc_context_alloc(engine, &context);
    if (err) {
        INFO("uc_context_alloc(arch=%d) failed: %s", arch, uc_strerror(err));
        goto out;
    }

    err = uc_context_save(engine, context);
    if (err) {
        INFO("uc_context_save(arch=%d) failed: %s", arch, uc_strerror(err));
        goto out;
    }

    ret = context;

out:
    if (!ret && context) {
        uc_context_free(context);
        context = NULL;
    }

    return ret;
}

struct emulator_saved_state *emulator_saved_state_create(int arch, int abi)
{
    struct emulator_saved_state *ess = NULL;
    struct emulator_saved_state *ret = NULL;

    if (!(ess = kalloc(sizeof(struct emulator_saved_state))))
        goto out;
    memset(ess, 0, sizeof(*ess));

    ess->ess_arch = arch;
    ess->ess_abi  = abi;

    void *stack = CALL_FUNC(mmap, NULL, EMULATOR_STACK_SIZE, PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
#ifdef TRACE_MMAP
    INFO("mmap %p-%p (emu stack)", stack, (uintptr_t)stack + EMULATOR_STACK_SIZE);
#endif
    if (stack == MAP_FAILED)
        goto out;
    ess->ess_stack = stack;
    ess->ess_stack_size = EMULATOR_STACK_SIZE;

    // point rsp to stack top
    uint64_t stack_top = (uint64_t)ess->ess_stack + ess->ess_stack_size;
    emulator_saved_state_write_reg(ess, ESS_REG_SP, stack_top);
    ret = ess;
out:
    if (!ret && ess)
        emulator_saved_state_destroy(ess);

    return ret;
}

struct emulator_context {
    struct ulinker *ec_ulinker;
    struct emulator_saved_state *ec_ss;
    struct emulator_engine *ec_all_engines;
    struct emulator_engine *ec_curr_engine;
};

struct emulator_context *emulator_context_create(struct ulinker *ulinker)
{
    struct emulator_context *ec = kalloc(sizeof(*ec));
    if (!ec)
        return NULL;

    memset(ec, 0, sizeof(*ec));
    ec->ec_ulinker = ulinker;
    return ec;
}

void emulator_context_destroy(struct emulator_context *ec)
{
    int prevtvm = TVM_OFF;

    while (ec->ec_all_engines) {
        struct emulator_engine *engine = ec->ec_all_engines;
        ec->ec_all_engines = ec->ec_all_engines->ee_next;
        emulator_engine_destroy(engine);
    }
    ec->ec_curr_engine = NULL;

    while (ec->ec_ss) {
        struct emulator_saved_state *ess = ec->ec_ss;
        ec->ec_ss = ec->ec_ss->ess_next;

        emulator_saved_state_destroy(ess);
    }

    TVM_RESTORE(prevtvm);
    kfree(ec);
}

struct emulator_engine *emulator_context_get_or_create_engine(struct emulator_context *ec, int arch)
{
    struct emulator_engine *ee = NULL;
    for (ee = ec->ec_all_engines; ee; ee = ee->ee_next) {
        if (ee->ee_arch == arch && !ee->ee_inuse)
            return ee;
    }

    if (!(ee = emulator_engine_create(ec, arch)))
        return NULL;

    ee->ee_next = ec->ec_all_engines;
    ec->ec_all_engines = ee;

    return ee;
}

#if defined(__linux__)
#define SELF_ABI ULABI_LINUX
#elif defined(__APPLE__)
#define SELF_ABI ULABI_DARWIN
#else
#define SELF_ABI ULABI_NONE
#endif

#if defined(__x86_64__)
#define SELF_ARCH ULARCH_X86_64
#elif defined(__aarch64__)
#define SELF_ARCH ULARCH_ARM64
#else
#define SELF_ARCH ULARCH_NONE
#endif


int emulator_find_dl(struct emulator_context *ec, uint64_t pc, struct ulinker_dladdr_info *out_info)
{
    memset(out_info, 0, sizeof(*out_info));

    // image only: no caller of emulator_find_dl uses the symbol fields
    if (ulinker_dladdr(ec->ec_ulinker, (void *)pc, out_info, DLA_NOSYM)) {
        return 0;
    }

    // NOTE: This is way too slow, cache it or smth
    // Dl_info info;
    // if (dladdr((void *)pc, &info) && info.dli_fbase) {
        out_info->udli_farch = ULARCH_NONE;
        return 0;
    // }

    // return 1;
}

int emulated_pc(struct emulator_context *ec, uint64_t pc)
{
    struct ulinker_dladdr_info dli;
    if (0 != emulator_find_dl(ec, pc, &dli))
        return 0;

    return dli.udli_femu;
}

bool emulator_hook_cb(uc_engine *uc, uc_mem_type type,
                                 uint64_t address, int size, int64_t value,
                                 struct emulator_context *ec)
{
    ec->ec_ss->ess_intr = ESS_INTR_MEM;
    ec->ec_ss->ess_intr_addr = address;
    ec->ec_ss->ess_intr_memtype = type;
    //uc_emu_stop(uc);
    //INFO("emulator_hook_cb: intr ESS_INTR_MEM addr %p memtype %d", (void *)address, type);
    return false;
}

int emulator_saved_state_from_uc(struct emulator_saved_state *ess, uc_engine *uc)
{
    int ret = 1;
    uc_err err;

    if (ess->ess_arch == ULARCH_ARM64) {
        for (int i = 0; i < 29; i++) {
            if ((err = uc_reg_read(uc, UC_ARM64_REG_X0 + i, &ess->ess_context.arm64.x[i]))) {
                INFO("uc_reg_read(UC_ARM64_REG_X%d) failed: %s", i, uc_strerror(err));
                goto out;
            }
        }
        if ((err = uc_reg_read(uc, UC_ARM64_REG_FP, &ess->ess_context.arm64.fp))) {
            INFO("uc_reg_read(UC_ARM64_REG_FP) failed: %s", uc_strerror(err));
            goto out;
        }
        if ((err = uc_reg_read(uc, UC_ARM64_REG_LR, &ess->ess_context.arm64.lr))) {
            INFO("uc_reg_read(UC_ARM64_REG_LR) failed: %s", uc_strerror(err));
            goto out;
        }
        if ((err = uc_reg_read(uc, UC_ARM64_REG_SP, &ess->ess_context.arm64.sp))) {
            INFO("uc_reg_read(UC_ARM64_REG_SP) failed: %s", uc_strerror(err));
            goto out;
        }
        if ((err = uc_reg_read(uc, UC_ARM64_REG_PC, &ess->ess_context.arm64.pc))) {
            INFO("uc_reg_read(UC_ARM64_REG_PC) failed: %s", uc_strerror(err));
            goto out;
        }
    }
    else if (ess->ess_arch == ULARCH_X86_64) {
        if ((err = uc_reg_read(uc, UC_X86_REG_RAX, &ess->ess_context.x86_64.rax))) {
            INFO("uc_reg_read(UC_X86_REG_RAX) failed: %s", uc_strerror(err));
            goto out;
        }
        if ((err = uc_reg_read(uc, UC_X86_REG_RBP, &ess->ess_context.x86_64.rbp))) {
            INFO("uc_reg_read(UC_X86_REG_RBP) failed: %s", uc_strerror(err));
            goto out;
        }
        if ((err = uc_reg_read(uc, UC_X86_REG_RBX, &ess->ess_context.x86_64.rbx))) {
            INFO("uc_reg_read(UC_X86_REG_RBX) failed: %s", uc_strerror(err));
            goto out;
        }
        if ((err = uc_reg_read(uc, UC_X86_REG_RCX, &ess->ess_context.x86_64.rcx))) {
            INFO("uc_reg_read(UC_X86_REG_RCX) failed: %s", uc_strerror(err));
            goto out;
        }
        if ((err = uc_reg_read(uc, UC_X86_REG_RDI, &ess->ess_context.x86_64.rdi))) {
            INFO("uc_reg_read(UC_X86_REG_RDI) failed: %s", uc_strerror(err));
            goto out;
        }
        if ((err = uc_reg_read(uc, UC_X86_REG_RDX, &ess->ess_context.x86_64.rdx))) {
            INFO("uc_reg_read(UC_X86_REG_RDX) failed: %s", uc_strerror(err));
            goto out;
        }
        if ((err = uc_reg_read(uc, UC_X86_REG_RIP, &ess->ess_context.x86_64.rip))) {
            INFO("uc_reg_read(UC_X86_REG_RIP) failed: %s", uc_strerror(err));
            goto out;
        }
        if ((err = uc_reg_read(uc, UC_X86_REG_RSI, &ess->ess_context.x86_64.rsi))) {
            INFO("uc_reg_read(UC_X86_REG_RSI) failed: %s", uc_strerror(err));
            goto out;
        }
        if ((err = uc_reg_read(uc, UC_X86_REG_RSP, &ess->ess_context.x86_64.rsp))) {
            INFO("uc_reg_read(UC_X86_REG_RSP) failed: %s", uc_strerror(err));
            goto out;
        }
        if ((err = uc_reg_read(uc, UC_X86_REG_R8, &ess->ess_context.x86_64.r8))) {
            INFO("uc_reg_read(UC_X86_REG_R8) failed: %s", uc_strerror(err));
            goto out;
        }
        if ((err = uc_reg_read(uc, UC_X86_REG_R9, &ess->ess_context.x86_64.r9))) {
            INFO("uc_reg_read(UC_X86_REG_R9) failed: %s", uc_strerror(err));
            goto out;
        }
        if ((err = uc_reg_read(uc, UC_X86_REG_R10, &ess->ess_context.x86_64.r10))) {
            INFO("uc_reg_read(UC_X86_REG_R10) failed: %s", uc_strerror(err));
            goto out;
        }
        if ((err = uc_reg_read(uc, UC_X86_REG_R11, &ess->ess_context.x86_64.r11))) {
            INFO("uc_reg_read(UC_X86_REG_R11) failed: %s", uc_strerror(err));
            goto out;
        }
        if ((err = uc_reg_read(uc, UC_X86_REG_R12, &ess->ess_context.x86_64.r12))) {
            INFO("uc_reg_read(UC_X86_REG_R12) failed: %s", uc_strerror(err));
            goto out;
        }
        if ((err = uc_reg_read(uc, UC_X86_REG_R13, &ess->ess_context.x86_64.r13))) {
            INFO("uc_reg_read(UC_X86_REG_R13) failed: %s", uc_strerror(err));
            goto out;
        }
        if ((err = uc_reg_read(uc, UC_X86_REG_R14, &ess->ess_context.x86_64.r14))) {
            INFO("uc_reg_read(UC_X86_REG_R14) failed: %s", uc_strerror(err));
            goto out;
        }
        if ((err = uc_reg_read(uc, UC_X86_REG_R15, &ess->ess_context.x86_64.r15))) {
            INFO("uc_reg_read(UC_X86_REG_R15) failed: %s", uc_strerror(err));
            goto out;
        }
    }
    else
        panic("emulator_saved_state_from_uc(arch=%d) unknown arch", ess->ess_arch);
    
    ret = 0;

out:

    return ret;
}

int emulator_saved_state_to_uc(struct emulator_saved_state *ess, uc_engine *uc)
{
    int ret = 1;
    uc_err err;

    if (ess->ess_arch == ULARCH_ARM64) {
        for (int i = 0; i < 29; i++) {
            if ((err = uc_reg_write(uc, UC_ARM64_REG_X0 + i, &ess->ess_context.arm64.x[i]))) {
                INFO("uc_reg_write(UC_ARM64_REG_X%d) failed: %s", i, uc_strerror(err));
                goto out;
            }
        }
        if ((err = uc_reg_write(uc, UC_ARM64_REG_FP, &ess->ess_context.arm64.fp))) {
            INFO("uc_reg_write(UC_ARM64_REG_FP) failed: %s", uc_strerror(err));
            goto out;
        }
        if ((err = uc_reg_write(uc, UC_ARM64_REG_LR, &ess->ess_context.arm64.lr))) {
            INFO("uc_reg_write(UC_ARM64_REG_LR) failed: %s", uc_strerror(err));
            goto out;
        }
        if ((err = uc_reg_write(uc, UC_ARM64_REG_SP, &ess->ess_context.arm64.sp))) {
            INFO("uc_reg_write(UC_ARM64_REG_SP) failed: %s", uc_strerror(err));
            goto out;
        }
        if ((err = uc_reg_write(uc, UC_ARM64_REG_PC, &ess->ess_context.arm64.pc))) {
            INFO("uc_reg_write(UC_ARM64_REG_PC) failed: %s", uc_strerror(err));
            goto out;
        }
    }
    else if (ess->ess_arch == ULARCH_X86_64) {
        if ((err = uc_reg_write(uc, UC_X86_REG_RAX, &ess->ess_context.x86_64.rax))) {
            INFO("uc_reg_write(UC_X86_REG_RAX) failed: %s", uc_strerror(err));
            goto out;
        }
        if ((err = uc_reg_write(uc, UC_X86_REG_RBP, &ess->ess_context.x86_64.rbp))) {
            INFO("uc_reg_write(UC_X86_REG_RBP) failed: %s", uc_strerror(err));
            goto out;
        }
        if ((err = uc_reg_write(uc, UC_X86_REG_RBX, &ess->ess_context.x86_64.rbx))) {
            INFO("uc_reg_write(UC_X86_REG_RBX) failed: %s", uc_strerror(err));
            goto out;
        }
        if ((err = uc_reg_write(uc, UC_X86_REG_RCX, &ess->ess_context.x86_64.rcx))) {
            INFO("uc_reg_write(UC_X86_REG_RCX) failed: %s", uc_strerror(err));
            goto out;
        }
        if ((err = uc_reg_write(uc, UC_X86_REG_RDI, &ess->ess_context.x86_64.rdi))) {
            INFO("uc_reg_write(UC_X86_REG_RDI) failed: %s", uc_strerror(err));
            goto out;
        }
        if ((err = uc_reg_write(uc, UC_X86_REG_RDX, &ess->ess_context.x86_64.rdx))) {
            INFO("uc_reg_write(UC_X86_REG_RDX) failed: %s", uc_strerror(err));
            goto out;
        }
        if ((err = uc_reg_write(uc, UC_X86_REG_RIP, &ess->ess_context.x86_64.rip))) {
            INFO("uc_reg_write(UC_X86_REG_RIP) failed: %s", uc_strerror(err));
            goto out;
        }
        if ((err = uc_reg_write(uc, UC_X86_REG_RSI, &ess->ess_context.x86_64.rsi))) {
            INFO("uc_reg_write(UC_X86_REG_RSI) failed: %s", uc_strerror(err));
            goto out;
        }
        if ((err = uc_reg_write(uc, UC_X86_REG_RSP, &ess->ess_context.x86_64.rsp))) {
            INFO("uc_reg_write(UC_X86_REG_RSP) failed: %s", uc_strerror(err));
            goto out;
        }
        if ((err = uc_reg_write(uc, UC_X86_REG_R8, &ess->ess_context.x86_64.r8))) {
            INFO("uc_reg_write(UC_X86_REG_R8) failed: %s", uc_strerror(err));
            goto out;
        }
        if ((err = uc_reg_write(uc, UC_X86_REG_R9, &ess->ess_context.x86_64.r9))) {
            INFO("uc_reg_write(UC_X86_REG_R9) failed: %s", uc_strerror(err));
            goto out;
        }
        if ((err = uc_reg_write(uc, UC_X86_REG_R10, &ess->ess_context.x86_64.r10))) {
            INFO("uc_reg_write(UC_X86_REG_R10) failed: %s", uc_strerror(err));
            goto out;
        }
        if ((err = uc_reg_write(uc, UC_X86_REG_R11, &ess->ess_context.x86_64.r11))) {
            INFO("uc_reg_write(UC_X86_REG_R11) failed: %s", uc_strerror(err));
            goto out;
        }
        if ((err = uc_reg_write(uc, UC_X86_REG_R12, &ess->ess_context.x86_64.r12))) {
            INFO("uc_reg_write(UC_X86_REG_R12) failed: %s", uc_strerror(err));
            goto out;
        }
        if ((err = uc_reg_write(uc, UC_X86_REG_R13, &ess->ess_context.x86_64.r13))) {
            INFO("uc_reg_write(UC_X86_REG_R13) failed: %s", uc_strerror(err));
            goto out;
        }
        if ((err = uc_reg_write(uc, UC_X86_REG_R14, &ess->ess_context.x86_64.r14))) {
            INFO("uc_reg_write(UC_X86_REG_R14) failed: %s", uc_strerror(err));
            goto out;
        }
        if ((err = uc_reg_write(uc, UC_X86_REG_R15, &ess->ess_context.x86_64.r15))) {
            INFO("uc_reg_write(UC_X86_REG_R15) failed: %s", uc_strerror(err));
            goto out;
        }
    }
    else
        panic("emulator_saved_state_from_uc(arch=%d) unknown arch", ess->ess_arch);
    
    ret = 0;

out:

    return ret;
}

int emulator_step(struct emulator_context *ec)
{
    int ret = 1;
    uc_err err = UC_ERR_ARCH;
    
    uint64_t pc = emulator_saved_state_read_reg(ec->ec_ss, ESS_REG_PC);
 
    if (ec->ec_curr_engine->ee_refresh) {
        // Creating/resetting a Unicorn engine is not async-signal-safe (it
        // allocates host memory).  If a signal arrives mid-allocation, its
        // handler is dispatched through the emulator, which resets the engine
        // again and re-enters the allocator — deadlocking on the malloc lock.
        // Defer signals across the reset.
        tvm_sig_enter();
        int reset_err = emulator_engine_reset(ec->ec_curr_engine);
        if (0 == reset_err)
            ec->ec_curr_engine->ee_refresh = 0;
        tvm_sig_leave();
        if (0 != reset_err) {
            INFO("emulator_step: engine_reset failed (%d)", reset_err);
            goto out;
        }
    }

    // Guard the whole engine-touching span, not just uc_emu_start: to_uc and
    // from_uc mutate the engine's uc registers and ec->ec_ss. If a signal is
    // dispatched here it reenters via emulator_call and, because this engine is
    // not yet marked in-use, get_or_create_engine hands the handler this very
    // engine mid-load — corrupting it so the outer step later faults on garbage
    // (observed near-NULL data fault). tvm_in_emu makes signal_handler defer;
    // ee_inuse is belt-and-suspenders against engine reuse either way.
    tvm_in_emu = 1;
    ec->ec_curr_engine->ee_inuse = 1;

    if (0 != emulator_saved_state_to_uc(ec->ec_ss, ec->ec_curr_engine->ee_uc)) {
        ec->ec_curr_engine->ee_inuse = 0;
        tvm_in_emu = 0;
        goto out;
    }

    ec->ec_ss->ess_intr = ESS_INTR_UNSPEC;

    err = uc_emu_start(ec->ec_curr_engine->ee_uc, pc, 0, 0, 0);

    int from_uc_err = emulator_saved_state_from_uc(ec->ec_ss, ec->ec_curr_engine->ee_uc);

    ec->ec_curr_engine->ee_inuse = 0;
    tvm_in_emu = 0;

    if (0 != from_uc_err)
        goto out;

    // Safe point: the engine is no longer in use and the guest state is saved
    // back into ec->ec_ss. Now it is safe to run any signal handlers that
    // arrived during emulation (deferred by signal_handler's tvm_in_emu check)
    // — they dispatch via a fresh nested emulator_call rather than corrupting
    // this interrupted emulation. Skip if signals are blocked for another
    // reason (they drain at that tvm_sig_leave).
    if (!tvm_sig_blocked)
        tvm_sig_drain();

    pc = emulator_saved_state_read_reg(ec->ec_ss, ESS_REG_PC);
    if (ec->ec_ss->ess_intr == ESS_INTR_UNSPEC) {
        INFO("emulator_step: invalid stop pc=%p err=%d (%s)", (void *)pc, err, uc_strerror(err));
        goto out;
    }

    ret = 0;

out:

    return ret;
}

#define EMULATOR_PARAMS_COUNT 20

struct emulator_call_params {
    uint64_t pc;
    uint64_t arg[EMULATOR_PARAMS_COUNT];
};

void emulator_call_params_from_saved_state(struct emulator_call_params *params, struct emulator_saved_state *ess)
{
    int nonstack_param_count = EMULATOR_PARAMS_COUNT;

    params->pc = emulator_saved_state_read_reg(ess, ESS_REG_PC);
    if (ess->ess_arch == ULARCH_ARM64) {
        nonstack_param_count = 8;
        params->arg[0] = ess->ess_context.arm64.x[0];
        params->arg[1] = ess->ess_context.arm64.x[1];
        params->arg[2] = ess->ess_context.arm64.x[2];
        params->arg[3] = ess->ess_context.arm64.x[3];
        params->arg[4] = ess->ess_context.arm64.x[4];
        params->arg[5] = ess->ess_context.arm64.x[5];
        params->arg[6] = ess->ess_context.arm64.x[6];
        params->arg[7] = ess->ess_context.arm64.x[7];
    }
    else if (ess->ess_arch == ULARCH_X86_64) {
        nonstack_param_count = 6;
        params->arg[0] = ess->ess_context.x86_64.rdi;
        params->arg[1] = ess->ess_context.x86_64.rsi;
        params->arg[2] = ess->ess_context.x86_64.rdx;
        params->arg[3] = ess->ess_context.x86_64.rcx;
        params->arg[4] = ess->ess_context.x86_64.r8;
        params->arg[5] = ess->ess_context.x86_64.r9;
    }
    else
        panic("emulator_call_params_from_saved_state(arch=%d) unknown arch", ess->ess_arch);

    // rest of arguments passed on stack
    uint64_t sp = emulator_saved_state_read_reg(ess, ESS_REG_SP);
    memcpy(&params->arg[nonstack_param_count], (void *)sp, (EMULATOR_PARAMS_COUNT - nonstack_param_count) * 8);
}

void emulator_saved_state_from_call_params(struct emulator_saved_state *ess, struct emulator_call_params *params)
{
    int ret = 1;
    uc_err err = UC_ERR_OK;

    int nonstack_param_count = EMULATOR_PARAMS_COUNT;
    emulator_saved_state_write_reg(ess, ESS_REG_PC, params->pc);

    if (ess->ess_arch == ULARCH_ARM64) {
        nonstack_param_count = 8;
        ess->ess_context.arm64.x[0] = params->arg[0];
        ess->ess_context.arm64.x[1] = params->arg[1];
        ess->ess_context.arm64.x[2] = params->arg[2];
        ess->ess_context.arm64.x[3] = params->arg[3];
        ess->ess_context.arm64.x[4] = params->arg[4];
        ess->ess_context.arm64.x[5] = params->arg[5];
        ess->ess_context.arm64.x[6] = params->arg[6];
        ess->ess_context.arm64.x[7] = params->arg[7];
    }
    else if (ess->ess_arch == ULARCH_X86_64) {
        nonstack_param_count = 6;
        ess->ess_context.x86_64.rdi = params->arg[0];
        ess->ess_context.x86_64.rsi = params->arg[1];
        ess->ess_context.x86_64.rdx = params->arg[2];
        ess->ess_context.x86_64.rcx = params->arg[3];
        ess->ess_context.x86_64.r8  = params->arg[4];
        ess->ess_context.x86_64.r9  = params->arg[5];
    }
    else
        panic("emulator_saved_state_from_call_params(arch=%d) unknown arch", ess->ess_arch);

    // rest of arguments passed on stack
    uint64_t sp = emulator_saved_state_read_reg(ess, ESS_REG_SP);
    sp -= (EMULATOR_PARAMS_COUNT - nonstack_param_count) * 8;
    emulator_saved_state_write_reg(ess, ESS_REG_SP, sp);
    memcpy((void *)sp, &params->arg[nonstack_param_count], (EMULATOR_PARAMS_COUNT - nonstack_param_count) * 8);
}

int emulator_saved_state_enter_call(struct emulator_saved_state *ess, uint64_t lr)
{
    int ret = 1;
    uc_err err;

    if (ess->ess_arch == ULARCH_ARM64) {
        ess->ess_context.arm64.lr = lr;
    }
    else if (ess->ess_arch == ULARCH_X86_64) {
        uint64_t sp = emulator_saved_state_read_reg(ess, ESS_REG_SP);
        sp -= 8;
        emulator_saved_state_write_reg(ess, ESS_REG_SP, sp);
        memcpy((void *)sp, &lr, 8);
    }

    ret = 0;
out:

    return ret;
}

int emulator_saved_state_exit_call(struct emulator_saved_state *ess, uint64_t *out_lr)
{
    int ret = 1;
    uc_err err;

    if (ess->ess_arch == ULARCH_ARM64) {
        *out_lr = ess->ess_context.arm64.lr;
    }
    else if (ess->ess_arch == ULARCH_X86_64) {
        uint64_t sp = emulator_saved_state_read_reg(ess, ESS_REG_SP);
        memcpy(out_lr, (void *)sp, 8);
        sp += 8;
        emulator_saved_state_write_reg(ess, ESS_REG_SP, sp);
    }

    ret = 0;

out:

    return ret;
}

#define EMUFUNC_FLAG 0x0000800000000000ULL
#define EMUFUNC_MARK(_PTR)   ((typeof(_PTR))((uint64_t)_PTR | EMUFUNC_FLAG))
#define EMUFUNC_MARKED(_PTR) ((uint64_t)_PTR & EMUFUNC_FLAG)
#define EMUFUNC_UNMARK(_PTR) ((typeof(_PTR))((uint64_t)_PTR & ~EMUFUNC_FLAG))

int emulator_sigaction(int signum, const struct sigaction *act, struct sigaction *oldact)
{
    // mark new handler
    struct sigaction act2;
    if (act) {
        memcpy(&act2, act, sizeof(*act));
        act = &act2;
        
        void **handler_func = (void **)&act->sa_handler;
        if (act->sa_flags & SA_SIGINFO)
            handler_func = (void **)&act->sa_sigaction;

        if (*handler_func && *handler_func != SIG_DFL && *handler_func != SIG_IGN && emulated_pc(current_thread->thr_emulator, (uint64_t)*handler_func))
            *handler_func = EMUFUNC_MARK(*handler_func);
    }

    int ret = sigaction(signum, act, oldact);

    // unmark old handler
    if (0 == ret && oldact) {
        void **handler_func = (void **)&oldact->sa_handler;
        if (oldact->sa_flags & SA_SIGINFO)
            handler_func = (void **)&oldact->sa_sigaction;

        // its okay to always strip emulator marker even if not emulated pc, its no-op in that case
        if (*handler_func && *handler_func != SIG_DFL && *handler_func != SIG_IGN)
            *handler_func = EMUFUNC_UNMARK(*handler_func);
    }

    return ret;
}

#if defined(__linux__)
sighandler_t emulator_signal(int signum, sighandler_t handler)
#elif defined(__APPLE__)
sig_t emulator_signal(int signum, sig_t handler)
#endif
{
    // mark new handler
    if (handler && handler != SIG_DFL && handler != SIG_IGN && emulated_pc(current_thread->thr_emulator, (uint64_t)handler))
        handler = EMUFUNC_MARK(handler);
    
    handler = signal(signum, handler);

    // unmark old handler (NOTE: must check SIG_ERR)
    // its okay to always strip emulator marker even if not emulated pc, its no-op in that case
    if (handler && handler != SIG_DFL && handler != SIG_IGN && handler != SIG_ERR)
        handler = EMUFUNC_UNMARK(handler);
    
    return handler;
}

int emulator_setjmp(jmp_buf env) {
    uc_err err;
    struct emulator_saved_state *ess = current_thread->thr_emulator->ec_ss;

    if (!ess)
        panic("emulator_setjmp called without emulator saved state");

    uint64_t *envarr = (uint64_t *)env;

    if (ess->ess_arch == ULARCH_X86_64/* && ess->ess_abi == ULABI_LINUX*/) {
        envarr[0] = ess->ess_context.x86_64.rbx;
        envarr[1] = ess->ess_context.x86_64.rbp;
        envarr[2] = ess->ess_context.x86_64.r12;
        envarr[3] = ess->ess_context.x86_64.r13;
        envarr[4] = ess->ess_context.x86_64.r14;
        envarr[5] = ess->ess_context.x86_64.r15;
        envarr[6] = ess->ess_context.x86_64.rsp;
        envarr[7] = ess->ess_fault_lr;
    }
    else if (ess->ess_arch == ULARCH_ARM64/* && ess->ess_abi == ULABI_DARWIN*/)
    {
        envarr[0]  = ess->ess_context.arm64.x[19];
        envarr[1]  = ess->ess_context.arm64.x[20];
        envarr[2]  = ess->ess_context.arm64.x[21];
        envarr[3]  = ess->ess_context.arm64.x[22];
        envarr[4]  = ess->ess_context.arm64.x[23];
        envarr[5]  = ess->ess_context.arm64.x[24];
        envarr[6]  = ess->ess_context.arm64.x[25];
        envarr[7]  = ess->ess_context.arm64.x[26];
        envarr[8]  = ess->ess_context.arm64.x[27];
        envarr[9]  = ess->ess_context.arm64.x[28];
        envarr[10] = ess->ess_context.arm64.fp;
        envarr[11] = ess->ess_context.arm64.lr;
        envarr[12] = ess->ess_context.arm64.sp;
        envarr[13] = ess->ess_fault_lr;
    }
    else
        panic("emulator_setjmp(arch=%d) unknown arch", ess->ess_arch);

    return 0;
}

int emulator_longjmp(jmp_buf env, int val) {
    uc_err err;
    struct emulator_saved_state *ess = current_thread->thr_emulator->ec_ss;

    if (!ess)
        panic("emulator_longjmp called without emulator saved state");

    uint64_t *envarr = (uint64_t *)env;

    if (!val) val = 1;

    if (ess->ess_arch == ULARCH_X86_64/* && ess->ess_abi == ULABI_LINUX*/) {
        ess->ess_context.x86_64.rbx = envarr[0];
        ess->ess_context.x86_64.rbp = envarr[1];
        ess->ess_context.x86_64.r12 = envarr[2];
        ess->ess_context.x86_64.r13 = envarr[3];
        ess->ess_context.x86_64.r14 = envarr[4];
        ess->ess_context.x86_64.r15 = envarr[5];
        ess->ess_context.x86_64.rsp = envarr[6];
        ess->ess_fault_lr           = envarr[7];
    }
    else if (ess->ess_arch == ULARCH_ARM64/* && ess->ess_abi == ULABI_DARWIN*/)
    {
        ess->ess_context.arm64.x[19] = envarr[0];
        ess->ess_context.arm64.x[20] = envarr[1];
        ess->ess_context.arm64.x[21] = envarr[2];
        ess->ess_context.arm64.x[22] = envarr[3];
        ess->ess_context.arm64.x[23] = envarr[4];
        ess->ess_context.arm64.x[24] = envarr[5];
        ess->ess_context.arm64.x[25] = envarr[6];
        ess->ess_context.arm64.x[26] = envarr[7];
        ess->ess_context.arm64.x[27] = envarr[8];
        ess->ess_context.arm64.x[28] = envarr[9];
        ess->ess_context.arm64.fp    = envarr[10];
        ess->ess_context.arm64.lr    = envarr[11];
        ess->ess_context.arm64.sp    = envarr[12];
        ess->ess_fault_lr            = envarr[13];
    }
    else
        panic("emulator_longjmp(arch=%d) unknown arch", ess->ess_arch);

    return val;
}

static const uint64_t EMULATOR_CALL_END_LR = 0x0000DEADBEEF4141ULL;

int emulator_call_ex(struct ulinker_dladdr_info *pcinfo, struct emulator_call_params *params, uint64_t *out_ret)
{
    int prevtvm;
    int r = 1;
    int emu_deferred = 0;  // did we enter the emulated path (and bump tvm_emu_call_depth)?
    struct emulator_saved_state *ess = NULL;
    struct emulator_context *ec = current_thread->thr_emulator;
    uc_err err;

    struct ulinker_dladdr_info local_pcinfo;
    if (!pcinfo) {
        if (0 != emulator_find_dl(ec, params->pc, &local_pcinfo)) {
            INFO("emulator_call_ex: emulator_find_dl(pc=%p) failed", (void *)params->pc);
            return 1;
        }
        pcinfo = &local_pcinfo;
    }

    prevtvm = TVM_OFF;

    // fast path: upcall to host
    if (!pcinfo->udli_femu) {
        TVM_ON;
        uint64_t ret = ((uint64_t (*)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t,
                                  uint64_t, uint64_t, uint64_t, uint64_t, uint64_t,
                                  uint64_t, uint64_t, uint64_t, uint64_t, uint64_t,
                                  uint64_t, uint64_t, uint64_t, uint64_t, uint64_t))params->pc)(
            params->arg[ 0], params->arg[ 1], params->arg[ 2], params->arg[ 3], params->arg[ 4],
            params->arg[ 5], params->arg[ 6], params->arg[ 7], params->arg[ 8], params->arg[ 9],
            params->arg[10], params->arg[11], params->arg[12], params->arg[13], params->arg[14],
            params->arg[15], params->arg[16], params->arg[17], params->arg[18], params->arg[19]
        );
        if (out_ret)
            *out_ret = ret;
        TVM_OFF;
        r = 0;
        goto out;
    }

    // check abi compatibility

    if (pcinfo->udli_farch == ULARCH_NONE || pcinfo->udli_fabi == ULABI_NONE) {
        INFO("emulator_call_ex() failed: unknown guest abi (arch=%d, abi=%d)", pcinfo->udli_farch, pcinfo->udli_fabi);
        goto out;
    }

    if (SELF_ARCH == ULARCH_NONE || SELF_ABI == ULABI_NONE) {
        INFO("emulator_call_ex() failed: unknown host abi");
        goto out;
    }

    int guest_darwin_arm64 = (pcinfo->udli_farch == ULARCH_ARM64) && (pcinfo->udli_fabi == ULABI_DARWIN);
    int host_darwin_arm64  = (SELF_ARCH == ULARCH_ARM64)          && (SELF_ABI == ULABI_DARWIN);

    // the arm64 api for apple is incompatible with arm64 linux, intel linux/apple when it comes to var args.
    // thanks apple.
    if (guest_darwin_arm64 != host_darwin_arm64) {
        INFO("emulator_call_ex() failed: host / guest api incompatibility due to darwin arm64 (guest=%d, host=%d)", guest_darwin_arm64, host_darwin_arm64);
        goto out;
    }

    // this should run in an emulator

    // Enter the emulated path: defer signal dispatch until the emulator_step
    // safe point. Covers engine setup/teardown, the step loop, and the
    // between-steps intr-processing that mutates ec->ec_ss / the engine.
    tvm_emu_call_depth++;
    emu_deferred = 1;

    struct emulator_engine *prev_engine = ec->ec_curr_engine;
    struct emulator_engine *ee = emulator_context_get_or_create_engine(ec, pcinfo->udli_farch);
    if (!ee) {
        INFO("emulator_call_ex: get_or_create_engine(arch=%d) failed", pcinfo->udli_farch);
        goto out;
    }
    ec->ec_curr_engine = ee;

    if (!(ess = emulator_saved_state_create(pcinfo->udli_farch, pcinfo->udli_fabi))) {
        INFO("emulator_call_ex: saved_state_create failed");
        goto out;
    }

    ess->ess_next = ec->ec_ss;
    ec->ec_ss = ess;

    // initialize initial context

    emulator_saved_state_from_call_params(ess, params);
    
    // writes to SP in x86_64
    if (0 != emulator_saved_state_enter_call(ess, EMULATOR_CALL_END_LR))
        goto out;

    while (1) {
        if (0 != emulator_step(ec))
            goto out;

        if (ess->ess_intr == ESS_INTR_MEM) {
            if (ess->ess_intr_memtype != UC_MEM_FETCH_PROT && ess->ess_intr_memtype != UC_MEM_FETCH_UNMAPPED) {
                INFO("ess->ess_intr_memtype %d != UC_MEM_FETCH_PROT/UNMAPPED at %p", ess->ess_intr_memtype, (void *)ess->ess_intr_addr);
                raise(SIGSEGV);
            }

            if (ess->ess_intr_addr == EMULATOR_CALL_END_LR) {
                // INFO("emulator_call_ex ret: %p", (void *)emulator_saved_state_read_reg(ess, ESS_REG_RET));
                // returned
                r = 0;
                if (out_ret)
                    *out_ret = emulator_saved_state_read_reg(ess, ESS_REG_RET);
                goto out;
            }

            // perhaps we need to page in executable memory
            uint64_t fault_range_start = 0;
            uint64_t fault_range_size  = 0;
            struct ulinker_dladdr_info fault_info;
            if (0 == emulator_find_dl(ec, ess->ess_intr_addr, &fault_info)) {
                if (fault_info.udli_farch != ess->ess_arch || !fault_info.udli_femu) {

                    // read lr before call so stack is adjusted for emulator_call_params_from_saved_state()
                    if (0 != emulator_saved_state_exit_call(ess, &ess->ess_fault_lr))
                        goto out;

                    struct emulator_call_params fault_params;
                    emulator_call_params_from_saved_state(&fault_params, ess);

                    // call with parameters and save ret. This is an upcall OUT
                    // of emulation to a host function (or a different-arch
                    // callee); the outer ess/engine are consistent here, so a
                    // signal is safe to dispatch — and must be, or a blocking or
                    // sleeping host call (usleep, read, …) would defer the guest's
                    // own signals until it returns. Drop our deferral for the
                    // duration; the callee re-establishes its own if emulated.
                    uint64_t fault_ret;
                    tvm_emu_call_depth--;
                    int __call_err = emulator_call_ex(&fault_info, &fault_params, &fault_ret);
                    tvm_emu_call_depth++;
                    if (0 != __call_err)
                        goto out;

                    // NOTE! It is possible `fork()` was called!
                    // Update local variables from thread state
                    ec = current_thread->thr_emulator;
                    ee = emulator_context_get_or_create_engine(ec, pcinfo->udli_farch);
                    ess = ec->ec_ss;
                    ec->ec_curr_engine = ee;

                    emulator_saved_state_write_reg(ess, ESS_REG_RET, fault_ret);
                    emulator_saved_state_write_reg(ess, ESS_REG_PC, ess->ess_fault_lr);
                    continue;
                }

                fault_range_start = (uint64_t)fault_info.udli_fbase;
                fault_range_size  = fault_info.udli_fsize;
            }

            if (!fault_range_start || !fault_range_size) {
                INFO("EMULATOR SEGFAULT AT %p", (void *)ess->ess_intr_addr);

                uint64_t segfault_sp = emulator_saved_state_read_reg(ess, ESS_REG_SP);
                INFO("emulator_call_ex segfault sp=%p", (void *)segfault_sp);

                uint64_t ulr = 5;
                uc_mem_read(ec->ec_curr_engine->ee_uc, segfault_sp, &ulr, 8);
                INFO("lr = %p, ulr = %p", *(void **)segfault_sp, (void *)ulr);
                goto out;
            }

            INFO("PAGE IN %p-%p)", (void *)fault_range_start, (void *)(fault_range_start + fault_range_size));
            
            if (0 != emulator_engine_add_map_entry(ec->ec_curr_engine, fault_range_start, fault_range_size))
                goto out;
        }
        else {
            INFO("emulator_call_ex: invalid unexpected intr %d", ess->ess_intr);
        }
    }
    
out:

    if (ess) {
        ec->ec_ss = ess->ess_next;
        emulator_saved_state_destroy(ess);
        ess = NULL;
        ec->ec_curr_engine = prev_engine;
    }

    // Leave the emulated path (after teardown, so the ess-destroy kfree is still
    // covered). Draining is left to the ordinary boundaries; do not drain here.
    if (emu_deferred)
        tvm_emu_call_depth--;

    TVM_RESTORE(prevtvm);

    return r;
}

int emulator_call(uint64_t *out_ret, uint64_t pc, uint64_t arg0, uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5) {
    struct emulator_call_params params = {
        .pc = pc,
        .arg = {arg0, arg1, arg2, arg3, arg4, arg5, 0}
    };

    return emulator_call_ex(NULL, &params, out_ret);
}

void call_sighandler(uint64_t handler_pc, uint64_t arg0, uint64_t arg1, uint64_t arg2) {

    // handler is not in emulator
    if (!EMUFUNC_MARKED(handler_pc)) {
        ((void (*)(uint64_t, uint64_t, uint64_t))handler_pc)(arg0, arg1, arg2);
        return;
    }

    // handler is in emulator, call emulator from signal handler so help me god
    handler_pc = EMUFUNC_UNMARK(handler_pc);
    INFO("sighandler in emulator start");
    emulator_call(NULL, handler_pc, arg0, arg1, arg2, 0, 0, 0);
    INFO("sighandler in emulator end");
}

struct emulator_pthread_create_arg {
    void *(*start_fn)(void *);
    void *arg;
};

void *tvm_emulator_thread_entry(struct emulator_pthread_create_arg *arg) {
    uint64_t ret;
    uintptr_t pc = (uintptr_t)arg->start_fn;
    uintptr_t arg1 = (uintptr_t)arg->arg;

    emulator_call(&ret, pc, arg1, 0, 0, 0, 0, 0);

    return (void *)ret;
}

static void chkstk_nop(void) {}

int emulator_pthread_create(pthread_t *thread, const pthread_attr_t *attr, void *(*start_routine)(void *), void *_arg)
{
    // short-circuit for non-emulated PC addresses
    if (!emulated_pc(current_thread->thr_emulator, (uint64_t)start_routine))
        return pthread_create(thread, attr, start_routine, _arg);

    struct emulator_pthread_create_arg *arg = kalloc(sizeof(*arg));
    if (!arg) {
        return ENOMEM;
    }

    arg->start_fn = start_routine;
    arg->arg = _arg;

    int ret = pthread_create(thread, attr, (void *(*)(void *))tvm_emulator_thread_entry, (void *)arg);

    if (ret) {
        kfree(arg);
    }

    return ret;
}

int emulator_duplicate(struct emulator_context *old, struct emulator_context *new, void (*dup_cb)(void *, void *, void *, size_t), void *arg)
{
    int ret = 1;

    int prevtvm = TVM_OFF;

    // duplicate engines
    struct emulator_engine *new_prev_ee = NULL;
    for (struct emulator_engine *old_ee = old->ec_all_engines; old_ee; old_ee = old_ee->ee_next) {
        struct emulator_engine *new_ee = emulator_engine_create(new, old_ee->ee_arch);
        if (!new_ee)
            goto out;
        
        if (new_prev_ee)
            new_prev_ee->ee_next = new_ee;
        else
            new->ec_all_engines = new_ee;
        new_prev_ee = new_ee;

        if (old->ec_curr_engine == old_ee)
            new->ec_curr_engine = new_ee;
        
        // NOTE: we can just let the emulator re-populate this on its own
        // struct emulator_map_entry *new_prev_eme = NULL;
        // for (struct emulator_map_entry *old_eme = old_ee->ee_eme; old_eme; old_eme = old_eme->eme_next) {
        //     struct emulator_map_entry *new_eme = emulator_map_entry(old_eme->eme_start, old_eme->eme_size);
        //     if (!new_eme)
        //         goto out;
            
        //     if (new_prev_eme)
        //         new_prev_eme->eme_next = new_eme;
        //     else
        //         new_ee->ee_eme = new_eme;
        //     new_prev_eme = new_eme;
        // }

        // new_ee->ee_refresh = 1;
    }

    struct emulator_saved_state *new_prev_ess = NULL;
    for (struct emulator_saved_state *old_ess = old->ec_ss; old_ess; old_ess = old_ess->ess_next) {
        struct emulator_saved_state *new_ess = emulator_saved_state_create(old_ess->ess_arch, old_ess->ess_abi);
        if (!new_ess)
            goto out;
        
        if (new_prev_ess)
            new_prev_ess->ess_next = new_ess;
        else
            new->ec_ss = new_ess;
        new_prev_ess = new_ess;

        new_ess->ess_intr = old_ess->ess_intr;
        new_ess->ess_intr_memtype = old_ess->ess_intr_memtype;
        new_ess->ess_intr_addr = old_ess->ess_intr_addr;
        new_ess->ess_fault_lr = old_ess->ess_fault_lr;

        uint64_t old_sp = emulator_saved_state_read_reg(old_ess, ESS_REG_SP);
        uint64_t new_sp = ((uint64_t)new_ess->ess_stack + (old_sp - (uint64_t)old_ess->ess_stack));
        uint64_t size_sp = (uint64_t)old_ess->ess_stack + old_ess->ess_stack_size - old_sp;
        
        // copy the context (regs)
        memcpy(&new_ess->ess_context, &old_ess->ess_context, sizeof(new_ess->ess_context));

        // copy the stack
        memcpy((void *)new_sp, (void *)old_sp, size_sp);

        if (dup_cb) {
            TVM_ON;
            dup_cb(arg, (void *)&old_ess->ess_context, (void *)&new_ess->ess_context, sizeof(new_ess->ess_context));
            dup_cb(arg, (void *)old_sp, (void *)new_sp, size_sp);
            dup_cb(arg, (void *)&old_ess->ess_fault_lr, (void *)&new_ess->ess_fault_lr, sizeof(new_ess->ess_fault_lr));
            dup_cb(arg, (void *)&old_ess->ess_intr_addr, (void *)&new_ess->ess_intr_addr, sizeof(new_ess->ess_intr_addr));
            TVM_OFF;
        }
    }

    ret = 0;

out:

    TVM_RESTORE(prevtvm);

    return ret;
}

/////////////
// MARK: Hooks API
/////////////

struct tvm_call {
    struct tvm_call *prev;
    void *lr;
};

// Defined per-platform below; _phdr_callback (Linux) takes its address to
// locate the executable text segment, so it needs the forward declaration.
void get_text(uintptr_t *out_start, uintptr_t *out_end);

#if defined(__linux__)

static int _phdr_callback(struct dl_phdr_info *info, size_t size, void *data)
{
    uintptr_t *out = data;
    uintptr_t addr = (uintptr_t)get_text;
    for (int i = 0; i < info->dlpi_phnum; i++) {
        if (info->dlpi_phdr[i].p_type != PT_LOAD ||
            !(info->dlpi_phdr[i].p_flags & PF_X))
            continue;
        uintptr_t seg_start = info->dlpi_addr + info->dlpi_phdr[i].p_vaddr;
        uintptr_t seg_end = seg_start + info->dlpi_phdr[i].p_memsz;
        if (addr >= seg_start && addr < seg_end) {
            out[0] = seg_start;
            out[1] = seg_end;
            return 1;
        }
    }
    return 0;
}

void get_text(uintptr_t *out_start, uintptr_t *out_end)
{
    // The executable segment containing get_text() is the main binary's .text —
    // fixed for the process lifetime — so cache it (as the __APPLE__ path does).
    // Beyond avoiding a dl_iterate_phdr on every malloc/free/can_enter_tvm, this
    // is a correctness fix: glibc thread exit runs __libc_thread_freeres, which
    // frees TLS-internal buffers via free() -> can_enter_tvm -> get_text. Calling
    // dl_iterate_phdr there re-resolves TLS (_dl_tls_get_addr_soft) on a thread
    // whose TLS is being torn down -> SIGSEGV. can_enter_tvm short-circuits on
    // current_thread for normal calls, so get_text is often first *reached* on
    // exactly that thread-exit path — hence the eager constructor below warms
    // the cache on the main thread before any worker thread can exit. A benign
    // first-call race just recomputes the same value.
    static uintptr_t cached_start = 0;
    static uintptr_t cached_end = 0;
    if (!cached_start) {
        uintptr_t out[2] = {0, 0};
        dl_iterate_phdr(_phdr_callback, out);
        cached_start = out[0];
        cached_end = out[1];
    }
    *out_start = cached_start;
    *out_end = cached_end;
}

// Warm get_text()'s cache before main(), on the main thread — so the interposed
// free() on any later thread-exit path hits the cache and never calls
// dl_iterate_phdr during that thread's TLS teardown. dl_iterate_phdr is safe in
// a constructor (the link map is complete post-relocation).
__attribute__((constructor))
static void _warm_get_text_cache(void)
{
    uintptr_t s, e;
    get_text(&s, &e);
}

#elif defined(__APPLE__)

static uintptr_t text_start = 0;
static size_t text_size = 0;

static const struct mach_header_64 *current_header(void)
{
    Dl_info info;
    if (dladdr((void *)&current_header, &info)) {
        return (const struct mach_header_64 *)info.dli_fbase;
    }
    return NULL;
}

void get_text(uintptr_t *out_start, uintptr_t *out_end)
{
    if (!text_start) {
        unsigned long size;
        void *text = getsectiondata(
            current_header(),
            "__TEXT",
            "__text",
            &size
        );

        text_start = (uintptr_t)text;
        text_size = (size_t)size;
    }

    *out_start = text_start;
    *out_end = text_start + text_size;
}

#endif

struct ulinker_loaded_range {
    struct list_head ulr_list;
    struct ulinker_load_event ulr_event;
};

static LIST_HEAD(all_ulinker_loaded_ranges);
static pthread_mutex_t all_ulinker_loaded_ranges_lock = PTHREAD_MUTEX_INITIALIZER;

void add_ulinker_loaded_range(struct ulinker_load_event *ule) {
    struct ulinker_loaded_range *ulr = kalloc(sizeof(*ulr));
    if (!ulr)
        panic("add_ulinker_loaded_range");

    memcpy(&ulr->ulr_event, ule, sizeof(*ule));

    pthread_mutex_lock(&all_ulinker_loaded_ranges_lock);
    list_add(&ulr->ulr_list, &all_ulinker_loaded_ranges);
    pthread_mutex_unlock(&all_ulinker_loaded_ranges_lock);
}

void remove_ulinker_loaded_range(struct ulinker_load_event *ule) {
    struct ulinker_loaded_range *ulr;
    pthread_mutex_lock(&all_ulinker_loaded_ranges_lock);
    list_for_each_entry(ulr, &all_ulinker_loaded_ranges, ulr_list) {
        if (0 == memcmp(&ulr->ulr_event, ule, sizeof(*ule))) {
            list_del(&ulr->ulr_list);
            kfree(ulr);
            break;
        }
    }
    pthread_mutex_unlock(&all_ulinker_loaded_ranges_lock);
}

struct ulinker_loaded_range *find_containing_ulinker_loaded_range_locked(void *addr) {
    struct ulinker_loaded_range *ulr;
    list_for_each_entry(ulr, &all_ulinker_loaded_ranges, ulr_list) {
        if ((uintptr_t)addr >= (uintptr_t)ulr->ulr_event.ule_base && (uintptr_t)addr < ((uintptr_t)ulr->ulr_event.ule_base) + ulr->ulr_event.ule_size) {
            return ulr;
        }
    }

    return NULL;
}

int in_tvm_range(void *lr) {
    uintptr_t text_start;
    uintptr_t text_end;
    get_text(&text_start, &text_end);

    if ((uintptr_t)lr >= text_start && (uintptr_t)lr < text_end)
        return 1;

    // Defer guest signal dispatch while holding the ranges lock. This runs in
    // every interposer prologue, i.e. OUTSIDE the wrapper's own tvm_sig_enter
    // section, so a signal landing here used to run the guest handler
    // synchronously on top of the held mutex; the handler's first syscall
    // (dropbear's SIGCHLD handler writes to its self-pipe) re-entered
    // can_enter_tvm -> in_tvm_range and self-deadlocked on this non-recursive
    // lock, leaving the task hung forever (seen as random 30s integration
    // timeouts: session never closes, comsub never returns). tvm_sig_leave
    // drains the deferred signal right after the unlock.
    tvm_sig_enter();
    pthread_mutex_lock(&all_ulinker_loaded_ranges_lock);
    int r = (NULL != find_containing_ulinker_loaded_range_locked(lr));
    pthread_mutex_unlock(&all_ulinker_loaded_ranges_lock);
    tvm_sig_leave();

    return r;
}

int can_enter_tvm(void *lr) {
    // A thread without per-thread TLS (newborn inside pthread_create, dying
    // after the key destructor, TVM-internal helpers) never enters TVM.
    // This must come first.
    //
    // The TLS block is looked up once and its fields read directly: this runs
    // on every interposed call, and each use of the `call` /
    // `thread_tvm_disallow` macros was a separate pthread_getspecific() --
    // together with the wrapper's own lookups the hottest thing in a bash
    // loop, ahead of the allocator.
    struct tvm_tls *t = TVM_TLS_PEEK();
    if (!t)
        return 0;

    // not initialized
    if (!main_task)
        return 0;

    if (t->tls_thread_tvm_disallow) {
        return 0;
    }

    struct tvm_call *c = t->tls_call;
    if (!lr) {
        if (!c)
            return 0;
        lr = c->lr;
    }


    if (!(in_tvm_range(lr)))
        return 0;

    // fork() does not use call (for now)
    if (!c)
        return 1;

    // if this isn't an inner call, just go
    if (!c->prev)
        return 1;

    // if this is an inner call, check first call
    struct tvm_call *call_itr = c;
    while (call_itr) {
        lr = call_itr->lr;
        call_itr = call_itr->prev;
    }

    return in_tvm_range(lr);
}

struct function_metadata {
    struct list_head fm_list;
    const char *fm_name;
    void *fm_ptr;
};

static LIST_HEAD(all_function_metadata);

#define FUNCTION_METADATA(_name, _ptr) \
static struct function_metadata _fm_##_name = { .fm_name = #_name, .fm_ptr = (void *)_ptr }; \
__attribute__((constructor)) \
static void _fmc_ ## _name () { list_add_tail( & (_fm_ ## _name).fm_list, &all_function_metadata); }



#define __MAP0(m,...)
#define __MAP1(m,t,a,...) m(t,a)
#define __MAP2(m,t,a,...) m(t,a), __MAP1(m,__VA_ARGS__)
#define __MAP3(m,t,a,...) m(t,a), __MAP2(m,__VA_ARGS__)
#define __MAP4(m,t,a,...) m(t,a), __MAP3(m,__VA_ARGS__)
#define __MAP5(m,t,a,...) m(t,a), __MAP4(m,__VA_ARGS__)
#define __MAP6(m,t,a,...) m(t,a), __MAP5(m,__VA_ARGS__)
#define __MAP(n,...) __MAP##n(__VA_ARGS__)

#define __LAST1(m,t,a,...) m(t,a)
#define __LAST2(m,t,a,...) __LAST1(m,__VA_ARGS__)
#define __LAST3(m,t,a,...) __LAST2(m,__VA_ARGS__)
#define __LAST4(m,t,a,...) __LAST3(m,__VA_ARGS__)
#define __LAST5(m,t,a,...) __LAST4(m,__VA_ARGS__)
#define __LAST6(m,t,a,...) __LAST5(m,__VA_ARGS__)
#define __LAST(n,...) __LAST##n(__VA_ARGS__)

#define __FN_DECL(t, a)	t a
#define __FN_ARGS(t, a)	a

// VM-enter chokepoint: a thread with no per-thread TLS is not a VM thread —
// route it straight to the host function and touch NOTHING of TVM's state
// (in particular the tls_call chain: glibc calls interposed functions on
// newborn threads inside pthread_create and on dying threads after the key
// destructor, and pushing onto shared state from there is a data race).
#define __FUNCTION_DEFINEx(_x, _rettype, _name, ...) \
_rettype __tvm_##_name(__MAP(_x, __FN_DECL, __VA_ARGS__)); \
_rettype _name(__MAP(_x, __FN_DECL, __VA_ARGS__)) { \
    struct tvm_tls *__tls = TVM_TLS_PEEK(); \
    if (!__tls) \
        return CALL_FUNC(_name, __MAP(_x, __FN_ARGS, __VA_ARGS__)); \
    struct tvm_call __call = { __tls->tls_call, __builtin_return_address(0) }; \
    __tls->tls_call = &__call; \
    _rettype __r; \
    if (!can_enter_tvm(NULL)) { __r = CALL_FUNC(_name, __MAP(_x, __FN_ARGS, __VA_ARGS__)); } \
    else { __r = __tvm_##_name (__MAP(_x, __FN_ARGS, __VA_ARGS__)); } \
    __tls->tls_call = __call.prev; \
    return __r; \
} \
FUNCTION_METADATA(_name, &_name); \
_rettype __tvm_##_name(__MAP(_x, __FN_DECL, __VA_ARGS__))

#define __FUNCTION_DEFINE_VOIDx(_x, _name, ...) \
void __tvm_##_name(__MAP(_x, __FN_DECL, __VA_ARGS__)); \
void _name(__MAP(_x, __FN_DECL, __VA_ARGS__)) { \
    struct tvm_tls *__tls = TVM_TLS_PEEK(); \
    if (!__tls) { \
        CALL_FUNC(_name, __MAP(_x, __FN_ARGS, __VA_ARGS__)); \
        return; \
    } \
    struct tvm_call __call = { __tls->tls_call, __builtin_return_address(0) }; \
    __tls->tls_call = &__call; \
    if (!can_enter_tvm(NULL)) { CALL_FUNC(_name, __MAP(_x, __FN_ARGS, __VA_ARGS__)); } \
    else { __tvm_##_name (__MAP(_x, __FN_ARGS, __VA_ARGS__)); } \
    __tls->tls_call = __call.prev; \
} \
FUNCTION_METADATA(_name, &_name); \
void __tvm_##_name(__MAP(_x, __FN_DECL, __VA_ARGS__))

#define __FUNCTION_DEFINE_NORETx(_x, _name, ...) \
void __tvm_##_name(__MAP(_x, __FN_DECL, __VA_ARGS__)); \
void _name(__MAP(_x, __FN_DECL, __VA_ARGS__)) { \
    struct tvm_tls *__tls = TVM_TLS_PEEK(); \
    if (!__tls) { \
        CALL_FUNC(_name, __MAP(_x, __FN_ARGS, __VA_ARGS__)); \
        CALL_FUNC(abort); \
        __builtin_unreachable(); \
    } \
    struct tvm_call __call = { __tls->tls_call, __builtin_return_address(0) }; \
    __tls->tls_call = &__call; \
    if (!can_enter_tvm(NULL)) { CALL_FUNC(_name, __MAP(_x, __FN_ARGS, __VA_ARGS__)); } \
    else {__tvm_##_name (__MAP(_x, __FN_ARGS, __VA_ARGS__)); } \
    panic("returned"); \
    __builtin_unreachable(); \
} \
FUNCTION_METADATA(_name, &_name); \
void __tvm_##_name(__MAP(_x, __FN_DECL, __VA_ARGS__))

#define __FUNCTION_DEFINE_VAx(_x, _rettype, _name, ...) \
_rettype __tvm_##_name##_va(__MAP(_x, __FN_DECL, __VA_ARGS__), va_list __ap); \
_rettype _name(__MAP(_x, __FN_DECL, __VA_ARGS__), ...) { \
    struct tvm_tls *__tls = TVM_TLS_PEEK(); \
    struct tvm_call __call = { __tls ? __tls->tls_call : NULL, __builtin_return_address(0) }; \
    if (__tls) \
        __tls->tls_call = &__call; \
    va_list __ap; \
    va_start(__ap, __LAST(_x, __FN_ARGS, __VA_ARGS__)); \
    _rettype __r = __tvm_##_name##_va (__MAP(_x, __FN_ARGS, __VA_ARGS__), __ap); \
    va_end(__ap); \
    if (__tls) \
        __tls->tls_call = __call.prev; \
    return __r; \
} \
FUNCTION_METADATA(_name, &_name); \
_rettype __tvm_##_name##_va(__MAP(_x, __FN_DECL, __VA_ARGS__), va_list __ap)

#define __FUNCTION_DEFINE_VOID_VAx(_x, _name, ...) \
void __tvm_##_name##_va(__MAP(_x, __FN_DECL, __VA_ARGS__), va_list __ap); \
void _name(__MAP(_x, __FN_DECL, __VA_ARGS__), ...) { \
    struct tvm_tls *__tls = TVM_TLS_PEEK(); \
    struct tvm_call __call = { __tls ? __tls->tls_call : NULL, __builtin_return_address(0) }; \
    if (__tls) \
        __tls->tls_call = &__call; \
    va_list __ap; \
    va_start(__ap, __LAST(_x, __FN_ARGS, __VA_ARGS__)); \
    __tvm_##_name##_va (__MAP(_x, __FN_ARGS, __VA_ARGS__), __ap); \
    va_end(__ap); \
    if (__tls) \
        __tls->tls_call = __call.prev; \
} \
FUNCTION_METADATA(_name, &_name); \
void __tvm_##_name##_va(__MAP(_x, __FN_DECL, __VA_ARGS__), va_list __ap)

#define __FUNCTION_DEFINE_NORET_VAx(_x, _name, ...) \
void __tvm_##_name##_va(__MAP(_x, __FN_DECL, __VA_ARGS__), va_list __ap); \
void _name(__MAP(_x, __FN_DECL, __VA_ARGS__), ...) { \
    struct tvm_tls *__tls = TVM_TLS_PEEK(); \
    struct tvm_call __call = { __tls ? __tls->tls_call : NULL, __builtin_return_address(0) }; \
    if (__tls) \
        __tls->tls_call = &__call; \
    va_list __ap; \
    va_start(__ap, __LAST(_x, __FN_ARGS, __VA_ARGS__)); \
    __tvm_##_name##_va (__MAP(_x, __FN_ARGS, __VA_ARGS__), __ap); \
    va_end(__ap); \
    panic("returned"); \
    __builtin_unreachable(); \
} \
FUNCTION_METADATA(_name, &_name); \
void __tvm_##_name##_va(__MAP(_x, __FN_DECL, __VA_ARGS__), va_list __ap)

#define FUNCTION_DEFINE0(_rettype, _name)      __FUNCTION_DEFINEx(0, _rettype, _name)
#define FUNCTION_DEFINE1(_rettype, _name, ...) __FUNCTION_DEFINEx(1, _rettype, _name, __VA_ARGS__)
#define FUNCTION_DEFINE2(_rettype, _name, ...) __FUNCTION_DEFINEx(2, _rettype, _name, __VA_ARGS__)
#define FUNCTION_DEFINE3(_rettype, _name, ...) __FUNCTION_DEFINEx(3, _rettype, _name, __VA_ARGS__)
#define FUNCTION_DEFINE4(_rettype, _name, ...) __FUNCTION_DEFINEx(4, _rettype, _name, __VA_ARGS__)
#define FUNCTION_DEFINE5(_rettype, _name, ...) __FUNCTION_DEFINEx(5, _rettype, _name, __VA_ARGS__)
#define FUNCTION_DEFINE6(_rettype, _name, ...) __FUNCTION_DEFINEx(6, _rettype, _name, __VA_ARGS__)

#define FUNCTION_DEFINE_VOID0(_name)      __FUNCTION_DEFINE_VOIDx(0, _name)
#define FUNCTION_DEFINE_VOID1(_name, ...) __FUNCTION_DEFINE_VOIDx(1, _name, __VA_ARGS__)
#define FUNCTION_DEFINE_VOID2(_name, ...) __FUNCTION_DEFINE_VOIDx(2, _name, __VA_ARGS__)
#define FUNCTION_DEFINE_VOID3(_name, ...) __FUNCTION_DEFINE_VOIDx(3, _name, __VA_ARGS__)
#define FUNCTION_DEFINE_VOID4(_name, ...) __FUNCTION_DEFINE_VOIDx(4, _name, __VA_ARGS__)
#define FUNCTION_DEFINE_VOID5(_name, ...) __FUNCTION_DEFINE_VOIDx(5, _name, __VA_ARGS__)
#define FUNCTION_DEFINE_VOID6(_name, ...) __FUNCTION_DEFINE_VOIDx(6, _name, __VA_ARGS__)

#define FUNCTION_DEFINE_NORET0(_name)      __FUNCTION_DEFINE_NORETx(0, _name)
#define FUNCTION_DEFINE_NORET1(_name, ...) __FUNCTION_DEFINE_NORETx(1, _name, __VA_ARGS__)
#define FUNCTION_DEFINE_NORET2(_name, ...) __FUNCTION_DEFINE_NORETx(2, _name, __VA_ARGS__)
#define FUNCTION_DEFINE_NORET3(_name, ...) __FUNCTION_DEFINE_NORETx(3, _name, __VA_ARGS__)
#define FUNCTION_DEFINE_NORET4(_name, ...) __FUNCTION_DEFINE_NORETx(4, _name, __VA_ARGS__)
#define FUNCTION_DEFINE_NORET5(_name, ...) __FUNCTION_DEFINE_NORETx(5, _name, __VA_ARGS__)
#define FUNCTION_DEFINE_NORET6(_name, ...) __FUNCTION_DEFINE_NORETx(6, _name, __VA_ARGS__)

#define FUNCTION_DEFINE_VA1(_rettype, _name, ...) __FUNCTION_DEFINE_VAx(1, _rettype, _name, __VA_ARGS__)
#define FUNCTION_DEFINE_VA2(_rettype, _name, ...) __FUNCTION_DEFINE_VAx(2, _rettype, _name, __VA_ARGS__)
#define FUNCTION_DEFINE_VA3(_rettype, _name, ...) __FUNCTION_DEFINE_VAx(3, _rettype, _name, __VA_ARGS__)
#define FUNCTION_DEFINE_VA4(_rettype, _name, ...) __FUNCTION_DEFINE_VAx(4, _rettype, _name, __VA_ARGS__)
#define FUNCTION_DEFINE_VA5(_rettype, _name, ...) __FUNCTION_DEFINE_VAx(5, _rettype, _name, __VA_ARGS__)
#define FUNCTION_DEFINE_VA6(_rettype, _name, ...) __FUNCTION_DEFINE_VAx(6, _rettype, _name, __VA_ARGS__)

#define FUNCTION_DEFINE_VOID_VA1(_name, ...) __FUNCTION_DEFINE_VOID_VAx(1, _name, __VA_ARGS__)
#define FUNCTION_DEFINE_VOID_VA2(_name, ...) __FUNCTION_DEFINE_VOID_VAx(2, _name, __VA_ARGS__)
#define FUNCTION_DEFINE_VOID_VA3(_name, ...) __FUNCTION_DEFINE_VOID_VAx(3, _name, __VA_ARGS__)

#define FUNCTION_DEFINE_NORET_VA1(_name, ...) __FUNCTION_DEFINE_NORET_VAx(1, _name, __VA_ARGS__)
#define FUNCTION_DEFINE_NORET_VA2(_name, ...) __FUNCTION_DEFINE_NORET_VAx(2, _name, __VA_ARGS__)
#define FUNCTION_DEFINE_NORET_VA3(_name, ...) __FUNCTION_DEFINE_NORET_VAx(3, _name, __VA_ARGS__)
#define FUNCTION_DEFINE_NORET_VA4(_name, ...) __FUNCTION_DEFINE_NORET_VAx(4, _name, __VA_ARGS__)
#define FUNCTION_DEFINE_NORET_VA5(_name, ...) __FUNCTION_DEFINE_NORET_VAx(5, _name, __VA_ARGS__)
#define FUNCTION_DEFINE_NORET_VA6(_name, ...) __FUNCTION_DEFINE_NORET_VAx(6, _name, __VA_ARGS__)

/////////////
// MARK: TVM builtins
/////////////

#define CCAL_LEFT  0
#define CCAL_RIGHT 1
struct cli_column {
    const char *cc_title;
    int cc_align; // CCAL_*
    int cc_verbose;
};

struct cli_table {
    struct { 
        struct cli_column desc;
        int currw;
    }     *ct_cols;
    size_t ct_coln;
    char **ct_rows;
    size_t ct_rown;
};

#define CT_CELL(Tbl, Row, Col) ((Tbl)->ct_rows[(Tbl)->ct_coln * (Row) + (Col)])
#define CT_COL_FOR_CELL(Tbl, Cell) (&((Tbl)->ct_cols[((Cell - (Tbl)->ct_rows) % (Tbl)->ct_coln)]))
#define CT_ROW_FOR_EACH(Head, Tbl, Row) for ( \
    (Head) = (Row); \
    ({ \
        if ((Head) > (Row) && (Head)[-1]) { \
            size_t __CELLN = strlen((Head)[-1]); \
            if (__CELLN > CT_COL_FOR_CELL((Tbl), ((Head)-1))->currw) \
                CT_COL_FOR_CELL((Tbl), ((Head)-1))->currw = __CELLN; \
        }; \
        (Head); \
    }) < ((Row) + (Tbl)->ct_coln); \
    (Head)++  )
#define CT_IS_CELL(Tbl, Cell, ColNameToCheck) (0 == strcmp(ColNameToCheck, CT_COL_FOR_CELL((Tbl), (Cell))->desc.cc_title))

static void cli_table_dealloc(struct cli_table *tbl)
{
    if (tbl->ct_rows) {
        for (int i = 0; i < (tbl->ct_coln * tbl->ct_rown); i++)
            free(tbl->ct_rows[i]);
        free(tbl->ct_rows);
    }

    if (tbl->ct_cols)
        free(tbl->ct_cols);
}

static int cli_table_alloc(struct cli_column *cols, size_t coln, struct cli_table *out_tbl)
{
    int ret = -1;
    struct cli_table tbl = { .ct_coln = coln , .ct_rown = 1 };

    tbl.ct_cols = calloc(tbl.ct_coln, sizeof(*tbl.ct_cols));
    if (!tbl.ct_cols)
        goto err;

    tbl.ct_rows = calloc(1 * tbl.ct_coln, sizeof(const char *));
    if (!tbl.ct_rows)
        goto err;
    
    // init columns and header row
    for (int i = 0; i < tbl.ct_coln; i++) {
        memcpy(&tbl.ct_cols[i].desc, cols + i, sizeof(*cols));
        tbl.ct_cols[i].currw = strlen(tbl.ct_cols[i].desc.cc_title); 
        if (!(CT_CELL(&tbl, 0, i) = strdup(tbl.ct_cols[i].desc.cc_title)))
            goto err;
    }

    memcpy(out_tbl, &tbl, sizeof(tbl));
    ret = 0;
err:
    if (ret)
        cli_table_dealloc(&tbl);
    
    return ret;
}

static char **cli_table_new_row(struct cli_table *tbl) {
    char **rowp = NULL;

    if (!(tbl->ct_rows = realloc(tbl->ct_rows, (tbl->ct_rown + 1) * tbl->ct_coln * sizeof(const char *))))
        goto err;
    
    memset(tbl->ct_rows + (tbl->ct_rown * tbl->ct_coln), 0, tbl->ct_coln * sizeof(const char *));

    rowp = &CT_CELL(tbl, tbl->ct_rown, 0);
    ++tbl->ct_rown;
err:
    return rowp;
}

static int cli_table_print(struct cli_table *tbl, int verbose) {
    int ret = -1;

    // print all rows
    for (int r = 0; r < tbl->ct_rown; r++) {
        printf("  ");
        int first = 1;
        for (int c = 0; c < tbl->ct_coln; c++) {
            if (tbl->ct_cols[c].desc.cc_verbose && !verbose)
                continue;
            
            if (!first)
                putchar(' ');
            first = 0;
            
            char *cell = CT_CELL(tbl, r, c);
            int celln = (cell ? strlen(cell) : 0);

            if (tbl->ct_cols[c].desc.cc_align == CCAL_RIGHT)
                for (int i = celln; i < tbl->ct_cols[c].currw; i++)
                    putchar(' ');

            if (cell) {
                size_t towrite = ((celln > tbl->ct_cols[c].currw) ? tbl->ct_cols[c].currw : celln);
                if (towrite != fwrite(cell, 1, towrite, stdout))
                    goto err;
            }
            
            if (tbl->ct_cols[c].desc.cc_align == CCAL_LEFT)
                for (int i = celln; i < tbl->ct_cols[c].currw; i++)
                    putchar(' ');
        }
        putchar('\n');
    }

    ret = 0;
err:
    return ret;
}

static int tvm_ps(int argc, char **argv)
{
    int ret = 1;
    struct task *t = NULL;
    pthread_mutex_lock(&tasks_lock);

    struct cli_column cols[] = {
        { .cc_title="PID",  .cc_align=CCAL_RIGHT },
        { .cc_title="PPID", .cc_align=CCAL_RIGHT },
        { .cc_title="SID",  .cc_align=CCAL_RIGHT, .cc_verbose=1 },
        { .cc_title="PGID", .cc_align=CCAL_RIGHT, .cc_verbose=1 },
        { .cc_title="TTY",  .cc_verbose=1 },
        { .cc_title="CMD" },
    };
    struct cli_table tbl = {0};
    if (cli_table_alloc(cols, sizeof(cols)/sizeof(cols[0]), &tbl))
        goto err;

    // iterate tasks
    t = main_task;
    do {
        task_lock(t);

        char **rowp = cli_table_new_row(&tbl);
        if (!rowp)
            goto err;

        char **cellp;
        CT_ROW_FOR_EACH(cellp, &tbl, rowp) {
            if (CT_IS_CELL(&tbl, cellp, "PID")) {
                if (-1 == asprintf(cellp, "%d", t->tsk_pid))
                    goto err;
            }

            else if (CT_IS_CELL(&tbl, cellp, "PPID")) {
                if (-1 == asprintf(cellp, "%d", (t->tsk_parent ? t->tsk_parent->tsk_pid : 1)))
                    goto err;
            }

            else if (CT_IS_CELL(&tbl, cellp, "SID")) {
                if (-1 == asprintf(cellp, "%d", t->tsk_sid))
                    goto err;
            }

            else if (CT_IS_CELL(&tbl, cellp, "PGID")) {
                if (-1 == asprintf(cellp, "%d", t->tsk_pgid))
                    goto err;
            }

            else if (CT_IS_CELL(&tbl, cellp, "TTY")) {
                if (-1 == asprintf(cellp, "?")) // TODO
                    goto err;
            }

            else if (CT_IS_CELL(&tbl, cellp, "CMD")) {
                if (t->tsk_state & TS_ZOMBIE) {
                    int commlen = strnlen(t->tsk_comm, sizeof(t->tsk_comm));
                    if (!(*cellp = malloc(commlen + 3)))
                        goto err;
                    memcpy((*cellp)+1, t->tsk_comm, commlen);
                    (*cellp)[0] = '[';
                    (*cellp)[commlen + 1] = ']';
                    (*cellp)[commlen + 2] = 0;
                }
                else {
                    if (!(*cellp = strndup(t->tsk_comm, sizeof(t->tsk_comm))))
                        goto err;
                }
            }
        }
        
        task_unlock(t);
    }
    while ( ({ t = list_next_entry(t, tsk_list); !list_entry_is_head(t, &main_task->tsk_list, tsk_list); }) );

    t = NULL;

    if (0 != cli_table_print(&tbl, argc > 1))
        goto err;

    ret = 0;
err:
    if (ret)
        perror("tvm: ps");
    if (t)
        task_unlock(t);
    cli_table_dealloc(&tbl);
    pthread_mutex_unlock(&tasks_lock);
    return ret;
}

enum {
    LSOFT_UNKNOWN,
    LSOFT_PROC,
    LSOFT_PTY,
};

static int tvm_lsof(int argc, char **argv)
{
    int ret = 1;
    struct fdentry *fdt = NULL;
    size_t fdn = 0;
    struct {
        int rfd;
        int type;
        union {
            struct {
                int nfd;
                char *fdtype;
                int pid;
                char comm[16+1];
            } proc;
            struct {
                int mfd;
                pid_t sid;
                char *end;
            } tty;
        };
    } *fdr = NULL;

    int verbose = argc > 1;

    if (read_fdtable(&fdt, &fdn))
        goto err;
    
    if (!(fdr = calloc(sizeof(*fdr), fdn)))
        goto err;
    
    for (int i = 0; i < fdn; i++) {
        fdr[i].rfd = fdt[i].fd;
        fdr[i].type = LSOFT_UNKNOWN;

        int nfd;
        struct task *t;
        if ((t = task_for_rfd(fdr[i].rfd, &nfd))) {
            fdr[i].type = LSOFT_PROC;
            fdr[i].proc.nfd = nfd;
            fdr[i].proc.fdtype = "unknown_type";
            fdr[i].proc.pid = t->tsk_pid;
            memcpy(fdr[i].proc.comm, t->tsk_comm, sizeof(t->tsk_comm));
            fdr[i].proc.comm[sizeof(t->tsk_comm)] = 0;
            
            struct file *file;
            VERIFY(NULL != (file = file_for_task_locked(t, nfd)));
            task_unlock(t);

            switch (file->f_type) {
            case DEV_TTY:
                fdr[i].proc.fdtype = "tty";
                break;
            }
            
            
            file_unlock(file, 1);
            continue;
        }
        
        int ttymode;
        struct tty *tt;
        if ((tt = tty_for_rfd(fdr[i].rfd, &ttymode))) {
            if (ttymode == TTM_MASTER) {
                fdr[i].type = LSOFT_PTY;
                fdr[i].tty.mfd = tt->t_mfd;
                fdr[i].tty.sid = tt->t_sid;
                fdr[i].tty.end = "master";
                tty_unlock(tt, 1);
                continue;
            }

            VERIFY(ttymode == TTM_SLAVE);

            fdr[i].type = LSOFT_PTY;
            fdr[i].tty.mfd = tt->t_mfd;
            fdr[i].tty.sid = tt->t_pgid;
            fdr[i].tty.end = "slave";
            tty_unlock(tt, 1);
            continue;
        }
    }

    // no longer needed, copied to `fdr`
    free(fdt);
    fdt = NULL;

    struct cli_column cols[] = {
        { .cc_title="RFD",  .cc_align=CCAL_RIGHT },
        { .cc_title="PID",  .cc_align=CCAL_RIGHT },
        { .cc_title="VAL",  .cc_align=CCAL_RIGHT },
        { .cc_title="TYPE", .cc_align=CCAL_RIGHT },
    };
    struct cli_table tbl = {0};
    if (cli_table_alloc(cols, sizeof(cols)/sizeof(cols[0]), &tbl))
        goto err;

    // iterate fds
    for (int i = 0; i < fdn; i++)
    {
        char **rowp = cli_table_new_row(&tbl);
        if (!rowp)
            goto err;

        char **cellp;
        CT_ROW_FOR_EACH(cellp, &tbl, rowp) {
            if (CT_IS_CELL(&tbl, cellp, "RFD")) {
                if (-1 == asprintf(cellp, "%d", fdr[i].rfd))
                    goto err;
            }

            if (CT_IS_CELL(&tbl, cellp, "PID")) {
                if (fdr[i].type == LSOFT_PROC) {
                    if (verbose) {
                        if (-1 == asprintf(cellp, "%s/%d", fdr[i].proc.comm, fdr[i].proc.pid))
                            goto err;
                    }
                    else {
                        if (-1 == asprintf(cellp, "%d", fdr[i].proc.pid))
                            goto err;
                    }
                }
                else if (fdr[i].type == LSOFT_PTY) {
                    if (fdr[i].tty.sid > 0) {
                        if (-1 == asprintf(cellp, "%d/pty", fdr[i].tty.sid))
                            goto err;
                    }
                    else {
                        if (-1 == asprintf(cellp, "-/pty"))
                            goto err;
                    }
                }
            }

            if (CT_IS_CELL(&tbl, cellp, "VAL")) {
                if (fdr[i].type == LSOFT_PROC) {
                    if (-1 == asprintf(cellp, "%d", fdr[i].proc.nfd))
                            goto err;
                }
                else if (fdr[i].type == LSOFT_PTY) {
                    if (-1 == asprintf(cellp, "/dev/pts%d", fdr[i].tty.mfd))
                        goto err;
                }
            }

            if (CT_IS_CELL(&tbl, cellp, "TYPE")) {
                if (fdr[i].type == LSOFT_PROC) {
                    if (fdr[i].proc.fdtype)
                        if (-1 == asprintf(cellp, "%s", fdr[i].proc.fdtype))
                                goto err;
                }
                else if (fdr[i].type == LSOFT_PTY) {
                    if (-1 == asprintf(cellp, "%s", fdr[i].tty.end))
                        goto err;
                }
            }
        }
    }

    if (0 != cli_table_print(&tbl, verbose))
        goto err;

    ret = 0;
err:
    if (ret)
        perror("tvm: lsof");
    cli_table_dealloc(&tbl);
    if (fdt)
        free(fdt);
    return ret;
}


static int tvm_pwd(int argc, char **argv)
{
    char pwd[PATH_MAX];
    getcwd(pwd, sizeof(pwd));

    printf("%s\n", pwd);
    return 0;
}

static int tvm_env(int argc, char **argv)
{
    char **p = *current->tsk_relocs.__environ;
    if (argc > 1)
        p = environ;
    for (; p && *p; p++)
        printf("%s\n", *p);
    return 0;
}

static int is_noemu_binary(const char *pathname);
static int tvm_sorunner(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "USAGE: tvm sorunner LIBRARY\n");
        return 1;
    }

    char pathname_abs[PATH_MAX];
    if (!normalize_path(NULL, argv[1], pathname_abs)) {
        fprintf(stderr, "failed to exec \"%s\": -%d (%s)\n", argv[1], errno, strerror(errno));
        return 1;
    }

    int dlflags = RTLD_NOW | (is_noemu_binary(pathname_abs) ? RTLD_NOEMU : 0);
    void *handle = ulinker_dlopen(current->tsk_linker, pathname_abs, dlflags);
    if (!handle) {
        fprintf(stderr, "failed to exec \"%s\": %s\n", pathname_abs, ulinker_dlerror());
        return 1;
    }

    void *sym = ulinker_dlsym(current->tsk_linker, handle, "main", 1);
    if (!sym) {
        fprintf(stderr, "failed to exec \"%s\": %s\n", pathname_abs, ulinker_dlerror());
        return 1;
    }

    struct ulinker_dladdr_info info;
    ulinker_dladdr(current->tsk_linker, sym, &info, 0);

    printf("sorunner %s %s %p base %p\n", argv[1], "main", sym, info.udli_fbase);

    uint64_t ret;
    int callerr = emulator_call(&ret, (uint64_t)sym,
                  (uint64_t)(argc - 1),
                  (uint64_t)(argv + 1),
                  (uint64_t)*current->tsk_relocs.__environ,
                0, 0, 0);
    if (callerr)
        printf("sorunner emulator_call failed!\n");

    return (callerr ? callerr : (int)ret);
}

static void tvm_cpio_print_mode(uint32_t mode) {
    char buf[11] = "----------";

    /* file type */
    switch (mode & 0170000) {
        case 0100000: buf[0] = '-'; break; /* regular */
        case 0040000: buf[0] = 'd'; break; /* dir */
        case 0120000: buf[0] = 'l'; break; /* symlink */
        default:      buf[0] = '?'; break;
    }

    /* owner */
    buf[1] = (mode & 0400) ? 'r' : '-';
    buf[2] = (mode & 0200) ? 'w' : '-';
    buf[3] = (mode & 0100) ? 'x' : '-';

    /* group */
    buf[4] = (mode & 0040) ? 'r' : '-';
    buf[5] = (mode & 0020) ? 'w' : '-';
    buf[6] = (mode & 0010) ? 'x' : '-';

    /* others */
    buf[7] = (mode & 0004) ? 'r' : '-';
    buf[8] = (mode & 0002) ? 'w' : '-';
    buf[9] = (mode & 0001) ? 'x' : '-';

    printf("%s ", buf);
}

static int tvm_cpio_cb(void *arg, const cpio_t *cpio) {
    (void)arg;

    tvm_cpio_print_mode(cpio->c_mode);

    /* uid/gid */
    printf("%u %u ", cpio->c_uid, cpio->c_gid);

    /* name */
    printf("%s\n", cpio->name);

    return 0;
}

static int tvm_cpio(int argc, char **argv)
{
    if (isatty(STDIN_FILENO))
        err(1, "isatty(0)");

    size_t size;
    void *buf = read_all(STDIN_FILENO, &size);
    if (!buf)
        err(1, "read(0)");

    if (0 != cpio_parse(buf, size, (cpio_cb)tvm_cpio_cb, NULL))
        errx(1, "cpio_parse");

    return 0;
}

static int tvm_mount_cpio(const char *pathname, const void *buf, unsigned long size) {
    struct vfs_node *root = vnalloc(pathname, S_IFDIR | 0766, getuid(), getgid());
    if (!root)
        return -1;
    
    if (0 != vfs_load_cpio(root, buf, size)) {
        vn_unlock(root, 1);
        return -1;
    }

    vn_unlock(root, 0);
    vfs_add_root(root);
    vn_lock(root, 0);
    vn_unlock(root, 1);
    return 0;
}

static int tvm_mount(int argc, char **argv)
{
    if (argc < 3)
        errx(1, "USAGE: tvm mount [CPIO] [TARGET]");

    int fd = open(argv[1], O_RDONLY);
    if (fd == -1)
        err(1, "%s", argv[1]);

    size_t size;
    void *buf = read_all(fd, &size);
    if (!buf)
        err(1, "read");

    close(fd);

    char pathname_abs[PATH_MAX];
    if (!normalize_path(NULL, argv[2], pathname_abs)) {
        err(1, "normalize_path(NULL, \"%s\")", argv[2]);
        return 1;
    }

    if (0 != tvm_mount_cpio(pathname_abs, buf, size))
        err(1, "tvm_mount_cpio");

    return 0;
}

int tvm_info(int argc, char **argv) {
    printf("tvm\n");
    return 0;
}

static int tvm_shell(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "USAGE: tvm shell COMMAND [ARGS..]\n");
        return 1;
    }

    // join all args into a single command string
    size_t len = 0;
    for (int i = 1; i < argc; i++)
        len += strlen(argv[i]) + 1; // +1 for space or NUL

    // Zero-padded to whole words: fork's pointer fixup rewrites any stack word
    // that looks like a pointer into a copied range (see tvm_alloca_zero).
    char *cmd = tvm_alloca_zero(len);
    char *p = cmd;
    for (int i = 1; i < argc; i++) {
        size_t n = strlen(argv[i]);
        memcpy(p, argv[i], n);
        p += n;
        *p++ = (i < argc - 1) ? ' ' : '\0';
    }

    int r = system(cmd);
    return WIFEXITED(r) ? WEXITSTATUS(r) : 128 + WTERMSIG(r);
}

static int tvm_main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "USAGE: tvm COMMAND\n");
        return 1;
    }

    if (0 == strcmp(argv[1], "-h")) {
        printf("USAGE: tvm COMMAND\n");
        printf("TembleOS Virtual Machine Diagnostics\n\n");
        printf("COMMANDs:\n");
        printf("  ps             tasks list\n");
        printf("  lsof           fd list\n");
        printf("  pwd            print working directory\n");
        printf("  env            print environment variables\n");
        printf("  sorunner       run shared object\n");
        printf("  cpio           CPIO archive reader\n");
        printf("  mount          VFS mount\n");
        printf("  shell          execute command via system()\n");
        printf("\n");
        return 0;
    }

    if (0 == strcmp(argv[1], "ps")) return tvm_ps(argc - 1, argv + 1);
    if (0 == strcmp(argv[1], "lsof")) return tvm_lsof(argc - 1, argv + 1);
    if (0 == strcmp(argv[1], "pwd")) return tvm_pwd(argc - 1, argv + 1);
    if (0 == strcmp(argv[1], "env")) return tvm_env(argc - 1, argv + 1);
    if (0 == strcmp(argv[1], "sorunner")) return tvm_sorunner(argc - 1, argv + 1);
    if (0 == strcmp(argv[1], "cpio")) return tvm_cpio(argc - 1, argv + 1);
    if (0 == strcmp(argv[1], "mount")) return tvm_mount(argc - 1, argv + 1);
    if (0 == strcmp(argv[1], "info")) return tvm_info(argc - 1, argv + 1);
    if (0 == strcmp(argv[1], "shell")) return tvm_shell(argc - 1, argv + 1);

    fprintf(stderr, "tvm: invalid argument '%s'\n", argv[1]);
    fprintf(stderr, "Try 'tvm -h' for more information.\n");
    return 1;
}

/////////////
// MARK: Initrc
/////////////

static int initrc_expand_vars(const char *input, char *output, size_t output_size)
{
    const char *p = input;
    char *out = output;
    char *end = output + output_size - 1;

    while (*p && out < end) {
        if (*p == '$') {
            p++;
            const char *var_start;
            size_t var_len;
            if (*p == '{') {
                p++;
                var_start = p;
                while (*p && *p != '}') p++;
                var_len = p - var_start;
                if (*p == '}') p++;
            } else {
                var_start = p;
                while (*p && ((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == '_'))
                    p++;
                var_len = p - var_start;
            }
            if (var_len > 0) {
                char name[256];
                if (var_len >= sizeof(name)) var_len = sizeof(name) - 1;
                memcpy(name, var_start, var_len);
                name[var_len] = 0;
                const char *val = getenv(name);
                if (val) {
                    while (*val && out < end)
                        *out++ = *val++;
                }
            }
        } else {
            *out++ = *p++;
        }
    }
    *out = 0;
    return 0;
}

static void initrc_cmd_mount(const char *arg)
{
    char expanded[PATH_MAX * 2];
    initrc_expand_vars(arg, expanded, sizeof(expanded));

    char *colon = strchr(expanded, ':');
    if (!colon) {
        INFO("initrc: mount: expected hostpath:guestpath, got '%s'", expanded);
        return;
    }
    *colon = 0;
    const char *hostpath = expanded;
    const char *guestpath = colon + 1;

    int fd = open(hostpath, O_RDONLY);
    if (fd == -1) {
        INFO("initrc: mount: cannot open '%s': %s", hostpath, strerror(errno));
        return;
    }

    size_t size;
    void *buf = read_all(fd, &size);
    close(fd);
    if (!buf) {
        INFO("initrc: mount: cannot read '%s'", hostpath);
        return;
    }

    if (tvm_mount_cpio(guestpath, buf, size) != 0)
        INFO("initrc: mount: failed to mount '%s' at '%s'", hostpath, guestpath);
    else
        INFO("initrc: mounted '%s' at '%s'", hostpath, guestpath);

    free(buf);
}

static void initrc_setenv_internal(const char *arg, int overwrite)
{
    char expanded[4096];
    initrc_expand_vars(arg, expanded, sizeof(expanded));

    char *eq = strchr(expanded, '=');
    if (!eq) {
        INFO("initrc: setenv: expected name=value, got '%s'", expanded);
        return;
    }
    *eq = 0;
    const char *name = expanded;
    const char *value = eq + 1;

    if (!overwrite && getenv(name)) {
        INFO("initrc: setenv %s already set, skipping", name);
        return;
    }

    if (!current || !current->tsk_relocs.__environ)
        return;

    size_t name_len = strlen(name);
    size_t value_len = strlen(value);
    char *string = malloc(name_len + value_len + 2);
    if (!string)
        return;
    memcpy(string, name, name_len);
    string[name_len] = '=';
    memcpy(string + name_len + 1, value, value_len + 1);

    char **env = *current->tsk_relocs.__environ
        ? findenv(*current->tsk_relocs.__environ, name, name_len)
        : NULL;
    if (env) {
        free(*env);
        *env = string;
    } else {
        char **old_environ = *current->tsk_relocs.__environ;
        char **new_environ = addenv(old_environ, string);
        if (new_environ) {
            *current->tsk_relocs.__environ = new_environ;
            free(old_environ);
        } else
            free(string);
    }

    INFO("initrc: setenv %s=%s", name, value);
}

static void initrc_cmd_setenv(const char *arg) { initrc_setenv_internal(arg, 1); }
static void initrc_cmd_setenv_default(const char *arg) { initrc_setenv_internal(arg, 0); }

static void initrc_cmd_echo(const char *arg)
{
    char expanded[4096];
    initrc_expand_vars(arg, expanded, sizeof(expanded));
    INFO("initrc: %s", expanded);
}

static void initrc_cmd_mkdir(const char *arg)
{
    char expanded[4096];
    initrc_expand_vars(arg, expanded, sizeof(expanded));
    char *p = expanded;
    while (*p) {
        p += strspn(p, "/");
        p += strcspn(p, "/");
        char saved = *p;
        *p = '\0';
        mkdir(expanded, 0755);
        *p = saved;
    }
}

static void initrc_cmd_umask(const char *arg)
{
    char expanded[64];
    initrc_expand_vars(arg, expanded, sizeof(expanded));
    const char *p = expanded;
    while (*p == ' ' || *p == '\t') p++;
    if (*p < '0' || *p > '7') {
        INFO("initrc: bad umask \"%s\"", expanded);
        return;
    }
    unsigned mask = 0;
    for (; *p >= '0' && *p <= '7'; p++)
        mask = (mask << 3) | (*p - '0');
    current->tsk_umask = mask & 0777;
}

static struct {
    const char *name;
    void (*handler)(const char *arg);
} initrc_cmds[] = {
    { "mount",   initrc_cmd_mount },
    { "setenv",  initrc_cmd_setenv },
    { "setenv?", initrc_cmd_setenv_default },
    { "echo",    initrc_cmd_echo },
    { "mkdir",   initrc_cmd_mkdir },
    { "umask",   initrc_cmd_umask },
    { NULL, NULL }
};

static void tvm_exec_initrc(const char *path)
{
    int fd = open(path, O_RDONLY);
    if (fd == -1) {
        INFO("initrc: cannot open '%s': %s", path, strerror(errno));
        return;
    }

    size_t size;
    void *buf = read_all(fd, &size);
    close(fd);
    if (!buf) {
        INFO("initrc: cannot read '%s'", path);
        return;
    }

    INFO("initrc: loading '%s'", path);

    char *data = (char *)buf;
    char *line = data;
    for (size_t i = 0; i <= size; i++) {
        if (i == size || data[i] == '\n') {
            data[i] = 0;

            while (*line == ' ' || *line == '\t') line++;

            if (*line && *line != '#') {
                const char *cmd = line;
                char *sp = line;
                while (*sp && *sp != ' ' && *sp != '\t') sp++;
                const char *arg = "";
                if (*sp) {
                    *sp = 0;
                    arg = sp + 1;
                    while (*arg == ' ' || *arg == '\t') arg++;
                }

                int found = 0;
                for (int j = 0; initrc_cmds[j].name; j++) {
                    if (strcmp(cmd, initrc_cmds[j].name) == 0) {
                        initrc_cmds[j].handler(arg);
                        found = 1;
                        break;
                    }
                }
                if (!found)
                    INFO("initrc: unknown command '%s'", cmd);
            }

            line = data + i + 1;
        }
    }

    free(buf);
}

/////////////
// MARK: API functions
/////////////

/**
 * I have no idea why `__panic()` is here and at this point, I'm too afraid to ask.
 */

static void rmem_print_cb(char *msg) {
    char __ts[32];
    format_time(__ts, sizeof(__ts));
    char __msgbuf[0x1000];
    int __num = snprintf(__msgbuf, sizeof(__msgbuf), "%s Kernel(%d): RMEM: %s", __ts, __gettid(), msg);
    log_write(__msgbuf, __num);
}

static void ulinker_print_cb(char *msg) {
    char __ts[32];
    format_time(__ts, sizeof(__ts));
    char __msgbuf[0x1000];
    int __num = snprintf(__msgbuf, sizeof(__msgbuf), "%s Kernel(%d): ulinker: %s", __ts, __gettid(), msg);
    log_write(__msgbuf, __num);
}

/*
 * fork's pointer fixup (cow_fixup_process) rewrites every 8-byte word of a
 * copied region whose value falls inside a copied region: it cannot tell a
 * pointer from two adjacent ints. Two small ints {a, b} read as the address
 * b<<32 | a, so a copied region covering [N<<32, N<<32 + small) turns every
 * such pair in the child into garbage. git's start_command() keeps its pipe
 * ends in an int[2] on the stack: with a heap slab mapped across
 * 0x400000000 the pair {3, 4}, read as 0x400000003, was "relocated", and
 * the child's dup2()/close() failed with EBADF ("fatal: dup2() in child
 * failed: Bad file descriptor", "unable to execute pager 'less'"). The
 * kernel packs large anonymous mappings bottom-up on macOS, so a slab lands
 * on a 4 GB boundary once the process has mapped ~4 GB of them: in a
 * long-lived sshd backend, after enough tasks.
 *
 * Two defenses. tvm_mmap_guarded() places the regions TVM maps and copies
 * (heap slabs, image segments) clear of [N<<32, N<<32 + TVM_MMAP_GUARD) for
 * every N, and tvm_reserve_boundary_pages() pins a PROT_NONE page at N<<32
 * for every N a descriptor number can be, so no mapping at all -- the
 * thread stacks libpthread places included -- can hold the address a pair
 * of descriptors forms. What stays ambiguous is a pair whose first int is
 * at least TVM_MMAP_GUARD (a page, on a stack) next to an int equal to the
 * 4 GB bucket of a copied region, and memory the emulator maps itself.
 */
#define TVM_MMAP_GUARD ((uintptr_t)16 << 20)

// The guard window [w, w + TVM_MMAP_GUARD) intersecting [start, start + len),
// if any; the caller keeps len small enough for at most one.
static int tvm_mmap_window_in(uintptr_t start, size_t len, uintptr_t *w)
{
    uintptr_t bucket = (uintptr_t)1 << 32;
    uintptr_t below = start & ~(bucket - 1);
    if (below + TVM_MMAP_GUARD > start) {
        *w = below;
        return 1;
    }
    if (below + bucket < start + len) {
        *w = below + bucket;
        return 1;
    }
    return 0;
}

// Unmap [lo, hi), except that the part inside the guard window [w, w + GUARD)
// (if `have_w`) stays mapped PROT_NONE, so the kernel never hands it out again.
static void tvm_mmap_release(uintptr_t lo, uintptr_t hi, int have_w, uintptr_t w)
{
    uintptr_t wlo = w > lo ? w : lo;
    uintptr_t whi = w + TVM_MMAP_GUARD < hi ? w + TVM_MMAP_GUARD : hi;
    if (!have_w || wlo >= whi) {
        if (lo < hi)
            CALL_FUNC(munmap, (void *)lo, hi - lo);
        return;
    }
    if (lo < wlo)
        CALL_FUNC(munmap, (void *)lo, wlo - lo);
    CALL_FUNC(mprotect, (void *)wlo, whi - wlo, PROT_NONE);
    if (whi < hi)
        CALL_FUNC(munmap, (void *)whi, hi - whi);
}

// An anonymous private mapping of `size` bytes that contains no address in
// a guard window. When the kernel's choice does, a reservation of twice the
// size plus a window is taken instead: one of its two sides of the window is
// big enough, the rest is given back (the window itself stays, PROT_NONE).
static void *tvm_mmap_guarded(size_t size, int prot, int flags)
{
    void *map = CALL_FUNC(mmap, NULL, size, prot, flags, -1, 0);
    if (map == MAP_FAILED)
        return map;

    uintptr_t w;
    if (!tvm_mmap_window_in((uintptr_t)map, size, &w))
        return map;

    size_t rsize = 2 * size + TVM_MMAP_GUARD;
    if (rsize + TVM_MMAP_GUARD > ((uintptr_t)1 << 32)) {
        // cannot fit between two windows: nothing to be done for it
        INFO("mmap %p-%p: %zu bytes crosses a 4 GB boundary, unguarded", map, (uintptr_t)map + size, size);
        return map;
    }

    CALL_FUNC(munmap, map, size);
    uintptr_t r = (uintptr_t)CALL_FUNC(mmap, NULL, rsize, prot, flags, -1, 0);
    if (r == (uintptr_t)MAP_FAILED) {
        // out of address space for the reservation: better unguarded than
        // ENOMEM for a mapping that was just possible
        return CALL_FUNC(mmap, NULL, size, prot, flags, -1, 0);
    }

    uintptr_t start;
    int have_w = tvm_mmap_window_in(r, rsize, &w);
    if (!have_w || w >= r + size)
        start = r;                       // no window, or room below it
    else
        start = w + TVM_MMAP_GUARD;      // room above it
    VERIFY(start + size <= r + rsize);

    tvm_mmap_release(r, start, have_w, w);
    tvm_mmap_release(start + size, r + rsize, have_w, w);
#ifdef TRACE_MMAP
    INFO("mmap %p-%p moved off the guard window at %p", (void *)start, (void *)(start + size), (void *)w);
#endif
    return (void *)start;
}

// A PROT_NONE page at N<<32 for every N a descriptor number can be (see
// tvm_mmap_guarded). Pages already in use stay as they are: whatever is
// there is not a region fork copies, and keeps our mappings out just the
// same. Called once, before the first arena.
static void tvm_reserve_boundary_pages(void)
{
    size_t pg = (size_t)sysconf(_SC_PAGESIZE);
    for (uintptr_t n = 1; n < MAX_FILES; n++) {
        void *hint = (void *)(n << 32);
        void *p = CALL_FUNC(mmap, hint, pg, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (p == MAP_FAILED)
            continue;
        if (p != hint)
            CALL_FUNC(munmap, p, pg);
    }
}

void *rmem_mmap(void *hint, size_t size, int prot, int flags, int fd, off_t offset) {
    void *map;
    if (!hint && fd == -1 && !(flags & MAP_FIXED))
        map = tvm_mmap_guarded(size, prot, flags);
    else
        map = CALL_FUNC(mmap, hint, size, prot, flags, fd, offset);
#ifdef TRACE_MMAP
    INFO("mmap %p-%p (rmem)", map, (uintptr_t)map + size);
#endif
    return map;
}

int rmem_munmap(void *addr, size_t size) {
#ifdef TRACE_MMAP
    INFO("munmap %p-%p (rmem)", addr, (uintptr_t)addr + size);
#endif
    return CALL_FUNC(munmap, addr, size);
}

struct rmem_ops rmem_ops = {
    .ro_mmap = rmem_mmap,
    .ro_munmap = rmem_munmap,
};

#define ULINKER_ARENA(_arg) ((_arg) ? ((struct task *)arg)->tsk_ar : global_arena)

// NOTE: we want to support destroying a linker from a different task, but we can safely assume no task wil dlopen for another so `VERIFY(arg == current);` is legit

static void *ulinker_malloc(void *arg, size_t size)             { return rmem_arena_alloc(ULINKER_ARENA(arg), size); }
static void  ulinker_free(void *arg, void *ptr)                 { rmem_arena_free(ULINKER_ARENA(arg), ptr); }
static void *ulinker_realloc(void *arg, void *ptr, size_t size) { return rmem_arena_realloc(ULINKER_ARENA(arg), ptr, size); }

static int     ulinker_open(void *arg, const char *name)                { VERIFY(arg == current); return open(name, O_RDONLY); }
static int     ulinker_close(void *arg, int fd)                         { VERIFY(arg == current); return close(fd); }
static ssize_t ulinker_read(void *arg, int fd, void *buf, size_t count) { VERIFY(arg == current); return read(fd, buf, count); }
static int     ulinker_fstat(void *arg, int fd, struct stat *statbuf)   { VERIFY(arg == current); return fstat(fd, statbuf); }

static void *ulinker_mmap(void *arg, void *hint, size_t length, int prot, int flags, int fd, off_t offset) {
    VERIFY(arg == current);
    // an image's address range (its writable segments are copied by fork)
    if (!hint && fd == -1 && !(flags & MAP_FIXED))
        return tvm_mmap_guarded(length, prot, flags);
    return mmap(hint, length, prot, flags, fd, offset);
}
static int   ulinker_munmap(void *arg, void *addr, size_t size)                                            {
                                                                    #ifdef TRACE_MMAP
                                                                        INFO("munmap %p-%p (ulinker)", addr, (uintptr_t)addr + size);
                                                                    #endif
                                                                    return CALL_FUNC(munmap, addr, size);
                                                                }
static int   ulinker_mprotect(void *arg, void *addr, size_t size, int flags)                               { return CALL_FUNC(mprotect, addr, size, flags); }

static void *ulinker_call(void *arg, void *addr) {
    uint64_t ret;
    if (0 != emulator_call(&ret, (uint64_t)addr, 0, 0, 0, 0, 0, 0))
        panic("ulinker_call emulator_call failed");
    return (void *)ret;
}

// Dependencies (ulinker.c): the host's dlopen for the system libraries a
// guest image depends on. dlopen
// is not interposed, so this is the host's; RTLD_GLOBAL makes the library's
// symbols reachable for ulinker_external_dlsym's dlsym(RTLD_DEFAULT).
static void *ulinker_dlopen_host(void *arg, const char *name) {
    void *h = dlopen(name, RTLD_NOW | RTLD_GLOBAL);
    if (!h)
        INFO("dependency %s: %s", name, dlerror());
    return h;
}
static int ulinker_dlclose_host(void *arg, void *handle) { return dlclose(handle); }

static struct ulinker_ops ulinker_ops = {
    .uo_malloc = ulinker_malloc,
    .uo_free = ulinker_free,
    .uo_realloc = ulinker_realloc,

    .uo_open = ulinker_open,
    .uo_close = ulinker_close,
    .uo_read = ulinker_read,
    .uo_fstat = ulinker_fstat,

    .uo_mmap = ulinker_mmap,
    .uo_munmap = ulinker_munmap,
    .uo_mprotect = ulinker_mprotect,

    .uo_call = ulinker_call,

    .uo_dlopen = ulinker_dlopen_host,
    .uo_dlclose = ulinker_dlclose_host,
};

static void *ulinker_external_dlsym(void *arg, const char *s, int emu)
{
    // Version-suffixed symbol variants (Mach-O `sym$DARWIN_EXTSN`/`$UNIX2003`,
    // ELF `sym@GLIBC_x`) must interpose to the same target as their base name;
    // otherwise e.g. `fdopen$DARWIN_EXTSN` escapes interposition and hands out a
    // real libc FILE that our interposed `fclose` then mis-frees. Every lookup
    // below matches against the stripped base name; only the host dlsym()
    // fallthrough gets the original, fully-qualified symbol.
    char base[256];
    size_t base_len = strcspn(s, "$@");
    if (base_len >= sizeof(base))
        base_len = sizeof(base) - 1;
    memcpy(base, s, base_len);
    base[base_len] = '\0';

    if (0 == strcmp(base, "sigaction"))    return &emulator_sigaction;
    if (0 == strcmp(base, "signal"))       return &emulator_signal;

    // only intercept setjmp/longjmp for emulated binaries — in NOEMU mode
    // emulator_setjmp calls real _setjmp which saves the host stack frame,
    // but longjmp back into that frame corrupts the stack canary (the frame
    // was reused by guest code in between), triggering __stack_chk_fail/SIGABRT
    if (emu) {
        if (0 == strcmp(base, "_setjmp"))      return &emulator_setjmp;
        if (0 == strcmp(base, "setjmp"))       return &emulator_setjmp;
        if (0 == strcmp(base, "__sigsetjmp"))  return &emulator_setjmp;
        if (0 == strcmp(base, "sigsetjmp"))    return &emulator_setjmp;
        if (0 == strcmp(base, "longjmp"))      return &emulator_longjmp;
        if (0 == strcmp(base, "_longjmp"))     return &emulator_longjmp;
        if (0 == strcmp(base, "siglongjmp"))   return &emulator_longjmp;
    }

    if (0 == strcmp(base, "__chkstk_darwin")) return &chkstk_nop;

    if (0 == strcmp(base, "pthread_create")) return &emulator_pthread_create;

    if (0 == strcmp(base, "optarg"))   return &current->tsk_data->optarg;
    if (0 == strcmp(base, "optind"))   return &current->tsk_data->optind;
    if (0 == strcmp(base, "opterr"))   return &current->tsk_data->opterr;
    if (0 == strcmp(base, "optopt"))   return &current->tsk_data->optopt;

    if (0 == strcmp(base, "__environ"))  return &current->tsk_data->__environ;
    if (0 == strcmp(base, "environ"))    return &current->tsk_data->__environ;

    struct function_metadata *fm;
    list_for_each_entry(fm, &all_function_metadata, fm_list) {
        if (0 == strcmp(fm->fm_name, base))
            return fm->fm_ptr;
    }
    return dlsym(RTLD_DEFAULT, s);
}

static struct ulinker_external_dlinfo_args ulinker_tvm_external_args = {
    .ue_private = NULL,
    .ue_name = "libtvm",
    .ue_dlsym = ulinker_external_dlsym,
};

static void ulinker_event_cb(int event, void *arg)
{
    struct ulinker_load_event *ule = (struct ulinker_load_event *)arg;
    if (event == ULEVENT_LOAD) {
        add_ulinker_loaded_range(ule);
    }
    else if (event == ULEVENT_UNLOAD) {
        remove_ulinker_loaded_range(ule);
    }
}

static const char *noemu_list = NULL;
static int noemu_global = 0;

static void tvm_parse_noemu(void)
{
    const char *env = getenv("TVM_NOEMU");
    if (!env)
        return;
    if (strcmp(env, "1") == 0)
        noemu_global = 1;
    else
        noemu_list = strdup(env);
}

static int is_noemu_binary(const char *pathname)
{
    if (noemu_global)
        return 1;
    if (!noemu_list)
        return 0;

    char resolved[PATH_MAX];
    strncpy(resolved, pathname, PATH_MAX - 1);
    resolved[PATH_MAX - 1] = 0;

    char link[PATH_MAX];
    ssize_t n;
    while ((n = readlink(resolved, link, sizeof(link) - 1)) > 0) {
        link[n] = 0;
        if (link[0] == '/') {
            strncpy(resolved, link, PATH_MAX - 1);
            resolved[PATH_MAX - 1] = 0;
        } else {
            char *slash = strrchr(resolved, '/');
            if (slash) {
                snprintf(slash + 1, PATH_MAX - (slash + 1 - resolved), "%s", link);
            } else {
                strncpy(resolved, link, PATH_MAX - 1);
                resolved[PATH_MAX - 1] = 0;
            }
        }
    }

    const char *basename = strrchr(resolved, '/');
    basename = basename ? basename + 1 : resolved;

    const char *p = noemu_list;
    while (*p) {
        const char *end = strchr(p, ':');
        size_t len = end ? (size_t)(end - p) : strlen(p);

        if ((len == strlen(resolved) && strncmp(p, resolved, len) == 0) ||
            (len == strlen(basename) && strncmp(p, basename, len) == 0)) {
            return 1;
        }

        p += len;
        if (*p == ':') p++;
    }
    return 0;
}


static void usershell_cache_init(void);
static int tvm_path_search(const char *file, char *out);

#if defined(__linux__)
// True if the ELF open on `fd` defines `main` in its dynamic or regular
// symbol table, where ulinker_dlsym(..., "main") finds it: execve starts an
// image there. TVM's own builds keep it (unstripped); a host executable
// (Linux /bin/sh is dash) is a perfectly good ELF that is stripped and
// fails there ("symbol main not found").
static int tvm_elf_exports_main(int fd)
{
    Elf64_Ehdr eh;
    if (pread(fd, &eh, sizeof(eh), 0) != (ssize_t)sizeof(eh) ||
        eh.e_ident[EI_CLASS] != ELFCLASS64 ||
        eh.e_shentsize != sizeof(Elf64_Shdr) || !eh.e_shnum || eh.e_shnum > 4096)
        return 0;

    size_t shsize = (size_t)eh.e_shnum * sizeof(Elf64_Shdr);
    Elf64_Shdr *sh = malloc(shsize);
    if (!sh)
        return 0;
    int found = 0;
    char *strtab = NULL;
    Elf64_Sym *syms = NULL;
    if (pread(fd, sh, shsize, (off_t)eh.e_shoff) != (ssize_t)shsize)
        goto out;

    for (int i = 0; i < eh.e_shnum && !found; i++) {
        if ((sh[i].sh_type != SHT_DYNSYM && sh[i].sh_type != SHT_SYMTAB) ||
            sh[i].sh_link >= eh.e_shnum ||
            sh[i].sh_entsize != sizeof(Elf64_Sym))
            continue;
        Elf64_Shdr *str = &sh[sh[i].sh_link];
        if (sh[i].sh_size > (256u << 20) || str->sh_size > (256u << 20) || !str->sh_size)
            continue;
        free(syms);
        free(strtab);
        syms = malloc(sh[i].sh_size);
        strtab = malloc(str->sh_size);
        if (!syms || !strtab ||
            pread(fd, syms, sh[i].sh_size, (off_t)sh[i].sh_offset) != (ssize_t)sh[i].sh_size ||
            pread(fd, strtab, str->sh_size, (off_t)str->sh_offset) != (ssize_t)str->sh_size)
            break;
        strtab[str->sh_size - 1] = 0;
        size_t nsyms = sh[i].sh_size / sizeof(Elf64_Sym);
        for (size_t k = 0; k < nsyms; k++) {
            unsigned bind = ELF64_ST_BIND(syms[k].st_info);
            if (syms[k].st_shndx != SHN_UNDEF && syms[k].st_name < str->sh_size &&
                (bind == STB_GLOBAL || bind == STB_WEAK) &&
                0 == strcmp(strtab + syms[k].st_name, "main")) {
                found = 1;
                break;
            }
        }
    }
out:
    free(syms);
    free(strtab);
    free(sh);
    return found;
}
#endif

// True if `path` is an image TVM's linker can load: a thin 64-bit Mach-O,
// or (Linux) an ELF that exports main. Fat Mach-O binaries (the host's
// /bin/*, /usr/bin/* on macOS), shared-cache stubs (iOS), host ELF
// executables (Linux) and anything else are not.
static int tvm_loadable_image(const char *path)
{
    unsigned char magic[4] = {0};
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return 0;
    ssize_t n = read(fd, magic, sizeof(magic));
    int ok = n == 4 &&
        (memcmp(magic, "\xcf\xfa\xed\xfe", 4) == 0 ||   /* MH_MAGIC_64 */
         memcmp(magic, "\x7f" "ELF", 4) == 0);
#if defined(__linux__)
    if (ok && memcmp(magic, "\x7f" "ELF", 4) == 0)
        ok = tvm_elf_exports_main(fd);
#endif
    close(fd);
    return ok;
}

#if defined(__linux__)
/*
 * gdb JIT registration of guest images (see ULINKER_GDB_JIT): TVM_GDB_JIT=1
 * or 0 decides; otherwise only when a tracer is attached at startup
 * (`gdb --args tvm ...`). Runs before any task exists, on the host's calls.
 */
static int tvm_want_gdb_jit(void)
{
    const char *env = getenv("TVM_GDB_JIT");
    if (env && *env)
        return 0 != strcmp(env, "0");

    char buf[4096];
    int fd = CALL_FUNC(open, "/proc/self/status", O_RDONLY);
    if (fd < 0)
        return 0;
    ssize_t n = CALL_FUNC(read, fd, buf, sizeof(buf) - 1);
    CALL_FUNC(close, fd);
    if (n <= 0)
        return 0;
    buf[n] = 0;
    const char *t = strstr(buf, "TracerPid:");
    return t && atoi(t + sizeof("TracerPid:") - 1) != 0;
}
#endif

static int tvm_started = 0;

/**
 * Boot the VM on the calling thread and exec `file` inside it. See tvm.h.
 */
int tvm_start(const char *file, char *const argv[], char *const envp[], const struct tvm_config *cfg)
{
    static const struct tvm_config default_cfg = { .log_fd = STDOUT_FILENO };
    if (!cfg)
        cfg = &default_cfg;

    // Validate. Nothing here touches global state, so every failure leaves
    // the process exactly as it was.

    if (!file || !argv || cfg->log_fd < -1) {
        errno = EINVAL;
        return -1;
    }

    if (tvm_started) {
        errno = EBUSY;
        return -1;
    }

    for (size_t i = 0; i < cfg->nmounts; i++) {
        const struct tvm_mount *m = &cfg->mounts[i];
        char abs[PATH_MAX];
        // vfs_lookup() prefix-matches a root's name verbatim, so the mount
        // point must be absolute and normalized ("/tvm", never "/tvm/" or
        // "//tvm"). normalize_path() with an explicit dirname needs no task.
        if (!m->path || m->path[0] != '/' || !m->data || !m->size ||
            !normalize_path("/", m->path, abs) || 0 == strcmp(abs, "/")) {
            errno = EINVAL;
            return -1;
        }
    }

    if (cfg->cwd) {
        struct stat st;
        if (0 != CALL_FUNC(fstatat, AT_FDCWD, cfg->cwd, &st, 0))
            return -1;
        if (!S_ISDIR(st.st_mode)) {
            errno = ENOTDIR;
            return -1;
        }
    }

    tvm_started = 1;
    g_outfd = cfg->log_fd;

    INFO("tvm_start(\"%s\")", file);

    // Kernel globals: allocator, TLS, linker callbacks.

    init_file_stream();

    rmem_setopt(RMEM_PRINT_CB, (void *)rmem_print_cb);
    rmem_setopt(RMEM_OPS, (void *)&rmem_ops);

    tvm_reserve_boundary_pages();

    global_arena = rmem_arena_create(NULL);
    if (!global_arena)
        panic("couldn't create global arena");

    pthread_key_create(&tls_key, tvm_thread_destructor);
    tls_initialized = 1;

    struct tvm_tls *t = kalloc(sizeof(struct tvm_tls));
    if (!t)
        panic("tvm_tls alloc");
    memset(t, 0, sizeof(struct tvm_tls));
    pthread_setspecific(tls_key, t);

    ulinker_setopt(ULINKER_PRINT_CB, (void *)ulinker_print_cb);
    ulinker_setopt(ULINKER_OPS, &ulinker_ops);
    ulinker_setopt(ULINKER_ADD_EXTERNAL_DLINFO, &ulinker_tvm_external_args);
    ulinker_setopt(ULINKER_EVENT_CB, (void *)ulinker_event_cb);
#if defined(__linux__)
    ulinker_setopt(ULINKER_GDB_JIT, (void *)(long)tvm_want_gdb_jit());
#endif

    // Config mounts. The VFS needs only the global arena, so a bad archive
    // is still reported to the caller before any task exists. A later
    // initrc `mount` of the same path shadows these (vfs_add_root prepends).

    for (size_t i = 0; i < cfg->nmounts; i++) {
        const struct tvm_mount *m = &cfg->mounts[i];
        char abs[PATH_MAX];
        normalize_path("/", m->path, abs);
        if (0 != tvm_mount_cpio(abs, m->data, m->size)) {
            INFO("tvm_start: mount %s failed", abs);
            errno = EINVAL;
            return -1;
        }
    }

    // The initrc must exist, on the host or inside a config mount (hence after
    // the mounts). Resolved the way the interposed open() will resolve it
    // later: against cfg->cwd, or the host cwd. file_access() walks the VFS
    // first and falls back to the host, and needs no task.
    if (cfg->initrc) {
        char base[PATH_MAX], abs[PATH_MAX];
        const char *dir = cfg->cwd;
        if (!dir) {
            if (!CALL_FUNC(getcwd, base, sizeof(base)))
                return -1;
            dir = base;
        }
        if (!normalize_path(dir, cfg->initrc, abs) || 0 != file_access(abs, R_OK, 0)) {
            INFO("tvm_start: initrc %s: %s", cfg->initrc, strerror(errno));
            return -1;
        }
    }

    // FROM HERE ON WE ENTER TVM ON CURRENT THREAD. No more returning.

    // we dont bother lockin current

    current = taskalloc_common();
    if (!current)
        panic("couldn't create main task");

    main_task = current;
    current->tsk_pid = __gettid();
    current->tsk_sid = current->tsk_pgid = current->tsk_pid;
    current->tsk_umask = 022;
    CALL_FUNC(umask, 0); // real umask=0; per-task tsk_umask applied in interposed open/mkdir

    current->tsk_data = rmem_arena_alloc(current->tsk_ar, sizeof(*current->tsk_data));
    if (!current->tsk_data)
        panic("tsk_data");
    memset(current->tsk_data, 0, sizeof(*current->tsk_data));

    // __environ on linux, environ on apple
    current->tsk_relocs.__environ = ulinker_dlsym(current->tsk_linker, RTLD_DEFAULT, "__environ", 0)
                                 ?: ulinker_dlsym(current->tsk_linker, RTLD_DEFAULT, "environ", 0)
                                 ?: &current->tsk_data->__environ;
    current->tsk_relocs.optind = ulinker_dlsym(current->tsk_linker, RTLD_DEFAULT, "optind", 0) ?: &current->tsk_data->optind;
    current->tsk_relocs.opterr = ulinker_dlsym(current->tsk_linker, RTLD_DEFAULT, "opterr", 0) ?: &current->tsk_data->opterr;
    current->tsk_relocs.optopt = ulinker_dlsym(current->tsk_linker, RTLD_DEFAULT, "optopt", 0) ?: &current->tsk_data->optopt;
    current->tsk_relocs.optarg = ulinker_dlsym(current->tsk_linker, RTLD_DEFAULT, "optarg", 0) ?: &current->tsk_data->optarg;

    *current->tsk_relocs.optind = 1;
    *current->tsk_relocs.opterr = 1;
    current->tsk_data->optreset = 0;

    // dup 0,1,2 fds
    for (int i = 0; i < 3; i++) {
        int rfd = CALL_FUNC(dup, i);
        if (-1 == rfd)
            panic("tvm_start::dup %d", i);

        struct file *f = filealloc_from_rfd(rfd);
        if (!f)
            panic("tvm_start::filealloc_from_rfd %d", rfd);

        f->f_refcnt++;

        int nfd = task_assign_file_locked(current, f, i);
        if (nfd != i)
            panic("tvm_start::task_assign_file %d %d", i, nfd);
    }

    // allocate stdin, stdout, stderr filestreams
    for (int i = 0; i < 3; i++) {
        struct file_stream *fs = task_filestreamalloc_locked(current);
        VERIFY(fs);

        struct file *file = file_for_task_locked(current, i);
        VERIFY(file);

        fs->fs_type = FS_FILE;
        fs->fs_nfd = i;
        fs->fs_gencnt = file->f_gencnt;

        memset(fs->fs_mode, 0, sizeof(fs->fs_mode));
        strncpy(fs->fs_mode, (i?"w":"r"), sizeof(fs->fs_mode)-1);

        VERIFY((fs->fs_fp = CALL_FUNC(fdopen, file->f_rfd, fs->fs_mode)));
        file_unlock(file, 1);
        current->tsk_io_filestreams[i] = fs;

        VERIFY(0 == CALL_FUNC(setvbuf, fs->fs_fp, NULL, _IOLBF, 0));
    }

    // The guest environment. Everything TVM reads about itself from here on
    // (TVM_NOEMU, TVM_SHELL, PLATFORM, ...) comes from this copy.
    *current->tsk_relocs.__environ = copyenv(current->tsk_ar, envp ? (char **)envp : environ);
    if (!*current->tsk_relocs.__environ)
        panic("tvm_start::copyenv");

    current_thread = threadalloc(current);
    if (!current_thread)
        panic("tvm_start::threadalloc");

    current_thread->thr_pthread = pthread_self();
    current_thread->thr_task = current;

    // wouldn't be able to fork if caller returned
    current_thread->thr_fork_fp = *(void **)__builtin_frame_address(0);

    list_add_tail(&current_thread->thr_list, &current->tsk_threads);
    current->tsk_threads_cnt++;

    current->tsk_cwd[0] = 0;
    if (!CALL_FUNC(getcwd, current->tsk_cwd, sizeof(current->tsk_cwd)))
        panic("tvm_start::getcwd");

    // execve() replaces this with the program's basename
    strncpy(current->tsk_comm, "tvm", sizeof(current->tsk_comm));
    task_rusage_start(current);

    vtimer_init(); // must be before signal handler installation

    struct sigaction act = {
        .sa_sigaction = signal_handler,
        .sa_flags = SA_SIGINFO,
    };

    for (int i = 0; i < MAX_SIGNALS; i++) {
        if (0 == CALL_FUNC(sigaction, i, NULL, current->tsk_sighandlers + i)) {
            if (i == SIGKILL || i == SIGSTOP)
                continue;
            if (0 != CALL_FUNC(sigaction, i, &act, NULL))
                panic("sigaction(%d): %s", i, strerror(errno));
        }
    }

    tvm_register_program("/usr/bin/tvm", (main_func_t) tvm_main);

#if defined(__APPLE__) && defined(__aarch64__)
    setenv("PLATFORM", "macos-arm64", 0);
#elif defined(__linux__) && defined(__x86_64__)
    setenv("PLATFORM", "linux-x64", 0);
#elif defined(__linux__) && defined(__aarch64__)
    setenv("PLATFORM", "linux-arm64", 0);
#endif

    // Guest-side setup. These are direct calls from libtvm text, so the
    // interposed functions see a TVM return address and enter the VM.

    if (cfg->cwd && 0 != chdir(cfg->cwd))
        panic("tvm_start::chdir(%s): %s", cfg->cwd, strerror(errno));

    if (cfg->initrc)
        tvm_exec_initrc(cfg->initrc);

    tvm_parse_noemu();

    // after the initrc, so a `setenv TVM_SHELL=...` there is honored
    usershell_cache_init();

    // Exec. Never returns on success (ends in the in-VM exit()).

    execvp(file, argv);
    panic("tvm_start::execvp(%s): %s", file, strerror(errno));
}

char ***_tvm_environ()
{
    if (!main_task)
        return &environ;
    
    return &*current->tsk_relocs.__environ;
}

void tvm_log(const char *function, const char *msg, ...)
{
    char msgbuf[0x1000];
    struct tvm_tls *t = tvm_tls();
    int n = snprintf(msgbuf, sizeof(msgbuf), "%s(%d): %s: ", (t && t->tls_current ? t->tls_current->tsk_comm : "<null>"), __gettid(), function);

    va_list ap;
    va_start(ap, msg);
    n += vsnprintf(msgbuf + n, sizeof(msgbuf) - n, msg, ap);
    va_end(ap);

    n += snprintf(msgbuf + n, sizeof(msgbuf) - n, "\n");

    log_write(msgbuf, n);
}

void tvm_dump(int mode)
{
    if (mode == TVM_DUMP_FUNCS) {
        printf("tvm functions:\n");
        struct function_metadata *fm;
        list_for_each_entry(fm, &all_function_metadata, fm_list) {
            printf("  %08zx  %s\n", (uintptr_t)fm->fm_ptr, fm->fm_name);
        }
        printf("\n");
    }
}

#define MAX_BT 16

__attribute__((noreturn))
static void __panic(const char *msg, const char *file, int line)
{
    // Peek, don't tvm_tls(): panic must work on keyless threads (tvm_tls()
    // aborts there — and its abort report would recurse into us via SIGABRT).
    struct tvm_tls *__tls = TVM_TLS_PEEK();
    struct task *cur = __tls ? __tls->tls_current : NULL;

    INFO("panic(caller pid %d tid %d): \"%s\"", (cur?cur->tsk_pid:-1), __gettid(), msg);
    INFO("%s:%d", file, line);
    INFO("Thread Info:");
    INFO(" ppid: %d", (cur?(cur->tsk_parent?cur->tsk_parent->tsk_pid:1):-1));
    INFO(" host pid: %d", CALL_FUNC(getpid));
    INFO(" current: %p", cur);
    INFO(" main_task: %p", main_task);
    INFO(" errno: %d (%s)", errno, strerror(errno));
    INFO("Stacktrace:");
    dump_stacktrace();

    // Normal path: TVM's own signal_handler dispatches the SIGABRT as a fatal
    // signal to the current task (terminate_current_locked), which never
    // returns — the thread is gone, or for the init task the process is.
    CALL_FUNC(raise, SIGABRT);

    // raise() returned, so the handler did not dispatch it: either it deferred
    // the signal (tvm_sig_blocked, in-emu, or the guest masked SIGABRT) or the
    // thread has no VM identity and the handler drops non-fault signals. A
    // panic is fatal regardless, so die by SIGABRT with the default disposition,
    // bypassing the handler, and never return to the caller.
    struct sigaction dfl = { .sa_handler = SIG_DFL };
    CALL_FUNC(sigaction, SIGABRT, &dfl, NULL);
    sigset_t unblk;
    sigemptyset(&unblk);
    sigaddset(&unblk, SIGABRT);
    CALL_FUNC(pthread_sigmask, SIG_UNBLOCK, &unblk, NULL);
    CALL_FUNC(raise, SIGABRT);
    CALL_FUNC(_exit, 128 + SIGABRT);
    __builtin_unreachable();
}

/////////////
// MARK: Hooks
/////////////



/**
 * Oh yeah, there are SOOOO many more functions to hook.
 * Code here mostly re-calls original functions with file-descriptors/streams translated with `file_for_task()` / `file_for_task_stream()`.
 * Some code redirects the call to userspace device drivers.
 * Some code needs to alter the task-specific file-descriptors/signal-handlers table.
 * Also, handle variadic arguments when passing through... thats a big bummer.
 * 
 * TODO:`
 * - _FORTIFY_SOURCE - `__printf_chk()`, `__dup_chk()` etc (for now just don't compile with -Os)
 */

FUNCTION_DEFINE1(DIR *, opendir, const char *, pathname) {
    char pathname_abs[PATH_MAX];

    if (!normalize_path(NULL, pathname, pathname_abs))
        return NULL;

    pathname = pathname_abs;

    int fd = open(pathname, O_RDONLY | O_DIRECTORY);
    if (-1 == fd)
        return NULL;

    DIR *dp = fdopendir(fd);
    if (!dp)
        close(fd);
    return dp;
}

FUNCTION_DEFINE1(DIR *, fdopendir, int, fd) {
    tvm_sig_enter();
    task_lock(current);
    struct file_stream *fs = task_filestreamalloc_locked(current);
    if (!fs) {
        task_unlock(current);
        tvm_sig_leave();
        return NULL;
    }

    struct file *file = file_for_task_locked(current, fd);
    if (!file) {
        task_filestreamdealloc_locked(current, fs);
        task_unlock(current);
        tvm_sig_leave();
        return NULL;
    }

    uint64_t gencnt = file->f_gencnt;

    DIR *dp = CALL_FUNC(fdopendir, file->f_rfd);
    if (!dp) {
        task_filestreamdealloc_locked(current, fs);
        task_unlock(current);
        file_unlock(file, 1);
        tvm_sig_leave();
        return NULL;
    }

    task_unlock(current);
    file_unlock(file, 1);

    fs->fs_nfd = fd;
    fs->fs_gencnt = gencnt;
    
    fs->fs_type = FS_DIR;
    fs->fs_dp = dp;

    tvm_sig_leave();
    
    return (DIR *)fs;
}

FUNCTION_DEFINE1(int, closedir, DIR *, dir) {
    if (!dir)
        return EBADF;

    struct file_stream *fs = (struct file_stream *)dir;


    int nfd = fs->fs_nfd;

    tvm_sig_enter();
    task_lock(current);
    task_filestreamdealloc_locked(current, fs);
    task_unlock(current);
    tvm_sig_leave();
    
    if (-1 == close(nfd))
        return errno;
    return 0;
}

FUNCTION_DEFINE1(struct dirent *, readdir, DIR *, dir) {
    tvm_sig_enter();
    struct file *file = file_for_task_dp(current, dir);
    if (!file) {
        tvm_sig_leave();
        return NULL;
    }

    struct file_stream *fs = (struct file_stream *)dir;

    if (!file->f_ops || !file->f_ops->f_readdir) {
        file_unlock(file, 0);
        struct dirent *dent = CALL_FUNC(readdir, real_dp(dir));
        file_lock(file, 0);
        file_unlock(file, 1);
        tvm_sig_leave();
        return dent;
    }

    struct dirent *de = &fs->fs_dirent;
    if (0 != file->f_ops->f_readdir(file, de)) {
        file_unlock(file, 1);
        tvm_sig_leave();
        return NULL;
    }

    file_unlock(file, 1);
    tvm_sig_leave();
    return de;
}

FUNCTION_DEFINE1(int, dirfd, DIR *, dir) {
    struct file_stream *fs = (struct file_stream *)dir;
    return fs->fs_nfd;
}

/**
 * returns -1 on error, 0 on success
 */
static int fmode_to_flags(const char *mode, int *out)
{
    int flags;
    const char *p = mode;

    switch (*p++) {
    case 'r': flags = O_RDONLY; break;
    case 'w': flags = O_CREAT | O_WRONLY | O_TRUNC; break;
    case 'a': flags = O_CREAT | O_WRONLY | O_APPEND; break;
    default:  errno = EINVAL; return -1;
    }

    if (*p == '+') { flags = (flags & ~O_ACCMODE) | O_RDWR; p++; }

    for (; *p; p++) {
        switch (*p) {
        case '+': flags = (flags & ~O_ACCMODE) | O_RDWR; break;
        case 'b': break;
        case 'e': flags |= O_CLOEXEC; break;
        case 'x': flags |= O_EXCL; break;
        default:  break;
        }
    }

    *out = flags;
    return 0;
}

FUNCTION_DEFINE2(FILE *, fopen, const char *, pathname, const char *, mode)
{
    char pathname_abs[PATH_MAX];

    if (!normalize_path(NULL, pathname, pathname_abs))
        return NULL;

    pathname = pathname_abs;

    int flags;
    if (fmode_to_flags(mode, &flags))
        return NULL;

    int fd = open(pathname, flags, 0666);
    if (-1 == fd)
        return NULL;

    FILE *fp = fdopen(fd, mode);
    if (!fp)
        close(fd);
    return fp;
}

FUNCTION_DEFINE2(FILE *, fdopen, int, fd, const char *, mode) {
    tvm_sig_enter();
    task_lock(current);
    struct file_stream *fs = task_filestreamalloc_locked(current);
    if (!fs) {
        task_unlock(current);
        tvm_sig_leave();
        return NULL;
    }

    struct file *file = file_for_task_locked(current, fd);
    if (!file) {
        task_filestreamdealloc_locked(current, fs);
        task_unlock(current);
        tvm_sig_leave();
        return NULL;
    }

    uint64_t gencnt = file->f_gencnt;

    FILE *fp = CALL_FUNC(fdopen, file->f_rfd, mode);
    if (!fp) {
        task_filestreamdealloc_locked(current, fs);
        task_unlock(current);
        file_unlock(file, 1);
        tvm_sig_leave();
        return NULL;
    }

    task_unlock(current);
    file_unlock(file, 1);

    fs->fs_nfd = fd;
    fs->fs_gencnt = gencnt;
    
    memset(fs->fs_mode, 0, sizeof(fs->fs_mode));
    strncpy(fs->fs_mode, mode, sizeof(fs->fs_mode)-1);

    fs->fs_type = FS_FILE;
    fs->fs_fp = fp;
    
    tvm_sig_leave();

    return (FILE *)fs;
}

/**
 * popen(3)/pclose(3): a pipe to `command` run by the guest shell (TVM_SHELL),
 * the way system() runs it. These were not interposed: the host libc popen
 * spawned a real host process outside the VM and returned a host FILE on an
 * fd this task does not own, which is where toybox awk's `"cmd" | getline`
 * and `print | "cmd"` crashed.
 */
FUNCTION_DEFINE2(FILE *, popen, const char *, command, const char *, mode) {
    int reading;
    if (mode && mode[0] == 'r')
        reading = 1;
    else if (mode && mode[0] == 'w')
        reading = 0;
    else {
        errno = EINVAL;
        return NULL;
    }

    const char *sh = getenv("TVM_SHELL");
    if (!sh || !*sh)
        sh = "/bin/sh";

    // The child must not read `command` from the caller's memory. TVM's
    // fork() copies the arena, the image maps and this thread's host stack
    // for the child and translates the pointers it finds in those copies,
    // but the child's `command` came through untranslated: it still named
    // the parent's buffer (same address on both sides), and popen's parent,
    // unlike system()'s, does not wait -- it is back in the program by the
    // time the child reaches execl(), which copies the string only then.
    // sqlite3 hands popen the command out of its input-line buffer and goes
    // on to read the next line into it, so the child's `sh -c` ran a
    // fragment of whatever sqlite3 had put there since ("sh: line 1:
    // $'\037': command not found", exit 127) and `.once |cmd`, `.output
    // |cmd` and `.import "|cmd"` lost their pipe about every other run. So
    // the parent copies the command into host memory before forking --
    // nothing but the child ever touches that copy -- and the child moves
    // it onto its own (private) stack and frees it before exec; the parent
    // releases it only when there is no child.
    size_t cmdlen = strlen(command) + 1;
    char *cmdcopy = kalloc(cmdlen);
    if (!cmdcopy) {
        errno = ENOMEM;
        return NULL;
    }
    memcpy(cmdcopy, command, cmdlen);

    int fds[2];
    if (pipe(fds) == -1) {
        int last_errno = errno;
        kfree(cmdcopy);
        errno = last_errno;
        return NULL;
    }
    int parent_end = reading ? fds[0] : fds[1];
    int child_end  = reading ? fds[1] : fds[0];

    // Our end is close-on-exec, as with the host libc (glibc uses
    // pipe2(O_CLOEXEC); POSIX requires a popen() child to close the streams
    // of earlier popen() calls). Otherwise every later popen child held this
    // pipe open: awk's `print | "sort"; print | "sort -r"; close("sort")`
    // hung, sort never saw EOF because `sort -r`'s shell still had the write
    // end, and pclose() waited on it forever.
    (void)fcntl(parent_end, F_SETFD, FD_CLOEXEC);

    pid_t pid = fork();
    if (pid == -1) {
        int last_errno = errno;
        close(fds[0]);
        close(fds[1]);
        kfree(cmdcopy);
        errno = last_errno;
        return NULL;
    }
    if (pid == 0) {
        char *cmd = alloca(cmdlen);
        memcpy(cmd, cmdcopy, cmdlen);
        kfree(cmdcopy);
        close(parent_end);
        int target = reading ? STDOUT_FILENO : STDIN_FILENO;
        // with 0 and 1 closed, pipe() can hand out the target itself; dup2
        // is then a no-op and the close would remove the only descriptor
        if (child_end != target) {
            if (dup2(child_end, target) == -1)
                _exit(127);
            close(child_end);
        }
        execl(sh, "sh", "-c", cmd, (char *)NULL);
        _exit(127);
    }

    close(child_end);
    FILE *fp = fdopen(parent_end, reading ? "r" : "w");
    if (!fp) {
        int last_errno = errno;
        close(parent_end);
        int status;
        while (waitpid(pid, &status, 0) == -1 && errno == EINTR)
            ;
        errno = last_errno;
        return NULL;
    }
    ((struct file_stream *)fp)->fs_pid = pid;
    return fp;
}

FUNCTION_DEFINE1(int, pclose, FILE *, stream) {
    struct file_stream *fs = (struct file_stream *)stream;
    if (!fs || fs->fs_type != FS_FILE || fs->fs_pid <= 0) {
        errno = EBADF;
        return -1;
    }
    pid_t pid = fs->fs_pid;
    fclose(stream);

    int status;
    while (waitpid(pid, &status, 0) == -1) {
        if (errno != EINTR)
            return -1;
    }
    return status;
}

FUNCTION_DEFINE1(int, fclose, FILE *, stream) {
    if (!stream)
        return EOF;

    struct file_stream *fs = (struct file_stream *)stream;

    tvm_sig_enter();
    task_lock(current);

    // fclose(stdout): the guest names libc's own stdout object, which every
    // other stdio interposer translates to the task's stream
    // (file_stream_fp). fclose() took the pointer verbatim and handed libc's
    // FILE to task_filestreamdealloc_locked(), which freed it from the task
    // arena: rmem's "invalid pointer" abort, and, abort() needing the task
    // lock fclose still held, a hang rather than a crash. git closes stdout
    // at the end of every built-in command (run_builtin in git.c, to report
    // ENOSPC/EIO on a file) -- `git add` never returned. The standard
    // streams are not freed, as libc does not free its static ones either:
    // the descriptor is closed and the stream detached from it (fd -1 on
    // the libc FILE, fs_nfd -1), so later use fails with EBADF instead of
    // reaching a descriptor the number may have been reused for. A fork
    // after that gives the child its own detached FILE
    // (task_filestreamrealloc_locked).
    int std = (stream == stdin) ? 0 : (stream == stdout) ? 1 : (stream == stderr) ? 2 : -1;
    if (std >= 0) {
        fs = current->tsk_io_filestreams[std];
        if (!fs || fs->fs_nfd < 0) {
            task_unlock(current);
            tvm_sig_leave();
            errno = EBADF;
            return EOF;
        }
        int nfd = fs->fs_nfd;
        int ret = 0, last_errno = errno;
        if (fs->fs_fp) {
            // Flush through file_for_task_stream_locked(), which re-syncs the
            // libc FILE's descriptor first: after dup2() over the stream's fd
            // the FILE still names the previous host descriptor, by now closed
            // or reused by another task, and the buffered output went there
            // (printf("x"); dup2(fd, 1); fclose(stdout) left the file empty).
            struct file *file = file_for_task_stream_locked(current, fs, FS_FILE);
            if (file) {
                if (CALL_FUNC(fflush, fs->fs_fp) == EOF) {
                    ret = EOF;
                    last_errno = errno;
                }
                file_unlock(file, 1);
            }
            fp_setfileno(fs->fs_fp, -1);
        }
        fs->fs_nfd = -1;
        task_unlock(current);
        tvm_sig_leave();
        errno = last_errno;
        if (ret == EOF)
            epipe_signal();
        if (close(nfd) == -1)
            return EOF;
        errno = last_errno;
        return ret;
    }

    int nfd = fs->fs_nfd;
    // The fclose-time flush failed: EOF with its errno, like libc's fclose
    // (this returned close()'s 0 and the caller never learned of the lost
    // output, e.g. EPIPE with SIGPIPE ignored).
    int ret = task_filestreamdealloc_locked(current, fs);
    int flush_errno = errno;
    task_unlock(current);
    tvm_sig_leave();

    if (close(nfd) == -1)
        return EOF;
    if (ret == EOF) {
        errno = flush_errno;
        return EOF;
    }
    return 0;
}

FUNCTION_DEFINE3(FILE *, freopen, const char *, pathname, const char *, mode, FILE *, stream)
{
    // not yet supported
    if (!pathname)
        return NULL;

    int flags;
    if (fmode_to_flags(mode, &flags))
        return NULL;

    // stdin/stdout/stderr name libc's own objects: translate to the task's
    // stream like every other stdio interposer (freopen("log", "w", stdout)
    // used libc's static FILE as a file_stream: a garbage fs_fp and an
    // arbitrary descriptor closed).
    struct file_stream *fs = file_stream_fp(stream);
    if (!fs) {
        errno = EBADF;
        return NULL;
    }
    int std = (stream == stdin || stream == stdout || stream == stderr);

    // Flush through the interposed fflush with the caller's stream, which
    // translates it and re-syncs the libc FILE's descriptor first. This
    // passed fs->fs_fp -- the libc FILE, which the interposed fflush does not
    // know -- so the buffered output of every stream given to freopen was
    // silently dropped.
    if (fs->fs_fp && fs->fs_nfd >= 0)
        (void)fflush(stream);

    if (fs->fs_nfd >= 0)
        close(fs->fs_nfd);
    fs->fs_nfd = -1;

    int fd = open(pathname, flags, 0666);

    tvm_sig_enter();

    task_lock(current);

    if (fs->fs_fp) {
        fp_setfileno(fs->fs_fp, -1);
        CALL_FUNC(fclose, fs->fs_fp);
    }
    fs->fs_fp = NULL;

    if (-1 == fd) {
        task_unlock(current);
        tvm_sig_leave();
        return NULL;
    }

    struct file *file = file_for_task_locked(current, fd);
    if (!file) {
        task_unlock(current);
        tvm_sig_leave();
        close(fd);
        return NULL;
    }

    uint64_t gencnt = file->f_gencnt;

    FILE *fp = CALL_FUNC(fdopen, file->f_rfd, mode);
    if (!fp) {
        task_unlock(current);
        file_unlock(file, 1);
        tvm_sig_leave();
        close(fd);
        return NULL;
    }

    task_unlock(current);
    file_unlock(file, 1);

    fs->fs_nfd = fd;
    fs->fs_gencnt = gencnt;
    
    memset(fs->fs_mode, 0, sizeof(fs->fs_mode));
    strncpy(fs->fs_mode, mode, sizeof(fs->fs_mode)-1);

    fs->fs_type = FS_FILE;
    fs->fs_fp = fp;

    tvm_sig_leave();

    // a standard stream keeps its identity (callers compare with stdout)
    return std ? stream : (FILE *)fs;
}

FUNCTION_DEFINE4(size_t, fwrite, const void *, ptr, size_t, size, size_t, nmemb, FILE *, stream) {
    size_t ret;

    tvm_sig_enter();

    struct file *file = file_for_task_fp(current, stream);
    if (!file) {
        tvm_sig_leave();
        return 0;
    }

    if (!file->f_ops || !file->f_ops->f_write) {
        file_unlock(file, 0);
        ret = CALL_FUNC(fwrite, ptr, size, nmemb, real_fp(stream));
        file_lock(file, 0);
        file_unlock(file, 1);
        tvm_sig_leave();
        if (ret < nmemb)
            epipe_signal();
        return ret;
    }
    
    if (!size) {
        file_unlock(file, 1);
        tvm_sig_leave();
        return 0;
    }
    
    ssize_t r = file->f_ops->f_write(file, ptr, size * nmemb, -1);
    if (r < 0)
        ret = 0;
    else
        ret = (size_t)r;
    
    file_unlock(file, 1);
    tvm_sig_leave();
    return ret / size;
}

// Host stdio reads, restarted the way read() is. TVM delivers every guest
// signal as a real signal to the task's thread -- a child's exit arrives as
// SIGCHLD through pthread_kill whatever the guest's disposition -- and its
// OS-level handlers are installed without SA_RESTART, so that read() sees
// EINTR and can hand it to a guest whose handler asks for it. The read the
// host libc does on the real FILE behind a guest stream is interrupted just
// the same, but libc turns that into EOF with the error flag set and errno
// EINTR, and the interposers passed the EOF on. So a program reading a pipe
// through stdio lost the rest of its input whenever a child exited meanwhile:
// sqlite3's `.import "|cmd"` said "empty file" and a script piped into
// sqlite3 stopped at its first `.shell` line, about every other run (toybox
// awk's `"cmd" | getline` failed with "i/o error 4" the same way). Same
// policy as read(): retry unless a guest handler ran and was installed
// without SA_RESTART. Callers clear tvm_sig_interrupted before the host call
// and make it under tvm_sig_enter/leave, where a deferred signal is
// dispatched and tvm_sig_interrupted set; they pass the errno the call left.
static int stdio_host_read_interrupted(FILE *real, int call_errno)
{
    if (call_errno != EINTR || !CALL_FUNC(ferror, real))
        return 0;
    if (tvm_sig_interrupted &&
        !(current->tsk_sighandlers[tvm_sig_interrupted].sa_flags & SA_RESTART))
        return 0;
    CALL_FUNC(clearerr, real);
    return 1;
}

FUNCTION_DEFINE4(size_t, fread, void *, ptr, size_t, size, size_t, nmemb, FILE *, stream) {
    size_t ret;
    size_t done = 0;

retry:
    tvm_sig_interrupted = 0;
    tvm_sig_enter();
    struct file *file = file_for_task_fp(current, stream);
    if (!file) {
        tvm_sig_leave();
        return done;
    }

    if (!file->f_ops || !file->f_ops->f_read) {
        file_unlock(file, 0);
        ret = CALL_FUNC(fread, ptr, size, nmemb, real_fp(stream));
        int last_errno = errno;
        file_lock(file, 0);
        file_unlock(file, 1);
        tvm_sig_leave();
        errno = last_errno;
        // Interrupted after `ret` whole items: they are in ptr, continue
        // behind them (bytes of a partial item are libc's loss, as in libc).
        if (size && ret < nmemb && stdio_host_read_interrupted(real_fp(stream), last_errno)) {
            done += ret;
            ptr = (char *)ptr + ret * size;
            nmemb -= ret;
            goto retry;
        }
        return done + ret;
    }
    
    if (!size) {
        file_unlock(file, 1);
        tvm_sig_leave();
        return 0;
    }
    
    ssize_t r = file->f_ops->f_read(file, ptr, size * nmemb, -1);
    if (r < 0)
        ret = 0;
    else
        ret = (size_t)r;
    
    file_unlock(file, 1);
    tvm_sig_leave();
    return ret / size;
}

FUNCTION_DEFINE3(int, fseek, FILE *, stream, long, offset, int, whence) {
    tvm_sig_enter();
    struct file *file = file_for_task_fp(current, stream);
    if (!file) {
        tvm_sig_leave();
        return -1;
    }

    int ret;
    if (file->f_ops && file->f_ops->f_lseek) {
        ret = file->f_ops->f_lseek(file, offset, whence) < 0 ? -1 : 0;
    } else {
        file_unlock(file, 0);
        ret = CALL_FUNC(fseek, real_fp(stream), offset, whence);
        file_lock(file, 0);
    }
    file_unlock(file, 1);
    tvm_sig_leave();
    return ret;
}

FUNCTION_DEFINE1(long, ftell, FILE *, stream) {
    tvm_sig_enter();
    struct file *file = file_for_task_fp(current, stream);
    if (!file) {
        tvm_sig_leave();
        return -1;
    }

    long ret;
    if (file->f_ops && file->f_ops->f_lseek) {
        ret = (long)file->f_ops->f_lseek(file, 0, SEEK_CUR);
    } else {
        file_unlock(file, 0);
        ret = CALL_FUNC(ftell, real_fp(stream));
        file_lock(file, 0);
    }
    file_unlock(file, 1);
    tvm_sig_leave();
    return ret;
}

FUNCTION_DEFINE3(int, fseeko, FILE *, stream, off_t, offset, int, whence) {
    return fseek(stream, (long)offset, whence);
}

FUNCTION_DEFINE1(off_t, ftello, FILE *, stream) {
    return (off_t)ftell(stream);
}

// rewind, for the same reason as every other stdio interposer here: a guest
// FILE is a TVM file_stream, not a libc FILE, and libc's rewind() took it for
// one. Nothing shipped had called it until sqlite3, whose shell slurps a file
// with fseek(SEEK_END)/ftell()/rewind() (`.open --deserialize`, the edit()
// SQL function) and died with SIGSEGV on the rewind. fseek() is interposed,
// so rewind is what C99 says: fseek(stream, 0, SEEK_SET) plus clearerr().
FUNCTION_DEFINE_VOID1(rewind, FILE *, stream) {
    fseek(stream, 0L, SEEK_SET);
    clearerr(stream);
}

// flockfile/ftrylockfile/funlockfile were not interposed either, and libc's
// took the guest FILE for one of its own: flockfile() dereferences the
// libc FILE's lock pointer (fp->_extra) at offset 8 of whatever it is
// handed, so a guest stream, being a TVM file_stream, was a SIGSEGV at
// address 0x8 in flockfile via pthread_mutex_lock. git brackets every
// config file read and strbuf_getwholeline() with flockfile()/funlockfile()
// (config.c, strbuf.c), so `git init`, `git var` and anything that opened a
// repository died at its first config read (nano's read_file() had the same
// crash, at the time sidestepped with ac_cv_func_flockfile=no). The
// lock is taken on the libc FILE behind the stream, which is what every
// other stdio call on it ends up using, so a program that really does share
// a stream between threads still gets the mutual exclusion it asked for. A
// stream with no libc FILE behind it (a cpio VFS file) has nothing to lock.
// The *_unlocked accessors that usually go with these are inline macros over
// the libc FILE's fields on Darwin and cannot be interposed; a program using
// them must be built with them mapped back to the locking functions (see
// third_party/include-shim/tvm_stdio_unlocked.h).
FUNCTION_DEFINE_VOID1(flockfile, FILE *, stream) {
    FILE *fp = real_fp(stream);
    if (fp)
        CALL_FUNC(flockfile, fp);
}

FUNCTION_DEFINE1(int, ftrylockfile, FILE *, stream) {
    FILE *fp = real_fp(stream);
    if (!fp)
        return 0;
    return CALL_FUNC(ftrylockfile, fp);
}

FUNCTION_DEFINE_VOID1(funlockfile, FILE *, stream) {
    FILE *fp = real_fp(stream);
    if (fp)
        CALL_FUNC(funlockfile, fp);
}

FUNCTION_DEFINE1(int, fflush, FILE *, stream) {
    tvm_sig_enter();

    // fflush all open streams
    //
    // - Never block on a stream's lock while holding the task lock: a thread
    //   between flockfile() and funlockfile() needs the task lock for every
    //   stdio call it makes on that stream, so waiting for it here deadlocked
    //   both (exit() calls fflush(NULL)). A stream whose lock is taken is
    //   retried after dropping the task lock, a bounded number of times.
    // - Go through file_for_task_stream_locked(), which re-syncs the libc
    //   FILE's descriptor and skips a stream whose descriptor is gone, rather
    //   than flushing into a stale or reused host descriptor.
    // - Only writable streams: a read-only one (stdin) fails with EBADF,
    //   which overwrote an EPIPE from an earlier stream, so exit() lost the
    //   SIGPIPE it should raise. EPIPE is remembered explicitly.
    if (!stream) {
        int ret = 0, saw_epipe = 0;
        for (int attempt = 0; attempt < 200; attempt++) {
            int busy = 0;
            task_lock(current);
            struct file_stream_list_entry *fsle;
            list_for_each_entry(fsle, &current->tsk_filestreams, fsle_list) {
                struct file_stream *fs = fsle->fsle_stream;
                if (fs->fs_type != FS_FILE || !fs->fs_fp || fs->fs_nfd < 0)
                    continue;
                if (!strpbrk(fs->fs_mode, "wa+"))
                    continue;
                if (attempt > 0 && !(fs->fs_flush_pending))
                    continue;
                if (0 != CALL_FUNC(ftrylockfile, fs->fs_fp)) {
                    fs->fs_flush_pending = 1;
                    busy = 1;
                    continue;
                }
                fs->fs_flush_pending = 0;
                struct file *file = file_for_task_stream_locked(current, fs, FS_FILE);
                if (file) {
                    if (CALL_FUNC(fflush, fs->fs_fp) == EOF) {
                        ret = EOF;
                        if (errno == EPIPE)
                            saw_epipe = 1;
                    }
                    file_unlock(file, 1);
                }
                CALL_FUNC(funlockfile, fs->fs_fp);
            }
            task_unlock(current);
            if (!busy)
                break;
            usleep(1000);
        }
        tvm_sig_leave();
        if (saw_epipe) {
            errno = EPIPE;
            epipe_signal();
        }
        return ret;
    }

    struct file *file = file_for_task_fp(current, stream);
    if (!file) {
        tvm_sig_leave();
        return EOF;
    }

    file_unlock(file, 0);
    int ret = CALL_FUNC(fflush, real_fp(stream));
    file_lock(file, 0);
    file_unlock(file, 1);
    tvm_sig_leave();
    if (ret == EOF)
        epipe_signal();
    return ret;
}

FUNCTION_DEFINE_VOID2(setbuf, FILE *, stream, char *, buf) {
    tvm_sig_enter();

    struct file *file = file_for_task_fp(current, stream);
    if (!file) {
        int last_fd = CALL_FUNC(fileno, real_fp(stream));
        fp_setfileno(real_fp(stream), -1);
        CALL_FUNC(setbuf, real_fp(stream), buf);
        fp_setfileno(real_fp(stream), last_fd);
        tvm_sig_leave();
        return;
    }

    file_unlock(file, 0);
    CALL_FUNC(setbuf, real_fp(stream), buf);
    file_lock(file, 0);
    file_unlock(file, 1);
    tvm_sig_leave();
}

#if defined(__linux__)
FUNCTION_DEFINE_VOID3(setbuffer, FILE *, stream, char *, buf, size_t, size) {
#elif defined(__APPLE__)
FUNCTION_DEFINE_VOID3(setbuffer, FILE *, stream, char *, buf, int, size) {
#endif
    tvm_sig_enter();
    struct file *file = file_for_task_fp(current, stream);
    if (!file) {
        int last_fd = CALL_FUNC(fileno, real_fp(stream));
        fp_setfileno(real_fp(stream), -1);
        CALL_FUNC(setbuffer, real_fp(stream), buf, size);
        fp_setfileno(real_fp(stream), last_fd);
        tvm_sig_leave();
        return;
    }

    file_unlock(file, 0);
    CALL_FUNC(setbuffer, real_fp(stream), buf, size);
    file_lock(file, 0);
    file_unlock(file, 1);
    tvm_sig_leave();
}

#if defined(__linux__)
FUNCTION_DEFINE_VOID1(setlinebuf, FILE *, stream) {
    tvm_sig_enter();

    struct file *file = file_for_task_fp(current, stream);
    if (!file) {
        tvm_sig_leave();
        return;
    }

    file_unlock(file, 0);
    CALL_FUNC(setlinebuf, real_fp(stream));
    file_lock(file, 0);
    file_unlock(file, 1);
    tvm_sig_leave();
}
#elif defined(__APPLE__)
FUNCTION_DEFINE1(int, setlinebuf, FILE *, stream) {
    tvm_sig_enter();

    struct file *file = file_for_task_fp(current, stream);
    if (!file) {
        tvm_sig_leave();
        return -1;
    }

    file_unlock(file, 0);
    int ret = CALL_FUNC(setlinebuf, real_fp(stream));
    file_lock(file, 0);
    file_unlock(file, 1);
    tvm_sig_leave();
    return ret;
}
#endif

FUNCTION_DEFINE4(int, setvbuf, FILE *, stream, char *, buf, int, mode, size_t, size) {
    tvm_sig_enter();

    struct file *file = file_for_task_fp(current, stream);
    if (!file) {
        tvm_sig_leave();
        return -1;
    }

    file_unlock(file, 0);
    int ret = CALL_FUNC(setvbuf, real_fp(stream), buf, mode, size);
    file_lock(file, 0);
    file_unlock(file, 1);
    tvm_sig_leave();
    return ret;
}

FUNCTION_DEFINE2(int, fputc, int, c, FILE *, stream) {
    int ret;

    tvm_sig_enter();

    struct file *file = file_for_task_fp(current, stream);
    if (!file) {
        tvm_sig_leave();
        return EOF;
    }

    if (!file->f_ops || !file->f_ops->f_write) {
        file_unlock(file, 0);
        ret = CALL_FUNC(fputc, c, real_fp(stream));
        file_lock(file, 0);
        file_unlock(file, 1);
        tvm_sig_leave();
        if (ret == EOF)
            epipe_signal();
        return ret;
    }
    
    ssize_t r = file->f_ops->f_write(file, &c, 1, -1);
    file_unlock(file, 1);
    tvm_sig_leave();
    if (r == 1)
        return c;
    return EOF;
}

FUNCTION_DEFINE2(int, fputs, const char *, s, FILE *, stream) {
    size_t n = strlen(s);
    int ret;

    tvm_sig_enter();
    struct file *file = file_for_task_fp(current, stream);
    if (!file) {
        tvm_sig_leave();
        return EOF;
    }

    if (!file->f_ops || !file->f_ops->f_write) {
        file_unlock(file, 0);
        ret = CALL_FUNC(fputs, s, real_fp(stream));
        file_lock(file, 0);
        file_unlock(file, 1);
        tvm_sig_leave();
        if (ret == EOF)
            epipe_signal();
        return ret;
    }
    
    ssize_t r = file->f_ops->f_write(file, s, n, -1);
    file_unlock(file, 1);
    tvm_sig_leave();
    if (r >= 0)
        return (int)r;
    return EOF;
}

FUNCTION_DEFINE2(int, putc, int, c, FILE *, stream) {
    return fputc(c, stream);
}

FUNCTION_DEFINE1(int, putchar, int, c) {
    return fputc(c, stdout);
}

FUNCTION_DEFINE1(int, puts, const char *, s) {
    if (EOF == fputs(s, stdout))
        return EOF;
    return fputc('\n', stdout);
}

FUNCTION_DEFINE1(int, fgetc, FILE *, stream) {
    int ret;
retry:
    tvm_sig_interrupted = 0;
    tvm_sig_enter();
    struct file *file = file_for_task_fp(current, stream);
    if (!file) {
        tvm_sig_leave();
        return EOF;
    }

    if (!file->f_ops || !file->f_ops->f_read) {
        file_unlock(file, 0);
        ret = CALL_FUNC(fgetc, real_fp(stream));
        int last_errno = errno;
        file_lock(file, 0);
        file_unlock(file, 1);
        tvm_sig_leave();
        errno = last_errno;
        if (ret == EOF && stdio_host_read_interrupted(real_fp(stream), last_errno))
            goto retry;
        return ret;
    }
    
    char c;
    ssize_t r = file->f_ops->f_read(file, &c, 1, -1);
    file_unlock(file, 1);
    tvm_sig_leave();
    if (r == 1)
        return c;
    return EOF;
}

FUNCTION_DEFINE3(char *, fgets, char *, s, int, size, FILE *, stream) {
    char *ret;
retry:
    tvm_sig_interrupted = 0;
    tvm_sig_enter();
    struct file *file = file_for_task_fp(current, stream);
    if (!file) {
        tvm_sig_leave();
        return NULL;
    }

    if (!file->f_ops || !file->f_ops->f_read) {
        file_unlock(file, 0);
        ret = CALL_FUNC(fgets, s, size, real_fp(stream));
        int last_errno = errno;
        file_lock(file, 0);
        file_unlock(file, 1);
        tvm_sig_leave();
        errno = last_errno;
        // Interrupted before any character: retry (with characters already
        // in s, libc returns the partial line and the next call goes on).
        if (!ret && stdio_host_read_interrupted(real_fp(stream), last_errno))
            goto retry;
        return ret;
    }
    
    if (size <= 0) {
        file_unlock(file, 1);
        tvm_sig_leave();
        return NULL;
    }

    if (size == 1) {
        file_unlock(file, 1);
        tvm_sig_leave();
        s[0] = 0;
        return s;
    }
    
    int err = 0;
    int i;
    char c = 0;
    for (i = 0; i < (size - 1) && c != '\n'; i++)
    {
        ssize_t r = file->f_ops->f_read(file, &c, 1, -1);
        if (r == -1)
            err = 1;
        if (1 != r)
            break;
        
        s[i] = c;
    }
    
    file_unlock(file, 1);
    tvm_sig_leave();

    if (i == 0)
        return NULL;

    s[i] = 0;
    return s;
}

FUNCTION_DEFINE1(int, getc, FILE *, stream) {
    return fgetc(stream);
}

FUNCTION_DEFINE0(int, getchar) {
    return fgetc(stdin);
}

FUNCTION_DEFINE2(int, ungetc, int, c, FILE *, stream) {
    // TODO support for file->f_ops
    return CALL_FUNC(ungetc, c, real_fp(stream));
}

FUNCTION_DEFINE_VOID1(clearerr, FILE *, stream) {
    CALL_FUNC(clearerr, real_fp(stream));
}

FUNCTION_DEFINE1(int, feof, FILE *, stream) {
    return CALL_FUNC(feof, real_fp(stream));
}

FUNCTION_DEFINE1(int, ferror, FILE *, stream) {
    return CALL_FUNC(ferror, real_fp(stream));
}

FUNCTION_DEFINE1(int, fileno, FILE *, stream) {
    return file_stream_fp(stream)->fs_nfd;
}

FUNCTION_DEFINE_VA1(int, printf, const char *, format) {
    return vfprintf(stdout, format, __ap);
}

FUNCTION_DEFINE_VA2(int, fprintf, FILE *, stream, const char *, format) {
    return vfprintf(stream, format, __ap);
}

FUNCTION_DEFINE_VA2(int, dprintf, int, fd, const char *, format) {
    return vdprintf(fd, format, __ap);
}

FUNCTION_DEFINE2(int, vprintf, const char *, format, va_list, ap) {
    return vfprintf(stdout, format, ap);
}

FUNCTION_DEFINE3(int, vfprintf, FILE *, stream, const char *, format, va_list, ap) {
    int ret;
    tvm_sig_enter();
    struct file *file = file_for_task_fp(current, stream);
    if (!file) {
        tvm_sig_leave();
        return -1;
    }

    if (!file->f_ops || !file->f_ops->f_write) {
        file_unlock(file, 0);
        ret = CALL_FUNC(vfprintf, real_fp(stream), format, ap);
        file_lock(file, 0);
        file_unlock(file, 1);
        tvm_sig_leave();
        if (ret < 0)
            epipe_signal();
        return ret;
    }

    file_unlock(file, 1);
    tvm_sig_leave();

    return vdprintf(fileno(stream), format, ap);
}

FUNCTION_DEFINE3(int, vdprintf, int, fd, const char *, format, va_list, ap) {
    tvm_sig_enter();
    struct file *file = file_for_task(current, fd);
    if (!file) {
        tvm_sig_leave();
        errno = EBADF;
        return -1;
    }

    int ret = -1;
    if (!file->f_ops || !file->f_ops->f_write) {
        file_unlock(file, 0);
        ret = CALL_FUNC(vdprintf, file->f_rfd, format, ap);
        if (ret < 0)
            epipe_signal_fd(fd, 0);
        file_lock(file, 0);
    }
    else {
        va_list ap2;
        va_copy(ap2, ap);
        int size = vsnprintf(NULL, 0, format, ap2);
        va_end(ap2);

        if (size < 0) {
            ret = size;
            goto out;
        }
        
        char *s = malloc(size+1);
        if (!s) {
            ret = -ENOMEM;
            goto out;
        }
        
        int wsize = vsnprintf(s, size+1, format, ap);
        ret = (int)file->f_ops->f_write(file, s, wsize, -1);
        free(s);
    }
    
out:
    file_unlock(file, 1);
    tvm_sig_leave();

    return ret;
}

FUNCTION_DEFINE_VA1(int, scanf, const char *, format) {
    return vfscanf(stdin, format, __ap);
}

FUNCTION_DEFINE_VA2(int, fscanf, FILE *, stream, const char *, format) {
    return vfscanf(stream, format, __ap);
}

FUNCTION_DEFINE2(int, vscanf, const char *, format, va_list, ap) {
    return vfscanf(stdin, format, ap);
}

FUNCTION_DEFINE3(int, vfscanf, FILE *, stream, const char *, format, va_list, ap) {
    tvm_sig_enter();
    struct file *file = file_for_task_fp(current, stream);
    if (!file) {
        tvm_sig_leave();
        return EOF;
    }

    file_unlock(file, 0);
    int ret = CALL_FUNC(vfscanf, real_fp(stream), format, ap);
    file_lock(file, 0);
    file_unlock(file, 1);
    tvm_sig_leave();
    return ret;
}

FUNCTION_DEFINE_VOID1(perror, const char *, s) {
    fprintf(stderr, "%s: %s\n", s, strerror(errno));
}

FUNCTION_DEFINE_VA2(int, open, const char *, pathname, int, flags) {
    mode_t mode = va_arg(__ap, int);
    if (!can_enter_tvm(NULL))
        return CALL_FUNC(open, pathname, flags, mode);

    return openat(AT_FDCWD, pathname, flags, mode);
}

FUNCTION_DEFINE2(int, creat, const char *, pathname, mode_t, mode) {
    return open(pathname, O_CREAT | O_WRONLY | O_TRUNC, mode);
}

FUNCTION_DEFINE_VA3(int, openat, int, dirfd, const char *, pathname, int, flags) {
    mode_t mode = va_arg(__ap, int);
    if (!can_enter_tvm(NULL))
        return CALL_FUNC(openat, dirfd, pathname, flags, mode);

    if (flags & O_CREAT)
        mode &= ~current->tsk_umask;

    char pathname_abs[PATH_MAX];
    if (dirfd == AT_FDCWD) {
        if (!normalize_path(NULL, pathname, pathname_abs))
            return -1;
        pathname = pathname_abs;
    }

    tvm_sig_enter();
    // __FILE's name is read without its lock: f_pathname is written once at
    // open and freed only with the file, on which a reference is held here.
    struct file *file = (void *)CURRENT_RFD(dirfd, (long)file_open(__rfd, __FILE ? __FILE->f_pathname : NULL, pathname, flags, mode, getuid(), getgid()));
    if (!file) {
        tvm_sig_leave();
        return -1;
    }

    task_lock(current);
    int fd;
    if (-1 == (fd = task_assign_file_locked(current, file, 0))) {
        task_unlock(current);
        file_unlock(file, 1);
        tvm_sig_leave();
        errno = EMFILE;
        return -1;
    }

    task_unlock(current);
    file_unlock(file, 1);
    tvm_sig_leave();

    return fd;
}

FUNCTION_DEFINE3(ssize_t, read, int, fd, void *, buf, size_t, count) {
    tvm_sig_interrupted = 0;
retry:
    tvm_sig_enter();
    struct file *file = file_for_task(current, fd);
    if (!file) {
        tvm_sig_leave();
        errno = EBADF;
        return -1;
    }

    size_t ret;
    if (!file->f_ops || !file->f_ops->f_read) {
        int rfd = file->f_rfd;
        file_unlock(file, 0);
        ret = CALL_FUNC(read, rfd, buf, count);
        file_lock(file, 0);
    }
    else
        ret = file->f_ops->f_read(file, buf, count, -1);

    file_unlock(file, 1);
    tvm_sig_leave();

    if (ret == (size_t)-1 && errno == EINTR) {
        if (!tvm_sig_interrupted)
            goto retry; // signal was handled internally (e.g. IGN'd), guest never sees it
        if (current->tsk_sighandlers[tvm_sig_interrupted].sa_flags & SA_RESTART)
            goto retry;
    }

    return ret;
}

/**
 * epipe_signal() for a write on the calling task's descriptor `nfd`, unless
 * the program opted out: MSG_NOSIGNAL in `send_flags`, or SO_NOSIGPIPE /
 * F_SETNOSIGPIPE on the descriptor. rfd_adopt() sets no-SIGPIPE on every host
 * descriptor, so the kernel's copy of that option says nothing about what the
 * program asked for; raising SIGPIPE regardless killed programs that rely on
 * the opt-out instead of ignoring the signal -- libcurl on Darwin (and so
 * curl and git-remote-https) uses SO_NOSIGPIPE and installs no handler, so a
 * server resetting a kept-alive connection killed it with status 141.
 */
static void epipe_signal_fd(int nfd, int send_flags)
{
#ifdef F_SETNOSIGPIPE
    if (errno != EPIPE)
        return;
#ifdef MSG_NOSIGNAL
    if (send_flags & MSG_NOSIGNAL)
        return;
#endif
    int last_errno = errno;
    int optout = 0;
    // Under tvm_sig_enter like every other task_lock site: write() and the
    // socket sends call this after their own tvm_sig_leave(), and a signal
    // dispatched while this thread holds the task lock (a plain, non-recursive
    // mutex) takes it again in signal_dispatch and deadlocks the thread.
    tvm_sig_enter();
    struct file *file = file_for_task(current, nfd);
    if (file) {
        optout = file->f_nosigpipe;
        file_unlock(file, 1);
    }
    tvm_sig_leave();
    errno = last_errno;
    if (!optout)
        epipe_signal();
#else
    (void)nfd; (void)send_flags;
#endif
}

FUNCTION_DEFINE3(ssize_t, write, int, fd, const void *, buf, size_t, count) {
    tvm_sig_interrupted = 0;
retry:
    tvm_sig_enter();
    struct file *file = file_for_task(current, fd);
    if (!file) {
        tvm_sig_leave();
        errno = EBADF;
        return -1;
    }

    size_t ret;
    if (!file->f_ops || !file->f_ops->f_write) {
        int wfd = file->f_rfd;
        file_unlock(file, 0);
        ret = CALL_FUNC(write, wfd, buf, count);
        file_lock(file, 0);
    }
    else
        ret = file->f_ops->f_write(file, buf, count, -1);

    file_unlock(file, 1);
    tvm_sig_leave();

    if (ret == (size_t)-1 && errno == EINTR) {
        if (!tvm_sig_interrupted)
            goto retry;
        if (current->tsk_sighandlers[tvm_sig_interrupted].sa_flags & SA_RESTART)
            goto retry;
    }

    if (ret == (size_t)-1)
        epipe_signal_fd(fd, 0);

    return ret;
}

FUNCTION_DEFINE1(int, close, int, fd) {
    tvm_sig_enter();

    task_lock(current);

    // detach from current before closing
    // inc refcnt for locking, dec refcnt for detaching
    struct file *file = task_unassign_fd_locked(current, fd);
    if (!file) {
        task_unlock(current);
        tvm_sig_leave();
        errno = EBADF;
        return -1;
    }

    // POSIX record locks: closing any descriptor of a file drops the task's
    // locks on that file, dup'd descriptors included (see fcntl_rlock).
    // tsk_rlocks is only a hint that spares tasks holding none the fstat.
    if (current->tsk_rlocks && !file->f_ops && file->f_rfd >= 0)
        rlock_release(current, file->f_rfd);

    // this order is extremely important!! file deallocation may lock current
    task_unlock(current);
    // lowest refcnt possible is 1, so may dealloc
    file_unlock(file, 1);
    
    tvm_sig_leave();
    return 0;
}

FUNCTION_DEFINE3(ssize_t, readv, int, fd, const struct iovec *, iov, int, iovcnt) {
    tvm_sig_enter();
    struct file *file = file_for_task(current, fd);
    if (!file) {
        tvm_sig_leave();
        errno = EBADF;
        return -1;
    }

    size_t ret;
    if (!file->f_ops || !file->f_ops->f_read) {
        file_unlock(file, 0);
        ret = CALL_FUNC(readv, file->f_rfd, iov, iovcnt);
        file_lock(file, 0);
    }
    else {
        // TODO: scatter-gather needs to be "atomic", maybe IO-lock per fops or per device instance?
        ret = 0;
        for (int i = 0; i < iovcnt; i++) {
            ssize_t curr = file->f_ops->f_read(file, iov[i].iov_base, iov[i].iov_len, -1);
            if (curr == 0)
                break;
            if (curr < 0) {
                if (ret == 0) {
                    file_unlock(file, 1);
                    tvm_sig_leave();
                    return -1;
                }
                break;
            }

            ret += curr;

            if (curr < iov[i].iov_len)
                break;
        }
    }
    
    file_unlock(file, 1);
    tvm_sig_leave();
    
    return ret;
}

FUNCTION_DEFINE3(ssize_t, writev, int, fd, const struct iovec *, iov, int, iovcnt) {
    tvm_sig_enter();
    struct file *file = file_for_task(current, fd);
    if (!file) {
        tvm_sig_leave();
        errno = EBADF;
        return -1;
    }

    size_t ret;
    if (!file->f_ops || !file->f_ops->f_write) {
        file_unlock(file, 0);
        ret = CALL_FUNC(writev, file->f_rfd, iov, iovcnt);
        file_lock(file, 0);
    }
    else {
        // TODO: see readv()
        ret = 0;
        for (int i = 0; i < iovcnt; i++) {
            ssize_t curr = file->f_ops->f_write(file, iov[i].iov_base, iov[i].iov_len, -1);
            if (curr == 0)
                break;
            if (curr < 0) {
                if (ret == 0) {
                    file_unlock(file, 1);
                    tvm_sig_leave();
                    epipe_signal_fd(fd, 0);
                    return -1;
                }
                break;
            }

            ret += curr;

            if (curr < iov[i].iov_len)
                break;
        }
    }
    
    file_unlock(file, 1);
    tvm_sig_leave();

    if (ret == (size_t)-1)
        epipe_signal_fd(fd, 0);

    return ret;
}

FUNCTION_DEFINE4(ssize_t, pread, int, fd, void *, buf, size_t, count, off_t, offset) {
    tvm_sig_interrupted = 0;
retry:
    tvm_sig_enter();
    struct file *file = file_for_task(current, fd);
    if (!file) {
        tvm_sig_leave();
        errno = EBADF;
        return -1;
    }

    size_t ret;
    if (!file->f_ops || !file->f_ops->f_read) {
        int rfd = file->f_rfd;
        file_unlock(file, 0);
        ret = CALL_FUNC(pread, rfd, buf, count, offset);
        file_lock(file, 0);
    }
    else
        ret = file->f_ops->f_read(file, buf, count, offset);

    file_unlock(file, 1);
    tvm_sig_leave();

    if (ret == (size_t)-1 && errno == EINTR) {
        if (!tvm_sig_interrupted)
            goto retry; // signal was handled internally (e.g. IGN'd), guest never sees it
        if (current->tsk_sighandlers[tvm_sig_interrupted].sa_flags & SA_RESTART)
            goto retry;
    }

    return ret;
}

FUNCTION_DEFINE4(ssize_t, pwrite, int, fd, const void *, buf, size_t, count, off_t, offset) {
    tvm_sig_interrupted = 0;
retry:
    tvm_sig_enter();
    struct file *file = file_for_task(current, fd);
    if (!file) {
        tvm_sig_leave();
        errno = EBADF;
        return -1;
    }

    size_t ret;
    if (!file->f_ops || !file->f_ops->f_write) {
        int wfd = file->f_rfd;
        file_unlock(file, 0);
        ret = CALL_FUNC(pwrite, wfd, buf, count, offset);
        file_lock(file, 0);
    }
    else
        ret = file->f_ops->f_write(file, buf, count, offset);

    file_unlock(file, 1);
    tvm_sig_leave();

    if (ret == (size_t)-1 && errno == EINTR) {
        if (!tvm_sig_interrupted)
            goto retry;
        if (current->tsk_sighandlers[tvm_sig_interrupted].sa_flags & SA_RESTART)
            goto retry;
    }

    if (ret == (size_t)-1)
        epipe_signal_fd(fd, 0);

    return ret;
}

FUNCTION_DEFINE4(ssize_t, preadv, int, fd, const struct iovec *, iov, int, iovcnt, off_t, offset) {
    tvm_sig_enter();
    struct file *file = file_for_task(current, fd);
    if (!file) {
        tvm_sig_leave();
        errno = EBADF;
        return -1;
    }

    size_t ret;
    if (!file->f_ops || !file->f_ops->f_read) {
        file_unlock(file, 0);
        ret = CALL_FUNC(preadv, file->f_rfd, iov, iovcnt, offset);
        file_lock(file, 0);
    }
    else {
        // TODO: see readv()
        ret = 0;
        for (int i = 0; i < iovcnt; i++) {
            ssize_t curr = file->f_ops->f_read(file, iov[i].iov_base, iov[i].iov_len, offset + ret);
            if (curr == 0)
                break;
            if (curr < 0) {
                if (ret == 0) {
                    file_unlock(file, 1);
                    tvm_sig_leave();
                    return -1;
                }
                break;
            }

            ret += curr;

            if (curr < iov[i].iov_len)
                break;
        }
    }

    file_unlock(file, 1);
    tvm_sig_leave();

    return ret;
}

FUNCTION_DEFINE4(ssize_t, pwritev, int, fd, const struct iovec *, iov, int, iovcnt, off_t, offset) {
    tvm_sig_enter();
    struct file *file = file_for_task(current, fd);
    if (!file) {
        tvm_sig_leave();
        errno = EBADF;
        return -1;
    }

    size_t ret;
    if (!file->f_ops || !file->f_ops->f_write) {
        file_unlock(file, 0);
        ret = CALL_FUNC(pwritev, file->f_rfd, iov, iovcnt, offset);
        file_lock(file, 0);
    }
    else {
        // TODO: see readv()
        ret = 0;
        for (int i = 0; i < iovcnt; i++) {
            ssize_t curr = file->f_ops->f_write(file, iov[i].iov_base, iov[i].iov_len, offset + ret);
            if (curr == 0)
                break;
            if (curr < 0) {
                if (ret == 0) {
                    file_unlock(file, 1);
                    tvm_sig_leave();
                    epipe_signal_fd(fd, 0);
                    return -1;
                }
                break;
            }

            ret += curr;

            if (curr < iov[i].iov_len)
                break;
        }
    }

    file_unlock(file, 1);
    tvm_sig_leave();

    if (ret == (size_t)-1)
        epipe_signal_fd(fd, 0);

    return ret;
}

FUNCTION_DEFINE3(off_t, lseek, int, fd, off_t, offset, int, whence) {
    tvm_sig_enter();
    struct file *file = file_for_task(current, fd);
    if (!file) {
        tvm_sig_leave();
        errno = EBADF;
        return -1;
    }

    off_t ret;
    if (file->f_ops && file->f_ops->f_lseek)
        ret = file->f_ops->f_lseek(file, offset, whence);
    else
        ret = CALL_FUNC(lseek, file->f_rfd, offset, whence);

    file_unlock(file, 1);
    tvm_sig_leave();
    return ret;
}

FUNCTION_DEFINE1(int, fsync, int, fd) {
    return CURRENT_RFD(fd, CALL_FUNC(fsync, __rfd));
}

#if defined(__linux__)
/*
 * glibc's large-file names. A program built with _FILE_OFFSET_BITS=64
 * (sqlite, git, coreutils-style code) calls pread64/pwrite64/fcntl64/...
 * instead of pread/pwrite/fcntl, and guest symbols bind to interposers by
 * name: these went to the host libc with TVM's task descriptor numbers.
 * sqlite then read and locked whatever host descriptor had that number
 * ("disk I/O error", "no such table"). On x86-64 off_t is already 64-bit,
 * so they are the same functions.
 */
FUNCTION_DEFINE4(ssize_t, pread64, int, fd, void *, buf, size_t, count, off64_t, offset) {
    return __tvm_pread(fd, buf, count, offset);
}

FUNCTION_DEFINE4(ssize_t, pwrite64, int, fd, const void *, buf, size_t, count, off64_t, offset) {
    return __tvm_pwrite(fd, buf, count, offset);
}

FUNCTION_DEFINE4(ssize_t, preadv64, int, fd, const struct iovec *, iov, int, iovcnt, off64_t, offset) {
    return __tvm_preadv(fd, iov, iovcnt, offset);
}

FUNCTION_DEFINE4(ssize_t, pwritev64, int, fd, const struct iovec *, iov, int, iovcnt, off64_t, offset) {
    return __tvm_pwritev(fd, iov, iovcnt, offset);
}

// sqlite on Linux syncs with fdatasync (HAVE_FDATASYNC), not fsync.
FUNCTION_DEFINE1(int, fdatasync, int, fd) {
    return CURRENT_RFD(fd, CALL_FUNC(fdatasync, __rfd));
}
#endif

FUNCTION_DEFINE1(int, dup, int, oldfd) {
    return fcntl(oldfd, F_DUPFD, 0);
}

FUNCTION_DEFINE2(int, dup2, int, oldfd, int, newfd) {
    tvm_sig_enter();

    task_lock(current);
    struct file *oldfile = file_for_task_locked(current, oldfd);
    if (!oldfile) {
        task_unlock(current);
        tvm_sig_leave();
        errno = EBADF;
        return -1;
    }

    // we can't unlock with nonzero ref while task it locked
    // hence the unlock kerfuffle

    struct file *newfile = file_dup_locked(oldfile);
    task_unlock(current);
    file_unlock(oldfile, 1);

    if (!newfile) {
        tvm_sig_leave();
        return -1;
    }

    file_unlock(newfile, 0);
    task_lock(current);
    file_lock(newfile, 0);

    // switch 'em
    struct file *prevnewfile = NULL;
    if (current->tsk_files[newfd])
        prevnewfile = task_unassign_fd_locked(current, newfd);
    VERIFY(newfd == task_assign_file_locked(current, newfile, newfd));

    // dup2 closes newfd first, and closing any descriptor of a file drops the
    // task's record locks on it -- exactly as in close(). Skipping this left
    // the locks in TVM's table after the host descriptor was gone, so every
    // other task got EAGAIN (or waited forever in F_SETLKW) until this task
    // closed another descriptor of the file or exited.
    if (prevnewfile && current->tsk_rlocks && !prevnewfile->f_ops && prevnewfile->f_rfd >= 0)
        rlock_release(current, prevnewfile->f_rfd);

    // this order is extremely important!! file deallocation may lock current
    task_unlock(current);
    file_unlock(newfile, 1);
    if (prevnewfile)
        file_unlock(prevnewfile, 1); // may dealloc
    tvm_sig_leave();
    return newfd;
}

FUNCTION_DEFINE1(int, pipe, int *, pipefd) {

    tvm_sig_enter();

    int r[2], f[2];
    if (0 != CALL_FUNC(pipe, r)) {
        tvm_sig_leave();
        return -1;
    }

    if (-1 == (f[0] = task_new_fd(current, r[0]))) {
        VERIFY(0 == CALL_FUNC(close, r[0]));
        VERIFY(0 == CALL_FUNC(close, r[1]));
        tvm_sig_leave();
        return -1;
    }

    if (-1 == (f[1] = task_new_fd(current, r[1]))) {
        VERIFY(0 == close(f[0]));
        VERIFY(0 == CALL_FUNC(close, r[1]));
        tvm_sig_leave();
        return -1;
    }

    tvm_sig_leave();

    memcpy(pipefd, f, sizeof(f));
    
    return 0;
}

static int tvm_poll_host(struct pollfd *fds, nfds_t nfds, int timeout, const struct timespec *ts, const sigset_t *os_mask);

// Milliseconds for poll() from a timespec; NULL is forever (-1).
static int tvm_timespec_to_ms(const struct timespec *ts)
{
    if (!ts)
        return -1;
    long long ms = (long long)ts->tv_sec * 1000 + (ts->tv_nsec + 999999) / 1000000;
    return ms > INT_MAX ? INT_MAX : (int)ms;
}

// select()/pselect() over the task's descriptors, as a poll(): a host
// descriptor number is not bounded by FD_SETSIZE. `ts` is the timeout (NULL:
// forever); with `os_mask` the wait is pselect's atomic masked one (see
// tvm_poll_host).
static int tvm_select(int nfds, fd_set *readfds, fd_set *writefds, fd_set *exceptfds, const struct timespec *ts, const sigset_t *os_mask) {
    int fdcount = 0;
    struct pollfd *fds = NULL;

    for (int i = 0; i < nfds; i++) {
        struct pollfd currfd = { .fd = i, 0, 0 };

        if (readfds && FD_ISSET(i, readfds))
            currfd.events |= POLLIN;
        if (writefds && FD_ISSET(i, writefds))
            currfd.events |= POLLOUT;
        if (exceptfds && FD_ISSET(i, exceptfds))
            currfd.events |= POLLPRI;
        
        if (!currfd.events)
            continue;
        
        fds = realloc(fds, sizeof(*fds) * (fdcount+1));
        if (!fds) {
            errno = ENOMEM;
            return -1;
        }

        memcpy(fds + fdcount, &currfd, sizeof(currfd));
        fdcount++;
    }

    int r = tvm_poll_host(fds, fdcount, tvm_timespec_to_ms(ts), ts, os_mask);

    if (r == -1) {
        free(fds);
        return -1;
    }

    r = 0;

    if (readfds)
        FD_ZERO(readfds);
    if (writefds)
        FD_ZERO(writefds);
    if (exceptfds)
        FD_ZERO(exceptfds);

    for (int i = 0; i < fdcount; i++) {
        if (fds[i].revents & POLLNVAL) {
            free(fds);
            errno = EBADF;
            return -1;
        }
        if (fds[i].revents & (POLLIN | POLLHUP | POLLERR)) {
            if (readfds) {
                FD_SET(fds[i].fd, readfds);
                r++;
            }
        }
        if (fds[i].revents & (POLLOUT | POLLERR)) {
            if (writefds) {
                FD_SET(fds[i].fd, writefds);
                r++;
            }
        }
        if (fds[i].revents & POLLPRI) {
            if (exceptfds) {
                FD_SET(fds[i].fd, exceptfds);
                r++;
            }
        }
    }

    free(fds);
    return r;
}

FUNCTION_DEFINE5(int, select, int, nfds, fd_set *, readfds, fd_set *, writefds, fd_set *, exceptfds, struct timeval *, timeout) {
    struct timespec ts;
    if (timeout) {
        ts.tv_sec = timeout->tv_sec;
        ts.tv_nsec = (long)timeout->tv_usec * 1000;
    }
    return tvm_select(nfds, readfds, writefds, exceptfds, timeout ? &ts : NULL, NULL);
}

/*
 * The wait behind pselect(): poll() on host descriptors with the OS-level
 * signal mask reopened to `os_mask` for exactly the duration of the wait --
 * ppoll() on Linux, the pselect() syscall on Darwin (which has no ppoll).
 * That syscall takes fd_sets, which cannot hold a host descriptor at
 * FD_SETSIZE or above; for such a set the mask is reopened around a plain
 * poll() instead, and the gap pselect closes stays open for that one call.
 */
static int tvm_poll_host_masked(struct pollfd *realfds, nfds_t nfds, const struct timespec *ts, const sigset_t *os_mask)
{
#if defined(__linux__)
    return CALL_FUNC(ppoll, realfds, nfds, ts, os_mask);
#else
    fd_set hr, hw, he;
    FD_ZERO(&hr);
    FD_ZERO(&hw);
    FD_ZERO(&he);
    int maxfd = -1, toobig = 0;
    for (nfds_t i = 0; i < nfds; i++) {
        int rfd = realfds[i].fd;
        if (rfd < 0)
            continue;
        if (rfd >= FD_SETSIZE) {
            toobig = 1;
            break;
        }
        if (realfds[i].events & POLLIN)
            FD_SET(rfd, &hr);
        if (realfds[i].events & POLLOUT)
            FD_SET(rfd, &hw);
        if (realfds[i].events & POLLPRI)
            FD_SET(rfd, &he);
        if (rfd > maxfd)
            maxfd = rfd;
    }
    if (toobig) {
        sigset_t blocked;
        CALL_FUNC(pthread_sigmask, SIG_SETMASK, os_mask, &blocked);
        int ret = CALL_FUNC(poll, realfds, nfds, tvm_timespec_to_ms(ts));
        int err = errno;
        CALL_FUNC(pthread_sigmask, SIG_SETMASK, &blocked, NULL);
        errno = err;
        return ret;
    }
    int ret = CALL_FUNC(pselect, maxfd + 1, &hr, &hw, &he, ts, os_mask);
    if (ret <= 0)
        return ret;
    ret = 0;
    for (nfds_t i = 0; i < nfds; i++) {
        int rfd = realfds[i].fd;
        realfds[i].revents = 0;
        if (rfd < 0)
            continue;
        if (FD_ISSET(rfd, &hr))
            realfds[i].revents |= POLLIN;
        if (FD_ISSET(rfd, &hw))
            realfds[i].revents |= POLLOUT;
        if (FD_ISSET(rfd, &he))
            realfds[i].revents |= POLLPRI;
        if (realfds[i].revents)
            ret++;
    }
    return ret;
#endif
}

// poll() over the task's descriptors: each translated to the host
// descriptor behind it for the host wait, which is poll() with `timeout`,
// or with `os_mask` the masked wait of tvm_poll_host_masked with `ts`.
static int tvm_poll_host(struct pollfd *fds, nfds_t nfds, int timeout, const struct timespec *ts, const sigset_t *os_mask)
{
    int ret = -1;
    struct pollfd *realfds = malloc(nfds * sizeof(*fds));
    if (!realfds) {
        errno = ENOMEM;
        return ret;
    }
    memcpy(realfds, fds, nfds * sizeof(*fds));

    struct file **files = malloc(nfds * (sizeof(struct file *)));
    if (!files) {
        free(realfds);
        errno = ENOMEM;
        return ret;
    }

    tvm_sig_enter();

    for (int i = 0; i < nfds; i++) {
        files[i] = file_for_task(current, fds[i].fd);
        if (!files[i]) {
            for (int j = 0; j < i; j++) {
                file_lock(files[j], 0);
                file_unlock(files[j], 1);
            }
            tvm_sig_leave();
            free(files);
            free(realfds);
            return ret;
        }
        realfds[i].fd = files[i]->f_rfd;
        file_unlock(files[i], 0);
    }

    if (os_mask)
        ret = tvm_poll_host_masked(realfds, nfds, ts, os_mask);
    else
        ret = CALL_FUNC(poll, realfds, nfds, timeout);
    int err = errno;

    for (int i = 0; i < nfds; i++)
        realfds[i].fd = fds[i].fd;
    memcpy(fds, realfds, nfds * sizeof(*fds));

    for (int i = 0; i < nfds; i++) {
        file_lock(files[i], 0);
        file_unlock(files[i], 1);
    }

    tvm_sig_leave();

    free(files);
    free(realfds);
    errno = err;
    return ret;
}

FUNCTION_DEFINE3(int, poll, struct pollfd *, fds, nfds_t, nfds, int, timeout) {
    return tvm_poll_host(fds, nfds, timeout, NULL, NULL);
}

// A signal pending for this task or thread that the mask `mask` lets through.
static int tvm_sig_pending_unblocked(const sigset_t *mask)
{
    uint32_t pending = __atomic_load_n(&current->tsk_pending_sigs, __ATOMIC_ACQUIRE);
    if (current_thread)
        pending |= __atomic_load_n(&current_thread->thr_pending_sigs, __ATOMIC_ACQUIRE);
    for (int sig = 1; sig < MAX_SIGNALS; sig++)
        if ((pending & (1u << sig)) && !sigismember(mask, sig))
            return 1;
    return 0;
}

/*
 * pselect(2): select() with the caller's signal mask installed for the
 * duration of the wait, atomically -- a signal the mask lets through is
 * either dispatched before the wait begins, so pselect returns EINTR at
 * once, or interrupts the wait; never lost in between. bash used to be
 * configured ac_cv_func_pselect=no (a guest pselect() reaching the host
 * waited on host descriptor numbers) and fell back to sigprocmask() +
 * select(), which has exactly that gap; readline's input timeouts, `read
 * -t` and the shell's timers all go through here now.
 *
 * TVM's signals are virtual: the task's mask (thr_sigmask) and pending bits
 * are TVM's own, and a kill() arrives as an OS signal on the task's thread
 * (pthread_kill), which signal_handler dispatches at once or, inside a
 * tvm_sig_enter() section, parks in the pending bits for the matching
 * tvm_sig_leave(). So the whole call is such a section with the pselect
 * mask installed as the virtual mask, the OS-level mask blocked, and:
 *  - a signal already pending that the mask lets through is answered
 *    without waiting;
 *  - otherwise the host wait reopens the OS-level mask atomically
 *    (tvm_poll_host_masked), so a signal arriving from then on is delivered
 *    inside the wait, parked, and ends it with EINTR;
 *  - a host wait ended by a signal the mask blocks (every OS signal
 *    interrupts the thread, blocked or not; it stays parked) is not pselect's
 *    to report: the wait restarts with the time that is left.
 * The tvm_sig_leave() at the end runs the handlers of whatever was parked
 * with the pselect mask still in effect, as POSIX requires, and the caller's
 * mask is restored last. select() is never restarted after a handler, so
 * SA_RESTART plays no part.
 */
FUNCTION_DEFINE6(int, pselect, int, nfds, fd_set *, readfds, fd_set *, writefds, fd_set *, exceptfds, const struct timespec *, timeout, const sigset_t *, sigmask) {
    if (timeout && (timeout->tv_sec < 0 || timeout->tv_nsec < 0 || timeout->tv_nsec >= 1000000000L)) {
        errno = EINVAL;
        return -1;
    }
    if (!sigmask)
        return tvm_select(nfds, readfds, writefds, exceptfds, timeout, NULL);

    struct timespec deadline;
    if (timeout) {
        clock_gettime(CLOCK_MONOTONIC, &deadline);
        deadline.tv_sec += timeout->tv_sec;
        deadline.tv_nsec += timeout->tv_nsec;
        if (deadline.tv_nsec >= 1000000000L) {
            deadline.tv_sec++;
            deadline.tv_nsec -= 1000000000L;
        }
    }

    tvm_sig_enter();
    sigset_t old = current_thread->thr_sigmask;
    current_thread->thr_sigmask = *sigmask;
    sigdelset(&current_thread->thr_sigmask, SIGKILL);
    sigdelset(&current_thread->thr_sigmask, SIGSTOP);

    sigset_t all, os_old;
    sigfillset(&all);
    CALL_FUNC(pthread_sigmask, SIG_BLOCK, &all, &os_old);

    int r;
    for (;;) {
        if (tvm_sig_pending_unblocked(&current_thread->thr_sigmask)) {
            errno = EINTR;
            r = -1;
            break;
        }
        struct timespec left, *leftp = NULL;
        if (timeout) {
            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            left.tv_sec = deadline.tv_sec - now.tv_sec;
            left.tv_nsec = deadline.tv_nsec - now.tv_nsec;
            if (left.tv_nsec < 0) {
                left.tv_sec--;
                left.tv_nsec += 1000000000L;
            }
            if (left.tv_sec < 0)
                left.tv_sec = left.tv_nsec = 0;
            leftp = &left;
        }
        r = tvm_select(nfds, readfds, writefds, exceptfds, leftp, &os_old);
        if (r == -1 && errno == EINTR && !tvm_sig_pending_unblocked(&current_thread->thr_sigmask))
            continue;
        break;
    }
    int err = errno;

    CALL_FUNC(pthread_sigmask, SIG_SETMASK, &os_old, NULL);
    tvm_sig_leave();
    current_thread->thr_sigmask = old;
    errno = err;
    return r;
}

// fcntl(F_GETLK/F_SETLK/F_SETLKW) on a task's descriptor: the record-lock
// table (see "POSIX record locks" above) decides between tasks, the kernel
// between this process and the outside.
static int fcntl_rlock(int fd, int cmd, struct flock *fl)
{
    if (!fl) {
        errno = EINVAL;
        return -1;
    }

    tvm_sig_enter();
    struct file *file = file_for_task(current, fd);
    if (!file) {
        tvm_sig_leave();
        errno = EBADF;
        return -1;
    }
    int rfd = file->f_rfd;
    int emulate = !file->f_ops;
    file_unlock(file, 0);

    int ret = -1;
    struct stat st;
    off_t start, end;
    if (!emulate || -1 == CALL_FUNC(fstat, rfd, &st) || !S_ISREG(st.st_mode))
        ret = CALL_FUNC(fcntl, rfd, cmd, fl);
    else if (0 == rlock_range(rfd, fl, &start, &end)) {
        if (cmd == F_GETLK) {
            pthread_mutex_lock(&rlocks_lock);
            struct rlock *c = (fl->l_type == F_UNLCK) ? NULL :
                rlock_conflict_locked(st.st_dev, st.st_ino, current, fl->l_type, start, end);
            if (c) {
                fl->l_type = c->rl_type;
                fl->l_whence = SEEK_SET;
                fl->l_start = c->rl_start;
                fl->l_len = (c->rl_end == RLOCK_EOF) ? 0 : c->rl_end - c->rl_start + 1;
                fl->l_pid = c->rl_owner->tsk_pid;
                ret = 0;
            }
            pthread_mutex_unlock(&rlocks_lock);
            if (!c) // no task holds one; a process outside TVM may
                ret = CALL_FUNC(fcntl, rfd, F_GETLK, fl);
        }
        else {
            for (;;) {
                pthread_mutex_lock(&rlocks_lock);
                struct rlock *c = (fl->l_type == F_UNLCK) ? NULL :
                    rlock_conflict_locked(st.st_dev, st.st_ino, current, fl->l_type, start, end);
                if (!c) {
                    // Kernel first -- inside the process it cannot refuse on
                    // account of another task, only of an outside process --
                    // then the table. Always F_SETLK: a kernel F_SETLKW would
                    // sleep with rlocks_lock held.
                    ret = CALL_FUNC(fcntl, rfd, F_SETLK, fl);
                    if (0 == ret)
                        ret = rlock_apply_locked(st.st_dev, st.st_ino, current, fl->l_type, start, end);
                    if (fl->l_type == F_UNLCK)
                        pthread_cond_broadcast(&rlocks_cond);
                    int held_outside = (-1 == ret && cmd == F_SETLKW && (errno == EAGAIN || errno == EACCES));
                    pthread_mutex_unlock(&rlocks_lock);
                    if (!held_outside)
                        break;
                    // F_SETLKW held off by a process outside TVM, which the
                    // condition variable knows nothing about: poll.
                    usleep(20000);
                }
                else if (cmd == F_SETLK) {
                    pthread_mutex_unlock(&rlocks_lock);
                    errno = EAGAIN;
                    ret = -1;
                    break;
                }
                else {
                    if (rlock_would_deadlock_locked(current, c->rl_owner)) {
                        pthread_mutex_unlock(&rlocks_lock);
                        errno = EDEADLK;
                        ret = -1;
                        break;
                    }
                    // F_SETLKW: sleep until a release, in short slices so a
                    // signal that arrived meanwhile interrupts the wait with
                    // EINTR, as the kernel's would.
                    struct rlock_waiter waiter = { .rw_waiter = current, .rw_owner = c->rl_owner };
                    list_add(&waiter.rw_list, &rlock_waiters);
                    struct timespec ts;
                    clock_gettime(CLOCK_REALTIME, &ts);
                    ts.tv_nsec += 20 * 1000000L;
                    if (ts.tv_nsec >= 1000000000L) {
                        ts.tv_sec++;
                        ts.tv_nsec -= 1000000000L;
                    }
                    pthread_cond_timedwait(&rlocks_cond, &rlocks_lock, &ts);
                    list_del(&waiter.rw_list);
                    pthread_mutex_unlock(&rlocks_lock);
                }
                // A deliverable signal ends the wait with EINTR. Deferred
                // signals are recorded per thread (signal_handler sets
                // thr_pending_sigs); checking only tsk_pending_sigs, which
                // nothing sets, made the wait impossible to interrupt, even
                // by SIGKILL.
                uint32_t pend = __atomic_load_n(&current->tsk_pending_sigs, __ATOMIC_ACQUIRE) |
                                __atomic_load_n(&current_thread->thr_pending_sigs, __ATOMIC_ACQUIRE);
                int deliverable = 0;
                for (int sig = 1; sig < MAX_SIGNALS && !deliverable; sig++)
                    if ((pend & (1u << sig)) && !sigismember(&current_thread->thr_sigmask, sig))
                        deliverable = 1;
                if (deliverable) {
                    errno = EINTR;
                    ret = -1;
                    break;
                }
            }
        }
    }

    int last_errno = errno;
    file_lock(file, 0);
    file_unlock(file, 1);
    tvm_sig_leave();
    errno = last_errno;
    return ret;
}

FUNCTION_DEFINE_VA2(int, fcntl, int, fd, int, cmd/*, ...  arg */ ) {
    void *arg = va_arg (__ap, void *);
    if (!can_enter_tvm(NULL))
        return CALL_FUNC(fcntl, fd, cmd, arg);

    if (cmd == F_GETLK || cmd == F_SETLK || cmd == F_SETLKW)
        return fcntl_rlock(fd, cmd, (struct flock *)arg);

#ifdef F_SETNOSIGPIPE
    // the program's own no-SIGPIPE setting, kept in TVM (see setsockopt)
    if (cmd == F_SETNOSIGPIPE || cmd == F_GETNOSIGPIPE) {
        tvm_sig_enter();
        struct file *file = file_for_task(current, fd);
        if (!file) {
            tvm_sig_leave();
            errno = EBADF;
            return -1;
        }
        int ret = 0;
        if (cmd == F_SETNOSIGPIPE)
            file->f_nosigpipe = ((int)(long)arg) ? 1 : 0;
        else
            ret = file->f_nosigpipe;
        file_unlock(file, 1);
        tvm_sig_leave();
        return ret;
    }
#endif

    if (cmd == F_DUPFD) {
        // TODO: F_DUPFD_CLOEXEC

        tvm_sig_enter();

        task_lock(current);
        struct file *oldfile = file_for_task_locked(current, fd);
        if (!oldfile) {
            task_unlock(current);
            tvm_sig_leave();
            errno = EBADF;
            return -1;
        }

        // see comment in dup2() regarding locks

        struct file *newfile = file_dup_locked(oldfile);
        task_unlock(current);
        file_unlock(oldfile, 1);
        
        if (!newfile) {
            tvm_sig_leave();
            return -1;
        }

        file_unlock(newfile, 0);
        task_lock(current);
        file_lock(newfile, 0);

        int fd;
        if (-1 == (fd = task_assign_file_locked(current, newfile, (int)(long)arg))) {
            task_unlock(current);
            file_unlock(newfile, 1);
            tvm_sig_leave();
            errno = EMFILE;
            return -1;
        }

        task_unlock(current);
        file_unlock(newfile, 1);
        tvm_sig_leave();

        return fd;
    }

    return CURRENT_RFD(fd, CALL_FUNC(fcntl, __rfd, cmd, arg));
}

#if defined(__linux__)
// glibc >= 2.28 with _FILE_OFFSET_BITS=64: fcntl is fcntl64 (see pread64).
// struct flock is struct flock64 and F_*LK64 are F_*LK on x86-64.
FUNCTION_DEFINE_VA2(int, fcntl64, int, fd, int, cmd/*, ...  arg */ ) {
    return __tvm_fcntl_va(fd, cmd, __ap);
}
#endif

FUNCTION_DEFINE2(int, ftruncate, int, fd, off_t, length) {
    return CURRENT_RFD(fd, CALL_FUNC(ftruncate, __rfd, length));
}

FUNCTION_DEFINE2(int, stat, const char *, pathname, struct stat *, statbuf) {
    char pathname_abs[PATH_MAX];
    if (!normalize_path(NULL, pathname, pathname_abs))
        return -1;
    pathname = pathname_abs;

    tvm_sig_enter();
    if (find_program(pathname)) {
        tvm_sig_leave();
        statbuf->st_mode = S_IFREG | S_IXUSR | S_IXGRP | S_IXOTH;
        return 0;
    }

    int ret = file_stat(pathname, statbuf, 0);
#if TARGET_OS_IOS
    // iOS sandbox may not report execute permission on signed dylibs
    // accessed from the app bundle. Add execute bits so the shell's
    // search_access doesn't reject them.
    if (ret == 0 && S_ISREG(statbuf->st_mode))
        statbuf->st_mode |= S_IXUSR | S_IXGRP | S_IXOTH;
#endif
    tvm_sig_leave();
    return ret;
}

FUNCTION_DEFINE2(int, lstat, const char *, pathname, struct stat *, statbuf) {
    char pathname_abs[PATH_MAX];
    if (!normalize_path(NULL, pathname, pathname_abs))
        return -1;
    pathname = pathname_abs;

    tvm_sig_enter();
    if (find_program(pathname)) {
        tvm_sig_leave();
        statbuf->st_mode = S_IFREG | S_IXUSR | S_IXGRP | S_IXOTH;
        return 0;
    }

    int ret = file_stat(pathname, statbuf, AT_SYMLINK_NOFOLLOW);
    tvm_sig_leave();
    return ret;
}

FUNCTION_DEFINE2(int, fstat, int, fd, struct stat *, statbuf) {
    tvm_sig_enter();
    struct file *file = file_for_task(current, fd);
    if (!file) {
        tvm_sig_leave();
        errno = EBADF;
        return -1;
    }

    size_t ret;
    if (!file->f_ops || !file->f_ops->f_fstat) {
        file_unlock(file, 0);
        ret = CALL_FUNC(fstat, file->f_rfd, statbuf);
        file_lock(file, 0);
    }
    else
        ret = file->f_ops->f_fstat(file, statbuf);
    
    file_unlock(file, 1);
    tvm_sig_leave();
    
    return ret;
}

// fstatfs was not interposed, so a guest's fstatfs(fd) reached libc with the
// *guest* descriptor number and queried whichever host descriptor happened to
// carry it: some other guest's file, a pipe, or nothing. sqlite3's unixOpen()
// calls fstatfs() on every file it opens when built for Apple (to spot
// msdos/exfat volumes), so opening a database failed with "disk I/O error"
// (SQLITE_IOERR_ACCESS, errno EINVAL from a pipe or EBADF) whenever the
// descriptor numbers of the guest and the host process disagreed -- which
// they do as soon as the shell has a pipeline or a background job around
// the call, so the same command passed alone and failed inside a script.
// statfs(path) is the path-based twin and goes through the task's cwd like
// stat() does. Neither consults the cpio VFS: a VFS file's real descriptor is
// /dev/null, whose devfs answer is as good as any for a read-only mount, and
// a VFS path is simply not on the host.
FUNCTION_DEFINE2(int, fstatfs, int, fd, struct statfs *, buf) {
    return CURRENT_RFD(fd, CALL_FUNC(fstatfs, __rfd, buf));
}

FUNCTION_DEFINE2(int, statfs, const char *, pathname, struct statfs *, buf) {
    char pathname_abs[PATH_MAX];
    if (!normalize_path(NULL, pathname, pathname_abs))
        return -1;
    return CALL_FUNC(statfs, pathname_abs, buf);
}

FUNCTION_DEFINE4(int, fstatat, int, dirfd, const char *, pathname, struct stat *, statbuf, int, flag) {
    char pathname_abs[PATH_MAX];
    if(!dirfd_normalize_path(dirfd, pathname, pathname_abs))
        return -1;
    pathname = pathname_abs;

    tvm_sig_enter();
    int ret = file_stat(pathname, statbuf, flag);
    tvm_sig_leave();
    return ret;
}

FUNCTION_DEFINE2(int, access, const char *, pathname, int, mode) {
    char pathname_abs[PATH_MAX];
    if (!normalize_path(NULL, pathname, pathname_abs))
        return -1;
    pathname = pathname_abs;

    tvm_sig_enter();

    // make sure the shell finds our programs
    if (find_program(pathname)) {
        tvm_sig_leave();
        return 0;
    }

    if (mode & X_OK) {
        mode &= ~X_OK;
        if (!mode)
            mode = F_OK;
    }
    
    int ret = file_access(pathname, mode, 0);
    tvm_sig_leave();
    return ret;
}

FUNCTION_DEFINE4(int, faccessat, int, dirfd, const char *, pathname, int, mode, int, flags) {
    char pathname_abs[PATH_MAX];
    if(!dirfd_normalize_path(dirfd, pathname, pathname_abs))
        return -1;
    pathname = pathname_abs;

    tvm_sig_enter();

    // make sure the shell finds our programs
    if (find_program(pathname)) {
        tvm_sig_leave();
        return 0;
    }

    if (mode & X_OK) {
        mode &= ~X_OK;
        if (!mode)
            mode = F_OK;
    }

    int ret = file_access(pathname, mode, flags);
    tvm_sig_leave();
    return ret;
}

#if defined(__linux__)
/*
 * glibc's eaccess()/euidaccess(): access() against the effective IDs. bash
 * built on Linux finds HAVE_EACCESS and tests every PATH candidate with
 * eaccess(X_OK) (findcmd.c file_status). Without these the call reached the
 * host, which has no /tvm/bin/cat: bash skipped TVM's programs and ran the
 * host's /usr/bin/cat, which TVM cannot load ("Invalid argument").
 */
static int tvm_eaccess(const char *pathname, int mode)
{
    return faccessat(AT_FDCWD, pathname, mode, AT_EACCESS);
}

FUNCTION_DEFINE2(int, eaccess, const char *, pathname, int, mode) {
    return tvm_eaccess(pathname, mode);
}

FUNCTION_DEFINE2(int, euidaccess, const char *, pathname, int, mode) {
    return tvm_eaccess(pathname, mode);
}
#endif

FUNCTION_DEFINE2(int, chmod, const char *, pathname, mode_t, mode) {
    char pathname_abs[PATH_MAX];
    if (!normalize_path(NULL, pathname, pathname_abs))
        return -1;
    pathname = pathname_abs;

    tvm_sig_enter();
    int ret = file_chmod(pathname, mode, 0);
    tvm_sig_leave();
    return ret;
}

FUNCTION_DEFINE2(int, lchmod, const char *, pathname, mode_t, mode) {
    char pathname_abs[PATH_MAX];
    if (!normalize_path(NULL, pathname, pathname_abs))
        return -1;
    pathname = pathname_abs;

    tvm_sig_enter();
    int ret = file_chmod(pathname, mode, AT_SYMLINK_NOFOLLOW);
    tvm_sig_leave();
    return ret;
}

FUNCTION_DEFINE2(int, fchmod, int, fd, mode_t, mode) {
    tvm_sig_enter();
    struct file *file = file_for_task(current, fd);
    if (!file) {
        tvm_sig_leave();
        errno = EBADF;
        return -1;
    }

    size_t ret;
    if (!file->f_ops || !file->f_ops->f_fchmod) {
        file_unlock(file, 0);
        ret = CALL_FUNC(fchmod, file->f_rfd, mode);
        file_lock(file, 0);
    }
    else
        ret = file->f_ops->f_fchmod(file, mode);
    
    file_unlock(file, 1);
    tvm_sig_leave();
    
    return ret;
}

FUNCTION_DEFINE4(int, fchmodat, int, dirfd, const char *, pathname, mode_t, mode, int, flag) {
    char pathname_abs[PATH_MAX];
    if(!dirfd_normalize_path(dirfd, pathname, pathname_abs))
        return -1;
    pathname = pathname_abs;

    tvm_sig_enter();
    int ret = file_chmod(pathname, mode, flag);
    tvm_sig_leave();
    return ret;
}

FUNCTION_DEFINE3(int, chown, const char *, pathname, uid_t, owner, gid_t, group) {
    char pathname_abs[PATH_MAX];
    if (!normalize_path(NULL, pathname, pathname_abs))
        return -1;
    pathname = pathname;

    tvm_sig_enter();
    int ret = file_chown(pathname, owner, group, 0);
    tvm_sig_leave();
    return ret;
}

FUNCTION_DEFINE3(int, lchown, const char *, pathname, uid_t, owner, gid_t, group) {
    char pathname_abs[PATH_MAX];
    if (!normalize_path(NULL, pathname, pathname_abs))
        return -1;
    pathname = pathname;

    tvm_sig_enter();
    int ret = file_chown(pathname, owner, group, AT_SYMLINK_NOFOLLOW);
    tvm_sig_leave();
    return ret;
}

FUNCTION_DEFINE3(int, fchown, int, fd, uid_t, owner, gid_t, group) {
    tvm_sig_enter();
    struct file *file = file_for_task(current, fd);
    if (!file) {
        tvm_sig_leave();
        errno = EBADF;
        return -1;
    }

    size_t ret;
    if (!file->f_ops || !file->f_ops->f_fchown) {
        file_unlock(file, 0);
        ret = CALL_FUNC(fchown, file->f_rfd, owner, group);
        file_lock(file, 0);
    }
    else
        ret = file->f_ops->f_fchown(file, owner, group);
    
    file_unlock(file, 1);
    tvm_sig_leave();
    
    return ret;
}

FUNCTION_DEFINE5(int, fchownat, int, dirfd, const char *, pathname, uid_t, owner, gid_t, group, int, flag) {
    char pathname_abs[PATH_MAX];
    if(!dirfd_normalize_path(dirfd, pathname, pathname_abs))
        return -1;
    pathname = pathname_abs;

    tvm_sig_enter();
    int ret = file_chown(pathname, owner, group, flag);
    tvm_sig_leave();
    return ret;
}

FUNCTION_DEFINE2(int, flock, int, fd, int, operation) {
    return CURRENT_RFD(fd, CALL_FUNC(flock, __rfd, operation));
}

FUNCTION_DEFINE3(int, lockf, int, fd, int, cmd, off_t, len) {
    return CURRENT_RFD(fd, CALL_FUNC(lockf, __rfd, cmd, len));
}

FUNCTION_DEFINE3(ssize_t, readlink, const char *, pathname, char *, buf, size_t, bufsiz) {
    char pathname_abs[PATH_MAX];
    if (!normalize_path(NULL, pathname, pathname_abs))
        return -1;
    pathname = pathname_abs;

    tvm_sig_enter();
    ssize_t ret = file_readlink(pathname, buf, bufsiz);
    tvm_sig_leave();
    return ret;
}

FUNCTION_DEFINE4(ssize_t, readlinkat, int, dirfd, const char *, pathname, char *, buf, size_t, bufsiz) {
    char pathname_abs[PATH_MAX];
    if (!dirfd_normalize_path(dirfd, pathname, pathname_abs))
        return -1;
    pathname = pathname_abs;

    tvm_sig_enter();
    ssize_t ret = file_readlink(pathname, buf, bufsiz);
    tvm_sig_leave();
    return ret;
}

FUNCTION_DEFINE2(int, symlink, const char *, target, const char *, linkpath) {
    char linkpath_abs[PATH_MAX];
    if (!normalize_path(NULL, linkpath, linkpath_abs))
        return -1;
    linkpath = linkpath_abs;

    return CALL_FUNC(symlink, target, linkpath);
}

FUNCTION_DEFINE3(int, symlinkat, const char *, target, int, newdirfd, const char *, linkpath) {
    char linkpath_abs[PATH_MAX];
    if (newdirfd == AT_FDCWD) {
        if (!normalize_path(NULL, linkpath, linkpath_abs))
            return -1;
        linkpath = linkpath_abs;
    }

    return CURRENT_RFD(newdirfd, CALL_FUNC(symlinkat, target, __rfd, linkpath));
}

FUNCTION_DEFINE2(int, link, const char *, oldpath, const char *, newpath) {
    char oldpath_abs[PATH_MAX];
    if (!normalize_path(NULL, oldpath, oldpath_abs))
        return -1;
    oldpath = oldpath_abs;

    char newpath_abs[PATH_MAX];
    if (!normalize_path(NULL, newpath, newpath_abs))
        return -1;
    newpath = newpath_abs;

    return CALL_FUNC(link, oldpath, newpath);
}

FUNCTION_DEFINE5(int, linkat, int, olddirfd, const char *, oldpath, int, newdirfd, const char *, newpath, int, flags) {
    char oldpath_abs[PATH_MAX];
    if (olddirfd == AT_FDCWD) {
        if (!normalize_path(NULL, oldpath, oldpath_abs))
            return -1;
        oldpath = oldpath_abs;
    }

    char newpath_abs[PATH_MAX];
    if (newdirfd == AT_FDCWD) {
        if (!normalize_path(NULL, newpath, newpath_abs))
            return -1;
        newpath = newpath_abs;
    }

    tvm_sig_enter();

    int olddirrfd = -1;
    struct file *olddirfile = file_for_task(current, olddirfd);
    if (olddirfile) {
        olddirrfd = olddirfile->f_rfd;
        file_unlock(olddirfile, 0);
    }

    int newdirrfd = -1;
    struct file *newdirfile = file_for_task(current, newdirfd);
    if (newdirfile) {
        newdirrfd = newdirfile->f_rfd;
        file_unlock(newdirfile, 0);
    }

    int ret = CALL_FUNC(linkat, olddirrfd, oldpath, newdirrfd, newpath, flags);

    if (newdirfile) {
        file_lock(newdirfile, 0);
        file_unlock(newdirfile, 1);
    }

    if (olddirfile) {
        file_lock(olddirfile, 0);
        file_unlock(olddirfile, 1);
    }

    tvm_sig_leave();

    return ret;
}

FUNCTION_DEFINE2(int, rename, const char *, oldpath, const char *, newpath) {
    char oldpath_abs[PATH_MAX];
    if (!normalize_path(NULL, oldpath, oldpath_abs))
        return -1;
    oldpath = oldpath_abs;

    char newpath_abs[PATH_MAX];
    if (!normalize_path(NULL, newpath, newpath_abs))
        return -1;
    newpath = newpath_abs;

    return CALL_FUNC(rename, oldpath, newpath);
}

FUNCTION_DEFINE4(int, renameat, int, olddirfd, const char *, oldpath, int, newdirfd, const char *, newpath) {
    char oldpath_abs[PATH_MAX];
    if (olddirfd == AT_FDCWD) {
        if (!normalize_path(NULL, oldpath, oldpath_abs))
            return -1;
        oldpath = oldpath_abs;
    }

    char newpath_abs[PATH_MAX];
    if (newdirfd == AT_FDCWD) {
        if (!normalize_path(NULL, newpath, newpath_abs))
            return -1;
        newpath = newpath_abs;
    }

    tvm_sig_enter();

    int olddirrfd = -1;
    struct file *olddirfile = file_for_task(current, olddirfd);
    if (olddirfile) {
        olddirrfd = olddirfile->f_rfd;
        file_unlock(olddirfile, 0);
    }

    int newdirrfd = -1;
    struct file *newdirfile = file_for_task(current, newdirfd);
    if (newdirfile) {
        newdirrfd = newdirfile->f_rfd;
        file_unlock(newdirfile, 0);
    }

    int ret = CALL_FUNC(renameat, olddirrfd, oldpath, newdirrfd, newpath);

    if (newdirfile) {
        file_lock(newdirfile, 0);
        file_unlock(newdirfile, 1);
    }

    if (olddirfile) {
        file_lock(olddirfile, 0);
        file_unlock(olddirfile, 1);
    }

    tvm_sig_leave();
    
    return ret;
}

FUNCTION_DEFINE1(int, unlink, const char *, pathname) {
    char pathname_abs[PATH_MAX];
    if (!normalize_path(NULL, pathname, pathname_abs))
        return -1;
    pathname = pathname_abs;

    return CALL_FUNC(unlink, pathname);
}

FUNCTION_DEFINE3(int, unlinkat, int, dirfd, const char *, pathname, int, flags) {
    char pathname_abs[PATH_MAX];
    if (dirfd == AT_FDCWD) {
        if (!normalize_path(NULL, pathname, pathname_abs))
            return -1;
        pathname = pathname_abs;
    }

    return CURRENT_RFD(dirfd, CALL_FUNC(unlinkat, __rfd, pathname, flags));
}

FUNCTION_DEFINE2(int, mkdir, const char *, pathname, mode_t, mode) {
    char pathname_abs[PATH_MAX];
    if (!normalize_path(NULL, pathname, pathname_abs))
        return -1;
    pathname = pathname_abs;

    return CALL_FUNC(mkdir, pathname, mode & ~current->tsk_umask);
}

FUNCTION_DEFINE3(int, mkdirat, int, dirfd, const char *, pathname, mode_t, mode) {
    char pathname_abs[PATH_MAX];
    if (dirfd == AT_FDCWD) {
        if (!normalize_path(NULL, pathname, pathname_abs))
            return -1;
        pathname = pathname_abs;
    }

    return CURRENT_RFD(dirfd, CALL_FUNC(mkdirat, __rfd, pathname, mode & ~current->tsk_umask));
}

FUNCTION_DEFINE1(int, rmdir, const char *, pathname) {
    char pathname_abs[PATH_MAX];
    if (!normalize_path(NULL, pathname, pathname_abs))
        return -1;
    pathname = pathname_abs;

    return CALL_FUNC(rmdir, pathname);
}

FUNCTION_DEFINE2(int, mkfifo, const char *, pathname, mode_t, mode) {
    char pathname_abs[PATH_MAX];
    if (!normalize_path(NULL, pathname, pathname_abs))
        return -1;
    pathname = pathname_abs;

    return CALL_FUNC(mkfifo, pathname, mode & ~current->tsk_umask);
}

FUNCTION_DEFINE3(int, mkfifoat, int, dirfd, const char *, pathname, mode_t, mode) {
    char pathname_abs[PATH_MAX];
    if (dirfd == AT_FDCWD) {
        if (!normalize_path(NULL, pathname, pathname_abs))
            return -1;
        pathname = pathname_abs;
    }

    return CURRENT_RFD(dirfd, CALL_FUNC(mkfifoat, __rfd, pathname, mode & ~current->tsk_umask));
}

// Copy the characters the host generated for the template's trailing X run
// back into the caller's template (the absolute copy has the same tail).
static void mktemp_copyback(char *template, const char *abs)
{
    size_t len = strlen(template), alen = strlen(abs), n = 0;
    while (n < len && n < alen && template[len - 1 - n] == 'X')
        n++;
    memcpy(template + len - n, abs + alen - n, n);
}

FUNCTION_DEFINE1(int, mkstemp, char *, template) {
    int r, f;
    // Resolve a relative template against the task's cwd, not the host
    // process's (see mkdtemp), and hand the generated suffix back.
    char abs[PATH_MAX];
    if (!normalize_path(NULL, template, abs))
        return -1;
    tvm_sig_enter();
    if (-1 == (r = CALL_FUNC(mkstemp, abs))) {
        tvm_sig_leave();
        return -1;
    }
    mktemp_copyback(template, abs);

    if (-1 == (f = task_new_fd(current, r))) {
        VERIFY(0 == CALL_FUNC(close, r));
        errno = EMFILE;
    }

    tvm_sig_leave();

    return f;
}

// mkdtemp was not interposed, so a relative template was created relative
// to the host process's cwd, which is not the task's (TVM tracks each task's
// cwd itself and resolves every other path call against it). git creates
// its quarantine directory for incoming objects that way --
// mkdtemp("./objects/tmp_objdir-incoming-XXXXXX") in the receiving
// repository -- so `git push` to a local repository failed in
// receive-pack with "unable to create temporary object directory". The
// template is resolved against the task's cwd, created by the host, and
// the generated suffix copied back into the caller's template, which is
// what the caller reads (mkdtemp returns it).
FUNCTION_DEFINE1(char *, mkdtemp, char *, template) {
    char abs[PATH_MAX];
    if (!normalize_path(NULL, template, abs))
        return NULL;
    if (!CALL_FUNC(mkdtemp, abs))
        return NULL;
    mktemp_copyback(template, abs);
    return template;
}

// A new host descriptor, entered into the task's table (cf. eventfd).
static int tvm_task_adopt_fd(int r)
{
    int f;
    if (-1 == r)
        return -1;
    if (-1 == (f = task_new_fd(current, r))) {
        CALL_FUNC(close, r);
        errno = EMFILE;
    }
    return f;
}

// shm_open(3)/shm_unlink(3). A POSIX shared memory object is a name in the
// host's namespace, not a path (the task's cwd and the VFS play no part),
// and the descriptor is the host's: it goes into the task's table like a
// socket's. bash was configured ac_cv_func_shm_open=no because of that:
// its nofork command substitution (lib/sh/anonfile.c) opens and at once
// unlinks such an object for its scratch file, and the host descriptor it
// got was not a guest one. (On Linux it asks memfd_create first.)
// shm_unlink is a plain pass-through, interposed so the pair stays together.
#if defined(__APPLE__)
FUNCTION_DEFINE_VA2(int, shm_open, const char *, name, int, oflag) {
    mode_t mode = (oflag & O_CREAT) ? (mode_t)va_arg(__ap, int) : 0;
#else
FUNCTION_DEFINE3(int, shm_open, const char *, name, int, oflag, mode_t, mode) {
#endif
    tvm_sig_enter();
    int f = tvm_task_adopt_fd(CALL_FUNC(shm_open, name, oflag, mode));
    tvm_sig_leave();
    return f;
}

FUNCTION_DEFINE1(int, shm_unlink, const char *, name) {
    return CALL_FUNC(shm_unlink, name);
}

FUNCTION_DEFINE3(int, socket, int, domain, int, type, int, protocol) {
    int r, f;

    tvm_sig_enter();

    if (-1 == (r = CALL_FUNC(socket, domain, type, protocol))) {
        tvm_sig_leave();
        return -1;
    }

    if (-1 == (f = task_new_fd(current, r))) {
        VERIFY(0 == CALL_FUNC(close, r));
        errno = EMFILE;
    }

    tvm_sig_leave();

    return f;
}

FUNCTION_DEFINE3(int, accept, int, sockfd, struct sockaddr *, addr, socklen_t *, addrlen) {
    int r, f;
    
    tvm_sig_enter();

    if (-1 == (r = CURRENT_RFD(sockfd, CALL_FUNC(accept, __rfd, addr, addrlen)))) {
        tvm_sig_leave();
        return -1;
    }

    if (-1 == (f = task_new_fd(current, r))) {
        VERIFY(0 == CALL_FUNC(close, r));
        errno = EMFILE;
    }

    tvm_sig_leave();

    return f;
}

FUNCTION_DEFINE3(int, bind, int, sockfd, const struct sockaddr *, addr, socklen_t, addrlen) {
    // TODO: apply tsk_umask for AF_UNIX sockets (real umask is 0)
    return CURRENT_RFD(sockfd, CALL_FUNC(bind, __rfd, addr, addrlen));
}

FUNCTION_DEFINE2(int, listen, int, sockfd, int,  backlog) {
    return CURRENT_RFD(sockfd, CALL_FUNC(listen, __rfd, backlog));
}

FUNCTION_DEFINE3(int, connect, int, sockfd, const struct sockaddr *, addr, socklen_t, addrlen) {
    return CURRENT_RFD(sockfd, CALL_FUNC(connect, __rfd, addr, addrlen));
}

FUNCTION_DEFINE3(int, getpeername, int, sockfd, struct sockaddr *, addr, socklen_t *, addrlen) {
    return CURRENT_RFD(sockfd, CALL_FUNC(getpeername, __rfd, addr, addrlen));
}

FUNCTION_DEFINE3(int, getsockname, int, sockfd, struct sockaddr *, addr, socklen_t *, addrlen) {
    return CURRENT_RFD(sockfd, CALL_FUNC(getsockname, __rfd, addr, addrlen));
}

FUNCTION_DEFINE5(int, getsockopt, int, sockfd, int, level, int, optname, void *, optval, socklen_t *, optlen) {
#ifdef SO_NOSIGPIPE
    // the program's own setting (see setsockopt), not the host's
    if (level == SOL_SOCKET && optname == SO_NOSIGPIPE) {
        if (!optval || !optlen || *optlen < (socklen_t)sizeof(int)) {
            errno = EINVAL;
            return -1;
        }
        tvm_sig_enter();
        struct file *file = file_for_task(current, sockfd);
        if (!file) {
            tvm_sig_leave();
            errno = EBADF;
            return -1;
        }
        *(int *)optval = file->f_nosigpipe;
        *optlen = sizeof(int);
        file_unlock(file, 1);
        tvm_sig_leave();
        return 0;
    }
#endif
    return CURRENT_RFD(sockfd, CALL_FUNC(getsockopt, __rfd, level, optname, optval, optlen));
}

FUNCTION_DEFINE5(int, setsockopt, int, sockfd, int, level, int, optname, const void *, optval, socklen_t, optlen) {
#ifdef SO_NOSIGPIPE
    // Recorded for epipe_signal_fd, not passed on: the host descriptor must
    // keep no-SIGPIPE on (rfd_adopt), even when the program turns it off.
    if (level == SOL_SOCKET && optname == SO_NOSIGPIPE) {
        if (!optval || optlen < (socklen_t)sizeof(int)) {
            errno = EINVAL;
            return -1;
        }
        tvm_sig_enter();
        struct file *file = file_for_task(current, sockfd);
        if (!file) {
            tvm_sig_leave();
            errno = EBADF;
            return -1;
        }
        file->f_nosigpipe = *(const int *)optval ? 1 : 0;
        file_unlock(file, 1);
        tvm_sig_leave();
        return 0;
    }
#endif
    return CURRENT_RFD(sockfd, CALL_FUNC(setsockopt, __rfd, level, optname, optval, optlen));
}

FUNCTION_DEFINE4(ssize_t, send, int, sockfd, const void *, buf, size_t, len, int, flags) {
    ssize_t ret = CURRENT_RFD(sockfd, CALL_FUNC(send, __rfd, buf, len, flags));
    if (ret == -1)
        epipe_signal_fd(sockfd, flags);
    return ret;
}

FUNCTION_DEFINE6(ssize_t, sendto, int, sockfd, const void *, buf, size_t, len, int, flags, const struct sockaddr *, dest_addr, socklen_t, addrlen) {
    ssize_t ret = CURRENT_RFD(sockfd, CALL_FUNC(sendto, __rfd, buf, len, flags, dest_addr, addrlen));
    if (ret == -1)
        epipe_signal_fd(sockfd, flags);
    return ret;
}

FUNCTION_DEFINE3(ssize_t, sendmsg, int, sockfd, const struct msghdr *, msg, int, flags) {
    ssize_t ret = CURRENT_RFD(sockfd, CALL_FUNC(sendmsg, __rfd, msg, flags));
    if (ret == -1)
        epipe_signal_fd(sockfd, flags);
    return ret;
}

FUNCTION_DEFINE4(ssize_t, recv, int, sockfd, void *, buf, size_t, len, int, flags) {
    return CURRENT_RFD(sockfd, CALL_FUNC(recv, __rfd, buf, len, flags));
}

FUNCTION_DEFINE6(ssize_t, recvfrom, int, sockfd, void *, buf, size_t, len, int, flags, struct sockaddr *, src_addr, socklen_t *, addrlen) {
    return CURRENT_RFD(sockfd, CALL_FUNC(recvfrom, __rfd, buf, len, flags, src_addr, addrlen));
}

FUNCTION_DEFINE3(ssize_t, recvmsg, int, sockfd, struct msghdr *, msg, int, flags) {
    return CURRENT_RFD(sockfd, CALL_FUNC(recvmsg, __rfd, msg, flags));
}

FUNCTION_DEFINE2(int, shutdown, int, sockfd, int, how) {
    return CURRENT_RFD(sockfd, CALL_FUNC(shutdown, __rfd, how));
}

FUNCTION_DEFINE4(int, socketpair, int, domain, int, type, int, protocol, int *, sv) {
    tvm_sig_enter();

    int r[2], f[2];
    if (0 != CALL_FUNC(socketpair, domain, type, protocol, r)) {
        tvm_sig_leave();
        return -1;
    }
    
    if (-1 == (f[0] = task_new_fd(current, r[0]))) {
        VERIFY(0 == CALL_FUNC(close, r[0]));
        VERIFY(0 == CALL_FUNC(close, r[1]));
        tvm_sig_leave();
        return -1;
    }

    if (-1 == (f[1] = task_new_fd(current, r[1]))) {
        VERIFY(0 == close(f[0]));
        VERIFY(0 == CALL_FUNC(close, r[1]));
        tvm_sig_leave();
        return -1;
    }

    memcpy(sv, f, sizeof(f));
    tvm_sig_leave();
    return 0;
}

FUNCTION_DEFINE_VA2(int, ioctl, int, fd, unsigned long, request) {
    void* arg = va_arg(__ap, void*);
    if (!can_enter_tvm(NULL))
        return CALL_FUNC(ioctl, fd, request, arg);

    tvm_sig_enter();
    struct file *file = file_for_task(current, fd);
    if (!file) {
        tvm_sig_leave();
        errno = EBADF;
        return -1;
    }

    size_t ret;
    if (!file->f_ops || !file->f_ops->f_ioctl) {
        file_unlock(file, 0);
        ret = CALL_FUNC(ioctl, file->f_rfd, request, arg);
        file_lock(file, 0);
    }
    else
        ret = file->f_ops->f_ioctl(file, request, arg);
    
    file_unlock(file, 1);
    tvm_sig_leave();
    
    return ret;
}

/**
 * Who knew imitating `fork()` API with threads is such a hassle. Well it is.
 * 
 * Idea is to `setjmp` on parent, on child stack copy some frames from the parent and `longjmp` to the new stack.
 * Main problem is fixing up all the stack pointers on the frames pointer to the old stack to be on the new stack,
 * read `Jmpbuf implementation` section for that.
 */

#define FORKLESS_STACK_MAGIC ((uintptr_t)0xCAFEFACF)
#define DEFAULT_STACK_SIZE (0x1000 * 2000) // 8Mb

#define TEMP_STACK_SIZE DEFAULT_STACK_SIZE

struct fork_arg {
    struct task *parent;
    struct task *tsk;
    struct thread *thr;
    struct thread *parent_thr;

    struct cow_fixup_context fixup;
    void *tmp_stack;

    pthread_mutex_t mtx;
    pthread_cond_t cond;
    jmp_buf jmpbuf_parent;
    jmp_buf jmpbuf_tmp;
    jmp_buf jmpbuf_fail;
    
    size_t duplicate_errors;
    int first_duplicate_error;

    pid_t pid;

    uintptr_t thread_fp;

    // parent's tvm_emu_call_depth at fork time. The child inherits the parent's
    // copied emulator_call_ex frames, so it must resume emulation with the same
    // depth or its signal-deferral (and the frames' decrements) go out of sync.
    int emu_call_depth;
};

static void fork_dup_emulator_cb(struct fork_arg *arg, void *old_mem, void *new_mem, size_t size) {
    if (0 != cow_fixup_ctx_add(&arg->fixup, new_mem, old_mem, size, 1)) {
        arg->duplicate_errors++;
        if (!arg->first_duplicate_error)
            arg->first_duplicate_error = errno;
    }
}

static void fork_dup_linker_cb(struct fork_arg *arg, void *old_map, void *new_map, size_t size, int prot) {

    int has_rw = ((PROT_READ | PROT_WRITE) == (prot & (PROT_READ | PROT_WRITE)));

    if (has_rw)
        memcpy(new_map, old_map, size);

    if (0 != cow_fixup_ctx_add(&arg->fixup, new_map, old_map, size, has_rw)) {
        arg->duplicate_errors++;
        if (!arg->first_duplicate_error)
            arg->first_duplicate_error = errno;
    }
}

static void fork_dup_heap_cb(struct fork_arg *arg, void *old_slab, void *new_slab, size_t slabsize)
{
    if (0 != cow_fixup_ctx_add(&arg->fixup, new_slab, old_slab, slabsize, 1)) {
        arg->duplicate_errors++;
        if (!arg->first_duplicate_error)
            arg->first_duplicate_error = errno;
    }
}

static void fork_done_heap_cb(struct fork_arg *arg)
{
    if (arg->duplicate_errors == 0) {
        // strip EMUFUNC_MARK from sighandler pointers so fixup can translate them
        uint32_t emu_marked = 0;
        for (int i = 0; i < MAX_SIGNALS; i++) {
            struct sigaction *sa = &arg->tsk->tsk_sighandlers[i];
            if (sa->sa_flags & SA_SIGINFO) {
                if (EMUFUNC_MARKED(sa->sa_sigaction)) {
                    emu_marked |= (1u << i);
                    sa->sa_sigaction = EMUFUNC_UNMARK(sa->sa_sigaction);
                }
            } else {
                if (EMUFUNC_MARKED(sa->sa_handler)) {
                    emu_marked |= (1u << i);
                    sa->sa_handler = EMUFUNC_UNMARK(sa->sa_handler);
                }
            }
        }

        cow_fixup_process(&arg->fixup);

        // re-mark only handlers that were originally marked
        for (int i = 0; i < MAX_SIGNALS; i++) {
            if (!(emu_marked & (1u << i)))
                continue;
            struct sigaction *sa = &arg->tsk->tsk_sighandlers[i];
            if (sa->sa_flags & SA_SIGINFO)
                sa->sa_sigaction = EMUFUNC_MARK(sa->sa_sigaction);
            else
                sa->sa_handler = EMUFUNC_MARK(sa->sa_handler);
        }
    }
}

static void tvm_proc_entry_cont(struct fork_arg *arg);

static void *tvm_proc_entry(struct fork_arg *arg)
{
    // save fp before modifying
    uintptr_t curr_fp = (uintptr_t)__builtin_frame_address(0);
    arg->thread_fp = curr_fp;

    struct tvm_tls *child_tls = kalloc(sizeof(struct tvm_tls));
    if (!child_tls)
        panic("tvm_proc_entry: tvm_tls alloc");
    memset(child_tls, 0, sizeof(struct tvm_tls));
    pthread_setspecific(tls_key, child_tls);

    // set thread-local current/current_thread early so ulinker callbacks
    // (which VERIFY(arg == current)) work during ulinker_duplicate
    current = arg->tsk;
    current_thread = arg->thr;
    current->tsk_pid = __gettid();
    current_thread->thr_pthread = pthread_self();
    task_rusage_start(current);
    list_add_tail(&current_thread->thr_list, &current->tsk_threads);
    current->tsk_threads_cnt++;

    // Inherit the parent's virtual signal mask.
    current_thread->thr_sigmask = arg->parent_thr->thr_sigmask;

    // Unblock OS signals now that current/current_thread are set.
    // The parent blocked all signals before pthread_create so we inherited
    // a fully-blocked signal mask.
    sigset_t empty_sigs;
    sigemptyset(&empty_sigs);
    CALL_FUNC(pthread_sigmask, SIG_SETMASK, &empty_sigs, NULL);

    // when this function fails, it is probably running on an adjusted stack
    // so we need to restore to the original stack frame of this function to return
    // the error number is set to thread's errno
    if ((setjmp(arg->jmpbuf_fail))) {
        return (void *)(long)errno;
    }

    // move to temporary stack
    if (0 == setjmp(arg->jmpbuf_tmp))
    {
        /**
         * this code moves to a temporary stack, since we're gonna modify our own stack.
         *
         *   ._______. 0x0000
         *   |       |
         *   |       |
         *   |       |
         *   |       |        <- sp
         *   |       | 
         *   '_______' 0xffff <- fp
         * 
         * NOTE: `fp` points to a 2-dword structure
         *   struct prologue {
         *     void *fp;
         *     void *lr;
         *   }
         * 
         * This means an additional 2-dword needs to be added to the frame on top of `fp - sp`.
         * We don't copy it, but rather forge it to values we can detect if a return is attempted.
         * 
         *   ^         ^  ^- sp is up there
         *   |         |
         *   |         |_
         *   | prev fp | '- fp points here 
         *   | prev lr |
         *   |         |
         */
        jmpbuf_demangle(&arg->jmpbuf_tmp);
        uintptr_t curr_sp = (uintptr_t)jmpbuf_getstack(&arg->jmpbuf_tmp);
        // `fp` is just before the value of `fp` itself, so keep room for `fp` on the stack
        uintptr_t next_fp = (uintptr_t)arg->tmp_stack + TEMP_STACK_SIZE - 2 * sizeof(uintptr_t);
        uintptr_t next_sp = next_fp - (curr_fp - curr_sp);

        memcpy((void *)next_sp, (void *)curr_sp, curr_fp - curr_sp);
        // forge prologue so if a return occurs we can detect it
        *(uintptr_t *)(next_fp) = 0xDEADBEEF46464646;
        *(uintptr_t *)(next_fp + 8) = 0xDEADBEEF42424242;

        // INFO("longjmp(arg->jmpbuf_tmp) jump curr-stack %p-%p next-stack %p-%p", curr_sp, curr_fp, next_sp, next_fp);
        // INFO("  curr-arg = %p *%p", &arg, arg);
        // INFO("  next-arg = %p *%p", (uintptr_t)&arg - ((uintptr_t)curr_sp - (uintptr_t)next_sp), *(uintptr_t *)((uintptr_t)&arg - ((uintptr_t)curr_sp - (uintptr_t)next_sp)));

        // include prologue (fp + lr) when adjusting jmpbuf context regs
        jmpbuf_setstack(&arg->jmpbuf_tmp, next_sp, curr_sp, curr_fp - curr_sp + 2 * sizeof(uintptr_t));

        jmpbuf_mangle(&arg->jmpbuf_tmp);
        longjmp(arg->jmpbuf_tmp, 1);
    }

    // running on a temporary stack until the next longjmp

    // NOTE: had to split logic to new func to deal with some pesky compiler optimizations that couldn't handle stack being changed midway through this func
    tvm_proc_entry_cont(arg);
    __builtin_unreachable();
}

static void tvm_proc_entry_cont(struct fork_arg *arg)
{
    uintptr_t curr_fp = arg->thread_fp;

    /**
     * setup child stack as a copy of a few important frames from parent stack
     *
     *                       parent               child
     *                     ._______.   0x0000   ._______.
     *                     |       |            |       |
     *        .--<  sp ->  |-------| -> -\      |       |
     *  stack |            |       |      -> -> |-------|  <- sp  >--.
     *  copy  |            |       |   memcpy   |       |            |  stack
     *  size  '-->  fp ->  |-------| -> -\      |       |            |  copy
     *                     '_______'      -> -> '_______'  <- fp  <--'  size
     */

    uintptr_t parent_sp = (uintptr_t)jmpbuf_getstack(&arg->jmpbuf_parent);
    uintptr_t parent_fp = (uintptr_t)arg->parent_thr->thr_fork_fp;

    // its important to "link" the new frames copied from parent to the current stack for `pthread_exit()` to work
    // as `pthread_exit()` throws a C++ exception on some implmentations that is meant to be caught
    // by pthread's `__thread_entry` func (which calls the start routine)
    uintptr_t child_fp = curr_fp;
    uintptr_t child_sp = curr_fp - (parent_fp - parent_sp);

    memcpy((void *)child_sp, (void *)parent_sp, parent_fp - parent_sp);
    // for debugging when unwinder crashes
    // *(uintptr_t *)(child_fp) = 0xDEADBEEF46464646;
    // *(uintptr_t *)(child_fp + 8) = 0xDEADBEEF42424242;

    // include prologue (fp + lr) when adjusting jmpbuf context regs (important for fixing up pointers to rbp)
    // INFO("STACK %p %p %p", (void *)parent_sp, (void *)child_sp, (void *)(parent_fp - parent_sp + 2 * sizeof(uintptr_t)));
    if (0 != cow_fixup_ctx_add(&arg->fixup, (void *)child_sp, (void *)parent_sp, parent_fp - parent_sp + 2 * sizeof(uintptr_t), 1))
        goto fail;

    // fix the tvm data section pointer (which is allocated on the task's arena)
    arg->tsk->tsk_data = arg->parent->tsk_data;
    if (0 != cow_fixup_ctx_add(&arg->fixup, (void *)&arg->tsk->tsk_data, (void *)&arg->parent->tsk_data, sizeof(arg->tsk->tsk_data), 1))
        goto fail;
    // relocations
    memcpy(&arg->tsk->tsk_relocs, &arg->parent->tsk_relocs, sizeof(arg->tsk->tsk_relocs));
    if (0 != cow_fixup_ctx_add(&arg->fixup, (void *)&arg->tsk->tsk_relocs, (void *)&arg->parent->tsk_relocs, sizeof(arg->tsk->tsk_relocs), 1))
        goto fail;
    // signal handlers contain function pointers that need translation
    if (0 != cow_fixup_ctx_add(&arg->fixup, (void *)arg->tsk->tsk_sighandlers, (void *)arg->parent->tsk_sighandlers, sizeof(arg->tsk->tsk_sighandlers), 1))
        goto fail;

    if (0 != ulinker_duplicate(arg->parent->tsk_linker, arg->tsk->tsk_linker, (void (*)(void *arg, void *old_map, void *new_map, size_t size, int))fork_dup_linker_cb, (void *)arg))
        goto fail;

    if (arg->duplicate_errors) {
        INFO("tvm_proc_entry - ulinker_duplicate() - %zu duplicate errors - first error: %d", arg->duplicate_errors, arg->first_duplicate_error);
        errno = arg->first_duplicate_error;
        goto fail;
    }

    if (0 != emulator_duplicate(arg->parent_thr->thr_emulator, arg->thr->thr_emulator, (void (*)(void *arg, void *old_mem, void *new_mem, size_t size))fork_dup_emulator_cb, (void *)arg))
        goto fail;

    if (arg->duplicate_errors) {
        INFO("tvm_proc_entry - emulator_duplicate() - %zu duplicate errors - first error: %d", arg->duplicate_errors, arg->first_duplicate_error);
        errno = arg->first_duplicate_error;
        goto fail;
    }

    // duplicate file-stream entries
    task_lock(arg->parent);
    struct file_stream_list_entry *old_fsle;
    struct file_stream_list_entry *new_fsle;
    list_for_each_entry(old_fsle, &arg->parent->tsk_filestreams, fsle_list) {
        new_fsle = kalloc(sizeof(struct file_stream_list_entry));
        if (!new_fsle)
            goto fail;
        list_add(&new_fsle->fsle_list, &arg->tsk->tsk_filestreams);
        new_fsle->fsle_stream = old_fsle->fsle_stream;

        if (0 != cow_fixup_ctx_add(&arg->fixup, (void *)&new_fsle->fsle_stream, (void *)&old_fsle->fsle_stream, sizeof(new_fsle->fsle_stream), 1))
            goto fail;
    }
    memcpy(arg->tsk->tsk_io_filestreams, arg->parent->tsk_io_filestreams, sizeof(arg->tsk->tsk_io_filestreams));
    if (0 != cow_fixup_ctx_add(&arg->fixup, (void *)arg->tsk->tsk_io_filestreams, (void *)arg->parent->tsk_io_filestreams, sizeof(arg->tsk->tsk_io_filestreams), 1))
        goto fail;

    arg->first_duplicate_error = 0;
    if (0 != rmem_arena_duplicate(
        arg->tsk->tsk_ar,
        arg->parent->tsk_ar,
        (void (*)(void *, void *, void *, size_t))fork_dup_heap_cb,
        (void (*)(void *))fork_done_heap_cb,
        (void *)arg))
        goto fail;

    if (arg->duplicate_errors) {
        INFO("tvm_proc_entry - rmem_arena_duplicate() - %zu duplicate errors - first error: %d", arg->duplicate_errors, arg->first_duplicate_error);
        errno = arg->first_duplicate_error;
        goto fail;
    }

    list_for_each_entry(new_fsle, &arg->tsk->tsk_filestreams, fsle_list) {
        if (new_fsle->fsle_stream)
            new_fsle->fsle_stream->fs_dp = NULL;
    }
    
    list_for_each_entry(new_fsle, &arg->tsk->tsk_filestreams, fsle_list) {
        // fixup gencnts and open new streams
        if (0 != task_filestreamrealloc_locked(arg->tsk, new_fsle->fsle_stream))
            goto fail;
    }

    task_unlock(arg->parent);

    arg->thr->thr_fork_fp = (void *)child_fp;

    // include prologue (fp + lr) when adjusting jmpbuf context regs
    //jmpbuf_setstack(&arg->jmpbuf_parent, child_sp, parent_sp, parent_fp - parent_sp + 2 * sizeof(uintptr_t));

    // The atexit handlers taskalloc_fork copied from the parent are
    // addresses in the parent's image; the child runs its own copy of the
    // image at another address. Nothing had noticed while guest atexit()
    // registered nothing (see atexit): git's `maintenance run --auto
    // --detach`, run after every commit, daemonizes with fork() + exit() in
    // the parent, and the child's exit() then called the parent's handler
    // addresses -- unmapped by then -- and took the whole VM down with a
    // hardware fault in TVM's exit path.
    for (int i = 0; i < (int)(sizeof(arg->tsk->tsk_exits)/sizeof(*arg->tsk->tsk_exits)); i++) {
        if (arg->tsk->tsk_exits[i])
            arg->tsk->tsk_exits[i] = (exit_t)cow_fixup_translate(&arg->fixup, (void *)arg->tsk->tsk_exits[i]);
        if (arg->tsk->tsk_exit_args[i])
            arg->tsk->tsk_exit_args[i] = cow_fixup_translate(&arg->fixup, arg->tsk->tsk_exit_args[i]);
    }

    // fix the jmpbuf
    void **jb = (void **) (&arg->jmpbuf_parent);
    for (int i = 0; i < (sizeof(jmp_buf)/8); i++)
        jb[i] = cow_fixup_translate(&arg->fixup, jb[i]);

    jmpbuf_mangle(&arg->jmpbuf_parent);

    // Resume emulation at the parent's emulator_call_ex depth: the longjmp lands
    // back in the copied emulated frame, whose eventual out: will decrement.
    tvm_emu_call_depth = arg->emu_call_depth;

    longjmp(arg->jmpbuf_parent, 1);

fail:
    panic("tvm_proc_entry failed (fail flow not yet implemented)");
    
    // notify parent to pthread join us
    pthread_mutex_lock(&arg->mtx);
    arg->pid = -1;
    pthread_cond_broadcast(&arg->cond);
    pthread_mutex_unlock(&arg->mtx);

    // TODO for some reason this crashes badly

    longjmp(arg->jmpbuf_fail, 1);
}

// for now we need strict control of how many frames we need to restore on new stack
// so do not use wrapper
//FUNCTION_DEFINE0(pid_t, fork) {
FUNCTION_METADATA(fork, &fork);
pid_t fork(void) {
    if (!can_enter_tvm(__builtin_return_address(0)))
        return CALL_FUNC(fork);
    
    struct task *tsk = NULL;
    struct thread *thr = NULL;
    struct fork_arg *arg = NULL;
    pthread_attr_t attr;
    pid_t ret = -1;
    int r;

    tvm_sig_enter();

    if ((r = pthread_attr_init(&attr))) {
        tvm_sig_leave();
        errno = r;
        return -1;
    }

    if ((r = pthread_attr_setstacksize(&attr, DEFAULT_STACK_SIZE))) {
        errno = r;
        goto out;
    }

    tsk = taskalloc(); // locks current and inherits whatever
    if (!tsk) {
        errno = ENOMEM;
        goto out;
    }

    thr = threadalloc(tsk);
    if (!thr) {
        errno = ENOMEM;
        goto out;
    }

    if ((r = copy_pthread_attr(&attr, &thr->thr_pthread_attr))) {
        errno = r;
        goto out;
    }

    arg = kalloc(sizeof(struct fork_arg));
    if (!arg) {
        errno = ENOMEM;
        goto out;
    }
    // kalloc does not zero: an uninitialized fixup.ptrs was handed to
    // krealloc (a reused block's stale pointer to a never-freed table)
    memset(&arg->fixup, 0, sizeof(arg->fixup));

    arg->pid = 0;
    arg->tsk = tsk;
    arg->thr = thr;
    arg->parent = current;
    arg->parent_thr = current_thread;
    arg->emu_call_depth = tvm_emu_call_depth;  // child resumes emulation at this depth

    arg->tmp_stack = CALL_FUNC(mmap, NULL, TEMP_STACK_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
#ifdef TRACE_MMAP
    INFO("mmap %p-%p (tmp stack)", arg->tmp_stack, (uintptr_t)arg->tmp_stack + TEMP_STACK_SIZE);
#endif
    if (arg->tmp_stack == MAP_FAILED) {
        arg->tmp_stack = NULL;
        errno = ENOMEM;
        goto out;
    }

    if ((r = pthread_mutex_init(&arg->mtx, NULL))) {
        errno = r;
        goto out;
    }

    if ((r = pthread_cond_init(&arg->cond, NULL))) {
        errno = r;
        goto out;
    }

    if (0 != setjmp(arg->jmpbuf_parent)) {

        // child

        // add child task to tasks list
        pthread_mutex_lock(&tasks_lock);
        list_add_tail(&current->tsk_list, &main_task->tsk_list);
        pthread_mutex_unlock(&tasks_lock);

        // let parent know our PID and exit on both child and parent
        pthread_mutex_lock(&arg->mtx);
        arg->pid = __gettid();
        pthread_cond_broadcast(&arg->cond);
        pthread_mutex_unlock(&arg->mtx);

        return 0;
    }

    // parent

    jmpbuf_demangle(&arg->jmpbuf_parent);

    // Block OS signals before creating the child thread so it inherits a
    // blocked signal mask.  The child unblocks after setting current/current_thread
    // in tvm_proc_entry — without this, a signal arriving on the new thread
    // before those thread-locals are set causes a panic (current == NULL).
    // Never block the synchronous fault signals: blocking SIGSEGV/BUS/ILL/FPE
    // and then faulting (e.g. inside pthread_create) is undefined — the kernel
    // force-kills with a core dump instead of delivering to signal_handler, so
    // the fault escapes TVM's handler entirely. They are per-thread and
    // synchronous, so leaving them unblocked can't spuriously hit the new thread.
    sigset_t all_sigs, old_sigs;
    sigfillset(&all_sigs);
    sigdelset(&all_sigs, SIGSEGV);
    sigdelset(&all_sigs, SIGBUS);
    sigdelset(&all_sigs, SIGILL);
    sigdelset(&all_sigs, SIGFPE);
    CALL_FUNC(pthread_sigmask, SIG_BLOCK, &all_sigs, &old_sigs);

    pthread_t pth;
    if ((r = CALL_FUNC(pthread_create, &pth, &attr, (void *(*)(void *))tvm_proc_entry, (void *)arg))) {
        CALL_FUNC(pthread_sigmask, SIG_SETMASK, &old_sigs, NULL);
        errno = r;
        goto out;
    }

    CALL_FUNC(pthread_sigmask, SIG_SETMASK, &old_sigs, NULL);

    // failure no longer allowed on parent

    pthread_mutex_lock(&arg->mtx);
    while (arg->pid == 0) {
        pthread_cond_wait(&arg->cond, &arg->mtx);
    }
    pthread_mutex_unlock(&arg->mtx);

    ret = arg->pid;

    // if child failed, it'll return -1 so join it
    if (ret == -1) {
        // we are not allowed to fail here
        void *thread_r;
        if ((r = pthread_join(pth, &thread_r)))
            panic("fork::pthread_join failed (errno %d)", r);

        errno = (int)(long)(thread_r);
    }

out:

    if (arg) {

        pthread_cond_destroy(&arg->cond);
        pthread_mutex_destroy(&arg->mtx);        

        if (arg->tmp_stack) {
#ifdef TRACE_MMAP
            INFO("munmap %p-%p (tmp stack)", arg->tmp_stack, (uintptr_t)arg->tmp_stack + TEMP_STACK_SIZE);
#endif
            CALL_FUNC(munmap, arg->tmp_stack, TEMP_STACK_SIZE);
            arg->tmp_stack = NULL;
        }        

        // the fork pointer-translation table, grown with krealloc in the
        // global arena by cow_fixup_ctx_add
        kfree(arg->fixup.ptrs);
        arg->fixup.ptrs = NULL;
        kfree(arg->fixup.order);
        arg->fixup.order = NULL;

        kfree(arg);
        arg = NULL;
    }

    if (ret == -1) {

        if (thr) {
            threaddealloc(thr);
            thr = NULL;
        }
        
        if (tsk) {
            taskfreelastref(tsk, 1, 1);
            tsk = NULL;
        }
    }

    pthread_attr_destroy(&attr);

    tvm_sig_leave();

    return ret;
}

// vfork() is used by toybox (xargs, etc.) — redirect to TVM's fork()
// since TVM's fork already uses pthreads with COW semantics.
FUNCTION_METADATA(vfork, &vfork);
pid_t vfork(void) {
    return fork();
}

struct pthread_create_arg
{
    start_routine_fn start_fn;
    void *start_arg;
    struct task *tsk;
    struct thread *thr;
    sigset_t sigmask; // snapshot of creating thread's mask

    pthread_mutex_t mtx;
    pthread_cond_t cond;
    int done; // -1 means not done, 0 - success, >0 error
};

static void *tvm_thread_entry(struct pthread_create_arg *arg)
{
    struct tvm_tls *t = kalloc(sizeof(struct tvm_tls));
    if (!t)
        panic("tvm_thread_entry: tvm_tls alloc");
    memset(t, 0, sizeof(struct tvm_tls));
    pthread_setspecific(tls_key, t);

    pthread_mutex_lock(&arg->mtx);

    current = arg->tsk;
    current_thread = arg->thr;

    current_thread->thr_pthread = pthread_self();
    current_thread->thr_fork_fp = (void *)__builtin_frame_address(0);

    // Inherit the creating thread's virtual signal mask.
    current_thread->thr_sigmask = arg->sigmask;

    // current is locked by the creating thread
    list_add_tail(&current_thread->thr_list, &current->tsk_threads);
    current->tsk_threads_cnt++;

    start_routine_fn start_fn = arg->start_fn;
    void *start_arg = arg->start_arg;

    arg->done = 0;
    pthread_cond_broadcast(&arg->cond);
    pthread_mutex_unlock(&arg->mtx);

    // unblock OS signals now that we're safe with locks
    sigset_t empty_sigs;
    sigemptyset(&empty_sigs);
    CALL_FUNC(pthread_sigmask, SIG_SETMASK, &empty_sigs, NULL);

    pthread_exit(start_fn(start_arg));
}

// Create a thread within a locked task.  The new thread gets the full TVM lifecycle
// (thread list, signal mask, refcount, emulator context, pthread_exit teardown).
//
// If parent_attr is non-NULL it is copied to the thread's pthread_attr.
// If parent_sigmask is non-NULL the thread inherits it; otherwise starts empty.
static int task_create_thread_locked(struct task *tsk, start_routine_fn start_fn, void *arg,
                              pthread_t *out_thread, const pthread_attr_t *attr,
                              const sigset_t *sigmask)
{
    int ret = ENOMEM;
    tsk->tsk_refcount++;

    struct thread *thr = threadalloc(tsk);
    if (!thr)
        goto fail_refcount;

    if (attr) {
        ret = copy_pthread_attr(attr, &thr->thr_pthread_attr);
        if (ret)
            goto fail_thr;
    }

    struct pthread_create_arg *a = kalloc(sizeof(*a));
    if (!a)
        goto fail_thr;

    a->start_fn = start_fn;
    a->start_arg = arg;
    a->tsk = tsk;
    a->thr = thr;
    if (sigmask)
        a->sigmask = *sigmask;
    else
        memset(&a->sigmask, 0, sizeof(a->sigmask));

    if ((ret = pthread_mutex_init(&a->mtx, NULL))) {
        errno = ret;
        goto fail_arg;
    }

    if ((ret = pthread_cond_init(&a->cond, NULL))) {
        errno = ret;
        goto fail_arg;
    }

    a->done = -1;

    // NOTE: for the entire duration of pthread_create, current is locked by the creating thread!
    // the new thread signals the creating thread when it finished initializing.

    pthread_mutex_lock(&a->mtx);

    sigset_t all, old;
    sigfillset(&all);
    CALL_FUNC(pthread_sigmask, SIG_BLOCK, &all, &old);

    ret = CALL_FUNC(pthread_create, out_thread, attr, (start_routine_fn)tvm_thread_entry, a);

    CALL_FUNC(pthread_sigmask, SIG_SETMASK, &old, NULL);

    if (ret) {
        pthread_mutex_unlock(&a->mtx);
        goto fail_arg;
    }

    while (a->done == -1)
        pthread_cond_wait(&a->cond, &a->mtx);

    // currently, the child always succeeds!!
    VERIFY(a->done == 0);
    ret = a->done;

    pthread_mutex_unlock(&a->mtx);

    pthread_cond_destroy(&a->cond);
    pthread_mutex_destroy(&a->mtx);
    kfree(a);

    return 0;
fail_arg:
    pthread_cond_destroy(&a->cond);
    pthread_mutex_destroy(&a->mtx);
    kfree(a);
fail_thr:
    threaddealloc(thr);
fail_refcount:
    tsk->tsk_refcount--;
    return ret;
}

FUNCTION_DEFINE4(int, pthread_create, pthread_t *, thread, const pthread_attr_t *, attr, start_routine_fn, start_routine, void *, arg) {
    tvm_sig_enter();
    task_lock(current);
    int ret = task_create_thread_locked(current, start_routine, arg, thread, attr, &current_thread->thr_sigmask);
    task_unlock(current);
    tvm_sig_leave();
    return ret;
}

FUNCTION_DEFINE_NORET1(pthread_exit, void *, retval) {
    tvm_sig_enter();

    task_lock(current);
    if (current->tsk_threads_cnt == 1) {
        task_unlock(current);
        exit((int) ((long) retval));
    }

    detach_current_thread_and_exit(retval);
}

FUNCTION_DEFINE_NORET1(exit, int, status) {
    tvm_sig_enter();
    task_lock(current);
    exit_t exits[sizeof(current->tsk_exits)/sizeof(*current->tsk_exits)];
    void *exit_args[sizeof(current->tsk_exits)/sizeof(*current->tsk_exits)];
    memcpy(&exits, current->tsk_exits, sizeof(exits));
    memcpy(&exit_args, current->tsk_exit_args, sizeof(exit_args));
    memset(&current->tsk_exits, 0, sizeof(current->tsk_exits));
    memset(&current->tsk_exit_args, 0, sizeof(current->tsk_exit_args));
    task_unlock(current);

    for (int i = 0; i < sizeof(current->tsk_exits)/sizeof(*current->tsk_exits); i++) {
        if (exits[i]) {
            if (0 != emulator_call(NULL, (uint64_t)exits[i], (uint64_t)exit_args[i], 0, 0, 0, 0, 0))
                abort();
        }
    }

    // Flush with signals deliverable: a broken pipe here is a fatal SIGPIPE
    // for the exiting task, as in exit(3). Under the tvm_sig_enter() above it
    // would only be deferred, and _exit() never drains it. _exit() re-enters.
    tvm_sig_leave();
    fflush(NULL);

    _exit(status);
}

FUNCTION_DEFINE_NORET1(_exit, int, status) {
    tvm_sig_enter();
    if (current == main_task)
        CALL_FUNC(_exit, status);

    pthread_mutex_lock(&tasks_lock);
    task_lock(current);

    terminate_current_locked(mkwstatus(1, status));
}

FUNCTION_DEFINE_NORET1(_Exit, int, status) {
    _exit(status);
}

FUNCTION_DEFINE_NORET0(abort) {
    raise(SIGABRT);
    _exit(1); // unreachable
}

// atexit is implemented in the executable rather than libc
// so we cannot really call the original one
//
// It is a plain function, not a FUNCTION_DEFINE interposer, so no tvm_call
// record is pushed for it, and can_enter_tvm(NULL) -- which takes the
// caller's address from that record -- found none and said no: every
// guest's atexit() returned 0 and registered nothing, and no guest exit
// handler ever ran. OpenSSL registers OPENSSL_cleanup() with atexit() (curl,
// git's curl helpers) so that its thread-state destructor does not fire
// during the task thread's teardown -- where it aborts in libmalloc and
// leaves the caller waiting forever, so every `git clone https://...` hung
// -- on every exit path, die() included. The
// caller is now taken from the return address, as the interposer wrappers
// record it, so a guest's registration is kept and exit() runs it.
static int tvm_atexit(exit_t function, void *arg, void *lr);

#if defined(__linux__)
// C++ static destructors: func(arg) at exit. The argument used to be
// dropped, so every destructor ran on a NULL object.
int __cxa_atexit(void (*func)(void *), void *arg, void *dso_handle) {
    return tvm_atexit((exit_t)func, arg, __builtin_return_address(0));
}
#endif

//FUNCTION_DEFINE1(int, atexit, exit_t, function) {
int atexit(exit_t function) {
    return tvm_atexit(function, NULL, __builtin_return_address(0));
}
// Listed with the interposers so the linker binds a guest's atexit here by
// name. Without the entry, ulinker_external_dlsym fell back to
// dlsym(RTLD_DEFAULT), which finds this definition only where the TVM
// executable itself exports it (the CLI); in the apps TVM is libtvm.dylib,
// searched after libSystem, so guests got libc's atexit and their handlers
// were queued for the app's own exit, never run at the task's.
FUNCTION_METADATA(atexit, &atexit);

static int tvm_atexit(exit_t function, void *arg, void *lr) {
    
    if (!can_enter_tvm(lr))
        return 0;

    tvm_sig_enter();

    if (!function) {
        errno = EINVAL;
        tvm_sig_leave();
        return -1;
    }

    task_lock(current);
    for (int i = (sizeof(current->tsk_exits)/sizeof(*current->tsk_exits)) - 1; i >= 0; i--) {
        if (!current->tsk_exits[i]) {
            current->tsk_exits[i] = function;
            current->tsk_exit_args[i] = arg;
            task_unlock(current);
            tvm_sig_leave();
            return 0;
        }
    }
    task_unlock(current);

    tvm_sig_leave();

    errno = ENOMEM;
    return -1;
}

FUNCTION_DEFINE_NORET_VA2(errx, int, eval, const char *, fmt) {
    verrx(eval, fmt, __ap);
}

FUNCTION_DEFINE_NORET_VA2(err, int, eval, const char *, fmt) {
    verr(eval, fmt, __ap);
}

FUNCTION_DEFINE_NORET3(verr, int, eval, const char *, fmt, va_list, args) {
    tvm_sig_enter();
    int saved_errno = errno;

    if (*current->tsk_comm)
        fprintf(stderr, "%s: ", current->tsk_comm);

    if (fmt) {
        vfprintf(stderr, fmt, args);
        fprintf(stderr, ": ");
    }

    fprintf(stderr, "%s\n", strerror(saved_errno));
    // balance the tvm_sig_enter() above: exit() only leaves its own, so a
    // SIGPIPE from the flush it does would otherwise stay deferred and be
    // lost at _exit()
    tvm_sig_leave();
    exit(eval);
}

FUNCTION_DEFINE_NORET3(verrx, int, eval, const char *, fmt, va_list, args) {
    tvm_sig_enter();
    if (*current->tsk_comm)
        fprintf(stderr, "%s: ", current->tsk_comm);

    if (fmt) {
        vfprintf(stderr, fmt, args);
    }

    fprintf(stderr, "\n");
    // balance the tvm_sig_enter() above: exit() only leaves its own, so a
    // SIGPIPE from the flush it does would otherwise stay deferred and be
    // lost at _exit()
    tvm_sig_leave();
    exit(eval);
}

// warn(3) family. The host libc's versions write straight to the host's
// stderr with the host process name ("tvm: ..."), bypassing the task's fd 2
// entirely; these go through the task's stderr with its own name, as the
// err()/errx() interposers above already do. `code` < 0 means no strerror.
static void tvm_vwarnc(int code, const char *fmt, va_list ap)
{
    if (*current->tsk_comm)
        fprintf(stderr, "%s: ", current->tsk_comm);
    if (fmt) {
        vfprintf(stderr, fmt, ap);
        if (code >= 0)
            fprintf(stderr, ": ");
    }
    if (code >= 0)
        fprintf(stderr, "%s", strerror(code));
    fprintf(stderr, "\n");
}

FUNCTION_DEFINE_VOID2(vwarn, const char *, fmt, va_list, args) {
    int last_errno = errno;
    tvm_vwarnc(last_errno, fmt, args);
    errno = last_errno;
}

FUNCTION_DEFINE_VOID2(vwarnx, const char *, fmt, va_list, args) {
    int last_errno = errno;
    tvm_vwarnc(-1, fmt, args);
    errno = last_errno;
}

#if defined(__APPLE__)
FUNCTION_DEFINE_VOID3(vwarnc, int, code, const char *, fmt, va_list, args) {
    int last_errno = errno;
    tvm_vwarnc(code, fmt, args);
    errno = last_errno;
}
#endif

FUNCTION_DEFINE_VOID_VA1(warn, const char *, fmt) {
    vwarn(fmt, __ap);
}

FUNCTION_DEFINE_VOID_VA1(warnx, const char *, fmt) {
    vwarnx(fmt, __ap);
}

#if defined(__APPLE__)
/*
 * sysctl(2) under the iOS app sandbox.
 *
 * The sandbox lets an app read most values (kern.boottime, hw.memsize,
 * hw.machine, ... are read by every app) and translate a name to a mib
 * (CTL_SYSCTL_NAME2OID, "0.3"), but denies with EPERM the other meta
 * queries sysctl(8) makes before it prints anything: the format of a mib
 * (CTL_SYSCTL_OIDFMT, "0.4.<mib>") and its name (CTL_SYSCTL_NAME,
 * "0.1.<mib>"), so on a device every `sysctl name` died with "sysctl fmt
 * -1 1024 1: Operation not permitted". A few values are denied outright
 * too. sysctl(8) is upstream's unpatched source, so the answers come from
 * here (they used to be a header force-included into that one program;
 * the kernel answers for every guest):
 *  - the last name-to-mib translation of the thread is remembered, and a
 *    denied mib-to-name query for that mib is answered from it;
 *  - a denied format query is answered by fetching the value and inferring
 *    the format oidfmt() expects (a u_int kind, then the format string)
 *    from its size and content: string, timeval, int, quad; anything else
 *    is an opaque, which sysctl(8) refuses to print rather than misprints;
 *  - vm.page_pageable_internal_count and vm.page_purgeable_count, denied as
 *    values, are the kernel's vm_page_pageable_internal_count and
 *    vm_page_purgeable_count, which host_statistics64() also reports
 *    (internal_page_count, purgeable_count; vm_stat prints them) and the
 *    sandbox allows: answered from there;
 *  - ifconfig's per-interface IFDATA_SUPPLEMENTAL table (net.link.generic
 *    .ifdata.<index>.5) is denied, and upstream ifconfig err()s out after
 *    the first interface. The table is informational (unaligned packets,
 *    data milestone interval, printed only when non-zero): a denied read
 *    is answered with zeros, and upstream ifconfig enumerates on.
 * Every other call, and every query the kernel does answer (macOS, or a
 * future iOS that permits them), passes straight through: only an EPERM
 * is ever acted on.
 */
#define TVM_CTL_SYSCTL          0   // the meta node; sysctl(8) defines these itself
#define TVM_CTL_SYSCTL_NAME     1
#define TVM_CTL_SYSCTL_NAME2OID 3
#define TVM_CTL_SYSCTL_OIDFMT   4
// net/if_mib.h (not in the iPhoneOS SDK) and net/if_mib_private.h (in none)
#define TVM_NETLINK_GENERIC     0
#define TVM_IFMIB_IFDATA        2
#define TVM_IFDATA_SUPPLEMENTAL 5

static int tvm_sysctl_value_fallback(int *oid, u_int len, void *oldp, size_t *oldlenp)
{
    struct tvm_tls *tls = tvm_tls();
    if (len == 6 && oid[0] == CTL_NET && oid[1] == PF_LINK && oid[2] == TVM_NETLINK_GENERIC &&
        oid[3] == TVM_IFMIB_IFDATA && oid[5] == TVM_IFDATA_SUPPLEMENTAL) {
        if (oldp)
            memset(oldp, 0, *oldlenp);
        return 0;
    }
    if (len != tls->tls_sysctl_miblen || tls->tls_sysctl_miblen == 0 ||
        memcmp(oid, tls->tls_sysctl_mib, len * sizeof(int)) != 0) {
        errno = EPERM;
        return -1;
    }
    int internal = strcmp(tls->tls_sysctl_name, "vm.page_pageable_internal_count") == 0;
    int purgeable = strcmp(tls->tls_sysctl_name, "vm.page_purgeable_count") == 0;
    if (!internal && !purgeable) {
        errno = EPERM;
        return -1;
    }
    vm_statistics64_data_t vs;
    mach_msg_type_number_t cnt = HOST_VM_INFO64_COUNT;
    if (host_statistics64(mach_host_self(), HOST_VM_INFO64, (host_info64_t)&vs, &cnt) != KERN_SUCCESS) {
        errno = EPERM;
        return -1;
    }
    unsigned int v = internal ? (unsigned int)vs.internal_page_count
                              : (unsigned int)vs.purgeable_count;
    if (oldp) {
        if (*oldlenp < sizeof(v)) {
            errno = ENOMEM;
            return -1;
        }
        memcpy(oldp, &v, sizeof(v));
    }
    *oldlenp = sizeof(v);
    return 0;
}

// sysctl(2) for a plain value, with the sandbox fallback.
static int tvm_sysctl_value(int *oid, u_int len, void *oldp, size_t *oldlenp)
{
    int r = CALL_FUNC(sysctl, oid, len, oldp, oldlenp, NULL, 0);
    if (r != 0 && errno == EPERM)
        r = tvm_sysctl_value_fallback(oid, len, oldp, oldlenp);
    return r;
}

static int tvm_sysctl_infer_fmt(int *oid, u_int len, void *oldp, size_t *oldlenp)
{
    size_t vlen = 0;
    if (tvm_sysctl_value(oid, len, NULL, &vlen) != 0)
        return -1;
    unsigned char *val = calloc(1, vlen + 1);
    if (!val)
        return -1;
    size_t got = vlen;
    if (vlen && tvm_sysctl_value(oid, len, val, &got) != 0) {
        int e = errno;
        free(val);
        errno = e;
        return -1;
    }

    u_int kind = CTLFLAG_RD;
    const char *fmt = "";

    // A string first: a NUL-terminated run of printable characters with at
    // least one alphanumeric. Size alone cannot tell a 4- or 8-byte string
    // ("iPad8,1", "16.7.10") from an int or a quad, and checking sizes first
    // printed those as numbers. An int/quad/timeval whose bytes all happen to
    // be printable-then-NUL would be misread the other way, which needs
    // every byte but the last to be printable text -- not a real value.
    if (got >= 2 && val[got - 1] == '\0') {
        size_t i;
        int alnum = 0;
        for (i = 0; i + 1 < got; i++) {
            if (!isprint(val[i]) && !isspace(val[i]))
                break;
            if (isalnum(val[i]))
                alnum = 1;
        }
        if (i + 1 == got && alnum) {
            kind |= CTLTYPE_STRING;
            fmt = "A";
        }
    }
    if (!*fmt && got == sizeof(struct timeval)) {
        struct timeval tv;
        memcpy(&tv, val, sizeof(tv));
        if (tv.tv_sec > 0 && tv.tv_usec >= 0 && tv.tv_usec < 1000000) {
            kind |= CTLTYPE_OPAQUE;
            fmt = "S,timeval";
        }
    }
    if (!*fmt && got == sizeof(int)) {
        kind |= CTLTYPE_INT;
        fmt = "I";
    } else if (!*fmt && got == sizeof(long long)) {
        kind |= CTLTYPE_QUAD;
        fmt = "Q";
    }
    if (!*fmt)
        kind |= CTLTYPE_OPAQUE;
    free(val);

    size_t need = sizeof(kind) + strlen(fmt) + 1;
    if (*oldlenp < need) {
        errno = ENOMEM;
        return -1;
    }
    memcpy(oldp, &kind, sizeof(kind));
    memcpy((char *)oldp + sizeof(kind), fmt, strlen(fmt) + 1);
    *oldlenp = need;
    return 0;
}

FUNCTION_DEFINE6(int, sysctl, int *, name, u_int, namelen, void *, oldp, size_t *, oldlenp, void *, newp, size_t, newlen) {
    struct tvm_tls *tls = tvm_tls();
    int r = CALL_FUNC(sysctl, name, namelen, oldp, oldlenp, newp, newlen);
    if (!oldlenp)
        return r;
    if (namelen < 2 || name[0] != TVM_CTL_SYSCTL) {
        if (r != 0 && errno == EPERM && !newp)
            r = tvm_sysctl_value_fallback(name, namelen, oldp, oldlenp);
        return r;
    }
    if (!oldp)
        return r;

    if (r == 0 && namelen == 2 && name[1] == TVM_CTL_SYSCTL_NAME2OID &&
        newp && newlen < sizeof(tls->tls_sysctl_name) &&
        *oldlenp <= sizeof(tls->tls_sysctl_mib)) {
        memcpy(tls->tls_sysctl_name, newp, newlen);
        tls->tls_sysctl_name[newlen] = '\0';
        memcpy(tls->tls_sysctl_mib, oldp, *oldlenp);
        tls->tls_sysctl_miblen = (u_int)(*oldlenp / sizeof(int));
        return r;
    }
    if (r == 0 || errno != EPERM || namelen < 3)
        return r;

    if (name[1] == TVM_CTL_SYSCTL_NAME) {
        size_t need = strlen(tls->tls_sysctl_name) + 1;
        if (namelen - 2 != tls->tls_sysctl_miblen || tls->tls_sysctl_miblen == 0 ||
            memcmp(name + 2, tls->tls_sysctl_mib, tls->tls_sysctl_miblen * sizeof(int)) != 0 ||
            *oldlenp < need) {
            errno = EPERM;
            return -1;
        }
        memcpy(oldp, tls->tls_sysctl_name, need);
        *oldlenp = need;
        return 0;
    }
    if (name[1] == TVM_CTL_SYSCTL_OIDFMT)
        return tvm_sysctl_infer_fmt(name + 2, namelen - 2, oldp, oldlenp);
    return r;
}
#endif

#if defined(__APPLE__)  // warnc/errc/getprogname/setprogname are BSD-only
FUNCTION_DEFINE_VOID_VA2(warnc, int, code, const char *, fmt) {
    vwarnc(code, fmt, __ap);
}

FUNCTION_DEFINE_NORET4(verrc, int, eval, int, code, const char *, fmt, va_list, args) {
    tvm_sig_enter();
    tvm_vwarnc(code, fmt, args);
    // balance the tvm_sig_enter() above: exit() only leaves its own, so a
    // SIGPIPE from the flush it does would otherwise stay deferred and be
    // lost at _exit()
    tvm_sig_leave();
    exit(eval);
}

FUNCTION_DEFINE_NORET_VA3(errc, int, eval, int, code, const char *, fmt) {
    verrc(eval, code, fmt, __ap);
}

// The BSD program name is per task, not the host process's ("tvm").
FUNCTION_DEFINE0(const char *, getprogname) {
    return current->tsk_comm;
}

FUNCTION_DEFINE_VOID1(setprogname, const char *, name) {
    const char *base = strrchr(name, '/');
    base = base ? base + 1 : name;
    memset(current->tsk_comm, 0, sizeof(current->tsk_comm));
    strncpy(current->tsk_comm, base, sizeof(current->tsk_comm) - 1);
}
#endif

FUNCTION_DEFINE0(pid_t, getpid) {
    return current->tsk_pid;
}

FUNCTION_DEFINE1(mode_t, umask, mode_t, mask) {
    mode_t old = current->tsk_umask;
    current->tsk_umask = mask & 0777;
    return old;
}

FUNCTION_DEFINE0(pid_t, getppid) {
    if (main_task == current)
        return CALL_FUNC(getppid);

    tvm_sig_enter();
    task_lock(current);
    pid_t ppid = (current->tsk_parent ? current->tsk_parent->tsk_pid : 1);
    task_unlock(current);
    tvm_sig_leave();

    return ppid;
}

FUNCTION_DEFINE1(pid_t, wait, int *, wstatus) {
    return waitpid(-1, wstatus, 0);
}

static pid_t tvm_wait4(pid_t pid, int *wstatus, int options, struct rusage *ru)
{
    struct task *zombiet = NULL;
    int found;

    tvm_sig_enter();

    pthread_mutex_lock(&tasks_lock);

    // 0 is current pgid (which is marked in pid as `-pgid`)
    if (pid == 0) {
        task_lock(current);
        pid = -(current->tsk_pgid);
        task_unlock(current);
    }

    while (!(zombiet = task_next_zombie_child(current, pid, &found))) {
        if (!found) {
            pthread_mutex_unlock(&tasks_lock);
            tvm_sig_leave();
            errno = ECHILD;
            return -1;
        }
        if (options & WNOHANG) {
            pthread_mutex_unlock(&tasks_lock);
            tvm_sig_leave();
            return 0;
        }
        pthread_cond_wait(&current->tsk_wait_cond, &tasks_lock);

        // A sibling thread is tearing our task down (its exit()/_exit(), or a
        // fatal signal it took). Its drain loop is waiting for us to leave;
        // reap ourselves like a normal non-last thread instead of looping here.
        struct thread *exiting = __atomic_load_n(&current->tsk_exiting_thread, __ATOMIC_ACQUIRE);
        if (exiting && exiting != current_thread) {
            task_lock(current);
            thread_exit_for_task_exit_locked();  // releases both locks, no return
        }

        // Signals are deferred across waitpid (tvm_sig_enter) and cond_wait is
        // not EINTR-interruptible, so without this a task sleeping here is
        // unkillable — even by SIGKILL. task_kill_locked broadcasts our wait
        // cond after delivering; drain the pending signal at a safe point
        // (fatal → terminate, no return; handlers just run) and restart. Only
        // *deliverable* (unblocked) pending signals count — a blocked one keeps
        // its pending bit and would otherwise spin this check forever.
        uint32_t pend = __atomic_load_n(&current->tsk_pending_sigs, __ATOMIC_ACQUIRE);
        if (current_thread)
            pend |= __atomic_load_n(&current_thread->thr_pending_sigs, __ATOMIC_ACQUIRE);
        int deliverable = 0;
        for (int sig = 1; sig < MAX_SIGNALS; sig++) {
            if ((pend & (1u << sig)) && !sigismember(&current_thread->thr_sigmask, sig)) {
                deliverable = 1;
                break;
            }
        }
        if (deliverable) {
            pthread_mutex_unlock(&tasks_lock);
            tvm_sig_leave();  // may not return
            tvm_sig_enter();
            pthread_mutex_lock(&tasks_lock);
        }
    }

    task_lock(zombiet);
    // Claim the zombie AND remove it from the task list atomically under
    // tasks_lock, before dropping the lock for pthread_join. TS_REAPED alone
    // left a window: between here and taskfreelastref()'s list_del (after the
    // join), tasks_lock is released, and another waitpid — concurrent on a
    // sibling thread, or re-entrant on this one via the emulator — could scan,
    // find, and reap the same zombie again (double pthread_join + double
    // taskfreelastref -> double rmem_arena_destroy -> munmap of a reused/live
    // region). list_del now makes it unreachable immediately; INIT_LIST_HEAD
    // keeps taskfreelastref()'s later list_del a safe no-op on the empty node.
    zombiet->tsk_state |= TS_REAPED;
    list_del(&zombiet->tsk_list);
    INIT_LIST_HEAD(&zombiet->tsk_list);
    pthread_mutex_unlock(&tasks_lock);

    if (list_empty(&zombiet->tsk_threads))
        panic("waitpid tsk_pthreads empty");
    struct thread *thr = list_entry(zombiet->tsk_threads.next, struct thread, thr_list);
    pthread_join(thr->thr_pthread, NULL);

    pid_t childpid = zombiet->tsk_pid;
    if (wstatus)
        *wstatus = zombiet->tsk_result;
    if (ru)
        *ru = zombiet->tsk_rusage;

    // lock ordering: tasks_lock before task
    task_unlock(zombiet);
    taskfreelastref(zombiet, 1, 1);

    // wake fellow waiters: one may be blocked waiting for this specific child
    // (it skipped the TS_REAPED zombie) and must re-scan to discover ECHILD
    pthread_cond_broadcast(&current->tsk_wait_cond);

    tvm_sig_leave();

    return childpid;
}

FUNCTION_DEFINE3(pid_t, waitpid, pid_t, pid, int *, wstatus, int, options) {
    return tvm_wait4(pid, wstatus, options, NULL);
}

// Not interposed before: toybox time's wait4() went to the host libc, which
// has no such child, and printed whatever was on the stack as user/sys time.
FUNCTION_DEFINE4(pid_t, wait4, pid_t, pid, int *, wstatus, int, options, struct rusage *, rusage) {
    return tvm_wait4(pid, wstatus, options, rusage);
}

FUNCTION_DEFINE3(pid_t, wait3, int *, wstatus, int, options, struct rusage *, rusage) {
    return tvm_wait4(-1, wstatus, options, rusage);
}

FUNCTION_DEFINE0(pid_t, setsid) {
    tvm_sig_enter();

    pthread_mutex_lock(&tasks_lock);
    task_lock(current);

    if (-1 == task_set_pgid(current, current->tsk_pid)) {
        task_unlock(current);
        pthread_mutex_unlock(&tasks_lock);
        tvm_sig_leave();
        return -1;
    }

    current->tsk_sid = current->tsk_pid;
    current->tsk_state &= ~TS_CTTY;

    task_unlock(current);
    pthread_mutex_unlock(&tasks_lock);

    // no need to lock because only set in `taskalloc()`
    tvm_sig_leave();
    return current->tsk_pid;
}

FUNCTION_DEFINE1(pid_t, getsid, pid_t, pid) {
    tvm_sig_enter();

    if (!pid)
        pid = current->tsk_pid;

    struct task *t = task_lookup_pid(pid, 1);
    if (!t) {
        tvm_sig_leave();
        return CALL_FUNC(getsid, pid);
    }
    pid_t sid = t->tsk_sid;
    task_unlock(t);

    tvm_sig_leave();
    return sid;
}

FUNCTION_DEFINE2(int, setpgid, pid_t, pid, pid_t, pgid) {
    tvm_sig_enter();

    if (pgid < 0) {
        errno = EINVAL;
        tvm_sig_leave();
        return -1;
    }

    pthread_mutex_lock(&tasks_lock);

    if (!pid)
        pid = current->tsk_pid;
    
    if (!pgid)
        pgid = pid;

    struct task *t = task_lookup_pid(pid, 0);
    if (!t) {
        pthread_mutex_unlock(&tasks_lock);
        tvm_sig_leave();
        return CALL_FUNC(setpgid, pid, pgid);
    }

    int ret = task_set_pgid(t, pgid);
    task_unlock(t);
    pthread_mutex_unlock(&tasks_lock);
    tvm_sig_leave();
    return ret;
}

FUNCTION_DEFINE1(pid_t, getpgid, pid_t, pid) {
    tvm_sig_enter();

    if (!pid)
        pid = current->tsk_pid;
    
    struct task *t = task_lookup_pid(pid, 1);
    if (!t) {
        tvm_sig_leave();
        return CALL_FUNC(getpgid, pid);
    }
    
    pid_t pgid = t->tsk_pgid;
    task_unlock(t);
    tvm_sig_leave();
    return pgid;
}

FUNCTION_DEFINE0(pid_t, getpgrp) {
    return getpgid(0);
}

FUNCTION_DEFINE0(int, setpgrp) {
    return setpgid(0, 0);
}

#if defined(__linux__)
FUNCTION_DEFINE2(sighandler_t, signal, int, signum, sighandler_t, handler) {
#elif defined(__APPLE__)
FUNCTION_DEFINE2(sig_t, signal, int, signum, sig_t, handler) {
#endif
    struct sigaction oldact, act = {
        .sa_handler = handler,
        .sa_flags = SA_RESTART,
    };

    if (-1 == sigaction(signum, &act, &oldact)) {
        return SIG_ERR;
    }

    return oldact.sa_handler;
}

FUNCTION_DEFINE3(int, sigaction, int, signum, const struct sigaction *, act, struct sigaction *, oldact) {
    tvm_sig_enter();

    if (signum >= MAX_SIGNALS || signum < 1 ||
        signum == SIGKILL || signum == SIGSTOP) {
        errno = EINVAL;
        tvm_sig_leave();
        return -1;
    }

    task_lock(current);
    if (oldact)
        memcpy(oldact, current->tsk_sighandlers + signum, sizeof(*current->tsk_sighandlers));
    if (act)
        memcpy(current->tsk_sighandlers + signum, act, sizeof(*current->tsk_sighandlers));
    task_unlock(current);

    tvm_sig_leave();
    return 0;
}

FUNCTION_DEFINE1(int, raise, int, sig) {
    tvm_sig_enter();

    if (stop_signal_dropped(sig, current->tsk_pid)) {
        tvm_sig_leave();
        return 0;
    }

    int r = pthread_kill(pthread_self(), sig);
    if (r != 0) {
        errno = r;
        r = -1;
    }
    
    tvm_sig_leave();
    return r;
}

FUNCTION_DEFINE2(int, kill, pid_t, pid, int, sig) {
    tvm_sig_enter();

    if (pid == 0) {
        task_lock(current);
        pid = -current->tsk_pgid;
        task_unlock(current);

        // should be positive but not 1 (-1 isn't want we would like to call kill with)
        if (pid >= -1)
            panic("kill self pgid (%d)", pid);
    }

    // Kill all processes with permissions not supported
    if (pid == -1) {
        errno = EINVAL;
        tvm_sig_leave();
        return -1;
    }

    int self = 0;
    int ntasks = 0;
    int kills_confirmed = 0;
    
    pthread_mutex_lock(&tasks_lock);
    struct task *t;
    // main_task is the list head and not visited by list_for_each_entry,
    // so check it explicitly first
    if (main_task->tsk_pid == pid || main_task->tsk_pgid == -pid) {
        if (main_task == current)
            self = 1;
        else {
            ntasks++;
            task_lock(main_task);
            if (main_task->tsk_state & TS_ZOMBIE)
                kills_confirmed++;
            else if (0 == task_kill_locked(main_task, sig))
                kills_confirmed++;
            task_unlock(main_task);
        }
    }
    list_for_each_entry(t, &main_task->tsk_list, tsk_list) {
        if (t->tsk_pid == pid || t->tsk_pgid == -pid) {
            if (t == current) {
                self = 1;
                continue;
            }

            ntasks++;
            task_lock(t);
            if (t->tsk_state & TS_ZOMBIE)
                kills_confirmed++;
            else if (0 == task_kill_locked(t, sig))
                kills_confirmed++;
            task_unlock(t);
        }
    }
    pthread_mutex_unlock(&tasks_lock);

    if (!self && !ntasks) {
        errno = ESRCH;
        tvm_sig_leave();
        return -1;
    }
    
    // must save self for last because if we die from the signal, we miss some of the target recipients
    if (self) {
        if (0 == raise(sig))
            kills_confirmed++;
    }

    tvm_sig_leave();
    return (kills_confirmed ? 0 : -1);
}

// killpg(2) as libc defines it: kill() of the process group, -pgrp (0 being
// the caller's own group). bash was configured ac_cv_func_killpg=no, which
// gave it the same definition from its lib/sh/oslib.c; a guest killpg()
// reaching the host would have signalled host process groups.
FUNCTION_DEFINE2(int, killpg, pid_t, pgrp, int, sig) {
    if (pgrp < 0) {
        errno = EINVAL;
        return -1;
    }
    return kill(pgrp ? -pgrp : 0, sig);
}

static int tvm_sigprocmask(int how, const sigset_t *set, sigset_t *oldset) {
    if (oldset)
        *oldset = current_thread->thr_sigmask;

    if (set) {
        switch (how) {
        case SIG_BLOCK:
            for (int i = 1; i < MAX_SIGNALS; i++)
                if (sigismember(set, i))
                    sigaddset(&current_thread->thr_sigmask, i);
            break;
        case SIG_UNBLOCK:
            for (int i = 1; i < MAX_SIGNALS; i++)
                if (sigismember(set, i))
                    sigdelset(&current_thread->thr_sigmask, i);
            break;
        case SIG_SETMASK:
            current_thread->thr_sigmask = *set;
            break;
        default:
            errno = EINVAL;
            return -1;
        }
        // SIGKILL and SIGSTOP can never be blocked
        sigdelset(&current_thread->thr_sigmask, SIGKILL);
        sigdelset(&current_thread->thr_sigmask, SIGSTOP);

        // Drain any pending signals that are now unblocked
        if (how == SIG_UNBLOCK || how == SIG_SETMASK)
            tvm_sig_drain();
    }

    return 0;
}

FUNCTION_DEFINE3(int, sigprocmask, int, how, const sigset_t *, set, sigset_t *, oldset) {
    return tvm_sigprocmask(how, set, oldset);
}

FUNCTION_DEFINE3(int, pthread_sigmask, int, how, const sigset_t *, set, sigset_t *, oldset) {
    return tvm_sigprocmask(how, set, oldset);
}

FUNCTION_DEFINE1(int, sigpending, sigset_t *, set) {
    if (!set) {
        errno = EFAULT;
        return -1;
    }
    sigemptyset(set);
    uint32_t pending = 0;
    if (current_thread)
        pending |= __atomic_load_n(&current_thread->thr_pending_sigs, __ATOMIC_ACQUIRE);
    pending |= __atomic_load_n(&current->tsk_pending_sigs, __ATOMIC_ACQUIRE);

    for (int sig = 1; sig < MAX_SIGNALS; sig++) {
        if ((pending & (1u << sig)) && sigismember(&current_thread->thr_sigmask, sig))
            sigaddset(set, sig);
    }
    return 0;
}

FUNCTION_DEFINE1(int, sigsuspend, const sigset_t *, mask) {
    sigset_t old = current_thread->thr_sigmask;
    current_thread->thr_sigmask = *mask;
    sigdelset(&current_thread->thr_sigmask, SIGKILL);
    sigdelset(&current_thread->thr_sigmask, SIGSTOP);

    // Drain any pending signals that are now unblocked by the temporary mask
    tvm_sig_interrupted = 0;
    tvm_sig_drain();

    while (!tvm_sig_interrupted) {
        // A signal dispatched between the loop condition above and the real
        // sigsuspend below would be lost and park this thread forever. Block
        // all OS signals, re-check, then let the real sigsuspend atomically
        // unblock-and-wait — a signal arriving after the block stays
        // kernel-pending and is delivered inside the sigsuspend.
        sigset_t all, os_old, empty;
        sigfillset(&all);
        sigemptyset(&empty);
        CALL_FUNC(pthread_sigmask, SIG_BLOCK, &all, &os_old);
        if (!tvm_sig_interrupted)
            CALL_FUNC(sigsuspend, &empty);
        CALL_FUNC(pthread_sigmask, SIG_SETMASK, &os_old, NULL);
        // Drain again — the handler may have deferred a now-unblocked signal
        tvm_sig_drain();
    }

    current_thread->thr_sigmask = old;
    errno = EINTR;
    return -1;
}

FUNCTION_DEFINE0(int, pause) {
    tvm_sig_interrupted = 0;
    tvm_sig_drain();

    while (!tvm_sig_interrupted) {
        // Same lost-wakeup guard as sigsuspend: block, re-check, park atomically.
        sigset_t all, os_old, empty;
        sigfillset(&all);
        sigemptyset(&empty);
        CALL_FUNC(pthread_sigmask, SIG_BLOCK, &all, &os_old);
        if (!tvm_sig_interrupted)
            CALL_FUNC(sigsuspend, &empty);
        CALL_FUNC(pthread_sigmask, SIG_SETMASK, &os_old, NULL);
        tvm_sig_drain();
    }

    errno = EINTR;
    return -1;
}

// Helper: consume one pending signal from the given set.
// Returns signal number or 0 if none found.
static int tvm_consume_pending(const sigset_t *set, siginfo_t *info) {
    // Check thread-directed first
    if (current_thread) {
        uint32_t pending = __atomic_load_n(&current_thread->thr_pending_sigs, __ATOMIC_ACQUIRE);
        for (int sig = 1; sig < MAX_SIGNALS; sig++) {
            if ((pending & (1u << sig)) && sigismember(set, sig)) {
                __atomic_fetch_and(&current_thread->thr_pending_sigs, ~(1u << sig), __ATOMIC_ACQ_REL);
                if (info)
                    *info = current_thread->thr_pending_siginfo[sig];
                return sig;
            }
        }
    }
    // Check task-directed
    uint32_t pending = __atomic_load_n(&current->tsk_pending_sigs, __ATOMIC_ACQUIRE);
    for (int sig = 1; sig < MAX_SIGNALS; sig++) {
        if ((pending & (1u << sig)) && sigismember(set, sig)) {
            __atomic_fetch_and(&current->tsk_pending_sigs, ~(1u << sig), __ATOMIC_ACQ_REL);
            if (info)
                *info = current->tsk_pending_siginfo[sig];
            return sig;
        }
    }
    return 0;
}

FUNCTION_DEFINE2(int, sigwait, const sigset_t *, set, int *, sig) {
    int s;
    while (!(s = tvm_consume_pending(set, NULL))) {
        // Lost-wakeup guard: a signal deferred into the pending set between
        // the consume above and the real sigsuspend below would leave this
        // thread parked forever. Block, re-check, park atomically.
        sigset_t all, os_old, empty;
        sigfillset(&all);
        sigemptyset(&empty);
        CALL_FUNC(pthread_sigmask, SIG_BLOCK, &all, &os_old);
        if (!(s = tvm_consume_pending(set, NULL)))
            CALL_FUNC(sigsuspend, &empty);
        CALL_FUNC(pthread_sigmask, SIG_SETMASK, &os_old, NULL);
        if (s)
            break;
    }
    *sig = s;
    return 0;
}

FUNCTION_DEFINE2(int, sigwaitinfo, const sigset_t *, set, siginfo_t *, info) {
    siginfo_t local_info;
    int s;
    while (!(s = tvm_consume_pending(set, &local_info))) {
        // Lost-wakeup guard: block, re-check, park atomically (see sigwait).
        sigset_t all, os_old, empty;
        sigfillset(&all);
        sigemptyset(&empty);
        CALL_FUNC(pthread_sigmask, SIG_BLOCK, &all, &os_old);
        if (!(s = tvm_consume_pending(set, &local_info)))
            CALL_FUNC(sigsuspend, &empty);
        CALL_FUNC(pthread_sigmask, SIG_SETMASK, &os_old, NULL);
        if (s)
            break;
    }
    if (info)
        *info = local_info;
    return s;
}

FUNCTION_DEFINE3(int, sigtimedwait, const sigset_t *, set, siginfo_t *, info, const struct timespec *, timeout) {
    siginfo_t local_info;
    int s;

    // Check immediately
    if ((s = tvm_consume_pending(set, &local_info))) {
        if (info)
            *info = local_info;
        return s;
    }

    if (timeout && timeout->tv_sec == 0 && timeout->tv_nsec == 0) {
        errno = EAGAIN;
        return -1;
    }

    struct timespec deadline;
    clock_gettime(CLOCK_MONOTONIC, &deadline);
    if (timeout) {
        deadline.tv_sec += timeout->tv_sec;
        deadline.tv_nsec += timeout->tv_nsec;
        if (deadline.tv_nsec >= 1000000000L) {
            deadline.tv_sec++;
            deadline.tv_nsec -= 1000000000L;
        }
    }

    while (!(s = tvm_consume_pending(set, &local_info))) {
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        if (timeout) {
            if (now.tv_sec > deadline.tv_sec ||
                (now.tv_sec == deadline.tv_sec && now.tv_nsec >= deadline.tv_nsec)) {
                errno = EAGAIN;
                return -1;
            }
        }
        // Sleep until a signal arrives or remaining time elapses
        struct timespec remaining;
        if (timeout) {
            remaining.tv_sec = deadline.tv_sec - now.tv_sec;
            remaining.tv_nsec = deadline.tv_nsec - now.tv_nsec;
            if (remaining.tv_nsec < 0) {
                remaining.tv_sec--;
                remaining.tv_nsec += 1000000000L;
            }
        } else {
            remaining.tv_sec = 3600;
            remaining.tv_nsec = 0;
        }
        // Lost-wakeup guard: block, re-check, then pselect atomically swaps
        // to the empty mask while sleeping (see sigwait).
        sigset_t all, os_old, empty;
        sigfillset(&all);
        sigemptyset(&empty);
        CALL_FUNC(pthread_sigmask, SIG_BLOCK, &all, &os_old);
        if (!(s = tvm_consume_pending(set, &local_info)))
            pselect(0, NULL, NULL, NULL, &remaining, &empty);
        CALL_FUNC(pthread_sigmask, SIG_SETMASK, &os_old, NULL);
        if (s)
            break;
    }
    if (info)
        *info = local_info;
    return s;
}

FUNCTION_DEFINE1(char *, getenv, const char *, name) {
    // Before any task exists (e.g. early in tvm_start) there is no per-task env;
    // fall back to the real libc getenv (process environ) instead of a NULL
    // deref. This lets callers use bare getenv() uniformly.
    if (!current)
        return CALL_FUNC(getenv, name);

    char **env = NULL;
    if (*current->tsk_relocs.__environ)
        env = findenv(*current->tsk_relocs.__environ, name, strlen(name));

    if (!env)
        return NULL;

    char *ret = strchr(*env, '=');
    if (ret)
        return ret+1;

    return ret;
}

FUNCTION_DEFINE1(int, putenv, char *, string) {
    char *value = strchr(string, '=');
    size_t klen;
    if (NULL == value) {
        value = string + strlen(string);
        klen = (size_t)(value - string);
    }
    else {
        klen = (size_t)(value - string);
        value++;
    }
    
    char **env = NULL;
    if (*current->tsk_relocs.__environ)
        env = findenv(*current->tsk_relocs.__environ, string, klen);
    
    if (env) {
        *env = string;
        return 0;
    }

    char **new_environ = addenv(*current->tsk_relocs.__environ, string);
    if (!new_environ) {
        errno = ENOMEM;
        return -1;
    }

    // TODO not sure if this is a good idea
    //free(*current->tsk_relocs.__environ);
    *current->tsk_relocs.__environ = new_environ;
    return 0;
}

FUNCTION_DEFINE3(int, setenv, const char *, name, const char *, value, int, overwrite) {
    size_t name_len = strlen(name);
    size_t value_len = strlen(value);
    char *string = malloc(name_len + value_len + 2);
    if (!string) {
        errno = ENOMEM;
        return -1;
    }
    memcpy(string, name, name_len);
    string[name_len] = '=';
    memcpy(string + name_len + 1, value, value_len);

    char **env = NULL;
    if (*current->tsk_relocs.__environ)
        env = findenv(*current->tsk_relocs.__environ, name, strlen(name));

    if (env) {
        if (overwrite)
            *env = string;
        else
            free(string);
        return 0;
    }

    char **new_environ = addenv(*current->tsk_relocs.__environ, string);
    if (!new_environ) {
        free(string);
        errno = ENOMEM;
        return -1;
    }

    // TODO not sure if this is a good idea
    //free(*current->tsk_relocs.__environ);
    *current->tsk_relocs.__environ = new_environ;
    return 0;
}

FUNCTION_DEFINE1(int, unsetenv, const char *, name) {
    if (name == NULL || *name == '\0' || strchr(name, '=') != NULL)
    {
        errno = EINVAL;
        return -1;
    }

    size_t len = strlen(name);
    if (!*current->tsk_relocs.__environ)
        return 0;

    for (char **e = *current->tsk_relocs.__environ; *e != NULL;) {
	    if (!strncmp (*e, name, len) && (*e)[len] == '=') {
            for (char **e2 = e; *e2 != NULL; e2++)
                e2[0] = e2[1];
	        continue;
	    }
        e++;
    }
    return 0;
}

/////////////
// MARK: Thread-safe wrappers for non-reentrant libc
/////////////

// getusershell cache — populated once at tvm_start, after the initrc ran (single-threaded)
static char **usershell_cache = NULL;
static int    usershell_cache_count = 0;

static void usershell_cache_add(char ***list, int *n, int *cap, const char *shell)
{
    for (int i = 0; i < *n; i++)
        if (strcmp((*list)[i], shell) == 0) return;
    if (*n >= *cap - 1) {
        *cap *= 2;
        *list = krealloc(*list, *cap * sizeof(char *));
        if (!*list) return;
    }
    (*list)[(*n)++] = kstrdup(shell);
}

static void usershell_cache_init(void)
{
    if (usershell_cache) return;

    int cap = 16, n = 0;
    char **list = kalloc(cap * sizeof(char *));
    if (!list) return;

    CALL_FUNC(setusershell);
    char *sh;
    while ((sh = CALL_FUNC(getusershell)) != NULL)
        usershell_cache_add(&list, &n, &cap, sh);
    CALL_FUNC(endusershell);

    // Runs after the init task and the initrc exist, so this reads the guest
    // environment (including a `setenv TVM_SHELL=...` from the rc file).
    const char *tvm_shell = getenv("TVM_SHELL");
    if (tvm_shell && *tvm_shell)
        usershell_cache_add(&list, &n, &cap, tvm_shell);

    usershell_cache_add(&list, &n, &cap, "/tvm/bin/sh");

    list[n] = NULL;
    usershell_cache = list;
    usershell_cache_count = n;
}

FUNCTION_DEFINE1(struct passwd *, getpwnam, const char *, name) {
    struct passwd *result = NULL;
    size_t bufsz = 256;
    for (;;) {
        char *buf = malloc(bufsz);
        if (!buf) return NULL;
        int rc = getpwnam_r(name, &current->tsk_data->pw_buf, buf, bufsz, &result);
        if (rc == ERANGE) { free(buf); bufsz *= 2; continue; }
        free(current->tsk_data->pw_strings);
        current->tsk_data->pw_strings = buf;
        break;
    }
    if (!result) return NULL;
    const char *home = getenv("HOME");
    if (home && *home)
        current->tsk_data->pw_buf.pw_dir = (char *)home;
    const char *tvm_shell = getenv("TVM_SHELL");
    if (tvm_shell && *tvm_shell)
        current->tsk_data->pw_buf.pw_shell = (char *)tvm_shell;
    return &current->tsk_data->pw_buf;
}

FUNCTION_DEFINE1(struct passwd *, getpwuid, uid_t, uid) {
    struct passwd *result = NULL;
    size_t bufsz = 256;
    for (;;) {
        char *buf = malloc(bufsz);
        if (!buf) return NULL;
        int rc = getpwuid_r(uid, &current->tsk_data->pw_buf, buf, bufsz, &result);
        if (rc == ERANGE) { free(buf); bufsz *= 2; continue; }
        free(current->tsk_data->pw_strings);
        current->tsk_data->pw_strings = buf;
        break;
    }
    if (!result) return NULL;
    const char *home = getenv("HOME");
    if (home && *home)
        current->tsk_data->pw_buf.pw_dir = (char *)home;
    const char *tvm_shell = getenv("TVM_SHELL");
    if (tvm_shell && *tvm_shell)
        current->tsk_data->pw_buf.pw_shell = (char *)tvm_shell;
    return &current->tsk_data->pw_buf;
}

FUNCTION_DEFINE1(struct group *, getgrnam, const char *, name) {
    struct group *result = NULL;
    size_t bufsz = 256;
    for (;;) {
        char *buf = malloc(bufsz);
        if (!buf) return NULL;
        int rc = getgrnam_r(name, &current->tsk_data->gr_buf, buf, bufsz, &result);
        if (rc == ERANGE) { free(buf); bufsz *= 2; continue; }
        free(current->tsk_data->gr_strings);
        current->tsk_data->gr_strings = buf;
        break;
    }
    return result ? &current->tsk_data->gr_buf : NULL;
}

FUNCTION_DEFINE1(struct group *, getgrgid, gid_t, gid) {
    struct group *result = NULL;
    size_t bufsz = 256;
    for (;;) {
        char *buf = malloc(bufsz);
        if (!buf) return NULL;
        int rc = getgrgid_r(gid, &current->tsk_data->gr_buf, buf, bufsz, &result);
        if (rc == ERANGE) { free(buf); bufsz *= 2; continue; }
        free(current->tsk_data->gr_strings);
        current->tsk_data->gr_strings = buf;
        break;
    }
    return result ? &current->tsk_data->gr_buf : NULL;
}

FUNCTION_DEFINE1(struct tm *, localtime, const time_t *, timep) {
    return localtime_r(timep, &current->tsk_data->tm_buf);
}

FUNCTION_DEFINE1(struct tm *, gmtime, const time_t *, timep) {
    return gmtime_r(timep, &current->tsk_data->tm_buf);
}

FUNCTION_DEFINE1(char *, ctime, const time_t *, timep) {
    return ctime_r(timep, current->tsk_data->ctime_buf);
}

FUNCTION_DEFINE1(char *, strerror, int, errnum) {
    char *buf = current->tsk_data->strerror_buf;
    size_t sz = sizeof(current->tsk_data->strerror_buf);
#if defined(__GLIBC__) && defined(_GNU_SOURCE)
    // GNU strerror_r returns the message, which may live in a static string
    // rather than buf, leaving buf untouched. Returning buf unconditionally
    // gave every Linux guest an empty string ("ls: /x: ").
    const char *s = strerror_r(errnum, buf, sz);
    if (s != buf)
        snprintf(buf, sz, "%s", s);
#else
    if (0 != strerror_r(errnum, buf, sz))
        snprintf(buf, sz, "Unknown error %d", errnum);
#endif
    return buf;
}

FUNCTION_DEFINE_VOID0(setusershell) {
    current->tsk_data->usershell_idx = 0;
}

FUNCTION_DEFINE0(char *, getusershell) {
    if (!usershell_cache) return NULL;
    int idx = current->tsk_data->usershell_idx;
    if (idx >= usershell_cache_count) return NULL;
    current->tsk_data->usershell_idx = idx + 1;
    return usershell_cache[idx];
}

FUNCTION_DEFINE_VOID0(endusershell) {
    current->tsk_data->usershell_idx = 0;
}

FUNCTION_DEFINE3(int, execve, const char *, pathname, char * const *, argv, char * const *, envp) {
    char pathname_abs[PATH_MAX];
    if (!normalize_path(NULL, pathname, pathname_abs))
        return -1;
    pathname = pathname_abs;

    INFO("execve(\"%s\")", pathname);

    // since we zero out the callers here, this is a small snippet to get a quick stacktrace if crashes occur
    // INFO("execve fp %p lr %p", (void *)__builtin_frame_address(0), (void *)__builtin_return_address(0));
    // if ((void *)__builtin_frame_address(0)) {
    //     INFO("       fp %p lr %p", *(void **)__builtin_frame_address(0), *((void **)__builtin_frame_address(0)+1));
    //     if (*(void **)__builtin_frame_address(0)) {
    //         INFO("       fp %p lr %p", **(void ***)__builtin_frame_address(0), *(*(void ***)__builtin_frame_address(0) + 1));
    //         if (**(void ***)__builtin_frame_address(0)) {
    //             INFO("       fp %p lr %p", ***(void ****)__builtin_frame_address(0), *(**(void ****)__builtin_frame_address(0) + 1));
    //         }
    //     }
    // }

    int argc = 0;
    for (char * const*a = argv; *a && argc < INT_MAX; a++)
        argc++;

    if (argc == INT_MAX) {
        INFO("  failed(E2BIG)");
        errno = E2BIG;
        return -1;
    }

    // Skip file inspection for registered programs (they don't exist on disk).
    if (find_program(pathname))
        goto do_exec;

    // Open the file and inspect magic bytes before entering the signal-critical section.
    // If it's a #! script, rewrite pathname/argv to point at the interpreter.
    char hdr[256];
    int sfd = open(pathname, O_RDONLY);
    if (sfd < 0) {
        // Keep open()'s errno: EACCES (mode 000), ENOENT, ENOTDIR, ELOOP are
        // all valid execve errors and shells map them to different exit
        // statuses (126 vs 127) and messages.
        INFO("  failed(open) errno=%d", errno);
        return -1;
    }
    ssize_t n = read(sfd, hdr, sizeof(hdr) - 1);
    int read_errno = errno;
    close(sfd);

    INFO("  open=%d read=%zd errno=%d magic=%02x%02x%02x%02x",
         sfd, n, read_errno,
         n > 0 ? (unsigned char)hdr[0] : 0, n > 1 ? (unsigned char)hdr[1] : 0,
         n > 2 ? (unsigned char)hdr[2] : 0, n > 3 ? (unsigned char)hdr[3] : 0);

    // Execute permission, as the guest's stat() reports it (on iOS stat()
    // adds the execute bits, see there). A real kernel refuses a mode-644
    // file with EACCES before looking at its contents; the shells rely on
    // that for their "Permission denied" / 126 status. TVM's access() drops
    // X_OK on purpose, so check here.
    struct stat st;
    if (0 == stat(pathname, &st)) {
        if (S_ISDIR(st.st_mode)) {
            INFO("  failed(EACCES): directory");
            errno = EACCES;
            return -1;
        }
        if (S_ISREG(st.st_mode) && !(st.st_mode & (S_IXUSR | S_IXGRP | S_IXOTH))) {
            INFO("  failed(EACCES): mode %o", (unsigned)st.st_mode & 07777);
            errno = EACCES;
            return -1;
        }
    }

    // An empty file (or a read error) is ENOEXEC, as from a kernel; bash
    // treats an empty sample as success without running anything. Files of
    // 1-3 bytes are still checked for text below: they used to get ENOEXEC
    // here, so `printf 'ls\n' > x; ./x` took bash's in-process longjmp path
    // that the text fallback exists to avoid.
    if (n <= 0) {
        INFO("  failed(ENOEXEC)");
        errno = ENOEXEC;
        return -1;
    }

    // A text file without a #! line: a kernel says ENOEXEC and lets the
    // calling shell run it as a script itself. bash does that in-process
    // (shell_execve -> sh_longjmp(subshell_top_level)), which cannot work in
    // a TVM fork child: the fork copied the parent's stack frames to a new
    // stack but subshell_top_level was setjmp'd on the parent's stack (shell.c
    // main), so the longjmp lands on the parent's live stack and the child
    // dies with a corrupted heap (rmem "invalid pointer" abort). Run such
    // files through the shell here instead, the way tvm_execvpe() already
    // does for execvp(). "Text" is what bash's check_binary_file() would
    // also treat as a script: no NUL byte in the sample; anything else (and
    // any non-Mach-O binary) still gets ENOEXEC, which bash reports as
    // "cannot execute binary file" without longjmp'ing.
    int has_shebang = n >= 2 && hdr[0] == '#' && hdr[1] == '!';
    if (!has_shebang && memchr(hdr, 0, n) == NULL
        && !(n >= 4 && memcmp(hdr, "\xcf\xfa\xed\xfe", 4) == 0) /* MH_MAGIC_64 */
        && !(n >= 4 && memcmp(hdr, "\x7f" "ELF", 4) == 0)) {
        const char *sh = getenv("TVM_SHELL");
        if (!sh || !*sh) sh = "/bin/sh";
        INFO("execve: text file without #!, running it with %s", sh);
        n = snprintf(hdr, sizeof(hdr), "#!%s", sh);
        if (n >= (ssize_t)sizeof(hdr)) {
            errno = ENOEXEC;
            return -1;
        }
        has_shebang = 1;
    }

    // too short to be a loadable image, and not a script
    if (!has_shebang && n < 4) {
        INFO("  failed(ENOEXEC)");
        errno = ENOEXEC;
        return -1;
    }

    if (has_shebang) {
        hdr[n] = '\0';
        char *eol = strchr(hdr + 2, '\n');
        if (eol) *eol = '\0';

        char *interp = hdr + 2;
        while (*interp == ' ' || *interp == '\t') interp++;

        char *interp_arg = NULL;
        char *sp = strchr(interp, ' ');
        if (!sp) sp = strchr(interp, '\t');
        if (sp) {
            *sp = '\0';
            interp_arg = sp + 1;
            while (*interp_arg == ' ' || *interp_arg == '\t') interp_arg++;
            if (*interp_arg == '\0') interp_arg = NULL;
        }

        if (!*interp) {
            errno = ENOEXEC;
            return -1;
        }

        // "#!/usr/bin/env prog [arg]": resolve prog through PATH the way
        // env(1) would. The host's /usr/bin/env is not loadable here (fat
        // binary on macOS, shared cache on iOS), and this is how most
        // portable scripts name their interpreter.
        char envprog[PATH_MAX];
        const char *ibase = strrchr(interp, '/');
        ibase = ibase ? ibase + 1 : interp;
        if (interp_arg && 0 == strcmp(ibase, "env")) {
            char *prog = interp_arg;
            char *rest = prog;
            while (*rest && *rest != ' ' && *rest != '\t') rest++;
            if (*rest) {
                *rest++ = '\0';
                while (*rest == ' ' || *rest == '\t') rest++;
            }
            interp_arg = *rest ? rest : NULL;
            if (strchr(prog, '/')) {
                interp = prog;
            } else if (0 == tvm_path_search(prog, envprog)) {
                interp = envprog;
            } else {
                INFO("execve shebang: env: %s not found in PATH", prog);
                return -1;
            }
        }

        INFO("execve shebang: interp=\"%s\" arg=\"%s\" script=\"%s\"",
             interp, interp_arg ? interp_arg : "(none)", pathname);

        // Save the script path before overwriting pathname_abs with the interpreter
        char *script_path = tvm_alloca_zero(strlen(pathname) + 1);
        strcpy(script_path, pathname);

        // Resolve interpreter path (overwrites pathname_abs)
        if (!normalize_path(NULL, interp, pathname_abs)) {
            errno = ENOENT;
            return -1;
        }

        // The interpreter must be something TVM can load: a thin Mach-O or
        // ELF. The host's /bin/bash, /bin/sh, /usr/bin/python3 ... are fat
        // binaries on macOS and shared-cache stubs on iOS, so a plain
        // "#!/bin/bash" script would fail with ENOEXEC — which bash then
        // handles by running the file in-process via longjmp, corrupting a
        // TVM fork child (see the #!-less case above). Fall back to the same
        // interpreter name through PATH (e.g. /tvm/bin/bash); if that fails
        // too, report a missing interpreter (ENOENT), as Linux does.
        if (!tvm_loadable_image(pathname_abs)) {
            const char *ibn = strrchr(interp, '/');
            ibn = ibn ? ibn + 1 : interp;
            char alt[PATH_MAX];
            if (0 == tvm_path_search(ibn, alt) && 0 != strcmp(alt, pathname_abs) && tvm_loadable_image(alt)) {
                INFO("execve shebang: %s not loadable, using %s", pathname_abs, alt);
                snprintf(pathname_abs, sizeof(pathname_abs), "%s", alt);
            } else {
                INFO("execve shebang: interpreter %s not loadable (ENOENT)", pathname_abs);
                errno = ENOENT;
                return -1;
            }
        }
        pathname = pathname_abs;

        // Rebuild argv: [interp, interp_arg?, script, original argv[1:]]
        int extra = interp_arg ? 2 : 1;
        int new_argc = extra + 1 + (argc > 0 ? argc - 1 : 0);
        char **new_argv = alloca(sizeof(char *) * (new_argc + 1));
        int i = 0;
        new_argv[i++] = interp;
        if (interp_arg) new_argv[i++] = interp_arg;
        new_argv[i++] = script_path;
        for (int j = 1; j < argc; j++)
            new_argv[i++] = argv[j];
        new_argv[i] = NULL;
        argv = new_argv;
        argc = i;
    }

do_exec:
    // Deep-copy argv and envp onto the stack — the originals may point into the
    // caller's emulator stack or shebang_buf which we're about to destroy.
    int envc = 0;
    if (envp)
        for (char * const *e = envp; *e; e++)
            envc++;

    // These copies stay in this frame, below thr_fork_fp, for the program's
    // whole life, so every fork it does copies and fixes them up: zero-pad
    // them (see tvm_alloca_zero).
    char **new_argv = alloca(sizeof(char *) * (argc + 1));
    for (int i = 0; i < argc; i++) {
        size_t len = strlen(argv[i]) + 1;
        new_argv[i] = tvm_alloca_zero(len);
        memcpy(new_argv[i], argv[i], len);
    }
    new_argv[argc] = NULL;

    char **new_envp = alloca(sizeof(char *) * (envc + 1));
    for (int i = 0; i < envc; i++) {
        size_t len = strlen(envp[i]) + 1;
        new_envp[i] = tvm_alloca_zero(len);
        memcpy(new_envp[i], envp[i], len);
    }
    new_envp[envc] = NULL;

    argv = new_argv;
    envp = new_envp;

    tvm_sig_enter();

    struct ulinker *new_linker = ulinker_create((void *)current);
    if (!new_linker) {
        INFO("   failed(ENOMEM)");
        errno = ENOMEM;
        tvm_sig_leave();
        return -1;
    }
    struct emulator_context *new_emulator = emulator_context_create(new_linker);
    if (!new_emulator) {
        ulinker_destroy(new_linker);
        INFO("   failed(ENOMEM)");
        errno = ENOMEM;
        tvm_sig_leave();
        return -1;
    }

    struct ulinker *old_linker = current->tsk_linker;
    struct emulator_context *old_emulator = current_thread->thr_emulator;
    current->tsk_linker = new_linker;
    current_thread->thr_emulator = new_emulator;

    main_func_t main_func = find_program(pathname);
    if (!main_func) {

        int dlflags = RTLD_NOW | (is_noemu_binary(pathname) ? RTLD_NOEMU : 0);
        void *handle = ulinker_dlopen(new_linker, pathname, dlflags);
        if (!handle) {
            current->tsk_linker = old_linker;
            current_thread->thr_emulator = old_emulator;
            emulator_context_destroy(new_emulator);
            ulinker_destroy(new_linker);
            errno = ENOEXEC;
            tvm_sig_leave();
            return -1;
        }

        void *entry = ulinker_dlsym(new_linker, handle, "main", 1);
        if (!entry) {
            INFO("failed to load executable library \"%s\": symbol \"main\" not found", pathname);
            current->tsk_linker = old_linker;
            current_thread->thr_emulator = old_emulator;
            emulator_context_destroy(new_emulator);
            ulinker_destroy(new_linker);
            errno = EINVAL;
            tvm_sig_leave();
            return -1;
        }

        main_func = (main_func_t)entry;
    }

    if (!main_func) {
        INFO("  failed(ENOENT)");
        current->tsk_linker = old_linker;
        current_thread->thr_emulator = old_emulator;
        emulator_context_destroy(new_emulator);
        ulinker_destroy(new_linker);
        errno = ENOENT;
        tvm_sig_leave();
        return -1;
    }

    // No failing from here!

    // next fork, no reason to copy stack frames above this one
    current_thread->thr_fork_fp = (void *)__builtin_frame_address(0);

    // cleanup old emulator and linker
    emulator_context_destroy(old_emulator);
    ulinker_destroy(old_linker);

    // we've possibly unloaded the caller
    // gotta make sure the unwinder won't crash by setting the previous frame pointers to NULL (return address is unmapped!)
    current_thread->thr_fork_fp = (void *)__builtin_frame_address(0);
    void **frame = (void **)__builtin_frame_address(0);
    frame[0] = NULL;
    frame[1] = NULL;

    // reset stale call chain from the old executable
    call = NULL;

    // TODO: kill all other threads
    if (current->tsk_threads_cnt != 1)
        panic("_exit tsk_threads_cnt");

    const char *basename = (const char *)strrchr(pathname, '/');
    if (!basename)
        basename = pathname;
    else
        basename++;
    task_lock(current);
    memset(current->tsk_comm, 0, sizeof(current->tsk_comm));
    strncpy(current->tsk_comm, basename, sizeof(current->tsk_comm) - 1);
    memset(&current->tsk_exits, 0, sizeof(current->tsk_exits));
    memset(&current->tsk_exit_args, 0, sizeof(current->tsk_exit_args));
    task_unlock(current);

    // close all file-descriptors marked as CLOEXEC
    int last_errno = errno;
    for (int i = 0; i < MAX_FILES; i++) {
        int fd_flags = fcntl(i, F_GETFD);
        if (-1 == fd_flags) {
            VERIFY(errno == EBADF);
            continue;
        }

        if (fd_flags & FD_CLOEXEC)
            VERIFY(0 == close(i));
    }
    errno = last_errno;

    task_lock(current);
    task_exec_reset_streams_locked(current);
    task_unlock(current);

    // POSIX execve: caught signals go back to the default, ignored signals
    // stay ignored (so `trap '' PIPE`, nohup and shells backgrounding a job
    // with SIGINT/SIGQUIT ignored behave as on a real system).
    struct sigaction act = { .sa_handler = SIG_DFL };
    for (int i = 1; i < MAX_SIGNALS; i++) {
        if (i == SIGKILL || i == SIGSTOP)
            continue;
        struct sigaction *cur = current->tsk_sighandlers + i;
        if (!(cur->sa_flags & SA_SIGINFO) && cur->sa_handler == SIG_IGN)
            continue;
        if (0 != sigaction(i, &act, NULL))
            panic("sigaction(%d)", i);
    }

    // POSIX: execve clears the signal mask
    sigemptyset(&current_thread->thr_sigmask);

    // TODO if 0, 1, 2 are closed we can be nice and open /dev/full, /dev/null and /dev/null for the program

    // TODO purge rmem heap + mmaps

    memset(tvm_tls()->tls_tty_name, 0, sizeof(tvm_tls()->tls_tty_name));
    memset(tvm_tls()->tls_pts_name, 0, sizeof(tvm_tls()->tls_pts_name));

    uint64_t ret;

    tvm_sig_leave();

    // Reset signal depth — the exec path accumulates nested tvm_sig_enter
    // calls from file I/O during setup that don't fully unwind
    tvm_sig_blocked = 0;

    // reinitialize relocations

    // __environ on linux, environ on apple
    current->tsk_relocs.__environ = ulinker_dlsym(current->tsk_linker, RTLD_DEFAULT, "__environ", 0)
                                 ?: ulinker_dlsym(current->tsk_linker, RTLD_DEFAULT, "environ", 0)
                                 ?: &current->tsk_data->__environ;
    current->tsk_relocs.optind = ulinker_dlsym(current->tsk_linker, RTLD_DEFAULT, "optind", 0) ?: &current->tsk_data->optind;
    current->tsk_relocs.opterr = ulinker_dlsym(current->tsk_linker, RTLD_DEFAULT, "opterr", 0) ?: &current->tsk_data->opterr;
    current->tsk_relocs.optopt = ulinker_dlsym(current->tsk_linker, RTLD_DEFAULT, "optopt", 0) ?: &current->tsk_data->optopt;
    current->tsk_relocs.optarg = ulinker_dlsym(current->tsk_linker, RTLD_DEFAULT, "optarg", 0) ?: &current->tsk_data->optarg;

    // re-initialize tvm data section
    memset(current->tsk_data, 0, sizeof(*current->tsk_data));
    *current->tsk_relocs.__environ = new_envp;
    *current->tsk_relocs.optind = 1;
    *current->tsk_relocs.opterr = 1;
    current->tsk_data->optreset = 0;

    int callerr = emulator_call(&ret, (uint64_t)main_func,
                  (uint64_t)argc,
                  (uint64_t)argv,
                  (uint64_t)envp,
                0, 0, 0);
    if (callerr)
        printf("execve emulator_call failed!\n");

    exit((int)ret);
}

FUNCTION_DEFINE_VA2(int, execl, const char *, pathname, const char *, arg/*, ... , (char *) NULL */) {
    size_t argc;
    va_list ap;
    
    va_copy(ap, __ap);
    for (argc = 1; va_arg(ap, const char *) && argc < INT_MAX;)
        argc++;
    va_end(ap);

    if (argc == INT_MAX) {
        errno = E2BIG;
        return -1;
    }

    char **argv = malloc(sizeof(char *) * (argc+1));
    if (!argv) {
        errno = ENOMEM;
        return -1;
    }
    argv[argc] = NULL;
    
    argv[0] = (char *)arg;
    for (char **a = (argv+1); a < (argv + argc); a++)
        *a = va_arg(__ap, char *);

    int r = execv(pathname, argv);
    free(argv);
    return r;
}

FUNCTION_DEFINE_VA2(int, execlp, const char *, file, const char *, arg/*, ... , (char *) NULL */) {
    size_t argc;
    va_list ap;
    
    va_copy(ap, __ap);
    for (argc = 0; va_arg(ap, const char *) && argc < INT_MAX;)
        argc++;
    va_end(ap);

    if (argc == INT_MAX) {
        errno = E2BIG;
        return -1;
    }

    char **argv = malloc(sizeof(char *) * (argc+1));
    if (!argv) {
        errno = ENOMEM;
        return -1;
    }
    argv[argc] = NULL;

    for (char **a = argv; a < (argv + argc); a++)
        *a = va_arg(__ap, char *);

    int r = execvp(file, argv);
    free(argv);
    return r;
}

FUNCTION_DEFINE_VA2(int, execle, const char *, pathname, const char *, arg/*, ... , (char *) NULL, char *const envp[] */) {
    size_t argc;
    va_list ap;
    
    va_copy(ap, __ap);
    for (argc = 0; va_arg(ap, const char *) && argc < INT_MAX;)
        argc++;
    va_end(ap);

    if (argc == INT_MAX) {
        errno = E2BIG;
        return -1;
    }

    char **argv = malloc(sizeof(char *) * (argc+1));
    if (!argv) {
        errno = ENOMEM;
        return -1;
    }
    argv[argc] = NULL;

    for (char **a = argv; a < (argv + argc); a++)
        *a = va_arg(__ap, char *);
    char **envp = va_arg(__ap, char **);

    int r = execve(pathname, argv, envp);
    free(argv);
    return r;
}

FUNCTION_DEFINE2(int, execv, const char *, pathname, char * const *, argv) {
    return execve(pathname, argv, *current->tsk_relocs.__environ);
}

/**
 * Resolve a bare program name the way execvp() does: registered programs
 * first, then each PATH entry (guest environment). On success `out` holds
 * the pathname; on failure returns -1 with errno EACCES or ENOENT.
 */
static int tvm_path_search(const char *file, char *out)
{
    const char *pathname = find_program_pathname(file);
    if (pathname) {
        snprintf(out, PATH_MAX, "%s", pathname);
        return 0;
    }
    const char *path = getenv("PATH");
    if (!path) path = "/usr/local/bin:/bin:/usr/bin";
    int eacces = 0;
    while (*path) {
        const char *end = strchr(path, ':');
        size_t len = end ? (size_t)(end - path) : strlen(path);
        if (len > 0 && len + 1 + strlen(file) < PATH_MAX) {
            memcpy(out, path, len);
            out[len] = '/';
            strcpy(out + len + 1, file);
            // An executable regular file (or a registered program), as
            // execvp() requires. This used access(X_OK), but the interposed
            // access() drops X_OK, so the first PATH entry that merely
            // existed won -- a directory or a non-executable file -- and
            // execve then failed with EACCES instead of the search moving on
            // to the next entry.
            if (find_program(out))
                return 0;
            struct stat st;
            if (0 == stat(out, &st)) {
                if (S_ISREG(st.st_mode) && (st.st_mode & (S_IXUSR | S_IXGRP | S_IXOTH)))
                    return 0;
                eacces = 1;
            }
            else if (errno == EACCES)
                eacces = 1;
        }
        path = end ? end + 1 : path + strlen(path);
    }
    errno = eacces ? EACCES : ENOENT;
    return -1;
}

// PATH search + ENOEXEC-shell fallback, shared by execvp (all platforms) and
// execvpe (Linux only — that libc function doesn't exist on Apple). The two
// differ only in the environment handed to execve.
static int tvm_execvpe(const char *file, char * const *argv,
                       char * const *envp) {
    char file_abs[PATH_MAX];
    if (strchr(file, '/')) {
        if (!normalize_path(NULL, file, file_abs))
            return -1;
    }
    else if (0 != tvm_path_search(file, file_abs)) {
        return -1;
    }
    file = file_abs;
    int rc = execve(file, argv, envp);
    if (errno == ENOEXEC) {
        const char *sh = getenv("TVM_SHELL");
        if (!sh || !*sh) sh = "/bin/sh";
        int argc = 0;
        while (argv[argc]) argc++;
        char **nargv = kalloc((argc + 2) * sizeof(char *));
        if (!nargv) return -1;
        nargv[0] = (char *)sh;
        nargv[1] = (char *)file;
        for (int i = 1; i <= argc; i++)
            nargv[i + 1] = argv[i];
        rc = execve(sh, nargv, envp);
        kfree(nargv);
    }
    return rc;
}

FUNCTION_DEFINE2(int, execvp, const char *, file, char * const *, argv) {
    INFO("execvp: %s", file);
    return tvm_execvpe(file, argv, *current->tsk_relocs.__environ);
}

FUNCTION_DEFINE1(int, system, const char *, command) {
    if (!command)
        return 1; // shell is available

    const char *sh = getenv("TVM_SHELL");
    if (!sh || !*sh)
        sh = "/bin/sh";

    // POSIX: ignore SIGINT/SIGQUIT, block SIGCHLD during system()
    struct sigaction ign = { .sa_handler = SIG_IGN };
    struct sigaction old_int, old_quit;
    sigaction(SIGINT, &ign, &old_int);
    sigaction(SIGQUIT, &ign, &old_quit);

    sigset_t chld, old_mask;
    sigemptyset(&chld);
    sigaddset(&chld, SIGCHLD);
    sigprocmask(SIG_BLOCK, &chld, &old_mask);

    pid_t pid = fork();
    if (pid == -1) {
        sigaction(SIGINT, &old_int, NULL);
        sigaction(SIGQUIT, &old_quit, NULL);
        sigprocmask(SIG_SETMASK, &old_mask, NULL);
        return -1;
    }

    if (pid == 0) {
        sigaction(SIGINT, &old_int, NULL);
        sigaction(SIGQUIT, &old_quit, NULL);
        sigprocmask(SIG_SETMASK, &old_mask, NULL);
        execl(sh, "sh", "-c", command, (char *)NULL);
        _exit(127);
    }

    // parent: wait for child
    int status;
    while (waitpid(pid, &status, 0) == -1) {
        if (errno != EINTR) {
            status = -1;
            break;
        }
    }

    sigaction(SIGINT, &old_int, NULL);
    sigaction(SIGQUIT, &old_quit, NULL);
    sigprocmask(SIG_SETMASK, &old_mask, NULL);

    return status;
}

#if defined(__linux__)
FUNCTION_DEFINE5(int, openpty, int *, amaster, int *, aslave, char *, name, const struct termios *, termp, const struct winsize *, winp) {
#elif defined(__APPLE__)
FUNCTION_DEFINE5(int, openpty, int *, amaster, int *, aslave, char *, name, struct termios *, termp, struct winsize *, winp) {
#endif
    // TODO
    if (name || termp || winp)
        panic("openpty todo");

    tvm_sig_enter();

    struct file *master = ptm_open();
    if (!master) {
        tvm_sig_leave();
        return -1;
    }

    struct tty *tt = tty_for_file_locked(master, NULL);
    VERIFY(tt);

    file_unlock(master, 0);
    task_lock(current);
    file_lock(master, 0);

    // locks at this point:
    //   tty -> task -> master

    int fmaster = task_assign_file_locked(current, master, 0);
    if (-1 == fmaster) {
        task_unlock(current);
        tty_unlock(tt, 1);
        file_unlock(master, 1);
        tvm_sig_leave();
        return -1;
    }

    // master is in current, so this wouldn't trigger dealloc
    file_unlock(master, 1);

    struct file *slave = pts_open_locked(tt);
    if (!slave) {
        task_unlock(current);
        tty_unlock(tt, 1);
        VERIFY(0 == close(fmaster));
        tvm_sig_leave();
        return -1;
    }

    // locks at this point:
    //   tty -> task -> slave

    int fslave = task_assign_file_locked(current, slave, 0);
    if (-1 == fmaster) {
        task_unlock(current);
        tty_unlock(tt, 1);
        file_unlock(slave, 1);
        VERIFY(0 == close(fmaster));
        tvm_sig_leave();
        return -1;
    }

    task_unlock(current);
    tty_unlock(tt, 1);
    file_unlock(slave, 1);

    tvm_sig_leave();

    *amaster = fmaster;
    *aslave = fslave;
    return 0;
}

FUNCTION_DEFINE1(int, isatty, int, fd) {
    // fall back to check outside vm for internalized file-descriptors that are TTYS
    if (islocaltty(fd))
        return 1;
    
    return CURRENT_RFD(fd, CALL_FUNC(isatty, __rfd));
}

FUNCTION_DEFINE3(int, ttyname_r, int, fd, char *, buf, size_t, buflen) {
    if (!islocaltty(fd))
        return CURRENT_RFD(fd, CALL_FUNC(ttyname_r, __rfd, buf, buflen));
    
    tvm_sig_enter();

    int ttymode;
    struct tty *tt = tty_for_nfd(fd, &ttymode);
    if (!tt) {
        tvm_sig_leave();
        return -1;
    }
    
    if (ttymode == TTM_MASTER) {
        snprintf(buf, buflen, "%s", PTMX_FILE);
        tty_unlock(tt, 1);
        tvm_sig_leave();
        return 0;
    }

    int ret = tty_slavename_locked(tt, buf, buflen);
    if (0 != ret)
        ret = errno;

    tty_unlock(tt, 1);
    tvm_sig_leave();
    return ret;
}

FUNCTION_DEFINE1(char *, ttyname, int, fd) {
    if (!islocaltty(fd))
        return (char *)CURRENT_RFD(fd, (long)CALL_FUNC(ttyname, __rfd));

    int r = ttyname_r(fd, tty_name, sizeof(tty_name));
    if (r) {
        errno = r;
        return NULL;
    }

    return tty_name;
}

FUNCTION_DEFINE3(int, ptsname_r, int, fd, char *, buf, size_t, buflen) {
    if (!islocaltty(fd))
        return CURRENT_RFD(fd, CALL_FUNC(ptsname_r, __rfd, buf, buflen));
    
    tvm_sig_enter();
    struct tty *tt = tty_for_nfd(fd, NULL);
    if (!tt) {
        tvm_sig_leave();
        return -1;
    }
    
    int ret = tty_slavename_locked(tt, buf, buflen);
    if (0 != ret)
        ret = errno;
    
    tty_unlock(tt, 1);
    tvm_sig_leave();

    return ret;
}

FUNCTION_DEFINE1(char *, ptsname, int, fd) {
    if (!islocaltty(fd))
        return (char *)CURRENT_RFD(fd, (long)CALL_FUNC(ptsname, __rfd));

    int r = ptsname_r(fd, pts_name, sizeof(pts_name));
    if (r) {
        errno = r;
        return NULL;
    }

    return pts_name;
}

FUNCTION_DEFINE1(pid_t, tcgetpgrp, int, fd) {
    if (!islocaltty(fd)) {
        // For native (host) TTYs, TVM tasks are threads sharing one real PID
        // so real tcsetpgrp/tcgetpgrp cannot work.  Virtualise it.
        return CURRENT_RFD(fd, CALL_FUNC(isatty, __rfd) ? (long)current->tsk_pgid : (long)CALL_FUNC(tcgetpgrp, __rfd));
    }

    pid_t ret;
    if (0 != CURRENT_FILE(fd, ttyop_ioctl(__file, TIOCGPGRP, &ret)))
        return -1;
    return ret;
}

FUNCTION_DEFINE2(int, tcsetpgrp, int, fd, pid_t, pgrp) {
    if (!islocaltty(fd)) {
        // For native (host) TTYs, TVM tasks are threads sharing one real PID
        // so real tcsetpgrp/tcgetpgrp cannot work.  Virtualise it: no-op success.
        return CURRENT_RFD(fd, CALL_FUNC(isatty, __rfd) ? 0L : (long)CALL_FUNC(tcsetpgrp, __rfd, pgrp));
    }

    return CURRENT_FILE(fd, ttyop_ioctl(__file, TIOCSPGRP, &pgrp));
}

FUNCTION_DEFINE2(int, tcgetattr, int, fd, struct termios *, termios_p) {
    if (!islocaltty(fd))
        return CURRENT_RFD(fd, CALL_FUNC(tcgetattr, __rfd, termios_p));

#if defined(__linux__)
    return CURRENT_FILE(fd, ttyop_ioctl(__file, TCGETS, termios_p));
#elif defined(__APPLE__)
    return CURRENT_FILE(fd, ttyop_ioctl(__file, TIOCGETA, termios_p));
#endif
}

FUNCTION_DEFINE3(int, tcsetattr, int, fd, int, optional_actions, const struct termios *, termios_p) {
    if (!islocaltty(fd))
        return CURRENT_RFD(fd, CALL_FUNC(tcsetattr, __rfd, optional_actions, termios_p));
    
    int request;
    switch (optional_actions) {
        default:
            errno = EINVAL;
            return -1;
        
        case TCSANOW:
#if defined(__linux__)
            request = TCSETS;
#elif defined(__APPLE__)
            request = TIOCSETA;
#endif
            break;
        
        case TCSADRAIN:
#if defined(__linux__)
            request = TCSETSW;
#elif defined(__APPLE__)
            request = TIOCSETAW;
#endif
            break;
        
        case TCSAFLUSH:
#if defined(__linux__)
            request = TCSETSF;
#elif defined(__APPLE__)
            request = TIOCSETAF;
#endif
            break;
    }

    return CURRENT_FILE(fd, ttyop_ioctl(__file, (unsigned)request, (void *)termios_p));
}

FUNCTION_DEFINE2(int, tcsendbreak, int, fd, int, duration) {
    if (!islocaltty(fd))
        return CURRENT_RFD(fd, CALL_FUNC(tcsendbreak, __rfd, duration));
    
#if defined(__linux__)
    return CURRENT_FILE(fd, ttyop_ioctl(__file, TCSBRK, (void *)(long)duration));
#elif defined(__APPLE__)
    int ret = -1;
    CURRENT_FILE(fd, ({
        if (ttyop_ioctl(__file, TIOCSBRK, 0) != -1) {
            usleep(400000);
            if (ttyop_ioctl(__file, TIOCCBRK, 0) != -1)
                ret = 0;
        }
        0;
    }));
    return ret;
#endif
}

FUNCTION_DEFINE1(int, tcdrain, int, fd) {
    if (!islocaltty(fd))
        return CURRENT_RFD(fd, CALL_FUNC(tcdrain, __rfd));
    
#if defined(__linux__)
    return tcsendbreak(fd, 1);
#elif defined(__APPLE__)
    return CURRENT_FILE(fd, ttyop_ioctl(__file, TIOCDRAIN, NULL));
#endif
}

FUNCTION_DEFINE2(int, tcflush, int, fd, int, queue_selector) {
    if (!islocaltty(fd))
        return CURRENT_RFD(fd, CALL_FUNC(tcflush, __rfd, queue_selector));

#if defined(__linux__)
    return CURRENT_FILE(fd, ttyop_ioctl(__file, TCFLSH, (void *)(long)queue_selector));
#elif defined(__APPLE__)
    int what = FREAD | FWRITE;
    if (queue_selector == TCIFLUSH)
        what = FREAD;
    else if (queue_selector == TCOFLUSH)
        what = FWRITE;
    return CURRENT_FILE(fd, ttyop_ioctl(__file, TIOCFLUSH, &what));
#endif
}

FUNCTION_DEFINE2(int, tcflow, int, fd, int, action) {
    if (!islocaltty(fd))
        return CURRENT_RFD(fd, CALL_FUNC(tcflow, __rfd, action));

#if defined(__linux__)
    return CURRENT_FILE(fd, ttyop_ioctl(__file, TCXONC, (void *)(long)action));
#elif defined(__APPLE__)
    unsigned long request;
    switch (action) {
	case TCOOFF:
		request = TIOCSTOP;
	case TCOON:
        request = TIOCSTART;
	case TCION:
		request = TIOCIXON;
	case TCIOFF:
		request = TIOCIXOFF;
    default:
        errno = EINVAL;
		return -1;
	}

    return CURRENT_FILE(fd, ttyop_ioctl(__file, request, NULL));
#endif
}

static void *tvm_calloc(size_t nmemb, size_t size);

// Straight to the implementation: going through the interposed calloc
// symbol ran the whole interposer wrapper (TLS lookups, can_enter_tvm, call
// chain) a second time for every malloc.
FUNCTION_DEFINE1(void *, malloc, size_t, size) {
    return tvm_calloc(1, size);
}

// malloc(0) and calloc(0, n) return a unique, freeable pointer, as the host
// libc does (Darwin and glibc alike). The arena used to return NULL, which a
// program configured against the host libc reads as out-of-memory: gnulib's
// autoconf probe records malloc(0) as non-NULL, so no replacement malloc is
// compiled in, and gnulib's regex (nano) then failed regcomp("^a") -- and
// nano's built-in quoting regex, aborting every start -- with REG_ESPACE
// ("Memory exhausted") as soon as one of its allocations came out zero-sized
// (plain "a" compiled, "^a" did not). The smallest arena object stands in
// for the zero-sized one.
FUNCTION_DEFINE2(void *, calloc, size_t, nmemb, size_t, size) {
    return tvm_calloc(nmemb, size);
}

static void *tvm_calloc(size_t nmemb, size_t size) {
    size_t total = nmemb * size;
    if (nmemb && total / nmemb != size) {
        errno = ENOMEM;
        return NULL;
    }
    if (0 == total)
        total = 1;

    struct rmem_arena *ar = arena_for_task(current);
    if (!ar)
        return NULL;

    void *p = rmem_arena_alloc(ar, total);
    if (NULL == p)
        return NULL;
    
    memset(p, 0, total);
    return p;
}

FUNCTION_DEFINE_VOID1(free, void *, ptr) {
    struct rmem_arena *ar = arena_for_task(current);
    if (!ar)
        panic("free for task without arena %p", current);

    rmem_arena_free(ar, ptr);
}

/*
 * The aligned allocators. Not interposed before, so they reached the host
 * libc and the guest later handed the host block to the interposed free(),
 * which aborted ("rmem_arena_free: invalid pointer"): OpenSSL on glibc
 * allocates its hash tables with posix_memalign (CRYPTO_aligned_alloc), and
 * curl died in OPENSSL_cleanup at every exit. They come from the task's
 * arena now, like malloc (rmem_arena_memalign: alignments up to a page).
 */
static void *tvm_memalign(size_t alignment, size_t size)
{
    struct rmem_arena *ar = arena_for_task(current);
    if (!ar)
        return NULL;
    return rmem_arena_memalign(ar, alignment, size);
}

FUNCTION_DEFINE3(int, posix_memalign, void **, memptr, size_t, alignment, size_t, size) {
    if (!alignment || (alignment & (alignment - 1)) || (alignment % sizeof(void *)))
        return EINVAL;
    int saved_errno = errno;
    void *p = tvm_memalign(alignment, size);
    if (!p) {
        int r = errno == EINVAL ? EINVAL : ENOMEM;
        errno = saved_errno;
        return r;
    }
    *memptr = p;
    return 0;
}

FUNCTION_DEFINE2(void *, aligned_alloc, size_t, alignment, size_t, size) {
    if (!alignment || (alignment & (alignment - 1))) {
        errno = EINVAL;
        return NULL;
    }
    return tvm_memalign(alignment, size);
}

FUNCTION_DEFINE1(void *, valloc, size_t, size) {
    return tvm_memalign((size_t)sysconf(_SC_PAGESIZE), size);
}

#if defined(__linux__)
// glibc: a non-power-of-two alignment is rounded up to the next one.
FUNCTION_DEFINE2(void *, memalign, size_t, alignment, size_t, size) {
    if (alignment > (size_t)sysconf(_SC_PAGESIZE)) {
        errno = EINVAL;  // beyond what the arena aligns (and the loop's range)
        return NULL;
    }
    size_t a = sizeof(void *);
    while (a < alignment)
        a <<= 1;
    return tvm_memalign(a, size);
}

FUNCTION_DEFINE1(void *, pvalloc, size_t, size) {
    size_t pagesz = (size_t)sysconf(_SC_PAGESIZE);
    size_t rounded = (size + pagesz - 1) & ~(pagesz - 1);
    if (rounded < size) {
        errno = ENOMEM;
        return NULL;
    }
    return tvm_memalign(pagesz, rounded ? rounded : pagesz);
}
#endif

FUNCTION_DEFINE2(void *, realloc, void *, ptr, size_t, size) {
    struct rmem_arena *ar = arena_for_task(current);
    if (!ar)
        panic("realloc for task without arena %p", current);

    // Zero sizes as the host libc has them (see calloc above): realloc(NULL,
    // 0) is malloc(0) everywhere; realloc(p, 0) frees p and returns a new
    // minimum-sized object on Darwin, but frees p and returns NULL with
    // glibc, which is what rmem_arena_realloc does.
    if (0 == size) {
#ifdef __APPLE__
        size = 1;
#else
        if (!ptr)
            size = 1;
#endif
    }
    return rmem_arena_realloc(ar, ptr, size);
}

FUNCTION_DEFINE1(char *, strdup, const char *, s) {
    size_t len = strlen(s);
    char *p = malloc(len + 1);
    if (!p)
        return NULL;
    memcpy(p, s, len);
    p[len] = 0;
    return p;
}

FUNCTION_DEFINE2(char *, strndup, const char *, s, size_t, n) {
    size_t len = strnlen(s, n);
    char *p = malloc(len + 1);
    if (!p)
        return NULL;
    memcpy(p, s, len);
    p[len] = 0;
    return p;
}

// wcsdup, for the same reason as strdup above: libc's version hands the
// caller memory from the host malloc, which the guest later releases through
// the interposed free(), and rmem_arena_free() aborts on a pointer outside the
// task's arena. bash's remove_wpattern() (subst.c) wcsdup()s the parameter
// for ${var#pat}/${var%pat} whenever the locale is multibyte (LANG=C.UTF-8
// leaks into the CLI backend from the host), so every such expansion took the
// shell down with SIGABRT; in the C locale the byte path never calls it.
FUNCTION_DEFINE1(wchar_t *, wcsdup, const wchar_t *, s) {
    size_t len = wcslen(s);
    wchar_t *p = malloc((len + 1) * sizeof(wchar_t));
    if (!p)
        return NULL;
    memcpy(p, s, (len + 1) * sizeof(wchar_t));
    return p;
}

FUNCTION_DEFINE_VA2(int, asprintf, char **, strp, const char *, format) {
    return vasprintf(strp, format, __ap);
}

FUNCTION_DEFINE3(int, vasprintf, char **, strp, const char *, format, va_list, ap) {
    va_list ap2;
    va_copy(ap2, ap);
    int n = vsnprintf(NULL, 0, format, ap2);
    va_end(ap2);

    if (n < 0)
        return -1;

    void *s = malloc(n + 1);
    if (!s)
        return -1;
    
    int n2 = vsnprintf(s, n + 1, format, ap);
    if (n != n2) {
        free(s);
        return -1;
    }

    *strp = s;
    return n;
}

FUNCTION_DEFINE3(ssize_t, getline, char **, lineptr, size_t *, n, FILE *, stream) {
    return getdelim(lineptr, n, '\n', stream);
}

FUNCTION_DEFINE4(ssize_t, getdelim, char **, lineptr, size_t *, n, int, delim, FILE *, stream) {
    char *tmp = NULL;
    size_t tmpn = 0;

retry:
    tvm_sig_interrupted = 0;
    tvm_sig_enter();
    struct file *file = file_for_task_fp(current, stream);
    if (!file) {
        tvm_sig_leave();
        return -1;
    }

    // TODO f_ops

    file_unlock(file, 0);
    ssize_t r = CALL_FUNC(getdelim, &tmp, &tmpn, delim, real_fp(stream));
    int last_errno = errno;
    file_lock(file, 0);
    file_unlock(file, 1);

    if (r < 0) {
        CALL_FUNC(free, tmp);
        tmp = NULL;
        tmpn = 0;
        tvm_sig_leave();
        errno = last_errno;
        // Interrupted: retry (a line cut in the middle is started over, as
        // libc's own getdelim would lose it too).
        if (stdio_host_read_interrupted(real_fp(stream), last_errno))
            goto retry;
        return -1;
    }

    if (!*lineptr || *n < (size_t)r + 1) {
        char *newp = realloc(*lineptr, (size_t)r + 1);
        if (!newp) {
            CALL_FUNC(free, tmp);
            tvm_sig_leave();
            return -1;
        }
        *lineptr = newp;
        *n = (size_t)r + 1;
    }

    memcpy(*lineptr, tmp, (size_t)r + 1);
    CALL_FUNC(free, tmp);
    tvm_sig_leave();

    return r;
}

FUNCTION_DEFINE6(void *, mmap, void *, addr, size_t, length, int, prot, int, flags, int, fd, off_t, offset) {
    struct file *file = NULL;
    
    tvm_sig_enter();

    if (fd >= 0) {
        file = file_for_task(current, fd);
        if (!file) {
            tvm_sig_leave();
            errno = EBADF;
            return MAP_FAILED;
        }
    }

    void *ret;
    if (!file || !file->f_ops || !file->f_ops->f_mmap) {
        if (file)
            file_unlock(file, 0);
        ret = CALL_FUNC(mmap, addr, length, prot, flags, (file ? file->f_rfd : -1), offset);
#ifdef TRACE_MMAP
        INFO("mmap %p-%p (mmap)", ret, (uintptr_t)ret + length);
#endif
        if (file)
            file_lock(file, 0);
    }
    else
        ret = file->f_ops->f_mmap(addr, length, prot, flags, file, offset);
    
    if (file)
        file_unlock(file, 1);
    
    tvm_sig_leave();

    return ret;
}

static void __getopt_msg(const char *a, const char *b, const char *c, size_t l)
{
	FILE *f = stderr;
// #if !defined(WIN32) && !defined(_WIN32)
// 	flockfile(f);
// #endif
	fputs(a, f);
	fwrite(b, strlen(b), 1, f);
	fwrite(c, 1, l, f);
	fputc('\n', f);
// #if !defined(WIN32) && !defined(_WIN32)
// 	funlockfile(f);
// #endif
}

FUNCTION_DEFINE3(int, getopt, int, argc, char * const *, argv, const char *, optstring) {
    int i, c, d;
	int k, l;
	char *optchar;

	/*
	 * Reset on optreset only — NOT on optind == 0.
	 *
	 * This implementation is musl's, where optind == 0 is the GNU "restart
	 * scanning" request. Darwin's plain getopt(3) is the BSD one, where optind
	 * is only an index and 0 legitimately means "start at argv[0]"; a restart
	 * is asked for with optreset instead. We emulate Darwin and run Darwin
	 * binaries, so BSD is the behaviour that matters here: netstat(1) does
	 * `argv++; argc--; optind--;` before parsing, and under the musl rule that
	 * turned into a reset to optind = 1, which skipped past every flag it was
	 * given (`netstat -rn` silently printed connections instead of routes).
	 *
	 * __getopt_long below keeps the optind == 0 reset: Darwin's getopt_long(3)
	 * is a different implementation that really does honour the GNU idiom.
	 */
	if (current->tsk_data->optreset) {
		current->tsk_data->optreset = 0;
		current->tsk_data->optpos = 0;
		*current->tsk_relocs.optind = 1;
	}

	if (*current->tsk_relocs.optind >= argc || !argv[*current->tsk_relocs.optind])
		return -1;

	if (argv[*current->tsk_relocs.optind][0] != '-') {
		if (optstring[0] == '-') {
			*current->tsk_relocs.optarg = argv[(*current->tsk_relocs.optind)++];
			return 1;
		}
		return -1;
	}

	if (!argv[*current->tsk_relocs.optind][1])
		return -1;

	if (argv[*current->tsk_relocs.optind][1] == '-' && !argv[*current->tsk_relocs.optind][2])
		return (*current->tsk_relocs.optind)++, -1;

	if (!current->tsk_data->optpos) current->tsk_data->optpos++;
	c = argv[*current->tsk_relocs.optind][current->tsk_data->optpos], k = 1;
	optchar = argv[*current->tsk_relocs.optind]+current->tsk_data->optpos;
	*current->tsk_relocs.optopt = c;
	current->tsk_data->optpos += k;

	if (!argv[*current->tsk_relocs.optind][current->tsk_data->optpos]) {
		(*current->tsk_relocs.optind)++;
		current->tsk_data->optpos = 0;
	}

	if (optstring[0] == '-' || optstring[0] == '+')
		optstring++;

	i = 0;
	d = 0;
	do {
		d = optstring[i], l = 1;
		if (l>0) i+=l; else i++;
	} while (l && d != c);

	if (d != c) {
		if (optstring[0] != ':' && opterr)
			__getopt_msg(argv[0], ": unrecognized option: ", optchar, k);
		return '?';
	}
	if (optstring[i] == ':') {
		if (optstring[i+1] == ':') *current->tsk_relocs.optarg = 0;
		else if (*current->tsk_relocs.optind >= argc) {
			if (optstring[0] == ':') return ':';
			if (opterr) __getopt_msg(argv[0],
				": option requires an argument: ",
				optchar, k);
			return '?';
		}
		if (optstring[i+1] != ':' || current->tsk_data->optpos) {
			*current->tsk_relocs.optarg = argv[(*current->tsk_relocs.optind)++] + current->tsk_data->optpos;
			current->tsk_data->optpos = 0;
		}
	}
	return c;
}

static void permute(char *const *argv, int dest, int src)
{
	char **av = (char **)argv;
	char *tmp = av[src];
	int i;
	for (i=src; i>dest; i--)
		av[i] = av[i-1];
	av[dest] = tmp;
}

static int __getopt_long_core(int argc, char *const *argv, const char *optstring, const struct option *longopts, int *idx, int longonly)
{
	*current->tsk_relocs.optarg = 0;
	if (longopts && argv[*current->tsk_relocs.optind][0] == '-' &&
		((longonly && argv[*current->tsk_relocs.optind][1] && argv[*current->tsk_relocs.optind][1] != '-') ||
		 (argv[optind][1] == '-' && argv[*current->tsk_relocs.optind][2])))
	{
		int colon = optstring[optstring[0]=='+'||optstring[0]=='-']==':';
		int i, cnt, match = -1;
		char *opt;
		for (cnt=i=0; longopts[i].name; i++) {
			const char *name = longopts[i].name;
			opt = argv[*current->tsk_relocs.optind]+1;
			if (*opt == '-') opt++;
			for (; *name && *name == *opt; name++, opt++);
			if (*opt && *opt != '=') continue;
			match = i;
			if (!*name) {
				cnt = 1;
				break;
			}
			cnt++;
		}
		if (cnt==1) {
			i = match;
			(*current->tsk_relocs.optind)++;
			*current->tsk_relocs.optopt = longopts[i].val;
			if (*opt == '=') {
				if (!longopts[i].has_arg) {
					if (colon || !opterr)
						return '?';
					__getopt_msg(argv[0],
						": option does not take an argument: ",
						longopts[i].name,
						strlen(longopts[i].name));
					return '?';
				}
				*current->tsk_relocs.optarg = opt+1;
			} else if (longopts[i].has_arg == required_argument) {
				if (!(*current->tsk_relocs.optarg = argv[*current->tsk_relocs.optind])) {
					if (colon) return ':';
					if (!opterr) return '?';
					__getopt_msg(argv[0],
						": option requires an argument: ",
						longopts[i].name,
						strlen(longopts[i].name));
					return '?';
				}
				(*current->tsk_relocs.optind)++;
			}
			if (idx) *idx = i;
			if (longopts[i].flag) {
				*longopts[i].flag = longopts[i].val;
				return 0;
			}
			return longopts[i].val;
		}
		if (argv[*current->tsk_relocs.optind][1] == '-') {
			if (!colon && opterr)
				__getopt_msg(argv[0], cnt ?
					": option is ambiguous: " :
					": unrecognized option: ",
					argv[*current->tsk_relocs.optind]+2,
					strlen(argv[*current->tsk_relocs.optind]+2));
			(*current->tsk_relocs.optind)++;
			return '?';
		}
	}
	return getopt(argc, argv, optstring);
}

static int __getopt_long(int argc, char *const *argv, const char *optstring, const struct option *longopts, int *idx, int longonly)
{
	int ret, skipped, resumed;
	if (!*current->tsk_relocs.optind || current->tsk_data->optreset) {
		current->tsk_data->optreset = 0;
		current->tsk_data->optpos = 0;
		*current->tsk_relocs.optind = 1;
	}
	if (*current->tsk_relocs.optind >= argc || !argv[*current->tsk_relocs.optind]) return -1;
	skipped = *current->tsk_relocs.optind;
	if (optstring[0] != '+' && optstring[0] != '-') {
		int i;
		for (i=*current->tsk_relocs.optind; ; i++) {
			if (i >= argc || !argv[i]) return -1;
			if (argv[i][0] == '-' && argv[i][1]) break;
		}
		*current->tsk_relocs.optind = i;
	}
	resumed = *current->tsk_relocs.optind;
	ret = __getopt_long_core(argc, argv, optstring, longopts, idx, longonly);
	if (resumed > skipped) {
		int i, cnt = *current->tsk_relocs.optind-resumed;
		for (i=0; i<cnt; i++)
			permute(argv, skipped, *current->tsk_relocs.optind-1);
		*current->tsk_relocs.optind = skipped + cnt;
	}
	return ret;
}

FUNCTION_DEFINE5(int, getopt_long, int, argc, char * const *, argv, const char *, optstring, const struct option *, longopts, int *, longindex) {
    return __getopt_long(argc, argv, optstring, longopts, longindex, 0);
}

FUNCTION_DEFINE5(int, getopt_long_only, int, argc, char * const *, argv, const char *, optstring, const struct option *, longopts, int *, longindex) {
    return __getopt_long(argc, argv, optstring, longopts, longindex, 1);
}

FUNCTION_DEFINE2(char *, realpath, const char *, file_name, char *, resolved_name)
{
    int c = 0;
    char tmp[PATH_MAX];
    tmp[0] = 0;

    tvm_sig_enter();

    task_lock(current);
    if (file_name[0] == '/')
        c = snprintf(tmp, PATH_MAX, "%s", file_name);
    else if (file_name[0] == 0)
        c = snprintf(tmp, PATH_MAX, "%s", current->tsk_cwd);
    else
        c = snprintf(tmp, PATH_MAX, "%s/%s", current->tsk_cwd, file_name);
    task_unlock(current);
    tvm_sig_leave();

    if (c >= PATH_MAX) {
        errno = ENAMETOOLONG;
        return NULL;
    }
    
    char *tmp2 = CALL_FUNC(realpath, tmp, resolved_name);
    if (!tmp2)
        return NULL;

    if (resolved_name)
        return tmp2;

    // strlen + 1: the terminator (this used to allocate one byte short and
    // strcpy() wrote past the end of the guest's allocation)
    char *ret = malloc(strlen(tmp2) + 1);
    if (ret)
        strcpy(ret, tmp2);
    CALL_FUNC(free, tmp2);
    return ret;
}

FUNCTION_DEFINE1(int, chdir, const char *, path) {
    char abs_path[PATH_MAX];
    if (!normalize_path(NULL, path, abs_path))
        return -1;
    path = abs_path;

    struct stat st;
    if (stat(path, &st) != 0) {
        return -1;
    }

    if (!S_ISDIR(st.st_mode)) {
        errno = ENOTDIR;
        return -1;
    }

    tvm_sig_enter();
    task_lock(current);
    strcpy(current->tsk_cwd, path);
    task_unlock(current);
    tvm_sig_leave();
    
    return 0;
}

FUNCTION_DEFINE1(int, fchdir, int, fildes) {
    char tmp[PATH_MAX];
    if (0 > CURRENT_RFD(fildes, fd_getpath(__rfd, tmp, sizeof(tmp))))
        return -1;

    return chdir(tmp);
}

// getcwd() returned the task's cwd string as chdir() had recorded it: the
// normalized path the program asked for, symlinks and all. POSIX has
// getcwd() return a path with no symbolic link components, and programs
// build on that: git's strbuf_realpath() (abspath.c) starts from getcwd()
// and resolves symlinks only in the part of the path it appends. With the
// cwd under /tmp (-> private/tmp) or $TMPDIR (/var -> private/var), git got
// /tmp/repo/.git/objects for one spelling of the object directory and
// /private/tmp/repo/.git/objects for another, took them for different
// object stores, and `git multi-pack-index verify --object-dir .git/objects`
// (run by every `git fsck`) dereferenced the NULL store it then got: SIGSEGV
// at 0x8. The recorded cwd stays as it is -- it is what relative paths are
// resolved against, on the cpio VFS as well -- and getcwd() now answers the
// host's physical path for it. A cwd on the VFS, which the host cannot
// resolve, is returned as recorded (the VFS has no symlinked directories on
// the way to a cwd: chdir() normalizes). Also: a buffer too small is ERANGE
// as POSIX says, rather than a silently unterminated string.
FUNCTION_DEFINE2(char *, getcwd, char *, buf, size_t, size) {
    char cwd[PATH_MAX], phys[PATH_MAX];
    tvm_sig_enter();
    task_lock(current);
    snprintf(cwd, sizeof(cwd), "%s", current->tsk_cwd);
    task_unlock(current);
    int last_errno = errno;
    const char *path = CALL_FUNC(realpath, cwd, phys) ? phys : cwd;
    errno = last_errno;
    tvm_sig_leave();

    size_t len = strlen(path) + 1;
    if (buf != NULL) {
        if (size == 0) {
            errno = EINVAL;
            return NULL;
        }
        if (len > size) {
            errno = ERANGE;
            return NULL;
        }
        memcpy(buf, path, len);
        return buf;
    }

    // getcwd(NULL, size) allocates (at least size bytes when size > 0)
    char *ret = malloc(size > len ? size : len);
    if (!ret)
        return NULL;
    memcpy(ret, path, len);
    return ret;
}

FUNCTION_DEFINE1(char *, getwd, char *, buf) {
    return getcwd(buf, PATH_MAX);
}

static int bsearch_compar_emulator(const void *a, const void *b)
{
    uint64_t ret;
    if (0 != emulator_call(&ret, (uint64_t)bsearch_compar, (uint64_t)a, (uint64_t)b, 0, 0, 0, 0))
        err(1, "emulator_call(bsearch_compar)");

    return (int)ret;
}

FUNCTION_DEFINE5(void *, bsearch, const void *, key, const void *, base, size_t, nel, size_t, width, compar_t, compar) {
    bsearch_compar = compar;
    void *ret = CALL_FUNC(bsearch, key, base, nel, width, bsearch_compar_emulator);
    bsearch_compar = NULL;
    return ret;
}

static int qsort_compar_emulator(const void *a, const void *b)
{
    uint64_t ret;
    if (0 != emulator_call(&ret, (uint64_t)qsort_compar, (uint64_t)a, (uint64_t)b, 0, 0, 0, 0))
        err(1, "emulator_call(qsort_compar)");

    return (int)ret;
}

FUNCTION_DEFINE_VOID4(qsort, void *, base, size_t, nel, size_t, width, compar_t, compar) {
    qsort_compar = compar;
    CALL_FUNC(qsort, base, nel, width, qsort_compar_emulator);
    qsort_compar = NULL;
}

static void *vtimer_thread_entry(void *_arg)
{
    struct vtimer_thread_arg *a = _arg;
    uint64_t pc = (uint64_t)(uintptr_t)a->func;
    uint64_t sigval_bits = 0;
    memcpy(&sigval_bits, &a->val, sizeof(a->val));
    free(a); // current is set → intercepted free → tsk_ar
    emulator_call(NULL, pc, sigval_bits, 0, 0, 0, 0, 0);
    return NULL;
}

FUNCTION_DEFINE3(int, timer_create, clockid_t, clockid, struct sigevent *, sevp, timer_t *, timerid) {
    tvm_sig_enter();
    pthread_mutex_lock(&vtimers_lock);
    task_lock(current);

    int notify = sevp ? sevp->sigev_notify : SIGEV_SIGNAL;
    int signo = SIGALRM;
    union sigval user_val = {0};
    start_routine_fn thread_func = NULL;

    if (sevp) {
        if (notify == SIGEV_SIGNAL || notify == SIGEV_THREAD_ID)
            signo = sevp->sigev_signo;
        user_val = sevp->sigev_value;
        if (notify == SIGEV_THREAD && sevp->sigev_notify_function)
            thread_func = vtimer_thread_entry;
    }

    // find free task slot
    int task_slot = -1;
    for (int i = 0; i < MAX_TIMERS_PER_TASK; i++) {
        if (current->tsk_timers[i] < 0) {
            task_slot = i;
            break;
        }
    }
    if (task_slot < 0) {
        task_unlock(current);
        pthread_mutex_unlock(&vtimers_lock);
        tvm_sig_leave();
        errno = EAGAIN;
        return -1;
    }

    // find free global slot
    int global_idx = -1;
    for (int i = 0; i < MAX_VIRTUAL_TIMERS; i++) {
        if (!vtimers[i].vt_active) {
            global_idx = i;
            break;
        }
    }
    if (global_idx < 0) {
        task_unlock(current);
        pthread_mutex_unlock(&vtimers_lock);
        tvm_sig_leave();
        errno = EAGAIN;
        return -1;
    }

    struct virtual_timer *vt = &vtimers[global_idx];
    memset(vt, 0, sizeof(*vt));

    vt->vt_active = 1;
    vt->vt_task = current;
    vt->vt_clockid = clockid;
    vt->vt_signo = signo;
    vt->vt_sigval = user_val;
    vt->vt_notify = notify;
    vt->vt_task_slot = task_slot;
    vt->vt_thread_func = thread_func;
    vt->vt_notify_function = (notify == SIGEV_THREAD && sevp) ? sevp->sigev_notify_function : NULL;
    // vt_deadline left at zero (not armed until timer_settime)

    current->tsk_timers[task_slot] = global_idx;
    pthread_cond_signal(&vtimers_cond);

    task_unlock(current);
    pthread_mutex_unlock(&vtimers_lock);
    tvm_sig_leave();

    // return task slot index as the timer_t
    *timerid = (timer_t)(intptr_t)task_slot;
    return 0;
}

FUNCTION_DEFINE4(int, timer_settime, timer_t, tmr, int, flags,
                 const struct itimerspec *, new_value,
                 struct itimerspec *, old_value) {
    tvm_sig_enter();
    pthread_mutex_lock(&vtimers_lock);
    task_lock(current);

    int slot = (int)(intptr_t)tmr;
    if (slot < 0 || slot >= MAX_TIMERS_PER_TASK || current->tsk_timers[slot] < 0) {
        task_unlock(current);
        pthread_mutex_unlock(&vtimers_lock);
        tvm_sig_leave();
        errno = EINVAL;
        return -1;
    }

    int global_idx = current->tsk_timers[slot];
    struct virtual_timer *vt = &vtimers[global_idx];
    if (!vt->vt_active || vt->vt_task != current) {
        task_unlock(current);
        pthread_mutex_unlock(&vtimers_lock);
        tvm_sig_leave();
        errno = EINVAL;
        return -1;
    }

    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);

    // return old value if requested
    if (old_value) {
        if (ts_is_zero(&vt->vt_deadline)) {
            memset(old_value, 0, sizeof(*old_value));
        } else {
            // remaining = deadline - now (clamp to 0 if past)
            if (ts_le(&vt->vt_deadline, &now)) {
                old_value->it_value.tv_sec = 0;
                old_value->it_value.tv_nsec = 0;
            } else {
                old_value->it_value.tv_sec = vt->vt_deadline.tv_sec - now.tv_sec;
                old_value->it_value.tv_nsec = vt->vt_deadline.tv_nsec - now.tv_nsec;
                if (old_value->it_value.tv_nsec < 0) {
                    old_value->it_value.tv_sec--;
                    old_value->it_value.tv_nsec += 1000000000L;
                }
            }
            old_value->it_interval = vt->vt_interval.it_interval;
        }
    }

    // set new value
    if (new_value->it_value.tv_sec == 0 && new_value->it_value.tv_nsec == 0) {
        // disarm
        memset(&vt->vt_deadline, 0, sizeof(vt->vt_deadline));
        memset(&vt->vt_interval, 0, sizeof(vt->vt_interval));
    } else {
        if (flags & TIMER_ABSTIME) {
            // absolute time — convert to monotonic if needed
            vt->vt_deadline = new_value->it_value;
        } else {
            // relative time — add to now
            vt->vt_deadline = ts_add(&now, &new_value->it_value);
        }
        vt->vt_interval.it_interval = new_value->it_interval;
    }

    pthread_cond_signal(&vtimers_cond);

    task_unlock(current);
    pthread_mutex_unlock(&vtimers_lock);
    tvm_sig_leave();

    return 0;
}

FUNCTION_DEFINE2(int, timer_gettime, timer_t, tmr, struct itimerspec *, curr_value) {
    tvm_sig_enter();
    pthread_mutex_lock(&vtimers_lock);
    task_lock(current);

    int slot = (int)(intptr_t)tmr;
    if (slot < 0 || slot >= MAX_TIMERS_PER_TASK || current->tsk_timers[slot] < 0) {
        task_unlock(current);
        pthread_mutex_unlock(&vtimers_lock);
        tvm_sig_leave();
        errno = EINVAL;
        return -1;
    }

    int global_idx = current->tsk_timers[slot];
    struct virtual_timer *vt = &vtimers[global_idx];
    if (!vt->vt_active || vt->vt_task != current) {
        task_unlock(current);
        pthread_mutex_unlock(&vtimers_lock);
        tvm_sig_leave();
        errno = EINVAL;
        return -1;
    }

    if (ts_is_zero(&vt->vt_deadline)) {
        memset(curr_value, 0, sizeof(*curr_value));
    } else {
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        if (ts_le(&vt->vt_deadline, &now)) {
            curr_value->it_value.tv_sec = 0;
            curr_value->it_value.tv_nsec = 0;
        } else {
            curr_value->it_value.tv_sec = vt->vt_deadline.tv_sec - now.tv_sec;
            curr_value->it_value.tv_nsec = vt->vt_deadline.tv_nsec - now.tv_nsec;
            if (curr_value->it_value.tv_nsec < 0) {
                curr_value->it_value.tv_sec--;
                curr_value->it_value.tv_nsec += 1000000000L;
            }
        }
        curr_value->it_interval = vt->vt_interval.it_interval;
    }

    task_unlock(current);
    pthread_mutex_unlock(&vtimers_lock);
    tvm_sig_leave();

    return 0;
}

FUNCTION_DEFINE1(int, timer_delete, timer_t, tmr) {

    tvm_sig_enter();
    pthread_mutex_lock(&vtimers_lock);
    task_lock(current);

    int slot = (int)(intptr_t)tmr;
    if (slot < 0 || slot >= MAX_TIMERS_PER_TASK || current->tsk_timers[slot] < 0) {
        task_unlock(current);
        pthread_mutex_unlock(&vtimers_lock);
        tvm_sig_leave();
        errno = EINVAL;
        return -1;
    }

    int global_idx = current->tsk_timers[slot];
    current->tsk_timers[slot] = -1;
    vtimer_destroy_locked(global_idx);

    task_unlock(current);
    pthread_mutex_unlock(&vtimers_lock);

    tvm_sig_leave();

    return 0;
}

FUNCTION_DEFINE1(int, timer_getoverrun, timer_t, tmr) {
    (void)tmr;
    return 0; // we don't track overruns
}

static void itimerval_to_itimerspec(const struct itimerval *iv, struct itimerspec *its)
{
    its->it_value.tv_sec = iv->it_value.tv_sec;
    its->it_value.tv_nsec = iv->it_value.tv_usec * 1000L;
    its->it_interval.tv_sec = iv->it_interval.tv_sec;
    its->it_interval.tv_nsec = iv->it_interval.tv_usec * 1000L;
}

static void itimerspec_to_itimerval(const struct itimerspec *its, struct itimerval *iv)
{
    iv->it_value.tv_sec = its->it_value.tv_sec;
    iv->it_value.tv_usec = its->it_value.tv_nsec / 1000;
    iv->it_interval.tv_sec = its->it_interval.tv_sec;
    iv->it_interval.tv_usec = its->it_interval.tv_nsec / 1000;
}

FUNCTION_DEFINE3(int, setitimer, ITIMER_WHICH_T, which, const struct itimerval *, new_value,
                 struct itimerval *, old_value) {
    if (which != ITIMER_REAL) {
        errno = EINVAL;
        return -1;
    }

    // lazily create a POSIX timer for ITIMER_REAL
    if (current->tsk_itimer_real < 0) {
        if (new_value->it_value.tv_sec == 0 && new_value->it_value.tv_usec == 0) {
            if (old_value)
                memset(old_value, 0, sizeof(*old_value));
            return 0;
        }
        timer_t tid;
        struct sigevent sev = { .sigev_notify = SIGEV_SIGNAL, .sigev_signo = SIGALRM };
        if (timer_create(CLOCK_MONOTONIC, &sev, &tid) != 0)
            return -1;
        current->tsk_itimer_real = (int)(intptr_t)tid;
    }

    struct itimerspec new_its, old_its;
    itimerval_to_itimerspec(new_value, &new_its);

    timer_t tid = (timer_t)(intptr_t)current->tsk_itimer_real;
    if (timer_settime(tid, 0, &new_its, old_value ? &old_its : NULL) != 0)
        return -1;

    if (old_value)
        itimerspec_to_itimerval(&old_its, old_value);

    return 0;
}

FUNCTION_DEFINE2(int, getitimer, ITIMER_WHICH_T, which, struct itimerval *, curr_value) {
    if (which != ITIMER_REAL) {
        errno = EINVAL;
        return -1;
    }

    if (current->tsk_itimer_real < 0) {
        memset(curr_value, 0, sizeof(*curr_value));
        return 0;
    }

    struct itimerspec its;
    timer_t tid = (timer_t)(intptr_t)current->tsk_itimer_real;
    if (timer_gettime(tid, &its) != 0)
        return -1;

    itimerspec_to_itimerval(&its, curr_value);
    return 0;
}

#if defined(__linux__)

/////////////
// MARK: Linux-specific hooks
/////////////

FUNCTION_DEFINE3(int, mknod, const char *, pathname, mode_t, mode, dev_t, dev) {
    char pathname_abs[PATH_MAX];
    if (!normalize_path(NULL, pathname, pathname_abs))
        return -1;
    pathname = pathname_abs;

    return CALL_FUNC(mknod, pathname, mode & ~current->tsk_umask, dev);
}

FUNCTION_DEFINE4(int, mknodat, int, dirfd, const char *, pathname, mode_t, mode, dev_t, dev) {
    char pathname_abs[PATH_MAX];
    if (dirfd == AT_FDCWD) {
        if (!normalize_path(NULL, pathname, pathname_abs))
            return -1;
        pathname = pathname_abs;
    }

    return CURRENT_RFD(dirfd, CALL_FUNC(mknodat, __rfd, pathname, mode & ~current->tsk_umask, dev));
}

FUNCTION_DEFINE3(int, execvpe, const char *, file, char * const *, argv, char * const *, envp) {
    INFO("execvpe: %s", file);
    return tvm_execvpe(file, argv, envp);
}

FUNCTION_DEFINE0(int, clearenv) {
    *current->tsk_relocs.__environ = NULL;
    return 0;
}

FUNCTION_DEFINE1(int, epoll_create, int, size) {
    int r, f;
    
    tvm_sig_enter();
    if (-1 == (r = CALL_FUNC(epoll_create, size))) {
        tvm_sig_leave();
        return -1;
    }

    if (-1 == (f = task_new_fd(current, r))) {
        CALL_FUNC(close, r);
        errno = EMFILE;
    }

    tvm_sig_leave();

    return f;
}

FUNCTION_DEFINE1(int, epoll_create1, int, flags) {
    int r, f;
    
    tvm_sig_enter();
    if (-1 == (r = CALL_FUNC(epoll_create1, flags))) {
        tvm_sig_leave();
        return -1;
    }

    if (-1 == (f = task_new_fd(current, r))) {
        CALL_FUNC(close, r);
        errno = EMFILE;
    }

    tvm_sig_leave();

    return f;
}

FUNCTION_DEFINE2(int, timerfd_create, int, clockid, int, flags) {
    int r, f;
    
    tvm_sig_enter();

    if (-1 == (r = CALL_FUNC(timerfd_create, clockid, flags))) {
        tvm_sig_leave();
        return -1;
    }

    if (-1 == (f = task_new_fd(current, r))) {
        CALL_FUNC(close, r);
        errno = EMFILE;
    }

    tvm_sig_leave();

    return f;
}

/*
 * eventfd(): like timerfd_create, a host descriptor entered into the task's
 * table. Not interposed before, so the guest got the raw host number, which
 * TVM's poll()/read()/write() then looked up in the task table: libcurl
 * (USE_EVENTFD on Linux) puts its wakeup eventfd in every multi poll, the
 * poll failed (CURLM_UNRECOVERABLE_POLL) and every transfer, even file://,
 * ended in "(43) A libcurl function was given a bad argument".
 */
FUNCTION_DEFINE2(int, eventfd, unsigned int, initval, int, flags) {
    int r, f;

    tvm_sig_enter();

    if (-1 == (r = CALL_FUNC(eventfd, initval, flags))) {
        tvm_sig_leave();
        return -1;
    }

    if (-1 == (f = task_new_fd(current, r))) {
        CALL_FUNC(close, r);
        errno = EMFILE;
    }

    tvm_sig_leave();

    return f;
}

// glibc's helpers read/write the descriptor internally, past the
// interposed read()/write(): route them through those.
FUNCTION_DEFINE2(int, eventfd_read, int, fd, eventfd_t *, value) {
    return read(fd, value, sizeof(*value)) == sizeof(*value) ? 0 : -1;
}

FUNCTION_DEFINE2(int, eventfd_write, int, fd, eventfd_t, value) {
    return write(fd, &value, sizeof(value)) == sizeof(value) ? 0 : -1;
}

/*
 * More of glibc's large-file names (see pread64): the same functions on
 * x86-64, interposed under their 64 names so a _FILE_OFFSET_BITS=64 guest
 * does not reach the host libc with a task descriptor (mmap64 of a file,
 * mkstemp64 returning a host descriptor the task table never saw) or a TVM
 * stream in place of a glibc FILE (fseeko64/ftello64: curl).
 */
FUNCTION_DEFINE6(void *, mmap64, void *, addr, size_t, length, int, prot, int, flags, int, fd, off64_t, offset) {
    return __tvm_mmap(addr, length, prot, flags, fd, offset);
}

FUNCTION_DEFINE2(int, statfs64, const char *, pathname, struct statfs64 *, buf) {
    return __tvm_statfs(pathname, (struct statfs *)buf);
}

FUNCTION_DEFINE1(int, mkstemp64, char *, template) {
    return __tvm_mkstemp(template);
}

FUNCTION_DEFINE3(int, fseeko64, FILE *, stream, off64_t, offset, int, whence) {
    return __tvm_fseeko(stream, offset, whence);
}

FUNCTION_DEFINE1(off64_t, ftello64, FILE *, stream) {
    return __tvm_ftello(stream);
}

FUNCTION_DEFINE3(int, posix_fallocate, int, fd, off_t, offset, off_t, len) {
    // returns the error number; an unknown descriptor reaches the host as
    // -1 (WITH_RFD), which answers EBADF
    return CURRENT_RFD(fd, CALL_FUNC(posix_fallocate, __rfd, offset, len));
}

FUNCTION_DEFINE3(int, posix_fallocate64, int, fd, off64_t, offset, off64_t, len) {
    return __tvm_posix_fallocate(fd, offset, len);
}

/*
 * The rest of what the Linux guest programs import that takes a FILE * or a
 * descriptor (from the undefined symbols of git, bash, toybox, curl, nano).
 * Anything not interposed runs in the host libc, where a guest stdout is
 * the TVM process's own stdout and a guest descriptor is just a number:
 *
 * - Fortified stdio (_FORTIFY_SOURCE, which Ubuntu's gcc enables at -O1 and
 *   up): git's printf/fprintf are __printf_chk/__fprintf_chk. `git
 *   --version` printed into the TVM process's stdout (the backend log)
 *   instead of the session, and `git init` aborted in glibc with "invalid
 *   stdio handle" on a TVM stream. The checks themselves only guard the
 *   caller's buffer sizes; they are kept for fread/fgets.
 * - The _unlocked stdio functions bash's include/unlocked-io.h maps
 *   fflush()/fgets()/fputc()/fputs()/fwrite() onto on glibc, and __fpurge.
 *   (The getc()/putc()/feof()/... family is mapped back to the locking
 *   functions at compile time by third_party/include-shim/
 *   tvm_stdio_unlocked.h, on every platform.)
 * - C99/C23 scanf: glibc's headers redirect fscanf to __isoc99_fscanf, or
 *   __isoc23_fscanf from 2.38 (git, toybox). This file's own
 *   fscanf/scanf/vfscanf are emitted under one of those names too, so a
 *   guest import only found them by symbol through dlsym, which works where
 *   TVM is the executable; register both spellings by name.
 * - Descriptors created outside the task table (accept4, memfd_create,
 *   inotify_init) and calls on task descriptors (xattrs, futimens, ...).
 */
static void tvm_chk_fail(void)
{
    // what glibc's __chk_fail does: a buffer overflow was caught
    static const char msg[] = "*** buffer overflow detected ***: terminated\n";
    write(STDERR_FILENO, msg, sizeof(msg) - 1);
    abort();
}

FUNCTION_DEFINE_VA2(int, __printf_chk, int, flag, const char *, format) {
    return vfprintf(stdout, format, __ap);
}

FUNCTION_DEFINE_VA3(int, __fprintf_chk, FILE *, stream, int, flag, const char *, format) {
    return vfprintf(stream, format, __ap);
}

FUNCTION_DEFINE3(int, __vprintf_chk, int, flag, const char *, format, va_list, ap) {
    return vfprintf(stdout, format, ap);
}

FUNCTION_DEFINE4(int, __vfprintf_chk, FILE *, stream, int, flag, const char *, format, va_list, ap) {
    return vfprintf(stream, format, ap);
}

FUNCTION_DEFINE_VA3(int, __dprintf_chk, int, fd, int, flag, const char *, format) {
    return vdprintf(fd, format, __ap);
}

FUNCTION_DEFINE4(int, __vdprintf_chk, int, fd, int, flag, const char *, format, va_list, ap) {
    return vdprintf(fd, format, ap);
}

FUNCTION_DEFINE5(size_t, __fread_chk, void *, ptr, size_t, ptrlen, size_t, size, size_t, n, FILE *, stream) {
    if (size && n > ptrlen / size)
        tvm_chk_fail();
    return fread(ptr, size, n, stream);
}

FUNCTION_DEFINE5(size_t, __fread_unlocked_chk, void *, ptr, size_t, ptrlen, size_t, size, size_t, n, FILE *, stream) {
    if (size && n > ptrlen / size)
        tvm_chk_fail();
    return fread(ptr, size, n, stream);
}

FUNCTION_DEFINE4(char *, __fgets_chk, char *, s, size_t, slen, int, n, FILE *, stream) {
    if (n > 0 && (size_t)n > slen)
        tvm_chk_fail();
    return fgets(s, n, stream);
}

FUNCTION_DEFINE4(char *, __fgets_unlocked_chk, char *, s, size_t, slen, int, n, FILE *, stream) {
    if (n > 0 && (size_t)n > slen)
        tvm_chk_fail();
    return fgets(s, n, stream);
}

FUNCTION_DEFINE4(size_t, fread_unlocked, void *, ptr, size_t, size, size_t, n, FILE *, stream) {
    return fread(ptr, size, n, stream);
}

FUNCTION_DEFINE4(size_t, fwrite_unlocked, const void *, ptr, size_t, size, size_t, n, FILE *, stream) {
    return fwrite(ptr, size, n, stream);
}

FUNCTION_DEFINE3(char *, fgets_unlocked, char *, s, int, n, FILE *, stream) {
    return fgets(s, n, stream);
}

FUNCTION_DEFINE2(int, fputs_unlocked, const char *, str, FILE *, stream) {
    return fputs(str, stream);
}

FUNCTION_DEFINE2(int, fputc_unlocked, int, c, FILE *, stream) {
    return fputc(c, stream);
}

FUNCTION_DEFINE1(int, fflush_unlocked, FILE *, stream) {
    return fflush(stream);
}

FUNCTION_DEFINE_VOID1(__fpurge, FILE *, stream) {
    tvm_sig_enter();
    struct file *file = file_for_task_fp(current, stream);
    if (!file) {
        tvm_sig_leave();
        return;
    }
    file_unlock(file, 0);
    CALL_FUNC(__fpurge, real_fp(stream));
    file_lock(file, 0);
    file_unlock(file, 1);
    tvm_sig_leave();
}

FUNCTION_METADATA(__isoc99_fscanf, &fscanf);
FUNCTION_METADATA(__isoc99_scanf, &scanf);
FUNCTION_METADATA(__isoc99_vfscanf, &vfscanf);
FUNCTION_METADATA(__isoc23_fscanf, &fscanf);
FUNCTION_METADATA(__isoc23_scanf, &scanf);
FUNCTION_METADATA(__isoc23_vfscanf, &vfscanf);

// _FORTIFY_SOURCE's open(): __open_2 checks that O_CREAT/O_TMPFILE came
// with a mode, i.e. went to the variadic open instead.
FUNCTION_DEFINE2(int, __open_2, const char *, pathname, int, flags) {
    return open(pathname, flags);
}

FUNCTION_DEFINE2(int, __open64_2, const char *, pathname, int, flags) {
    return open(pathname, flags);
}

FUNCTION_DEFINE4(int, accept4, int, sockfd, struct sockaddr *, addr, socklen_t *, addrlen, int, flags) {
    tvm_sig_enter();
    int f = tvm_task_adopt_fd(CURRENT_RFD(sockfd, CALL_FUNC(accept4, __rfd, addr, addrlen, flags)));
    tvm_sig_leave();
    return f;
}

FUNCTION_DEFINE2(int, memfd_create, const char *, name, unsigned int, flags) {
    tvm_sig_enter();
    int f = tvm_task_adopt_fd(CALL_FUNC(memfd_create, name, flags));
    tvm_sig_leave();
    return f;
}

FUNCTION_DEFINE0(int, inotify_init) {
    tvm_sig_enter();
    int f = tvm_task_adopt_fd(CALL_FUNC(inotify_init));
    tvm_sig_leave();
    return f;
}

FUNCTION_DEFINE1(int, inotify_init1, int, flags) {
    tvm_sig_enter();
    int f = tvm_task_adopt_fd(CALL_FUNC(inotify_init1, flags));
    tvm_sig_leave();
    return f;
}

FUNCTION_DEFINE3(int, inotify_add_watch, int, fd, const char *, pathname, uint32_t, mask) {
    char pathname_abs[PATH_MAX];
    if (!normalize_path(NULL, pathname, pathname_abs))
        return -1;
    return CURRENT_RFD(fd, CALL_FUNC(inotify_add_watch, __rfd, pathname_abs, mask));
}

FUNCTION_DEFINE2(int, inotify_rm_watch, int, fd, int, wd) {
    return CURRENT_RFD(fd, CALL_FUNC(inotify_rm_watch, __rfd, wd));
}

FUNCTION_DEFINE5(int, recvmmsg, int, fd, struct mmsghdr *, msgvec, unsigned int, vlen, int, flags, struct timespec *, timeout) {
    return CURRENT_RFD(fd, CALL_FUNC(recvmmsg, __rfd, msgvec, vlen, flags, timeout));
}

FUNCTION_DEFINE4(int, sendmmsg, int, fd, struct mmsghdr *, msgvec, unsigned int, vlen, int, flags) {
    return CURRENT_RFD(fd, CALL_FUNC(sendmmsg, __rfd, msgvec, vlen, flags));
}

FUNCTION_DEFINE4(int, sync_file_range, int, fd, off64_t, offset, off64_t, nbytes, unsigned int, flags) {
    return CURRENT_RFD(fd, CALL_FUNC(sync_file_range, __rfd, offset, nbytes, flags));
}

FUNCTION_DEFINE2(long, fpathconf, int, fd, int, name) {
    return CURRENT_RFD(fd, CALL_FUNC(fpathconf, __rfd, name));
}

FUNCTION_DEFINE2(int, futimens, int, fd, const struct timespec *, times) {
    return CURRENT_RFD(fd, CALL_FUNC(futimens, __rfd, times));
}

FUNCTION_DEFINE4(int, utimensat, int, dirfd, const char *, pathname, const struct timespec *, times, int, flags) {
    if (!pathname)  // Linux: the descriptor itself, as futimens
        return CURRENT_RFD(dirfd, CALL_FUNC(utimensat, __rfd, NULL, times, flags));
    char pathname_abs[PATH_MAX];
    if (!dirfd_normalize_path(dirfd, pathname, pathname_abs))
        return -1;
    return CALL_FUNC(utimensat, AT_FDCWD, pathname_abs, times, flags);
}

FUNCTION_DEFINE3(int, futimesat, int, dirfd, const char *, pathname, const struct timeval *, times) {
    if (!pathname)
        return CURRENT_RFD(dirfd, CALL_FUNC(futimesat, __rfd, NULL, times));
    char pathname_abs[PATH_MAX];
    if (!dirfd_normalize_path(dirfd, pathname, pathname_abs))
        return -1;
    return CALL_FUNC(futimesat, AT_FDCWD, pathname_abs, times);
}

FUNCTION_DEFINE4(ssize_t, fgetxattr, int, fd, const char *, name, void *, value, size_t, size) {
    return CURRENT_RFD(fd, CALL_FUNC(fgetxattr, __rfd, name, value, size));
}

FUNCTION_DEFINE5(int, fsetxattr, int, fd, const char *, name, const void *, value, size_t, size, int, flags) {
    return CURRENT_RFD(fd, CALL_FUNC(fsetxattr, __rfd, name, value, size, flags));
}

FUNCTION_DEFINE3(ssize_t, flistxattr, int, fd, char *, list, size_t, size) {
    return CURRENT_RFD(fd, CALL_FUNC(flistxattr, __rfd, list, size));
}

FUNCTION_DEFINE2(int, fremovexattr, int, fd, const char *, name) {
    return CURRENT_RFD(fd, CALL_FUNC(fremovexattr, __rfd, name));
}

FUNCTION_DEFINE4(ssize_t, getxattr, const char *, pathname, const char *, name, void *, value, size_t, size) {
    char pathname_abs[PATH_MAX];
    if (!normalize_path(NULL, pathname, pathname_abs))
        return -1;
    return CALL_FUNC(getxattr, pathname_abs, name, value, size);
}

FUNCTION_DEFINE4(ssize_t, lgetxattr, const char *, pathname, const char *, name, void *, value, size_t, size) {
    char pathname_abs[PATH_MAX];
    if (!normalize_path(NULL, pathname, pathname_abs))
        return -1;
    return CALL_FUNC(lgetxattr, pathname_abs, name, value, size);
}

// As statfs: the host's answer for the resolved path.
FUNCTION_DEFINE2(int, statvfs, const char *, pathname, struct statvfs *, buf) {
    char pathname_abs[PATH_MAX];
    if (!normalize_path(NULL, pathname, pathname_abs))
        return -1;
    return CALL_FUNC(statvfs, pathname_abs, buf);
}

FUNCTION_DEFINE2(int, statvfs64, const char *, pathname, struct statvfs64 *, buf) {
    char pathname_abs[PATH_MAX];
    if (!normalize_path(NULL, pathname, pathname_abs))
        return -1;
    return CALL_FUNC(statvfs64, pathname_abs, buf);
}

FUNCTION_DEFINE2(int, fstatvfs, int, fd, struct statvfs *, buf) {
    return CURRENT_RFD(fd, CALL_FUNC(fstatvfs, __rfd, buf));
}

FUNCTION_DEFINE2(int, fstatvfs64, int, fd, struct statvfs64 *, buf) {
    return CURRENT_RFD(fd, CALL_FUNC(fstatvfs64, __rfd, buf));
}

FUNCTION_DEFINE2(int, pipe2, int *, pipefd, int, flags) {
    int r[2], f[2];

    tvm_sig_enter();

    if (0 != CALL_FUNC(pipe2, r, flags)) {
        tvm_sig_leave();
        return -1;
    }
    
    if (-1 == (f[0] = task_new_fd(current, r[0]))) {
        VERIFY(0 == CALL_FUNC(close, r[0]));
        VERIFY(0 == CALL_FUNC(close, r[1]));
        tvm_sig_leave();
        return -1;
    }

    if (-1 == (f[1] = task_new_fd(current, r[1]))) {
        VERIFY(0 == close(f[0]));
        VERIFY(0 == CALL_FUNC(close, r[1]));
        tvm_sig_leave();
        return -1;
    }
    
    tvm_sig_leave();

    memcpy(pipefd, f, sizeof(f));
    return 0;
}

FUNCTION_DEFINE2(int, fstatfs64, int, fd, struct statfs64 *, buf) {
    return CURRENT_RFD(fd, CALL_FUNC(fstatfs64, __rfd, buf));
}

FUNCTION_DEFINE_VA2(int, open64, const char *, pathname, int, flags) {
    mode_t mode = va_arg(__ap, int);
    if (!can_enter_tvm(NULL))
        return CALL_FUNC(open64, pathname, flags, mode);

    return open(pathname, flags, mode);
}

FUNCTION_DEFINE_VA3(int, openat64, int, dirfd, const char *, pathname, int, flags) {
    mode_t mode = va_arg(__ap, int);
    if (!can_enter_tvm(NULL))
        return CALL_FUNC(openat64, dirfd, pathname, flags, mode);

    return openat(dirfd, pathname, flags | O_LARGEFILE, mode);
}

FUNCTION_DEFINE4(int, fstatat64, int, dirfd, const char *, pathname, struct stat64 *, statbuf, int, flags) {
    char pathname_abs[PATH_MAX];
    if(!dirfd_normalize_path(dirfd, pathname, pathname_abs))
        return -1;
    pathname = pathname_abs;

    tvm_sig_enter();
    int ret = file_stat(pathname, (struct stat *)statbuf, flags);
    tvm_sig_leave();
    return ret;
}

FUNCTION_DEFINE3(off_t, lseek64, int, fd, off64_t, offset, int, whence) {
    return CURRENT_RFD(fd, CALL_FUNC(lseek64, __rfd, offset, whence));
}

FUNCTION_DEFINE2(int, ftruncate64, int, fd, off64_t, length) {
    return CURRENT_RFD(fd, CALL_FUNC(ftruncate64, __rfd, length));
}

FUNCTION_DEFINE1(struct dirent64 *, readdir64, DIR *, dir) {
    return (struct dirent64 *)readdir(dir);
}

FUNCTION_DEFINE2(FILE *, fopen64, const char *, pathname, const char *, mode) {
    return fopen(pathname, mode);
}

FUNCTION_DEFINE3(FILE *, freopen64, const char *, pathname, const char *, mode, FILE *, stream) {
    return freopen(pathname, mode, stream);
}

FUNCTION_DEFINE2(int, stat64, const char *, pathname, struct stat64 *, statbuf) {
    return stat(pathname, (struct stat *)statbuf);
}

FUNCTION_DEFINE2(int, lstat64, const char *, pathname, struct stat64 *, statbuf) {
    return lstat(pathname, (struct stat *)statbuf);
}

FUNCTION_DEFINE2(int, fstat64, int, fd, struct stat64 *, statbuf) {
    return fstat(fd, (struct stat *)statbuf);
}

FUNCTION_DEFINE0(char *, get_current_dir_name) {
    char *pwd = getenv("PWD");
    if (pwd)
        return strdup(pwd);
    return getcwd(NULL, 0);
}

#elif defined(__APPLE__)

/////////////
// MARK: Apple-specific hooks
/////////////

static void CFDictionaryApplyFunction_emulator(const void *key, const void *value, void *context)
{
    if (0 != emulator_call(NULL, (uint64_t)CFDictionaryApplyFunction_function, (uint64_t)key, (uint64_t)value, (uint64_t)context, 0, 0, 0))
        err(1, "emulator_call(CFDictionaryApplyFunction_function)");
}

FUNCTION_DEFINE_VOID3(CFDictionaryApplyFunction, CFDictionaryRef, theDict, CFDictionaryApplierFunction CF_NOESCAPE, applier, void *, context) {
    CFDictionaryApplyFunction_function = applier;
    CALL_FUNC(CFDictionaryApplyFunction, theDict, CFDictionaryApplyFunction_emulator, context);
    CFDictionaryApplyFunction_function = NULL;
}

FUNCTION_DEFINE2(int, fileport_makeport, int, fd, fileport_t *, port) {
    return CURRENT_RFD(fd, CALL_FUNC(fileport_makeport, __rfd, port));
}

FUNCTION_DEFINE1(int, fileport_makefd, fileport_t, port) {
    tvm_sig_enter();
    int rfd = CALL_FUNC(fileport_makefd, port);
    if (rfd < 0) {
        tvm_sig_leave();
        return -1;
    }
    int vfd = task_new_fd(current, rfd);
    if (vfd < 0) {
        VERIFY(0 == CALL_FUNC(close, rfd));
        errno = EMFILE;
    }
    tvm_sig_leave();
    return vfd;
}

#endif
