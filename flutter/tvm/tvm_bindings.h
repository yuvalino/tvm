#ifndef TVM_BINDINGS_H
#define TVM_BINDINGS_H

#define TVM_EXPORT __attribute__((visibility("default"), used))

// Called from the reader thread whenever TVM produces output.
// `data` is a heap buffer of `len` bytes that the callee takes ownership
// of and must free() (matches the malloc in the reader thread).
typedef void (*tvm_output_cb)(const char *data, int len);

// Launches the TVM worker + output reader threads. Returns 0 on success.
// Loads libtvm.dylib from libtvm_path, builds the guest environment, and
// boots the VM with the initrd mounted at /tvm and dropbear listening on
// the given port. Does not return until the threads are spawned (the worker
// runs for the app's lifetime).
//
// Named tvm_app_start (not tvm_start) because libtvm.dylib exports the
// kernel's tvm_start into the same process and Dart looks this one up by
// name through DynamicLibrary.process().
TVM_EXPORT int tvm_app_start(const char *libtvm_path, int port,
                             const char *password, const char *initrc_path,
                             const char *initrd_path, tvm_output_cb on_output);

#endif
