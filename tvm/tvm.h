#pragma once

#include <stddef.h>

struct tvm_mount {
    const char *path;   // absolute, normalized guest mount point, e.g. "/tvm"
    const void *data;   // cpio archive bytes; consumed synchronously, caller may free after
    size_t      size;
};

struct tvm_config {
    int                     log_fd;   // kernel log destination; 0 = stdout
    const char             *cwd;      // init task working directory; NULL = host cwd
    const struct tvm_mount *mounts;   // cpio archives to mount before anything runs
    size_t                  nmounts;
};

/**
 * Boot the VM on the calling thread and exec `file` inside it.
 *
 * `file` is PATH-searched like execvp(), against the guest environment.
 * `envp` is the guest environment; NULL inherits the host environ. Everything
 * TVM configures itself from (TVM_INITRC, TVM_NOINITRC, TVM_NOEMU, TVM_SHELL,
 * HOME, PATH, ...) is read from that environment.
 * `cfg` may be NULL: log to stdout, keep the host cwd, mount nothing.
 *
 * Never returns on success: the calling thread becomes the init task's thread
 * and ends in the in-VM exit(). Returns -1 with errno set only for failures
 * detected before any task exists (bad argument, bad mount, bad cwd), in
 * which case the calling thread may return normally. One call per process.
 *
 * Mount order: config mounts first, then the initrc; an initrc `mount` of the
 * same path shadows the config mount.
 */
int tvm_start(const char *file, char *const argv[], char *const envp[],
              const struct tvm_config *cfg);

#define tvm_environ (*(_tvm_environ()))
char ***_tvm_environ(void);

typedef int (*main_func_t)(int, char * const*, char * const*);
void tvm_register_program(const char *pathname, main_func_t main_routine);

void tvm_log(const char *function, const char *msg, ...);
#define TVM_INFO(_msg, ...) tvm_log(__func__, _msg, ##__VA_ARGS__)

enum {
    TVM_DUMP_FUNCS
};
void tvm_dump(int mode);
