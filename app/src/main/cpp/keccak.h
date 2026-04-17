/**
 * Keccak and cSHAKE256 implementation for Kaspa mining.
 *
 * Kaspa's rusty-kaspa uses cSHAKE256 (NIST SP 800-185) with domain
 * separation for its hash functions:
 *   - "ProofOfWorkHash" for outer PoW hash (80 bytes input)
 *   - "HeavyHash" for inner heavy hash (32 bytes input)
 *
 * Copyright (c) 2026 Proof of Prints
 */

#ifndef KECCAK_H
#define KECCAK_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Compute cSHAKE256 one-shot hash with customization string.
 * cSHAKE256(name, data) -> 32 bytes output.
 *
 * @param name      Customization string (e.g., "ProofOfWorkHash", "HeavyHash")
 * @param name_len  Length of name in bytes
 * @param data      Input data
 * @param data_len  Length of data (must be <= 136 bytes for single-block)
 * @param output    32-byte output buffer
 */
void cshake256_hash(const uint8_t *name, size_t name_len,
                    const uint8_t *data, size_t data_len,
                    uint8_t *output);

/**
 * Keccak-f[1600] permutation.
 */
void keccakf(uint64_t state[25]);

#ifdef __cplusplus
}
#endif

#endif /* KECCAK_H */
