/*
 * Force-included (-include) into every toybox translation unit on iOS.
 *
 * The iPhoneOS SDK marks system(3) unavailable at compile time
 * (__IOS_PROHIBITED), so toys/pending/awk.c, whose awk system() builtin
 * calls it, fails to compile. The symbol itself is still exported by the
 * SDK's libSystem stub, and inside TVM the call is interposed anyway. Bind
 * the call to that symbol through a fresh declaration, which carries no
 * availability attribute.
 */
#ifndef TVM_IOS_LIBC_H
#define TVM_IOS_LIBC_H

#include <stdlib.h>

extern int tvm_ios_system(const char *) __asm__("_system");
#define system tvm_ios_system

#endif
