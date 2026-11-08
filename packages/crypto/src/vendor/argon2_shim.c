// ABI trampolines, with no logic: crypto_argon2 takes structs by value, which
// `#[c]` does not pass, and the OS entropy call differs per platform. See
// docs/boundary.md.
#ifdef _WIN32
#define _CRT_RAND_S /* before any CRT header: declares rand_s */
#include <stdlib.h>
#else
#include <unistd.h>
#ifdef __APPLE__
#include <sys/random.h>
#endif
#endif
#include "monocypher.h"

/* `n` (at most 256) bytes from the OS CSPRNG. 0 on success, like getentropy.
   Windows: rand_s, which draws from RtlGenRandom and needs no extra library. */
int shim_crypto_entropy(uint8_t *out, int64_t n) {
#ifdef _WIN32
    int64_t i = 0;
    while (i < n) {
        unsigned int r;
        if (rand_s(&r) != 0) return -1;
        for (int k = 0; k < 4 && i < n; k++, i++) out[i] = (uint8_t)(r >> (8 * k));
    }
    return 0;
#else
    return getentropy(out, (size_t)n);
#endif
}

void shim_crypto_argon2id(uint8_t *hash, uint32_t hash_size, void *work_area, uint32_t nb_blocks,
                          uint32_t nb_passes, uint32_t nb_lanes, const uint8_t *pass,
                          uint32_t pass_size, const uint8_t *salt, uint32_t salt_size,
                          const uint8_t *key, uint32_t key_size, const uint8_t *ad,
                          uint32_t ad_size) {
    crypto_argon2_config config = {CRYPTO_ARGON2_ID, nb_blocks, nb_passes, nb_lanes};
    crypto_argon2_inputs inputs = {pass, salt, pass_size, salt_size};
    crypto_argon2_extras extras = {key, ad, key_size, ad_size};
    crypto_argon2(hash, hash_size, work_area, config, inputs, extras);
}
