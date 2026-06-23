#include <dlfcn.h>
#include <fcntl.h>
#include <mach-o/getsect.h>
#include <mach-o/loader.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <unistd.h>

#include "tvm.h"

static void _anchor(void) {}

static const struct mach_header_64 *dylib_header(void)
{
    Dl_info info;
    if (dladdr((void *)_anchor, &info))
        return (const struct mach_header_64 *)info.dli_fbase;
    return NULL;
}

static void outlog(int fd, const char *msg)
{
    write(fd, msg, strlen(msg));
}

static const char *get_tmpdir(void)
{
    const char *t = getenv("TMPDIR");
    if (t && t[0]) return t;
    return "/tmp/";
}

int tvm_main(int outfd, int port)
{
    outlog(outfd, "tvm_main: starting\n");

    const struct mach_header_64 *hdr = dylib_header();
    if (!hdr) {
        outlog(outfd, "tvm_main: cannot find dylib header\n");
        return -1;
    }
    unsigned long initrd_size = 0;
    const uint8_t *initrd_data = getsectiondata(hdr, "__DATA", "__initrd", &initrd_size);
    if (!initrd_data || initrd_size == 0) {
        outlog(outfd, "tvm_main: no embedded initrd\n");
        return -1;
    }

    const char *tmpdir = get_tmpdir();
    char initrd_path[PATH_MAX];
    snprintf(initrd_path, sizeof(initrd_path), "%stvm_initrd_XXXXXX", tmpdir);
    int tmpfd = mkstemp(initrd_path);
    if (tmpfd < 0) {
        outlog(outfd, "tvm_main: mkstemp failed\n");
        return -1;
    }
    const uint8_t *p = initrd_data;
    size_t remain = initrd_size;
    while (remain > 0) {
        ssize_t w = write(tmpfd, p, remain);
        if (w <= 0) { close(tmpfd); return -1; }
        p += w;
        remain -= w;
    }
    close(tmpfd);

    char initrc_path[PATH_MAX];
    snprintf(initrc_path, sizeof(initrc_path), "%stvm_initrc_XXXXXX", tmpdir);
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
    write(tmpfd, initrc, n);
    close(tmpfd);

    setenv("TVM_INITRC", initrc_path, 1);

    outlog(outfd, "tvm_main: init\n");
    tvm_init_with_outfd("devtool", outfd);

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
