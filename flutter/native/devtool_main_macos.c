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
        // dropbear multi-call symlinks
        const char *dropbear_applets[] = {
            "scp", "dbclient", "ssh", "dropbearkey", "ssh-keygen", "sshd", NULL
        };
        char dropbear_target[PATH_MAX];
        snprintf(dropbear_target, sizeof(dropbear_target), "%s/libdropbear.dylib", fwdir);
        for (const char **a = dropbear_applets; *a; a++) {
            char link[PATH_MAX];
            snprintf(link, sizeof(link), "%s/%s", bindir, *a);
            unlink(link);
            symlink(dropbear_target, link);
        }
        // vi -> vim
        {
            char link[PATH_MAX];
            char vim_target[PATH_MAX];
            snprintf(vim_target, sizeof(vim_target), "%s/libvim.dylib", fwdir);
            snprintf(link, sizeof(link), "%s/vi", bindir);
            unlink(link);
            symlink(vim_target, link);
        }
        // toybox applet symlinks — link common applets to toybox
        const char *toybox_applets[] = {
            "basename", "cat", "chmod", "chown", "cmp", "comm", "cp",
            "cut", "date", "dd", "df", "diff", "dirname", "du",
            "echo", "env", "expr", "false", "find", "grep", "groups",
            "head", "hostname", "id", "kill", "ln", "ls", "md5sum",
            "mkdir", "mktemp", "mount", "mv", "nice", "nl", "nohup",
            "od", "paste", "patch", "printenv", "printf", "ps", "pwd",
            "readlink", "realpath", "rm", "rmdir", "sed", "seq",
            "sha1sum", "sha256sum", "sleep", "sort", "split", "stat",
            "strings", "tail", "tar", "tee", "test", "time", "touch",
            "tr", "true", "truncate", "tty", "uname", "uniq",
            "unlink", "wc", "which", "who", "whoami", "xargs", "yes",
            NULL
        };
        char toybox_target[PATH_MAX];
        snprintf(toybox_target, sizeof(toybox_target), "%s/libtoybox.dylib", fwdir);
        for (const char **a = toybox_applets; *a; a++) {
            char link[PATH_MAX];
            snprintf(link, sizeof(link), "%s/%s", bindir, *a);
            if (access(link, F_OK) != 0) {
                symlink(toybox_target, link);
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

    // Also set PATH at process level
    char path_env[PATH_MAX * 2];
    snprintf(path_env, sizeof(path_env), "%s:/tvm/bin:/usr/bin:/bin:/usr/sbin:/sbin", bindir);
    setenv("PATH", path_env, 1);

    // Use dup'd stderr for TVM kernel logs so --console captures them
    int logfd = dup(STDERR_FILENO);
    if (logfd < 0) logfd = outfd;

    outlog(outfd, "tvm_main: init\n");
    tvm_init_with_outfd("devtool", logfd);

    unlink(initrc_path);

    // Mount initrd from embedded Mach-O section data directly
    tvm_mount_cpio("/tvm", initrd_data, initrd_size);

    // Override initrd binaries with symlinks to the Frameworks dylibs.
    // The initrd contains macOS binaries; on iOS we need the dylibs.
    if (fwdir) {
        struct { const char *vpath; const char *dylib; } overrides[] = {
            { "/tvm/bin/dropbear", "libdropbear.dylib" },
            { "/tvm/bin/sh",      "libmksh.dylib"     },
            { "/tvm/bin/toybox",  "libtoybox.dylib"   },
            { "/tvm/bin/vim",     "libvim.dylib"      },
            { NULL, NULL }
        };
        for (int i = 0; overrides[i].vpath; i++) {
            char target[PATH_MAX];
            snprintf(target, sizeof(target), "%s/%s", fwdir, overrides[i].dylib);
            unlink(overrides[i].vpath);
            symlink(target, overrides[i].vpath);
        }
    }

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
