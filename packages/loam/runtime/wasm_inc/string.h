#ifndef LOAM_WASM_STRING_H
#define LOAM_WASM_STRING_H
#include <stddef.h>
/* Bulk memory (`-mbulk-memory`): each of these is one `memory.copy` or
   `memory.fill` instruction, not a call, so no libc defines them. */
#define memcpy __builtin_memcpy
#define memmove __builtin_memmove
#define memset __builtin_memset
int memcmp(const void *a, const void *b, size_t n);
size_t strlen(const char *s);
#endif
