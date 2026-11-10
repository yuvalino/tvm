/*
 * TVM's own mutexes are counted per thread: a thread waiting in one of the
 * host's wait traps is torn down from the signal handler when its task
 * exits (tvm.c, thread_in_kernel_wait), which is only safe when it holds
 * none of TVM's locks and waits for none, or they stay held (or the mutex's
 * waiter state wrong) for every task after it. tvm.c, ulinker.c and rmem.c
 * include this after their other headers, so every pthread_mutex_lock of
 * theirs is counted; a program's own locks are the host's, uncounted, and
 * go with its task. tvm.c defines the functions; rmem.c has plain defaults
 * for the tests that link it alone (test_rmem).
 */
#ifndef TVM_MUTEX_H
#define TVM_MUTEX_H

#include <pthread.h>

int tvm_mutex_lock(pthread_mutex_t *m) __attribute__((visibility("hidden")));
int tvm_mutex_unlock(pthread_mutex_t *m) __attribute__((visibility("hidden")));

#define pthread_mutex_lock(m)   tvm_mutex_lock(m)
#define pthread_mutex_unlock(m) tvm_mutex_unlock(m)

#endif
