#pragma once
#include <dlfcn.h>
#include <stddef.h>
#include <sys/types.h>

enum {
    ULINKER_OPS, // struct ulinker_ops *
    ULINKER_ADD_EMULATED_DLINFO,
    ULINKER_PRINT_CB,
    ULINKER_EVENT_CB,
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
};

struct ulinker_emulated_dlinfo_args {
    void *ue_private;
    const char *ue_name;
    void *(*ue_dlsym)(void *, const char *);
};

int ulinker_setopt(int opt, void *val);

struct ulinker *ulinker_create(void *private);
void ulinker_destroy(struct ulinker *ulinker);

void *ulinker_dlopen(struct ulinker *ulinker, const char *filename, int flags);
int   ulinker_dlclose(void *handle);
void *ulinker_dlsym(struct ulinker *ulinker, void *handle, const char *symbol);
char *ulinker_dlerror(void);

/**
 * opens an executable file, closing all previous open libraries.
 */
void *ulinker_dlopen_executable(struct ulinker *ulinker, const char *filename);
