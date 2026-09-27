/*
 * Force-included (-include) into system_cmds/sysctl/sysctl.c only.
 *
 * sysctl(8) never prints a value without first asking the kernel about it
 * through the CTL_SYSCTL meta node: "0.3" to turn the name into a mib
 * (name2oid), then "0.4.<mib>" for the format (oidfmt) and "0.1.<mib>" to
 * turn the mib back into a name (show_var). The iOS app sandbox allows the
 * name-to-mib query and the value itself (apps read kern.boottime,
 * hw.memsize, hw.machine ... all the time) but denies the format and
 * mib-to-name queries with EPERM, so on a device every `sysctl -n name` died
 * with "sysctl fmt -1 1024 1: Operation not permitted" before fetching
 * anything.
 *
 * Wrap sysctl(2) for this translation unit:
 *  - remember the last name-to-mib translation, and answer a denied
 *    mib-to-name query for that same mib from it;
 *  - when the format query is denied, fetch the value and infer the format
 *    that oidfmt() expects (a u_int kind followed by the format string) from
 *    its size and content. Only the types neofetch-style callers ask for are
 *    inferred: timeval, int, quad, string; anything else is reported as an
 *    opaque, which sysctl(8) refuses to print rather than misprints.
 *  - two values the sandbox denies outright, vm.page_pageable_internal_count
 *    and vm.page_purgeable_count, are the kernel's vm_page_pageable_internal_count
 *    and vm_page_purgeable_count, which host_statistics64() also reports (as
 *    internal_page_count and purgeable_count; vm_stat prints them) and the
 *    sandbox allows. Answer them from there.
 * Every other sysctl() call, and every query the kernel does answer (macOS,
 * or a future iOS that permits them), passes straight through.
 */
#ifndef TVM_SYSCTL_OIDFMT_SHIM_H
#define TVM_SYSCTL_OIDFMT_SHIM_H

#include <sys/types.h>
#include <sys/time.h>
#include <sys/sysctl.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <stdio.h>
#include <mach/mach.h>
#include <mach/mach_host.h>

#define TVM_CTL_SYSCTL          0
#define TVM_CTL_SYSCTL_NAME     1
#define TVM_CTL_SYSCTL_NAME2OID 3
#define TVM_CTL_SYSCTL_OIDFMT   4

static int    tvm_last_mib[CTL_MAXNAME];
static u_int  tvm_last_miblen;
static char   tvm_last_name[BUFSIZ];

/* A value query the sandbox denied; answer it from host_statistics64 when
 * the mib is the one just translated from a name we know how to map. */
static int tvm_sysctl_value_fallback(int *oid, u_int len, void *oldp, size_t *oldlenp)
{
    if (len != tvm_last_miblen || tvm_last_miblen == 0 ||
        memcmp(oid, tvm_last_mib, len * sizeof(int)) != 0) {
        errno = EPERM;
        return -1;
    }
    int internal = strcmp(tvm_last_name, "vm.page_pageable_internal_count") == 0;
    int purgeable = strcmp(tvm_last_name, "vm.page_purgeable_count") == 0;
    if (!internal && !purgeable) {
        errno = EPERM;
        return -1;
    }
    vm_statistics64_data_t vs;
    mach_msg_type_number_t cnt = HOST_VM_INFO64_COUNT;
    if (host_statistics64(mach_host_self(), HOST_VM_INFO64, (host_info64_t)&vs, &cnt) != KERN_SUCCESS) {
        errno = EPERM;
        return -1;
    }
    unsigned int v = internal ? (unsigned int)vs.internal_page_count
                              : (unsigned int)vs.purgeable_count;
    if (oldp) {
        if (*oldlenp < sizeof(v)) {
            errno = ENOMEM;
            return -1;
        }
        memcpy(oldp, &v, sizeof(v));
    }
    *oldlenp = sizeof(v);
    return 0;
}

/* sysctl(2) for a plain value, with the sandbox fallback. */
static int tvm_sysctl_value(int *oid, u_int len, void *oldp, size_t *oldlenp)
{
    int r = sysctl(oid, len, oldp, oldlenp, NULL, 0);
    if (r != 0 && errno == EPERM)
        r = tvm_sysctl_value_fallback(oid, len, oldp, oldlenp);
    return r;
}

static int tvm_sysctl_infer_fmt(int *oid, u_int len, void *oldp, size_t *oldlenp)
{
    size_t vlen = 0;
    if (tvm_sysctl_value(oid, len, NULL, &vlen) != 0)
        return -1;
    unsigned char *val = calloc(1, vlen + 1);
    if (!val)
        return -1;
    size_t got = vlen;
    if (vlen && tvm_sysctl_value(oid, len, val, &got) != 0) {
        int e = errno;
        free(val);
        errno = e;
        return -1;
    }

    u_int kind = CTLFLAG_RD;
    const char *fmt = "";
    if (got == sizeof(struct timeval)) {
        struct timeval tv;
        memcpy(&tv, val, sizeof(tv));
        if (tv.tv_sec > 0 && tv.tv_usec >= 0 && tv.tv_usec < 1000000) {
            kind |= CTLTYPE_OPAQUE;
            fmt = "S,timeval";
        }
    }
    if (!*fmt && got == sizeof(int)) {
        kind |= CTLTYPE_INT;
        fmt = "I";
    } else if (!*fmt && got == sizeof(long long)) {
        kind |= CTLTYPE_QUAD;
        fmt = "Q";
    } else if (!*fmt && got > 0 && val[got - 1] == '\0') {
        size_t i;
        for (i = 0; i + 1 < got; i++)
            if (!isprint(val[i]) && !isspace(val[i]))
                break;
        if (i + 1 == got) {
            kind |= CTLTYPE_STRING;
            fmt = "A";
        }
    }
    if (!*fmt)
        kind |= CTLTYPE_OPAQUE;
    free(val);

    size_t need = sizeof(kind) + strlen(fmt) + 1;
    if (*oldlenp < need) {
        errno = ENOMEM;
        return -1;
    }
    memcpy(oldp, &kind, sizeof(kind));
    memcpy((char *)oldp + sizeof(kind), fmt, strlen(fmt) + 1);
    *oldlenp = need;
    return 0;
}

static int tvm_sysctl(int *name, u_int namelen, void *oldp, size_t *oldlenp,
                      void *newp, size_t newlen)
{
    int r = sysctl(name, namelen, oldp, oldlenp, newp, newlen);
    if (!oldlenp)
        return r;
    if (namelen < 2 || name[0] != TVM_CTL_SYSCTL) {
        if (r != 0 && errno == EPERM && !newp)
            r = tvm_sysctl_value_fallback(name, namelen, oldp, oldlenp);
        return r;
    }
    if (!oldp)
        return r;

    if (r == 0 && namelen == 2 && name[1] == TVM_CTL_SYSCTL_NAME2OID &&
        newp && newlen < sizeof(tvm_last_name) &&
        *oldlenp <= sizeof(tvm_last_mib)) {
        memcpy(tvm_last_name, newp, newlen);
        tvm_last_name[newlen] = '\0';
        memcpy(tvm_last_mib, oldp, *oldlenp);
        tvm_last_miblen = (u_int)(*oldlenp / sizeof(int));
        return r;
    }
    if (r == 0 || errno != EPERM || namelen < 3)
        return r;

    if (name[1] == TVM_CTL_SYSCTL_NAME) {
        size_t need = strlen(tvm_last_name) + 1;
        if (namelen - 2 != tvm_last_miblen || tvm_last_miblen == 0 ||
            memcmp(name + 2, tvm_last_mib, tvm_last_miblen * sizeof(int)) != 0 ||
            *oldlenp < need) {
            errno = EPERM;
            return -1;
        }
        memcpy(oldp, tvm_last_name, need);
        *oldlenp = need;
        return 0;
    }
    if (name[1] == TVM_CTL_SYSCTL_OIDFMT)
        return tvm_sysctl_infer_fmt(name + 2, namelen - 2, oldp, oldlenp);
    return r;
}

#define sysctl tvm_sysctl

#endif
