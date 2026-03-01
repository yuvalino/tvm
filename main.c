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

static int mount_cpio(const char *target, const char *cpio_path)
{
    int fd = open(cpio_path, O_RDONLY);
    if (fd == -1) {
        perror(cpio_path);
        return -1;
    }

    struct stat st;
    if (fstat(fd, &st) == -1) {
        perror(cpio_path);
        close(fd);
        return -1;
    }

    void *buf = malloc(st.st_size);
    if (!buf) {
        perror("malloc");
        close(fd);
        return -1;
    }

    size_t total = 0;
    while (total < (size_t)st.st_size) {
        ssize_t n = read(fd, (char *)buf + total, st.st_size - total);
        if (n <= 0) {
            perror("read");
            free(buf);
            close(fd);
            return -1;
        }
        total += n;
    }
    close(fd);

    if (tvm_mount_cpio(target, buf, total) != 0) {
        fprintf(stderr, "tvm_mount_cpio: failed to mount %s at %s\n", cpio_path, target);
        free(buf);
        return -1;
    }

    free(buf);
    return 0;
}

int main(int argc, char **argv)
{
    // set home to current directory
    char cwd[PATH_MAX];
    getcwd(cwd, sizeof(cwd));
    setenv("HOME", cwd, 1);

    if (argc < 2) {
        fprintf(stderr, "USAGE: %s [OPTIONS] PROGRAM [ARG...]\n", argv[0]);
        fprintf(stderr, "Try '%s -h' for more information.\n", argv[0]);
        return 1;
    }

    tvm_init("tvm");

    int i = 1;

    while (i < argc) {
        if (strcmp(argv[i], "-h") == 0) {
            printf("USAGE: %s [OPTIONS] PROGRAM [ARG...]\n\n", argv[0]);
            printf("Run programs in TVM (Thread Virtual Machine)\n\n");
            printf("Options:\n");
            printf("  -h             display this help and exit\n");
            printf("  -v PATH:CPIO   mount CPIO archive at PATH (repeatable)\n");
            return 0;
        } else if (strcmp(argv[i], "-v") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "%s: -v requires argument PATH:CPIO\n", argv[0]);
                return 1;
            }
            i++;
            char *sep = strchr(argv[i], ':');
            if (!sep) {
                fprintf(stderr, "%s: -v argument must be PATH:CPIO (got '%s')\n", argv[0], argv[i]);
                return 1;
            }
            *sep = '\0';
            const char *target = argv[i];
            const char *cpio_path = sep + 1;
            if (mount_cpio(target, cpio_path) != 0)
                return 1;
            i++;
        } else {
            break;
        }
    }

    if (i >= argc) {
        fprintf(stderr, "%s: no command specified\n", argv[0]);
        return 1;
    }

    execvp(argv[i], &argv[i]);
    perror(argv[i]);
    return 127;
}
