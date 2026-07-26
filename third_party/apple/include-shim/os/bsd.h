/*
 * Hand-written stand-in for <os/bsd.h>.
 *
 * Unlike everything under include/, this header has no open-source original —
 * os/bsd.h ships only in Apple's internal SDK. The functions it declares do
 * exist in libSystem on both macOS and iOS, so declaring them here is enough to
 * build against them; only the declarations were ever missing.
 *
 * Used by diskdev_cmds (fsck, mount, edt_fstab) to read boot-args. Under TVM
 * these calls simply return false — a sandboxed guest has no boot-args — which
 * is the same thing that happens on a normal userland install.
 */

#ifndef _TVM_SHIM_OS_BSD_H
#define _TVM_SHIM_OS_BSD_H

#include <stdbool.h>
#include <stdint.h>
#include <sys/cdefs.h>

__BEGIN_DECLS

/*
 * Look up `arg_string` in the kernel boot-args. On success copies the value
 * into `buf` (NUL-terminated, truncated to `size`) and returns true.
 */
extern bool os_parse_boot_arg_string(const char *arg_string, char *buf, size_t size);

/* Integer form of the above; writes through `val` on success. */
extern bool os_parse_boot_arg_int(const char *arg_string, int64_t *val);

__END_DECLS

#endif /* _TVM_SHIM_OS_BSD_H */
