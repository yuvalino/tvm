# TVM Signal Implementation TODO

## Currently Implemented
| Function | Status | Notes |
|----------|--------|-------|
| `sigaction` | Done | Stores in `tsk_sighandlers[]`, no OS sigaction call |
| `signal` | Done | Wrapper around sigaction, sets SA_RESTART |
| `kill` | Done | Walks task list, handles pid/pgid targeting |
| `raise` | Done | `pthread_kill(pthread_self(), sig)` |
| `abort` | Done | `raise(SIGABRT)` + `_exit(1)` |
| `timerfd_create` | Done | FD wrapping only, no virtual timer |

## Architecture: Per-Task Virtual Signal Mask

Key missing piece: `tsk_sigmask` (sigset_t) per task.

Currently `signal_handler` always fires (OS mask is unblocked) and checks
`tvm_sig_blocked` (internal refcount). Need a second layer:

1. `sigprocmask`/`pthread_sigmask` modify `tsk_sigmask` (not the OS mask)
2. `signal_handler` checks `tsk_sigmask` to decide deliver vs defer
3. OS mask stays fully unblocked so `signal_handler` always fires
4. Blocking waiters (`sigsuspend`, `pause`, `sigwait`) use condvar woken
   by `signal_dispatch` when a matching signal arrives

This mirrors real kernel architecture: hardware mask vs per-process mask.

## Fork Signal Safety

Both `fork()` and `pthread_create()` have a window where the child OS thread
runs with `current == NULL`. Any signal delivered during this window panics.

Fix: block all OS signals before `pthread_create()`, restore in parent after,
restore saved mask in child after `current` is set and thread is registered.

Also: `fork()` currently has no `tvm_sig_enter()`/`tvm_sig_leave()`.

## Critical — Need Override

### Signal Mask Manipulation
| Function | Priority | Why |
|----------|----------|-----|
| `sigprocmask` | CRITICAL | Must maintain per-task `tsk_sigmask`, not change real OS mask |
| `pthread_sigmask` | CRITICAL | Same, but explicitly thread-scoped |

### Signal Waiting (all blocking — cannot use SIG_GUARD)
| Function | Priority | Why |
|----------|----------|-----|
| `sigsuspend` | CRITICAL | Atomically swap mask + wait for virtual signal |
| `pause` | CRITICAL | Wait for any virtual signal |
| `sigwait` | CRITICAL | Synchronously consume signal from virtual queue |
| `sigwaitinfo` | CRITICAL | Like sigwait, returns siginfo_t |
| `sigtimedwait` | CRITICAL | Like sigwaitinfo with timeout |

### Signal Sending (missing)
| Function | Priority | Why |
|----------|----------|-----|
| `killpg` | CRITICAL | Process group kill; trivial via `kill(-pgrp, sig)` |
| `pthread_kill` | CRITICAL | Thread-directed signal; user calls need TVM thread ID mapping |

### Timers
| Function | Priority | Why |
|----------|----------|-----|
| `alarm` | CRITICAL | Per-process SIGALRM timer |
| `setitimer` | CRITICAL | ITIMER_REAL/VIRTUAL/PROF timers |

## Nice-to-Have
| Function | Why |
|----------|-----|
| `sigpending` | Return `tsk_pending_sigs \| thr_pending_sigs` |
| `sigaltstack` | Per-task alternate signal stack tracking |
| `getitimer` | Query counterpart to setitimer |
| `timer_create` / `timer_settime` | Modern POSIX timer API |
| `signalfd` | Linux-specific signal-to-fd interface |
| `sigqueue` | Real-time signal with data payload |

## Legacy/Optional
`sigblock`, `sigsetmask`, `sigvec`, `sigset`, `sighold`, `sigrelse`,
`sigignore`, `sigpause`, `siginterrupt`, `tgkill`, `tkill`
