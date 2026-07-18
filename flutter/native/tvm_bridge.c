#include "tvm_bridge.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <fcntl.h>
#include <errno.h>
#include <dlfcn.h>

typedef int (*tvm_main_fn)(int outfd, int port);
static tvm_main_fn fn_tvm_main;

static int pipe_read = -1;
static int pipe_write = -1;
static int running = 0;
static pthread_t worker_thread;

static void *worker_entry(void *arg) {
    int port = (int)(intptr_t)arg;
    fn_tvm_main(pipe_write, port);
    return NULL;
}

TVM_EXPORT int tvm_load(const char *libtvm_path) {
    if (fn_tvm_main) return 0;

    void *h = dlopen(libtvm_path, RTLD_NOW | RTLD_GLOBAL);
    if (!h) return -1;
    fn_tvm_main = (tvm_main_fn)dlsym(h, "tvm_main");

    return fn_tvm_main ? 0 : -1;
}

TVM_EXPORT int tvm_start(int port, const char *password) {
    if (running) return -1;
    if (!fn_tvm_main) return -1;

    if (password)
        setenv("DROPBEAR_PASSWORD", password, 1);

    int fds[2];
    if (pipe(fds) != 0) return -1;
    pipe_read = fds[0];
    pipe_write = fds[1];

    fcntl(pipe_read, F_SETFL, fcntl(pipe_read, F_GETFL, 0) | O_NONBLOCK);

    running = 1;

    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 64 * 1024 * 1024);
    if (pthread_create(&worker_thread, &attr, worker_entry, (void *)(intptr_t)port) != 0) {
        pthread_attr_destroy(&attr);
        running = 0;
        return -1;
    }
    pthread_attr_destroy(&attr);

    return 0;
}

TVM_EXPORT int tvm_poll(char *buf, int buf_size) {
    if (!running || pipe_read < 0 || buf_size <= 0) return 0;

    int n = read(pipe_read, buf, buf_size);
    if (n < 0 && errno == EAGAIN) return 0;
    return n > 0 ? n : 0;
}

TVM_EXPORT void tvm_stop(void) {
    if (!running) return;
    running = 0;

    if (pipe_write >= 0) { close(pipe_write); pipe_write = -1; }
    if (pipe_read >= 0) { close(pipe_read); pipe_read = -1; }

    pthread_join(worker_thread, NULL);
}
