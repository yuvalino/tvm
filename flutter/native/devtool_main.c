#define _GNU_SOURCE
#include <dlfcn.h>
#include <elf.h>
#include <errno.h>
#include <fcntl.h>
#include <link.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <limits.h>

#include "tvm.h"

extern const unsigned char _binary_initrd_cpio_start[];
extern const unsigned char _binary_initrd_cpio_end[];

char __executable_start __attribute__((weak));
char etext __attribute__((weak));

static void _anchor(void) {}

void get_text(uintptr_t *out_start, uintptr_t *out_end)
{
    Dl_info info;
    struct link_map *lm;
    if (dladdr1((void *)_anchor, &info, (void **)&lm, RTLD_DL_LINKMAP) && lm) {
        *out_start = (uintptr_t)lm->l_addr;
        const ElfW(Ehdr) *ehdr = (const ElfW(Ehdr) *)lm->l_addr;
        const ElfW(Phdr) *phdr = (const ElfW(Phdr) *)((char *)lm->l_addr + ehdr->e_phoff);
        uintptr_t end = *out_start;
        for (int i = 0; i < ehdr->e_phnum; i++) {
            if (phdr[i].p_type == PT_LOAD && (phdr[i].p_flags & PF_X)) {
                uintptr_t seg_end = lm->l_addr + phdr[i].p_vaddr + phdr[i].p_memsz;
                if (seg_end > end) end = seg_end;
            }
        }
        *out_end = end;
    } else {
        *out_start = 0;
        *out_end = 0;
    }
}

static void outlog(int fd, const char *msg)
{
    syscall(SYS_write, fd, msg, strlen(msg));
}

int tvm_main(int outfd, int port)
{
    outlog(outfd, "tvm_main: starting\n");

    size_t initrd_size = (size_t)(_binary_initrd_cpio_end - _binary_initrd_cpio_start);

    char initrd_path[] = "/tmp/tvm_initrd_XXXXXX";
    int tmpfd = mkstemp(initrd_path);
    if (tmpfd < 0) {
        outlog(outfd, "tvm_main: mkstemp failed\n");
        return -1;
    }
    const unsigned char *p = _binary_initrd_cpio_start;
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
