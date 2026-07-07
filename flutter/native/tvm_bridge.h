#ifndef TVM_BRIDGE_H
#define TVM_BRIDGE_H

#ifdef _WIN32
#define TVM_EXPORT __declspec(dllexport)
#else
#define TVM_EXPORT __attribute__((visibility("default"), used))
#endif

#ifdef __cplusplus
extern "C" {
#endif

TVM_EXPORT int tvm_load(const char *libtvm_path);
TVM_EXPORT int tvm_start(int port, const char *password);
TVM_EXPORT int tvm_poll(char *buf, int buf_size);
TVM_EXPORT void tvm_stop(void);

#ifdef __cplusplus
}
#endif

#endif
