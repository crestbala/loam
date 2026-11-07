// An ABI trampoline, with no logic: crypto_argon2 takes structs by value, which
// `#[c]` does not pass. See docs/boundary.md.
#include "monocypher.h"

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
