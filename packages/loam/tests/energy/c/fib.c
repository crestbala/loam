#include <stdio.h>
#include <stdint.h>
// Wrapping add (uint64_t arithmetic is defined to wrap), printed as i64.
int main(void) {
    uint64_t a = 0, b = 1;
    for (int i = 0; i < 100000000; i++) { uint64_t t = a + b; a = b; b = t; }
    printf("%lld\n", (long long)(int64_t)b);
    return 0;
}
