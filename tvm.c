#define _GNU_SOURCE
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
#include <errno.h>
#include <stdio.h>
#include <pthread.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
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
#include <signal.h>
#include <dirent.h>

#if defined(__APPLE__)
#include <util.h>
#include <mach-o/getsect.h>
#elif defined(__linux__)
#include <pty.h>
#include <sys/vfs.h>
#include <sys/epoll.h>
#include <sys/timerfd.h>
#include <asm/prctl.h>
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

// LOCK ORDERING

// tty
// task

/////////////
// Macros
/////////////

/**
 * "the things you need the most are the things you never have" - T.S.
 */

#define panic(Msg, ...) do { char __MSG[0x1000] = {0}; snprintf(__MSG, sizeof(__MSG), Msg, ##__VA_ARGS__); __panic(__MSG, __FILE__, __LINE__); } while (0)
static void __panic(const char *m, const char *f, int l);


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

static __thread char msgbuf[0x1000];

static void format_time(char *buf, size_t buflen)
{
    struct timeval tv;
    struct tm tm;

    gettimeofday(&tv, NULL);
    localtime_r(&tv.tv_sec, &tm);

    int ms = tv.tv_usec / 1000;

    snprintf(buf, buflen,
        "%04d-%02d-%02d %02d:%02d:%02d.%03d",
        tm.tm_year + 1900,
        tm.tm_mon + 1,
        tm.tm_mday,
        tm.tm_hour,
        tm.tm_min,
        tm.tm_sec,
        ms
    );
}

static int g_outfd = STDOUT_FILENO;

#define INFO(Msg, ...) do { \
    char __ts[32]; \
    format_time(__ts, sizeof(__ts)); \
    int __num = snprintf(msgbuf, sizeof(msgbuf), "%s Kernel(%d): %s: " Msg "\n", __ts, __gettid(), __FUNCTION__, ##__VA_ARGS__); \
    CALL_FUNC(write, g_outfd, msgbuf, __num); \
} while (0)

#define __ALIGN_MASK(x,mask)    (((x)+(mask))&~(mask))

/////////////
// List
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
// Functions
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
#define DECL_FUNC(Name) \
    static typeof(Name) *LOCAL_SYM(Name) = NULL; \
    static struct original_func_t _of_ ## Name = { .ptrdest = (void *)&LOCAL_SYM(Name), .name = #Name }; \
    __attribute__((constructor)) \
    static void _ofc_ ## Name () { list_add_tail( & (_of_ ## Name).list, &original_functions); }
#define CALL_FUNC(Name, ...) (LOCAL_SYM(Name) (__VA_ARGS__))

struct original_func_t {
    struct list_head list;
    void **ptrdest;
    const char *name;
};

static LIST_HEAD(original_functions);

DECL_FUNC(malloc);
DECL_FUNC(calloc);
DECL_FUNC(realloc);
DECL_FUNC(free);

DECL_FUNC(fopen);
DECL_FUNC(freopen);
DECL_FUNC(fdopen);
DECL_FUNC(opendir)
DECL_FUNC(fdopendir);
DECL_FUNC(fwrite);
DECL_FUNC(fread);
DECL_FUNC(fclose);
DECL_FUNC(fseek);
DECL_FUNC(ftell);
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
DECL_FUNC(lseek);
DECL_FUNC(fsync);
DECL_FUNC(dup);
DECL_FUNC(dup2);
DECL_FUNC(pipe);
DECL_FUNC(select);
DECL_FUNC(poll);
DECL_FUNC(fcntl);
DECL_FUNC(ftruncate);
DECL_FUNC(stat);
DECL_FUNC(lstat);
DECL_FUNC(fstat);
DECL_FUNC(fstatat);
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

#if defined(__linux__)
DECL_FUNC(mknod);
DECL_FUNC(mknodat);
DECL_FUNC(clearenv);
DECL_FUNC(execvpe);
DECL_FUNC(epoll_create);
DECL_FUNC(epoll_create1);
DECL_FUNC(timerfd_create);
DECL_FUNC(pipe2);
DECL_FUNC(fstatfs);
DECL_FUNC(fstatfs64);
DECL_FUNC(open64);
DECL_FUNC(openat64);
DECL_FUNC(fstatat64);
DECL_FUNC(lseek64);
DECL_FUNC(ftruncate64);
DECL_FUNC(get_current_dir_name);
#endif

__attribute__((constructor))
static void init_funcs()
{
    struct original_func_t *curr_func;
    list_for_each_entry(curr_func, &original_functions, list) {
        *curr_func->ptrdest = dlsym(RTLD_NEXT, curr_func->name);
        if (NULL == *curr_func->ptrdest)
            panic("function '%s' not found", curr_func->name);
    }
};

/////////////
// Declarations
/////////////

/**
 * Just needed this for code in `Task` section.
 */

struct fops;
struct task;

struct fops *fops_for_fd_locked(struct task *t, int fd);
int fops_fdflag(struct fops *fops);
int fops_dup(struct fops *fops, int oldfd, int newfd);

void *kalloc(size_t size);
void kfree(void *p);
void *krealloc(void *ptr, size_t size);

int can_enter_tvm(void *lr);

struct emulator_context;
struct emulator_context *emulator_context_create(struct ulinker *ulinker);
void                     emulator_context_destroy(struct emulator_context *ec);

void emulator_call_sighandler(uint64_t handler_pc, uint64_t arg0, uint64_t arg1, uint64_t arg2);

/////////////
// Helpers
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

/////////////
// Jmpbuf manipulation
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
// Environment
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
// Task
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

#define TS_ZOMBIE  0x1
#define TS_CTTY    0x2

#define TFD_MASK 0x0F000000
#define TFD_TTY  0x01000000

typedef void (*exit_t)(void);

struct task {
    struct list_head tsk_list;
    struct task *tsk_parent;
    unsigned tsk_refcount;
    pid_t tsk_pid;
    int tsk_result;  // wstatus as returned from `waitpid()`
    pthread_mutex_t tsk_lock;
    int tsk_fd[MAX_FILES];
    FILE *tsk_f[3];
    int tsk_state;
    pthread_cond_t tsk_wait_cond;
    struct sigaction tsk_sighandlers[MAX_SIGNALS];
    unsigned tsk_threads_cnt;
    struct list_head tsk_threads;  // thread
    pid_t tsk_sid;
    pid_t tsk_pgid;
    char **tsk_environ;
    char tsk_comm[16];
    struct rmem_arena *tsk_ar;
    struct ulinker *tsk_linker;
    char tsk_cwd[PATH_MAX];
    exit_t tsk_exits[32];
};

struct thread {
    struct list_head thr_list;
    struct task *thr_task;  // back pointer to owning task
    pthread_t thr_pthread;
    pthread_attr_t thr_pthread_attr;
    uint64_t thr_refcount;
    struct emulator_context *thr_emulator;
    void *thr_fork_fp;  // copy frames from sp to this on fork()
};

static Dl_info main_dlinfo = {0};
static struct rmem_arena *global_arena = NULL;
static pthread_t main_pthread;
static struct task *main_task = NULL;
static pthread_mutex_t tasks_lock = PTHREAD_MUTEX_INITIALIZER;
static __thread struct task *current;
static __thread struct thread *current_thread;
static __thread int thread_tvm_disallow = 0;
#define TVM_ON  do { thread_tvm_disallow = 0; } while (0)
#define TVM_OFF ({ int __val = thread_tvm_disallow; thread_tvm_disallow = 1; __val; })
#define TVM_RESTORE(_val) do { thread_tvm_disallow = (_val); } while (0)

static void task_lock(struct task *t)
{
    pthread_mutex_lock(&t->tsk_lock);
}

static void task_unlock(struct task *t)
{
    pthread_mutex_unlock(&t->tsk_lock);
}

static int t_fd(int fd)
{
    if (!main_task)
        return fd;

    if (fd < 0)
        return fd;

    if (!current)
        panic("t_fd");
    
    if (fd >= MAX_FILES)
        return -1;

    if (current->tsk_fd[fd] >= 0)
        return current->tsk_fd[fd] & (~TFD_MASK);
    return current->tsk_fd[fd];
}

static int t_fdr(int fd, int lock)
{
    if (!main_task)
        return fd;
    
    if (lock)
        task_lock(current);
    
    if (fd >= 0) {
        for (int i = 0; i < MAX_FILES; i++) {
            if (fd == (current->tsk_fd[i] & ~TFD_MASK)) {
                if (lock)
                    task_unlock(current);
                return i;
            }
        }
    }

    if (lock)
        task_unlock(current);
    return -1;
}

static struct task *taskalloc_common()
{
    struct task *ret = NULL;
    struct task *t = kalloc(sizeof(struct task));
    if (!t)
        goto out;
    
    memset(t, 0, sizeof(*t));

    INIT_LIST_HEAD(&t->tsk_list);
    INIT_LIST_HEAD(&t->tsk_threads);

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

    for (int i = 0; i < MAX_FILES; i++)
        t->tsk_fd[i] = -1;

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
    t->tsk_environ = NULL; // allocated in the task arena

    pthread_cond_destroy(&t->tsk_wait_cond);
    pthread_mutex_unlock(&t->tsk_lock);
    pthread_mutex_destroy(&t->tsk_lock);

    kfree(t);
}

/**
 * tasks_lock must either be locked or dolocktasks=1
 * locks children
 */
static void taskfreelastref(struct task *t, int dolocktasks, int dealloc)
{
    if (dolocktasks)
        pthread_mutex_lock(&tasks_lock);

    for (int i = 0; i < 3; i++) {
        if (t->tsk_f[i] != NULL)
            CALL_FUNC(fclose, t->tsk_f[i]);
        else if (t->tsk_fd[i] != -1)
            CALL_FUNC(close, t->tsk_fd[i] & ~TFD_MASK);
        t->tsk_fd[i] = -1;
        t->tsk_f[i] = NULL;
    }

    for (int i = 3; i < MAX_FILES; i++) {
        if (t->tsk_fd[i] > -1)
            CALL_FUNC(close, t->tsk_fd[i] & ~TFD_MASK);
        t->tsk_fd[i] = -1;
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
    t->tsk_parent = current;
    t->tsk_pid = -1;
    t->tsk_sid = current->tsk_sid;
    t->tsk_pgid = current->tsk_pgid;
    t->tsk_state = current->tsk_state & TS_CTTY;
    memcpy(t->tsk_comm, current->tsk_comm, sizeof(t->tsk_comm));
    memcpy(&t->tsk_exits, &current->tsk_exits, sizeof(t->tsk_exits));

    for (int i = 0; i < MAX_FILES; i++) {
        if (t_fd(i) == -1)
            continue;
        
        int newfd = CALL_FUNC(dup, t_fd(i));
        if (newfd == -1)
            goto out;
        t->tsk_fd[i] = newfd;
        
        struct fops *fops = fops_for_fd_locked(current, i);
        t->tsk_fd[i] |= fops_fdflag(fops);
        if (0 != fops_dup(fops, t_fd(i), newfd)) {
            CALL_FUNC(close, newfd);
            t->tsk_fd[i] &= ~fops_fdflag(fops);
            goto out;
        }
    }

    for (int i = 0; i < 3; i++) {
        t->tsk_f[i] = CALL_FUNC(fdopen, t->tsk_fd[i] & ~TFD_MASK, (i?"w":"r"));
        if (!t->tsk_f[i])
            goto out;
    }

    memcpy(t->tsk_sighandlers, t->tsk_parent->tsk_sighandlers, sizeof(t->tsk_sighandlers));

    t->tsk_cwd[0] = 0;
    strncpy(t->tsk_cwd, current->tsk_cwd, sizeof(t->tsk_cwd));

    task_unlock(current);

    ret = t;

out:

    if (!ret) {
        if (t) {
            taskfreelastref(t, 1, 1);
            t = NULL;
        }
    }

    return ret;
}

static int task_get_fd_locked(struct task *t, int min_fd)
{
    if (min_fd < 0)
        return -1;
    
    for (int i = min_fd; i < MAX_FILES; i++) {
        if (t->tsk_fd[i] == -1)
            return i;
    }

    return -1;
}

static int task_reserve_fd(struct task *t, int min_fd)
{
    task_lock(t);
    int f = task_get_fd_locked(t, min_fd);
    if (f != -1)
        current->tsk_fd[f] = -2;
    task_unlock(t);
    return f;
}

static int task_set_fd(struct task *t, int f, int r)
{
    if (f < 0 || f >= MAX_FILES)
        panic("f %d", f);
    
    task_lock(t);
    t->tsk_fd[f] = r;
    task_unlock(t);
    
    return r;
}

static int task_new_fd(struct task *t, int min_fd, int new_fd) {
    
    task_lock(current);
    int fd = task_get_fd_locked(current, min_fd);
    if (fd != -1) {
        current->tsk_fd[fd] = new_fd;
    }
    task_unlock(current);

    return fd;

}

/**
 * tasks_lock must be locked (or dolocktasks=1)
 * returns locked task
 */
static struct task *task_for_pid(pid_t pid, int dolocktasks) {
    if (main_task->tsk_pid == pid)
        return main_task;

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
 * tasks_lock must be locked (or dolocktasks=1)
 * returns locked task
 */
static struct task *task_for_rfd(int rfd, int dolocktasks, int *out_nfd) {
    if (rfd < 0)
        return NULL;
    
    if (dolocktasks)
        pthread_mutex_lock(&tasks_lock);
    
    struct task *t = main_task;
    do {
        task_lock(t);
        for (int nfd = 0; nfd < MAX_FILES; nfd++) {
            if ((t->tsk_fd[nfd] >= 0) && (t->tsk_fd[nfd] & ~TFD_MASK) == rfd) {
                if (dolocktasks)
                    pthread_mutex_unlock(&tasks_lock);
                *out_nfd = nfd;
                return t;
            }
        }
        task_unlock(t);
    }
    while ( ({ t = list_next_entry(t, tsk_list); !list_entry_is_head(t, &main_task->tsk_list, tsk_list); }) );

    if (dolocktasks)
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

    struct task *ct;
    list_for_each_entry(ct, &main_task->tsk_list, tsk_list)
    {
        // TODO lock ct?
        if (pgid == ct->tsk_pgid)
        {
            if (t->tsk_pid == ct->tsk_pid || t->tsk_sid != ct->tsk_sid) {
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
 * needs tasks_lock and t locked
 */
static int task_kill_locked(struct task *t, int signum) {
    if (list_empty(&t->tsk_threads))
        return ESRCH;
    
    struct thread *thr = list_entry(t->tsk_threads.next, struct thread, thr_list);
    return pthread_kill(thr->thr_pthread, signum);
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
__attribute__((noreturn))
static void terminate_current_locked(int result)
{
    // TODO kill all threads, make sure tsk_threads_cnt==1
    if (current->tsk_threads_cnt != 1)
        panic("_exit tsk_threads_cnt");
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
    }

    if (auto_reap) {
        taskfreelastref(current, 0, 1);
        pthread_detach(pthread_self());
    }
    else {
        if (!parent)
            panic("auto_reap=0 but parent is NULL");
        
        
        taskfreelastref(current, 0, 0);

        current->tsk_state |= TS_ZOMBIE;
        current->tsk_result = result;

        task_unlock(current);
    }
    current = NULL;

    if (notify_parent && 0 != task_kill_locked(parent, SIGCHLD))
        panic("terminate_current_locked notify parent failed (%d)", errno);

    // parent may be blocked on waitpid but no more pids, wake him up!
    if (parent) {
        pthread_cond_signal(&parent->tsk_wait_cond);
        task_unlock(parent);
    }

    // if this is before the pthread_cond_signal, a not-that-rare deadlock occurs in stress tests on unlocked mutex of parent.
    // not sure why, nor gonna figure this out now
    pthread_mutex_unlock(&tasks_lock);

    CALL_FUNC(pthread_exit, 0);
    __builtin_unreachable();
}

static FILE *t_f(FILE *f)
{
    if (!main_task)
        return f;
    
    if (!current)
        panic("t_f");

    if (f == stdin)
        return current->tsk_f[0];
    if (f == stdout)
        return current->tsk_f[1];
    if (f == stderr)
        return current->tsk_f[2];
    return f;
}

char *current_normalize_path(const char *file_name, char *out_path)
{
    char tmp[PATH_MAX];
    int c = 0;

    task_lock(current);
    if (file_name[0] == '/')
        c = snprintf(tmp, PATH_MAX, "%s", file_name);
    else if (file_name[0] == 0)
        c = snprintf(tmp, PATH_MAX, "%s", current->tsk_cwd);
    else
        c = snprintf(tmp, PATH_MAX, "%s/%s", current->tsk_cwd, file_name);
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
// Allocator
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
// Signals
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

static __thread int sigshutup = 0;

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
    Dl_info dlinfo;
    size_t nstack = get_stacktrace(stack, sizeof(stack)/sizeof(*stack));

    for (int i = 0; i < nstack; i++) {
        memset(&dlinfo, 0, sizeof(dlinfo));
        if (0 == dladdr(stack[i], &dlinfo) || (!dlinfo.dli_fname))
            INFO("  stacktrace[% 2d] = %08lx", i, (long)stack[i]);
        else if (!dlinfo.dli_sname)
            INFO("  stacktrace[% 2d] = %s + %lx", i, dlinfo.dli_fname, (long)stack[i] - (long)dlinfo.dli_fbase);
        else
            INFO("  stacktrace[% 2d] = %s : %s + %lx", i, dlinfo.dli_fname, dlinfo.dli_sname, (long)stack[i] - (long)dlinfo.dli_saddr);
    }
}

void signal_handler(int signum, siginfo_t *siginfo, void *ucontext)
{
    if (!sigshutup) {
        if (signum == SIGSEGV || signum == SIGBUS || signum == SIGILL)
            INFO("signum=%d at %p", signum, siginfo->si_addr);
        else if (signum != SIGCHLD) {
            INFO("signum=%d", signum);
            if (signum == SIGABRT)
                dump_stacktrace();
        }
    }

    if (!current)
        panic("signal %d on pthread_exit", signum);
    
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

    if (whatdo == SAD_TERM || whatdo == SAD_CORE)
        terminate_current_locked(mkwstatus(0, signum));
    pthread_mutex_unlock(&tasks_lock);

    if (whatdo == SAD_IGN) {
        task_unlock(current);
        return;
    }
    if (whatdo == SAD_STOP) {
        task_unlock(current);
        panic("signal_handler STOP"); // TODO what do we do
    }
    if (whatdo == SAD_CONT) {
        task_unlock(current);
        // TODO anything else we need to do?
        return;
    }
    
    if ((act->sa_flags & SA_SIGINFO) == 0) {
        task_unlock(current);
        emulator_call_sighandler((uint64_t)act->sa_handler, signum, 0, 0);
        return;
    }
    
    task_unlock(current);
    emulator_call_sighandler((uint64_t)act->sa_sigaction, signum, (uint64_t)siginfo, (uint64_t)ucontext);
    return;
}

/////////////
// Devices
/////////////

/**
 * Yeah when I said in `Task` section we create a little kernel,
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

struct fops {
    struct list_head list;

    int f_fdflag; // TFD_*

    int (*f_openchk)(const char *pathname);
    int (*f_open)(const char *pathname, int flags, mode_t mode);
    int (*f_dup)(int oldfd, int newfd);
    int (*f_stat)(int fd, struct stat *st);
    int (*f_chown)(int fd, uid_t owner, gid_t group);
    int (*f_chmod)(int fd, mode_t mode);
    int (*f_ioctl)(int fd, unsigned long request, void *arg);
    ssize_t (*f_read)(int fd, void *buf, size_t count, int offset);
    ssize_t (*f_write)(int fd, const void *buf, size_t count, int offset);
    int (*f_close)(int fd);
};

static LIST_HEAD(devices);
#define REGISTER_DEV(Fops) __attribute__((constructor)) \
    static void _tvm_dreg_ ## Fops () { list_add_tail(&((Fops).list), &devices); }

int fops_open(const char *pathname, int flags, mode_t mode)
{
    struct fops *f;
    
    list_for_each_entry(f, &devices, list) {
        if (f->f_openchk && f->f_open && f->f_openchk(pathname))
            return f->f_open(pathname, flags, mode);
    }

    return -1;
}

int fops_fdflag(struct fops *fops)
{
    if (!fops)
        return 0;
    
    return fops->f_fdflag;
}

int fops_dup(struct fops *fops, int oldfd, int newfd)
{
    if (!fops || !fops->f_dup)
        return 0;
    
    return fops->f_dup(oldfd, newfd);
}

struct fops *fops_for_fd_locked(struct task *t, int fd)
{
    if (fd < 0 || fd >= MAX_FILES || t->tsk_fd[fd] < 0)
        return NULL;
    
    struct fops *f;
    list_for_each_entry(f, &devices, list) {
        if (f->f_fdflag && (t->tsk_fd[fd] & f->f_fdflag))
            return f;
    }

    return NULL;
}

struct fops *fops_for_fd(struct task *t, int fd) {
    task_lock(current);
    struct fops *f = fops_for_fd_locked(t, fd);
    task_unlock(current);
    return f;
}

struct fops *fops_for_stream(struct task *t, FILE *stream) {
    task_lock(current);
    struct fops *f = fops_for_fd_locked(t, t_fdr(fileno(stream), 0));
    task_unlock(current);
    return f;
}

struct fops *fops_for_pathname(const char *pathname)
{
    struct fops *f;
    
    list_for_each_entry(f, &devices, list) {
        if (f->f_openchk && f->f_open && f->f_openchk(pathname))
            return f;
    }

    return NULL;
}

struct fops *fops_for_open(const char *pathname)
{
    struct fops *f;
    list_for_each_entry(f, &devices, list) {
        if (f->f_openchk && f->f_openchk(pathname))
            return f;
    }

    return NULL;
}

#define FOPS_PATH(Fops, Func, Pathname, ...) do { \
        if (!(Fops -> Func)) { \
            errno = ENOTSUP; \
            return -1; \
        } \
        int __FD = open(pathname, O_RDONLY | O_NOCTTY); \
        if (__FD == -1) \
            return -1; \
        int __R = (Fops -> Func)(t_fd(__FD), ##__VA_ARGS__); \
        if (0 == __R) \
            __R = close(__FD); \
        else \
            close(__R); \
        return __R; \
    } while (0)

ssize_t fops_read(struct fops *fops, int fd, void *buf, size_t count)
{
    if (!fops->f_read) {
        errno = ENOTSUP;
        return -1;
    }

    return fops->f_read(fd, buf, count, -1);
}

ssize_t fops_write(struct fops *fops, int fd, const void *buf, size_t count)
{
    if (!fops->f_write) {
        errno = ENOTSUP;
        return -1;
    }

    return fops->f_write(fd, buf, count, -1);
}

/////////////
// PTY/TTY
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
 * 
 * TODO:
 * - Support enough features so atleast `vi` works.
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

static COW_IMPL(char[PATH_MAX], tty_name);
static COW_IMPL(char[PATH_MAX], pts_name);

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
 * Locks
 */
static struct tty *tty_for_fd(int fd, int mode)
{
    struct stat st;
    if (-1 == CALL_FUNC(fstat, fd, &st))
        return NULL;

    pthread_mutex_lock(&ttys_lock);
    struct tty *tt;
    list_for_each_entry(tt, &ttys, t_list) {
        if (((mode & TTM_MASTER) && tt->t_mfd_ino == st.st_ino) || 
            ((mode & TTM_SLAVE)  && tt->t_sfd_ino == st.st_ino)) {
            tty_lock(tt, 0);
            
            // tty is being destroyed, leave it be
            if (!tt->t_refcnt) {
                tty_unlock(tt, 0);
                continue;
            }

            tt->t_refcnt++;
            pthread_mutex_unlock(&ttys_lock);
            return tt;
        }
    }
    pthread_mutex_unlock(&ttys_lock);

    return NULL;
}

/**
 * Returns 1 if local (tvm) tty, 0 if not.
 */
static int islocaltty(int fd)
{
    task_lock(current);

    // make its in `tsk_fd` first
    if (t_fd(fd) == -1)
        return 0;

    if (0 == (current->tsk_fd[fd] & TFD_TTY)) {
        task_unlock(current);
        return 0;
    }

    task_unlock(current);
    struct tty *tt = tty_for_fd(t_fd(fd), TTM_ALL);
    if (!tt) {
        return 0;
    }

    tty_unlock(tt, 1);
    return 1;
}

static int ttyop_openchk(const char *pathname) {
    return (0 == strncmp(pathname, TTY_DIR "/", sizeof(TTY_DIR "/") - 1) || 0 == strcmp(pathname, TTY_FILE));
}

/**
 * Returns locked and ups ref
 */
static struct tty *tty_for_path(const char *pathname) {
    if (!(ttyop_openchk(pathname)))
        return NULL;
    
    // Open the controlling TTY
    if (0 == strcmp(pathname, TTY_FILE)) {
        task_lock(current);
        if (0 == (current->tsk_state & TS_CTTY)) {
            task_unlock(current);
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

            if (tt->t_sid == current->tsk_sid) {
                tt->t_refcnt++;
                pthread_mutex_unlock(&ttys_lock);
                task_unlock(current);
                return tt;
            }
            tty_unlock(tt, 0);
        }
        pthread_mutex_unlock(&ttys_lock);

        panic("TS_CTTY but no TTY found");
    }

    // Open some other TTY

    long long n;
    if (0 != intparse(pathname + sizeof(TTY_DIR "/") - 1, &n, 10))
        return NULL;
    
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

    return NULL;
}

static int ttyop_open(const char *pathname, int flags, mode_t mode)
{
    struct tty *tt = tty_for_path(pathname);
    if (!tt) {
        errno = ENOTTY;
        return -1;
    }
    
    // tt is locked and ref'd

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
                errno = EIO;
                return -1;
            }

            // tt is still locked in this case!
        }
    }

    if (tt->t_sfd == -1)
        panic("%s t_sfd invalid?", pathname);

    int r_sfd = CALL_FUNC(dup, tt->t_sfd);

    if (0 == (flags & O_NOCTTY)) {
        if (-1 != r_sfd) {
            current->tsk_state |= TS_CTTY;
            tt->t_sid = current->tsk_sid;
            tt->t_pgid = current->tsk_pgid;
        }

        task_unlock(current);
    }

    tty_unlock(tt, 1);
    return r_sfd;
}

static int ttyop_dup(int oldfd, int newfd) {
    struct tty *tt = tty_for_fd(oldfd, TTM_MASTER);
    if (!tt)
        return 0;
    
    tt->t_refcnt++;
    tty_unlock(tt, 1);
    return 0;
}

static int ttyop_stat(int fd, struct stat *st)
{
    struct tty *tt = tty_for_fd(fd, TTM_ALL);
    if (!tt) {
        errno = ENOTTY;
        return -1;
    }

    if (!st) {
        tty_unlock(tt, 1);
        return 0;
    }

    memset(st, 0, sizeof(*st));

    st->st_ino = tt->t_mfd_ino;
    st->st_uid = tt->t_uid;
    st->st_gid = tt->t_gid;
    st->st_mode = tt->t_mode;

    tty_unlock(tt, 1);
    return 0;
}

static int ttyop_chmod(int fd, mode_t mode)
{
    struct tty *tt = tty_for_fd(fd, TTM_ALL);
    if (!tt) {
        errno = ENOTTY;
        return -1;
    }

    tt->t_mode = mode;

    tty_unlock(tt, 1);
    return 0;
}

static int ttyop_chown(int fd, uid_t owner, gid_t group)
{
    struct tty *tt = tty_for_fd(fd, TTM_ALL);
    if (!tt) {
        errno = ENOTTY;
        return -1;
    }

    tt->t_uid = owner;
    tt->t_gid = group;

    tty_unlock(tt, 1);
    return 0;
}

static int ttyop_ioctl(int fd, unsigned long request, void *arg)
{
    int is_master = 1;
    struct tty *tt = tty_for_fd(fd, TTM_MASTER);
    if (!tt) {
        tt = tty_for_fd(fd, TTM_SLAVE);
        if (!tt) {
            errno = ENOTTY;
            return -1;
        }
        is_master = 0;
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
            tty_unlock(tt, 0);
            kill(-pgid, SIGWINCH);
            tty_lock(tt, 0);
            ret = 0;
            break;
        
        case TIOCSCTTY:
            task_lock(current);
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
            task_lock(current);
            if (current->tsk_sid != current->tsk_pid) {
                current->tsk_state &= ~TS_CTTY;
                task_unlock(current);
                ret = 0;
                break;
            }
            task_unlock(current);

            tty_unlock(tt, 0);
            pthread_mutex_lock(&tasks_lock);
            list_for_each_entry(t, &main_task->tsk_list, tsk_list) {
                task_lock(t);
                if (t->tsk_sid == tt->t_sid)
                    t->tsk_state &= ~TS_CTTY;
                task_unlock(t);
            }
            pthread_mutex_unlock(&tasks_lock);
            tty_lock(tt, 0);
            
            pgid = tt->t_pgid;
            tty_unlock(tt, 0);
            kill(-pgid, SIGHUP);
            kill(-pgid, SIGCONT);
            tty_lock(tt, 0);
            ret = 0;
            break;

        case TIOCGPGRP:
            task_lock(current);
            if (!is_master &&
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
            task_lock(current);
            if (!is_master &&
                (0 == (current->tsk_state & TS_CTTY) || tt->t_sid != current->tsk_sid)) {
                task_unlock(current);
                errno = ENOTTY;
                break;
            }
            task_unlock(current);

            tty_unlock(tt, 0);
            pthread_mutex_lock(&tasks_lock);
            list_for_each_entry(t, &main_task->tsk_list, tsk_list) {
                if (t->tsk_pgid == *((pid_t *)arg)) {
                    break;
                }
            }
            pthread_mutex_unlock(&tasks_lock);
            tty_lock(tt, 0);

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
            task_lock(current);
            if (!is_master &&
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
    return ret;
}

static ssize_t ttyop_read(int fd, void *buf, size_t count, int offset)
{
    if (offset != -1) {
        errno = EINVAL;
        return -1;
    }

    int is_master = 1;
    struct tty *tt = tty_for_fd(fd, TTM_MASTER);
    if (!tt) {
        tt = tty_for_fd(fd, TTM_SLAVE);
        if (!tt) {
            errno = ENOTTY;
            return -1;
        }
        is_master = 0;
    }

    struct termios termios;
    memcpy(&termios, &tt->t_termios, sizeof(termios));

    tty_unlock(tt, 0);

    size_t total_read = 0;
    uint8_t *out = buf;
    struct pollfd pfd = { .fd = fd, .events = POLLIN };

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
            if (errno == EINTR)
                continue;
            tty_lock(tt, 0);
            tty_unlock(tt, 1);
            return -1;
        }

        // Data is available: read one byte at a time
        unsigned char c;
        ssize_t n = CALL_FUNC(read, fd, &c, 1);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            tty_lock(tt, 0);
            tty_unlock(tt, 1);
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

static int tty_putc(struct tty *tt, int fd, int c)
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

static int tty_output(struct tty *tt, int fd, int c)
{
    int col;

    if (!ISSET(tt->t_oflag, OPOST)) {
        if (ISSET(tt->t_lflag, FLUSHO))
            return 0;
        return tty_putc(tt, fd, c);
    }

    // expand tabs or smth

    // if ONLCR is set, translate newline into "\r\n"
    if (c == '\n' && ISSET(tt->t_oflag, ONLCR)) {
        if (tty_putc(tt, fd, '\r'))
            return -1;
    }
    // if OCRNL is set, translate "\r" into "\n"
    else if (c == '\r' && ISSET(tt->t_oflag, OCRNL))
        c = '\n';
    // if ONOCR is set, don't transmit CRs when on column 0
    else if (c == '\r' && ISSET(tt->t_oflag, ONOCR) && tt->t_col == 0)
        return 0;

    if (!ISSET(tt->t_lflag, FLUSHO) && tty_putc(tt, fd, c))
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

static ssize_t tty_output_buffer(struct tty *tt, int fd, const uint8_t *buf, size_t count)
{
    int fflags;
    ssize_t ret = -1;

    if (-1 == (fflags = CALL_FUNC(fcntl, fd, F_GETFL)))
        return -1;

    if (-1 == CALL_FUNC(fcntl, fd, F_SETFL, O_NONBLOCK))
        return -1;

    size_t i;
    for (i = 0; i < count; i++)
        if (0 != tty_output(tt, fd, (int)buf[i]))
            break;
    
    ret = i;

    CALL_FUNC(fcntl, fd, F_SETFL, fflags);

    return ret;
}

static void tty_echo(struct tty *tt, int c)
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
		(void)tty_output(tt, slave, '^');
		CLR(c, ~TTY_CHARMASK);
		if (c == 0177) {
			c = '?';
		} else {
			c += 'A' - 1;
		}
	}

	tty_output(tt, slave, c);
    CALL_FUNC(close, slave);
}

static int tty_input(struct tty *tt, int fd, int c)
{
    if (ISSET(tt->t_iflag, ISTRIP))
		CLR(c, 0x80);
    
    if (!ISSET(tt->t_lflag, EXTPROC)) {
        // Signals
        if (ISSET(tt->t_lflag, ISIG)) {
            if (CCEQ(tt->t_cc[VINTR], c) || CCEQ(tt->t_cc[VQUIT], c) || CCEQ(tt->t_cc[VSUSP], c)) {
                tty_echo(tt, c);
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

    return tty_putc(tt, fd, c);
}

static ssize_t tty_input_buffer(struct tty *tt, int fd, const uint8_t *buf, size_t count)
{
    int fflags;
    ssize_t ret = -1;

    if (-1 == (fflags = CALL_FUNC(fcntl, fd, F_GETFL)))
        return -1;

    if (-1 == CALL_FUNC(fcntl, fd, F_SETFL, O_NONBLOCK))
        return -1;

    size_t i;
    for (i = 0; i < count; i++)
        if (0 != tty_input(tt, fd, (int)buf[i]))
            break;
    
    ret = i;

    CALL_FUNC(fcntl, fd, F_SETFL, fflags);

    return ret;
}

static ssize_t ttyop_write(int fd, const void *buf, size_t count, int offset)
{
    if (offset != -1) {
        errno = EINVAL;
        return -1;
    }

    int is_master = 1;
    struct tty *tt = tty_for_fd(fd, TTM_MASTER);
    if (!tt) {
        tt = tty_for_fd(fd, TTM_SLAVE);
        if (!tt) {
            errno = ENOTTY;
            return -1;
        }
        is_master = 0;
    }

    ssize_t ret;
    if (is_master)
        ret = tty_input_buffer(tt, fd, buf, count);
    else
        ret = tty_output_buffer(tt, fd, buf, count);
    tty_unlock(tt, 1);
    return ret;
}


static int ttyop_close(int fd) {
    struct tty *tt = tty_for_fd(fd, TTM_MASTER);
    if (!tt)
        return 0;

    tt->t_refcnt--;
    if (tt->t_refcnt > 1) {
        tty_unlock(tt, 1);
        return 0;
    }

    // Disassociate the entire session
    struct task *signaltask = NULL;
    if (tt->t_sid != -1) {
        pthread_mutex_lock(&tasks_lock);
        struct task *t;
        list_for_each_entry(t, &main_task->tsk_list, tsk_list) {
            task_lock(t);
            if (t->tsk_sid == tt->t_sid)
                t->tsk_state &= ~TS_CTTY;
            if (t->tsk_pid == tt->t_sid)
                signaltask = t;
            else
                task_unlock(t);
        }
        pthread_mutex_unlock(&tasks_lock);
    }

    // leave refcnt at 0, no one will take ref in global list after unlocking
    tt->t_refcnt--;
    tty_unlock(tt, 0);

    pthread_mutex_lock(&ttys_lock);
    list_del(&tt->t_list);
    pthread_mutex_unlock(&ttys_lock);

    ttydealloc(tt);

    if (signaltask) {
        pid_t p = signaltask->tsk_pid;
        task_unlock(signaltask);
        kill(p, SIGHUP);
        kill(p, SIGCONT);
    }

    return 0;
}

static struct fops ttyops = {
    .f_fdflag = TFD_TTY,

    .f_openchk = ttyop_openchk,
    .f_open = ttyop_open,
    .f_dup = ttyop_dup,
    .f_stat = ttyop_stat,
    .f_chmod = ttyop_chmod,
    .f_chown = ttyop_chown,
    .f_ioctl = ttyop_ioctl,
    .f_read = ttyop_read,
    .f_write = ttyop_write,
    .f_close = ttyop_close,
};
REGISTER_DEV(ttyops);

static int ptm_open()
{
    struct tty *tt = ttyalloc();
    if (!tt) {
        errno = ENOMEM;
        return -1;
    }

    int mfd = CALL_FUNC(dup, tt->t_mfd);
    if (-1 == mfd) {
        ttydealloc(tt);
        return -1;
    }

    tt->t_refcnt++;

    pthread_mutex_lock(&ttys_lock);
    list_add_tail(&tt->t_list, &ttys);
    pthread_mutex_unlock(&ttys_lock);
    return mfd;
}

static int tty_slavename(int fd, char *buf, size_t buflen)
{
    struct tty *tt = tty_for_fd(fd, TTM_ALL);
    if (!tt)
        return ENOTTY;

    if (tt->t_mfd == -1) {
        tty_unlock(tt, 1);
        return EINVAL;
    }

    snprintf(buf, buflen, TTY_DIR "/%d", tt->t_mfd);
    tty_unlock(tt, 1);
    return 0;
}

/////////////
// Copy-On-Write (COW)
/////////////

/**
 * Idea here is to let each task feel like it has COW memory with its parent,
 * but in reality we deep-copy every relevant buffer.
 * 
 * The major downside here is that EVERY global must be declared/defined with COW_DECL()/COW_IMPL() macros.
 * 
 * How it works:
 * 1. We register all global variables to a global list (with sizes)
 * 2. We mark every `malloc()` buffer so we could identify it
 * 3. On fork(), get all global variable pointers from parent
 * 4. On fork(), memcpy() all parent global variables to the child
 * 5. On fork(), iterate every global variable and recursively deep-copy malloc'd buffer pointers
 * 6. On fork(), iterate copied stack frame (see `Forkless API` section) and deep-copy malloc'd pointers as-well
 * 
 * We also use a "deep-copy" cache (struct cow_deepcopy_ctx) so we properly handled pointers that appear twice while deep-copying.
 * 
 * TODO:
 * - Find a way to NOT mark every global variable with COW_IMPL().
 * - Support multi-threading, we can force dynamic tlsmodel and override _get_tls_data() resolve function.
 */

#define COW_FIXUP 0x1

struct cow_data_variable_t {
    struct list_head list;
    void *(*getptr_fn)();
    void (*init_fn)();
    char *name; // for debugging purposes
    size_t size;
    int flags;
};
static LIST_HEAD(cow_data_variables);

void _tvm_register_cow(void *(*getptr_fn)(), unsigned size, void (*init_fn)(), int deepcopy, char *name)
{
    struct cow_data_variable_t *cow_data_var;
    if (NULL == (cow_data_var = CALL_FUNC(malloc, sizeof(*cow_data_var))))
        panic("cow_data_var");
    
    list_add_tail(&cow_data_var->list, &cow_data_variables);
    cow_data_var->getptr_fn = getptr_fn;
    cow_data_var->init_fn = init_fn;
    cow_data_var->name = name;
    cow_data_var->size = size;
    if (deepcopy) 
        cow_data_var->flags |= COW_FIXUP;
}

static void cow_run_init_funcs()
{
    struct cow_data_variable_t *cow;
    list_for_each_entry(cow, &cow_data_variables, list) {
        if (cow->init_fn)
            cow->init_fn();
        else
            memset(cow->getptr_fn(), 0, cow->size);
    }
}

struct cow_fixup_context {
    struct {
        void *new;
        void *old;
        size_t size;
        int fixup;
    } *ptrs;
    size_t count;
};

static int cow_fixup_load_data_variables(struct cow_fixup_context *ctx, int new)
{
    size_t data_variable_count = 0;
    struct cow_data_variable_t *cow;

    list_for_each_entry(cow, &cow_data_variables, list)
        data_variable_count++;
    
    if (ctx->count == 0) {
        ctx->ptrs = kalloc(data_variable_count * sizeof(*ctx->ptrs));
        if (!ctx->ptrs) {
            errno = ENOMEM;
            return -1;
        }
        ctx->count = data_variable_count;
    }
    else if (ctx->count != data_variable_count)
        panic("data variable count mismatch (%zu != %zu)", data_variable_count, ctx->count);
    
    int i = 0;
    list_for_each_entry(cow, &cow_data_variables, list) {
        if (i >= (data_variable_count))
            panic("data variable count mismatch in iteration (%d !+ %zu)", (i+1), data_variable_count);

        ctx->ptrs[i].size = cow->size;
        void *p = cow->getptr_fn();
        if (new)
            ctx->ptrs[i].new = cow->getptr_fn();
        else
            ctx->ptrs[i].old = cow->getptr_fn();
        ctx->ptrs[i].fixup = 1;

        i++;
    }

    if (i != (data_variable_count))
        panic("data variable coumt mismatch after iteration (%d != %zu)", i, data_variable_count);
    
    return 0;
}

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

static void *cow_fixup_translate(struct cow_fixup_context *dctx, void *p)
{
    for (int i = 0; i < dctx->count; i++)
    {
        if ((uintptr_t)p < (uintptr_t)dctx->ptrs[i].old || (uintptr_t)p >= (uintptr_t)dctx->ptrs[i].old + dctx->ptrs[i].size)
            continue;

        return (void *)((uintptr_t)p - (uintptr_t)dctx->ptrs[i].old + (uintptr_t)dctx->ptrs[i].new);
    }

    return p;
}

static void cow_fixup_process(struct cow_fixup_context *dctx)
{
    for (int i = 0; i < dctx->count; i++) {
        // only used for other fixups
        if (!dctx->ptrs[i].fixup)
            continue;
        
        for (
            void **p = (void **)dctx->ptrs[i].new;
            (uintptr_t)(p + 1) <= (uintptr_t)dctx->ptrs[i].new + dctx->ptrs[i].size;
            p++
        ) {
            *p = cow_fixup_translate(dctx, *p);
        }
    }
}

/////////////
// Getopt
/////////////

/**
 * Atleast I tried to do something with this shitty API.
 */

__thread char *tvm_optarg;
__thread int   tvm_optind = 1;
__thread int   tvm_opterr;
__thread int   tvm_optopt;
static pthread_mutex_t getopt_lock = PTHREAD_MUTEX_INITIALIZER;

/////////////
// Exec
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
// Emulator
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
};

void emulator_engine_destroy(struct emulator_engine *ee) {
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

    if (arch == ULARCH_ARM64)
        err = uc_open(UC_ARCH_ARM64, UC_MODE_ARM, &engine);
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

    uint64_t last_entry_end = EMULATOR_ADDRESS_MIN; // incase there are no entries, map everything

    for (struct emulator_map_entry *eme = ee->ee_eme; eme; eme = eme->eme_next) {
        if (eme->eme_start > last_entry_end) {
            err = uc_mem_map_ptr(engine, last_entry_end, eme->eme_start - last_entry_end, UC_PROT_READ | UC_PROT_WRITE, (void *)last_entry_end);
            if (err) {
                INFO("uc_mem_map_ptr(addr=%p, size=%p, perms=UC_PROT_READ|UC_PROT_WRITE) failed: %s", (void *)last_entry_end, (void *)(eme->eme_start - last_entry_end), uc_strerror(err));
                goto out;
            }
        }

        err = uc_mem_map_ptr(engine, eme->eme_start, eme->eme_size, UC_PROT_READ | UC_PROT_WRITE | UC_PROT_EXEC, (void *)eme->eme_start);
        if (err) {
            INFO("uc_mem_map_ptr(addr=%p, size=%p, perms=UC_PROT_READ|UC_PROT_WRITE|UC_PROT_EXEC) failed: %s", (void *)eme->eme_start, (void *)eme->eme_size, uc_strerror(err));
            goto out;
        }

        last_entry_end = eme->eme_start + eme->eme_size;
    }

    if (last_entry_end < EMULATOR_ADDRESS_MAX) {
        err = uc_mem_map_ptr(engine, last_entry_end, EMULATOR_ADDRESS_MAX - last_entry_end, UC_PROT_READ | UC_PROT_WRITE, (void *)last_entry_end);
        if (err) {
            INFO("uc_mem_map_ptr(addr=%p, size=%p, perms=UC_PROT_READ|UC_PROT_WRITE) failed: %s", (void *)last_entry_end, (void *)(EMULATOR_ADDRESS_MAX - last_entry_end), uc_strerror(err));
            goto out;
        }
    }

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
    struct emulator_map_entry *eme = emulator_map_entry_create(start, size);
    if (!eme)
        return 1;
    eme->eme_next = ee->ee_eme;
    ee->ee_eme = eme;
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
        if (ee->ee_arch == arch)
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

    if (ulinker_dladdr(ec->ec_ulinker, (void *)pc, out_info)) {
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
        if (0 != emulator_engine_reset(ec->ec_curr_engine))
            goto out;
        ec->ec_curr_engine->ee_refresh = 0;
    }

    if (0 != emulator_saved_state_to_uc(ec->ec_ss, ec->ec_curr_engine->ee_uc))
        goto out;

    ec->ec_ss->ess_intr = ESS_INTR_UNSPEC;
    err = uc_emu_start(ec->ec_curr_engine->ee_uc, pc, 0, 0, 0);

    if (0 != emulator_saved_state_from_uc(ec->ec_ss, ec->ec_curr_engine->ee_uc))
        goto out;

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

        if (*handler_func && *handler_func != SIG_DFL && *handler_func != SIG_IGN)
            *handler_func = EMUFUNC_MARK(*handler_func);
    }

    int ret = sigaction(signum, act, oldact);

    // unmark old handler
    if (oldact) {
        void **handler_func = (void **)&act->sa_handler;
        if (act->sa_flags & SA_SIGINFO)
            handler_func = (void **)&act->sa_sigaction;

        if (*handler_func && *handler_func != SIG_DFL && *handler_func != SIG_IGN)
            *handler_func = EMUFUNC_UNMARK(*handler_func);
    }

    return ret;
}

#if defined(__linux__)
sighandler_t emulator_signal(int signum, sighandler_t handler) {
#elif defined(__APPLE__)
sig_t emulator_signal(int signum, sig_t handler) {
#endif
    // mark new handler
    if (handler && handler != SIG_DFL && handler != SIG_IGN)
        handler = EMUFUNC_MARK(handler);
    
    handler = signal(signum, handler);

    // unmark old handler
    if (handler && handler != SIG_DFL && handler != SIG_IGN && handler != SIG_ERR)
        handler = EMUFUNC_UNMARK(handler);
    
    return handler;
}

int emulator_setjmp(jmp_buf env) {
    uc_err err;
    struct emulator_saved_state *ess = current_thread->thr_emulator->ec_ss;

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
    struct emulator_saved_state *ess = NULL;
    struct emulator_context *ec = current_thread->thr_emulator;
    uc_err err;

    struct ulinker_dladdr_info local_pcinfo;
    if (!pcinfo) {
        if (0 != emulator_find_dl(ec, params->pc, &local_pcinfo)) {
            INFO("emulator_find_dl(pc=%p) failed", (void *)params->pc);
            return 1;
        }
        pcinfo = &local_pcinfo;
    }

    prevtvm = TVM_OFF;

    // fast path: upcall to host
#ifdef ENABLE_RWX
    if (1) {
#else
    if (pcinfo->udli_farch == ULARCH_NONE) {
#endif
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

    struct emulator_engine *ee = emulator_context_get_or_create_engine(ec, pcinfo->udli_farch);
    if (!ee)
        goto out;
    ec->ec_curr_engine = ee;

    if (!(ess = emulator_saved_state_create(pcinfo->udli_farch, pcinfo->udli_fabi)))
        goto out;

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
            if (ess->ess_intr_memtype != UC_MEM_FETCH_PROT) {
                INFO("ess->ess_intr_memtype %d != UC_MEM_FETCH_PROT at %p", ess->ess_intr_memtype, (void *)ess->ess_intr_addr);
                raise(SIGSEGV);
            }

            if (ess->ess_intr_addr == EMULATOR_CALL_END_LR) {
                //INFO("emulator_call_ex ret: %p", (void *)emulator_saved_state_read_reg(ess, ESS_REG_RET));
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
                if (fault_info.udli_farch != ess->ess_arch) {

                    // read lr before call so stack is adjusted for emulator_call_params_from_saved_state()
                    if (0 != emulator_saved_state_exit_call(ess, &ess->ess_fault_lr))
                        goto out;

                    struct emulator_call_params fault_params;
                    emulator_call_params_from_saved_state(&fault_params, ess);

                    // INFO("emulator_call_ex subcall: %p ( %p, %p )", (void *)fault_params.pc, (void *)fault_params.arg[0], (void *)fault_params.arg[1]);

                    // call with parameters and save ret
                    uint64_t fault_ret;
                    if (0 != emulator_call_ex(&fault_info, &fault_params, &fault_ret))
                        goto out;

                    // INFO("emulator_call_ex subcall ret: %p", (void *)fault_ret);

                    // NOTE! It is possible `fork()` was called!
                    // Update local variables from thread state
                    ec = current_thread->thr_emulator;
                    ee = emulator_context_get_or_create_engine(ec, pcinfo->udli_farch);
                    ess = ec->ec_ss;

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
    }

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

void emulator_call_sighandler(uint64_t handler_pc, uint64_t arg0, uint64_t arg1, uint64_t arg2) {

    // handler is not in emulator
#ifdef ENABLE_RWX
    if (1) {
#else
    if (!EMUFUNC_MARKED(handler_pc)) {
#endif
        ((void (*)(uint64_t, uint64_t, uint64_t))handler_pc)(arg0, arg1, arg2);
        return;
    }

    // handler is in emulator, call emulator from signal handler so help me god
    handler_pc = EMUFUNC_UNMARK(handler_pc);
    emulator_call(NULL, handler_pc, arg0, arg1, arg2, 0, 0, 0);
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

int emulator_pthread_create(pthread_t *thread, const pthread_attr_t *attr, void *(*start_routine)(void *), void *_arg)
{
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
// Hooks API
/////////////

struct tvm_call {
    struct tvm_call *prev;
    void *lr;
};
static __thread struct tvm_call *call = NULL;

#if defined(__linux__)
extern char etext;
extern char __executable_start;

void get_text(uintptr_t *out_start, uintptr_t *out_end)
{
    *out_start = (uintptr_t)&__executable_start;
    *out_end = (uintptr_t)&etext;
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
        if (0 == memcpy(&ulr->ulr_event, ule, sizeof(*ule))) {
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

    pthread_mutex_lock(&all_ulinker_loaded_ranges_lock);
    int r = (NULL != find_containing_ulinker_loaded_range_locked(lr));
    pthread_mutex_unlock(&all_ulinker_loaded_ranges_lock);

    return r;
}

int can_enter_tvm(void *lr) {
    // not initialized
    if (!main_task)
        return 0;

    if (thread_tvm_disallow)
        return 0;

    if (!lr) {
        if (!call)
            return 0;
        lr = call->lr;
    }


    if (!(in_tvm_range(lr)))
        return 0;

    // fork() does not use call (for now)
    if (!call)
        return 1;

    // if this isn't an inner call, just go
    if (!call->prev)
        return 1;

    // if this is an inner call, check first call
    struct tvm_call *call_itr = call;
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

#define __FUNCTION_DEFINEx(_x, _rettype, _name, ...) \
_rettype __tvm_##_name(__MAP(_x, __FN_DECL, __VA_ARGS__)); \
_rettype _name(__MAP(_x, __FN_DECL, __VA_ARGS__)) { \
    struct tvm_call __call = { call, __builtin_return_address(0) }; \
    call = &__call; \
    _rettype __r; \
    if (!can_enter_tvm(NULL)) { __r = CALL_FUNC(_name, __MAP(_x, __FN_ARGS, __VA_ARGS__)); } \
    else { __r = __tvm_##_name (__MAP(_x, __FN_ARGS, __VA_ARGS__)); } \
    call = call->prev; \
    return __r; \
} \
FUNCTION_METADATA(_name, &_name); \
_rettype __tvm_##_name(__MAP(_x, __FN_DECL, __VA_ARGS__))

#define __FUNCTION_DEFINE_VOIDx(_x, _name, ...) \
void __tvm_##_name(__MAP(_x, __FN_DECL, __VA_ARGS__)); \
void _name(__MAP(_x, __FN_DECL, __VA_ARGS__)) { \
    struct tvm_call __call = { call, __builtin_return_address(0) }; \
    call = &__call; \
    if (!can_enter_tvm(NULL)) { CALL_FUNC(_name, __MAP(_x, __FN_ARGS, __VA_ARGS__)); } \
    else { __tvm_##_name (__MAP(_x, __FN_ARGS, __VA_ARGS__)); } \
    call = call->prev; \
} \
FUNCTION_METADATA(_name, &_name); \
void __tvm_##_name(__MAP(_x, __FN_DECL, __VA_ARGS__))

#define __FUNCTION_DEFINE_NORETx(_x, _name, ...) \
void __tvm_##_name(__MAP(_x, __FN_DECL, __VA_ARGS__)); \
void _name(__MAP(_x, __FN_DECL, __VA_ARGS__)) { \
    struct tvm_call __call = { call, __builtin_return_address(0) }; \
    call = &__call; \
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
    struct tvm_call __call = { call, __builtin_return_address(0) }; \
    call = &__call; \
    va_list __ap; \
    va_start(__ap, __LAST(_x, __FN_ARGS, __VA_ARGS__)); \
    _rettype __r = __tvm_##_name##_va (__MAP(_x, __FN_ARGS, __VA_ARGS__), __ap); \
    va_end(__ap); \
    call = call->prev; \
    return __r; \
} \
FUNCTION_METADATA(_name, &_name); \
_rettype __tvm_##_name##_va(__MAP(_x, __FN_DECL, __VA_ARGS__), va_list __ap)

#define __FUNCTION_DEFINE_NORET_VAx(_x, _name, ...) \
void __tvm_##_name##_va(__MAP(_x, __FN_DECL, __VA_ARGS__), va_list __ap); \
void _name(__MAP(_x, __FN_DECL, __VA_ARGS__), ...) { \
    struct tvm_call __call = { call, __builtin_return_address(0) }; \
    call = &__call; \
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

#define FUNCTION_DEFINE_NORET_VA1(_name, ...) __FUNCTION_DEFINE_NORET_VAx(1, _name, __VA_ARGS__)
#define FUNCTION_DEFINE_NORET_VA2(_name, ...) __FUNCTION_DEFINE_NORET_VAx(2, _name, __VA_ARGS__)
#define FUNCTION_DEFINE_NORET_VA3(_name, ...) __FUNCTION_DEFINE_NORET_VAx(3, _name, __VA_ARGS__)
#define FUNCTION_DEFINE_NORET_VA4(_name, ...) __FUNCTION_DEFINE_NORET_VAx(4, _name, __VA_ARGS__)
#define FUNCTION_DEFINE_NORET_VA5(_name, ...) __FUNCTION_DEFINE_NORET_VAx(5, _name, __VA_ARGS__)
#define FUNCTION_DEFINE_NORET_VA6(_name, ...) __FUNCTION_DEFINE_NORET_VAx(6, _name, __VA_ARGS__)

/////////////
// TVM builtins
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

    pthread_mutex_lock(&tasks_lock);

    if (read_fdtable(&fdt, &fdn))
        goto err;
    
    if (!(fdr = calloc(sizeof(*fdr), fdn)))
        goto err;
    
    for (int i = 0; i < fdn; i++) {
        fdr[i].rfd = fdt[i].fd;
        fdr[i].type = LSOFT_UNKNOWN;

        int nfd;
        struct task *t;
        if ((t = task_for_rfd(fdr[i].rfd, 0, &nfd))) {
            fdr[i].type = LSOFT_PROC;
            fdr[i].proc.nfd = nfd;
            fdr[i].proc.fdtype = "unknown_type";
            fdr[i].proc.pid = t->tsk_pid;
            memcpy(fdr[i].proc.comm, t->tsk_comm, sizeof(t->tsk_comm));
            fdr[i].proc.comm[sizeof(t->tsk_comm)] = 0;
            
            if ((t->tsk_fd[nfd] & TFD_MASK) == 0)
                fdr[i].proc.fdtype = NULL;
            else if ((t->tsk_fd[nfd] & TFD_MASK) == TFD_TTY)
                fdr[i].proc.fdtype = "tty";
            
            task_unlock(t);
            continue;
        }
        
        struct tty *tt;
        if ((tt = tty_for_fd(fdr[i].rfd, TTM_MASTER))) {
            fdr[i].type = LSOFT_PTY;
            fdr[i].tty.mfd = tt->t_mfd;
            fdr[i].tty.sid = tt->t_sid;
            fdr[i].tty.end = "master";
            tty_unlock(tt, 1);
            continue;
        }
        if ((tt = tty_for_fd(fdr[i].rfd, TTM_SLAVE))) {
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
    pthread_mutex_unlock(&tasks_lock);
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
    char **p = tvm_environ;
    if (argc > 1)
        p = environ;
    for (; p && *p; p++)
        printf("%s\n", *p);
    return 0;
}

static int tvm_sorunner(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "USAGE: tvm sorunner LIBRARY\n");
        return 1;
    }

    char pathname_abs[PATH_MAX];
    if (!current_normalize_path(argv[1], pathname_abs)) {
        fprintf(stderr, "failed to exec \"%s\": -%d (%s)\n", pathname_abs, errno, strerror(errno));
        return 1;
    }

    void *handle = ulinker_dlopen(current->tsk_linker, pathname_abs, 0);
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
    ulinker_dladdr(current->tsk_linker, sym, &info);

    printf("sorunner %s %s %p base %p\n", argv[1], "main", sym, info.udli_fbase);

    uint64_t ret;
    int callerr = emulator_call(&ret, (uint64_t)sym,
                  (uint64_t)(argc - 1),
                  (uint64_t)(argv + 1),
                  (uint64_t)tvm_environ,
                0, 0, 0);
    if (callerr)
        printf("sorunner emulator_call failed!\n");

    return (callerr ? callerr : (int)ret);
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
        printf("\n");
        return 0;
    }

    if (0 == strcmp(argv[1], "ps")) return tvm_ps(argc - 1, argv + 1);
    if (0 == strcmp(argv[1], "lsof")) return tvm_lsof(argc - 1, argv + 1);
    if (0 == strcmp(argv[1], "pwd")) return tvm_pwd(argc - 1, argv + 1);
    if (0 == strcmp(argv[1], "env")) return tvm_env(argc - 1, argv + 1);
    if (0 == strcmp(argv[1], "sorunner")) return tvm_sorunner(argc - 1, argv + 1);
    
    fprintf(stderr, "tvm: invalid argument '%s'\n", argv[1]);
    fprintf(stderr, "Try 'tvm -h' for more information.\n");
    return 1;
}

/////////////
// API functions
/////////////

/**
 * I have no idea why `__panic()` is here and at this point, I'm too afraid to ask.
 */

static void rmem_print_cb(char *msg) {
    char __ts[32];
    format_time(__ts, sizeof(__ts));
    int __num = snprintf(msgbuf, sizeof(msgbuf), "%s Kernel(%d): RMEM: %s", __ts, __gettid(), msg);
    CALL_FUNC(write, g_outfd, msgbuf, __num);
}

static void ulinker_print_cb(char *msg) {
    char __ts[32];
    format_time(__ts, sizeof(__ts));
    int __num = snprintf(msgbuf, sizeof(msgbuf), "%s Kernel(%d): ulinker: %s", __ts, __gettid(), msg);
    CALL_FUNC(write, g_outfd, msgbuf, __num);
}

void *rmem_mmap(void *hint, size_t size, int prot, int flags, int fd, off_t offset) {
    return CALL_FUNC(mmap, hint, size, prot, flags, fd, offset);
}

int rmem_munmap(void *addr, size_t size) {
    return CALL_FUNC(munmap, addr, size);
}

struct rmem_ops rmem_ops = {
    .ro_mmap = rmem_mmap,
    .ro_munmap = rmem_munmap,
};

#define ULINKER_ARENA(_arg) ((_arg) ? ((struct task *)arg)->tsk_ar : global_arena)

static void *ulinker_malloc(void *arg, size_t size)             { return rmem_arena_alloc(ULINKER_ARENA(arg), size); }
static void  ulinker_free(void *arg, void *ptr)                 { rmem_arena_free(ULINKER_ARENA(arg), ptr); }
static void *ulinker_realloc(void *arg, void *ptr, size_t size) { return rmem_arena_realloc(ULINKER_ARENA(arg), ptr, size); }

static int     ulinker_open(void *arg, const char *name)                { return CALL_FUNC(open, name, O_RDONLY); }
static int     ulinker_close(void *arg, int fd)                         { return CALL_FUNC(close, fd); }
static ssize_t ulinker_read(void *arg, int fd, void *buf, size_t count) { return CALL_FUNC(read, fd, buf, count); }
static int     ulinker_fstat(void *arg, int fd, struct stat *statbuf)   { return CALL_FUNC(fstat, fd, statbuf); }

static void *ulinker_mmap(void *arg, void *hint, size_t length, int prot, int flags, int fd, off_t offset) { return CALL_FUNC(mmap, hint, length, prot, flags, fd, offset); }
static int   ulinker_munmap(void *arg, void *addr, size_t size)                                            { return CALL_FUNC(munmap, addr, size); }
static int   ulinker_mprotect(void *arg, void *addr, size_t size, int flags)                               { return CALL_FUNC(mprotect, addr, size, flags); }

static void *ulinker_call(void *arg, void *addr) {
    uint64_t ret;
    if (0 != emulator_call(&ret, (uint64_t)addr, 0, 0, 0, 0, 0, 0))
        panic("ulinker_call emulator_call failed");
    return (void *)ret;
}

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
};

static void *ulinker_external_dlsym(void *arg, const char *s)
{
    #ifndef ENABLE_RWX
    if (0 == strcmp(s, "sigaction")) return &emulator_sigaction;
    if (0 == strcmp(s, "signal"))    return &emulator_signal;
    if (0 == strcmp(s, "_setjmp"))   return &emulator_setjmp;  // linux
    if (0 == strcmp(s, "setjmp"))    return &emulator_setjmp;  // darwin
    if (0 == strcmp(s, "longjmp"))   return &emulator_longjmp;

    if (0 == strcmp(s, "pthread_create")) return &emulator_pthread_create;
    #endif
    return dlsym(RTLD_DEFAULT, s);
}

static struct ulinker_external_dlinfo_args ulinker_tvm_external_args = {
    .ue_private = NULL,
    .ue_name = "libtvm",
    .ue_dlsym = ulinker_external_dlsym,
};

static void ulinker_event_cb(int event, void *arg)
{
    if (event == ULEVENT_LOAD) {
        add_ulinker_loaded_range((struct ulinker_load_event *)arg);
    }
    else if (event == ULEVENT_UNLOAD) {
        remove_ulinker_loaded_range((struct ulinker_load_event *)arg);
    }
}

void tvm_init(const char *init_comm)
{
    rmem_setopt(RMEM_PRINT_CB, (void *)rmem_print_cb);
    rmem_setopt(RMEM_OPS, (void *)&rmem_ops);

    if (0 == dladdr(tvm_init, &main_dlinfo))
        panic("dladdr(tvm_init) failed");

    global_arena = rmem_arena_create(NULL);
    if (!global_arena)
        panic("couldn't create global arena");

    ulinker_setopt(ULINKER_PRINT_CB, (void *)ulinker_print_cb);
    ulinker_setopt(ULINKER_OPS, &ulinker_ops);
    ulinker_setopt(ULINKER_ADD_EXTERNAL_DLINFO, &ulinker_tvm_external_args);
    ulinker_setopt(ULINKER_EVENT_CB, (void *)ulinker_event_cb);

    current = taskalloc_common();
    if (!current)
        panic("couldn't create main task");

    main_task = current;
    current->tsk_pid = __gettid();
    current->tsk_sid = current->tsk_pgid = current->tsk_pid;

    for (int i = 0; i < 3; i++) {
        int rfd = CALL_FUNC(dup, i);
        if (-1 == rfd)
            panic("tvm_init::dup %d", i);
        current->tsk_fd[i] = rfd;
    }

    for (int i = 0; i < 3; i++) {
        current->tsk_f[i] = CALL_FUNC(fdopen, current->tsk_fd[i], (i?"w":"r"));
        if (!current->tsk_f[i])
            panic("tvm_init::fdopen");
        if (0 != CALL_FUNC(setvbuf, current->tsk_f[i], NULL, _IOLBF, 0))
            panic("tvm_init: setvbuf");
    }

    current->tsk_environ = copyenv(current->tsk_ar, environ);
    if (!current->tsk_environ)
        panic("tvm_init::copyenv");

    current_thread = threadalloc(current);
    if (!current_thread)
        panic("tvm_init::threadalloc");

    current_thread->thr_pthread = pthread_self();
    current_thread->thr_task = current;

    // wouldn't be able to fork if caller returned
    current_thread->thr_fork_fp = *(void **)__builtin_frame_address(0);

    list_add_tail(&current_thread->thr_list, &current->tsk_threads);
    current->tsk_threads_cnt++;

    current->tsk_cwd[0] = 0;
    if (!CALL_FUNC(getcwd, current->tsk_cwd, sizeof(current->tsk_cwd)))
        panic("tvm_init::getcwd");

    strncpy(current->tsk_comm, init_comm, sizeof(current->tsk_comm));

    cow_run_init_funcs(); // initialize copy-on-write vars with init statements
    
    struct sigaction act = {
        .sa_sigaction = signal_handler,
        .sa_flags = SA_SIGINFO | SA_RESTART,
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
}

int tvm_setopt(int opt, void *val) {
    if (opt == TVMOPT_OUTFD) {
        g_outfd = (int)(long)val;
        return 0;
    }
    if (opt == TVMOPT_SETFD) {
        int inner_fd = ((int *)val)[0];
        int outer_fd = ((int *)val)[1];

        // simple dup2 to old fd
        if (t_fd(inner_fd) != -1)
            return (-1 == CALL_FUNC(dup2, outer_fd, t_fd(inner_fd)));

        // dup new fd and set
        int rfd = CALL_FUNC(dup, outer_fd);
        if (rfd == -1)
            return 1;
        task_set_fd(current, inner_fd, rfd);
        return 0;
    }
    return 1;
}

char ***_tvm_environ()
{
    if (!main_task)
        return &environ;
    
    return &current->tsk_environ;
}

void tvm_log(const char *function, const char *msg, ...)
{
    int n = snprintf(msgbuf, sizeof(msgbuf), "%s(%d): %s: ", (current ? current->tsk_comm : "<null>"), __gettid(), function);

    va_list ap;
    va_start(ap, msg);
    n += vsnprintf(msgbuf + n, sizeof(msgbuf) - n, msg, ap);
    va_end(ap);

    n += snprintf(msgbuf + n, sizeof(msgbuf) - n, "\n");

    CALL_FUNC(write, g_outfd, msgbuf, n);
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

static void __panic(const char *msg, const char *file, int line)
{
    CALL_FUNC(fprintf, stderr, "\n");
    CALL_FUNC(fprintf, stderr, "panic(caller pid %d tid %d): \"%s\"\n", (current?current->tsk_pid:-1), __gettid(), msg);
    CALL_FUNC(fprintf, stderr, "%s:%d\n\n", file, line);
    
    CALL_FUNC(fprintf, stderr, "Thread Info:\n");
    CALL_FUNC(fprintf, stderr, " ppid: %d\n", (current?(current->tsk_parent?current->tsk_parent->tsk_pid:1):-1));
    CALL_FUNC(fprintf, stderr, " host pid: %d\n", CALL_FUNC(getpid));
    CALL_FUNC(fprintf, stderr, " current: %p\n", current);
    CALL_FUNC(fprintf, stderr, " main_task: %p\n", main_task);
    CALL_FUNC(fprintf, stderr, " errno: %d (%s)\n", errno, strerror(errno));
    CALL_FUNC(fprintf, stderr, "\n");

    CALL_FUNC(fprintf, stderr, "Stacktrace:\n");
    dump_stacktrace();

    CALL_FUNC(raise, SIGABRT);
}

/////////////
// Hooks
/////////////



/**
 * Oh yeah, there are SOOOO many more functions to hook.
 * Code here mostly re-calls original functions with file-descriptors/streams translated with `t_fd()`/`t_f()`.
 * Some code redirects the call to userspace device drivers.
 * Some code needs to alter the task-specific file-descriptors/signal-handlers table.
 * Also, handle variadic arguments when passing through... thats a big bummer.
 * 
 * TODO:`
 * - _FORTIFY_SOURCE - `__printf_chk()`, `__dup_chk()` etc (for now just don't compile with -Os)
 */

FUNCTION_DEFINE2(FILE *, fopen, const char *, pathname, const char *, mode)
{
    char pathname_abs[PATH_MAX];
    if (!current_normalize_path(pathname, pathname_abs))
        return NULL;
    pathname = pathname_abs;

    return CALL_FUNC(fopen, pathname, mode);
}

FUNCTION_DEFINE3(FILE *, freopen, const char *, pathname, const char *, mode, FILE *, stream)
{
    char pathname_abs[PATH_MAX];
    if (pathname) {
        if (!current_normalize_path(pathname, pathname_abs))
            return NULL;
        pathname = pathname_abs;
    }
    
    return CALL_FUNC(freopen, pathname, mode, t_f(stream));
}

FUNCTION_DEFINE2(FILE *, fdopen, int, fd, const char *, mode) {
    return CALL_FUNC(fdopen, t_fd(fd), mode);
}

FUNCTION_DEFINE1(DIR *, opendir, const char *, pathname) {
    char pathname_abs[PATH_MAX];
    if (!current_normalize_path(pathname, pathname_abs))
        return NULL;
    pathname = pathname_abs;

    return CALL_FUNC(opendir, pathname);
}

FUNCTION_DEFINE1(DIR *, fdopendir, int, fd) {
    return CALL_FUNC(fdopendir, t_fd(fd));
}

FUNCTION_DEFINE4(size_t, fwrite, const void *, ptr, size_t, size, size_t, nmemb, FILE *, stream) {
    struct fops *fops;
    if (!(fops = fops_for_stream(current, stream)))
        return CALL_FUNC(fwrite, ptr, size, nmemb, t_f(stream));
    
    if (!size)
        return 0;
    
    ssize_t r = fops_write(fops, fileno(stream), ptr, size * nmemb);
    if (r < 0)
        r = 0;
    
    return r / size;
}

FUNCTION_DEFINE4(size_t, fread, void *, ptr, size_t, size, size_t, nmemb, FILE *, stream) {
    struct fops *fops;
    if (!(fops = fops_for_stream(current, stream)))
        return CALL_FUNC(fread, ptr, size, nmemb, t_f(stream));
    
    if (!size)
        return 0;
    
    ssize_t r = fops_read(fops, fileno(stream), ptr, size * nmemb);
    if (r < 0)
        r = 0;
    
    return r / size;
}

FUNCTION_DEFINE3(int, fseek, FILE *, stream, long, offset, int, whence) {
    return CALL_FUNC(fseek, t_f(stream), offset, whence);
}

FUNCTION_DEFINE1(long, ftell, FILE *, stream) {
    return CALL_FUNC(ftell, t_f(stream));
}

FUNCTION_DEFINE1(int, fflush, FILE *, stream) {
    return CALL_FUNC(fflush, t_f(stream));
}

FUNCTION_DEFINE_VOID2(setbuf, FILE *, stream, char *, buf) {
    return CALL_FUNC(setbuf, t_f(stream), buf);
}

#if defined(__linux__)
FUNCTION_DEFINE_VOID3(setbuffer, FILE *, stream, char *, buf, size_t, size) {
#elif defined(__APPLE__)
FUNCTION_DEFINE_VOID3(setbuffer, FILE *, stream, char *, buf, int, size) {
#endif
    return CALL_FUNC(setbuffer, t_f(stream), buf, size);
}

#if defined(__linux__)
FUNCTION_DEFINE_VOID1(setlinebuf, FILE *, stream) {
#elif defined(__APPLE__)
FUNCTION_DEFINE1(int, setlinebuf, FILE *, stream) {
#endif
    return CALL_FUNC(setlinebuf, t_f(stream));
}

FUNCTION_DEFINE4(int, setvbuf, FILE *, stream, char *, buf, int, mode, size_t, size) {
    return CALL_FUNC(setvbuf, t_f(stream), buf, mode, size);
}

FUNCTION_DEFINE2(int, fputc, int, c, FILE *, stream) {
    struct fops *fops;
    if (!(fops = fops_for_stream(current, stream)))
        return CALL_FUNC(fputc, c, t_f(stream));
    
    if (1 == fops_write(fops, fileno(stream), &c, 1))
        return c;
    return EOF;
}

FUNCTION_DEFINE2(int, fputs, const char *, s, FILE *, stream) {
    struct fops *fops;
    if (!(fops = fops_for_stream(current, stream)))
        return CALL_FUNC(fputs, s, t_f(stream));
    
    ssize_t r = fops_write(fops, fileno(stream), s, strlen(s));
    if (r >= 0)
        return r;
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
    struct fops *fops;
    if (!(fops = fops_for_stream(current, stream)))
        return CALL_FUNC(fgetc, t_f(stream));
    
    char c;
    if (1 == fops_read(fops, fileno(stream), &c, 1))
        return (int)c;
    return EOF;
}

FUNCTION_DEFINE3(char *, fgets, char *, s, int, size, FILE *, stream) {
    struct fops *fops;
    if (!(fops = fops_for_stream(current, stream)))
        return  CALL_FUNC(fgets, s, size, t_f(stream));
    
    if (size <= 0)
        return NULL;
    if (size == 1) {
        s[0] = 0;
        return s;
    }
    
    int err = 0;
    int i;
    char c = 0;
    for (i = 0; i < (size - 1) && c != '\n'; i++)
    {
        ssize_t r = fops_read(fops, fileno(stream), &c, 1);
        if (r == -1)
            err = 1;
        if (1 != r)
            break;
        
        s[i] = c;
    }
    

    if (i == 0 && err)
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
    return CALL_FUNC(ungetc, c, t_f(stream));
}

FUNCTION_DEFINE_VOID1(clearerr, FILE *, stream) {
    return CALL_FUNC(clearerr, t_f(stream));
}

FUNCTION_DEFINE1(int, feof, FILE *, stream) {
    return CALL_FUNC(feof, t_f(stream));
}

FUNCTION_DEFINE1(int, ferror, FILE *, stream) {
    return CALL_FUNC(ferror, t_f(stream));
}

FUNCTION_DEFINE1(int, fileno, FILE *, stream) {
    return CALL_FUNC(fileno, t_f(stream));
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
    struct fops *fops;
    int fd = t_fdr(fileno(stream), 1);
    if (!(fops = fops_for_fd(current, fd)))
        return CALL_FUNC(vfprintf, t_f(stream), format, ap);
    
    return vdprintf(fd, format, ap);
}

FUNCTION_DEFINE3(int, vdprintf, int, fd, const char *, format, va_list, ap) {
    struct fops *fops;
    if (!(fops = fops_for_fd(current, fd)))
        return CALL_FUNC(vdprintf, t_fd(fd), format, ap);
    
    va_list ap2;
    va_copy(ap2, ap);
    int size = vsnprintf(NULL, 0, format, ap2);
    va_end(ap2);

    if (size < 0)
        return size;
    
    char *s = malloc(size+1);
    if (!s)
        return -1;
    
    int wsize = vsnprintf(s, size+1, format, ap);
    wsize = (int)fops_write(fops, t_fd(fd), s, wsize);
    free(s);
    return wsize;
}

FUNCTION_DEFINE_VA1(int, scanf, const char *, format) {
    return vfscanf(stdin, format, __ap);
}

FUNCTION_DEFINE_VA2(int, fscanf, FILE *, stream, const char *, format) {
    return vfscanf(stream, format, __ap);
}

FUNCTION_DEFINE2(int, vscanf, const char *, format, va_list, ap) {
    return CALL_FUNC(vfscanf, t_f(stdin), format, ap);
}

FUNCTION_DEFINE3(int, vfscanf, FILE *, stream, const char *, format, va_list, ap) {
    return CALL_FUNC(vfscanf, t_f(stream), format, ap);
}

FUNCTION_DEFINE_VOID1(perror, const char *, s) {
    fprintf(stderr, "%s: %s\n", s, strerror(errno));
}

FUNCTION_DEFINE_VA2(int, open, const char *, pathname, int, flags) {
    mode_t mode = va_arg(__ap, int);
    if (!can_enter_tvm(NULL))
        return CALL_FUNC(open, pathname, flags, mode);

    char pathname_abs[PATH_MAX];
    if (!current_normalize_path(pathname, pathname_abs))
        return -1;
    pathname = pathname_abs;

    int f = task_reserve_fd(current, 0);
    if (f == -1) {
        errno = EMFILE;
        return -1;
    }

    struct fops *fops = fops_for_open(pathname);

    int r;
    if (fops && fops->f_open)
        r = fops->f_open(pathname, flags, mode) | fops->f_fdflag;
    else
        r = CALL_FUNC(open, pathname, flags, mode);
    
    if (r == -1) {
        task_set_fd(current, f, -1);
        return r;
    }

    task_set_fd(current, f, r);
    return f;
}

FUNCTION_DEFINE2(int, creat, const char *, pathname, mode_t, mode) {
    char pathname_abs[PATH_MAX];
    if (!current_normalize_path(pathname, pathname_abs))
        return -1;
    pathname = pathname_abs;

    int r = CALL_FUNC(creat, pathname, mode);
    if (r == -1) {
        return r;
    }

    int f = task_new_fd(current, 0, r);
    if (f == -1) {
        CALL_FUNC(close, r);
        errno = ENOMEM;
    }
    return f;
}

FUNCTION_DEFINE_VA3(int, openat, int, dirfd, const char *, pathname, int, flags) {
    mode_t mode = va_arg(__ap, int);
    if (!can_enter_tvm(NULL))
        return CALL_FUNC(openat, dirfd, pathname, flags, mode);
    
    char pathname_abs[PATH_MAX];
    if (dirfd == AT_FDCWD) {
        if (!current_normalize_path(pathname, pathname_abs))
            return -1;
        pathname = pathname_abs;
    }

    int f = task_reserve_fd(current, 0);
    if (f == -1) {
        errno = EMFILE;
        return -1;
    }

    int r = CALL_FUNC(openat, t_fd(dirfd), pathname, flags, mode);
    
    if (r == -1) {
        task_set_fd(current, f, -1);
        return r;
    }

    task_set_fd(current, f, r);
    return f;
}

FUNCTION_DEFINE3(ssize_t, read, int, fd, void *, buf, size_t, count) {
    struct fops *fops;
    if (!(fops = fops_for_fd(current, fd)))
        return CALL_FUNC(read, t_fd(fd), buf, count);
    
    return fops_read(fops, t_fd(fd), buf, count);
}

FUNCTION_DEFINE3(ssize_t, write, int, fd, const void *, buf, size_t, count) {
    struct fops *fops;
    if (!(fops = fops_for_fd(current, fd)))
        return CALL_FUNC(write, t_fd(fd), buf, count);
    
    return fops_write(fops, t_fd(fd), buf, count);
}

FUNCTION_DEFINE1(int, close, int, fd) {
    task_lock(current);
    struct fops *f = fops_for_fd_locked(current, fd);
    int rfd = t_fd(fd);
    if (rfd == -1) {
        errno = EBADF;
        task_unlock(current);
        return -1;
    }
    current->tsk_fd[fd] = -1;
    task_unlock(current);

    // we had to "detach" the fd from `current` so we can call fops->close with task unlocked

    int r = 0;
    if (f && f->f_close)
        r = f->f_close(rfd);
    
    if (!r)
        r = CALL_FUNC(close, rfd);
    else
        CALL_FUNC(close, rfd);
    
    return r;
}

FUNCTION_DEFINE3(ssize_t, readv, int, fd, const struct iovec *, iov, int, iovcnt) {
    struct fops *fops;
    if (!(fops = fops_for_fd(current, fd)))
        return CALL_FUNC(readv, t_fd(fd), iov, iovcnt);
    
    // TODO: scatter-gather needs to be "atomic", maybe IO-lock per fops or per device instance?
    ssize_t total = 0;
    for (int i = 0; i < iovcnt; i++) {
        ssize_t curr = fops_read(fops, t_fd(fd), iov[i].iov_base, iov[i].iov_len);
        if (curr == 0)
            break;
        if (curr < 0) {
            if (total == 0)
                return -1;
            break;
        }

        total += curr;

        if (curr < iov[i].iov_len)
            break;
    }

    return total;
}

FUNCTION_DEFINE3(ssize_t, writev, int, fd, const struct iovec *, iov, int, iovcnt) {
    struct fops *fops;
    if (!(fops = fops_for_fd(current, fd)))
        return CALL_FUNC(writev, t_fd(fd), iov, iovcnt);
    
    // TODO: see readv()
    ssize_t total = 0;
    for (int i = 0; i < iovcnt; i++) {
        ssize_t curr = fops_write(fops, t_fd(fd), iov[i].iov_base, iov[i].iov_len);
        if (curr == 0)
            break;
        if (curr < 0) {
            if (total == 0)
                return -1;
            break;
        }

        total += curr;

        if (curr < iov[i].iov_len)
            break;
    }

    return total;
}

FUNCTION_DEFINE3(off_t, lseek, int, fd, off_t, offset, int, whence) {
    return CALL_FUNC(lseek, t_fd(fd), offset, whence);
}

FUNCTION_DEFINE1(int, fsync, int, fd) {
    return CALL_FUNC(fsync, t_fd(fd));
}

FUNCTION_DEFINE1(int, dup, int, oldfd) {
    return fcntl(oldfd, F_DUPFD, 0);
}

FUNCTION_DEFINE2(int, dup2, int, oldfd, int, newfd) {
    task_lock(current);
    int oldfd_real = t_fd(oldfd);
    int newfd_realprev = t_fd(newfd);
    int newfd_realprevflags = current->tsk_fd[newfd];
    int newfd_realnext = -1;

    if (oldfd_real < 0 || newfd < 0 || newfd >= MAX_FILES) {
        task_unlock(current);
        errno = EBADF;
        return -1;
    }

    // fd is currently reserved
    if (current->tsk_fd[newfd] < -1) {
        task_unlock(current);
        errno = EFAULT;
        return -1;
    }

    if (oldfd == newfd) {
        task_unlock(current);
        return newfd;
    }

    newfd_realnext = CALL_FUNC(dup, oldfd_real);
    if (newfd_realnext == -1) {
        task_unlock(current);
        return -1;
    }

    int newfd_realnextflags = newfd_realnext;

    struct fops *ofops = fops_for_fd_locked(current, oldfd);
    newfd_realnextflags |= fops_fdflag(ofops);
    current->tsk_fd[newfd] = newfd_realnextflags;
    struct fops *nfops = fops_for_fd_locked(current, newfd);
    task_unlock(current);

    // TODO from here on theres an inherent race with other threads in the same process
    //      if `fops_dup` fails. maybe we need to proc_fdlock() or smth

    if ((0 != fops_dup(ofops, oldfd_real, newfd_realnext))) {
        task_lock(current);
        current->tsk_fd[newfd] = newfd_realprevflags;
        CALL_FUNC(close, newfd_realnext);
        task_unlock(current);
        errno = EFAULT;
        return -1;
    }

    // from here on out, no failures

    if (nfops && nfops->f_close) {
        nfops->f_close(newfd_realprev);
    }

    CALL_FUNC(close, newfd_realprev);
    
    return newfd;
}

FUNCTION_DEFINE1(int, pipe, int *, pipefd) {
    int p[2];
    if (0 != CALL_FUNC(pipe, p))
        return -1;
    
    pipefd[0] = task_new_fd(current, 0, p[0]);
    if (pipefd[0] == -1) {
        CALL_FUNC(close, p[0]);
        CALL_FUNC(close, p[1]);
        errno = ENOMEM;
        return -1;
    }

    pipefd[1] = task_new_fd(current, 0, p[1]);
    if (pipefd[1] == -1) {
        close(pipefd[0]);
        CALL_FUNC(close, p[1]);
        errno = ENOMEM;
        return -1;
    }

    return 0;
}

FUNCTION_DEFINE5(int, select, int, nfds, fd_set *, readfds, fd_set *, writefds, fd_set *, exceptfds, struct timeval *, timeout) {
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

    int timeout_p = -1;
    if (timeout)
        timeout_p = (timeout->tv_sec * 1000) + (timeout->tv_usec / 1000);

    int r = poll(fds, fdcount, timeout_p);

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
        if (fds[i].revents & POLLIN) {
            FD_SET(fds[i].fd, readfds);
            r++;
        }
        if (fds[i].revents & POLLOUT) {
            FD_SET(fds[i].fd, writefds);
            r++;
        }
        if (fds[i].revents & POLLPRI) {
            FD_SET(fds[i].fd, exceptfds);
            r++;
        }
    }

    free(fds);
    return r;
}

FUNCTION_DEFINE3(int, poll, struct pollfd *, fds, nfds_t, nfds, int, timeout) {
    int ret = -1;
    struct pollfd *realfds = malloc(nfds * sizeof(*fds));
    if (!realfds) {
        errno = ENOMEM;
        return ret;
    }
    memcpy(realfds, fds, nfds * sizeof(*fds));

    for (int i = 0; i < nfds; i++)
        realfds[i].fd = t_fd(realfds[i].fd);

    ret = CALL_FUNC(poll, realfds, nfds, timeout);

    for (int i = 0; i < nfds; i++)
        realfds[i].fd = fds[i].fd;
    memcpy(fds, realfds, nfds * sizeof(*fds));

    free(realfds);
    return ret;
}

FUNCTION_DEFINE_VA2(int, fcntl, int, fd, int, cmd/*, ...  arg */ ) {
    void *arg = va_arg (__ap, void *);
    if (!can_enter_tvm(NULL))
        return CALL_FUNC(fcntl, fd, cmd, arg);
    
    if (cmd == F_DUPFD) {
        // TODO: F_DUPFD_CLOEXEC
        int f = task_reserve_fd(current, (int)(long)arg);
        if (-1 == f) {
            errno = EMFILE;
            return -1;
        }

        int r = task_set_fd(current, f, CALL_FUNC(dup, t_fd(fd)));
        if (r == -1) {
            return r;
        }

        task_lock(current);

        struct fops *fops = fops_for_fd_locked(current, fd);
        current->tsk_fd[f] |= fops_fdflag(fops);
        if (0 != fops_dup(fops, t_fd(fd), r)) {
            CALL_FUNC(close, r);
            current->tsk_fd[f] = -1;
            f = -1;
            errno = EFAULT;
        }
    
        task_unlock(current);
        return f;
    }
    return CALL_FUNC(fcntl, t_fd(fd), cmd, arg);
}

FUNCTION_DEFINE2(int, ftruncate, int, fd, off_t, length) {
    return CALL_FUNC(ftruncate, t_fd(fd), length);
}

FUNCTION_DEFINE2(int, stat, const char *, pathname, struct stat *, statbuf) {
    char pathname_abs[PATH_MAX];
    if (!current_normalize_path(pathname, pathname_abs))
        return -1;
    pathname = pathname_abs;

    struct fops *fops;
    if ((fops = fops_for_pathname(pathname))) {
        FOPS_PATH(fops, f_stat, pathname, statbuf);
    }

    if (find_program(pathname)) {
        statbuf->st_mode = S_IFREG | S_IXUSR | S_IXGRP | S_IXOTH;
        return 0;
    }
    
    return CALL_FUNC(stat, pathname, statbuf);
}

FUNCTION_DEFINE2(int, lstat, const char *, pathname, struct stat *, statbuf) {
    char pathname_abs[PATH_MAX];
    if (!current_normalize_path(pathname, pathname_abs))
        return -1;
    pathname = pathname_abs;

    if (find_program(pathname)) {
        statbuf->st_mode = S_IFREG | S_IXUSR | S_IXGRP | S_IXOTH;
        return 0;
    }

    return CALL_FUNC(lstat, pathname, statbuf);
}

FUNCTION_DEFINE2(int, fstat, int, fd, struct stat *, statbuf) {
    struct fops *fops;
    if (!(fops = fops_for_fd(current, fd)))
        return CALL_FUNC(fstat, t_fd(fd), statbuf);
    
    if (!fops->f_stat) {
        errno = ENOTSUP;
        return -1;
    }
    
    return fops->f_stat(t_fd(fd), statbuf);
}

FUNCTION_DEFINE4(int, fstatat, int, dirfd, const char *, pathname, struct stat *, statbuf, int, flags) {
    char pathname_abs[PATH_MAX];
    if (dirfd == AT_FDCWD) {
        if (!current_normalize_path(pathname, pathname_abs))
            return -1;
        pathname = pathname_abs;
    }

    return CALL_FUNC(fstatat, t_fd(dirfd), pathname, statbuf, flags);
}

FUNCTION_DEFINE2(int, access, const char *, pathname, int, mode) {
    char pathname_abs[PATH_MAX];
    if (!current_normalize_path(pathname, pathname_abs))
        return -1;
    pathname = pathname_abs;

    // make sure the shell finds our programs
    if (find_program(pathname))
        return 0;

    if (mode & X_OK) {
        mode &= ~X_OK;
        if (!mode)
            mode = F_OK;
    }

    return CALL_FUNC(access, pathname, mode);
}

FUNCTION_DEFINE4(int, faccessat, int, dirfd, const char *, pathname, int, mode, int, flags) {
    char pathname_abs[PATH_MAX];
    if (dirfd == AT_FDCWD) {
        if (!current_normalize_path(pathname, pathname_abs))
            return -1;
        pathname = pathname_abs;
    }

    return CALL_FUNC(faccessat, t_fd(dirfd), pathname, mode, flags);
}

FUNCTION_DEFINE2(int, chmod, const char *, pathname, mode_t, mode) {
    char pathname_abs[PATH_MAX];
    if (!current_normalize_path(pathname, pathname_abs))
        return -1;
    pathname = pathname_abs;

    struct fops *fops;
    if (!(fops = fops_for_pathname(pathname)))
        return CALL_FUNC(chmod, pathname, mode);

    FOPS_PATH(fops, f_chmod, pathname, mode);
}

FUNCTION_DEFINE2(int, lchmod, const char *, pathname, mode_t, mode) {
    char pathname_abs[PATH_MAX];
    if (!current_normalize_path(pathname, pathname_abs))
        return -1;
    pathname = pathname_abs;

    return CALL_FUNC(lchmod, pathname, mode);
}

FUNCTION_DEFINE2(int, fchmod, int, fd, mode_t, mode) {
    struct fops *fops;
    if (!(fops = fops_for_fd(current, fd)))
        return CALL_FUNC(fchmod, t_fd(fd), mode);
    
    if (!fops->f_chmod) {
        errno = ENOTSUP;
        return -1;
    }
    
    return fops->f_chmod(t_fd(fd), mode);
}

// TODO support dirfd properly
FUNCTION_DEFINE4(int, fchmodat, int, dirfd, const char *, pathname, mode_t, mode, int, flags) {
    char pathname_abs[PATH_MAX];
    if (dirfd == AT_FDCWD) {
        if (!current_normalize_path(pathname, pathname_abs))
            return -1;
        pathname = pathname_abs;
    }

    return CALL_FUNC(fchmodat, t_fd(dirfd), pathname, mode, flags);
}

FUNCTION_DEFINE3(int, chown, const char *, pathname, uid_t, owner, gid_t, group) {
    char pathname_abs[PATH_MAX];
    if (!current_normalize_path(pathname, pathname_abs))
        return -1;
    pathname = pathname;

    struct fops *fops;
    if (!(fops = fops_for_pathname(pathname)))
        return CALL_FUNC(chown, pathname, owner, group);

    FOPS_PATH(fops, f_chown, pathname, owner, group);
}

FUNCTION_DEFINE3(int, lchown, const char *, pathname, uid_t, owner, gid_t, group) {
    char pathname_abs[PATH_MAX];
    if (!current_normalize_path(pathname, pathname_abs))
        return -1;
    pathname = pathname;

    return CALL_FUNC(lchown, pathname, owner, group);
}

FUNCTION_DEFINE3(int, fchown, int, fd, uid_t, owner, gid_t, group) {
    struct fops *fops;
    if (!(fops = fops_for_fd(current, fd)))
        return CALL_FUNC(fchown, t_fd(fd), owner, group);
    
    if (!fops->f_chown) {
        errno = ENOTSUP;
        return -1;
    }
    
    return fops->f_chown(t_fd(fd), owner, group);
}

FUNCTION_DEFINE5(int, fchownat, int, dirfd, const char *, pathname, uid_t, owner, gid_t, group, int, flags) {
    char pathname_abs[PATH_MAX];
    if (dirfd == AT_FDCWD) {
        if (!current_normalize_path(pathname, pathname_abs))
            return -1;
        pathname = pathname_abs;
    }

    return CALL_FUNC(fchownat, t_fd(dirfd), pathname, owner, group, flags);
}

FUNCTION_DEFINE2(int, flock, int, fd, int, operation) {
    return CALL_FUNC(flock, t_fd(fd), operation);
}

FUNCTION_DEFINE3(int, lockf, int, fd, int, cmd, off_t, len) {
    return CALL_FUNC(lockf, t_fd(fd), cmd, len);
}

FUNCTION_DEFINE3(ssize_t, readlink, const char *, pathname, char *, buf, size_t, bufsiz) {
    char pathname_abs[PATH_MAX];
    if (!current_normalize_path(pathname, pathname_abs))
        return -1;
    pathname = pathname_abs;

    return CALL_FUNC(readlink, pathname, buf, bufsiz);
}

FUNCTION_DEFINE4(ssize_t, readlinkat, int, dirfd, const char *, pathname, char *, buf, size_t, bufsiz) {
    char pathname_abs[PATH_MAX];
    if (dirfd == AT_FDCWD) {
        if (!current_normalize_path(pathname, pathname_abs))
            return -1;
        pathname = pathname_abs;
    }

    return CALL_FUNC(readlinkat, t_fd(dirfd), pathname, buf, bufsiz);
}

FUNCTION_DEFINE2(int, symlink, const char *, target, const char *, linkpath) {
    char linkpath_abs[PATH_MAX];
    if (!current_normalize_path(linkpath, linkpath_abs))
        return -1;
    linkpath = linkpath_abs;

    return CALL_FUNC(symlink, target, linkpath);
}

FUNCTION_DEFINE3(int, symlinkat, const char *, target, int, newdirfd, const char *, linkpath) {
    char linkpath_abs[PATH_MAX];
    if (newdirfd == AT_FDCWD) {
        if (!current_normalize_path(linkpath, linkpath_abs))
            return -1;
        linkpath = linkpath_abs;
    }

    return CALL_FUNC(symlinkat, target, t_fd(newdirfd), linkpath);
}

FUNCTION_DEFINE2(int, link, const char *, oldpath, const char *, newpath) {
    char oldpath_abs[PATH_MAX];
    if (!current_normalize_path(oldpath, oldpath_abs))
        return -1;
    oldpath = oldpath_abs;

    char newpath_abs[PATH_MAX];
    if (!current_normalize_path(newpath, newpath_abs))
        return -1;
    newpath = newpath_abs;

    return CALL_FUNC(link, oldpath, newpath);
}

FUNCTION_DEFINE5(int, linkat, int, olddirfd, const char *, oldpath, int, newdirfd, const char *, newpath, int, flags) {
    char oldpath_abs[PATH_MAX];
    if (olddirfd == AT_FDCWD) {
        if (!current_normalize_path(oldpath, oldpath_abs))
            return -1;
        oldpath = oldpath_abs;
    }

    char newpath_abs[PATH_MAX];
    if (newdirfd == AT_FDCWD) {
        if (!current_normalize_path(newpath, newpath_abs))
            return -1;
        newpath = newpath_abs;
    }

    return CALL_FUNC(linkat, t_fd(olddirfd), oldpath, t_fd(newdirfd), newpath, flags);
}

FUNCTION_DEFINE2(int, rename, const char *, oldpath, const char *, newpath) {
    char oldpath_abs[PATH_MAX];
    if (!current_normalize_path(oldpath, oldpath_abs))
        return -1;
    oldpath = oldpath_abs;

    char newpath_abs[PATH_MAX];
    if (!current_normalize_path(newpath, newpath_abs))
        return -1;
    newpath = newpath_abs;

    return CALL_FUNC(rename, oldpath, newpath);
}

FUNCTION_DEFINE4(int, renameat, int, olddirfd, const char *, oldpath, int, newdirfd, const char *, newpath) {
    char oldpath_abs[PATH_MAX];
    if (olddirfd == AT_FDCWD) {
        if (!current_normalize_path(oldpath, oldpath_abs))
            return -1;
        oldpath = oldpath_abs;
    }

    char newpath_abs[PATH_MAX];
    if (newdirfd == AT_FDCWD) {
        if (!current_normalize_path(newpath, newpath_abs))
            return -1;
        newpath = newpath_abs;
    }

    return CALL_FUNC(renameat, t_fd(olddirfd), oldpath, t_fd(newdirfd), newpath);
}

FUNCTION_DEFINE1(int, unlink, const char *, pathname) {
    char pathname_abs[PATH_MAX];
    if (!current_normalize_path(pathname, pathname_abs))
        return -1;
    pathname = pathname_abs;

    return CALL_FUNC(unlink, pathname);
}

FUNCTION_DEFINE3(int, unlinkat, int, dirfd, const char *, pathname, int, flags) {
    char pathname_abs[PATH_MAX];
    if (dirfd == AT_FDCWD) {
        if (!current_normalize_path(pathname, pathname_abs))
            return -1;
        pathname = pathname_abs;
    }

    return CALL_FUNC(unlinkat, t_fd(dirfd), pathname, flags);
}

FUNCTION_DEFINE2(int, mkdir, const char *, pathname, mode_t, mode) {
    char pathname_abs[PATH_MAX];
    if (!current_normalize_path(pathname, pathname_abs))
        return -1;
    pathname = pathname_abs;

    return CALL_FUNC(mkdir, pathname, mode);
}

FUNCTION_DEFINE3(int, mkdirat, int, dirfd, const char *, pathname, mode_t, mode) {
    char pathname_abs[PATH_MAX];
    if (dirfd == AT_FDCWD) {
        if (!current_normalize_path(pathname, pathname_abs))
            return -1;
        pathname = pathname_abs;
    }

    return CALL_FUNC(mkdirat, t_fd(dirfd), pathname, mode);
}

FUNCTION_DEFINE1(int, rmdir, const char *, pathname) {
    char pathname_abs[PATH_MAX];
    if (!current_normalize_path(pathname, pathname_abs))
        return -1;
    pathname = pathname_abs;

    return CALL_FUNC(rmdir, pathname);
}

FUNCTION_DEFINE2(int, mkfifo, const char *, pathname, mode_t, mode) {
    char pathname_abs[PATH_MAX];
    if (!current_normalize_path(pathname, pathname_abs))
        return -1;
    pathname = pathname_abs;

    return CALL_FUNC(mkfifo, pathname, mode);
}

FUNCTION_DEFINE3(int, mkfifoat, int, dirfd, const char *, pathname, mode_t, mode) {
    char pathname_abs[PATH_MAX];
    if (dirfd == AT_FDCWD) {
        if (!current_normalize_path(pathname, pathname_abs))
            return -1;
        pathname = pathname_abs;
    }

    return CALL_FUNC(mkfifoat, t_fd(dirfd), pathname, mode);
}

FUNCTION_DEFINE1(int, mkstemp, char *, template) {
    int f = task_reserve_fd(current, 0);
    if (f == -1) {
        errno = EMFILE;
        return -1;
    }

    int r = CALL_FUNC(mkstemp, template);
    
    if (r == -1) {
        task_set_fd(current, f, -1);
        return r;
    }

    task_set_fd(current, f, r);
    return f;
}

FUNCTION_DEFINE3(int,  socket, int, domain, int, type, int, protocol) {
    int r = CALL_FUNC(socket, domain, type, protocol);
    if (r == -1) {
        return r;
    }

    int f = task_new_fd(current, 0, r);
    if (f == -1) {
        CALL_FUNC(close, r);
        errno = ENOMEM;
    }
    return f;
}

FUNCTION_DEFINE3(int, accept, int, sockfd, struct sockaddr *, addr, socklen_t *, addrlen) {
    int r = CALL_FUNC(accept, t_fd(sockfd), addr, addrlen);
    if (r == -1) {
        return r;
    }

    // TODO: allocate before calling `accept()`
    int f = task_new_fd(current, 0, r);
    if (f == -1) {
        CALL_FUNC(close, r);
        errno = ENOMEM;
    }
    return f;
}

FUNCTION_DEFINE3(int, bind, int, sockfd, const struct sockaddr *, addr, socklen_t, addrlen) {
    return CALL_FUNC(bind, t_fd(sockfd), addr, addrlen);
}

FUNCTION_DEFINE2(int, listen, int, sockfd, int,  backlog) {
    return CALL_FUNC(listen, t_fd(sockfd), backlog);
}

FUNCTION_DEFINE3(int, connect, int, sockfd, const struct sockaddr *, addr, socklen_t, addrlen) {
    return CALL_FUNC(connect, t_fd(sockfd), addr, addrlen);
}

FUNCTION_DEFINE3(int, getpeername, int, sockfd, struct sockaddr *, addr, socklen_t *, addrlen) {
    return CALL_FUNC(getpeername, t_fd(sockfd), addr, addrlen);
}

FUNCTION_DEFINE3(int, getsockname, int, sockfd, struct sockaddr *, addr, socklen_t *, addrlen) {
    return CALL_FUNC(getsockname, t_fd(sockfd), addr, addrlen);
}

FUNCTION_DEFINE5(int, getsockopt, int, sockfd, int, level, int, optname, void *, optval, socklen_t *, optlen) {
    return CALL_FUNC(getsockopt, t_fd(sockfd), level, optname, optval, optlen);
}

FUNCTION_DEFINE5(int, setsockopt, int, sockfd, int, level, int, optname, const void *, optval, socklen_t, optlen) {
    return CALL_FUNC(setsockopt, t_fd(sockfd), level, optname, optval, optlen);
}

FUNCTION_DEFINE4(ssize_t, send, int, sockfd, const void *, buf, size_t, len, int, flags) {
    return CALL_FUNC(send, t_fd(sockfd), buf, len, flags);
}

FUNCTION_DEFINE6(ssize_t, sendto, int, sockfd, const void *, buf, size_t, len, int, flags, const struct sockaddr *, dest_addr, socklen_t, addrlen) {
    return CALL_FUNC(sendto, t_fd(sockfd), buf, len, flags, dest_addr, addrlen);
}

FUNCTION_DEFINE3(ssize_t, sendmsg, int, sockfd, const struct msghdr *, msg, int, flags) {
    return CALL_FUNC(sendmsg, t_fd(sockfd), msg, flags);
}

FUNCTION_DEFINE4(ssize_t, recv, int, sockfd, void *, buf, size_t, len, int, flags) {
    return CALL_FUNC(recv, t_fd(sockfd), buf, len, flags);
}

FUNCTION_DEFINE6(ssize_t, recvfrom, int, sockfd, void *, buf, size_t, len, int, flags, struct sockaddr *, src_addr, socklen_t *, addrlen) {
    return CALL_FUNC(recvfrom, t_fd(sockfd), buf, len, flags, src_addr, addrlen);
}

FUNCTION_DEFINE3(ssize_t, recvmsg, int, sockfd, struct msghdr *, msg, int, flags) {
    return CALL_FUNC(recvmsg, t_fd(sockfd), msg, flags);
}

FUNCTION_DEFINE2(int, shutdown, int, sockfd, int, how) {
    return CALL_FUNC(shutdown, t_fd(sockfd), how);
}

FUNCTION_DEFINE4(int, socketpair, int, domain, int, type, int, protocol, int *, sv) {
    int p[2];
    if (0 != CALL_FUNC(socketpair, domain, type, protocol, p))
        return -1;
    
    sv[0] = task_new_fd(current, 0, p[0]);
    if (sv[0] == -1) {
        CALL_FUNC(close, p[0]);
        CALL_FUNC(close, p[1]);
        errno = ENOMEM;
        return -1;
    }

    sv[1] = task_new_fd(current, 0, p[1]);
    if (sv[1] == -1) {
        close(sv[0]);
        CALL_FUNC(close, p[1]);
        errno = ENOMEM;
        return -1;
    }

    return 0;
}

FUNCTION_DEFINE_VA2(int, ioctl, int, fd, unsigned long, request) {
    void* arg = va_arg(__ap, void*);
    if (!can_enter_tvm(NULL))
        return CALL_FUNC(ioctl, fd, request, arg);

    struct fops *fops;
    if (!(fops = fops_for_fd(current, fd)))
        return CALL_FUNC(ioctl, t_fd(fd), request, arg);
    
    if (!fops->f_ioctl) {
        errno = ENOTSUP;
        return -1;
    }
    
    return fops->f_ioctl(t_fd(fd), request, arg);
    
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
    if (arg->duplicate_errors == 0)
        cow_fixup_process(&arg->fixup);
}

static void *tvm_proc_entry(struct fork_arg *arg)
{
    uintptr_t curr_fp = (uintptr_t)__builtin_frame_address(0);

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

        // include prologue (fp + lr) when adjusting jmpbuf context regs
        jmpbuf_setstack(&arg->jmpbuf_tmp, next_sp, curr_sp, curr_fp - curr_sp + 2 * sizeof(uintptr_t));
        jmpbuf_mangle(&arg->jmpbuf_tmp);

        longjmp(arg->jmpbuf_tmp, 1);
    }

    // running on a temporary stack until the next longjmp

    // load and copy all data vars
    if (0 != cow_fixup_load_data_variables(&arg->fixup, 1))
        goto fail;

    for (int i = 0; i < arg->fixup.count; i++)
        memcpy(arg->fixup.ptrs[i].new, arg->fixup.ptrs[i].old, arg->fixup.ptrs[i].size);

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

    // include prologue (fp + lr) when adjusting jmpbuf context regs (important for fixing up pointers to rbp)
    INFO("STACK %p %p %p", (void *)parent_sp, (void *)child_sp, (void *)(parent_fp - parent_sp + 2 * sizeof(uintptr_t)));
    if (0 != cow_fixup_ctx_add(&arg->fixup, (void *)child_sp, (void *)parent_sp, parent_fp - parent_sp + 2 * sizeof(uintptr_t), 1))
        goto fail;

    // fix the environ pointer (which is allocated on the task's arena)
    arg->tsk->tsk_environ = arg->parent->tsk_environ;
    if (0 != cow_fixup_ctx_add(&arg->fixup, (void *)&arg->tsk->tsk_environ, (void *)&arg->parent->tsk_environ, sizeof(arg->tsk->tsk_environ), 1))
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

    arg->thr->thr_fork_fp = (void *)child_fp;

    // include prologue (fp + lr) when adjusting jmpbuf context regs
    //jmpbuf_setstack(&arg->jmpbuf_parent, child_sp, parent_sp, parent_fp - parent_sp + 2 * sizeof(uintptr_t));

    // fix the jmpbuf
    void **jb = (void **) (&arg->jmpbuf_parent);
    for (int i = 0; i < (sizeof(jmp_buf)/8); i++)
        jb[i] = cow_fixup_translate(&arg->fixup, jb[i]);

    jmpbuf_mangle(&arg->jmpbuf_parent);

    longjmp(arg->jmpbuf_parent, 1);

fail:

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
pid_t fork(void) {
    if (!can_enter_tvm(__builtin_return_address(0)))
        return CALL_FUNC(fork);
    
    struct task *tsk = NULL;
    struct thread *thr = NULL;
    struct fork_arg *arg = NULL;
    pthread_attr_t attr;
    pid_t ret = -1;
    int r;

    if ((r = pthread_attr_init(&attr))) {
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

    arg->pid = 0;
    arg->tsk = tsk;
    arg->thr = thr;
    arg->parent = current;
    arg->parent_thr = current_thread;

    arg->tmp_stack = CALL_FUNC(mmap, NULL, TEMP_STACK_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
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

    if (0 != cow_fixup_load_data_variables(&arg->fixup, 0)) {
        // HANDLE??
        goto out;
    }

    if (0 != setjmp(arg->jmpbuf_parent)) {
        
        // child

        current = arg->tsk;
        current_thread = arg->thr;

        current->tsk_pid = __gettid();
        current_thread->thr_pthread = pthread_self();

        list_add_tail(&current_thread->thr_list, &current->tsk_threads);
        current->tsk_threads_cnt++;

        // finally, add child task to tasks list
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

    pthread_t pth;
    if ((r = CALL_FUNC(pthread_create, &pth, &attr, (void *(*)(void *))tvm_proc_entry, (void *)arg))) {
        errno = r;
        goto out;
    }

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
            CALL_FUNC(munmap, arg->tmp_stack, TEMP_STACK_SIZE);
            arg->tmp_stack = NULL;
        }        

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

    return ret;
}

typedef void *(*start_routine_fn)(void *);
struct pthread_create_arg
{
    start_routine_fn start_fn;
    void *start_arg;
    struct task *tsk;
    struct thread *thr;
};

static void *tvm_thread_entry(struct pthread_create_arg *arg)
{
    current = arg->tsk;
    current_thread = arg->thr;

    current_thread->thr_pthread = pthread_self();
    current_thread->thr_fork_fp = (void *)__builtin_frame_address(0);

    task_lock(current);
    list_add_tail(&current_thread->thr_list, &current->tsk_threads);
    current->tsk_threads_cnt++;
    task_unlock(current);

    start_routine_fn start_fn = arg->start_fn;
    void *start_arg = arg->start_arg;
    kfree(arg);

    pthread_exit(start_fn(start_arg));
}

FUNCTION_DEFINE4(int, pthread_create, pthread_t *, thread, const pthread_attr_t *, attr, start_routine_fn, start_routine, void *, _arg) {
    int ret = EINVAL;
    struct pthread_create_arg *arg = NULL;
    struct thread *thr = NULL;

    task_lock(current);
    current->tsk_refcount++;
    task_unlock(current);

    thr = threadalloc(current);
    if (!thr) {
        ret = ENOMEM;
        goto out;
    }

    if (attr) {
        if (0 != (ret = copy_pthread_attr(attr, &thr->thr_pthread_attr)))
            goto out;
        ret = EINVAL;
    }

    arg = kalloc(sizeof(struct pthread_create_arg));
    if (!arg) {
        ret = ENOMEM;
        goto out;
    }

    arg->start_fn = start_routine;
    arg->start_arg = _arg;
    arg->thr = thr;

    arg->tsk = current;

    ret = CALL_FUNC(pthread_create, thread, attr, (start_routine_fn)tvm_thread_entry, (void *)arg);

    if (!ret) {
        arg = NULL;
        thr = NULL;
    }

    // TODO condition variable wait for child to succeed

out:
    if (ret) {

        if (arg) {
            kfree(arg);
            arg = NULL;
        }

        if (thr) {
            threaddealloc(thr);
            thr = NULL;
        }

        task_lock(current);
        current->tsk_refcount--;
        task_unlock(current);
    }

    return ret;
}

FUNCTION_DEFINE_NORET1(pthread_exit, void *, retval) {

    task_lock(current);
    if (current->tsk_threads_cnt == 1) {
        task_unlock(current);
        exit((int) ((long) retval));
    }
    
    list_del(&current_thread->thr_list);
    current->tsk_threads_cnt--;
    current->tsk_refcount--;

    if (current_thread->thr_refcount == 1) {
        threaddealloc(current_thread);
    }

    current_thread = NULL;

    task_unlock(current);

    CALL_FUNC(pthread_exit, retval);
    __builtin_unreachable();
}

FUNCTION_DEFINE_NORET1(exit, int, status) {
    task_lock(current);
    exit_t exits[sizeof(current->tsk_exits)/sizeof(*current->tsk_exits)];
    memcpy(&exits, current->tsk_exits, sizeof(exits));
    memset(&current->tsk_exits, 0, sizeof(current->tsk_exits));
    task_unlock(current);

    for (int i = 0; i < sizeof(current->tsk_exits)/sizeof(*current->tsk_exits); i++) {
        if (exits[i]) {
            if (0 != emulator_call(NULL, (uint64_t)exits[i], 0, 0, 0, 0, 0, 0))
                abort();
        }
    }

    _exit(status);
}

FUNCTION_DEFINE_NORET1(_exit, int, status) {
    if (pthread_self() == main_pthread)
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

#if defined(__linux__)
int __cxa_atexit(void (*func)(void *), void *arg, void *dso_handle) {
    return atexit((exit_t)func);
}
#endif

//FUNCTION_DEFINE1(int, atexit, exit_t, function) {
int atexit(exit_t function) {
    
    if (!can_enter_tvm(NULL))
        return 0;

    if (!function) {
        errno = EINVAL;
        return -1;
    }

    task_lock(current);
    for (int i = (sizeof(current->tsk_exits)/sizeof(*current->tsk_exits)) - 1; i >= 0; i--) {
        if (!current->tsk_exits[i]) {
            current->tsk_exits[i] = function;
            task_unlock(current);
            return 0;
        }
    }
    task_unlock(current);

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
    int saved_errno = errno;

    if (*current->tsk_comm)
        fprintf(stderr, "%s: ", current->tsk_comm);

    if (fmt) {
        vfprintf(stderr, fmt, args);
        fprintf(stderr, ": ");
    }

    fprintf(stderr, "%s\n", strerror(saved_errno));
    exit(eval);
}

FUNCTION_DEFINE_NORET3(verrx, int, eval, const char *, fmt, va_list, args) {
    if (*current->tsk_comm)
        fprintf(stderr, "%s: ", current->tsk_comm);

    if (fmt) {
        vfprintf(stderr, fmt, args);
    }

    fprintf(stderr, "\n");
    exit(eval);
}

FUNCTION_DEFINE0(pid_t, getpid) {
    return current->tsk_pid;
}

FUNCTION_DEFINE0(pid_t, getppid) {
    if (main_task == current)
        return CALL_FUNC(getppid);

    task_lock(current);
    pid_t ppid = (current->tsk_parent ? current->tsk_parent->tsk_pid : 1);
    task_unlock(current);

    return ppid;
}

FUNCTION_DEFINE1(pid_t, wait, int *, wstatus) {
    return waitpid(-1, wstatus, 0);
}

FUNCTION_DEFINE3(pid_t, waitpid, pid_t, pid, int *, wstatus, int, options) {
    struct task *zombiet = NULL;
    int found;

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
            errno = ECHILD;
            return -1;
        }
        if (options & WNOHANG) {
            pthread_mutex_unlock(&tasks_lock);
            return 0;
        }
        pthread_cond_wait(&current->tsk_wait_cond, &tasks_lock);
    }

    task_lock(zombiet);
    pthread_mutex_unlock(&tasks_lock);
    
    if (list_empty(&zombiet->tsk_threads))
        panic("waitpid tsk_pthreads empty");
    struct thread *thr = list_entry(zombiet->tsk_threads.next, struct thread, thr_list);
    pthread_join(thr->thr_pthread, NULL);
    
    pid_t childpid = zombiet->tsk_pid;
    if (wstatus)
        *wstatus = zombiet->tsk_result;

    // lock ordering: tasks_lock before task
    task_unlock(zombiet);
    taskfreelastref(zombiet, 1, 1);

    return childpid;
}

FUNCTION_DEFINE0(pid_t, setsid) {
    pthread_mutex_lock(&tasks_lock);
    task_lock(current);

    if (-1 == task_set_pgid(current, current->tsk_pid)) {
        task_unlock(current);
        pthread_mutex_unlock(&tasks_lock);
        return -1;
    }

    current->tsk_sid = current->tsk_pid;
    current->tsk_state &= ~TS_CTTY;

    task_unlock(current);
    pthread_mutex_unlock(&tasks_lock);

    // no need to lock because only set in `taskalloc()`
    return current->tsk_pid;
}

FUNCTION_DEFINE1(pid_t, getsid, pid_t, pid) {
    if (!pid)
        pid = current->tsk_pid;
    
    struct task *t = task_for_pid(pid, 1);
    if (!t)
        return CALL_FUNC(getsid, pid);
    pid_t sid = t->tsk_sid;
    task_unlock(t);

    return sid;
}

FUNCTION_DEFINE2(int, setpgid, pid_t, pid, pid_t, pgid) {
    if (pgid < 0) {
        errno = EINVAL;
        return -1;
    }

    pthread_mutex_lock(&tasks_lock);

    if (!pid)
        pid = current->tsk_pid;
    
    if (!pgid)
        pgid = pid;

    struct task *t = task_for_pid(pid, 0);
    if (!t) {
        pthread_mutex_unlock(&tasks_lock);
        return CALL_FUNC(setpgid, pid, pgid);
    }

    int ret = task_set_pgid(t, pgid);
    task_unlock(t);
    pthread_mutex_unlock(&tasks_lock);
    return ret;
}

FUNCTION_DEFINE1(pid_t, getpgid, pid_t, pid) {
    if (!pid)
        pid = current->tsk_pid;
    
    struct task *t = task_for_pid(pid, 1);
    if (!t)
        return CALL_FUNC(getpgid, pid);
    
    pid_t pgid = t->tsk_pgid;
    task_unlock(t);
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
    if (signum >= MAX_SIGNALS || signum < 1 ||
        signum == SIGKILL || signum == SIGSTOP) {
        errno = EINVAL;
        return -1;
    }

    task_lock(current);
    if (oldact)
        memcpy(oldact, current->tsk_sighandlers + signum, sizeof(*current->tsk_sighandlers));
    if (act)
        memcpy(current->tsk_sighandlers + signum, act, sizeof(*current->tsk_sighandlers));
    task_unlock(current);

    return 0;
}

FUNCTION_DEFINE1(int, raise, int, sig) {
    int r = pthread_kill(pthread_self(), sig);
    if (r != 0) {
        errno = r;
        r = -1;
    }
    return r;
}

FUNCTION_DEFINE2(int, kill, pid_t, pid, int, sig) {
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
        return -1;
    }

    int self = 0;
    int ntasks = 0;
    int kills_confirmed = 0;
    
    pthread_mutex_lock(&tasks_lock);
    struct task *t;
    list_for_each_entry(t, &main_task->tsk_list, tsk_list) {
        if (t->tsk_pid == pid || t->tsk_pgid == -pid) {
            if (t == current) {
                self = 1;
                continue;
            }

            ntasks++;
            task_lock(t);
            if (t->tsk_state & TS_ZOMBIE)
                errno = ESRCH;
            else if (0 == task_kill_locked(t, sig))
                kills_confirmed++;
            task_unlock(t);
        }
    }
    pthread_mutex_unlock(&tasks_lock);

    if (!self && !ntasks) {
        errno = ESRCH;
        return -1;
    }
    
    // must save self for last because if we die from the signal, we miss some of the target recipients
    if (self) {
        if (0 == raise(sig))
            kills_confirmed++;
    }

    return (kills_confirmed ? 0 : -1);
}

FUNCTION_DEFINE1(char *, getenv, const char *, name) {
    task_lock(current);
    char **env = NULL;
    if (current->tsk_environ)
        env = findenv(current->tsk_environ, name, strlen(name));

    if (!env) {
        task_unlock(current);
        return NULL;
    }
    
    char *ret = strchr(*env, '=');
    task_unlock(current);
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
    
    task_lock(current);
    char **env = NULL;
    if (current->tsk_environ)
        env = findenv(current->tsk_environ, string, klen);
    
    if (env) {
        *env = string;
        task_unlock(current);
        return 0;
    }

    char **new_environ = addenv(current->tsk_environ, string);
    if (!new_environ) {
        task_unlock(current);
        errno = ENOMEM;
        return -1;
    }

    // TODO not sure if this is a good idea
    //free(current->tsk_environ);
    current->tsk_environ = new_environ;
    task_unlock(current);

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

    task_lock(current);
    char **env = NULL;
    if (current->tsk_environ)
        env = findenv(current->tsk_environ, name, strlen(name));
    
    if (env) {
        if (overwrite)
            *env = string;
        else
            free(string);
        task_unlock(current);
        return 0;    
    }

    char **new_environ = addenv(current->tsk_environ, string);
    if (!new_environ) {
        task_unlock(current);
        free(string);
        errno = ENOMEM;
        return -1;
    }

    // TODO not sure if this is a good idea
    //free(current->tsk_environ);
    current->tsk_environ = new_environ;
    task_unlock(current);

    return 0;
}

FUNCTION_DEFINE1(int, unsetenv, const char *, name) {
    if (name == NULL || *name == '\0' || strchr(name, '=') != NULL)
    {
        errno = EINVAL;
        return -1;
    }

    size_t len = strlen(name);

    task_lock(current);
    if (!current->tsk_environ) {
        task_unlock(current);
        return 0;
    }

    for (char **e = current->tsk_environ; *e != NULL;) {
	    if (!strncmp (*e, name, len) && (*e)[len] == '=') {
            for (char **e2 = e; *e2 != NULL; e2++)
                e2[0] = e2[1];
	        continue;
	    }
        e++;
    }

    task_unlock(current);

    return 0;
}

FUNCTION_DEFINE3(int, execve, const char *, pathname, char * const *, argv, char * const *, envp) {
    char pathname_abs[PATH_MAX];
    if (!current_normalize_path(pathname, pathname_abs))
        return -1;
    pathname = pathname_abs;

    INFO("execve(\"%s\")", pathname);

    int argc = 0;
    for (char * const*a = argv; *a && argc < INT_MAX; a++)
        argc++;
    
    if (argc == INT_MAX) {
        INFO("  failed(E2BIG)");
        errno = E2BIG;
        return -1;
    }

    main_func_t main_func = find_program(pathname);
    if (!main_func) {

        struct ulinker *new_linker = ulinker_create((void *)current);
        if (!new_linker) {
            INFO("   failed(ENOMEM)");
            errno = ENOMEM;
            return -1;
        }

        struct ulinker *old_linker = current->tsk_linker;
        current->tsk_linker = current_thread->thr_emulator->ec_ulinker = new_linker;

        void *handle = ulinker_dlopen(new_linker, pathname, 0);
        if (!handle) {
            current->tsk_linker = current_thread->thr_emulator->ec_ulinker = old_linker;
            ulinker_destroy(new_linker);
            errno = ENOENT;
            return -1;
        }

        void *entry = ulinker_dlsym(new_linker, handle, "main", 1);
        if (!entry) {
            INFO("failed to load executable library \"%s\": symbol \"main\" not found", pathname);
            current->tsk_linker = current_thread->thr_emulator->ec_ulinker = old_linker;
            ulinker_destroy(new_linker);
            errno = EINVAL;
            return -1;
        }

        ulinker_destroy(old_linker);

        main_func = (main_func_t)entry;
    }

    if (!main_func) {
        INFO("  failed(ENOENT)");
        errno = ENOENT;
        return -1;
    }

    // No failing from here!

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
    strncpy(current->tsk_comm, basename, sizeof(current->tsk_comm));
    memset(&current->tsk_exits, 0, sizeof(current->tsk_exits));
    task_unlock(current);

    for (int i = 0; i < MAX_FILES; i++) {
        if (t_fd(i) == -1) 
            continue;
        
        int fd_flags = fcntl(i, F_GETFD);
        if (-1 == fd_flags)
            panic("execve(\"%s\") fcntl(%d, F_GETFD)", pathname, i);
        if (fd_flags & FD_CLOEXEC) {
            if (0 != close(i))
                panic("execve(\"%s\") close(%d)", pathname, i);
        }
    }

    struct sigaction act = { .sa_handler = SIG_DFL };
    for (int i = 1; i < MAX_SIGNALS; i++) {
        if (i == SIGKILL || i == SIGSTOP)
            continue;
        if (0 != sigaction(i, &act, NULL))
            panic("sigaction(%d)", i);
    }
    
    // TODO if 0, 1, 2 are closed we can be nice and open /dev/full, /dev/null and /dev/null for the program

    // TODO purge rmem heap + mmaps

    cow_run_init_funcs(); // re-initialize copy-on-write vars with init statements

    // TODO hack, getopt should have a tvm thread-safe version
    extern int optind;
    optind = 1;

    uint64_t ret;
    int callerr = emulator_call(&ret, (uint64_t)main_func,
                  (uint64_t)argc,
                  (uint64_t)argv,
                  (uint64_t)envp,
                0, 0, 0);
    if (callerr)
        printf("execve emulator_call failed!\n");

    exit(callerr ? callerr : (int)ret);
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
    return execve(pathname, argv, tvm_environ);
}

FUNCTION_DEFINE2(int, execvp, const char *, file, char * const *, argv) {
    INFO("execvp: %s", file);
    char file_abs[PATH_MAX];
    if (strchr(file, '/')) {
        if (!current_normalize_path(file, file_abs))
            return -1;
        file = file_abs;
    }
    else {
        const char *pathname = find_program_pathname(file);
        if (!pathname) {
            errno = ENOENT;
            return -1;
        }
        file = pathname;
    }

    return execve(file, argv, tvm_environ);
}

#if defined(__linux__)
FUNCTION_DEFINE5(int, openpty, int *, amaster, int *, aslave, char *, name, const struct termios *, termp, const struct winsize *, winp) {
#elif defined(__APPLE__)
FUNCTION_DEFINE5(int, openpty, int *, amaster, int *, aslave, char *, name, struct termios *, termp, struct winsize *, winp) {
#endif
    // TODO
    if (name || termp || winp)
        panic("openpty todo");

    int master = task_reserve_fd(current, 0);
    if (master == -1) {
        errno = EMFILE;
        return -1;
    }

    if (-1 == task_set_fd(current, master, ptm_open() | TFD_TTY))
        return -1;

    char sname[PATH_MAX];
    if (0 != ptsname_r(master, sname, PATH_MAX)) {
        close(master);
        return -1;
    }
    INFO("OPENPTY OPEN %s", sname);
    int slave = open(sname, O_RDWR | O_NOCTTY);
    if (-1 == slave) {
        close(master);
        return -1;
    }

    *amaster = master;
    *aslave = slave;
    return 0;
}

FUNCTION_DEFINE1(int, isatty, int, fd) {
    if (islocaltty(fd))
        return 1;
    
    return CALL_FUNC(isatty, t_fd(fd));
}

FUNCTION_DEFINE3(int, ttyname_r, int, fd, char *, buf, size_t, buflen) {
    if (!islocaltty(fd))
        return CALL_FUNC(ttyname_r, t_fd(fd), buf, buflen);
    
    struct tty *tt = tty_for_fd(t_fd(fd), TTM_MASTER);
    if (tt) {
        snprintf(buf, buflen, "%s", PTMX_FILE);
        tty_unlock(tt, 1);
        return 0;
    }

    return tty_slavename(t_fd(fd), buf, buflen);
}

FUNCTION_DEFINE1(char *, ttyname, int, fd) {
    if (!islocaltty(fd))
        return CALL_FUNC(ttyname, t_fd(fd));

    int r = ttyname_r(fd, tty_name, sizeof(tty_name));
    if (r) {
        errno = r;
        return NULL;
    }

    return tty_name;
}

FUNCTION_DEFINE3(int, ptsname_r, int, fd, char *, buf, size_t, buflen) {
    if (!islocaltty(fd))
        return CALL_FUNC(ptsname_r, t_fd(fd), buf, buflen);
    
    return tty_slavename(t_fd(fd), buf, buflen);
}

FUNCTION_DEFINE1(char *, ptsname, int, fd) {
    if (!islocaltty(fd))
        return CALL_FUNC(ptsname, t_fd(fd));

    int r = ptsname_r(fd, pts_name, sizeof(pts_name));
    if (r) {
        errno = r;
        return NULL;
    }

    return pts_name;
}

FUNCTION_DEFINE1(pid_t, tcgetpgrp, int, fd) {
    if (!islocaltty(fd))
        return CALL_FUNC(tcgetpgrp, t_fd(fd));
    
    pid_t ret;
    if (0 != ttyop_ioctl(t_fd(fd), TIOCGPGRP, &ret))
        return -1;
    return ret;
}

FUNCTION_DEFINE2(int, tcsetpgrp, int, fd, pid_t, pgrp) {
    if (!islocaltty(fd))
        return CALL_FUNC(tcsetpgrp, t_fd(fd), pgrp);
    
    return ttyop_ioctl(t_fd(fd), TIOCSPGRP, &pgrp); 
}

FUNCTION_DEFINE2(int, tcgetattr, int, fd, struct termios *, termios_p) {
    if (!islocaltty(fd))
        return CALL_FUNC(tcgetattr, t_fd(fd), termios_p);

#if defined(__linux__)
    return ttyop_ioctl(t_fd(fd), TCGETS, termios_p);
#elif defined(__APPLE__)
    return ttyop_ioctl(t_fd(fd), TIOCGETA, termios_p);
#endif
}

FUNCTION_DEFINE3(int, tcsetattr, int, fd, int, optional_actions, const struct termios *, termios_p) {
    if (!islocaltty(fd))
        return CALL_FUNC(tcsetattr, t_fd(fd), optional_actions, termios_p);
    
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

    return ttyop_ioctl(t_fd(fd), (unsigned)request, (void *)termios_p);
}

FUNCTION_DEFINE2(int, tcsendbreak, int, fd, int, duration) {
    if (!islocaltty(fd))
        return CALL_FUNC(tcsendbreak, t_fd(fd), duration);
    
#if defined(__linux__)
    return ttyop_ioctl(t_fd(fd), TCSBRK, (void *)(long)duration);
#elif defined(__APPLE__)
	if (ttyop_ioctl(t_fd(fd), TIOCSBRK, 0) == -1)
		return -1;
	usleep(400000);
	if (ttyop_ioctl(t_fd(fd), TIOCCBRK, 0) == -1)
		return -1;
	return 0;
#endif
}

FUNCTION_DEFINE1(int, tcdrain, int, fd) {
    if (!islocaltty(fd))
        return CALL_FUNC(tcdrain, t_fd(fd));
    
#if defined(__linux__)
    return tcsendbreak(fd, 1);
#elif defined(__APPLE__)
    return ttyop_ioctl(t_fd(fd), TIOCDRAIN, NULL);
#endif
}

FUNCTION_DEFINE2(int, tcflush, int, fd, int, queue_selector) {
    if (!islocaltty(fd))
        return CALL_FUNC(tcflush, t_fd(fd), queue_selector);

#if defined(__linux__)
    return ttyop_ioctl(t_fd(fd), TCFLSH, (void *)(long)queue_selector);
#elif defined(__APPLE__)
    int what = FREAD | FWRITE;
    if (queue_selector == TCIFLUSH)
        what = FREAD;
    else if (queue_selector == TCOFLUSH)
        what = FWRITE;
    return ttyop_ioctl(t_fd(fd), TIOCFLUSH, &what);
#endif
}

FUNCTION_DEFINE2(int, tcflow, int, fd, int, action) {
    if (!islocaltty(fd))
        return CALL_FUNC(tcflow, t_fd(fd), action);

#if defined(__linux__)
    return ttyop_ioctl(t_fd(fd), TCXONC, (void *)(long)action);
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

    return ttyop_ioctl(t_fd(fd), request, NULL);
#endif
}

FUNCTION_DEFINE1(void *, malloc, size_t, size) {
    return calloc(1, size);
}

FUNCTION_DEFINE2(void *, calloc, size_t, nmemb, size_t, size) {
    if (0 == (nmemb * size))
        return NULL;

    struct rmem_arena *ar = arena_for_task(current);
    if (!ar)
        return NULL;

    void *p = rmem_arena_alloc(ar, nmemb * size);
    if (NULL == p)
        return NULL;
    
    memset(p, 0, nmemb * size);
    return p;
}

FUNCTION_DEFINE_VOID1(free, void *, ptr) {
    struct rmem_arena *ar = arena_for_task(current);
    if (!ar)
        panic("free for task without arena %p", current);

    rmem_arena_free(ar, ptr);
}

FUNCTION_DEFINE2(void *, realloc, void *, ptr, size_t, size) {
    struct rmem_arena *ar = arena_for_task(current);
    if (!ar)
        panic("realloc for task without arena %p", current);

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

    ssize_t r = CALL_FUNC(getdelim, &tmp, &tmpn, delim, t_f(stream));
    if (r < 0) {
        CALL_FUNC(free, tmp);
        return -1;
    }

    if (!*lineptr || *n < (size_t)r + 1) {
        char *newp = realloc(*lineptr, (size_t)r + 1);
        if (!newp) {
            CALL_FUNC(free, tmp);
            return -1;
        }
        *lineptr = newp;
        *n = (size_t)r + 1;
    }

    memcpy(*lineptr, tmp, (size_t)r + 1);
    CALL_FUNC(free, tmp);

    return r;
}

FUNCTION_DEFINE6(void *, mmap, void *, addr, size_t, length, int, prot, int, flags, int, fd, off_t, offset) {
    return CALL_FUNC(mmap, addr, length, prot, flags, t_fd(fd), offset);
}

FUNCTION_DEFINE3(int, getopt, int, argc, char * const *, argv, const char *, optstring) {
    pthread_mutex_lock(&getopt_lock);
    optarg = tvm_optarg;
    optind = tvm_optind;
    opterr = tvm_opterr;
    optopt = tvm_optopt;
    int r = CALL_FUNC(getopt, argc, argv, optstring);
    tvm_optarg = optarg;
    tvm_optind = optind;
    tvm_opterr = opterr;
    tvm_optopt = optopt;
    pthread_mutex_unlock(&getopt_lock);
    return r;
}

FUNCTION_DEFINE5(int, getopt_long, int, argc, char * const *, argv, const char *, optstring, const struct option *, longopts, int *, longindex) {
    pthread_mutex_lock(&getopt_lock);
    optarg = tvm_optarg;
    optind = tvm_optind;
    opterr = tvm_opterr;
    optopt = tvm_optopt;
    int r = CALL_FUNC(getopt_long, argc, argv, optstring, longopts, longindex);
    tvm_optarg = optarg;
    tvm_optind = optind;
    tvm_opterr = opterr;
    tvm_optopt = optopt;
    pthread_mutex_unlock(&getopt_lock);
    return r;
}

FUNCTION_DEFINE5(int, getopt_long_only, int, argc, char * const *, argv, const char *, optstring, const struct option *, longopts, int *, longindex) {
    pthread_mutex_lock(&getopt_lock);
    optarg = tvm_optarg;
    optind = tvm_optind;
    opterr = tvm_opterr;
    optopt = tvm_optopt;
    int r = CALL_FUNC(getopt_long_only, argc, argv, optstring, longopts, longindex);
    tvm_optarg = optarg;
    tvm_optind = optind;
    tvm_opterr = opterr;
    tvm_optopt = optopt;
    pthread_mutex_unlock(&getopt_lock);
    return r;
}

FUNCTION_DEFINE2(char *, realpath, const char *, file_name, char *, resolved_name)
{
    int c = 0;
    char tmp[PATH_MAX];
    tmp[0] = 0;

    task_lock(current);
    if (file_name[0] == '/')
        c = snprintf(tmp, PATH_MAX, "%s", file_name);
    else if (file_name[0] == 0)
        c = snprintf(tmp, PATH_MAX, "%s", current->tsk_cwd);
    else
        c = snprintf(tmp, PATH_MAX, "%s/%s", current->tsk_cwd, file_name);
    task_unlock(current);

    if (c >= PATH_MAX) {
        errno = ENAMETOOLONG;
        return NULL;
    }
    
    char *tmp2 = CALL_FUNC(realpath, tmp, resolved_name);
    if (!tmp2)
        return NULL;

    if (resolved_name)
        return tmp2;

    char *ret = malloc(strlen(tmp2));
    if (ret)
        strcpy(ret, tmp2);
    CALL_FUNC(free, tmp2);
    return ret;
}

FUNCTION_DEFINE1(int, chdir, const char *, path) {
    char abs_path[PATH_MAX];
    if (!current_normalize_path(path, abs_path))
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

    task_lock(current);
    strcpy(current->tsk_cwd, path);
    task_unlock(current);

    return 0;
}

FUNCTION_DEFINE1(int, fchdir, int, fildes) {
    char tmp[PATH_MAX];
    if (0 > fd_getpath(t_fd(fildes), tmp, sizeof(tmp)))
        return -1;

    return chdir(tmp);
}

FUNCTION_DEFINE2(char *, getcwd, char *, buf, size_t, size) {
    task_lock(current);
    if (buf != NULL) {
        strncpy(buf, current->tsk_cwd, size);
        task_unlock(current);
        return buf;
    }
    
    char *ret = strdup(current->tsk_cwd);
    task_unlock(current);
    return ret;
}

FUNCTION_DEFINE1(char *, getwd, char *, buf) {
    return getcwd(buf, PATH_MAX);
}



#if defined(__linux__)

/////////////
// Linux-specific hooks
/////////////

FUNCTION_DEFINE3(int, mknod, const char *, pathname, mode_t, mode, dev_t, dev) {
    char pathname_abs[PATH_MAX];
    if (!current_normalize_path(pathname, pathname_abs))
        return -1;
    pathname = pathname_abs;

    return CALL_FUNC(mknod, pathname, mode, dev);
}

FUNCTION_DEFINE4(int, mknodat, int, dirfd, const char *, pathname, mode_t, mode, dev_t, dev) {
    char pathname_abs[PATH_MAX];
    if (dirfd == AT_FDCWD) {
        if (!current_normalize_path(pathname, pathname_abs))
            return -1;
        pathname = pathname_abs;
    }

    return CALL_FUNC(mknodat, t_fd(dirfd), pathname, mode, dev);
}

FUNCTION_DEFINE3(int, execvpe, const char *, file, char * const *, argv, char * const *, envp) {
    char file_abs[PATH_MAX];
    if (strchr(file, '/')) {
        if (!current_normalize_path(file, file_abs))
            return -1;
        file = file_abs;
    }
    else {
        const char *pathname = find_program_pathname(file);
        if (!pathname) {
            errno = ENOENT;
            return -1;
        }
        file = pathname;
    }

    return execve(file, argv, envp);
}

FUNCTION_DEFINE0(int, clearenv) {
    task_lock(current);
    current->tsk_environ = NULL;
    task_unlock(current);

    return 0;
}

FUNCTION_DEFINE1(int, epoll_create, int, size) {
    int r = CALL_FUNC(epoll_create, size);
    if (r == -1) {
        return r;
    }

    int f = task_new_fd(current, 0, r);
    if (f == -1) {
        CALL_FUNC(close, r);
        errno = ENOMEM;
    }
    return f;
}

FUNCTION_DEFINE1(int, epoll_create1, int, flags) {
    int r = CALL_FUNC(epoll_create1, flags);
    if (r == -1) {
        return r;
    }

    int f = task_new_fd(current, 0, r);
    if (f == -1) {
        CALL_FUNC(close, r);
        errno = ENOMEM;
    }
    return f;
}

FUNCTION_DEFINE2(int, timerfd_create, int, clockid, int, flags) {
    int r = CALL_FUNC(timerfd_create, clockid, flags);
    if (r == -1) {
        return r;
    }

    int f = task_new_fd(current, 0, r);
    if (f == -1) {
        CALL_FUNC(close, r);
        errno = ENOMEM;
    }

    return f;
}

FUNCTION_DEFINE2(int, pipe2, int *, pipefd, int, flags) {
    int p[2];
    if (0 != CALL_FUNC(pipe2, p, flags))
        return -1;
    
    pipefd[0] = task_new_fd(current, 0, p[0]);
    if (pipefd[0] == -1) {
        CALL_FUNC(close, p[0]);
        CALL_FUNC(close, p[1]);
        errno = ENOMEM;
        return -1;
    }

    pipefd[1] = task_new_fd(current, 0, p[1]);
    if (pipefd[1] == -1) {
        close(pipefd[0]);
        CALL_FUNC(close, p[1]);
        errno = ENOMEM;
        return -1;
    }

    return 0;
}

FUNCTION_DEFINE2(int, fstatfs, int, fd, struct statfs *, buf) {
    return CALL_FUNC(fstatfs, t_fd(fd), buf);
}

FUNCTION_DEFINE2(int, fstatfs64, int, fd, struct statfs64 *, buf) {
    return CALL_FUNC(fstatfs64, t_fd(fd), buf);
}

FUNCTION_DEFINE_VA2(int, open64, const char *, pathname, int, flags) {
    mode_t mode = va_arg(__ap, int);
    if (!can_enter_tvm(NULL))
        return CALL_FUNC(open64, pathname, flags, mode);

    char pathname_abs[PATH_MAX];
    if (!current_normalize_path(pathname, pathname_abs))
        return -1;
    pathname = pathname_abs;

    int f = task_reserve_fd(current, 0);
    if (f == -1) {
        errno = EMFILE;
        return -1;
    }

    struct fops *fops = fops_for_open(pathname);

    int r;
    if (fops && fops->f_open)
        r = fops->f_open(pathname, flags | O_LARGEFILE, mode) | fops->f_fdflag;
    else
        r = CALL_FUNC(open, pathname, flags | O_LARGEFILE, mode);
    
    if (r == -1) {
        task_set_fd(current, f, -1);
        return r;
    }

    task_set_fd(current, f, r);
    return f;
}

FUNCTION_DEFINE_VA3(int, openat64, int, dirfd, const char *, pathname, int, flags) {
    mode_t mode = va_arg(__ap, int);
    if (!can_enter_tvm(NULL))
        return CALL_FUNC(openat64, dirfd, pathname, flags, mode);

    char pathname_abs[PATH_MAX];
    if (dirfd == AT_FDCWD) {
        if (!current_normalize_path(pathname, pathname_abs))
            return -1;
        pathname = pathname_abs;
    }

    int f = task_reserve_fd(current, 0);
    if (f == -1) {
        errno = EMFILE;
        return -1;
    }

    int r = CALL_FUNC(openat, t_fd(dirfd), pathname, flags | O_LARGEFILE, mode);

    if (r == -1) {
        task_set_fd(current, f, -1);
        return r;
    }

    task_set_fd(current, f, r);
    return f;
}

FUNCTION_DEFINE4(int, fstatat64, int, dirfd, const char *, pathname, struct stat64 *, statbuf, int, flags) {
    char pathname_abs[PATH_MAX];
    if (dirfd == AT_FDCWD) {
        if (!current_normalize_path(pathname, pathname_abs))
            return -1;
        pathname = pathname_abs;
    }

    return CALL_FUNC(fstatat64, t_fd(dirfd), pathname, statbuf, flags);
}

FUNCTION_DEFINE3(off_t, lseek64, int, fd, off64_t, offset, int, whence) {
    return CALL_FUNC(lseek64, t_fd(fd), offset, whence);
}

FUNCTION_DEFINE2(int, ftruncate64, int, fd, off64_t, length) {
    return CALL_FUNC(ftruncate64, t_fd(fd), length);
}

FUNCTION_DEFINE0(char *, get_current_dir_name) {
    char *pwd = getenv("PWD");
    if (pwd)
        return strdup(pwd);
    return getcwd(NULL, 0);
}

#endif
