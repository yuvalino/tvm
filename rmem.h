#pragma once
#include <stddef.h>

struct rmem_arena;

struct rmem_arena *rmem_arena_create(void *private);
void               rmem_arena_destroy(struct rmem_arena *ar);
struct rmem_arena *arena_duplicate(struct rmem_arena *ar, void **lookup_buf_addrs, size_t *lookup_buf_sizes, size_t lookup_buf_count);

void *rmem_arena_alloc(struct rmem_arena *ar, size_t size);
void  rmem_arena_free(struct rmem_arena *ar, void *ptr);
