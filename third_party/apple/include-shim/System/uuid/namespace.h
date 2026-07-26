/*
 * Hand-written stand-in for <System/uuid/namespace.h>.
 *
 * Ships only in Apple's internal SDK. Unlike the boot-arg shim next door this
 * one has no library symbol behind it — kFSUUIDNamespaceSHA1 is a constant, so
 * the value has to be right or vsdbutil computes different volume UUIDs than
 * the rest of the system.
 *
 * The value below is the well-known HFS/filesystem UUID namespace that
 * diskdev_cmds, hfs and diskarbitrationd all hash volume identifiers against:
 *   B3E20F39-F292-11D6-97A4-00306543ECAC
 */

#ifndef _TVM_SHIM_SYSTEM_UUID_NAMESPACE_H
#define _TVM_SHIM_SYSTEM_UUID_NAMESPACE_H

#include <uuid/uuid.h>

static const uuid_t kFSUUIDNamespaceSHA1 = {
	0xB3, 0xE2, 0x0F, 0x39,
	0xF2, 0x92,
	0x11, 0xD6,
	0x97, 0xA4,
	0x00, 0x30, 0x65, 0x43, 0xEC, 0xAC
};

#endif /* _TVM_SHIM_SYSTEM_UUID_NAMESPACE_H */
