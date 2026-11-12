#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
int main(void) {
    size_t len = 0, cap = 1024;
    int32_t *v = malloc(cap * sizeof *v);
    if (!v) return 1;
    for (int32_t i = 0; i < 1000000; i++) {
        if (len == cap) { cap *= 2; v = realloc(v, cap * sizeof *v); if (!v) return 1; }
        v[len++] = i;
    }
    int64_t s = 0;
    for (size_t i = 0; i < len; i++) s += (int64_t)v[i]; // no overflow
    printf("%zu %lld\n", len, (long long)s);
    free(v);
    return 0;
}
