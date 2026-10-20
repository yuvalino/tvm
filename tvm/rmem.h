#pragma once
#include <stddef.h>

enum {
    RMEM_PRINT_CB,
    RMEM_OPS, // struct rmem_ops *
};

struct rmem_ops {
    void *(*ro_mmap)(void *, size_t, int, int, int, off_t);
    int   (*ro_munmap)(void *, size_t);
};

int rmem_setopt(int opt, void *val);

struct rmem_arena;

struct rmem_arena *rmem_arena_create(void *private);
void               rmem_arena_destroy(struct rmem_arena *ar);

void *rmem_arena_alloc(struct rmem_arena *ar, size_t size);
void  rmem_arena_free(struct rmem_arena *ar, void *ptr);
void *rmem_arena_realloc(struct rmem_arena *ar, void *ptr, size_t size);
void *rmem_arena_memalign(struct rmem_arena *ar, size_t alignment, size_t size);
// The usable size of the live object at `ptr` (its size class), or 0 when
// `ptr` is no object of the arena's
size_t rmem_arena_usable_size(struct rmem_arena *ar, const void *ptr);

int rmem_arena_duplicate(struct rmem_arena *dst_ar, struct rmem_arena *src_ar, void (*dup_cb)(void *, void *, void *, size_t), void (*done_cb)(void *), void *arg);
