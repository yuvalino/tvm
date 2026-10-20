/*
 * fenv.h for TVM's clang toolchain bundle (written for TVM; see
 * scripts/sdk/build_sdk.sh): the C standard's floating-point environment
 * (C99/C11, 7.6) for arm64, whose types and values are the architecture's
 * and Darwin's ABI: the environment is the FPSR and FPCR registers, the
 * exception flags their bits in the FPSR (and the cumulative input-denormal
 * flag, which Darwin calls FE_FLUSHTOZERO), the rounding modes the FPCR's
 * RMode field. The functions are the system's libm.
 */
#ifndef __FENV_H__
#define __FENV_H__

#include <sys/cdefs.h>

#if !defined(__arm64__) && !defined(__aarch64__)
#error "this fenv.h is for arm64"
#endif

typedef struct {
    unsigned long long __fpsr;
    unsigned long long __fpcr;
} fenv_t;

typedef unsigned short fexcept_t;

#define FE_INVALID      0x0001
#define FE_DIVBYZERO    0x0002
#define FE_OVERFLOW     0x0004
#define FE_UNDERFLOW    0x0008
#define FE_INEXACT      0x0010
#define FE_FLUSHTOZERO  0x0080
#define FE_ALL_EXCEPT   (FE_INVALID | FE_DIVBYZERO | FE_OVERFLOW | FE_UNDERFLOW | FE_INEXACT | FE_FLUSHTOZERO)

#define FE_TONEAREST    0x00000000
#define FE_UPWARD       0x00400000
#define FE_DOWNWARD     0x00800000
#define FE_TOWARDZERO   0x00C00000

__BEGIN_DECLS

extern const fenv_t _FE_DFL_ENV;
#define FE_DFL_ENV (&_FE_DFL_ENV)

extern int feclearexcept(int);
extern int fegetexceptflag(fexcept_t *, int);
extern int feraiseexcept(int);
extern int fesetexceptflag(const fexcept_t *, int);
extern int fetestexcept(int);
extern int fegetround(void);
extern int fesetround(int);
extern int fegetenv(fenv_t *);
extern int feholdexcept(fenv_t *);
extern int fesetenv(const fenv_t *);
extern int feupdateenv(const fenv_t *);

__END_DECLS

#endif /* __FENV_H__ */
