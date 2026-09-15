#ifndef YUGA_WASM_UNISTD_H
#define YUGA_WASM_UNISTD_H
#include <stddef.h>
typedef long ssize_t;
#define STDOUT_FILENO 1
#define STDERR_FILENO 2
ssize_t write(int fd, const void *p, size_t n);
#endif
