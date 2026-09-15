#ifndef LOAM_WASM_SYS_UIO_H
#define LOAM_WASM_SYS_UIO_H
#include <stddef.h>
struct iovec {
    void *iov_base;
    size_t iov_len;
};
ssize_t writev(int fd, const struct iovec *iov, int n);
#endif
