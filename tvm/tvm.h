#pragma once

#include <stddef.h>

#define TVM_MOUNT_CPIO 0   // a cpio archive (data, size): the default
#define TVM_MOUNT_BIND 1   // a bind mount of `source` over `path`

struct tvm_mount {
    const char *path;   // absolute, normalized guest mount point, e.g. "/tvm"
    int         type;   // TVM_MOUNT_CPIO (the default) or TVM_MOUNT_BIND
    const void *data;   // cpio: archive bytes; consumed synchronously, caller may free after
    size_t      size;   // cpio: byte count
    const char *source; // bind: absolute guest path the mount point shows (mount --bind)
};

struct tvm_config {
    int                     log_fd;   // kernel log fd; -1 disables logging
    const char             *cwd;      // init task working directory; NULL = host cwd
    const char             *initrc;   // init script to run before exec; NULL = none
    const struct tvm_mount *mounts;   // archives and binds to mount before anything runs
    size_t                  nmounts;
};

/**
 * Boot the VM and exec `file` inside it, as the init task, on a thread of
 * its own.
 *
 * `file` is PATH-searched like execvp(), against the guest environment.
 * `envp` is the guest environment; NULL inherits the host environ. Runtime
 * knobs TVM reads about itself (TVM_NOEMU, TVM_SHELL, HOME, PATH, ...) come
 * from that environment.
 * `cfg` may be NULL: log to stdout, keep the host cwd, no initrc, mount
 * nothing. A relative `initrc` resolves against `cwd` (or the host cwd) and
 * may live inside one of the config mounts.
 *
 * Returns 0 once the init task is set up (the initrc and the exec follow on
 * its thread), or -1 with errno set for failures detected before any task
 * exists (bad argument, bad mount, bad cwd, missing initrc). The arguments
 * are copied: the caller may free them on return. The init task's exit()
 * ends the process. One call per process.
 *
 * Mount order: config mounts first, then the initrc; an initrc `mount` of the
 * same path shadows the config mount. A TVM_MOUNT_BIND lays `source` over
 * `path` the way the initrc's `mount --bind` does (INITRC.md).
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
