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
#include <sys/stat.h>
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

static const char *get_frameworks_dir(void)
{
    static char dir[PATH_MAX];
    Dl_info info;
    if (dladdr((void *)_anchor, &info) && info.dli_fname) {
        char *last_slash = strrchr(info.dli_fname, '/');
        if (last_slash) {
            size_t len = last_slash - info.dli_fname;
            memcpy(dir, info.dli_fname, len);
            dir[len] = '\0';
            return dir;
        }
    }
    return NULL;
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

    // Use Frameworks dir directly on PATH — iOS requires mmap(PROT_EXEC)
    // to operate on files in the signed app bundle, not through symlinks
    const char *fwdir = get_frameworks_dir();
    if (!fwdir) {
        outlog(outfd, "tvm_main: cannot find Frameworks dir\n");
        return -1;
    }
    char bindir[PATH_MAX];
    snprintf(bindir, sizeof(bindir), "%s", fwdir);

    char initrc_path[PATH_MAX];
    snprintf(initrc_path, sizeof(initrc_path), "%stvm_initrc_XXXXXX", tmpdir);
    int tmpfd = mkstemp(initrc_path);
    if (tmpfd < 0) {
        outlog(outfd, "tvm_main: mkstemp initrc failed\n");
        return -1;
    }
    const char *home = getenv("HOME");
    if (!home || !home[0])
        home = tmpdir;

    char home_tmp[PATH_MAX];
    snprintf(home_tmp, sizeof(home_tmp), "%s/tmp", home);
    mkdir(home_tmp, 0777);

    char initrc[1024];
    int n = sprintf(initrc,
        "setenv TVM_SHELL=%s/libmksh.dylib\n"
        "setenv PATH=%s:/tvm/bin:$PATH\n"
        "setenv HOME=%s\n"
        "setenv TMPDIR=%s\n"
        "setenv? TVM_NOEMU=libdropbear.dylib:libmksh.dylib:libtoybox.dylib:libvim.dylib:/tvm/bin/dropbear:/tvm/bin/sh:/tvm/bin/toybox:/tvm/bin/vim\n"
        "echo TVM initialized\n",
        bindir,
        bindir,
        home, home_tmp);
    write(tmpfd, initrc, n);
    close(tmpfd);

    setenv("TVM_INITRC", initrc_path, 1);

    // Pre-load dylibs via dlopen BEFORE tvm_init (uses real dlopen).
    // On iOS, the ulinker can't mmap(PROT_EXEC) so we dlopen and
    // register the main symbols for tvm_register_program after init.
    struct { const char *vpath; const char *dylib; main_func_t main; } progs[] = {
        { "/tvm/bin/dropbear", "libdropbear.dylib", NULL },
        { "/tvm/bin/sh",      "libmksh.dylib",     NULL },
        { "/tvm/bin/toybox",  "libtoybox.dylib",   NULL },
        { "/tvm/bin/vim",     "libvim.dylib",      NULL },
        { NULL, NULL, NULL }
    };
    for (int i = 0; progs[i].vpath; i++) {
        char path[PATH_MAX];
        snprintf(path, sizeof(path), "%s/%s", fwdir, progs[i].dylib);
        void *h = dlopen(path, RTLD_NOW | RTLD_LOCAL);
        if (h) {
            progs[i].main = (main_func_t)dlsym(h, "main");
            if (progs[i].main) {
                outlog(outfd, "tvm_main: loaded ");
                outlog(outfd, progs[i].dylib);
                outlog(outfd, "\n");
            }
        } else {
            outlog(outfd, "tvm_main: dlopen failed: ");
            outlog(outfd, dlerror());
            outlog(outfd, "\n");
        }
    }

    outlog(outfd, "tvm_main: init\n");
    tvm_init_with_outfd("devtool", outfd);

    unlink(initrc_path);

    // Mount initrd directly from embedded data
    tvm_mount_cpio("/tvm", initrd_data, initrd_size);

    // Register dlopen'd programs with TVM
    for (int i = 0; progs[i].vpath; i++) {
        if (progs[i].main)
            tvm_register_program(progs[i].vpath, progs[i].main);
    }

    chdir(home);

    outlog(outfd, "tvm_main: exec dropbear\n");

    char port_str[16];
    sprintf(port_str, "%d", port);

    char *argv[] = {"sshd", "-R", "-e", "-F", "-E", "-p", port_str, NULL};
    execvp("/tvm/bin/dropbear", argv);

    outlog(outfd, "tvm_main: execvp failed\n");
    return -1;
}
