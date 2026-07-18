#include "tvm_bindings.h"

#include <dlfcn.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

// Resolved from libtvm.dylib at runtime. execvp and chdir must come from
// libtvm (TVM's interposed versions) so the worker enters the VM instead of
// the host's libc — this file is in the app binary, where a bare execvp()
// would bind to libc and try to exec a real process.
typedef void (*tvm_init_with_outfd_fn)(const char *init_comm, int outfd);
typedef int (*tvm_mount_cpio_fn)(const char *pathname, const void *buf,
                                 unsigned long size);
typedef int (*execvp_fn)(const char *file, char *const argv[]);
typedef int (*chdir_fn)(const char *path);

static tvm_init_with_outfd_fn fn_tvm_init;
static tvm_mount_cpio_fn fn_tvm_mount_cpio;
static execvp_fn fn_tvm_execvp;
static chdir_fn fn_tvm_chdir;

static int pipe_read = -1;
static int pipe_write = -1;
static int worker_port = 0;
static char worker_initrd[PATH_MAX];
static char worker_home[PATH_MAX];
static tvm_output_cb worker_cb;

// Write a status line to the pipe so it reaches both Dart and stderr
// (the reader thread forwards everything it reads).
static void logline(const char *msg)
{
    write(pipe_write, msg, strlen(msg));
}

// Reader thread: blocks on the pipe, forwards each chunk to stderr (Xcode
// console) and to the Dart callback. The callback owns the malloc'd copy.
static void *reader_entry(void *arg)
{
    (void)arg;
    char tmp[4096];
    for (;;) {
        ssize_t n = read(pipe_read, tmp, sizeof(tmp));
        if (n <= 0)
            break;
        write(STDERR_FILENO, tmp, n);
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
static void *worker_entry(void *arg)
{
    (void)arg;

    logline("tvm_start: init\n");
    fn_tvm_init("tvm", pipe_write);

    int ifd = open(worker_initrd, O_RDONLY);
    if (ifd < 0) {
        logline("tvm_start: cannot open initrd\n");
        return NULL;
    }
    struct stat st;
    if (fstat(ifd, &st) != 0) {
        close(ifd);
        return NULL;
    }
    void *initrd_data = malloc(st.st_size);
    if (!initrd_data) {
        close(ifd);
        return NULL;
    }
    read(ifd, initrd_data, st.st_size);
    close(ifd);
    fn_tvm_mount_cpio("/tvm", initrd_data, st.st_size);
    free(initrd_data);

    fn_tvm_chdir(worker_home);

    logline("tvm_start: exec dropbear\n");
    char port_str[16];
    sprintf(port_str, "%d", worker_port);
    char *argv[] = {"sshd", "-R", "-e", "-F", "-E", "-p", port_str, NULL};
    fn_tvm_execvp("dropbear", argv);

    logline("tvm_start: execvp failed\n");
    return NULL;
}

TVM_EXPORT int tvm_start(const char *libtvm_path, int port,
                         const char *password, const char *initrc_path,
                         const char *initrd_path, tvm_output_cb on_output)
{
    if (pipe_write >= 0)
        return -1; // already started

    // Resolve the TVM kernel entry points from libtvm.dylib.
    void *h = dlopen(libtvm_path, RTLD_NOW | RTLD_GLOBAL);
    if (!h)
        return -1;
    fn_tvm_init = (tvm_init_with_outfd_fn)dlsym(h, "tvm_init_with_outfd");
    fn_tvm_mount_cpio = (tvm_mount_cpio_fn)dlsym(h, "tvm_mount_cpio");
    fn_tvm_execvp = (execvp_fn)dlsym(h, "execvp");
    fn_tvm_chdir = (chdir_fn)dlsym(h, "chdir");
    if (!fn_tvm_init || !fn_tvm_mount_cpio || !fn_tvm_execvp || !fn_tvm_chdir)
        return -1;

    // Frameworks dir = dirname(libtvm_path). The sidecar dylibs live there,
    // so it must be on PATH for the launchers' execvp to resolve them.
    char fwdir[PATH_MAX];
    strncpy(fwdir, libtvm_path, sizeof(fwdir) - 1);
    fwdir[sizeof(fwdir) - 1] = '\0';
    char *slash = strrchr(fwdir, '/');
    if (slash)
        *slash = '\0';

    const char *home = getenv("HOME");
    if (!home || !home[0])
        home = getenv("TMPDIR");
    if (!home || !home[0])
        home = "/tmp";
    strncpy(worker_home, home, sizeof(worker_home) - 1);
    worker_home[sizeof(worker_home) - 1] = '\0';

    char home_tmp[PATH_MAX];
    snprintf(home_tmp, sizeof(home_tmp), "%s/tmp", worker_home);
    mkdir(home_tmp, 0777);

    // Environment for TVM. TVM_SHELL must be set before tvm_init because
    // usershell_cache_init() (in tvm_init) reads it before the initrc runs.
    if (password)
        setenv("DROPBEAR_PASSWORD", password, 1);
    setenv("HOME", worker_home, 1);
    setenv("TMPDIR", home_tmp, 1);
    setenv("TVM_SHELL", "/tvm/bin/sh", 1);
    setenv("TVM_INITRC", initrc_path, 1);

    const char *old_path = getenv("PATH");
    if (!old_path)
        old_path = "";
    char new_path[PATH_MAX * 2];
    snprintf(new_path, sizeof(new_path), "%s:/tvm/bin:%s", fwdir, old_path);
    setenv("PATH", new_path, 1);

    worker_port = port;
    worker_cb = on_output;
    strncpy(worker_initrd, initrd_path, sizeof(worker_initrd) - 1);
    worker_initrd[sizeof(worker_initrd) - 1] = '\0';

    int fds[2];
    if (pipe(fds) != 0)
        return -1;
    pipe_read = fds[0];
    pipe_write = fds[1];

    pthread_t reader_thread;
    if (pthread_create(&reader_thread, NULL, reader_entry, NULL) != 0) {
        close(pipe_read);
        close(pipe_write);
        pipe_read = pipe_write = -1;
        return -1;
    }
    pthread_detach(reader_thread);

    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 64 * 1024 * 1024);
    pthread_t worker_thread;
    if (pthread_create(&worker_thread, &attr, worker_entry, NULL) != 0) {
        pthread_attr_destroy(&attr);
        return -1;
    }
    pthread_attr_destroy(&attr);
    pthread_detach(worker_thread);

    return 0;
}
