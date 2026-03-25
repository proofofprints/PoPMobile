/**
 * kHeavyHash implementation for Kaspa mining.
 *
 * Ported from KASDeck Arduino (ESP32) code to platform-independent C.
 * Matches the kaspa-miner CUDA reference implementation.
 *
 * Copyright (c) 2026 Proof of Prints
 */

#ifndef KHEAVYHASH_H
#define KHEAVYHASH_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Heavy matrix (64x64 of 4-bit values stored as uint16_t).
 */
typedef struct {
    uint16_t data[64][64];
} HeavyMatrix;

/**
 * kHeavyHash state — holds the matrix and caching info.
 */
typedef struct {
    HeavyMatrix matrix;
    uint8_t last_hash1[32];
    bool matrix_valid;
} KHeavyHashState;

/**
 * Initialize a KHeavyHashState (must call before first use).
 */
void kheavyhash_init(KHeavyHashState *state);

/**
 * Compute kHeavyHash on an 80-byte input (72-byte header + 8-byte nonce).
 *
 * @param state   Persistent state (caches matrix between calls)
 * @param input   80-byte work buffer
 * @param output  32-byte hash output
 */
void kheavyhash_compute(KHeavyHashState *state, const uint8_t *input, uint8_t *output);

/**
 * Check if a hash meets the target difficulty.
 * Returns true if hash <= target (big-endian byte comparison).
 */
bool check_difficulty(const uint8_t *hash, const uint8_t *target);

/**
 * Set target bytes from a floating-point difficulty value.
 *
 * @param difficulty  Pool difficulty (can be fractional for low-diff pools)
 * @param target      32-byte output target
 */
void set_target_from_difficulty(double difficulty, uint8_t *target);

#ifdef __cplusplus
}
#endif

#endif /* KHEAVYHASH_H */
