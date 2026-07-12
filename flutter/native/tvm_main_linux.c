#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <limits.h>

#include "tvm.h"

extern const unsigned char _binary_initrd_flutter_cpio_start[];
extern const unsigned char _binary_initrd_flutter_cpio_end[];

static void outlog(int fd, const char *msg)
{
    syscall(SYS_write, fd, msg, strlen(msg));
}

int tvm_main(int outfd, int port)
{
    outlog(outfd, "tvm_main: starting\n");

    size_t initrd_size = (size_t)(_binary_initrd_flutter_cpio_end - _binary_initrd_flutter_cpio_start);

    char initrd_path[] = "/tmp/tvm_initrd_XXXXXX";
    int tmpfd = mkstemp(initrd_path);
    if (tmpfd < 0) {
        outlog(outfd, "tvm_main: mkstemp failed\n");
        return -1;
    }
    const unsigned char *p = _binary_initrd_flutter_cpio_start;
    size_t remain = initrd_size;
    while (remain > 0) {
        ssize_t w = syscall(SYS_write, tmpfd, p, remain);
        if (w <= 0) { close(tmpfd); return -1; }
        p += w;
        remain -= w;
    }
    close(tmpfd);

    char initrc_path[] = "/tmp/tvm_initrc_XXXXXX";
    tmpfd = mkstemp(initrc_path);
    if (tmpfd < 0) {
        outlog(outfd, "tvm_main: mkstemp initrc failed\n");
        return -1;
    }
    char initrc[512];
    int n = sprintf(initrc,
        "mount %s:/tvm\n"
        "setenv TVM_SHELL=/tvm/bin/sh\n"
        "setenv PATH=/tvm/bin:$PATH\n"
        "setenv? TVM_NOEMU=/tvm/bin/dropbear:/tvm/bin/sh:/tvm/bin/toybox:/tvm/bin/vim\n"
        "echo TVM initialized\n",
        initrd_path);
    syscall(SYS_write, tmpfd, initrc, n);
    close(tmpfd);

    setenv("TVM_INITRC", initrc_path, 1);

    outlog(outfd, "tvm_main: init\n");
    tvm_init_with_outfd("tvm", outfd);

    unlink(initrd_path);
    unlink(initrc_path);

    outlog(outfd, "tvm_main: exec dropbear\n");

    char port_str[16];
    sprintf(port_str, "%d", port);

    char *argv[] = {"sshd", "-R", "-e", "-F", "-E", "-p", port_str, NULL};
    execvp("/tvm/bin/dropbear", argv);

    outlog(outfd, "tvm_main: execvp failed\n");
    return -1;
}
