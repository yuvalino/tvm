#pragma once
#include <dlfcn.h>
#include <stddef.h>
#include <sys/types.h>

#include <unicorn/unicorn.h>

enum {
    ULINKER_OPS, // struct ulinker_ops *
    ULINKER_ADD_EXTERNAL_DLINFO,
    ULINKER_PRINT_CB,
    ULINKER_EVENT_CB,
    ULINKER_NOEMU, // deprecated — use RTLD_NOEMU per-dlopen instead
};

enum {
    ULEVENT_LOAD,   // struct ulinker_load_event *
    ULEVENT_UNLOAD, // struct ulinker_load_event *
};

struct ulinker_load_event {
    void  *ule_private;
    void  *ule_base;
    size_t ule_size;
};

#ifndef RTLD_NEXT
# define RTLD_NEXT	((void *) -1l)
#endif
#ifndef RTLD_DEFAULT
# define RTLD_DEFAULT	((void *) 0)
#endif

#define RTLD_NOEMU 0x40000000  // allow PROT_EXEC mappings

struct ulinker_ops {
    void *(*uo_malloc)(void *, size_t);
    void  (*uo_free)(void *, void *);
    void *(*uo_realloc)(void *, void *, size_t);

    int     (*uo_open)(void *, const char *);
    int     (*uo_close)(void *, int);
    ssize_t (*uo_read)(void *, int, void *, size_t);
    int     (*uo_fstat)(void *, int, struct stat *);

    void *(*uo_mmap)(void *, void *, size_t, int, int, int, off_t);
    int   (*uo_munmap)(void *, void *, size_t);
    int   (*uo_mprotect)(void *, void *, size_t, int);

    void *(*uo_call)(void *, void *);
};

struct ulinker_external_dlinfo_args {
    void *ue_private;
    const char *ue_name;
    void *(*ue_dlsym)(void *, const char *, int emu);
};

int ulinker_setopt(int opt, void *val);

struct ulinker *ulinker_create(void *private);
void ulinker_destroy(struct ulinker *ulinker);

void *ulinker_dlopen(struct ulinker *ulinker, const char *filename, int flags);
int   ulinker_dlclose(void *handle);
void *ulinker_dlsym(struct ulinker *ulinker, void *handle, const char *symbol, int all);
char *ulinker_dlerror(void);

enum {
    ULARCH_NONE = 0,
    ULARCH_X86_64,
    ULARCH_ARM64,
};

enum {
    ULABI_NONE = 0,
    ULABI_LINUX,
    ULABI_DARWIN
};

struct ulinker_dladdr_info {
    const char *udli_fname;
    void       *udli_fbase;
    size_t      udli_fsize;
    int         udli_farch;
    int         udli_fabi;
    int         udli_femu;  // != 0 means no PROT_EXEC mappings, made to run under emulator
    const char *udli_sname;
    void       *udli_saddr;
};
int ulinker_dladdr(struct ulinker *ulinker, const void *addr, struct ulinker_dladdr_info *info);
int ulinker_dladdr_safe(struct ulinker *ulinker, const void *addr, struct ulinker_dladdr_info *info);

/**
 * duplicate a linker with all loaded libraries and data.
 * `segment_cb` is called with each writable segment.
 */
int ulinker_duplicate(struct ulinker *old, struct ulinker *new, void (*segment_cb)(void *, void *, void *, size_t, int), void *arg);