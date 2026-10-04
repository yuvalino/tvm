// Tiny launcher for TVM Flutter initrd.
// Each launcher is compiled with -DTARGET="libfoo.dylib" and placed
// in the initrd at /tvm/bin/<name>. When executed (under the emulator),
// it execs the target dylib which the ulinker finds via PATH.
#include <unistd.h>
#include <stdio.h>

#ifndef TARGET
#error "Define TARGET as the dylib name, e.g. -DTARGET='\"libbash.dylib\"'"
#endif

int main(int argc, char **argv, char **envp)
{
    execvp(TARGET, argv);
    perror(TARGET);
    return 127;
}
