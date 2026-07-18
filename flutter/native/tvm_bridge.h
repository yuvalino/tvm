#ifndef TVM_BRIDGE_H
#define TVM_BRIDGE_H

#define TVM_EXPORT __attribute__((visibility("default"), used))

int tvm_load(const char *libtvm_path);
int tvm_start(int port, const char *password);
int tvm_poll(char *buf, int buf_size);
void tvm_stop(void);

#endif
