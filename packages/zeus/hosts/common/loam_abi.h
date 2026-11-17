/* loam_abi.h — the C ABI of Loam values, for C linked into a Loam program
 * (the Zeus hosts, an app's `runtime/` seam). It is all such C may assume:
 * two value layouts, the panic message buffer, and three exports that share,
 * copy and drop a value the way generated code does. Codegen defines the
 * exports when a program has a C seam (a bodyless fn outside the language
 * std); the `host_contract` stanza compiles generated C with this header
 * after it, so the layouts and prototypes below are checked against it.
 */
#ifndef LOAM_ABI_H
#define LOAM_ABI_H

#include <stdint.h>
#include <stddef.h>

#ifdef __wasm32__
/* No libc on wasm: the program defines these (codegen's wasm C library). */
void *malloc(size_t n);
void *realloc(void *p, size_t n);
void free(void *p);
__attribute__((noreturn)) void abort(void);
int memcmp(const void *a, const void *b, size_t n);
size_t strlen(const char *s);
long write(int fd, const void *p, size_t n);
#define memcpy __builtin_memcpy
#define memmove __builtin_memmove
#define memset __builtin_memset
#else
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#endif

/* Generated C defines the same two types and `LOAM_ABI_TYPES`. */
#ifndef LOAM_ABI_TYPES
#define LOAM_ABI_TYPES 1
/* A string: `own` 1 when `ptr` is a refcounted heap buffer, 0 for static
   storage. Hand Loam an owned one from `loam_abi_str_copy`. */
typedef struct {
    const char *ptr;
    int64_t len;
    int64_t own;
} loam_str;

/* A `[]T`: `ptr` is a refcounted buffer of `len` elements. */
typedef struct {
    void *ptr;
    int64_t len;
    int64_t cap;
} loam_vec;
#endif

_Static_assert(sizeof(loam_str) == 24 && __builtin_offsetof(loam_str, len) == 8 && __builtin_offsetof(loam_str, own) == 16,
               "loam_str layout");
_Static_assert(sizeof(loam_vec) == 24 && __builtin_offsetof(loam_vec, len) == 8 && __builtin_offsetof(loam_vec, cap) == 16,
               "loam_vec layout");

/* Why the last panic happened, NUL-terminated (wasm reads it after a trap). */
extern char loam_jmp_msg[512];

/* An owned copy of `n` bytes at `p` (empty for `n <= 0`). A Loam parameter is
   borrowed: release the copy after the call. */
loam_str loam_abi_str_copy(const char *p, int64_t n);
/* Drop one reference to a string Loam returned; the last one frees it. */
void loam_abi_str_release(loam_str *s);
/* Drop one reference to a vec Loam returned. */
void loam_abi_vec_drop(loam_vec *v);

#endif /* LOAM_ABI_H */
