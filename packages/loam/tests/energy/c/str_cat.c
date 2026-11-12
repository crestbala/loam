#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int main(void) {
    size_t len = 0, cap = 1 << 20;
    char *b = malloc(cap);
    if (!b) return 1;
    const char *piece = "hello, loam ";
    size_t pl = strlen(piece);
    for (int i = 0; i < 100000; i++) {
        if (len + pl > cap) { cap *= 2; b = realloc(b, cap); if (!b) return 1; }
        memcpy(b + len, piece, pl);
        len += pl;
    }
    printf("%zu\n", len);
    free(b);
    return 0;
}
