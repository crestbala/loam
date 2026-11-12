#include <stdio.h>
#include <stdint.h>
// LCG-style accumulation mirroring the Loam twin: a multiply-add chain the
// optimizer must execute (no closed form). uint64_t arithmetic wraps.
int main(void) {
    uint64_t s = 0;
    for (uint64_t i = 0; i < 1000000000ULL; i++)
        s = s * 6364136223846793005ULL + i + 1442695040888963407ULL;
    printf("%lld\n", (long long)(int64_t)s);
    return 0;
}
