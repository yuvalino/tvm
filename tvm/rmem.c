#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <errno.h>
#include <pthread.h>

#include "rmem.h"
// its mutexes are TVM's, counted (see the header); plain where rmem is
// linked without TVM (test_rmem)
#include "tvm_mutex.h"

__attribute__((weak)) int tvm_mutex_lock(pthread_mutex_t *m) { return (pthread_mutex_lock)(m); }
__attribute__((weak)) int tvm_mutex_unlock(pthread_mutex_t *m) { return (pthread_mutex_unlock)(m); }



/* -------------------- Helpers -------------------- */

static void (*print_cb)(char *, int) = NULL;


#define PRINT0(_msg, _len) do { \
    if (print_cb) { print_cb(_msg, _len); } \
    else { write(1, _msg, _len); } \
} while (0)


#define PRINT(MSG, ...) do { \
    char __msgbuf[4096]; \
    int __len = snprintf(__msgbuf, sizeof(__msgbuf), "%s:%d %s: " MSG "\n", __FILE__, __LINE__, __func__, ##__VA_ARGS__); \
    PRINT0(__msgbuf, __len); \
} while (0)


#define ABORT(MSG, ...) do { \
    char __msgbuf[4096]; \
    int __len = snprintf(__msgbuf, sizeof(__msgbuf), "ABORT! %s:%d %s: " MSG "\n", __FILE__, __LINE__, __func__, ##__VA_ARGS__); \
    PRINT0(__msgbuf, __len); \
    abort(); \
} while (0)
// Code holding an arena lock unlocks it before ABORT: the abort is delivered
// as a signal that tears the task down, and the teardown frees into that
// arena.


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

/*
 * A slab reserves room for 512 objects of its size class, but never more
 * than RMEM_MAX_SLAB of address space: larger size classes get as many objects
 * as fit, at least one, plus a page for the header and bitmap.
 *
 * Uncapped, the reservation grew with the object size: one 16 MB allocation
 * reserved an 8 GB slab, and a git process reached 31 slabs / ~20 GB of
 * reserved address space. That is harmless on macOS, but fork duplicates each
 * slab at its full size, so the child needed another ~20 GB, and on iOS,
 * whose per-app address space is far smaller, the slab mmap failed with
 * ENOMEM in every fork of a git process (`git commit`'s auto-maintenance
 * daemonizes with fork), which then brought the whole VM down.
 */
#define RMEM_MAX_SLAB ((size_t)64 << 20)

size_t get_slab_size(size_t objsize) {
    size_t pagesz = (size_t)sysconf(_SC_PAGESIZE);

    size_t min_nobj = 512;   // tweakable
    size_t slabsize;
    if (objsize <= RMEM_MAX_SLAB / min_nobj) {
        slabsize = objsize * min_nobj;
    }
    else {
        size_t nobj = RMEM_MAX_SLAB / objsize;
        if (!nobj)
            nobj = 1;
        slabsize = nobj * objsize + pagesz;
    }

    /* round up to multiple of page size for mmap */
    slabsize = round_up(slabsize, pagesz);

    return slabsize;
}

/*
 * Zero [p, p + len) of a slab, as a freed object (or the tail of a shrunk one)
 * must be: the next allocation from it relies on it. The whole pages in it
 * are replaced by fresh anonymous ones instead of written, which zeroes them
 * just the same but hands their memory back to the system.
 *
 * memset kept every page an object had ever covered resident, and dirtied
 * the pages it had not: a freed 128 KB object stayed 128 KB of the process's
 * footprint even if its program had written a few bytes of it, and nothing in
 * a slab was ever returned. node under TVM ran Claude Code with a 3.4 GB peak
 * footprint, ~1.1 GB of it slabs of freed, zeroed V8 and libuv buffers, 16
 * times its footprint outside TVM; iOS killed the app for it (jetsam,
 * per-process-limit). Below RMEM_ZERO_REMAP the system call costs more than
 * the memset it saves.
 */
#define RMEM_ZERO_REMAP ((size_t)64 << 10)

static void rmem_zero(void *p, size_t len)
{
    if (len >= RMEM_ZERO_REMAP) {
        size_t pagesz = (size_t)sysconf(_SC_PAGESIZE);
        uintptr_t lo = round_up((uintptr_t)p, pagesz);
        uintptr_t hi = ((uintptr_t)p + len) & ~(uintptr_t)(pagesz - 1);
        if (hi > lo && hi - lo >= RMEM_ZERO_REMAP &&
            rops.ro_mmap((void *)lo, hi - lo, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED, -1, 0) == (void *)lo) {
            memset(p, 0, lo - (uintptr_t)p);
            memset((void *)hi, 0, (uintptr_t)p + len - hi);
            return;
        }
    }
    memset(p, 0, len);
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

struct rmem_arena {
    void *ar_private;

    pthread_mutex_t    ar_lock;
    struct rmem_slab *ar_slabs;
};

static struct rmem_slab *rmem_slab_create(size_t slabsize, size_t objsize) {
    size_t pagesz = sysconf(_SC_PAGESIZE);
    slabsize = round_up(slabsize, pagesz);

    /* layout:
    [ header | bitmap | pad | data ]

    The data starts on a multiple of min(objsize, page size). objsize is a
    power of two, so every object is then aligned to its own size up to a
    page, which is what rmem_arena_memalign() relies on (posix_memalign,
    aligned_alloc, ...). Without the pad the objects were only 8-aligned.
    */
    size_t align = objsize < pagesz ? objsize : pagesz;
    size_t nobj = (slabsize - sizeof(struct rmem_slab)) / objsize;
    size_t bitmap_words = 0;
    size_t data_off = 0;
    while (nobj > 0) {
        bitmap_words = (nobj + BITMAP_WORD_BITS - 1) / BITMAP_WORD_BITS;
        data_off = round_up(sizeof(struct rmem_slab) + bitmap_words * sizeof(BITMAP_WORD), align);
        if (data_off + nobj * objsize <= slabsize)
            break;
        nobj--;
    }

    if (!nobj) {
        errno = ENOMEM;  // not even one object of this size fits
        return NULL;
    }

    void *mem = rops.ro_mmap(NULL, slabsize, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
    if (mem == MAP_FAILED)
        return NULL;


    struct rmem_slab *sl = (struct rmem_slab *)mem;
    sl->sl_next = NULL;

    sl->sl_base = (char*)mem + data_off;
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

// called with the arena lock held, which is released before any abort
static void rmem_slab_free(struct rmem_arena *ar, struct rmem_slab *sl, void *p)
{
    uintptr_t addr = (uintptr_t)p;

    if (addr < (uintptr_t)sl->sl_base ||
        addr >= (uintptr_t)sl->sl_base + sl->sl_nobj * sl->sl_objsize) {
        pthread_mutex_unlock(&ar->ar_lock);
        ABORT("invalid pointer %p", p);
    }

    uintptr_t off = addr - (uintptr_t)sl->sl_base;
    if (off % sl->sl_objsize != 0) {
        pthread_mutex_unlock(&ar->ar_lock);
        ABORT("p %p objsize 0x%zx remainder 0x%zx",
              p, sl->sl_objsize, off % sl->sl_objsize);
    }

    size_t idx = off / sl->sl_objsize;

    if (!bitmap_test_range(sl->sl_bitmap, idx, 1)) {
        pthread_mutex_unlock(&ar->ar_lock);
        ABORT("detected double free on %p (objsize 0x%zx)",
              p, sl->sl_objsize);
    }

    bitmap_set_range(sl->sl_bitmap, idx, 1, 0);
    rmem_zero(p, sl->sl_objsize);

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

/*
 * Move `sl` (found after `prev` while walking ar_slabs) to the head of the
 * list. Called with the arena lock held.
 *
 * alloc and free both find their slab by walking ar_slabs, and new slabs are
 * pushed at the head, so the small size classes a program uses constantly --
 * created first -- ended up at the tail: bash walked ~20 of its 24 slabs on
 * every malloc and every free (~55 of each per iteration of a shell loop),
 * which with the arena lock and TLS lookups made `i=$((i+1))` loops ~8x
 * slower than the same bash binary outside TVM. Keeping the slab last used
 * at the head makes the common case one or two steps. List order carries no
 * other meaning (fork copies every slab).
 */
static inline void rmem_slab_to_front(struct rmem_arena *ar, struct rmem_slab *prev, struct rmem_slab *sl)
{
    if (!prev)
        return;
    prev->sl_next = sl->sl_next;
    sl->sl_next = ar->ar_slabs;
    ar->ar_slabs = sl;
}

static void *rmem_arena_alloc_locked(struct rmem_arena *ar, size_t size) {
    if (!size)
        return NULL;

    // round_up_objsize doubles until it reaches size: above 2^63 that
    // overflows to 0 and never terminates (malloc(SIZE_MAX) spun forever
    // holding the arena lock). Nothing that large can be mapped anyway.
    if (size > ((size_t)1 << 48)) {
        errno = ENOMEM;
        return NULL;
    }

    size = round_up_objsize(size);

    size_t need = 1;

    struct rmem_slab *sl, *prev = NULL;
    size_t idx;
    void *p = NULL;
    for (sl = ar->ar_slabs; sl; prev = sl, sl = sl->sl_next) {
        if (sl->sl_objsize != size)
            continue;

        idx = bitmap_find_run_zero(sl->sl_bitmap, sl->sl_bitmap_words, sl->sl_nobj, need);
        if (idx != SIZE_MAX) {
            p = (char*)sl->sl_base + idx * sl->sl_objsize;
            rmem_slab_to_front(ar, prev, sl);
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

/*
 * An object aligned to `alignment` (a power of two). Objects are aligned to
 * their size class up to a page (see rmem_slab_create), so a size class of
 * at least `alignment` is enough; larger alignments are not supported
 * (EINVAL). The result is an ordinary object: rmem_arena_free and
 * rmem_arena_realloc take it like any other.
 */
void *rmem_arena_memalign(struct rmem_arena *ar, size_t alignment, size_t size) {
    size_t pagesz = (size_t)sysconf(_SC_PAGESIZE);
    if (!alignment || (alignment & (alignment - 1)) || alignment > pagesz) {
        errno = EINVAL;
        return NULL;
    }
    if (!size)
        size = 1;
    return rmem_arena_alloc(ar, size < alignment ? alignment : size);
}

void rmem_arena_free(struct rmem_arena *ar, void *p) {
    if (!p) return;
    pthread_mutex_lock(&ar->ar_lock);
    for (struct rmem_slab *sl = ar->ar_slabs, *prev = NULL; sl; prev = sl, sl = sl->sl_next) {
        if ((uintptr_t)p < (uintptr_t)sl->sl_base || (uintptr_t)p >= ((uintptr_t)sl->sl_base + sl->sl_nobj * sl->sl_objsize))
            continue;

        rmem_slab_free(ar, sl, p);
        rmem_slab_to_front(ar, prev, sl);
        pthread_mutex_unlock(&ar->ar_lock);
        return;
    }
    pthread_mutex_unlock(&ar->ar_lock);
    ABORT("invalid pointer %p", p);
}

size_t rmem_arena_usable_size(struct rmem_arena *ar, const void *p)
{
    size_t ret = 0;
    if (!p)
        return 0;
    pthread_mutex_lock(&ar->ar_lock);
    for (struct rmem_slab *sl = ar->ar_slabs; sl; sl = sl->sl_next) {
        if ((uintptr_t)p < (uintptr_t)sl->sl_base || (uintptr_t)p >= ((uintptr_t)sl->sl_base + sl->sl_nobj * sl->sl_objsize))
            continue;
        uintptr_t off = (uintptr_t)p - (uintptr_t)sl->sl_base;
        if (off % sl->sl_objsize == 0 && bitmap_test_range(sl->sl_bitmap, off / sl->sl_objsize, 1))
            ret = sl->sl_objsize;
        break;
    }
    pthread_mutex_unlock(&ar->ar_lock);
    return ret;
}

void *rmem_arena_realloc(struct rmem_arena *ar, void *p, size_t size)
{
    if (!p)
        return rmem_arena_alloc(ar, size);

    if (size == 0) {
        rmem_arena_free(ar, p);
        return NULL;
    }

    // see rmem_arena_alloc_locked: round_up_objsize cannot represent this
    if (size > ((size_t)1 << 48)) {
        errno = ENOMEM;
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
        if (off % sl->sl_objsize != 0) {
            pthread_mutex_unlock(&ar->ar_lock);
            ABORT("p %p objsize 0x%zx remainder 0x%zx", p, sl->sl_objsize, off % sl->sl_objsize);
        }

        idx = off / sl->sl_objsize;
        if (!bitmap_test_range(sl->sl_bitmap, idx, 1)) {
            pthread_mutex_unlock(&ar->ar_lock);
            ABORT("detected double free on %p (objsize 0x%zx)", p, sl->sl_objsize);
        }

        // short circuit, keep in same slab
        if (objsize == sl->sl_objsize) {
            if (objsize > size)
                rmem_zero((char *)p + size, sl->sl_objsize - size);
            pthread_mutex_unlock(&ar->ar_lock);
            return p;
        }
        
        prev_sl = sl;
        break;
    }

    if (!prev_sl) {
        pthread_mutex_unlock(&ar->ar_lock);
        ABORT("invalid pointer %p", p);
    }

    void *newp = rmem_arena_alloc_locked(ar, size);
    if (!newp) {
        pthread_mutex_unlock(&ar->ar_lock);
        return NULL;
    }

    memcpy(newp, p, (prev_sl->sl_objsize < size ? prev_sl->sl_objsize : size));
    rmem_slab_free(ar, prev_sl, p);
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

    for (struct rmem_slab *sl = src_ar->ar_slabs; sl; sl = sl->sl_next) {
        struct rmem_slab *sl2 = rmem_slab_create(sl->sl_size, sl->sl_objsize);
        if (!sl2) {
            // The caller owns dst_ar and destroys it. Destroying it here
            // unmapped the arena and then unlocked the mutex inside it: a
            // fault in the fork child while holding the parent's arena lock,
            // which deadlocked the VM instead of reporting the failure.
            // errno is rmem_slab_create's.
            pthread_mutex_unlock(&src_ar->ar_lock);
            pthread_mutex_unlock(&dst_ar->ar_lock);
            return -1;
        }

        memcpy(sl2->sl_bitmap, sl->sl_bitmap, sl->sl_bitmap_words * sizeof(BITMAP_WORD));

        /* only copy the allocated range [sl_low..sl_high), rest is zero from mmap */
        if (sl->sl_low && sl->sl_high) {
            size_t copy_off = (size_t)((char *)sl->sl_low - (char *)sl->sl_base);
            size_t copy_len = (size_t)((char *)sl->sl_high - (char *)sl->sl_low);
            memcpy((char *)sl2->sl_base + copy_off, sl->sl_low, copy_len);
        }

        (*prev_sl2) = sl2;
        prev_sl2 = &sl2->sl_next;

        if (sl->sl_low)
            sl2->sl_low = (char *)sl2->sl_base + ((char *)sl->sl_low - (char *)sl->sl_base);
        if (sl->sl_high)
            sl2->sl_high = (char *)sl2->sl_base + ((char *)sl->sl_high - (char *)sl->sl_base);

        if (sl->sl_low && sl->sl_high && sl->sl_low != sl->sl_high) {
            if (dup_cb)
                dup_cb(arg, sl->sl_low, sl2->sl_low,
                       (size_t)sl->sl_high - (size_t)sl->sl_low);
        }
    }

    if (done_cb)
        done_cb(arg);

    pthread_mutex_unlock(&src_ar->ar_lock);
    pthread_mutex_unlock(&dst_ar->ar_lock);
    return 0;
}