#include <errno.h>
#include <stdio.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <signal.h>
#include <getopt.h>
#include <sys/select.h>
#include <sys/ioctl.h>
#include <poll.h>
#include <pthread.h>
#include <setjmp.h>

#if defined(__linux__)
#include <pty.h>
#elif defined(__APPLE__)
#include <util.h>
#endif
#include "rmem.h"

//////////
//// API
//////////

#define __S(X) #X
#define _S(X) __S(X)

void _add_test(const char *n, void (*f)());
#define TEST(Name) void _test_ ## Name (); __attribute__((constructor)) void _c_ ## Name () { _add_test(#Name , _test_ ## Name); } void _test_ ## Name ()

#define FAIL(Msg, ...) do { fprintf(stderr, "FAIL (errno %d):\n  File \"" __FILE__ "\", line " _S(__LINE__) ", in ", errno); (strncmp("_test_", __FUNCTION__, 6)?fputs(__FUNCTION__, stderr):fprintf(stderr, "TEST(%s)", __FUNCTION__+6)); fprintf(stderr, "\n" Msg "\n", ##__VA_ARGS__); fflush(stderr); exit(1); } while (0)
#define ASSERT_EQ(X, Y) do { __auto_type __X = (X); __auto_type __Y = (Y); if (__X != __Y) FAIL("    ASSERT_EQ(" #X ", " #Y ");\n    ----\n    %lld != %lld", (long long) __X, (long long)__Y); } while (0)
#define ASSERT_NEQ(X, Y) do { __auto_type __X = (X); __auto_type __Y = (Y); if (__X == __Y) FAIL("    ASSERT_NEQ(" #X ", " #Y ");\n    ----\n    %lld == %lld", (long long) __X, (long long)__Y); } while (0)

static pthread_mutex_t test_phase_lock  = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  test_phase_cond  = PTHREAD_COND_INITIALIZER;
static int             test_phase_value = 0;
#define PHASE_WAIT(Phase) do { \
        pthread_mutex_lock(&test_phase_lock); \
        while (test_phase_value < (Phase)) \
            pthread_cond_wait(&test_phase_cond, &test_phase_lock); \
        pthread_mutex_unlock(&test_phase_lock); \
    } while(0)
#define PHASE_SET(Phase) do { \
        pthread_mutex_lock(&test_phase_lock); \
        test_phase_value = (Phase); \
        pthread_cond_broadcast(&test_phase_cond); \
        pthread_mutex_unlock(&test_phase_lock); \
    } while (0)
#define PHASE test_phase_value

#define PHASE_INC() PHASE_SET(test_phase_value+1)

static jmp_buf __assert_sig_buf;
static volatile sig_atomic_t __assert_sig_flag = 0;

static void __assert_sig_handler(int sig) {
    __assert_sig_flag = sig;
    longjmp(__assert_sig_buf, 1);
}

#define ASSERT_SIG(SIGNUM, BLOCK)                                  \
    do {                                                           \
        struct sigaction old_act, act;                             \
        act.sa_handler = __assert_sig_handler;                     \
        sigemptyset(&act.sa_mask);                                  \
        act.sa_flags = 0;                                          \
        sigaction(SIGNUM, &act, &old_act);                         \
                                                                    \
        __assert_sig_flag = 0;                                      \
        if (setjmp(__assert_sig_buf) == 0) {                        \
            BLOCK                                                   \
            if (__assert_sig_flag != SIGNUM)                        \
                FAIL("Expected signal %d", SIGNUM); \
        } else {                                                    \
            if (__assert_sig_flag != SIGNUM)                        \
                FAIL("Caught wrong signal %d != %d", __assert_sig_flag, SIGNUM); \
        }                                                           \
                                                                    \
        sigaction(SIGNUM, &old_act, NULL);                          \
    } while (0)

// -------------------- Simple tests --------------------

TEST(BasicAllocFree) {
    struct rmem_arena *ar = rmem_arena_create(NULL);
    ASSERT_NEQ(ar, NULL);

    void *p = rmem_arena_alloc(ar, 64);
    ASSERT_NEQ(p, NULL);

    rmem_arena_free(ar, p);

    rmem_arena_destroy(ar);
}

TEST(MultipleAllocFree) {
    struct rmem_arena *ar = rmem_arena_create(NULL);
    ASSERT_NEQ(ar, NULL);

    void *ptrs[100];
    for (int i = 0; i < 100; i++) {
        ptrs[i] = rmem_arena_alloc(ar, 32 + i);  // varying sizes
        ASSERT_NEQ(ptrs[i], NULL);
        memset(ptrs[i], 0xAA, 32 + i);      // write to memory
    }

    for (int i = 0; i < 100; i++) {
        rmem_arena_free(ar, ptrs[i]);
    }

    rmem_arena_destroy(ar);
}

TEST(AllocZero) {
    struct rmem_arena *ar = rmem_arena_create(NULL);
    ASSERT_NEQ(ar, NULL);

    void *p = rmem_arena_alloc(ar, 0);
    ASSERT_EQ(p, NULL);

    rmem_arena_destroy(ar);
}

TEST(AllocAfterFree) {
    struct rmem_arena *ar = rmem_arena_create(NULL);
    ASSERT_NEQ(ar, NULL);

    void *p1 = rmem_arena_alloc(ar, 128);
    ASSERT_NEQ(p1, NULL);

    rmem_arena_free(ar, p1);

    void *p2 = rmem_arena_alloc(ar, 128);
    ASSERT_NEQ(p2, NULL);

    rmem_arena_destroy(ar);
}

TEST(MultipleArenas) {
    struct rmem_arena *a1 = rmem_arena_create(NULL);
    struct rmem_arena *a2 = rmem_arena_create(NULL);
    ASSERT_NEQ(a1, NULL);
    ASSERT_NEQ(a2, NULL);

    void *p1 = rmem_arena_alloc(a1, 64);
    void *p2 = rmem_arena_alloc(a2, 64);
    ASSERT_NEQ(p1, NULL);
    ASSERT_NEQ(p2, NULL);

    rmem_arena_free(a1, p1);
    rmem_arena_free(a2, p2);

    rmem_arena_destroy(a1);
    rmem_arena_destroy(a2);
}

TEST(LargeAlloc) {
    struct rmem_arena *ar = rmem_arena_create(NULL);
    ASSERT_NEQ(ar, NULL);

    void *p = rmem_arena_alloc(ar, 1024*1024); // 1MB
    ASSERT_NEQ(p, NULL);

    memset(p, 0x55, 1024*1024);

    rmem_arena_free(ar, p);
    rmem_arena_destroy(ar);
}

TEST(RandomAllocFree) {
    struct rmem_arena *ar = rmem_arena_create(NULL);
    ASSERT_NEQ(ar, NULL);

    void *ptrs[50];
    for (int i = 0; i < 50; i++) {
        ptrs[i] = rmem_arena_alloc(ar, (i+1)*16);
        ASSERT_NEQ(ptrs[i], NULL);
    }

    // free in reverse order
    for (int i = 49; i >= 0; i--) {
        rmem_arena_free(ar, ptrs[i]);
    }

    rmem_arena_destroy(ar);
}

#define N_THREADS 8
#define N_ALLOC 1000

typedef struct {
    struct rmem_arena *arena;
    size_t thread_id;
} thread_arg_t;

static void *thread_alloc_free(void *arg) {
    thread_arg_t *ta = (thread_arg_t *)arg;
    void *ptrs[N_ALLOC];

    for (size_t i = 0; i < N_ALLOC; i++) {
        ptrs[i] = rmem_arena_alloc(ta->arena, 32 + (i % 128));
        if (!ptrs[i]) {
            fprintf(stderr, "Thread %zu allocation failed at %zu\n", ta->thread_id, i);
            exit(1);
        }
        memset(ptrs[i], (int)ta->thread_id, 32 + (i % 128));
    }

    // free in reverse order
    for (ssize_t i = N_ALLOC - 1; i >= 0; i--) {
        rmem_arena_free(ta->arena, ptrs[i]);
    }
    return NULL;
}

TEST(MultiThreadedStress) {
    struct rmem_arena *ar = rmem_arena_create(NULL);
    ASSERT_NEQ(ar, NULL);

    pthread_t threads[N_THREADS];
    thread_arg_t args[N_THREADS];

    for (size_t t = 0; t < N_THREADS; t++) {
        args[t].arena = ar;
        args[t].thread_id = t;
        if (pthread_create(&threads[t], NULL, thread_alloc_free, &args[t]) != 0)
            FAIL("Failed to create thread %zu", t);
    }

    for (size_t t = 0; t < N_THREADS; t++) {
        pthread_join(threads[t], NULL);
    }

    rmem_arena_destroy(ar);
}

TEST(DoubleFree) {
    struct rmem_arena *ar = rmem_arena_create(NULL);
    ASSERT_NEQ(ar, NULL);

    void *p = rmem_arena_alloc(ar, 64);
    ASSERT_NEQ(p, NULL);

    rmem_arena_free(ar, p); // normal free

    ASSERT_SIG(SIGABRT, {
        rmem_arena_free(ar, p); // double free should abort
    });

    rmem_arena_destroy(ar);
}

TEST(InvalidFree) {
    struct rmem_arena *ar = rmem_arena_create(NULL);
    ASSERT_NEQ(ar, NULL);

    void *p = rmem_arena_alloc(ar, 32);
    ASSERT_NEQ(p, NULL);

    char buf[8];

    ASSERT_SIG(SIGABRT, {
        rmem_arena_free(ar, buf); // invalid free should abort
    });

    rmem_arena_destroy(ar);
}

TEST(InvalidFreeAligned) {
    struct rmem_arena *ar = rmem_arena_create(NULL);
    ASSERT_NEQ(ar, NULL);

    void *p = rmem_arena_alloc(ar, 64);
    ASSERT_NEQ(p, NULL);

    ASSERT_SIG(SIGABRT, {
        rmem_arena_free(ar, (void *)((char *)p + 1)); // free not in start of obj
    });

    rmem_arena_destroy(ar);
}

static void fill_pattern(void *p, size_t n, uint8_t seed)
{
    uint8_t *c = p;
    for (size_t i = 0; i < n; i++)
        c[i] = (seed + i) & 0xFF;
}

static int check_pattern(void *p, size_t n, uint8_t seed)
{
    uint8_t *c = p;
    for (size_t i = 0; i < n; i++)
        if (c[i] != ((seed + i) & 0xFF))
            return 0;
    return 1;
}

TEST(ReallocFromNull)
{
    struct rmem_arena *ar = rmem_arena_create(NULL);

    void *p = rmem_arena_realloc(ar, NULL, 64);
    ASSERT_NEQ(p, NULL);

    rmem_arena_destroy(ar);
}

TEST(ReallocZeroFrees)
{
    struct rmem_arena *ar = rmem_arena_create(NULL);

    void *p = rmem_arena_alloc(ar, 64);
    ASSERT_NEQ(p, NULL);

    void *r = rmem_arena_realloc(ar, p, 0);
    ASSERT_EQ(r, NULL);

    /* Ensure it's freed by trying to free again */
    ASSERT_SIG(SIGABRT, {
        rmem_arena_free(ar, p);
    });

    rmem_arena_destroy(ar);
}

TEST(ReallocSameSizeReturnsSamePointer)
{
    struct rmem_arena *ar = rmem_arena_create(NULL);

    void *p = rmem_arena_alloc(ar, 48);
    ASSERT_NEQ(p, NULL);

    void *r = rmem_arena_realloc(ar, p, 48);
    ASSERT_EQ(r, p);

    rmem_arena_destroy(ar);
}

TEST(ReallocShrink)
{
    struct rmem_arena *ar = rmem_arena_create(NULL);

    void *p = rmem_arena_alloc(ar, 128);
    ASSERT_NEQ(p, NULL);

    fill_pattern(p, 128, 0x5A);

    void *r = rmem_arena_realloc(ar, p, 32);
    ASSERT_NEQ(r, NULL);

    ASSERT_EQ(check_pattern(r, 32, 0x5A), 1);

    rmem_arena_destroy(ar);
}

TEST(ReallocGrowSameSlabSizeClass)
{
    struct rmem_arena *ar = rmem_arena_create(NULL);

    void *p = rmem_arena_alloc(ar, 60);
    ASSERT_NEQ(p, NULL);

    fill_pattern(p, 60, 0x11);

    void *r = rmem_arena_realloc(ar, p, 63);
    ASSERT_EQ(r, p);
    ASSERT_EQ(check_pattern(r, 60, 0x11), 1);

    rmem_arena_destroy(ar);
}

TEST(ReallocGrowToNewSlab)
{
    struct rmem_arena *ar = rmem_arena_create(NULL);

    void *p = rmem_arena_alloc(ar, 40);
    ASSERT_NEQ(p, NULL);

    fill_pattern(p, 40, 0x22);

    void *r = rmem_arena_realloc(ar, p, 300);
    ASSERT_NEQ(r, NULL);
    ASSERT_NEQ(r, p);

    ASSERT_EQ(check_pattern(r, 40, 0x22), 1);

    /* Old pointer should have been freed */
    ASSERT_SIG(SIGABRT, {
        rmem_arena_free(ar, p);
    });

    rmem_arena_destroy(ar);
}

TEST(ReallocInvalidPointer)
{
    struct rmem_arena *ar = rmem_arena_create(NULL);

    int bogus = 123;

    ASSERT_SIG(SIGABRT, {
        rmem_arena_realloc(ar, &bogus, 100);
    });

    rmem_arena_destroy(ar);
}

TEST(ReallocAfterFree)
{
    struct rmem_arena *ar = rmem_arena_create(NULL);

    void *p = rmem_arena_alloc(ar, 80);
    ASSERT_NEQ(p, NULL);

    rmem_arena_free(ar, p);

    ASSERT_SIG(SIGABRT, {
        rmem_arena_realloc(ar, p, 100);
    });

    rmem_arena_destroy(ar);
}

TEST(ReallocLarge)
{
    struct rmem_arena *ar = rmem_arena_create(NULL);

    void *p = rmem_arena_alloc(ar, 512);
    ASSERT_NEQ(p, NULL);

    fill_pattern(p, 512, 0x33);

    void *r = rmem_arena_realloc(ar, p, 2000);
    ASSERT_NEQ(r, NULL);

    ASSERT_EQ(check_pattern(r, 512, 0x33), 1);

    rmem_arena_destroy(ar);
}

//////////
//// Framework
//////////

#define MAX_TESTS 1024

struct testinfo_t {
    void (*f)();
    const char * n;
    int r;
    void *out;
    size_t out_s;
    void *err;
    size_t err_s;
} tests[MAX_TESTS + 1] = {0};

static int get_next_test_idx() {
    int idx;
    for (idx = 0; idx < MAX_TESTS; idx++) {
        if (tests[idx].f == NULL)
            return idx;
    }

    return -1;
}

void _add_test(const char *n, void (*f)())
{
    int idx = get_next_test_idx();
    if (idx == -1) {
        fprintf(stderr, "too many tests for %s", (n));
        abort();
    }
    tests[idx].f = f;
    tests[idx].n = n;
}

static int runtest_gdb(struct testinfo_t *testinfo, int verbose)
{
    testinfo->f();
    return 0;
}

static int runtest(struct testinfo_t *testinfo, int verbose)
{
    int outp[2] = {-1, -1};
    if (-1 == pipe(outp)) {
        perror("pipe");
        abort();
    }

    int errp[2] = {-1, -1};
    if (-1 == pipe(errp)) {
        perror("pipe");
        abort();
    }

    if (-1 == fcntl(outp[0], F_SETFL, O_NONBLOCK) || -1 == fcntl(errp[0], F_SETFL, O_NONBLOCK)) {
        perror("fcntl");
        abort();
    }

    pid_t p = fork();
    if (p == -1) {
        perror("fork");
        abort();
    }

    if (0 == p) {
        close(outp[0]);
        close(errp[0]);
        dup2(outp[1], 1);
        dup2(errp[1], 2);
        close(outp[1]);
        close(errp[1]);

        if (getpid() != setsid()) {
            perror("set sid");
            exit(1);
        }

        testinfo->f();
        exit(0);
    }

    close(outp[1]);
    close(errp[1]);
    int status = 0;
    char tmp[0x4000] = {0};
    int newline = 1;

    while (1) {
        if (-1 == usleep(2000)) {
            perror("usleep");
            abort();
        }

        ssize_t b = read(outp[0], tmp, 0x4000);
        if (b == -1) {
            if (errno != EAGAIN) {
                perror("read(outp)");
                abort();
            }
            b = 0;
        }
        if (b) {
            if (verbose)
            {
                if (newline) {
                    printf("\n");
                    newline = 0;
                }
                write(1, tmp, b);
            }
            testinfo->out = realloc(testinfo->out, testinfo->out_s + b);
            if (!testinfo->out) {
                perror("realloc(out)");
                abort();
            }
            memcpy(((char *) testinfo->out) + testinfo->out_s, tmp, b);
            testinfo->out_s += b;
        }

        b = read(errp[0], tmp, 0x4000);
        if (b == -1) {
            if (errno != EAGAIN) {
                perror("read(errp)");
                abort();
            }
            b = 0;
        }
        if (b) {
            if (verbose) {
                if (newline) {
                    printf("\n");
                    newline = 0;
                }
                write(1, tmp, b);
            }
            testinfo->err = realloc(testinfo->err, testinfo->err_s + b);
            if (!testinfo->err) {
                perror("realloc(err)");
                abort();
            }
            memcpy(((char *) testinfo->err) + testinfo->err_s, tmp, b);
            testinfo->err_s += b;
        }

        pid_t a = 0;
        if (-1 == (a = waitpid(p, &status, WNOHANG))) {
            perror("waitpid");
            abort();
        }

        if (a == p) {
            break;
        }

        if (a != 0) {
            fprintf(stderr, "waitpid: unknown pid %d (want %d)\n", a, p);
            abort();
        }
    }

    close(outp[1]);
    close(errp[1]);

    if (!WIFEXITED(status)) {
        testinfo->r = -WTERMSIG(status);
        return testinfo->r;
    }

    testinfo->r = WEXITSTATUS(status);;
    return testinfo->r;
}

void helpexit(char *exe)
{
    printf("USAGE: %s [OPTION]...\n", exe);
    printf("Run tests\n\n");
    printf("  -l             print test list and exit\n");
    printf("  -k=TEST        only run TEST (can be passed multiple times)\n");
    printf("  -v             verbose output\n");
    printf("  -g             debug with gdb\n");
    printf("  -h             display this help and exit\n");
    exit(0);
}

void listtests(char *exe)
{
    printf("Tests list for %s:\n\n", exe);
    int i = 0;
    for (; tests[i].f; i++) {
        printf("  %d. %s\n", i+1, tests[i].n);
    }
    if (!i)
        printf("  - no tests\n");
    exit(0);
}

int gdb(int argc, char **argv)
{
    const char *gargv[] = {
        "gdb",
        "-ex",
        "r",
        "--args"
    };
    int gargc = sizeof(gargv)/sizeof(*gargv);

    putenv("TEST_IN_GDB=1");

    int nargc = gargc + argc;
    char **nargv = malloc((nargc+1) * sizeof(char *));
    if (!nargv) {
        errno = ENOMEM;
        perror("gdb");
        exit(1);
    }
    memcpy(nargv, gargv, gargc * sizeof(char *));
    memcpy(nargv + gargc, argv, argc * sizeof(char *));
    nargv[nargc] = 0;

    execvp("gdb", nargv);
    perror("execvp(\"gdb\")");
    exit(1);
}

int main(int argc, char **argv)
{
    setbuf(stdout, NULL);
    
    int test_names_s = 0;
    char **test_names = NULL;
    int c;
    int verbose = 0;
    int do_gdb = 0;
    do {
        c = getopt(argc, argv, "hlgvk:");

        if (c == 'h') {
            helpexit(argv[0]);
        }
        else if (c == 'l') {
            listtests(argv[0]);
        }
        else if (c == 'g') {
            if (!getenv("TEST_IN_GDB") || 0 != strcmp(getenv("TEST_IN_GDB"), "1"))
                do_gdb = 1;
        }
        else if (c == 'v') {
            verbose++;
        }
        else if (c == 'k') {
            test_names = realloc(test_names, sizeof(char *) * (test_names_s+1));
            if (!test_names) {
                fprintf(stderr, "out of memory\n");
                return 1;
            }
            test_names[test_names_s] = strdup(optarg);
            test_names_s++;
        }
        else {
            if (c == -1) {
                if (!argv[optind])
                    break;
                fprintf(stderr, "%s: invalid argument '%s'\n", argv[0], argv[optind]);
            }
            fprintf(stderr, "Try '%s -h' for more information.\n", argv[0]);
            exit(1);
        }
    }
    while (c >= 0);

    if (do_gdb) {
        if (test_names_s != 1) {
            fprintf(stderr, "%s: option -g may only be passed with a single test (-k TEST)\n", argv[0]);
            exit(1);
        }
        gdb(argc, argv);
    }

    int in_gdb = (getenv("TEST_IN_GDB") && 0 == strcmp(getenv("TEST_IN_GDB"), "1"));

    if (test_names_s) {
        for (int j = 0; j < test_names_s; j++) {
            int found = 0;
            for (int i = 0; tests[i].f; i++) {
                if (strcmp(test_names[j], tests[i].n) == 0) {
                    found = 1;
                    break;
                }
            }
            if (!found) {
                fprintf(stderr, "%s: invalid test \"%s\"\n", argv[0], test_names[j]);
                fprintf(stderr, "Try '%s -l' for tests list.\n", argv[0]);
                exit(1);
            }
        }
    }

    if (!in_gdb)
        printf("%s ", __FILE__);
        
    int r = 0, i = 0;
    for (; tests[i].f; i++) {
        if (test_names_s) {
            int skip = 1;
            for (int j = 0; j < test_names_s; j++) {
                if (strcmp(test_names[j], tests[i].n) == 0) {
                    skip = 0;
                    break;
                }
            }
            if (skip)
                continue;
        }

        if (!in_gdb) {
            r = runtest(tests + i, verbose);
            if (r) {
                printf((r>0)?"F":"C");
                break;
            }
            else {
                printf(".");
            }
        }
        else {
            runtest_gdb(tests + i, verbose);
            break;
        }
    }

    printf("\n");

    if (r) {
        printf("\n======== TEST FAILED: %s ========\n", tests[i].n);
        if (r<0) {
            printf("** CRASHED !!! ***\nSignal: %d\n", -r);
        }
        if (tests[i].out_s) {
            printf("\n==== captured stdout ====\n");
            while (tests[i].out_s) {
                size_t w = write(1, tests[i].out, tests[i].out_s);
                if (w == -1 || 0 == w) {
                    perror("write(out)");
                    break;
                }
                tests[i].out_s -= w;
                tests[i].out = (void *) (((char *) tests[i].out) + w);
            }
        }
        if (tests[i].err_s) {
            printf("\n==== captured stderr ====\n");
            while (tests[i].err_s) {
                size_t w = write(1, tests[i].err, tests[i].err_s);
                if (w == -1 || 0 == w) {
                    perror("write(err)");
                    break;
                }
                tests[i].err_s -= w;
                tests[i].err = (void *) (((char *) tests[i].err) + w);
            }
        }
    }

    return r;
}