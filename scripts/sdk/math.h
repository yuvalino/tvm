/*
 * math.h for TVM's clang toolchain bundle (written for TVM; see
 * scripts/sdk/build_sdk.sh): the C standard's <math.h> (C99/C11, 7.12) and
 * the BSD and Darwin additions programs use, declaring what the system's
 * libm (libsystem_m, in libSystem) defines. Apple no longer publishes the
 * header (Libm's last open-source math.h is a 2002 one); the classification
 * and comparison macros here are clang's type-generic builtins, and the
 * FP_* values are Darwin's ABI.
 */
#ifndef __MATH_H__
#define __MATH_H__
#ifndef __MATH__
#define __MATH__
#endif

#include <sys/cdefs.h>

__BEGIN_DECLS

/* ── types ── */

#if !defined(__FLT_EVAL_METHOD__) || __FLT_EVAL_METHOD__ == 0 || __FLT_EVAL_METHOD__ == -1 || __FLT_EVAL_METHOD__ == 16
typedef float       float_t;
typedef double      double_t;
#elif __FLT_EVAL_METHOD__ == 1
typedef double      float_t;
typedef double      double_t;
#else
typedef long double float_t;
typedef long double double_t;
#endif

/* ── constants ── */

#define HUGE_VAL    __builtin_huge_val()
#define HUGE_VALF   __builtin_huge_valf()
#define HUGE_VALL   __builtin_huge_vall()
#define INFINITY    __builtin_inff()
#define NAN         __builtin_nanf("0x7fc00000")

#define FP_NAN          1
#define FP_INFINITE     2
#define FP_ZERO         3
#define FP_NORMAL       4
#define FP_SUBNORMAL    5
#define FP_SUPERNORMAL  6

#if defined(__FP_FAST_FMA)
#define FP_FAST_FMA     1
#endif
#if defined(__FP_FAST_FMAF)
#define FP_FAST_FMAF    1
#endif
#if defined(__FP_FAST_FMAL)
#define FP_FAST_FMAL    1
#endif

#define FP_ILOGB0       (-2147483647 - 1)
#define FP_ILOGBNAN     (-2147483647 - 1)

#define MATH_ERRNO      1
#define MATH_ERREXCEPT  2
/* Darwin's libm reports errors by the floating-point exceptions only */
#define math_errhandling MATH_ERREXCEPT

/* ── classification and comparison (type-generic) ── */

#define fpclassify(x)   __builtin_fpclassify(FP_NAN, FP_INFINITE, FP_NORMAL, FP_SUBNORMAL, FP_ZERO, (x))
#define isfinite(x)     __builtin_isfinite(x)
#define isinf(x)        __builtin_isinf(x)
#define isnan(x)        __builtin_isnan(x)
#define isnormal(x)     __builtin_isnormal(x)
#define signbit(x)      __builtin_signbit(x)

#define isgreater(x, y)         __builtin_isgreater((x), (y))
#define isgreaterequal(x, y)    __builtin_isgreaterequal((x), (y))
#define isless(x, y)            __builtin_isless((x), (y))
#define islessequal(x, y)       __builtin_islessequal((x), (y))
#define islessgreater(x, y)     __builtin_islessgreater((x), (y))
#define isunordered(x, y)       __builtin_isunordered((x), (y))

/* ── functions (7.12.4-7.12.13), each with its float and long double kin ── */

#define __TVM_MATH1(name)  extern double name(double); extern float name##f(float); extern long double name##l(long double);
#define __TVM_MATH2(name)  extern double name(double, double); extern float name##f(float, float); extern long double name##l(long double, long double);

__TVM_MATH1(acos)   __TVM_MATH1(asin)   __TVM_MATH1(atan)   __TVM_MATH2(atan2)
__TVM_MATH1(cos)    __TVM_MATH1(sin)    __TVM_MATH1(tan)
__TVM_MATH1(acosh)  __TVM_MATH1(asinh)  __TVM_MATH1(atanh)
__TVM_MATH1(cosh)   __TVM_MATH1(sinh)   __TVM_MATH1(tanh)
__TVM_MATH1(exp)    __TVM_MATH1(exp2)   __TVM_MATH1(expm1)
__TVM_MATH1(log)    __TVM_MATH1(log10)  __TVM_MATH1(log2)   __TVM_MATH1(log1p)  __TVM_MATH1(logb)
__TVM_MATH1(cbrt)   __TVM_MATH1(sqrt)   __TVM_MATH1(fabs)   __TVM_MATH2(hypot)  __TVM_MATH2(pow)
__TVM_MATH1(erf)    __TVM_MATH1(erfc)   __TVM_MATH1(lgamma) __TVM_MATH1(tgamma)
__TVM_MATH1(ceil)   __TVM_MATH1(floor)  __TVM_MATH1(nearbyint) __TVM_MATH1(rint)
__TVM_MATH1(round)  __TVM_MATH1(trunc)
__TVM_MATH2(fmod)   __TVM_MATH2(remainder) __TVM_MATH2(copysign) __TVM_MATH2(nextafter)
__TVM_MATH2(fdim)   __TVM_MATH2(fmax)   __TVM_MATH2(fmin)

#undef __TVM_MATH1
#undef __TVM_MATH2

extern double      frexp(double, int *);
extern float       frexpf(float, int *);
extern long double frexpl(long double, int *);
extern double      ldexp(double, int);
extern float       ldexpf(float, int);
extern long double ldexpl(long double, int);
extern double      modf(double, double *);
extern float       modff(float, float *);
extern long double modfl(long double, long double *);
extern double      scalbn(double, int);
extern float       scalbnf(float, int);
extern long double scalbnl(long double, int);
extern double      scalbln(double, long);
extern float       scalblnf(float, long);
extern long double scalblnl(long double, long);
extern int         ilogb(double);
extern int         ilogbf(float);
extern int         ilogbl(long double);
extern long        lrint(double);
extern long        lrintf(float);
extern long        lrintl(long double);
extern long long   llrint(double);
extern long long   llrintf(float);
extern long long   llrintl(long double);
extern long        lround(double);
extern long        lroundf(float);
extern long        lroundl(long double);
extern long long   llround(double);
extern long long   llroundf(float);
extern long long   llroundl(long double);
extern double      remquo(double, double, int *);
extern float       remquof(float, float, int *);
extern long double remquol(long double, long double, int *);
extern double      nan(const char *);
extern float       nanf(const char *);
extern long double nanl(const char *);
extern double      nexttoward(double, long double);
extern float       nexttowardf(float, long double);
extern long double nexttowardl(long double, long double);
extern double      fma(double, double, double);
extern float       fmaf(float, float, float);
extern long double fmal(long double, long double, long double);

/* ── Darwin's additions ── */

/* 10 to the x; pi times x; the sine and cosine at once (which the compiler
   itself also calls, for sin(x) and cos(x) of one x) */
extern float  __exp10f(float);
extern double __exp10(double);
extern float  __cospif(float);
extern double __cospi(double);
extern float  __sinpif(float);
extern double __sinpi(double);
extern float  __tanpif(float);
extern double __tanpi(double);
struct __float2  { float  __sinval; float  __cosval; };
struct __double2 { double __sinval; double __cosval; };
extern struct __float2  __sincosf_stret(float);
extern struct __double2 __sincos_stret(double);
extern struct __float2  __sincospif_stret(float);
extern struct __double2 __sincospi_stret(double);

/* ── X/Open and BSD ── */

#if !defined(_ANSI_SOURCE) && (!defined(_POSIX_C_SOURCE) || defined(_DARWIN_C_SOURCE) || defined(_XOPEN_SOURCE))
#define M_E         2.71828182845904523536028747135266250   /* e */
#define M_LOG2E     1.44269504088896340735992468100189214   /* log2(e) */
#define M_LOG10E    0.434294481903251827651128918916605082  /* log10(e) */
#define M_LN2       0.693147180559945309417232121458176568  /* ln(2) */
#define M_LN10      2.30258509299404568401799145468436421   /* ln(10) */
#define M_PI        3.14159265358979323846264338327950288   /* pi */
#define M_PI_2      1.57079632679489661923132169163975144   /* pi/2 */
#define M_PI_4      0.785398163397448309615660845819875721  /* pi/4 */
#define M_1_PI      0.318309886183790671537767526745028724  /* 1/pi */
#define M_2_PI      0.636619772367581343075535053490057448  /* 2/pi */
#define M_2_SQRTPI  1.12837916709551257389615890312154517   /* 2/sqrt(pi) */
#define M_SQRT2     1.41421356237309504880168872420969808   /* sqrt(2) */
#define M_SQRT1_2   0.707106781186547524400844362104849039  /* 1/sqrt(2) */

#define MAXFLOAT    3.40282346638528859811704183484516925e+38F

extern int signgam;

/* Bessel functions of the first and second kinds */
extern double j0(double);
extern double j1(double);
extern double jn(int, double);
extern double y0(double);
extern double y1(double);
extern double yn(int, double);
extern double scalb(double, double);
#endif

#if !defined(_ANSI_SOURCE) && (!defined(_POSIX_C_SOURCE) || defined(_DARWIN_C_SOURCE))
extern double      lgamma_r(double, int *);
extern float       lgammaf_r(float, int *);
extern long double lgammal_r(long double, int *);
#endif

__END_DECLS

#endif /* __MATH_H__ */
