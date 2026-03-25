/**
 * Standalone Keccak-256 implementation for Kaspa mining.
 *
 * This replaces the Arduino KeccakCore library with a platform-independent
 * implementation suitable for Android NDK.
 *
 * Kaspa uses Keccak-256 (NOT SHA3-256 — no 0x06 domain separator).
 * Capacity = 512 bits, rate = 1088 bits, output = 256 bits.
 * Padding byte = 0x01 (standard Keccak, not SHA3's 0x06).
 */

#ifndef KECCAK_H
#define KECCAK_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Compute Keccak-256 hash.
 *
 * @param input   Input data
 * @param inlen   Length of input data in bytes
 * @param output  Output buffer (must be at least 32 bytes)
 */
void keccak256(const uint8_t *input, size_t inlen, uint8_t *output);

#ifdef __cplusplus
}
#endif

#endif /* KECCAK_H */
