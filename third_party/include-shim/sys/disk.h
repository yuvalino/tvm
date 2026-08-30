#ifndef _TVM_SHIM_SYS_DISK_H
#define _TVM_SHIM_SYS_DISK_H

#include <sys/ioctl.h>
#include <stdint.h>

#define DKIOCGETBLOCKSIZE  _IOR('d', 24, uint32_t)
#define DKIOCGETBLOCKCOUNT _IOR('d', 25, uint64_t)

#endif
