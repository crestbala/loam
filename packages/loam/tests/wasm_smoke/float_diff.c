/* The wasm libc's exact `%g` / `strtod` (the wasm-float block of
   runtime/zeus_wasm_libc.c, included as WASM_FLOAT_BLOCK) against the host
   libc, on random doubles, every precision, exact midpoints, and edge strings.
   macOS libc keeps trailing zeros after an exact tie (`%.3g` of 2605 gives
   2.60e+03); C requires them stripped, so the libc side is normalized. */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define strtod wf_strtod
#include WASM_FLOAT_BLOCK
#undef strtod

static uint64_t rs = 88172645463325252ull;
static uint64_t rnd(void) {
    rs ^= rs << 13;
    rs ^= rs >> 7;
    rs ^= rs << 17;
    return rs;
}

static double rdouble(void) {
    uint64_t u = rnd();
    double d;
    switch (rnd() % 6) {
        case 0: u &= 0x000fffffffffffffull; break;
        case 1: u = (u & 0x800fffffffffffffull) | ((uint64_t)(1023 + (int)(rnd() % 60) - 30) << 52); break;
        case 2:
            d = (double)(rnd() % 1000000) / (double)(1 + rnd() % 1000);
            memcpy(&u, &d, 8);
            break;
        default: break;
    }
    memcpy(&d, &u, 8);
    return d;
}

static void strip_zeros(char *s) {
    char *e = strchr(s, 'e'), *dot = strchr(s, '.');
    if (!dot) return;
    char *end = e ? e : s + strlen(s), *q = end;
    while (q > dot + 1 && q[-1] == '0') q--;
    if (q == dot + 1) q = dot;
    memmove(q, end, strlen(end) + 1);
}

int main(int argc, char **argv) {
    long n = 0, bad = 0, iters = argc > 1 ? atol(argv[1]) : 50000;
    char a[64], b[64], s[128];
    for (long i = 0; i < iters; i++) {
        double v = rdouble();
        if (isnan(v)) continue;
        for (int p = 1; p <= 17; p++) {
            snprintf(a, sizeof a, "%.*g", p, v);
            strip_zeros(a);
            b[wf_fmt_g(v, p, b)] = 0;
            n++;
            if (strcmp(a, b) && bad++ < 5) printf("fmt %.17g p=%d libc=%s wasm=%s\n", v, p, a, b);
        }
        snprintf(s, sizeof s, "%.*e", 1 + (int)(rnd() % 25), v);
        double x = strtod(s, NULL), y = wf_strtod(s, NULL);
        n++;
        if (memcmp(&x, &y, 8) && bad++ < 10) printf("parse %s libc=%.17g wasm=%.17g\n", s, x, y);
        if (isfinite(v) && v != 0) {
            long double mid = ((long double)v + (long double)nextafter(v, INFINITY)) / 2;
            snprintf(s, sizeof s, "%.40Le", mid);
            x = strtod(s, NULL);
            y = wf_strtod(s, NULL);
            n++;
            if (memcmp(&x, &y, 8) && bad++ < 15) printf("midpoint %s libc=%.17g wasm=%.17g\n", s, x, y);
        }
    }
    const char *edge[] = {"0", "-0", "1e400", "-1e400", "1e-400", "4.9406564584124654e-324",
                          "2.4703282292062327e-324", "2.4703282292062328e-324",
                          "1.7976931348623157e308", "1.7976931348623158e308", "1.7976931348623159e308",
                          "123456789012345678901234567890", "000.000", "1.", "0.5e1", "inf",
                          "-Infinity", "9007199254740993", "2.2250738585072011e-308",
                          "0.1000000000000000055511151231257827021181583404541015625", 0};
    for (int i = 0; edge[i]; i++) {
        char *e1, *e2;
        double x = strtod(edge[i], &e1), y = wf_strtod(edge[i], &e2);
        n++;
        if (memcmp(&x, &y, 8) || e1 - edge[i] != e2 - edge[i]) {
            bad++;
            printf("edge %s libc=%.17g wasm=%.17g\n", edge[i], x, y);
        }
    }
    printf("%ld checks, %ld mismatches\n", n, bad);
    return bad != 0;
}
