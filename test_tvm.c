#include <errno.h>
#include <stdio.h>
#include <fcntl.h>
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
#include <signal.h>

#if defined(__linux__)
#include <pty.h>
#elif defined(__APPLE__)
#include <util.h>
#endif

#include "tvm.h"

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

// Assuming these are shared between all tvm tasks
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

//////////
//// Tests
//////////

TEST(ProcWait) {
    ASSERT_EQ(-1, wait(NULL));
    ASSERT_EQ(errno, ECHILD);
}

TEST(ProcExit) {
    printf("begin\n");

    pid_t p = fork();
    ASSERT_NEQ(p, -1);
    if (!p) {
        printf("p 1\n");
        exit(25);
    }
        
    int s;
    ASSERT_EQ(p, wait(&s));
    ASSERT_EQ(WIFEXITED(s), 1);
    ASSERT_EQ(WIFSIGNALED(s), 0);
    ASSERT_EQ(WEXITSTATUS(s), 25);

    printf("again\n");

    p = fork();
    ASSERT_NEQ(p, -1);
    if (!p) {
        printf("p 2");
        exit(26);
    }
    ASSERT_EQ(p, wait(&s));
    ASSERT_EQ(WIFEXITED(s), 1);
    ASSERT_EQ(WIFSIGNALED(s), 0);
    ASSERT_EQ(WEXITSTATUS(s), 26);

    printf("done\n");
}

TEST(ProcWNOHANG) {
    pid_t p = fork();
    ASSERT_NEQ(p, -1);

    if (!p) {
        PHASE_WAIT(1);
        _exit(0);
    }
    
    ASSERT_EQ(0, waitpid(-1, NULL, WNOHANG));
    PHASE_SET(1);
    
    int s;
    ASSERT_EQ(p, waitpid(-1, &s, 0));
    ASSERT_EQ(WIFEXITED(s), 1);
    ASSERT_EQ(WEXITSTATUS(s), 0);
}

TEST(ProcWaitPgrp) {
    pid_t p = fork();
    ASSERT_NEQ(p, -1);

    if (!p) {
        ASSERT_EQ(0, setpgid(0, 0));
        PHASE_SET(1);
        _exit(0);
    }

    PHASE_WAIT(1);

    ASSERT_EQ(-1, waitpid(0, NULL, WNOHANG));
    ASSERT_EQ(errno, ECHILD);

    int s;
    ASSERT_EQ(p, waitpid(-p, &s, 0));
    ASSERT_EQ(WIFEXITED(s), 1);
    ASSERT_EQ(WEXITSTATUS(s), 0);
}

TEST(ProcRaise) {
    pid_t p = fork();
    ASSERT_NEQ(p, -1);

    if (!p)
        raise(15);
    
    int s;
    ASSERT_EQ(p, wait(&s));
    ASSERT_EQ(WIFEXITED(s), 0);
    ASSERT_EQ(WIFSIGNALED(s), 1);
    ASSERT_EQ(WTERMSIG(s), 15);
}

TEST(ProcKill) {
    pid_t p = fork();
    ASSERT_NEQ(p, -1);

    if (!p) {
        PHASE_SET(1);
        pause();
    }
    
    PHASE_WAIT(1);
    ASSERT_EQ(kill(p, 11), 0);

    int s;
    ASSERT_EQ(p, wait(&s));
    ASSERT_EQ(WIFEXITED(s), 0);
    ASSERT_EQ(WIFSIGNALED(s), 1);
    ASSERT_EQ(WTERMSIG(s), 11);
}

TEST(ProcParent) {
    int pip[2];
    int pip2[2];
    ASSERT_EQ(pipe(pip), 0);
    ASSERT_EQ(pipe(pip2), 0);
    printf("[%d] start\n", getpid());
    pid_t p2;
    pid_t p1 = fork();
    ASSERT_NEQ(p1, -1);

    if (!p1) {
        printf("[%d] p1 start\n", getpid());
        ASSERT_EQ(0, close(pip[0]));
        ASSERT_EQ(0, close(pip2[0]));
        
        pid_t p_2 = fork();
        ASSERT_NEQ(p_2, -1);
        
        if (!p_2) {
            printf("[%d] p2 start\n", getpid());
            ASSERT_EQ(0, close(pip[1]));
            pid_t pz_1 = getppid();

            PHASE_WAIT(1); /// wait for parent to kill(p1, 0)
            PHASE_SET(2);
            PHASE_WAIT(4); // wait for p1 to die and be reaped by parent

            pid_t pz_2 = getppid();

            ASSERT_EQ(sizeof(pz_1), write(pip2[1], &pz_1, sizeof(pz_1)));
            ASSERT_EQ(sizeof(pz_2), write(pip2[1], &pz_2, sizeof(pz_2)));
            
            printf("[%d] p2 end\n", getpid());
            exit(0);
        }
        ASSERT_EQ(0, close(pip2[1]));
        printf("[%d] forked %d\n", getpid(), p_2);

        PHASE_WAIT(2);  // wait for grandchild to do `pz_1 = getppid()` and get p1

        ASSERT_EQ(sizeof(p_2), write(pip[1], &p_2, sizeof(p_2)));

        PHASE_WAIT(3);
        exit(0);
    }

    printf("[%d] forked %d\n", getpid(), p1);

    ASSERT_EQ(0, close(pip[1]));
    ASSERT_EQ(0, close(pip2[1]));
    ASSERT_EQ(0, kill(p1, 0));

    PHASE_SET(1); // let grandchild continue

    ASSERT_EQ(sizeof(p2), read(pip[0], &p2, sizeof(p2)));
    ASSERT_EQ(PHASE, 2);

    ASSERT_EQ(0, kill(p2, 0));

    PHASE_SET(3);

    ASSERT_EQ(0, read(pip[0], &p2, sizeof(p2))); // ensure child is zombie
    ASSERT_EQ(-1, kill(p1, 0)); // kill zombie should fail
    ASSERT_EQ(ESRCH, errno);

    ASSERT_EQ(p1, waitpid(p1, NULL, 0));

    // we can't check this because the system may have re-used this PID
    //ASSERT_EQ(-1, kill(p1, 0));
    //ASSERT_EQ(ESRCH, errno);

    PHASE_SET(4);

    pid_t pzz_1, pzz_2;
    ASSERT_EQ(sizeof(pzz_1), read(pip2[0], &pzz_1, sizeof(pzz_1)));
    ASSERT_EQ(pzz_1, p1);
    ASSERT_EQ(sizeof(pzz_2), read(pip2[0], &pzz_2, sizeof(pzz_2)));
    ASSERT_EQ(pzz_2, 1);
}

TEST(ProcSid) {
    ASSERT_EQ(getpid(), getsid(0));
    ASSERT_EQ(-1, setsid());
    ASSERT_EQ(EPERM, errno);

    pid_t p = fork();
    ASSERT_NEQ(p, -1);

    if (!p) {
        ASSERT_EQ(getppid(), getsid(0));
        
        PHASE_WAIT(1);

        ASSERT_EQ(getpid(), setsid());
        ASSERT_EQ(getpid(), getsid(0));
        
        PHASE_SET(2);

        exit(0);
    }

    ASSERT_EQ(getpid(), getsid(p));

    PHASE_SET(1);
    PHASE_WAIT(2);

    ASSERT_EQ(p, getsid(p));

    int s;
    ASSERT_EQ(p, wait(&s));
    ASSERT_EQ(WIFEXITED(s), 1);
    ASSERT_EQ(WEXITSTATUS(s), 0);
}

TEST(ProcPgid) {
    ASSERT_EQ(getpid(), getpgrp());
    ASSERT_EQ(-1, setpgrp());
    ASSERT_EQ(EPERM, errno);

    pid_t p = fork();
    ASSERT_NEQ(p, -1);

    if (!p) {
        ASSERT_EQ(getppid(), getpgrp());
        
        PHASE_WAIT(1);

        ASSERT_EQ(0, setpgrp());
        ASSERT_EQ(getpid(), getpgrp());

        PHASE_SET(2);

        exit(0);
    }

    ASSERT_EQ(getpid(), getpgid(p));

    PHASE_SET(1);
    PHASE_WAIT(2);

    ASSERT_EQ(getpid(), getsid(p));
    ASSERT_EQ(p, getpgid(p));

    int s;
    ASSERT_EQ(p, wait(&s));
    ASSERT_EQ(WIFEXITED(s), 1);
    ASSERT_EQ(WEXITSTATUS(s), 0);
}

TEST(ProcPgidSet) {
    pid_t p1 = fork();
    ASSERT_NEQ(p1, -1);
    if (!p1) {
        PHASE_WAIT(1);
        
        ASSERT_EQ(getpid(), getpgrp());
        
        exit(0);
    }

    pid_t p2 = fork();
    ASSERT_NEQ(p2, -1);
    if (!p2) {
        PHASE_WAIT(1);
        
        ASSERT_EQ(p1, getpgrp());

        exit(0);
    }

    ASSERT_EQ(0, setpgid(p1, 0));
    ASSERT_EQ(p1, getpgid(p1));
    ASSERT_EQ(0, setpgid(p2, p1));
    ASSERT_EQ(p1, getpgid(p2));

    PHASE_SET(1);

    int s;
    ASSERT_EQ(p1, waitpid(p1, &s, 0));
    ASSERT_EQ(WIFEXITED(s), 1);
    ASSERT_EQ(WEXITSTATUS(s), 0);

    ASSERT_EQ(p2, wait(&s));
    ASSERT_EQ(WIFEXITED(s), 1);
    ASSERT_EQ(WEXITSTATUS(s), 0);
}

static __thread int sigged = 0;
void my_handler(int signo, siginfo_t *si, void * ctx) {
    sigged = 1;
}

TEST(SignalsHandler) {

    struct sigaction old_act, act = {
        .sa_sigaction = my_handler,
        .sa_flags = SA_SIGINFO,
    };
    ASSERT_EQ(0, sigaction(SIGFPE, &act, &old_act));

    pid_t p = fork();
    ASSERT_NEQ(p, -1);

    if (!p) {
        if (0 != sigaction(SIGFPE, &old_act, NULL))
            exit(1);
        raise(SIGFPE);
    }

    int s;
    ASSERT_EQ(p, wait(&s));
    ASSERT_EQ(WIFSIGNALED(s), 1);
    ASSERT_EQ(WTERMSIG(s), SIGFPE);
    // child died , but parent will live!
    ASSERT_EQ(0, raise(SIGFPE));
    ASSERT_EQ(1, sigged);
}

static int testhandlersig = 0;
static void testhandler(int signo) {
    testhandlersig++;
}

TEST(SignalsSIGCHLD) {

    /////
    // SIGCHLD set to SIG_IGN
    // wait before child has exited
    /////
    struct sigaction act = {
        .sa_handler = SIG_IGN,
        .sa_flags = 0,
    };
    ASSERT_EQ(0, sigaction(SIGCHLD, &act, NULL));

    pid_t p = fork();
    ASSERT_NEQ(p, -1);
    if (!p) {
        PHASE_WAIT(1);
        exit(0);
    }
    /**
     * POSIX.1-2001 specifies that if the disposition of SIGCHLD is set to SIG_IGN or the SA_NOCLDWAIT flag is set
     * for  SIGCHLD (see sigaction(2)), then children that terminate do not become zombies and a call to wait() or
     * waitpid() will block until all children have terminated, and then fail with  errno  set  to  ECHILD.
     */
    ASSERT_EQ(0, waitpid(-1, NULL, WNOHANG));
    PHASE_SET(1);
    ASSERT_EQ(-1, wait(NULL));
    ASSERT_EQ(errno, ECHILD);

    /////
    // SIGCHLD set to SIG_IGN
    // wait after child has exited
    /////
    int pip[2];
    ASSERT_EQ(0, pipe(pip));
    
    p = fork();
    ASSERT_NEQ(p, -1);
    
    if (!p) {
        ASSERT_EQ(0, close(pip[0]));
        exit(0);
    }

    ASSERT_EQ(0, close(pip[1]));
    ASSERT_EQ(0, read(pip[0], pip, 1)); // wait for child exit first (pip is closed)
    ASSERT_EQ(-1, waitpid(-1, NULL, WNOHANG));
    ASSERT_EQ(errno, ECHILD);
    ASSERT_EQ(0, close(pip[0]));

    /////
    // SIGCHLD set to a handler
    /////
    act.sa_handler = testhandler;
    ASSERT_EQ(0, sigaction(SIGCHLD, &act, NULL));

    p = fork();
    ASSERT_NEQ(p, -1);
    if (!p) {
        exit(0);
    }
    ASSERT_EQ(p, wait(NULL));
    ASSERT_EQ(testhandlersig, 1);

    /////
    // SIGCHLD set to a handler and flags to NOCLDWAIT
    /////
    act.sa_flags = SA_NOCLDWAIT;
    ASSERT_EQ(0, sigaction(SIGCHLD, &act, NULL));

    p = fork();
    ASSERT_NEQ(p, -1);
    if (!p) {
        exit(0);
    }
    ASSERT_EQ(-1, wait(NULL));
    ASSERT_EQ(errno, ECHILD);
    ASSERT_EQ(testhandlersig, 2);
}

TEST(SignalsPgrp) {
    pid_t p1 = fork();
    ASSERT_NEQ(p1, -1);
    if (!p1) {
        ASSERT_EQ(0, setpgid(0, 0));
        PHASE_SET(1);
        pause();
        _exit(0);
    }

    PHASE_WAIT(1);
    
    // we test -p1 actually redirects a signal to the child
    ASSERT_EQ(0, kill(-p1, SIGTERM));

    int s;
    ASSERT_EQ(p1, wait(&s));
    ASSERT_EQ(WIFSIGNALED(s), 1);
    ASSERT_EQ(WTERMSIG(s), SIGTERM);
}

TEST(FDsInit) {
    ASSERT_EQ(-1, close(3));
    ASSERT_EQ(3, open("/dev/random", O_RDONLY));
    ASSERT_EQ(-1, close(4));
    
    pid_t p = fork();
    ASSERT_NEQ(p, -1);

    if (!p) {
        ASSERT_EQ(0, close(3));
        exit(open("/dev/random", O_RDONLY));
    }
    
    int s;
    ASSERT_EQ(p, wait(&s));
    ASSERT_EQ(WIFEXITED(s), 1);
    ASSERT_EQ(WEXITSTATUS(s), 3);  // parent has 0-3, child closed 3 and opened, so its 3

    ASSERT_EQ(4, open("/dev/random", O_RDONLY)); // child had exited, so its FDs were cleared
    ASSERT_EQ(0, close(3)); // child fd table and parent are different, so can close 3 if child closed 3
    ASSERT_EQ(-1, close(3));
}

TEST(FDsPoll) {
    pid_t p = fork();
    ASSERT_NEQ(p, -1);

    if (!p) {
        int pp[2];
        ASSERT_EQ(0, pipe(pp));

        PHASE_SET(1);
        PHASE_WAIT(2);

        _exit(0);
    }

    PHASE_WAIT(1);

    int pp[2];
    ASSERT_EQ(0, pipe(pp));

    ASSERT_EQ(0, waitpid(-1, NULL, WNOHANG));
    PHASE_SET(2);

    // the point of the fork is to make sure our pipes' FD value are bigger than other FDs

    int s;
    ASSERT_EQ(p, wait(&s));
    ASSERT_EQ(WIFEXITED(s), 1);
    ASSERT_EQ(WEXITSTATUS(s), 0);
    
    fd_set rfds;
    fd_set wfds;
    int nfds = ((pp[0]<pp[1])?pp[1]:pp[0]) + 1;
    FD_ZERO(&rfds);
    FD_ZERO(&wfds);
    struct pollfd fd[2] = {{.fd = pp[0], POLLIN, 0}, {.fd = pp[1], POLLOUT, 0}};
    struct timeval t = {0, 0};

    ASSERT_EQ(poll(fd, 2, 0), 1);
    ASSERT_NEQ(fd[1].revents & POLLOUT, 0);
    fd[1].revents = 0;
    FD_SET(pp[0], &rfds);
    FD_SET(pp[1], &wfds);
    ASSERT_EQ(select(nfds, &rfds, &wfds, NULL, &t), 1);
    ASSERT_NEQ(FD_ISSET(pp[1], &wfds), 0);
    ASSERT_EQ(FD_ISSET(pp[0], &rfds), 0);
    
    ASSERT_EQ(1, write(pp[1], (void *)&rfds, 1));
    ASSERT_EQ(poll(fd, 2, 0), 2);
    ASSERT_EQ(fd[0].revents & POLLIN, POLLIN);
    ASSERT_EQ(fd[1].revents & POLLOUT, POLLOUT);
    FD_SET(pp[0], &rfds);
    FD_SET(pp[1], &wfds);
    ASSERT_EQ(select(nfds, &rfds, &wfds, NULL, &t), 2);
    ASSERT_NEQ(FD_ISSET(pp[1], &wfds), 0);
    ASSERT_NEQ(FD_ISSET(pp[0], &rfds), 0);
}

TEST(FDsFcntl) {
    pid_t p = fork();
    ASSERT_NEQ(p, -1);

    if (!p) {
        int pp[2];
        ASSERT_EQ(0, pipe(pp));
        
        PHASE_SET(1);
        PHASE_WAIT(2);

        _exit(0);
    }

    PHASE_WAIT(1);

    int pp[2];
    ASSERT_EQ(0, pipe(pp));

    // the point of the fork is to make sure our pipes are bigger than other FDs

    PHASE_SET(2);

    int s;
    ASSERT_EQ(p, wait(&s));
    ASSERT_EQ(WIFEXITED(s), 1);
    ASSERT_EQ(WEXITSTATUS(s), 0);
    
    int nf = fcntl(pp[0], F_DUPFD, 10);
    ASSERT_EQ(nf, 10);
    int nf2 = fcntl(pp[0], F_DUPFD, 10);
    ASSERT_EQ(nf2, 11);
}

TEST(FDsCloseOnExit) {
    int pp[2];
    ASSERT_EQ(0, pipe(pp));

    pid_t p = fork();
    ASSERT_NEQ(p, -1);
    if (!p) {
        close(pp[0]); // child doesn't need read end
        // child holds write end open, then exits — write end should close
        _exit(0);
    }
    close(pp[1]); // parent doesn't need write end

    // After child exits, its write end is closed, so read should return 0 (EOF)
    char buf[1];
    int r = read(pp[0], buf, 1);
    ASSERT_EQ(0, r);
    close(pp[0]);

    int s;
    ASSERT_EQ(p, wait(&s));
    ASSERT_EQ(WIFEXITED(s), 1);
    ASSERT_EQ(WEXITSTATUS(s), 0);
}

static COW_IMPL(int, cow_int);

TEST(COWSingleThreaded) {
    cow_int = 69;

    pid_t p = fork();
    ASSERT_NEQ(p, -1);

    if (!p) {
        ASSERT_EQ(cow_int, 69);
        cow_int = 5;

        PHASE_SET(1);
        PHASE_WAIT(2);

        ASSERT_EQ(cow_int, 5);

        _exit(0);
    }

    PHASE_WAIT(1);

    ASSERT_EQ(cow_int, 69);
    cow_int = 420;

    PHASE_SET(2);

    ASSERT_EQ(cow_int, 420);

    int s;
    ASSERT_EQ(p, wait(&s));
    ASSERT_EQ(WIFEXITED(s), 1);
    ASSERT_EQ(WEXITSTATUS(s), 0);
}

static COW_IMPL(struct {
    int val;
    char *ptr;  
}, cow_deep);

TEST(COWDeepCopy) {
    cow_deep.val = 69;
    cow_deep.ptr = malloc(0x69);
    ASSERT_NEQ(NULL, cow_deep.ptr);
    memset(cow_deep.ptr, 'A', 0x69);

    pid_t p = fork();
    ASSERT_NEQ(p, -1);

    if (!p) {
        ASSERT_NEQ(cow_deep.ptr, NULL);
        for (int i = 0; i < 0x69; i++)
            ASSERT_EQ(cow_deep.ptr[i], 'A');
        
        PHASE_SET(1);
        PHASE_WAIT(2);

        for (int i = 0; i < 0x69; i++)
            ASSERT_EQ(cow_deep.ptr[i], 'A');

        _exit(0);
    }

    PHASE_WAIT(1);

    for (int i = 0; i < 0x69; i++)
        ASSERT_EQ(cow_deep.ptr[i], 'A');
    
    memset(cow_deep.ptr, 'B', 0x69);

    PHASE_SET(2);

    int s;
    ASSERT_EQ(p, wait(&s));
    ASSERT_EQ(WIFEXITED(s), 1);
    ASSERT_EQ(WEXITSTATUS(s), 0);
}

TEST(COWDeepCopyMidBufferUnaligned)
{
    char *ptr = malloc(0x69);
    ASSERT_NEQ(NULL, ptr);

    cow_deep.val = 69;
    cow_deep.ptr = ptr + 5;
    int i;
    for (i = 0; i < 10; i++)
        ptr[i] = 'A'+i;
    ptr[i] = 0;
    ASSERT_EQ(0, strcmp(cow_deep.ptr, "FGHIJ"));

    pid_t p = fork();
    ASSERT_NEQ(p, -1);

    if (!p) {
        ASSERT_NEQ(cow_deep.ptr, NULL);
        
        PHASE_WAIT(1);
        
        ASSERT_EQ(0, strcmp(cow_deep.ptr, "FGHIJ"));
        
        _exit(0);
    }

    for (i = 0; i < 10; i++)
        ptr[i] = '0' + i;

    PHASE_SET(1);

    int s;
    ASSERT_EQ(p, wait(&s));
    ASSERT_EQ(WIFEXITED(s), 1);
    ASSERT_EQ(WEXITSTATUS(s), 0);
}

TEST(COWDeepCopyInStack)
{
    char *ptr = malloc(0x69);
    int i;
    for (i = 0; i < 10; i++)
        ptr[i] = 'A'+i;
    ptr[i] = 0;

    ptr += 5;
    ASSERT_EQ(0, strcmp(ptr, "FGHIJ"));

    pid_t p = fork();
    ASSERT_NEQ(p, -1);

    if (!p) {
        ASSERT_NEQ(ptr, NULL);
        
        PHASE_WAIT(1);
        
        ASSERT_EQ(0, strcmp(ptr, "FGHIJ"));

        _exit(0);
    }

    for (i = 5; i < 10; i++)
        ptr[i] = '0' + i;

    PHASE_SET(1);

    int s;
    ASSERT_EQ(p, wait(&s));
    ASSERT_EQ(WIFEXITED(s), 1);
    ASSERT_EQ(WEXITSTATUS(s), 0);
}


extern char **environ;

TEST(EnvBasic) {
    for (char **e = environ, **te = tvm_environ; *e != NULL; e++, te++) {
        ASSERT_NEQ(NULL, *te);
        ASSERT_EQ(0, strcmp(*e, *te));
    }

    ASSERT_EQ(NULL, getenv("kd85n2lk2"));
    ASSERT_EQ(0, putenv("kd85n2lk2=1"));
    ASSERT_NEQ(NULL, getenv("kd85n2lk2"));
    ASSERT_EQ(0, strcmp("1", getenv("kd85n2lk2")));
    
    char **e;
    for (e = tvm_environ; *(e+1) != NULL; e++) { }
    ASSERT_EQ(0, strcmp(*e, "kd85n2lk2=1"));

    ASSERT_EQ(0, unsetenv("kd85n2lk2"));
    ASSERT_EQ(NULL, getenv("kd85n2lk2"));

    ASSERT_EQ(0, putenv("kd85n2lk2=2"));
    ASSERT_NEQ(NULL, getenv("kd85n2lk2"));
    ASSERT_EQ(0, strcmp("2", getenv("kd85n2lk2")));
    ASSERT_EQ(0, unsetenv("kd85n2lk2"));

    ASSERT_EQ(0, setenv("kd85n2lk2", "3", 0));
    ASSERT_NEQ(NULL, getenv("kd85n2lk2"));
    ASSERT_EQ(0, strcmp("3", getenv("kd85n2lk2")));

    ASSERT_EQ(0, setenv("kd85n2lk2", "4", 0));
    ASSERT_NEQ(NULL, getenv("kd85n2lk2"));
    ASSERT_EQ(0, strcmp("3", getenv("kd85n2lk2")));

    ASSERT_NEQ(NULL, tvm_environ[0]);
}

TEST(EnvFork) {
    ASSERT_EQ(NULL, getenv("kd85n2lk2"));
    ASSERT_EQ(0, putenv("kd85n2lk2=1"));
    ASSERT_EQ(0, strcmp(getenv("kd85n2lk2"), "1"));

    pid_t p = fork();
    ASSERT_NEQ(p, -1);

    if (!p) {
        tvm_environ[0] = "lol";

        ASSERT_NEQ(NULL, getenv("kd85n2lk2"));
        ASSERT_EQ(0, strcmp(getenv("kd85n2lk2"), "1"));

        ASSERT_EQ(0, setenv("kd85n2lk2", "2", 1));
        ASSERT_EQ(0, strcmp(getenv("kd85n2lk2"), "2"));
        
        PHASE_SET(1);
        PHASE_WAIT(2);
        
        ASSERT_EQ(0, strcmp(getenv("kd85n2lk2"), "2"));

        _exit(0);
    }

    PHASE_WAIT(1);

    ASSERT_EQ(0, strcmp(getenv("kd85n2lk2"), "1"));
    ASSERT_EQ(0, setenv("kd85n2lk2", "3", 1));
    ASSERT_NEQ(0, strcmp(tvm_environ[0], "lol"));

    PHASE_SET(2);

    int s;
    ASSERT_EQ(p, wait(&s));
    ASSERT_EQ(WIFEXITED(s), 1);
    ASSERT_EQ(WEXITSTATUS(s), 0);
}

TEST(EnvNULL) {
    ASSERT_NEQ(NULL, getenv("PATH"));
#if defined(__linux__)
    ASSERT_EQ(0, clearenv());
    ASSERT_EQ(tvm_environ, NULL);
#else
    tvm_environ = NULL;
#endif
    ASSERT_EQ(NULL, getenv("PATH"));
    ASSERT_EQ(0, unsetenv("PATH"));
    ASSERT_EQ(tvm_environ, NULL);

    ASSERT_EQ(0, putenv("hello=world"));
    ASSERT_NEQ(NULL, getenv("hello"));
    ASSERT_EQ(0, strcmp(getenv("hello"), "world"));
    ASSERT_NEQ(tvm_environ, NULL);
}

static int my_basic_prog()
{
    return 69;
}

TEST(ExecBasic) {
    ASSERT_EQ(-1, execl("/aaa", "/aaa", NULL));
    ASSERT_EQ(ENOENT, errno);

    tvm_register_program("/basic_prog", (main_func_t)my_basic_prog);
    pid_t p = fork();
    ASSERT_NEQ(p, -1);

    if (!p) {
        execl("/basic_prog", "/basic_prog", NULL);
        _exit(1);
    }

    int s;
    ASSERT_EQ(p, wait(&s));
    ASSERT_EQ(WIFEXITED(s), 1);
    ASSERT_EQ(WEXITSTATUS(s), 69);
}

static int my_args_prog(int argc, char **argv)
{
    if (argc != 2)
        return 2;
    
    if (0 != strcmp(argv[0], "arg0"))
        return 3;

    if (0 != strcmp(argv[1], "arg1"))
        return 4;
    
    if (NULL != argv[2])
        return 5;
    
    return 0;
}

TEST(ExecArgs) {
    tvm_register_program("/args_prog", (main_func_t)my_args_prog);
    pid_t p = fork();
    ASSERT_NEQ(p, -1);

    if (!p) {
        execl("/args_prog", "arg0", "arg1", NULL);
        _exit(1);
    }

    int s;
    ASSERT_EQ(p, wait(&s));
    ASSERT_EQ(WIFEXITED(s), 1);
    ASSERT_EQ(WEXITSTATUS(s), 0);
}

char *ptsname(int fd);

TEST(TTYBasic) {
    int master, slave;
	ASSERT_EQ(0, openpty(&master, &slave, NULL, NULL, NULL));
    ASSERT_EQ(1, isatty(master));
    ASSERT_EQ(1, isatty(slave));

    ASSERT_NEQ(NULL, ttyname(master));
    ASSERT_EQ(0, strcmp(ttyname(master), "/dev/ptmx"));

    ASSERT_NEQ(NULL, ttyname(slave));
    ASSERT_EQ(0, strncmp(ttyname(slave), "/tvm/pts/", 9));
    ASSERT_EQ(0, strncmp(ptsname(master), "/tvm/pts/", 9));
    ASSERT_EQ(0, strcmp(ptsname(master), ttyname(slave)));
}

TEST(TTYCloseMasterFork) {
    int master, slave;
	ASSERT_EQ(0, openpty(&master, &slave, NULL, NULL, NULL));
    ASSERT_EQ(1, isatty(slave));
    ASSERT_EQ(0, strncmp(ttyname(slave), "/tvm/pts/", 9));

    pid_t p = fork();
    ASSERT_NEQ(p, -1);
    if (!p) {
        ASSERT_EQ(1, isatty(slave));
        ASSERT_EQ(0, close(master));
        ASSERT_EQ(1, isatty(slave));
        exit(0);
    }
    ASSERT_EQ(p, wait(NULL));
    
    ASSERT_EQ(0, close(master));
    ASSERT_EQ(NULL, ttyname(slave));
    ASSERT_EQ(0, isatty(slave));
}

TEST(TTYCloseMasterDup2) {
    int master, slave;
	ASSERT_EQ(0, openpty(&master, &slave, NULL, NULL, NULL));
    
    int master2 = dup(master);
    ASSERT_NEQ(master2, -1);
    ASSERT_EQ(master, dup2(slave, master));
    ASSERT_EQ(1, isatty(master));
    ASSERT_EQ(master2, dup2(master, master2));
    ASSERT_EQ(0, isatty(master));
}

TEST(TTYCloseSlave) {
    int master, slave;
	ASSERT_EQ(0, openpty(&master, &slave, NULL, NULL, NULL));
    ASSERT_EQ(1, isatty(master));
    ASSERT_EQ(0, strncmp(ptsname(master), "/tvm/pts/", 9));

    ASSERT_EQ(0, close(slave));
    ASSERT_EQ(1, isatty(master));
    ASSERT_EQ(0, strncmp(ptsname(master), "/tvm/pts/", 9));

    ASSERT_NEQ(-1, open(ptsname(master), O_RDWR | O_NOCTTY));
}

static COW_IMPL_INIT(int, got_sig, 0);
static void sighup_handler(int signo) {
    got_sig++;
}

TEST(TTYDetachCloseMaster) {
    int master, slave;
	ASSERT_EQ(0, openpty(&master, &slave, NULL, NULL, NULL));

    struct sigaction act = { .sa_handler = sighup_handler };

    pid_t p = fork();
    ASSERT_NEQ(-1, p);

    if (!p) {
        ASSERT_EQ(0, sigaction(SIGHUP, &act, NULL));
        ASSERT_EQ(0, close(master));
        ASSERT_EQ(getpid(), setsid());
        ASSERT_EQ(0, ioctl(slave, TIOCSCTTY, 0));

        PHASE_SET(1);
        PHASE_WAIT(2);
        exit(got_sig-1);
    }

    PHASE_WAIT(1);

    ASSERT_EQ(0, close(master));

    PHASE_SET(2);

    int s;
    ASSERT_EQ(p, wait(&s));
    ASSERT_EQ(WIFEXITED(s), 1);
    ASSERT_EQ(WEXITSTATUS(s), 0);
}

TEST(TTYDetachTIOCNOTTY) {
    int master, slave;
	ASSERT_EQ(0, openpty(&master, &slave, NULL, NULL, NULL));

    struct sigaction act = { .sa_handler = sighup_handler };

    pid_t p = fork();
    ASSERT_NEQ(-1, p);

    if (!p) {
        ASSERT_EQ(0, sigaction(SIGHUP, &act, NULL));
        ASSERT_EQ(0, close(master));
        ASSERT_EQ(getpid(), setsid());
        ASSERT_EQ(0, ioctl(slave, TIOCSCTTY, 0));

        pid_t p2 = fork();
        ASSERT_NEQ(-1, p2);

        if (!p2) {
            PHASE_WAIT(1);
            exit(got_sig-1);
        }

        ASSERT_EQ(0, setpgid(p2, p2));
        ASSERT_EQ(0, tcsetpgrp(slave, p2));

        ASSERT_EQ(0, ioctl(slave, TIOCNOTTY, 0));

        PHASE_SET(1);

        int s;
        ASSERT_EQ(p2, wait(&s));
        ASSERT_EQ(WIFEXITED(s), 1);
        ASSERT_EQ(WEXITSTATUS(s), 0);

        exit(got_sig);
    }

    int s;
    ASSERT_EQ(p, wait(&s));
    ASSERT_EQ(WIFEXITED(s), 1);
    ASSERT_EQ(WEXITSTATUS(s), 0);
}

// =========================================================================
// Signal edge-case tests
// =========================================================================

TEST(SignalZombie) {
    // Use a pipe so we know when the child has exited (i.e. is now a zombie)
    int pip[2];
    ASSERT_EQ(0, pipe(pip));

    pid_t p = fork();
    ASSERT_NEQ(p, -1);
    if (!p) {
        close(pip[0]);
        close(pip[1]); // closing write end signals EOF to parent
        _exit(0);
    }
    close(pip[1]);

    // Read returns 0 (EOF) once child has exited and is zombie
    char c;
    read(pip[0], &c, 1);
    close(pip[0]);

    // Zombie: any real signal should fail with ESRCH
    ASSERT_EQ(-1, kill(p, SIGUSR1));
    ASSERT_EQ(ESRCH, errno);

    int s;
    ASSERT_EQ(p, wait(&s));
    ASSERT_EQ(WIFEXITED(s), 1);
    ASSERT_EQ(WEXITSTATUS(s), 0);
}

TEST(SignalIgnFork) {
    // Install SIG_IGN for SIGUSR1, then fork. Child should inherit SIG_IGN.
    struct sigaction act = { .sa_handler = SIG_IGN };
    ASSERT_EQ(0, sigaction(SIGUSR1, &act, NULL));

    pid_t p = fork();
    ASSERT_NEQ(p, -1);
    if (!p) {
        // Verify child inherited disposition
        struct sigaction old;
        ASSERT_EQ(0, sigaction(SIGUSR1, NULL, &old));
        ASSERT_EQ(SIG_IGN, old.sa_handler);

        // Raising an ignored signal: process survives
        ASSERT_EQ(0, raise(SIGUSR1));
        exit(0);
    }

    int s;
    ASSERT_EQ(p, wait(&s));
    ASSERT_EQ(WIFEXITED(s), 1);
    ASSERT_EQ(WEXITSTATUS(s), 0);
}

// Pipe written from signal handler to confirm each delivery (write is async-signal-safe)
static int sig_multi_pipe = -1;
static COW_IMPL_INIT(int, sig_multi_count, 0);
static void sig_multi_handler(int sig) {
    sig_multi_count++;
    char c = '!';
    write(sig_multi_pipe, &c, 1);
}

TEST(SignalMultiDeliver) {
    sig_multi_count = 0;

    int notify[2];
    ASSERT_EQ(0, pipe(notify));
    sig_multi_pipe = notify[1]; // handler writes here

    struct sigaction act = { .sa_handler = sig_multi_handler };
    ASSERT_EQ(0, sigaction(SIGUSR1, &act, NULL));

    int done[2];
    ASSERT_EQ(0, pipe(done));

    pid_t p = fork();
    ASSERT_NEQ(p, -1);
    if (!p) {
        close(notify[0]);
        close(done[1]);
        // Block until parent says done
        char c;
        while (read(done[0], &c, 1) == -1 && errno == EINTR)
            ;
        close(done[0]);
        exit(sig_multi_count > 0 ? 0 : 1);
    }
    close(notify[1]);
    close(done[0]);

    // Send 3 signals, waiting for each to be acknowledged via notify pipe
    for (int i = 0; i < 3; i++) {
        ASSERT_EQ(0, kill(p, SIGUSR1));
        char c;
        // read one byte per delivery — ensures signal was processed
        while (read(notify[0], &c, 1) == -1 && errno == EINTR)
            ;
    }
    close(notify[0]);

    write(done[1], "x", 1);
    close(done[1]);

    int s;
    ASSERT_EQ(p, wait(&s));
    ASSERT_EQ(WIFEXITED(s), 1);
    ASSERT_EQ(WEXITSTATUS(s), 0);
}

static COW_IMPL_INIT(int, sig_intr_count, 0);
static void sig_intr_handler(int sig) { sig_intr_count++; }

TEST(SignalInterruptRead) {
    sig_intr_count = 0;

    struct sigaction act = { .sa_handler = sig_intr_handler };
    ASSERT_EQ(0, sigaction(SIGUSR1, &act, NULL));

    // data: parent blocks reading here; child holds write end open until
    //       after sending SIGUSR1 so the pipe doesn't prematurely EOF.
    // sync: child signals it is about to sleep+send so parent can enter read().
    int data[2];
    int sync[2];
    ASSERT_EQ(0, pipe(data));
    ASSERT_EQ(0, pipe(sync));

    pid_t p = fork();
    ASSERT_NEQ(p, -1);
    if (!p) {
        close(data[0]);
        close(sync[0]);

        write(sync[1], "x", 1); // tell parent we're ready
        close(sync[1]);

        usleep(10000); // give parent time to enter read()
        kill(getppid(), SIGUSR1);
        usleep(5000);  // give signal time to be processed
        close(data[1]); // now EOF-close so parent isn't stuck if no EINTR
        _exit(0);
    }
    close(data[1]);
    close(sync[1]);

    char c;
    while (read(sync[0], &c, 1) == -1 && errno == EINTR)
        ;
    close(sync[0]);

    // Expect EINTR from SIGUSR1 interrupting this read
    char buf[1];
    int r = read(data[0], buf, 1);
    int saved_errno = errno;
    close(data[0]);

    ASSERT_EQ(-1, r);
    ASSERT_EQ(EINTR, saved_errno);
    ASSERT_EQ(1, sig_intr_count);

    int s;
    ASSERT_EQ(p, wait(&s));
}

TEST(SignalGrandchild) {
    int pip[2];
    ASSERT_EQ(0, pipe(pip));

    pid_t child = fork();
    ASSERT_NEQ(child, -1);
    if (!child) {
        close(pip[0]);
        pid_t grandchild = fork();
        ASSERT_NEQ(grandchild, -1);
        if (!grandchild) {
            close(pip[1]);
            pause(); // wait for signal
            _exit(0);
        }
        // Send grandchild PID to grandparent
        write(pip[1], &grandchild, sizeof(grandchild));
        close(pip[1]);
        // Reap grandchild after it's killed
        int s;
        ASSERT_EQ(grandchild, wait(&s));
        ASSERT_EQ(WIFSIGNALED(s), 1);
        ASSERT_EQ(WTERMSIG(s), SIGUSR2);
        exit(0);
    }
    close(pip[1]);

    pid_t grandchild;
    ASSERT_EQ((ssize_t)sizeof(grandchild), read(pip[0], &grandchild, sizeof(grandchild)));
    close(pip[0]);

    ASSERT_EQ(0, kill(grandchild, SIGUSR2));

    int s;
    ASSERT_EQ(child, wait(&s));
    ASSERT_EQ(WIFEXITED(s), 1);
    ASSERT_EQ(WEXITSTATUS(s), 0);
}

static COW_IMPL_INIT(int, sig_pgrp_count, 0);
static void sig_pgrp_handler(int sig) { sig_pgrp_count++; }

TEST(SignalKillZero) {
    sig_pgrp_count = 0;
    struct sigaction act = { .sa_handler = sig_pgrp_handler };
    ASSERT_EQ(0, sigaction(SIGUSR1, &act, NULL));

    // Pipe: child writes its count to parent after receiving signal
    int pip[2];
    ASSERT_EQ(0, pipe(pip));

    pid_t p = fork();
    ASSERT_NEQ(p, -1);
    if (!p) {
        // Stay in same process group as parent
        close(pip[0]);
        PHASE_WAIT(1);
        // Wait briefly for signal to arrive
        for (int i = 0; i < 50 && !sig_pgrp_count; i++)
            usleep(2000);
        int cnt = sig_pgrp_count;
        write(pip[1], &cnt, sizeof(cnt));
        close(pip[1]);
        _exit(0);
    }
    close(pip[1]);

    PHASE_SET(1);
    // Send to process group (both parent and child are in same pgrp)
    ASSERT_EQ(0, kill(0, SIGUSR1));

    int child_count;
    while (read(pip[0], &child_count, sizeof(child_count)) == -1 && errno == EINTR)
        ;
    close(pip[0]);

    ASSERT_EQ(1, sig_pgrp_count);
    ASSERT_EQ(1, child_count);

    int s;
    ASSERT_EQ(p, wait(&s));
}

static COW_IMPL_INIT(int, sig_fork_count, 0);
static void sig_fork_handler(int sig) { sig_fork_count++; }

TEST(SignalWhileFork) {
    sig_fork_count = 0;
    struct sigaction act = { .sa_handler = sig_fork_handler };
    ASSERT_EQ(0, sigaction(SIGUSR1, &act, NULL));

    // Pipe: child writes after sending signal so parent knows to check
    int pip[2];
    ASSERT_EQ(0, pipe(pip));

    pid_t p = fork();
    ASSERT_NEQ(p, -1);
    if (!p) {
        close(pip[0]);
        // Race a SIGUSR1 against the parent's fork return path
        kill(getppid(), SIGUSR1);
        write(pip[1], "x", 1);
        close(pip[1]);
        _exit(0);
    }
    close(pip[1]);

    // Wait for child to confirm it sent the signal
    char c;
    while (read(pip[0], &c, 1) == -1 && errno == EINTR)
        ;
    close(pip[0]);

    int s;
    ASSERT_EQ(p, wait(&s));
    ASSERT_EQ(WIFEXITED(s), 1);

    // Signal racing with fork should still be delivered to parent
    ASSERT_EQ(1, sig_fork_count);
}

// =========================================================================
// Stress / race tests
// =========================================================================

TEST(StressForkExit) {
    int N = 20;
    for (int i = 0; i < N; i++) {
        pid_t p = fork();
        ASSERT_NEQ(p, -1);
        if (!p)
            _exit(i % 64);
    }
    for (int i = 0; i < N; i++) {
        int s;
        pid_t p = wait(&s);
        ASSERT_NEQ(-1, p);
        ASSERT_EQ(WIFEXITED(s), 1);
    }
}

TEST(TTYSighupRace) {
    int master, slave;
    ASSERT_EQ(0, openpty(&master, &slave, NULL, NULL, NULL));

    pid_t p = fork();
    ASSERT_NEQ(-1, p);
    if (!p) {
        close(master);
        ASSERT_EQ(getpid(), setsid());
        ASSERT_EQ(0, ioctl(slave, TIOCSCTTY, 0));

        struct sigaction act = { .sa_handler = SIG_IGN };
        sigaction(SIGHUP, &act, NULL);

        PHASE_SET(1);

        // Poll for POLLIN/POLLHUP with a timeout so we don't hang if EIO
        // is not yet implemented — the important thing is we don't crash.
        struct pollfd pfd = { .fd = slave, .events = POLLIN | POLLHUP };
        poll(&pfd, 1, 3000); // 3s timeout

        exit(0);
    }

    PHASE_WAIT(1);
    usleep(5000); // ensure child is blocking in poll()
    close(master); // triggers SIGHUP + EIO/POLLHUP on slave

    int s;
    ASSERT_EQ(p, wait(&s));
    ASSERT_EQ(WIFEXITED(s), 1);
    ASSERT_EQ(WEXITSTATUS(s), 0);
}

static COW_IMPL_INIT(int, cc_count, 0);
static void cc_handler(int sig) { cc_count++; }

TEST(ConcurrentKills) {
    cc_count = 0;
    struct sigaction act = { .sa_handler = cc_handler };
    ASSERT_EQ(0, sigaction(SIGUSR1, &act, NULL));

    // Two pipes for PID exchange
    int p1pip[2], p2pip[2];
    ASSERT_EQ(0, pipe(p1pip));
    ASSERT_EQ(0, pipe(p2pip));

    pid_t p1 = fork();
    ASSERT_NEQ(p1, -1);
    if (!p1) {
        close(p1pip[1]); close(p2pip[0]); close(p2pip[1]);
        pid_t other;
        read(p1pip[0], &other, sizeof(other));
        close(p1pip[0]);
        kill(other, SIGUSR1);
        // Wait for reciprocal signal
        while (!cc_count) pause();
        _exit(cc_count == 1 ? 0 : 1);
    }

    pid_t p2 = fork();
    ASSERT_NEQ(p2, -1);
    if (!p2) {
        close(p2pip[1]); close(p1pip[0]); close(p1pip[1]);
        pid_t other;
        read(p2pip[0], &other, sizeof(other));
        close(p2pip[0]);
        kill(other, SIGUSR1);
        while (!cc_count) pause();
        _exit(cc_count == 1 ? 0 : 1);
    }

    close(p1pip[0]); close(p2pip[0]);

    // Give each child the other's PID
    write(p1pip[1], &p2, sizeof(p2));
    write(p2pip[1], &p1, sizeof(p1));
    close(p1pip[1]); close(p2pip[1]);

    int s1, s2;
    waitpid(p1, &s1, 0);
    waitpid(p2, &s2, 0);
    ASSERT_EQ(WIFEXITED(s1), 1);
    ASSERT_EQ(WEXITSTATUS(s1), 0);
    ASSERT_EQ(WIFEXITED(s2), 1);
    ASSERT_EQ(WEXITSTATUS(s2), 0);
}

// =========================================================================
// Multi-threading tests
// NOTE: exit() with >1 thread alive is NOT supported — always join first.
//       SIGALRM is not supported.
// =========================================================================

static void *thread_basic_fn(void *arg) {
    *(int *)arg = 42;
    return NULL;
}

TEST(ThreadBasic) {
    int val = 0;
    pthread_t thr;
    ASSERT_EQ(0, pthread_create(&thr, NULL, thread_basic_fn, &val));
    ASSERT_EQ(0, pthread_join(thr, NULL));
    ASSERT_EQ(42, val);
}

static volatile int thread_sig_flag = 0;
static void thread_sig_handler(int sig) { thread_sig_flag = 1; }

static void *thread_sig_worker(void *arg) {
    while (!thread_sig_flag)
        usleep(1000);
    return NULL;
}

TEST(ThreadSignal) {
    thread_sig_flag = 0;
    struct sigaction act = { .sa_handler = thread_sig_handler };
    ASSERT_EQ(0, sigaction(SIGUSR1, &act, NULL));

    pthread_t thr;
    ASSERT_EQ(0, pthread_create(&thr, NULL, thread_sig_worker, NULL));

    usleep(5000); // let worker start spinning
    ASSERT_EQ(0, raise(SIGUSR1));

    ASSERT_EQ(0, pthread_join(thr, NULL));
    ASSERT_EQ(1, thread_sig_flag);
}

static void *thread_exit_fn(void *arg) {
    usleep(10000);
    return NULL;
}

TEST(ThreadExitNoDeadlock) {
    // Thread exits while main holds a user mutex — verify no deadlock
    pthread_mutex_t m = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_lock(&m);

    pthread_t thr;
    ASSERT_EQ(0, pthread_create(&thr, NULL, thread_exit_fn, NULL));

    usleep(30000); // let thread run and exit
    pthread_mutex_unlock(&m);

    ASSERT_EQ(0, pthread_join(thr, NULL));
}

static void *thread_pid_fn(void *arg) {
    *(pid_t *)arg = getpid();
    return NULL;
}

TEST(ThreadPidShared) {
    pid_t thread_pid = 0;
    pthread_t thr;
    ASSERT_EQ(0, pthread_create(&thr, NULL, thread_pid_fn, &thread_pid));
    ASSERT_EQ(0, pthread_join(thr, NULL));
    // All threads of the same TVM task share the same pid
    ASSERT_EQ(getpid(), thread_pid);
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
    tvm_init(testinfo->n);
    testinfo->f();
    return 0;
}

static struct testinfo_t *g_testinfo = NULL;

static void sigint_handler(int sig) {
    if (!g_testinfo) {
        printf("\n======== KEYBOARD INTERRUPT ========\n");    
        exit(-sig);
    }

    printf("\n======== KEYBOARD INTERRUPT: %s ========\n", g_testinfo->n);
    if (g_testinfo->out_s) {
        printf("\n==== captured stdout ====\n");
        while (g_testinfo->out_s) {
            size_t w = write(1, g_testinfo->out, g_testinfo->out_s);
            if (w == -1 || 0 == w) {
                perror("write(out)");
                break;
            }
            g_testinfo->out_s -= w;
           g_testinfo->out = (void *) (((char *) g_testinfo->out) + w);
        }
    }
    if (g_testinfo->err_s) {
        printf("\n==== captured stderr ====\n");
        while (g_testinfo->err_s) {
            size_t w = write(1, g_testinfo->err, g_testinfo->err_s);
            if (w == -1 || 0 == w) {
                perror("write(err)");
                break;
            }
            g_testinfo->err_s -= w;
            g_testinfo->err = (void *) (((char *) g_testinfo->err) + w);
        }
    }
    exit(-sig);
}

static int runtest(struct testinfo_t *testinfo, int verbose)
{
    int outp[2] = {-1, -1};
    if (-1 == pipe(outp)) {
        perror("test_tvm: pipe");
        abort();
    }

    int errp[2] = {-1, -1};
    if (-1 == pipe(errp)) {
        perror("test_tvm: pipe");
        abort();
    }

    if (-1 == fcntl(outp[0], F_SETFL, O_NONBLOCK) || -1 == fcntl(errp[0], F_SETFL, O_NONBLOCK)) {
        perror("test_tvm: fcntl");
        abort();
    }

    struct sigaction act, oldact;
    act.sa_handler = sigint_handler;
    sigemptyset(&act.sa_mask);
    act.sa_flags = 0;
    g_testinfo = testinfo;
    if (-1 == sigaction(SIGINT, &act, &oldact)) {
        perror("test_tvm: sigaction");
        abort();
    }

    pid_t p = fork();
    if (p == -1) {
        perror("test_tvm: fork");
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
            perror("test_tvm: set sid");
            exit(1);
        }

        tvm_init(testinfo->n);
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
            perror("test_tvm: usleep");
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
            if (verbose >= 2)
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
            if (verbose >= 2) {
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
            perror("test_tvm: waitpid");
            abort();
        }

        if (a == p) {
            break;
        }

        if (a != 0) {
            fprintf(stderr, "test_tvm: waitpid: unknown pid %d (want %d)\n", a, p);
            abort();
        }
    }

    close(outp[1]);
    close(errp[1]);

    if (-1 == sigaction(SIGINT, &oldact, NULL)) {
        perror("test_tvm: sigaction restore");
        abort();
    }
    g_testinfo = NULL;

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

    if (!in_gdb) {
        printf("tvm ");
        if (verbose)
            printf("\n\n");
    }
        
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
            if (verbose) {
                printf("%s ", tests[i].n);
                fflush(stdout);
            }
            r = runtest(tests + i, verbose);
            if (r) {
                printf((r>0)?"F":"C");
                break;
            }
            else {
                printf(".");
            }
            if (verbose) {
                printf("\n");
            }
            fflush(stdout);
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