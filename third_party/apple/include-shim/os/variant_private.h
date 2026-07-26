/*
 * Trimmed stand-in for <os/variant_private.h>.
 *
 * The real Libc copy is annotated with API_UNAVAILABLE(ios, tvos, watchos,
 * bridgeos), and `bridgeos` is no longer a platform token the current SDK's
 * <os/availability.h> accepts — so the upstream header will not parse at all.
 * diskdev_cmds only #includes it (mount.c, fsck.c) without calling into it, so
 * the declarations are reproduced here without the availability attributes.
 */

#ifndef _TVM_SHIM_OS_VARIANT_PRIVATE_H
#define _TVM_SHIM_OS_VARIANT_PRIVATE_H

#include <stdbool.h>
#include <sys/cdefs.h>

__BEGIN_DECLS

extern bool os_variant_has_internal_content(const char *subsystem);
extern bool os_variant_has_internal_diagnostics(const char *subsystem);
extern bool os_variant_has_internal_ui(const char *subsystem);
extern bool os_variant_allows_internal_security_policies(const char *subsystem);
extern bool os_variant_is_darwinos(const char *subsystem);
extern bool os_variant_is_recovery(const char *subsystem);
extern bool os_variant_is_basesystem(const char *subsystem);
extern bool os_variant_uses_ephemeral_storage(const char *subsystem);

__END_DECLS

#endif /* _TVM_SHIM_OS_VARIANT_PRIVATE_H */
