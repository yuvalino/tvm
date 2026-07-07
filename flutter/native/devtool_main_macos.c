#include <dlfcn.h>
#include <fcntl.h>
#include <mach-o/dyld.h>
#include <mach-o/getsect.h>
#include <mach-o/loader.h>
#include <mach-o/nlist.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <unistd.h>

#include "tvm.h"

// Patch a loaded dylib's GOT entries to redirect function calls to TVM's
// interposed versions. This is needed on iOS where the ulinker can't load
// dylibs (mmap PROT_EXEC blocked) so we use dlopen instead.
static void patch_got_for_image(const struct mach_header_64 *header, intptr_t slide)
{
    const struct symtab_command *symtab_cmd = NULL;
    const struct dysymtab_command *dysymtab_cmd = NULL;
    const struct segment_command_64 *linkedit_seg = NULL;
    const struct segment_command_64 *data_seg = NULL;
    const struct segment_command_64 *data_const_seg = NULL;

    const uint8_t *p = (const uint8_t *)(header + 1);
    for (uint32_t i = 0; i < header->ncmds; i++) {
        const struct load_command *lc = (const struct load_command *)p;
        if (lc->cmd == LC_SYMTAB)
            symtab_cmd = (const struct symtab_command *)lc;
        else if (lc->cmd == LC_DYSYMTAB)
            dysymtab_cmd = (const struct dysymtab_command *)lc;
        else if (lc->cmd == LC_SEGMENT_64) {
            const struct segment_command_64 *seg = (const struct segment_command_64 *)lc;
            if (strcmp(seg->segname, SEG_LINKEDIT) == 0) linkedit_seg = seg;
            else if (strcmp(seg->segname, "__DATA") == 0) data_seg = seg;
            else if (strcmp(seg->segname, "__DATA_CONST") == 0) data_const_seg = seg;
        }
        p += lc->cmdsize;
    }

    if (!symtab_cmd || !dysymtab_cmd || !linkedit_seg) return;

    uintptr_t linkedit_base = slide + linkedit_seg->vmaddr - linkedit_seg->fileoff;
    const struct nlist_64 *symtab = (const struct nlist_64 *)(linkedit_base + symtab_cmd->symoff);
    const char *strtab = (const char *)(linkedit_base + symtab_cmd->stroff);
    const uint32_t *indirect_symtab = (const uint32_t *)(linkedit_base + dysymtab_cmd->indirectsymoff);

    // Process __DATA and __DATA_CONST segments
    const struct segment_command_64 *segs[] = { data_seg, data_const_seg, NULL };
    for (int s = 0; segs[s]; s++) {
        const struct section_64 *sec = (const struct section_64 *)((const uint8_t *)segs[s] + sizeof(*segs[s]));
        for (uint32_t j = 0; j < segs[s]->nsects; j++, sec++) {
            uint32_t type = sec->flags & SECTION_TYPE;
            if (type != S_LAZY_SYMBOL_POINTERS && type != S_NON_LAZY_SYMBOL_POINTERS)
                continue;

            uint32_t count = (uint32_t)(sec->size / sizeof(void *));
            void **got = (void **)(slide + sec->addr);

            for (uint32_t k = 0; k < count; k++) {
                uint32_t sym_idx = indirect_symtab[sec->reserved1 + k];
                if (sym_idx == INDIRECT_SYMBOL_ABS || sym_idx == INDIRECT_SYMBOL_LOCAL)
                    continue;
                if (sym_idx >= symtab_cmd->nsyms) continue;

                const char *name = strtab + symtab[sym_idx].n_un.n_strx;
                if (name[0] == '_') name++;

                // Only patch process-management functions that TVM needs
                // to interpose. I/O functions (read/write/open) should use
                // real syscalls since the registered program's fds aren't
                // in TVM's fd table.
                // Patch process + I/O functions for TVM interposition.
                // Socket I/O (read/write/close/fcntl/select/poll) must
                // also be patched because TVM's fork copies TVM's fd
                // table which needs to track all fds.
                static const char *patch_list[] = {
                    "fork", "vfork",
                    "execv", "execve", "execvp",
                    "exit", "_exit",
                    "waitpid", "wait",
                    "kill",
                    "getpid", "getppid",
                    "setsid", "getsid",
                    "setpgid", "getpgid", "getpgrp",
                    "sigprocmask", "pthread_sigmask",
                    "tcsetattr", "tcgetattr",
                    "ioctl",
                    "openpty", "forkpty",
                    "login_tty",
                    // I/O must go through TVM so thread-based fork
                    // properly refcounts fds (real close would kill
                    // the fd for both parent and child threads)
                    "open", "openat", "close",
                    "read", "write", "readv", "writev",
                    "pread", "pwrite",
                    "dup", "dup2", "pipe", "fcntl",
                    "socket", "bind", "listen", "accept",
                    "connect", "setsockopt", "getsockopt",
                    "getpeername", "getsockname",
                    "send", "sendto", "sendmsg",
                    "recv", "recvfrom", "recvmsg",
                    "select", "poll",
                    "sigaction", "signal",
                    "getenv", "setenv", "unsetenv", "putenv",
                    "chdir", "getcwd", "mkdir",
                    "isatty",
                    NULL
                };
                int should_patch = 0;
                for (const char **pp = patch_list; *pp; pp++) {
                    if (strcmp(name, *pp) == 0) { should_patch = 1; break; }
                }
                if (should_patch) {
                    void *tvm_func = dlsym(RTLD_SELF, name);
                    if (tvm_func && tvm_func != got[k]) {
                        uintptr_t page = (uintptr_t)&got[k] & ~0xFFF;
                        mprotect((void *)page, 0x1000, PROT_READ | PROT_WRITE);
                        got[k] = tvm_func;
                    }
                }
            }
        }
    }
}

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
        "setenv TVM_SHELL=/tvm/bin/sh\n"
        "setenv PATH=%s:/tvm/bin:$PATH\n"
        "setenv HOME=%s\n"
        "setenv TMPDIR=%s\n"
        "setenv? TVM_NOEMU=libdropbear.dylib:libmksh.dylib:libtoybox.dylib:libvim.dylib:/tvm/bin/dropbear:/tvm/bin/sh:/tvm/bin/toybox:/tvm/bin/vim\n"
        "echo TVM initialized\n",
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
        uint32_t img_count_before = _dyld_image_count();
        void *h = dlopen(path, RTLD_NOW | RTLD_LOCAL);
        if (h) {
            progs[i].main = (main_func_t)dlsym(h, "main");
            if (progs[i].main) {
                // Patch GOT — only fork/vfork needed for iOS
                uint32_t img_count_after = _dyld_image_count();
                for (uint32_t idx = img_count_before; idx < img_count_after; idx++) {
                    const struct mach_header_64 *mh = (const struct mach_header_64 *)_dyld_get_image_header(idx);
                    intptr_t slide = _dyld_get_image_vmaddr_slide(idx);
                    if (mh) patch_got_for_image(mh, slide);
                }
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

    // Use dup'd stderr for TVM logs so --console captures them on iOS
    int logfd = dup(STDERR_FILENO);
    if (logfd < 0) logfd = outfd;

    outlog(outfd, "tvm_main: init\n");
    tvm_init_with_outfd("devtool", logfd);

    unlink(initrc_path);

    tvm_mount_cpio("/tvm", initrd_data, initrd_size);

    for (int i = 0; progs[i].vpath; i++) {
        if (progs[i].main)
            tvm_register_program(progs[i].vpath, progs[i].main);
    }

    chdir(home);

    // Call dropbear directly (not through TVM execvp) to avoid
    // emulator_call and TVM exec context issues
    char port_str[16];
    sprintf(port_str, "%d", port);
    setenv("DROPBEAR_PASSWORD", "alpine", 1);
    char *argv[] = {"sshd", "-R", "-e", "-F", "-E", "-p", port_str, NULL};
    if (progs[0].main) {
        progs[0].main(7, argv, (char *[]){NULL});
    }

    outlog(outfd, "tvm_main: execvp failed\n");
    return -1;
}
