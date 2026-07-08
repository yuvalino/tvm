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

    // Create symlinks to bundled dylibs so TVM's execvp finds them via PATH
    const char *fwdir = get_frameworks_dir();
    char bindir[PATH_MAX];
    snprintf(bindir, sizeof(bindir), "%stvm_bin", tmpdir);
    mkdir(bindir, 0755);

    struct { const char *name; const char *dylib; } bins[] = {
        { "dropbear", "libdropbear.dylib" },
        { "sh",       "libmksh.dylib"     },
        { "toybox",   "libtoybox.dylib"   },
        { "vim",      "libvim.dylib"      },
        { NULL, NULL }
    };

    if (fwdir) {
        for (int i = 0; bins[i].name; i++) {
            char target[PATH_MAX], link[PATH_MAX];
            snprintf(target, sizeof(target), "%s/%s", fwdir, bins[i].dylib);
            snprintf(link, sizeof(link), "%s/%s", bindir, bins[i].name);
            unlink(link);
            if (0 == symlink(target, link)) {
                outlog(outfd, "tvm_main: linked ");
                outlog(outfd, bins[i].name);
                outlog(outfd, "\n");
            }
        }
    }

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
        "setenv TVM_SHELL=%s/sh\n"
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

    // Set TVM_SHELL at the process level too (the initrc sets it in TVM's
    // env table, but dropbear's getenv might use the process environ)
    char tvm_shell[PATH_MAX];
    snprintf(tvm_shell, sizeof(tvm_shell), "%s/sh", bindir);
    setenv("TVM_SHELL", tvm_shell, 1);

    // Use dup'd stderr for TVM kernel logs so --console captures them
    int logfd = dup(STDERR_FILENO);
    if (logfd < 0) logfd = outfd;

    outlog(outfd, "tvm_main: init\n");
    tvm_init_with_outfd("devtool", logfd);

    unlink(initrc_path);

    // Mount initrd from embedded Mach-O section data directly
    tvm_mount_cpio("/tvm", initrd_data, initrd_size);

    chdir(home);

    const char *shell_check = getenv("TVM_SHELL");
    outlog(outfd, "tvm_main: TVM_SHELL=");
    outlog(outfd, shell_check ? shell_check : "(null)");
    outlog(outfd, "\n");
    outlog(outfd, "tvm_main: exec dropbear\n");

    char port_str[16];
    sprintf(port_str, "%d", port);

    char *argv[] = {"sshd", "-R", "-e", "-F", "-E", "-p", port_str, NULL};
    execvp("dropbear", argv);

    outlog(outfd, "tvm_main: execvp failed\n");
    return -1;
}
