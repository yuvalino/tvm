#include "tvm_bridge.h"
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#include <io.h>
#include <fcntl.h>
#else
#include <unistd.h>
#include <pthread.h>
#include <fcntl.h>
#include <errno.h>
#endif

int tvm_main(int outfd, int port) __attribute__((weak));
int tvm_main(int outfd, int port) { return -1; }

static int pipe_read = -1;
static int pipe_write = -1;
static int running = 0;

#ifdef _WIN32
static HANDLE worker_handle = NULL;

static DWORD WINAPI worker_entry(LPVOID arg) {
    int port = (int)(intptr_t)arg;
    tvm_main(pipe_write, port);
    return 0;
}
#else
static pthread_t worker_thread;

static void *worker_entry(void *arg) {
    int port = (int)(intptr_t)arg;
    tvm_main(pipe_write, port);
    return NULL;
}
#endif

TVM_EXPORT int tvm_start(int port, const char *password) {
    if (running) return -1;

    if (password) {
#ifdef _WIN32
        SetEnvironmentVariableA("DROPBEAR_PASSWORD", password);
#else
        setenv("DROPBEAR_PASSWORD", password, 1);
#endif
    }

    int fds[2];
#ifdef _WIN32
    if (_pipe(fds, 65536, _O_BINARY) != 0) return -1;
#else
    if (pipe(fds) != 0) return -1;
#endif
    pipe_read = fds[0];
    pipe_write = fds[1];

#ifndef _WIN32
    int flags = fcntl(pipe_read, F_GETFL, 0);
    fcntl(pipe_read, F_SETFL, flags | O_NONBLOCK);
#endif

    running = 1;

#ifdef _WIN32
    worker_handle = CreateThread(NULL, 0, worker_entry, (LPVOID)(intptr_t)port, 0, NULL);
    if (!worker_handle) { running = 0; return -1; }
#else
    if (pthread_create(&worker_thread, NULL, worker_entry, (void *)(intptr_t)port) != 0) {
        running = 0;
        return -1;
    }
#endif

    return 0;
}

TVM_EXPORT int tvm_poll(char *buf, int buf_size) {
    if (!running || pipe_read < 0 || buf_size <= 0) return 0;

#ifdef _WIN32
    DWORD available = 0;
    HANDLE h = (HANDLE)_get_osfhandle(pipe_read);
    if (!PeekNamedPipe(h, NULL, 0, NULL, &available, NULL) || available == 0)
        return 0;
    int to_read = available < (DWORD)buf_size ? (int)available : buf_size;
    int n = _read(pipe_read, buf, to_read);
    return n > 0 ? n : 0;
#else
    int n = read(pipe_read, buf, buf_size);
    if (n < 0 && errno == EAGAIN) return 0;
    return n > 0 ? n : 0;
#endif
}

TVM_EXPORT void tvm_stop(void) {
    if (!running) return;
    running = 0;

    if (pipe_write >= 0) {
#ifdef _WIN32
        _close(pipe_write);
#else
        close(pipe_write);
#endif
        pipe_write = -1;
    }
    if (pipe_read >= 0) {
#ifdef _WIN32
        _close(pipe_read);
#else
        close(pipe_read);
#endif
        pipe_read = -1;
    }

#ifdef _WIN32
    if (worker_handle) {
        WaitForSingleObject(worker_handle, 5000);
        CloseHandle(worker_handle);
        worker_handle = NULL;
    }
#else
    pthread_join(worker_thread, NULL);
#endif
}
