// Tiny launcher for TVM Flutter initrd.
// Each launcher is compiled with -DTARGET="libfoo" and placed
// in the initrd at /tvm/bin/<name>. When executed (under the emulator),
// it execs the binary of libfoo.framework in the app bundle's Frameworks
// directory, which the app names in TVM_FRAMEWORKS.
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#ifndef TARGET
#error "Define TARGET as the framework binary's name, e.g. -DTARGET='\"libbash\"'"
#endif

int main(int argc, char **argv, char **envp)
{
    const char *frameworks = getenv("TVM_FRAMEWORKS");
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/" TARGET ".framework/" TARGET, frameworks ? frameworks : "");
    execv(path, argv);
    perror(path);
    return 127;
}
