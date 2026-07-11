#include <dlfcn.h>
#include <fcntl.h>
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

static void outlog(int fd, const char *msg)
{
    write(fd, msg, strlen(msg));
    write(STDERR_FILENO, msg, strlen(msg));
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

    const char *tmpdir = get_tmpdir();
    const char *fwdir = get_frameworks_dir();
    if (!fwdir) {
        outlog(outfd, "tvm_main: cannot find Frameworks dir\n");
        return -1;
    }

    const char *home = getenv("HOME");
    if (!home || !home[0])
        home = tmpdir;

    char home_tmp[PATH_MAX];
    snprintf(home_tmp, sizeof(home_tmp), "%s/tmp", home);
    mkdir(home_tmp, 0777);

    // initrc: set up environment
    // PATH includes Frameworks dir so launchers' execvp finds the dylibs
    char initrc_path[PATH_MAX];
    snprintf(initrc_path, sizeof(initrc_path), "%stvm_initrc_XXXXXX", tmpdir);
    int tmpfd = mkstemp(initrc_path);
    if (tmpfd < 0) {
        outlog(outfd, "tvm_main: mkstemp initrc failed\n");
        return -1;
    }

    char initrc[2048];
    int n = sprintf(initrc,
        "setenv TVM_SHELL=/tvm/bin/sh\n"
        "setenv PATH=%s:/tvm/bin:$PATH\n"
        "setenv HOME=%s\n"
        "setenv TMPDIR=%s\n"
        "setenv? TVM_NOEMU=libdropbear.dylib:libmksh.dylib:libtoybox.dylib:libvim.dylib\n"
        "echo TVM initialized\n",
        fwdir,
        home, home_tmp);
    write(tmpfd, initrc, n);
    close(tmpfd);

    setenv("TVM_INITRC", initrc_path, 1);
    setenv("TVM_SHELL", "/tvm/bin/sh", 1);

    // Use dup'd stderr for TVM kernel logs so --console captures them
    int logfd = dup(STDERR_FILENO);
    if (logfd < 0) logfd = outfd;

    outlog(outfd, "tvm_main: init\n");
    tvm_init_with_outfd("devtool", logfd);

    unlink(initrc_path);

    // Mount initrd from file in the Frameworks dir
    {
        char initrd_path[PATH_MAX];
        snprintf(initrd_path, sizeof(initrd_path), "%s/initrd.cpio", fwdir);
        int ifd = open(initrd_path, O_RDONLY);
        if (ifd < 0) {
            outlog(outfd, "tvm_main: cannot open initrd.cpio\n");
            return -1;
        }
        struct stat st;
        fstat(ifd, &st);
        void *initrd_data = malloc(st.st_size);
        if (!initrd_data) { close(ifd); return -1; }
        read(ifd, initrd_data, st.st_size);
        close(ifd);
        tvm_mount_cpio("/tvm", initrd_data, st.st_size);
        free(initrd_data);
    }

    chdir(home);

    outlog(outfd, "tvm_main: exec dropbear\n");

    char port_str[16];
    sprintf(port_str, "%d", port);

    char *argv[] = {"sshd", "-R", "-e", "-F", "-E", "-p", port_str, NULL};
    execvp("dropbear", argv);

    outlog(outfd, "tvm_main: execvp failed\n");
    return -1;
}
