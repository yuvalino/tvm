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

static void *read_file(const char *path, size_t *out_size)
{
    int fd = open(path, O_RDONLY);
    if (fd == -1) {
        perror(path);
        return NULL;
    }

    struct stat st;
    if (fstat(fd, &st) == -1) {
        perror(path);
        close(fd);
        return NULL;
    }

    void *buf = malloc(st.st_size);
    if (!buf) {
        perror("malloc");
        close(fd);
        return NULL;
    }

    size_t total = 0;
    while (total < (size_t)st.st_size) {
        ssize_t n = read(fd, (char *)buf + total, st.st_size - total);
        if (n <= 0) {
            perror("read");
            free(buf);
            close(fd);
            return NULL;
        }
        total += n;
    }
    close(fd);

    *out_size = total;
    return buf;
}

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

    // -i/-n become guest environment variables. The launcher's own environ is
    // the guest environ (tvm_start envp=NULL), so plain setenv is enough.
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

    // -v mounts are read up front and handed to tvm_start, which mounts them
    // before the initrc runs.
    struct tvm_mount *mounts = calloc(argc, sizeof(*mounts));
    if (!mounts) {
        perror("calloc");
        return 1;
    }
    size_t nmounts = 0;
    for (int j = 1; j < i; j++) {
        if (strcmp(argv[j], "-v") != 0)
            continue;
        j++;
        char *sep = strchr(argv[j], ':');
        if (!sep) {
            fprintf(stderr, "%s: -v argument must be PATH:CPIO (got '%s')\n", argv[0], argv[j]);
            return 1;
        }
        *sep = '\0';
        size_t size;
        void *data = read_file(sep + 1, &size);
        if (!data)
            return 1;
        mounts[nmounts].path = argv[j];
        mounts[nmounts].data = data;
        mounts[nmounts].size = size;
        nmounts++;
    }

    int log_fd = STDOUT_FILENO;
    if (!verbose) {
        log_fd = open("/dev/null", O_WRONLY);
        if (log_fd == -1) {
            perror("/dev/null");
            return 1;
        }
    }

    struct tvm_config cfg = {
        .log_fd = log_fd,
        .cwd = NULL,
        .mounts = mounts,
        .nmounts = nmounts,
    };

    // Never returns on success; the program's exit() ends the process.
    tvm_start(argv[i], &argv[i], NULL, &cfg);
    perror("tvm_start");
    return 1;
}
