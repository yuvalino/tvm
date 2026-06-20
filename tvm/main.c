#include <unistd.h>
#include <stdio.h>
#include <setjmp.h>
#include <stdlib.h>
#include <ctype.h>
#include <string.h>
#include <sys/wait.h>
#include <signal.h>
#include <limits.h>
#include <fcntl.h>
#include <sys/stat.h>

#include "tvm.h"

int main(int argc, char **argv)
{
    char cwd[PATH_MAX];
    getcwd(cwd, sizeof(cwd));
    setenv("HOME", cwd, 1);

    if (argc < 2) {
        fprintf(stderr, "USAGE: %s [OPTIONS] PROGRAM [ARG...]\n", argv[0]);
        fprintf(stderr, "Try '%s -h' for more information.\n", argv[0]);
        return 1;
    }

    int verbose = 0;
    for (int j = 1; j < argc; j++) {
        if (strcmp(argv[j], "--verbose") == 0) {
            verbose = 1;
            break;
        }
    }

    int i = 1;
    while (i < argc) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            printf("USAGE: %s [OPTIONS] PROGRAM [ARG...]\n\n", argv[0]);
            printf("Run programs in TVM (Thread Virtual Machine)\n\n");
            printf("Options:\n");
            printf("  -h, --help              display this help and exit\n");
            printf("  --verbose               enable kernel log output\n");
            printf("  -i, --initrc PATH       load initrc from PATH\n");
            printf("  -n, --no-initrc         skip initrc loading\n");
            printf("  -v PATH:CPIO            mount CPIO archive at PATH (repeatable)\n");
            return 0;
        } else if (strcmp(argv[i], "--verbose") == 0) {
            i++;
        } else if (strcmp(argv[i], "-i") == 0 || strcmp(argv[i], "--initrc") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "%s: %s requires a path argument\n", argv[0], argv[i]);
                return 1;
            }
            i++;
            setenv("TVM_INITRC", argv[i], 1);
            i++;
        } else if (strcmp(argv[i], "-n") == 0 || strcmp(argv[i], "--no-initrc") == 0) {
            setenv("TVM_NOINITRC", "1", 1);
            i++;
        } else if (strcmp(argv[i], "-v") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "%s: -v requires argument PATH:CPIO\n", argv[0]);
                return 1;
            }
            i++;
            i++;
        } else {
            break;
        }
    }

    if (i >= argc) {
        fprintf(stderr, "%s: no command specified\n", argv[0]);
        return 1;
    }

    if (verbose)
        tvm_init("tvm");
    else {
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull == -1) {
            perror("/dev/null");
            return 1;
        }
        tvm_init_with_outfd("tvm", devnull);
    }

    // process -v mounts after tvm_init (initrc may have mounted things already)
    int j = 1;
    while (j < i) {
        if (strcmp(argv[j], "-v") == 0) {
            j++;
            char *sep = strchr(argv[j], ':');
            if (!sep) {
                fprintf(stderr, "%s: -v argument must be PATH:CPIO (got '%s')\n", argv[0], argv[j]);
                return 1;
            }
            *sep = '\0';
            const char *target = argv[j];
            const char *cpio_path = sep + 1;

            int fd = open(cpio_path, O_RDONLY);
            if (fd == -1) { perror(cpio_path); return 1; }
            struct stat st;
            if (fstat(fd, &st) == -1) { perror(cpio_path); close(fd); return 1; }
            void *buf = malloc(st.st_size);
            if (!buf) { perror("malloc"); close(fd); return 1; }
            size_t total = 0;
            while (total < (size_t)st.st_size) {
                ssize_t n = read(fd, (char *)buf + total, st.st_size - total);
                if (n <= 0) { perror("read"); free(buf); close(fd); return 1; }
                total += n;
            }
            close(fd);
            if (tvm_mount_cpio(target, buf, total) != 0) {
                fprintf(stderr, "tvm_mount_cpio: failed to mount %s at %s\n", cpio_path, target);
                free(buf); return 1;
            }
            free(buf);
        }
        j++;
    }

    execvp(argv[i], &argv[i]);
    perror(argv[i]);
    return 127;
}
