This is TVM: Thread-based process virtualization framework that lets sandboxed applicatiosn with no fork() permissions to POSIX compliant programs.

The basic idea here is that fork() creates a new thread group (struct task) that shares resources such as file descriptors and signal handlers. To imitate Copy-On-Write, on fork() memory ranges are duplicated and pointers to old ranges are fixed up to point to the equivilant new ranges.

Dedicated heap and linker were written in tvm/rmem.c and tvm/ulinker.c to optimize the fork() process to be as fast as possible.

The entire VM is implemented in tvm/tvm.c .

In addition, unsigned code execution is made possible when code signing is validated by running the unsigned code parts using Unicorn emulator engine. When the emulator hits a signed region, it traps and lets the real CPU execute that code. The emulator integration too is implemented in tvm/tvm.c .

