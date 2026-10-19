/*
 * complex.h for TVM's clang toolchain bundle (written for TVM; see
 * scripts/sdk/build_sdk.sh): the C standard's complex arithmetic (C99/C11,
 * 7.3), declaring what the system's libm defines.
 */
#ifndef __COMPLEX_H__
#define __COMPLEX_H__

#include <sys/cdefs.h>

#define complex     _Complex
#define _Complex_I  (__extension__ 1.0iF)
#define I           _Complex_I

#define CMPLX(x, y)     __builtin_complex((double)(x), (double)(y))
#define CMPLXF(x, y)    __builtin_complex((float)(x), (float)(y))
#define CMPLXL(x, y)    __builtin_complex((long double)(x), (long double)(y))

__BEGIN_DECLS

#define __TVM_CPLX1(name) \
    extern double _Complex name(double _Complex); \
    extern float _Complex name##f(float _Complex); \
    extern long double _Complex name##l(long double _Complex);
#define __TVM_CPLXR(name) \
    extern double name(double _Complex); \
    extern float name##f(float _Complex); \
    extern long double name##l(long double _Complex);

__TVM_CPLX1(cacos)  __TVM_CPLX1(casin)  __TVM_CPLX1(catan)
__TVM_CPLX1(ccos)   __TVM_CPLX1(csin)   __TVM_CPLX1(ctan)
__TVM_CPLX1(cacosh) __TVM_CPLX1(casinh) __TVM_CPLX1(catanh)
__TVM_CPLX1(ccosh)  __TVM_CPLX1(csinh)  __TVM_CPLX1(ctanh)
__TVM_CPLX1(cexp)   __TVM_CPLX1(clog)   __TVM_CPLX1(csqrt)
__TVM_CPLX1(conj)   __TVM_CPLX1(cproj)
__TVM_CPLXR(cabs)   __TVM_CPLXR(carg)   __TVM_CPLXR(cimag)  __TVM_CPLXR(creal)

#undef __TVM_CPLX1
#undef __TVM_CPLXR

extern double _Complex      cpow(double _Complex, double _Complex);
extern float _Complex       cpowf(float _Complex, float _Complex);
extern long double _Complex cpowl(long double _Complex, long double _Complex);

__END_DECLS

#endif /* __COMPLEX_H__ */
