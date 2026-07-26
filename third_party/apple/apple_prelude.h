/*
 * Force-included (-include) into every apple *_cmds translation unit.
 *
 * Two different problems are being solved here, and they need different fixes:
 *
 *  1. Headers the SDK omits entirely (libproc_private.h, sys/stackshot.h,
 *     System/sys/proc.h, ...). Those are handled purely by -idirafter against
 *     third_party/apple/include — the source's own #include finds them.
 *
 *  2. Headers the SDK *does* ship, but with the PRIVATE-guarded sections
 *     stripped out. <sys/socket.h> is the canonical case: the public copy has
 *     no SO_TC_CTL, and -idirafter cannot help because the SDK copy legitimately
 *     wins the lookup. Apple's internal SDK installs these as one combined
 *     header; the open-source tree splits the private half into a separate
 *     *_private.h. So we pull the private halves in ourselves, after the public
 *     ones, which is exactly what the internal SDK would have given the source.
 *
 * Keep this limited to declaration-only headers — it lands in every TU.
 */

#ifndef TVM_APPLE_PRELUDE_H
#define TVM_APPLE_PRELUDE_H

/*
 * __APPLE_USE_RFC_3542 is deliberately NOT set here. It selects between two
 * mutually exclusive halves of <netinet6/in6.h>: with it you get IPV6_PKTINFO
 * and friends (rtsol wants those), without it you get the RFC2292 aliases
 * IPV6_PKTOPTIONS/IPV6_HOPOPTS/... (ping6 and traceroute6 want those). Setting
 * it globally silently broke ping6. It is passed per-command from the module
 * manifest instead.
 */

#include <sys/types.h>

/* ── sockets ─────────────────────────────────────────────────────────── */
#include <sys/socket.h>
#if __has_include(<sys/socket_private.h>)
#include <sys/socket_private.h>
#endif
#if __has_include(<sys/sockio_private.h>)
#include <sys/sockio_private.h>
#endif

/* ── IP ──────────────────────────────────────────────────────────────── */
#include <netinet/in.h>
/* in_private.h pulls in netinet6/in6_private.h itself and then drops the
 * guard macro again, so including the in6 half here as well is not just
 * redundant — it trips in6_private.h's "don't include me directly" #error. */
#if __has_include(<netinet/in_private.h>)
#include <netinet/in_private.h>
#endif

/* kernel control / system sockets: netstat's systm.c uses struct xsystmgen and
 * struct xkctl_reg without including the private headers that define them. */
#if __has_include(<sys/sys_domain_private.h>)
#include <sys/sys_domain.h>
#include <sys/sys_domain_private.h>
#endif
#if __has_include(<sys/kern_control_private.h>)
#include <sys/kern_control.h>
#include <sys/kern_control_private.h>
#endif
#if __has_include(<sys/kern_event_private.h>)
#include <sys/kern_event.h>
#include <sys/kern_event_private.h>
#endif

/* TCP: netstat reads struct tcp_conn_status out of the private half. */
#if __has_include(<netinet/tcp_private.h>)
#include <netinet/tcp.h>
#include <netinet/tcp_private.h>
#endif

/* ── interfaces and routing ──────────────────────────────────────────── */
#if __has_include(<net/if.h>)
#include <net/if.h>
#if __has_include(<net/if_private.h>)
#include <net/if_private.h>
#endif
#endif
#if __has_include(<net/route.h>)
#include <net/route.h>
#if __has_include(<net/route_private.h>)
#include <net/route_private.h>
#endif
#endif

#endif /* TVM_APPLE_PRELUDE_H */
