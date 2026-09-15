#ifndef YUGA_WASM_STRINGS_H
#define YUGA_WASM_STRINGS_H
#include <string.h>
static inline int strcasecmp(const char *a, const char *b) {
    return strcmp(a, b);
}
#endif
