#include "tvm_bindings.h"
#include "../../tvm/tvm.h"

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

extern char **environ;

// The single kernel entry point, resolved from libtvm.dylib at runtime. It
// boots and execs from inside libtvm text, so nothing in this file needs to
// call interposed libc functions (which dispatch on return address).
typedef int (*tvm_start_fn)(const char *file, char *const argv[],
                            char *const envp[], const struct tvm_config *cfg);

static tvm_start_fn fn_tvm_start;

static int pipe_read = -1;
static int pipe_write = -1;
static int worker_port = 0;
static char worker_initrd[PATH_MAX];
static char worker_home[PATH_MAX];
static char **worker_envp;
static tvm_output_cb worker_cb;

// Log a line to the console. Once the pipe exists it goes through the reader
// thread (→ Dart callback → stderr + UI). During the synchronous part of
// tvm_app_start (before the pipe/reader exist) it goes straight to the Dart
// callback, which owns and frees the malloc'd copy.
static void app_log(const char *fmt, ...)
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

// Build the guest environment: the host environ with `overrides` (KEY=VALUE,
// NULL-terminated) replacing or adding their keys. The host process env is
// left untouched.
static char **build_envp(const char *const *overrides)
{
    size_t nhost = 0, nover = 0;
    for (char **e = environ; e && *e; e++)
        nhost++;
    for (const char *const *o = overrides; *o; o++)
        nover++;

    char **out = calloc(nhost + nover + 1, sizeof(char *));
    if (!out)
        return NULL;

    size_t n = 0;
    for (char **e = environ; e && *e; e++) {
        const char *eq = strchr(*e, '=');
        size_t klen = eq ? (size_t)(eq - *e) : strlen(*e);
        int shadowed = 0;
        for (const char *const *o = overrides; *o; o++) {
            if (strncmp(*o, *e, klen) == 0 && (*o)[klen] == '=') {
                shadowed = 1;
                break;
            }
        }
        if (!shadowed)
            out[n++] = *e;
    }
    for (const char *const *o = overrides; *o; o++)
        out[n++] = (char *)*o;
    out[n] = NULL;
    return out;
}

// Worker thread: reads the initrd and hands everything to tvm_start, which
// boots the VM and execs dropbear on this thread and never returns.
//
// tvm_start only returns -1 before any task exists, so a plain return from
// this thread is safe on every failure path below.
static void *worker_entry(void *arg)
{
    (void)arg;

    int ifd = open(worker_initrd, O_RDONLY);
    if (ifd < 0) {
        app_log("tvm_app_start: open(%s) failed: %s\n", worker_initrd,
                strerror(errno));
        return NULL;
    }
    struct stat st;
    if (fstat(ifd, &st) != 0) {
        app_log("tvm_app_start: fstat(initrd) failed: %s\n", strerror(errno));
        close(ifd);
        return NULL;
    }
    void *initrd_data = malloc(st.st_size);
    if (!initrd_data) {
        app_log("tvm_app_start: malloc(%lld) for initrd failed\n",
                (long long)st.st_size);
        close(ifd);
        return NULL;
    }
    ssize_t rd = read(ifd, initrd_data, st.st_size);
    close(ifd);
    if (rd != (ssize_t)st.st_size) {
        app_log("tvm_app_start: short read of initrd (%zd/%lld): %s\n", rd,
                (long long)st.st_size, strerror(errno));
        free(initrd_data);
        return NULL;
    }

    struct tvm_mount initrd = {
        .path = "/tvm",
        .data = initrd_data,
        .size = (size_t)st.st_size,
    };
    struct tvm_config cfg = {
        .log_fd = pipe_write,
        .cwd = worker_home,
        .mounts = &initrd,
        .nmounts = 1,
    };

    char port_str[16];
    snprintf(port_str, sizeof(port_str), "%d", worker_port);
    char *argv[] = {"sshd", "-R", "-e", "-F", "-E", "-p", port_str, NULL};

    app_log("tvm_app_start: booting, exec dropbear\n");
    fn_tvm_start("dropbear", argv, worker_envp, &cfg);

    app_log("tvm_app_start: tvm_start failed: %s\n", strerror(errno));
    free(initrd_data);
    return NULL;
}

TVM_EXPORT int tvm_app_start(const char *libtvm_path, int port,
                             const char *password, const char *initrc_path,
                             const char *initrd_path, tvm_output_cb on_output)
{
    if (pipe_write >= 0) {
        app_log("tvm_app_start: already running\n");
        return -1;
    }

    // Enable logging for the synchronous phase below (goes to on_output until
    // the pipe/reader are up).
    worker_cb = on_output;

    // Resolve the TVM kernel entry point from libtvm.dylib.
    void *h = dlopen(libtvm_path, RTLD_NOW | RTLD_GLOBAL);
    if (!h) {
        app_log("tvm_app_start: dlopen(%s) failed: %s\n", libtvm_path, dlerror());
        return -1;
    }
    fn_tvm_start = (tvm_start_fn)dlsym(h, "tvm_start");
    if (!fn_tvm_start) {
        app_log("tvm_app_start: dlsym(tvm_start) failed: %s\n", dlerror());
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
        app_log("tvm_app_start: mkdir(%s) failed: %s\n", worker_home,
                strerror(errno));

    char home_tmp[PATH_MAX];
    snprintf(home_tmp, sizeof(home_tmp), "%s/tmp", worker_home);
    if (mkdir(home_tmp, 0777) != 0 && errno != EEXIST)
        app_log("tvm_app_start: mkdir(%s) failed: %s\n", home_tmp, strerror(errno));

    // Guest environment. TVM_SHELL comes from init.flutter.rc (the usershell
    // cache is populated after the initrc runs).
    const char *old_path = getenv("PATH");
    if (!old_path)
        old_path = "";

    static char env_home[PATH_MAX + 8];
    static char env_tmpdir[PATH_MAX + 8];
    static char env_initrc[PATH_MAX + 16];
    static char env_password[256];
    static char env_path[PATH_MAX * 2 + 8];
    snprintf(env_home, sizeof(env_home), "HOME=%s", worker_home);
    snprintf(env_tmpdir, sizeof(env_tmpdir), "TMPDIR=%s", home_tmp);
    snprintf(env_initrc, sizeof(env_initrc), "TVM_INITRC=%s", initrc_path);
    snprintf(env_path, sizeof(env_path), "PATH=%s:/tvm/bin:%s", fwdir, old_path);

    const char *overrides[6];
    int no = 0;
    overrides[no++] = env_home;
    overrides[no++] = env_tmpdir;
    overrides[no++] = env_initrc;
    overrides[no++] = env_path;
    if (password) {
        snprintf(env_password, sizeof(env_password), "DROPBEAR_PASSWORD=%s", password);
        overrides[no++] = env_password;
    }
    overrides[no] = NULL;

    worker_envp = build_envp(overrides);
    if (!worker_envp) {
        app_log("tvm_app_start: building environment failed\n");
        return -1;
    }

    worker_port = port;
    strncpy(worker_initrd, initrd_path, sizeof(worker_initrd) - 1);
    worker_initrd[sizeof(worker_initrd) - 1] = '\0';

    int fds[2];
    if (pipe(fds) != 0) {
        app_log("tvm_app_start: pipe() failed: %s\n", strerror(errno));
        return -1;
    }
    pipe_read = fds[0];
    pipe_write = fds[1];

    pthread_t reader_thread;
    int rc = pthread_create(&reader_thread, NULL, reader_entry, NULL);
    if (rc != 0) {
        // Tear down the pipe first so app_log falls back to on_output (nothing
        // is reading the pipe).
        close(pipe_read);
        close(pipe_write);
        pipe_read = pipe_write = -1;
        app_log("tvm_app_start: pthread_create(reader) failed: %s\n", strerror(rc));
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
        app_log("tvm_app_start: pthread_create(worker) failed: %s\n", strerror(rc));
        return -1;
    }
    pthread_detach(worker_thread);

    return 0;
}
