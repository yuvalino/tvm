#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <errno.h>
#include <pthread.h>



/* -------------------- Helpers -------------------- */


#define ABORT(MSG, ...) do { fprintf(stderr, "rmem: " MSG "\n", ##__VA_ARGS__); fflush(stderr); abort(); } while (0)


static inline size_t round_up(size_t x, size_t y) { return ((x+y-1)/y)*y; }

static size_t round_up_objsize(size_t size) {
    if (size <= 8) return 8;
    size_t n = 1;
    while (n < size) n <<= 1;
    return n;
}

size_t get_slab_size(size_t objsize) {
    size_t pagesz = (size_t)sysconf(_SC_PAGESIZE);

    /* target at least N chunks per slab */
    size_t min_nobj = 512;   // tweakable
    size_t slabsize = objsize * min_nobj;

    /* round up to multiple of page size for mmap */
    slabsize = round_up(slabsize, pagesz);

    return slabsize;
}

/* -------------------- BITMAP -------------------- */


#define BITMAP_WORD uint64_t
#define BITMAP_WORD_BITS (sizeof(BITMAP_WORD)*8)

static void bitmap_set_range(BITMAP_WORD *bitmap, size_t start, size_t len, int set)
{
    if (len == 0) return;

    size_t end = start + len;
    size_t wstart = start / BITMAP_WORD_BITS;
    size_t wend   = (end - 1) / BITMAP_WORD_BITS;

    for (size_t w = wstart; w <= wend; ++w) {
        unsigned s = (w == wstart) ? (start % BITMAP_WORD_BITS) : 0;
        unsigned e = (w == wend)   ? ((end - 1) % BITMAP_WORD_BITS)
                                   : (BITMAP_WORD_BITS - 1);

        BITMAP_WORD mask;
        if (s == 0 && e == BITMAP_WORD_BITS - 1)
            mask = ~0ULL;
        else
            mask = ((BITMAP_WORD)((1ULL << (e - s + 1)) - 1ULL) << s);

        if (set) bitmap[w] |= mask;
        else     bitmap[w] &= ~mask;
    }
}

static size_t bitmap_find_run_zero(
    BITMAP_WORD *bitmap,
    size_t words,
    size_t num_bits,
    size_t need
){
    if (need == 0) return SIZE_MAX;

    /* Quick path: find a single free bit if len == 1 */
    if (need == 1) {
        for (size_t w = 0; w < words; ++w) {
            BITMAP_WORD v = ~bitmap[w];
            if (v) {
                unsigned tz = __builtin_ctzll(v);
                size_t bit = w * BITMAP_WORD_BITS + tz;
                if (bit < num_bits) return bit;
            }
        }
        return SIZE_MAX;
    }

    size_t limit = num_bits - need + 1;
    size_t idx = 0;

    while (idx < limit) {
        size_t w = idx / BITMAP_WORD_BITS;
        unsigned off = idx % BITMAP_WORD_BITS;

        BITMAP_WORD word = bitmap[w];

        /* Mask out bits before offset */
        if (off != 0)
            word |= ((1ULL << off) - 1ULL);

        BITMAP_WORD inv = ~word;
        if (inv) {
            /* There is at least one zero bit ≥ off */
            unsigned tz = __builtin_ctzll(inv);
            size_t candidate = w * BITMAP_WORD_BITS + tz;

            if (candidate >= limit)
                break;

            /* Check run of `need` bits starting at candidate */
            size_t remaining = need;
            size_t cur = candidate;
            int ok = 1;

            while (remaining > 0) {
                size_t cw = cur / BITMAP_WORD_BITS;
                unsigned coff = cur % BITMAP_WORD_BITS;

                if (coff == 0 && remaining >= BITMAP_WORD_BITS) {
                    /* Check whole word */
                    if (bitmap[cw] != 0ULL) {
                        ok = 0;
                        break;
                    }
                    cur += BITMAP_WORD_BITS;
                    remaining -= BITMAP_WORD_BITS;
                } else {
                    unsigned take = BITMAP_WORD_BITS - coff;
                    if (take > remaining)
                        take = (unsigned)remaining;

                    BITMAP_WORD m =
                        (((take == 64) ? ~0ULL : ((1ULL << take) - 1ULL)) << coff);

                    if (bitmap[cw] & m) {
                        ok = 0;
                        break;
                    }
                    cur += take;
                    remaining -= take;
                }
            }

            if (ok)
                return candidate;

            idx = candidate + 1; /* continue searching */
        } else {
            /* No free bits in this whole word segment */
            idx = (w + 1) * BITMAP_WORD_BITS;
        }
    }

    return SIZE_MAX;
}

static int bitmap_test_range(BITMAP_WORD *bitmap, size_t start, size_t len) {
    if (len == 0) return 1;

    size_t end = start + len;
    size_t wstart = start / BITMAP_WORD_BITS;
    size_t wend   = (end - 1) / BITMAP_WORD_BITS;

    for (size_t w = wstart; w <= wend; ++w) {
        unsigned s = (w == wstart) ? (start % BITMAP_WORD_BITS) : 0;
        unsigned e = (w == wend)   ? ((end - 1) % BITMAP_WORD_BITS)
                                   : (BITMAP_WORD_BITS - 1);

        BITMAP_WORD mask;
        if (s == 0 && e == BITMAP_WORD_BITS - 1)
            mask = ~0ULL;
        else
            mask = ((1ULL << (e - s + 1)) - 1ULL) << s;

        if ((bitmap[w] & mask) != mask)
            return 0;
    }

    return 1;
}

/* -------------------- Slab -------------------- */

struct rmem_slab {
    struct rmem_slab *sl_next;

    void   *sl_base;
    size_t  sl_size;
    size_t  sl_objsize;
    size_t  sl_nobj;

    size_t       sl_bitmap_words;
    BITMAP_WORD *sl_bitmap;
};

static struct rmem_slab *rmem_slab_create(size_t slabsize, size_t objsize) {
    size_t pagesz = sysconf(_SC_PAGESIZE);
    slabsize = round_up(slabsize, pagesz);

    /* layout:
    [ header | bitmap | data ]
    */
    size_t nobj = (slabsize - sizeof(struct rmem_slab)) / objsize;
    size_t bitmap_words = 0; 
    while (nobj > 0) {
        bitmap_words = (nobj + BITMAP_WORD_BITS - 1) / BITMAP_WORD_BITS;
        if ((sizeof(struct rmem_slab) + bitmap_words * sizeof(BITMAP_WORD) + nobj * objsize) <= slabsize)
            break;
        nobj--;
    }

    if (!nobj)
        return NULL;

    void *mem = mmap(NULL, slabsize, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
    if (mem == MAP_FAILED)
        return NULL;


    struct rmem_slab *sl = (struct rmem_slab *)mem;
    sl->sl_next = NULL;

    sl->sl_base = (char*)mem + sizeof(struct rmem_slab) + bitmap_words * sizeof(BITMAP_WORD);
    sl->sl_size = slabsize;
    sl->sl_objsize = objsize;
    sl->sl_nobj = nobj;

    sl->sl_bitmap_words = bitmap_words;
    sl->sl_bitmap = (BITMAP_WORD*)((char*)mem + sizeof(struct rmem_slab));
    memset(sl->sl_bitmap, 0, bitmap_words * sizeof(BITMAP_WORD));
    return sl;
}

static void rmem_slab_destroy(struct rmem_slab *sl) {
    munmap(sl, sl->sl_size);
}

/* -------------------- Arena  -------------------- */

struct rmem_arena {
    void *ar_private;

    pthread_mutex_t    ar_lock;
    struct rmem_slab *ar_slabs;
};

struct rmem_arena *rmem_arena_create(void *private) {
    void *mem = mmap(NULL, sizeof(struct rmem_arena), PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
    if (mem == MAP_FAILED)
        return NULL;

    struct rmem_arena *ar = (struct rmem_arena *)mem;
    ar->ar_private = private;
    
    pthread_mutex_init(&ar->ar_lock, NULL);
    ar->ar_slabs = NULL;
    return ar;
}

void rmem_arena_destroy(struct rmem_arena *ar) {
    while (ar->ar_slabs) {
        struct rmem_slab *sl = ar->ar_slabs;
        ar->ar_slabs = sl->sl_next;
        rmem_slab_destroy(sl);
    }
    pthread_mutex_destroy(&ar->ar_lock);
    munmap(ar, sizeof(struct rmem_arena));
}

static void *rmem_arena_alloc_locked(struct rmem_arena *ar, size_t size) {
    if (!size)
        return NULL;

    size = round_up_objsize(size);

    size_t need = 1;

    for (struct rmem_slab *sl = ar->ar_slabs; sl; sl = sl->sl_next) {
        if (sl->sl_objsize != size)
            continue;

        size_t idx = bitmap_find_run_zero(sl->sl_bitmap, sl->sl_bitmap_words, sl->sl_nobj, need);
        if (idx != SIZE_MAX) {
            bitmap_set_range(sl->sl_bitmap, idx, need, 1);
            return (char*)sl->sl_base + idx * sl->sl_objsize;
        }
    }

    // need new slab
    struct rmem_slab *sl = rmem_slab_create(get_slab_size(size), size);
    if (!sl)
        return NULL;

    sl->sl_next = ar->ar_slabs;
    ar->ar_slabs = sl;

    bitmap_set_range(sl->sl_bitmap, 0, need, 1);
    return sl->sl_base;
}

void *rmem_arena_alloc(struct rmem_arena *ar, size_t size) {
    pthread_mutex_lock(&ar->ar_lock);
    void *p = rmem_arena_alloc_locked(ar, size);
    pthread_mutex_unlock(&ar->ar_lock);
    return p;
}

void rmem_arena_free(struct rmem_arena *ar, void *p) {
    if (!p) return;
    pthread_mutex_lock(&ar->ar_lock);
    for (struct rmem_slab *sl = ar->ar_slabs; sl; sl = sl->sl_next) {
        if ((uintptr_t)p < (uintptr_t)sl->sl_base || (uintptr_t)p >= ((uintptr_t)sl->sl_base + sl->sl_nobj * sl->sl_objsize))
            continue;

        uintptr_t off = (uintptr_t)p - (uintptr_t)sl->sl_base;
        if (off % sl->sl_objsize != 0)
            ABORT("p %p objsize 0x%zx remainder 0x%zx", p, sl->sl_objsize, off % sl->sl_objsize);

        size_t idx = off / sl->sl_objsize;
        if (!bitmap_test_range(sl->sl_bitmap, idx, 1))
            ABORT("detected double free on %p (objsize 0x%zx)", p, sl->sl_objsize);

        bitmap_set_range(sl->sl_bitmap, idx, 1, 0);
        pthread_mutex_unlock(&ar->ar_lock);
        return;
    }
    ABORT("invalid pointer %p", p);
}

void *rmem_arena_realloc(struct rmem_arena *ar, void *p, size_t size)
{
    if (!p)
        return rmem_arena_alloc(ar, size);

    if (size == 0) {
        rmem_arena_free(ar, p);
        return NULL;
    }

    size_t objsize = round_up_objsize(size);

    size_t idx;
    size_t old_objsize;
    struct rmem_slab *prev_sl = NULL;
    pthread_mutex_lock(&ar->ar_lock);
    for (struct rmem_slab *sl = ar->ar_slabs; sl; sl = sl->sl_next) {
        if ((uintptr_t)p < (uintptr_t)sl->sl_base || (uintptr_t)p >= ((uintptr_t)sl->sl_base + sl->sl_nobj * sl->sl_objsize))
            continue;

        uintptr_t off = (uintptr_t)p - (uintptr_t)sl->sl_base;
        if (off % sl->sl_objsize != 0)
            ABORT("p %p objsize 0x%zx remainder 0x%zx", p, sl->sl_objsize, off % sl->sl_objsize);

        idx = off / sl->sl_objsize;
        if (!bitmap_test_range(sl->sl_bitmap, idx, 1))
            ABORT("detected double free on %p (objsize 0x%zx)", p, sl->sl_objsize);

        // short circuit, keep in same slab
        if (objsize == sl->sl_objsize) {
            pthread_mutex_unlock(&ar->ar_lock);
            return p;
        }
        
        old_objsize = sl->sl_objsize;
        prev_sl = sl;
        break;
    }

    if (!prev_sl)
        ABORT("invalid pointer %p", p);

    void *newp = rmem_arena_alloc_locked(ar, size);
    if (!newp) {
        pthread_mutex_unlock(&ar->ar_lock);
        return NULL;
    }

    memcpy(newp, p, (old_objsize < size ? old_objsize : size));
    bitmap_set_range(prev_sl->sl_bitmap, idx, 1, 0);
    pthread_mutex_unlock(&ar->ar_lock);
    return newp;
}

struct rmem_arena *rmem_arena_duplicate(struct rmem_arena *ar, void *private, void (*dup_cb)(void *, void *, void *, size_t), void (*done_cb)(void *), void *arg)
{
    struct rmem_arena *ar2 = rmem_arena_create(private);
    if (!ar2)
        return NULL;

    pthread_mutex_lock(&ar->ar_lock);

    struct rmem_slab *prev_sl2 = NULL;
    for (struct rmem_slab *sl = ar->ar_slabs; sl; sl = sl->sl_next) {
        struct rmem_slab *sl2 = rmem_slab_create(sl->sl_size, sl->sl_objsize);
        if (!sl2) {
            rmem_arena_destroy(ar2);
            pthread_mutex_unlock(&ar->ar_lock);
            return NULL;
        }

        memcpy(sl2->sl_bitmap, sl->sl_bitmap, sl->sl_bitmap_words * sizeof(BITMAP_WORD));
        memcpy(sl2->sl_base, sl->sl_base, sl->sl_nobj * sl->sl_objsize);

        if (prev_sl2)
            prev_sl2->sl_next = sl2;
        else
            ar2->ar_slabs = sl2;
        prev_sl2 = sl2;

        if (dup_cb)
            dup_cb(arg, sl->sl_base, sl2->sl_base, sl->sl_nobj * sl->sl_objsize);
    }

    if (done_cb)
        done_cb(arg);

    pthread_mutex_unlock(&ar->ar_lock);
    return ar2;
}