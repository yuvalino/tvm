#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <inttypes.h>
#include <sys/time.h>
#include <sys/mman.h>
#include <unistd.h>
#include <errno.h>
#include <pthread.h>

#include "rmem.h"



/* -------------------- Helpers -------------------- */

static __thread char msgbuf[4096];
static void (*print_cb)(char *, int) = NULL;


#define PRINT0(_msg, _len) do { \
    if (print_cb) { print_cb(_msg, _len); } \
    else { write(1, _msg, _len); } \
} while (0)


#define PRINT(MSG, ...) do { \
    int __len = snprintf(msgbuf, sizeof(msgbuf), "%s:%d %s: " MSG "\n", __FILE__, __LINE__, __func__, ##__VA_ARGS__); \
    PRINT0(msgbuf, __len); \
} while (0)


#define ABORT(MSG, ...) do { \
    int __len = snprintf(msgbuf, sizeof(msgbuf), "ABORT! %s:%d %s: " MSG "\n", __FILE__, __LINE__, __func__, ##__VA_ARGS__); \
    PRINT0(msgbuf, __len); \
    abort(); \
} while (0)


#define TIME_MS() ({ \
    struct timeval __tv; \
    gettimeofday(&__tv, NULL); \
    (uint64_t)__tv.tv_sec * 1000 + (uint64_t)__tv.tv_usec / 1000; \
})

struct rmem_ops rops = {
    .ro_mmap = mmap,
    .ro_munmap = munmap,
};

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

static size_t bitmap_find_run(
    BITMAP_WORD *bitmap,
    size_t words,
    size_t num_bits,
    size_t need,
    int want_one   /* 0 = find zeros, 1 = find ones */
){
    if (need == 0)
        return SIZE_MAX;

    /* Quick path: find a single bit */
    if (need == 1) {
        for (size_t w = 0; w < words; ++w) {
            BITMAP_WORD v = want_one ? bitmap[w] : ~bitmap[w];
            if (v) {
                unsigned tz = __builtin_ctzll(v);
                size_t bit = w * BITMAP_WORD_BITS + tz;
                if (bit < num_bits)
                    return bit;
            }
        }
        return SIZE_MAX;
    }

    size_t limit = num_bits - need + 1;
    size_t idx = 0;

    while (idx < limit) {
        size_t w = idx / BITMAP_WORD_BITS;
        unsigned off = idx % BITMAP_WORD_BITS;

        BITMAP_WORD word = want_one ? bitmap[w] : ~bitmap[w];

        /* Mask out bits before offset */
        if (off != 0)
            word &= ~((1ULL << off) - 1ULL);

        if (word) {
            unsigned tz = __builtin_ctzll(word);
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

                BITMAP_WORD wv = want_one ? bitmap[cw] : ~bitmap[cw];

                if (coff == 0 && remaining >= BITMAP_WORD_BITS) {
                    /* Check whole word */
                    if (wv != ~0ULL) {
                        ok = 0;
                        break;
                    }
                    cur += BITMAP_WORD_BITS;
                    remaining -= BITMAP_WORD_BITS;
                } else {
                    unsigned take = BITMAP_WORD_BITS - coff;
                    if (take > remaining)
                        take = (unsigned)remaining;

                    BITMAP_WORD mask =
                        ((take == 64 ? ~0ULL : ((1ULL << take) - 1ULL)) << coff);

                    if ((wv & mask) != mask) {
                        ok = 0;
                        break;
                    }
                    cur += take;
                    remaining -= take;
                }
            }

            if (ok)
                return candidate;

            idx = candidate + 1;
        } else {
            /* No matching bits in this word */
            idx = (w + 1) * BITMAP_WORD_BITS;
        }
    }

    return SIZE_MAX;
}


static size_t bitmap_find_run_bwd(
    BITMAP_WORD *bitmap,
    size_t words,
    size_t num_bits,
    size_t need,
    int want_one
){
    if (need == 0)
        return SIZE_MAX;

    /* Fast path: single bit */
    if (need == 1) {
        for (size_t w = words; w-- > 0; ) {
            BITMAP_WORD v = want_one ? bitmap[w] : ~bitmap[w];
            if (v) {
                unsigned b = 63 - __builtin_clzll(v);
                size_t bit = w * BITMAP_WORD_BITS + b;
                if (bit < num_bits)
                    return bit;
            }
        }
        return SIZE_MAX;
    }

    ssize_t idx = (ssize_t)num_bits - (ssize_t)need;

    while (idx >= 0) {
        size_t w = (size_t)idx / BITMAP_WORD_BITS;
        unsigned off = (unsigned)idx % BITMAP_WORD_BITS;

        BITMAP_WORD word = want_one ? bitmap[w] : ~bitmap[w];

        /* Mask out bits above idx */
        BITMAP_WORD mask =
            off == BITMAP_WORD_BITS - 1 ?
            ~0ULL : ((1ULL << (off + 1)) - 1ULL);

        word &= mask;

        if (word) {
            unsigned b = 63 - __builtin_clzll(word);
            ssize_t candidate =
                (ssize_t)(w * BITMAP_WORD_BITS + b) - (ssize_t)(need - 1);

            if (candidate < 0)
                goto next;

            /* Verify run */
            size_t cur = (size_t)candidate;
            size_t remaining = need;
            int ok = 1;

            while (remaining) {
                size_t cw = cur / BITMAP_WORD_BITS;
                unsigned coff = cur % BITMAP_WORD_BITS;

                unsigned take = BITMAP_WORD_BITS - coff;
                if (take > remaining)
                    take = remaining;

                BITMAP_WORD m =
                    ((take == 64 ? ~0ULL : ((1ULL << take) - 1ULL)) << coff);

                BITMAP_WORD wv = want_one ? bitmap[cw] : ~bitmap[cw];
                if ((wv & m) != m) {
                    ok = 0;
                    break;
                }

                cur += take;
                remaining -= take;
            }

            if (ok)
                return (size_t)candidate;
        }

    next:
        idx = (ssize_t)(w * BITMAP_WORD_BITS) - 1;
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

static inline size_t bitmap_find_run_zero(
    BITMAP_WORD *bitmap,
    size_t words,
    size_t num_bits,
    size_t need
){
    return bitmap_find_run(bitmap, words, num_bits, need, 0);
}

static inline size_t bitmap_find_run_one(
    BITMAP_WORD *bitmap,
    size_t words,
    size_t num_bits,
    size_t need
){
    return bitmap_find_run(bitmap, words, num_bits, need, 1);
}

static inline size_t bitmap_find_run_one_bwd(
    BITMAP_WORD *bitmap,
    size_t words,
    size_t num_bits,
    size_t need
) {
    return bitmap_find_run_bwd(bitmap, words, num_bits, need, 1);
}

static inline size_t bitmap_find_run_zero_bwd(
    BITMAP_WORD *bitmap,
    size_t words,
    size_t num_bits,
    size_t need
) {
    return bitmap_find_run_bwd(bitmap, words, num_bits, need, 0);
}

/* -------------------- Slab -------------------- */

struct rmem_slab {
    struct rmem_slab *sl_next;

    void   *sl_base;
    size_t  sl_size;
    size_t  sl_objsize;
    size_t  sl_nobj;

    void *sl_low;
    void *sl_high;

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

    void *mem = rops.ro_mmap(NULL, slabsize, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
    if (mem == MAP_FAILED)
        return NULL;


    struct rmem_slab *sl = (struct rmem_slab *)mem;
    sl->sl_next = NULL;

    sl->sl_base = (char*)mem + sizeof(struct rmem_slab) + bitmap_words * sizeof(BITMAP_WORD);
    sl->sl_size = slabsize;
    sl->sl_objsize = objsize;
    sl->sl_nobj = nobj;
    sl->sl_high = sl->sl_low = NULL;

    sl->sl_bitmap_words = bitmap_words;
    sl->sl_bitmap = (BITMAP_WORD*)((char*)mem + sizeof(struct rmem_slab));
    memset(sl->sl_bitmap, 0, bitmap_words * sizeof(BITMAP_WORD));
    return sl;
}

static void rmem_slab_destroy(struct rmem_slab *sl) {
    rops.ro_munmap(sl, sl->sl_size);
}

static void rmem_slab_free(struct rmem_slab *sl, void *p)
{
    uintptr_t addr = (uintptr_t)p;

    if (addr < (uintptr_t)sl->sl_base ||
        addr >= (uintptr_t)sl->sl_base + sl->sl_nobj * sl->sl_objsize)
        ABORT("invalid pointer %p", p);

    uintptr_t off = addr - (uintptr_t)sl->sl_base;
    if (off % sl->sl_objsize != 0)
        ABORT("p %p objsize 0x%zx remainder 0x%zx",
              p, sl->sl_objsize, off % sl->sl_objsize);

    size_t idx = off / sl->sl_objsize;

    if (!bitmap_test_range(sl->sl_bitmap, idx, 1))
        ABORT("detected double free on %p (objsize 0x%zx)",
              p, sl->sl_objsize);

    bitmap_set_range(sl->sl_bitmap, idx, 1, 0);
    memset(p, 0, sl->sl_objsize);

    uintptr_t obj_low  = addr;
    uintptr_t obj_high = addr + sl->sl_objsize;

    // fast path - only object allocated is this object.
    if (obj_low == (uintptr_t)sl->sl_low && obj_high == (uintptr_t)sl->sl_high) {
        sl->sl_low = sl->sl_high = NULL;
        return;
    }
    
    // TODO: optimize to unly run on the relevant bitmap part rather than everything

    if (obj_low == (uintptr_t)sl->sl_low) {
        size_t first = bitmap_find_run_one(
            sl->sl_bitmap,
            sl->sl_bitmap_words,
            sl->sl_nobj,
            1
        );

        if (first == SIZE_MAX) {
            sl->sl_low = sl->sl_high = NULL;
            return;
        }

        sl->sl_low = (char *)sl->sl_base + first * sl->sl_objsize;
    }

    if (obj_high == (uintptr_t)sl->sl_high) {
        size_t last = bitmap_find_run_one_bwd(
            sl->sl_bitmap,
            sl->sl_bitmap_words,
            sl->sl_nobj,
            1
        );

        sl->sl_high = (char *)sl->sl_base + (last + 1) * sl->sl_objsize;
    }
}

/* -------------------- Options  -------------------- */

int rmem_setopt(int opt, void *val)
{
    if (opt == RMEM_PRINT_CB) {
        print_cb = (void (*)(char *, int))val;
        return 0;
    }

    if (opt == RMEM_OPS) {
        memcpy(&rops, val, sizeof(rops));
        return 0;
    }

    // unknown opt?
    return 1;
}

/* -------------------- Arena  -------------------- */

struct rmem_arena {
    void *ar_private;

    pthread_mutex_t    ar_lock;
    struct rmem_slab *ar_slabs;
};

struct rmem_arena *rmem_arena_create(void *private) {
    void *mem = rops.ro_mmap(NULL, sizeof(struct rmem_arena), PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
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
    rops.ro_munmap(ar, sizeof(struct rmem_arena));
}

static void *rmem_arena_alloc_locked(struct rmem_arena *ar, size_t size) {
    if (!size)
        return NULL;

    size = round_up_objsize(size);

    size_t need = 1;

    struct rmem_slab *sl;
    size_t idx;
    void *p = NULL;
    for (sl = ar->ar_slabs; sl; sl = sl->sl_next) {
        if (sl->sl_objsize != size)
            continue;

        idx = bitmap_find_run_zero(sl->sl_bitmap, sl->sl_bitmap_words, sl->sl_nobj, need);
        if (idx != SIZE_MAX) {
            p = (char*)sl->sl_base + idx * sl->sl_objsize;
            break;
        }
    }

    // need new slab
    if (!p) {
        sl = rmem_slab_create(get_slab_size(size), size);
        if (!sl)
            return NULL;
        sl->sl_next = ar->ar_slabs;
        ar->ar_slabs = sl;

        // choose first object
        idx = 0;
        p = sl->sl_base;
    }

    // `p` contains a valid pointer, `idx` a valid range and `sl` a valid slab
    bitmap_set_range(sl->sl_bitmap, idx, need, 1);

    if (!sl->sl_low || (char *)p < (char *)sl->sl_low)
        sl->sl_low = p;
    if (!sl->sl_high || (char *)p >= (char *)sl->sl_high)
        sl->sl_high = (char *)p + sl->sl_objsize;

    return p;
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

        rmem_slab_free(sl, p);
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
            if (objsize > size)
                memset((char *)p + size, 0, sl->sl_objsize - size);
            pthread_mutex_unlock(&ar->ar_lock);
            return p;
        }
        
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

    memcpy(newp, p, (prev_sl->sl_objsize < size ? prev_sl->sl_objsize : size));
    rmem_slab_free(prev_sl, p);
    pthread_mutex_unlock(&ar->ar_lock);
    return newp;
}

int rmem_arena_duplicate(struct rmem_arena *dst_ar, struct rmem_arena *src_ar, void (*dup_cb)(void *, void *, void *, size_t), void (*done_cb)(void *), void *arg)
{
    if (dst_ar == src_ar) {
        errno = EINVAL;
        return -1;
    }

    if (src_ar > dst_ar) {
        pthread_mutex_lock(&dst_ar->ar_lock);
        pthread_mutex_lock(&src_ar->ar_lock);
    }
    else {
        pthread_mutex_lock(&src_ar->ar_lock);
        pthread_mutex_lock(&dst_ar->ar_lock);
    }
    
    struct rmem_slab *last_sl2 = dst_ar->ar_slabs;
    while (last_sl2 && last_sl2->sl_next)
        last_sl2 = last_sl2->sl_next;
    struct rmem_slab **prev_sl2 = (last_sl2 ? &last_sl2->sl_next : &dst_ar->ar_slabs);

    uint64_t start_ms = TIME_MS();

    for (struct rmem_slab *sl = src_ar->ar_slabs; sl; sl = sl->sl_next) {
        struct rmem_slab *sl2 = rmem_slab_create(sl->sl_size, sl->sl_objsize);
        if (!sl2) {
            rmem_arena_destroy(dst_ar);
            pthread_mutex_unlock(&src_ar->ar_lock);
            pthread_mutex_unlock(&dst_ar->ar_lock);
            return -1;
        }

        memcpy(sl2->sl_bitmap, sl->sl_bitmap, sl->sl_bitmap_words * sizeof(BITMAP_WORD));
        memcpy(sl2->sl_base, sl->sl_base, sl->sl_nobj * sl->sl_objsize);

        (*prev_sl2) = sl2;
        prev_sl2 = &sl2->sl_next;

        if (sl->sl_low)
            sl2->sl_low = (char *)sl2->sl_base + ((char *)sl->sl_low - (char *)sl->sl_base);
        if (sl->sl_high)
            sl2->sl_high = (char *)sl2->sl_base + ((char *)sl->sl_high - (char *)sl->sl_base);

        if (sl->sl_low && sl->sl_high && sl->sl_low != sl->sl_high) {
            if (dup_cb)
                dup_cb(arg, sl->sl_low, sl2->sl_low, (size_t)sl->sl_high - (size_t)sl->sl_low);
        }
    }

    uint64_t mid_ms = TIME_MS();

    if (done_cb)
        done_cb(arg);

    uint64_t end_ms = TIME_MS();
    PRINT("duplicate mid %" PRIu64 " ms last %" PRIu64 " ms", mid_ms - start_ms, end_ms - mid_ms);

    pthread_mutex_unlock(&src_ar->ar_lock);
    pthread_mutex_unlock(&dst_ar->ar_lock);
    return 0;
}