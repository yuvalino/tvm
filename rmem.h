#pragma once
#include <stddef.h>

struct rmem_arena;

struct rmem_arena *rmem_arena_create(void *private);
void               rmem_arena_destroy(struct rmem_arena *ar);

void *rmem_arena_alloc(struct rmem_arena *ar, size_t size);
void  rmem_arena_free(struct rmem_arena *ar, void *ptr);
void *rmem_arena_realloc(struct rmem_arena *ar, void *ptr, size_t size);

struct rmem_arena *rmem_arena_duplicate(struct rmem_arena *ar, void *private, void (*dup_cb)(void *, void *, void *, size_t), void (*done_cb)(void *), void *arg);