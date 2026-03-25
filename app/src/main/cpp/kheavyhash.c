/**
 * kHeavyHash implementation for Kaspa mining.
 *
 * Ported from KASDeck Arduino (ESP32) code to platform-independent C.
 * Matches the kaspa-miner CUDA reference implementation.
 *
 * The algorithm:
 *   1. Keccak-256 of 80-byte input → hash1
 *   2. Generate 64x64 matrix from hash1 via XoShiRo256**
 *   3. Split hash1 into 64 nibbles (4-bit values)
 *   4. Matrix-vector multiply with specific shift/mask
 *   5. XOR product with hash1
 *   6. Keccak-256 of result → final hash
 *
 * Copyright (c) 2026 Proof of Prints
 */

#include "kheavyhash.h"
#include "keccak.h"
#include <string.h>

/* ===== XoShiRo256** PRNG (for matrix generation) ===== */

typedef struct {
    uint64_t s[4];
} XoShiRo256StarStar;

static inline uint64_t xoshiro_rotl(uint64_t x, int k) {
    return (x << k) | (x >> (64 - k));
}

static void xoshiro_init(XoShiRo256StarStar *rng, const uint8_t *hash) {
    for (int i = 0; i < 4; i++) {
        rng->s[i] = 0;
        for (int j = 0; j < 8; j++) {
            rng->s[i] |= ((uint64_t)hash[i * 8 + j]) << (j * 8);
        }
    }
}

static uint64_t xoshiro_next(XoShiRo256StarStar *rng) {
    /* StarStar (**) variant: (s[1] * 5 rotated by 7) * 9 */
    const uint64_t result = xoshiro_rotl(rng->s[1] * 5, 7) * 9;
    const uint64_t t = rng->s[1] << 17;

    rng->s[2] ^= rng->s[0];
    rng->s[3] ^= rng->s[1];
    rng->s[1] ^= rng->s[2];
    rng->s[0] ^= rng->s[3];
    rng->s[2] ^= t;
    rng->s[3] = xoshiro_rotl(rng->s[3], 45);

    return result;
}

/* ===== Matrix generation ===== */

static void matrix_generate(HeavyMatrix *matrix, const uint8_t *hash) {
    XoShiRo256StarStar rng;
    xoshiro_init(&rng, hash);

    for (int i = 0; i < 64; i++) {
        for (int j = 0; j < 64; j += 16) {
            uint64_t val = xoshiro_next(&rng);
            for (int shift = 0; shift < 16; shift++) {
                matrix->data[i][j + shift] = (val >> (4 * shift)) & 0x0F;
            }
        }
    }
}

/* ===== kHeavyHash core ===== */

void kheavyhash_init(KHeavyHashState *state) {
    memset(state->last_hash1, 0, 32);
    state->matrix_valid = false;
}

void kheavyhash_compute(KHeavyHashState *state, const uint8_t *input, uint8_t *output) {
    uint8_t hash1[32];
    uint8_t vec[64];
    uint8_t product[32];

    /* Step 1: First Keccak-256 of 80-byte input */
    keccak256(input, 80, hash1);

    /* Step 2: Generate matrix if hash changed (cache optimization) */
    if (!state->matrix_valid || memcmp(hash1, state->last_hash1, 32) != 0) {
        matrix_generate(&state->matrix, hash1);
        memcpy(state->last_hash1, hash1, 32);
        state->matrix_valid = true;
    }

    /* Step 3: Split hash into 64 nibbles (4-bit values) */
    for (int i = 0; i < 32; i++) {
        vec[2 * i]     = hash1[i] >> 4;        /* Upper nibble */
        vec[2 * i + 1] = hash1[i] & 0x0F;      /* Lower nibble */
    }

    /* Step 4: Matrix-vector multiplication with asymmetric shifts */
    for (int i = 0; i < 32; i++) {
        uint16_t sum1 = 0;
        uint16_t sum2 = 0;

        for (int j = 0; j < 64; j++) {
            sum1 += state->matrix.data[2 * i][j] * vec[j];
            sum2 += state->matrix.data[2 * i + 1][j] * vec[j];
        }

        /* CORRECTED shifts and masks (from KASDeck):
         * sum1 uses shift 6 and keeps upper nibble (0xF0)
         * sum2 uses shift 10 and keeps lower nibble (0x0F) */
        uint8_t upper = (sum1 >> 6) & 0xF0;
        uint8_t lower = (sum2 >> 10) & 0x0F;
        product[i] = upper | lower;
    }

    /* Step 5: XOR with original hash */
    for (int i = 0; i < 32; i++) {
        product[i] ^= hash1[i];
    }

    /* Step 6: Final Keccak-256 */
    keccak256(product, 32, output);
}

/* ===== Difficulty utilities ===== */

bool check_difficulty(const uint8_t *hash, const uint8_t *target) {
    for (int i = 0; i < 32; i++) {
        if (hash[i] < target[i]) return true;
        if (hash[i] > target[i]) return false;
    }
    return true; /* Exactly equal */
}

void set_target_from_difficulty(double difficulty, uint8_t *target) {
    memset(target, 0, 32);

    if (difficulty <= 0) difficulty = 0.0001;

    /* Difficulty 1 target: 00000000ffff0000...00 */
    uint8_t diff1[32] = {0};
    diff1[4] = 0xff;
    diff1[5] = 0xff;

    if (difficulty >= 1.0) {
        memcpy(target, diff1, 32);
    } else {
        /* Fractional difficulty — scale target up (easier) */
        double scale = 1.0 / difficulty;
        uint32_t base_value = 0xffff;
        uint64_t scaled = (uint64_t)(base_value * scale);

        if (scaled > 0xFFFFFFFFFFFFULL) scaled = 0xFFFFFFFFFFFFULL;

        target[0] = (scaled >> 40) & 0xFF;
        target[1] = (scaled >> 32) & 0xFF;
        target[2] = (scaled >> 24) & 0xFF;
        target[3] = (scaled >> 16) & 0xFF;
        target[4] = (scaled >> 8) & 0xFF;
        target[5] = scaled & 0xFF;
    }
}
