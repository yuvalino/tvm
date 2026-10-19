/*
 * TargetConditionals.h for TVM's clang toolchain bundle (written for TVM;
 * see scripts/sdk/build_sdk.sh). The TARGET_* macros the system headers
 * test, each 0 or 1, answered from what the compiler knows of the target:
 * its OS and environment (clang's __is_target_os, __is_target_environment)
 * and its architecture. The Apple SDK's own copy is not redistributable;
 * this one only gives the documented meaning of each name.
 */
#ifndef __TARGETCONDITIONALS__
#define __TARGETCONDITIONALS__

#if !defined(__is_target_os)
#define __is_target_os(x) 0
#endif
#if !defined(__is_target_environment)
#define __is_target_environment(x) 0
#endif

/* ── operating system ── */

/* Mac Catalyst: iOS code run on macOS (the macabi environment of ios) */
#if __is_target_os(ios) && __is_target_environment(macabi)
#define TARGET_OS_MACCATALYST   1
#else
#define TARGET_OS_MACCATALYST   0
#endif
#define TARGET_OS_UIKITFORMAC   TARGET_OS_MACCATALYST

#if __is_target_os(macos)
#define TARGET_OS_OSX           1
#else
#define TARGET_OS_OSX           0
#endif

#if __is_target_os(ios)
#define TARGET_OS_IOS           1
#else
#define TARGET_OS_IOS           0
#endif

#if __is_target_os(tvos)
#define TARGET_OS_TV            1
#else
#define TARGET_OS_TV            0
#endif

#if __is_target_os(watchos)
#define TARGET_OS_WATCH         1
#else
#define TARGET_OS_WATCH         0
#endif

#if __is_target_os(xros) || __is_target_os(visionos)
#define TARGET_OS_VISION        1
#else
#define TARGET_OS_VISION        0
#endif
#define TARGET_OS_XR            TARGET_OS_VISION

#if __is_target_os(bridgeos)
#define TARGET_OS_BRIDGE        1
#else
#define TARGET_OS_BRIDGE        0
#endif

#if __is_target_os(driverkit)
#define TARGET_OS_DRIVERKIT     1
#else
#define TARGET_OS_DRIVERKIT     0
#endif

/* the iPhone family: iOS and the systems derived from it (Catalyst is iOS) */
#if TARGET_OS_IOS || TARGET_OS_TV || TARGET_OS_WATCH || TARGET_OS_VISION || TARGET_OS_BRIDGE
#define TARGET_OS_IPHONE        1
#else
#define TARGET_OS_IPHONE        0
#endif

/* any Darwin system */
#if defined(__APPLE__) && defined(__MACH__)
#define TARGET_OS_MAC           1
#define TARGET_OS_DARWIN        1
#else
#define TARGET_OS_MAC           0
#define TARGET_OS_DARWIN        0
#endif

/* a device rather than a Mac or a simulator */
#if TARGET_OS_IPHONE && !TARGET_OS_MACCATALYST && !__is_target_environment(simulator)
#define TARGET_OS_EMBEDDED      1
#else
#define TARGET_OS_EMBEDDED      0
#endif

#if __is_target_environment(simulator)
#define TARGET_OS_SIMULATOR     1
#else
#define TARGET_OS_SIMULATOR     0
#endif
#define TARGET_IPHONE_SIMULATOR TARGET_OS_SIMULATOR

#define TARGET_OS_NANO          TARGET_OS_WATCH

/* the secure-world environments, which nothing here targets */
#define TARGET_OS_EXCLAVECORE   0
#define TARGET_OS_EXCLAVEKIT    0

/* not Darwin */
#define TARGET_OS_WIN32         0
#define TARGET_OS_WINDOWS       0
#define TARGET_OS_UNIX          0
#define TARGET_OS_LINUX         0

/* ── processor ── */

#if defined(__arm64__) || defined(__aarch64__)
#define TARGET_CPU_ARM64        1
#else
#define TARGET_CPU_ARM64        0
#endif
#if defined(__arm__) && !TARGET_CPU_ARM64
#define TARGET_CPU_ARM          1
#else
#define TARGET_CPU_ARM          0
#endif
#if defined(__x86_64__)
#define TARGET_CPU_X86_64       1
#else
#define TARGET_CPU_X86_64       0
#endif
#if defined(__i386__)
#define TARGET_CPU_X86          1
#else
#define TARGET_CPU_X86          0
#endif
#define TARGET_CPU_PPC          0
#define TARGET_CPU_PPC64        0
#define TARGET_CPU_68K          0
#define TARGET_CPU_MIPS         0
#define TARGET_CPU_SPARC        0
#define TARGET_CPU_ALPHA        0

/* ── runtime ── */

#if defined(__BIG_ENDIAN__) || (defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__)
#define TARGET_RT_LITTLE_ENDIAN 0
#define TARGET_RT_BIG_ENDIAN    1
#else
#define TARGET_RT_LITTLE_ENDIAN 1
#define TARGET_RT_BIG_ENDIAN    0
#endif
#if defined(__LP64__)
#define TARGET_RT_64_BIT        1
#else
#define TARGET_RT_64_BIT        0
#endif
#define TARGET_RT_MAC_CFM       0
#define TARGET_RT_MAC_MACHO     1

/* where the macOS and iOS ABIs differ, everything but an Intel Mac
   (Catalyst included) has iOS's values */
#if !TARGET_CPU_X86_64 || (TARGET_OS_IPHONE && !TARGET_OS_MACCATALYST)
#define TARGET_ABI_USES_IOS_VALUES 1
#else
#define TARGET_ABI_USES_IOS_VALUES 0
#endif

#endif /* __TARGETCONDITIONALS__ */
