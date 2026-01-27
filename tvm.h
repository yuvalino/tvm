#pragma once

#define __TVM_STRINGIFY(X) #X
#define _TVM_STRINGIFY(X) __TVM_STRINGIFY(X)

#define _COW_REG(Name, InitFunc, DeepCopy) \
    static void *_tvm_rcg_ ## Name () { return (& Name); } \
    __attribute__((constructor)) \
    static void _tvm_rcc_ ## Name () { _tvm_register_cow(_tvm_rcg_ ## Name, sizeof(Name), InitFunc, DeepCopy, (__FILE__ ":" _TVM_STRINGIFY(__LINE__) " " #Name)); }
#define _COW_REG_INIT(Name, InitExpr, DeepCopy) \
    static void _tvm_rci_ ## Name () { Name = (InitExpr); } \
    _COW_REG(Name, _tvm_rci_ ## Name, DeepCopy)

#define COW_IMPL_INIT(Type, Name, InitExpr) __thread typeof(Type) Name; _COW_REG_INIT(Name, InitExpr, 1)
#define COW_IMPL(Type, Name) __thread typeof(Type) Name; _COW_REG(Name, NULL, 1)

#define COW_DECL(TypeName) extern __thread TypeName
    
void _tvm_register_cow(void *(*getptr_fn)(void), unsigned size, void (*init_fn)(void), int deepcopy, char *name);

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

extern __thread char *tvm_optarg;
extern __thread int tvm_optind;
extern __thread int tvm_opterr;
extern __thread int tvm_optopt;

void tvm_log(const char *function, const char *msg, ...);
#define TVM_INFO(_msg, ...) tvm_log(__func__, _msg, ##__VA_ARGS__)

enum {
    TVM_DUMP_FUNCS
};
void tvm_dump(int mode);

int tvm_mount_cpio(const char *pathname, const void *buf, unsigned long size);