#include "tvm_bindings.h"

#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

// Resolved from libtvm.dylib at runtime. exec/chdir must go through libtvm's
// tvm_execvp/tvm_chdir trampolines: the interposed execvp/chdir dispatch by
// return address (can_enter_tvm), and a caller in this app binary's text is
// outside every TVM range — dlsym'ing them directly silently fell through to
// the HOST functions (host execvp("dropbear") -> ENOENT).
typedef void (*tvm_init_with_outfd_fn)(const char *init_comm, int outfd);
typedef int (*tvm_mount_cpio_fn)(const char *pathname, const void *buf,
                                 unsigned long size);
typedef int (*execvp_fn)(const char *file, char *const argv[]);
typedef int (*chdir_fn)(const char *path);

static tvm_init_with_outfd_fn fn_tvm_init;
static tvm_mount_cpio_fn fn_tvm_mount_cpio;
static execvp_fn fn_tvm_execvp;
static chdir_fn fn_tvm_chdir;

// Park the worker thread forever after a startup failure.  It must not
// plain-return (after tvm_init this thread IS the init task's thread; the
// pthread TLS destructor would tear the task down with the thread still
// registered and panic "taskdealloc tsk_threads_cnt 1"), and it must not run
// TVM's exit() either — the init task's teardown closes every fd in its table,
// which it inherited from the whole app at tvm_init, killing the app silently.
// Parking keeps the app (and its log pipe) alive with the failure visible.
__attribute__((noreturn)) static void worker_park(void)
{
    for (;;)
        sleep(3600);
}

static int pipe_read = -1;
static int pipe_write = -1;
static int worker_port = 0;
static char worker_initrd[PATH_MAX];
static char worker_home[PATH_MAX];
static tvm_output_cb worker_cb;

// Log a line to the console. Once the pipe exists it goes through the reader
// thread (→ Dart callback → stderr + UI). During the synchronous part of
// tvm_start (before the pipe/reader exist) it goes straight to the Dart
// callback, which owns and frees the malloc'd copy.
static void tvm_log(const char *fmt, ...)
{
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n <= 0)
        return;
    size_t len = (size_t)n < sizeof(buf) ? (size_t)n : sizeof(buf) - 1;

    if (pipe_write >= 0) {
        (void)write(pipe_write, buf, len);
    } else if (worker_cb) {
        char *copy = malloc(len);
        if (copy) {
            memcpy(copy, buf, len);
            worker_cb(copy, (int)len);
        }
    }
}

// Reader thread: blocks on the pipe and forwards each chunk to the Dart
// callback (which owns the malloc'd copy). Dart is the single sink — it
// mirrors everything to stderr for `devicectl --console` and to the UI.
static void *reader_entry(void *arg)
{
    (void)arg;
    char tmp[4096];
    for (;;) {
        ssize_t n = read(pipe_read, tmp, sizeof(tmp));
        if (n <= 0)
            break;
        if (worker_cb) {
            char *copy = malloc(n);
            if (copy) {
                memcpy(copy, tmp, n);
                worker_cb(copy, (int)n);
            }
        }
    }
    return NULL;
}

// Worker thread: initializes TVM, mounts the initrd, execs dropbear.
//
// Failure paths end in worker_park() — see its comment for why neither a
// plain return nor a TVM exit() is allowed here.
static void *worker_entry(void *arg)
{
    (void)arg;

    tvm_log("tvm_start: init\n");
    fn_tvm_init("tvm", pipe_write);

    int ifd = open(worker_initrd, O_RDONLY);
    if (ifd < 0) {
        tvm_log("tvm_start: open(%s) failed: %s\n", worker_initrd,
                strerror(errno));
        worker_park();
    }
    struct stat st;
    if (fstat(ifd, &st) != 0) {
        tvm_log("tvm_start: fstat(initrd) failed: %s\n", strerror(errno));
        worker_park();
    }
    void *initrd_data = malloc(st.st_size);
    if (!initrd_data) {
        tvm_log("tvm_start: malloc(%lld) for initrd failed\n",
                (long long)st.st_size);
        worker_park();
    }
    ssize_t rd = read(ifd, initrd_data, st.st_size);
    close(ifd);
    if (rd != (ssize_t)st.st_size) {
        tvm_log("tvm_start: short read of initrd (%zd/%lld): %s\n", rd,
                (long long)st.st_size, strerror(errno));
        worker_park();
    }
    if (fn_tvm_mount_cpio("/tvm", initrd_data, st.st_size) != 0)
        tvm_log("tvm_start: tvm_mount_cpio(/tvm) failed\n");
    free(initrd_data);

    if (fn_tvm_chdir(worker_home) != 0)
        tvm_log("tvm_start: chdir(%s) failed: %s\n", worker_home,
                strerror(errno));

    tvm_log("tvm_start: exec dropbear\n");
    char port_str[16];
    sprintf(port_str, "%d", worker_port);
    char *argv[] = {"sshd", "-R", "-e", "-F", "-E", "-p", port_str, NULL};
    fn_tvm_execvp("dropbear", argv);

    tvm_log("tvm_start: execvp(dropbear) failed: %s\n", strerror(errno));
    worker_park();
    return NULL;
}

TVM_EXPORT int tvm_start(const char *libtvm_path, int port,
                         const char *password, const char *initrc_path,
                         const char *initrd_path, tvm_output_cb on_output)
{
    if (pipe_write >= 0) {
        tvm_log("tvm_start: already running\n");
        return -1;
    }

    // Enable logging for the synchronous phase below (goes to on_output until
    // the pipe/reader are up).
    worker_cb = on_output;

    // Resolve the TVM kernel entry points from libtvm.dylib.
    void *h = dlopen(libtvm_path, RTLD_NOW | RTLD_GLOBAL);
    if (!h) {
        tvm_log("tvm_start: dlopen(%s) failed: %s\n", libtvm_path, dlerror());
        return -1;
    }
    fn_tvm_init = (tvm_init_with_outfd_fn)dlsym(h, "tvm_init_with_outfd");
    fn_tvm_mount_cpio = (tvm_mount_cpio_fn)dlsym(h, "tvm_mount_cpio");
    fn_tvm_execvp = (execvp_fn)dlsym(h, "tvm_execvp");
    fn_tvm_chdir = (chdir_fn)dlsym(h, "tvm_chdir");
    if (!fn_tvm_init || !fn_tvm_mount_cpio || !fn_tvm_execvp || !fn_tvm_chdir) {
        tvm_log("tvm_start: dlsym failed "
                "(init=%p mount=%p execvp=%p chdir=%p): %s\n",
                (void *)fn_tvm_init, (void *)fn_tvm_mount_cpio,
                (void *)fn_tvm_execvp, (void *)fn_tvm_chdir, dlerror());
        return -1;
    }

    // Frameworks dir = dirname(libtvm_path). The sidecar dylibs live there,
    // so it must be on PATH for the launchers' execvp to resolve them.
    char fwdir[PATH_MAX];
    strncpy(fwdir, libtvm_path, sizeof(fwdir) - 1);
    fwdir[sizeof(fwdir) - 1] = '\0';
    char *slash = strrchr(fwdir, '/');
    if (slash)
        *slash = '\0';

    // Home is the app sandbox's Documents dir (the writable area on iOS),
    // matching devtool. The sandbox root itself is not writable.
    const char *home_base = getenv("HOME");
    if (!home_base || !home_base[0])
        home_base = getenv("TMPDIR");
    if (!home_base || !home_base[0])
        home_base = "/tmp";
    snprintf(worker_home, sizeof(worker_home), "%s/Documents", home_base);
    worker_home[sizeof(worker_home) - 1] = '\0';
    if (mkdir(worker_home, 0777) != 0 && errno != EEXIST)
        tvm_log("tvm_start: mkdir(%s) failed: %s\n", worker_home,
                strerror(errno));

    char home_tmp[PATH_MAX];
    snprintf(home_tmp, sizeof(home_tmp), "%s/tmp", worker_home);
    if (mkdir(home_tmp, 0777) != 0 && errno != EEXIST)
        tvm_log("tvm_start: mkdir(%s) failed: %s\n", home_tmp, strerror(errno));

    // Environment for TVM. TVM_SHELL must be set before tvm_init because
    // usershell_cache_init() (in tvm_init) reads it before the initrc runs.
    if (password && setenv("DROPBEAR_PASSWORD", password, 1) != 0)
        tvm_log("tvm_start: setenv(DROPBEAR_PASSWORD) failed: %s\n",
                strerror(errno));
    if (setenv("HOME", worker_home, 1) != 0)
        tvm_log("tvm_start: setenv(HOME) failed: %s\n", strerror(errno));
    if (setenv("TMPDIR", home_tmp, 1) != 0)
        tvm_log("tvm_start: setenv(TMPDIR) failed: %s\n", strerror(errno));
    if (setenv("TVM_SHELL", "/tvm/bin/sh", 1) != 0)
        tvm_log("tvm_start: setenv(TVM_SHELL) failed: %s\n", strerror(errno));
    if (setenv("TVM_INITRC", initrc_path, 1) != 0)
        tvm_log("tvm_start: setenv(TVM_INITRC) failed: %s\n", strerror(errno));

    const char *old_path = getenv("PATH");
    if (!old_path)
        old_path = "";
    char new_path[PATH_MAX * 2];
    snprintf(new_path, sizeof(new_path), "%s:/tvm/bin:%s", fwdir, old_path);
    if (setenv("PATH", new_path, 1) != 0)
        tvm_log("tvm_start: setenv(PATH) failed: %s\n", strerror(errno));

    worker_port = port;
    strncpy(worker_initrd, initrd_path, sizeof(worker_initrd) - 1);
    worker_initrd[sizeof(worker_initrd) - 1] = '\0';

    int fds[2];
    if (pipe(fds) != 0) {
        tvm_log("tvm_start: pipe() failed: %s\n", strerror(errno));
        return -1;
    }
    pipe_read = fds[0];
    pipe_write = fds[1];

    pthread_t reader_thread;
    int rc = pthread_create(&reader_thread, NULL, reader_entry, NULL);
    if (rc != 0) {
        // Tear down the pipe first so tvm_log falls back to on_output (nothing
        // is reading the pipe).
        close(pipe_read);
        close(pipe_write);
        pipe_read = pipe_write = -1;
        tvm_log("tvm_start: pthread_create(reader) failed: %s\n", strerror(rc));
        return -1;
    }
    pthread_detach(reader_thread);

    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 64 * 1024 * 1024);
    pthread_t worker_thread;
    rc = pthread_create(&worker_thread, &attr, worker_entry, NULL);
    pthread_attr_destroy(&attr);
    if (rc != 0) {
        // The reader thread is up, so this reaches the console via the pipe.
        tvm_log("tvm_start: pthread_create(worker) failed: %s\n", strerror(rc));
        return -1;
    }
    pthread_detach(worker_thread);

    return 0;
}
