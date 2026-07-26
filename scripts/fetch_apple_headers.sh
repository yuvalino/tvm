#!/bin/bash
# Refresh third_party/apple/include/ from Apple's open-source releases.
#
# The iOS SDK ships the same libSystem as macOS but installs far fewer headers,
# and neither SDK installs the PRIVATE-guarded ones that the *_cmds trees are
# built against internally. Every symbol behind these headers is present on the
# device — only the declarations are missing — so we vendor the declarations
# from the corresponding open-source project.
#
# These are consulted via -idirafter, i.e. only when the real SDK has no such
# header. That keeps the SDK authoritative and stops xnu's kernel-side headers
# from shadowing the userspace ones.
#
# Usage: fetch_apple_headers.sh [dest-dir]   (default: third_party/apple/include)
set -e

DEST="${1:-$(cd "$(dirname "$0")/.." && pwd)/third_party/apple/include}"
BASE="https://raw.githubusercontent.com/apple-oss-distributions"

# <dest-relative-path>  <repo>  <path-in-repo>
#
# Everything xnu marks *_private.h is vendored wholesale: those filenames do not
# exist in either SDK, so they can never shadow a real SDK header.
MANIFEST='
sys/socket_private.h            xnu      bsd/sys/socket_private.h
sys/sockio_private.h            xnu      bsd/sys/sockio_private.h
sys/kern_control_private.h      xnu      bsd/sys/kern_control_private.h
sys/kern_event_private.h        xnu      bsd/sys/kern_event_private.h
sys/sys_domain_private.h        xnu      bsd/sys/sys_domain_private.h
sys/coalition_private.h         xnu      bsd/sys/coalition_private.h
sys/fcntl_private.h             xnu      bsd/sys/fcntl_private.h
sys/fsgetpath_private.h         xnu      bsd/sys/fsgetpath_private.h
sys/kpi_private.h               xnu      bsd/sys/kpi_private.h
sys/mem_acct_private.h          xnu      bsd/sys/mem_acct_private.h
sys/kern_memorystatus.h         xnu      bsd/sys/kern_memorystatus.h
sys/disklabel.h                 xnu      bsd/sys/disklabel.h
sys/stackshot.h                 xnu      bsd/sys/stackshot.h
sys/pgo.h                       xnu      bsd/sys/pgo.h
sys/vsock_private.h             xnu      bsd/sys/vsock_private.h
sys/proc_info_private.h         xnu      bsd/sys/proc_info_private.h
sys/process_policy.h            xnu      bsd/sys/process_policy.h
sys/resource_private.h          xnu      bsd/sys/resource_private.h
sys/disk_private.h              xnu      bsd/sys/disk_private.h
sys/event_private.h             xnu      bsd/sys/event_private.h
sys/kdebug_private.h            xnu      bsd/sys/kdebug_private.h
sys/attr_private.h              xnu      bsd/sys/attr_private.h
sys/dirent_private.h            xnu      bsd/sys/dirent_private.h
sys/fsctl.h                     xnu      bsd/sys/fsctl.h
spawn_private.h                 xnu      libsyscall/wrappers/spawn/spawn_private.h
os/base_private.h               xnu      libkern/os/base_private.h
kern/debug.h                    xnu      osfmk/kern/debug.h
vm_statistics.h                 xnu      osfmk/mach/vm_statistics.h
stack_logging.h                 libmalloc private/stack_logging.h
os/linker_set.h                 Libc     os/linker_set.h
sys/fsgetpath.h                 xnu      bsd/sys/fsgetpath.h
sys/persona.h                   xnu      bsd/sys/persona.h
sys/reason.h                    xnu      bsd/sys/reason.h
sys/proc_uuid_policy.h          xnu      bsd/sys/proc_uuid_policy.h
sys/spawn_internal.h            xnu      bsd/sys/spawn_internal.h
System/sys/proc.h               xnu      bsd/sys/proc.h
System/sys/persona.h            xnu      bsd/sys/persona.h
System/sys/reason.h             xnu      bsd/sys/reason.h
System/sys/fsctl.h              xnu      bsd/sys/fsctl.h
System/sys/proc_uuid_policy.h   xnu      bsd/sys/proc_uuid_policy.h
System/sys/kern_memorystatus.h  xnu      bsd/sys/kern_memorystatus.h
net/radix.h                     xnu      bsd/net/radix.h
net/if_bond_internal.h          xnu      bsd/net/if_bond_internal.h
net/net_api_stats.h             xnu      bsd/net/net_api_stats.h
net/net_perf.h                  xnu      bsd/net/net_perf.h
net/classq/classq.h             xnu      bsd/net/classq/classq.h
net/pktsched/pktsched_fq_codel.h xnu     bsd/net/pktsched/pktsched_fq_codel.h
netinet/in_stat.h               xnu      bsd/netinet/in_stat.h
netinet6/in6_pcb.h              xnu      bsd/netinet6/in6_pcb.h
skywalk/os_channel.h            xnu      bsd/skywalk/channel/os_channel.h
skywalk/os_channel_event.h      xnu      bsd/skywalk/channel/os_channel_event.h
skywalk/os_nexus.h              xnu      bsd/skywalk/nexus/os_nexus.h
skywalk/os_packet.h             xnu      bsd/skywalk/packet/os_packet.h
machine/disklabel.h             xnu      bsd/arm/disklabel.h
os/errno.h                      xnu      bsd/sys/errno.h
mach/coalition.h                xnu      bsd/sys/coalition.h
kern/panic_call.h               xnu      osfmk/kern/panic_call.h
net/ntstat.h                    xnu      bsd/net/ntstat.h
net/route_private.h             xnu      bsd/net/route_private.h
net/if_private.h                xnu      bsd/net/if_private.h
net/network_agent.h             xnu      bsd/net/network_agent.h
net/nat464_utils.h              xnu      bsd/net/nat464_utils.h
net/if_mib_private.h            xnu      bsd/net/if_mib_private.h
net/bpf_private.h               xnu      bsd/net/bpf_private.h
net/dlil_var_private.h          xnu      bsd/net/dlil_var_private.h
net/strict_type_cnv_private.h   xnu      bsd/net/strict_type_cnv_private.h
netinet6/in6_private.h          xnu      bsd/netinet6/in6_private.h
netinet/ip_flowid.h             xnu      bsd/netinet/ip_flowid.h
net/if_var_private.h            xnu      bsd/net/if_var_private.h
net/if_fake_var.h               xnu      bsd/net/if_fake_var.h
net/if_vlan_var.h               xnu      bsd/net/if_vlan_var.h
net/if_llreach.h                xnu      bsd/net/if_llreach.h
net/if_bridgevar.h              xnu      bsd/net/if_bridgevar.h
net/if_bond_var.h               xnu      bsd/net/if_bond_var.h
net/if_ports_used.h             xnu      bsd/net/if_ports_used.h
net/pktsched/pktsched.h         xnu      bsd/net/pktsched/pktsched.h
net/classq/if_classq.h          xnu      bsd/net/classq/if_classq.h
netinet/in_private.h            xnu      bsd/netinet/in_private.h
netinet/ip_dummynet.h           xnu      bsd/netinet/ip_dummynet.h
netinet/mptcp_var.h             xnu      bsd/netinet/mptcp_var.h
netinet/tcp_private.h           xnu      bsd/netinet/tcp_private.h
sys/kern_control.h              xnu      bsd/sys/kern_control.h
netinet6/ip6_var.h              xnu      bsd/netinet6/ip6_var.h
Kernel/kern/ledger.h            xnu      osfmk/kern/ledger.h
libproc_private.h               xnu      libsyscall/wrappers/libproc/libproc_private.h
os/assumes.h                    Libc     os/assumes.h
libutil.h                       libutil  libutil.h
vproc_priv.h                    launchd  liblaunch/vproc_priv.h
bootstrap_priv.h                launchd  liblaunch/bootstrap_priv.h
'

# Headers the SDKs *do* ship, but with the PRIVATE sections stripped out. These
# go in a separate directory that is passed with -I (i.e. it beats the SDK)
# rather than -idirafter, because for these the SDK copy would otherwise win and
# we would still be missing the declarations. xnu is the tree Apple builds the
# real thing from, so the layouts match — this only restores what was elided.
OVERRIDE_MANIFEST='
netinet6/nd6.h                  xnu      bsd/netinet6/nd6.h
netinet6/in6.h                  xnu      bsd/netinet6/in6.h
net/bpf.h                       xnu      bsd/net/bpf.h
net/lacp.h                      xnu      bsd/net/lacp.h
net/if_mib.h                    xnu      bsd/net/if_mib.h
net/if.h                        xnu      bsd/net/if.h
sys/socketvar.h                 xnu      bsd/sys/socketvar.h
sys/mbuf.h                      xnu      bsd/sys/mbuf.h
sys/kern_control.h              xnu      bsd/sys/kern_control.h
sys/sys_domain.h                xnu      bsd/sys/sys_domain.h
sys/kern_event.h                xnu      bsd/sys/kern_event.h
sys/unpcb.h                     xnu      bsd/sys/unpcb.h
netinet/igmp_var.h              xnu      bsd/netinet/igmp_var.h
netinet6/mld6_var.h             xnu      bsd/netinet6/mld6_var.h
netinet6/mld6_var.h             xnu      bsd/netinet6/mld6_var.h
netinet6/in6_var.h              xnu      bsd/netinet6/in6_var.h
netinet/in_pcb.h                xnu      bsd/netinet/in_pcb.h
netinet/tcp_var.h               xnu      bsd/netinet/tcp_var.h
'

fetch_into() { # <dir> <manifest>
    local dir="$1" manifest="$2" n=0
    mkdir -p "$dir"
    while read -r dst repo src; do
        [ -n "$dst" ] || continue
        mkdir -p "$dir/$(dirname "$dst")"
        if curl -sfL "$BASE/$repo/main/$src" -o "$dir/$dst"; then
            n=$((n + 1))
        else
            echo "  MISSING  $dst  <- $repo/$src" >&2
            rm -f "$dir/$dst"
            miss=$((miss + 1))
        fi
    done <<< "$manifest"
    echo "$n"
}

miss=0
ok=$(fetch_into "$DEST" "$MANIFEST")
ok_ovr=$(fetch_into "$DEST/../include-override" "$OVERRIDE_MANIFEST")

# ── Reconstructed declarations ────────────────────────────────────────────
# A handful of things the *_cmds sources use have never appeared in any public
# xnu drop — Apple's internal kernel has them, the open-source one does not.
# We add them here so the surrounding command still builds. The affected
# feature (ifconfig's netem packet scheduler) needs root and a real ifnet, so
# it cannot work under TVM regardless; everything else in ifconfig is fine.
fixup() { # <file> <anchor> <text-to-insert-after>
    local f="$1" anchor="$2" text="$3"
    grep -q "$anchor" "$f" || { echo "error: anchor '$anchor' not found in $f — xnu layout changed" >&2; exit 1; }
    grep -q "$(printf '%s' "$text" | head -1)" "$f" && return 0
    awk -v a="$anchor" -v t="$text" '{ print; if (index($0, a)) print t }' "$f" > "$f.tmp" && mv "$f.tmp" "$f"
}

# NB: awk -v expands \t and \n itself, so the inserted text is written escaped.
IFVP="$DEST/net/if_var_private.h"
fixup "$IFVP" 'IF_NETEM_MODEL_NLC = 1,' \
    '\tIF_NETEM_MODEL_IOD = 2,\t/* reconstructed: absent from public xnu */\n\tIF_NETEM_MODEL_FPD = 3,\t/* reconstructed: absent from public xnu */'
fixup "$IFVP" 'ifnetem_reordering_p;' \
    '\tuint32_t        ifnetem_reordering_ms;\t/* reconstructed: absent from public xnu */'

# ifconfig's `disable_output' verb. Another internal-only field; it is a new
# member of an existing union, so it does not move any of the others.
fixup "$DEST/../include-override/net/if.h" 'u_int32_t ifru_functional_type;' \
    '\t\tint     ifru_disable_output;\t/* reconstructed: absent from public xnu */'

# ping6 and traceroute6 use the RFC3542 option names *and* the RFC2292 alias
# IPV6_PKTOPTIONS. Upstream in6.h makes those two halves mutually exclusive on
# __APPLE_USE_RFC_3542; Apple's internal SDK exposes both. Re-add the alias so
# the RFC3542 half no longer costs us IPV6_PKTOPTIONS.
fixup "$DEST/../include-override/netinet6/in6.h" '#define IPV6_2292PKTOPTIONS' \
    '#if defined(__APPLE_USE_RFC_3542) && !defined(IPV6_PKTOPTIONS)\n#define IPV6_PKTOPTIONS IPV6_2292PKTOPTIONS\t/* reconstructed: internal SDK exposes both */\n#endif'

echo "fetched $ok header(s) into $DEST"
echo "fetched $ok_ovr override header(s) into $(cd "$DEST/.." && pwd)/include-override"
[ "$miss" -eq 0 ] || echo "warning: $miss header(s) missing" >&2
