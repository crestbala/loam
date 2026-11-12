// Open-addressing string->int32 map mirroring the Loam energy workload:
// 200k inserts of "k<i>" then 200k lookups summed into an i64.
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static uint64_t fnv1a(const char *s) {
    uint64_t h = 1469598103934665603ULL;
    for (; *s; s++) { h ^= (unsigned char)*s; h *= 1099511628211ULL; }
    return h;
}

typedef struct { char *key; int32_t val; char used; } Slot;

int main(void) {
    size_t cap = 1 << 19; // 524288 > 2x 200k
    Slot *t = calloc(cap, sizeof *t);
    if (!t) return 1;
    char kb[32];
    for (int i = 0; i < 200000; i++) {
        snprintf(kb, sizeof kb, "k%d", i);
        uint64_t h = fnv1a(kb);
        size_t j = h & (cap - 1);
        while (t[j].used) {
            if (strcmp(t[j].key, kb) == 0) { t[j].val = i; break; }
            j = (j + 1) & (cap - 1);
        }
        if (!t[j].used) { t[j].key = strdup(kb); t[j].val = i; t[j].used = 1; }
    }
    int64_t s = 0;
    size_t live = 0;
    for (size_t j = 0; j < cap; j++) if (t[j].used) live++;
    for (int i = 0; i < 200000; i++) {
        snprintf(kb, sizeof kb, "k%d", i);
        uint64_t h = fnv1a(kb);
        size_t j = h & (cap - 1);
        while (t[j].used) {
            if (strcmp(t[j].key, kb) == 0) { s += (int64_t)t[j].val; break; }
            j = (j + 1) & (cap - 1);
        }
    }
    printf("%zu %lld\n", live, (long long)s);
    return 0;
}
