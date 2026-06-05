#pragma once

void tvm_init_with_outfd(const char *init_comm, int outfd);
void tvm_init(const char *init_comm);

enum {
    TVMOPT_OUTFD, // int fd
    TVMOPT_SETFD, // int[2] = {inner_fd, outer_fd}
};
int tvm_setopt(int opt, void *val);

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

int tvm_mount_cpio(const char *pathname, const void *buf, unsigned long size);