/* The host libc's answers for `std:floattext`'s differential test
   (float_check.loam): random doubles of every shape, each as `%.<p>g` for
   p in 1..17 and as the shortest of %.15g / %.16g / %.17g that reads back;
   then strtod over `%.*e` texts, exact midpoints and edge strings.
   macOS libc keeps trailing zeros after an exact tie (`%.3g` of 2605 gives
   2.60e+03); C requires them stripped, so the libc side is normalized.

   Lines: `F <bits> <p1> .. <p17> <shortest>` and `P <text> <bits>`. */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
        case 3:
            d = (double)(rnd() % 2 ? rnd() % 10000 : rnd() % 100000000000000000ull);
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

static unsigned long long bits(double v) {
    uint64_t u;
    memcpy(&u, &v, 8);
    return (unsigned long long)u;
}

static void parse_line(const char *s) { printf("P %s %llu\n", s, bits(strtod(s, NULL))); }

int main(int argc, char **argv) {
    long iters = argc > 1 ? atol(argv[1]) : 20000;
    char a[64], s[128];
    for (long i = 0; i < iters; i++) {
        double v = rdouble();
        if (isnan(v)) continue;
        printf("F %llu", bits(v));
        for (int p = 1; p <= 17; p++) {
            snprintf(a, sizeof a, "%.*g", p, v);
            strip_zeros(a);
            printf(" %s", a);
        }
        for (int p = 15; p <= 17; p++) {
            snprintf(a, sizeof a, "%.*g", p, v);
            if (p == 17 || strtod(a, NULL) == v) break;
        }
        printf(" %s\n", a);
        snprintf(s, sizeof s, "%.*e", 1 + (int)(rnd() % 25), v);
        parse_line(s);
        if (isfinite(v) && v != 0) {
            long double mid = ((long double)v + (long double)nextafter(v, INFINITY)) / 2;
            snprintf(s, sizeof s, "%.40Le", mid);
            parse_line(s);
            snprintf(s, sizeof s, "%.17g", v);
            parse_line(s);
        }
    }
    const char *edge[] = {"0", "-0", "1e400", "-1e400", "1e-400", "4.9406564584124654e-324",
                          "2.4703282292062327e-324", "2.4703282292062328e-324", "3e-324", "7e-324",
                          "1.7976931348623157e308", "1.7976931348623158e308", "1.7976931348623159e308",
                          "123456789012345678901234567890", "000.000", "1.", "0.5e1", "inf",
                          "-Infinity", "9007199254740993", "2.2250738585072011e-308",
                          "2.2250738585072014e-308", "4.4501477170144023e-308", "1e23",
                          "0.1000000000000000055511151231257827021181583404541015625",
                          "1.000000000000000111022302462515654042363166809082031251",
                          "0.000000000000000000000000000001", "12345678901234567890123", 0};
    for (int i = 0; edge[i]; i++) parse_line(edge[i]);
    return 0;
}
