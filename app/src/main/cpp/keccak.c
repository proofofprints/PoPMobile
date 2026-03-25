/**
 * Standalone Keccak-256 implementation for Kaspa mining.
 *
 * Ported from the reference Keccak implementation.
 * Uses capacity=512, rate=1088, padding=0x01, output=256 bits.
 * This matches the Arduino KeccakCore behavior used in KASDeck.
 *
 * Copyright (c) 2026 Proof of Prints
 */

#include "keccak.h"
#include <string.h>

/* Keccak-f[1600] round constants */
static const uint64_t RC[24] = {
    0x0000000000000001ULL, 0x0000000000008082ULL,
    0x800000000000808AULL, 0x8000000080008000ULL,
    0x000000000000808BULL, 0x0000000080000001ULL,
    0x8000000080008081ULL, 0x8000000000008009ULL,
    0x000000000000008AULL, 0x0000000000000088ULL,
    0x0000000080008009ULL, 0x000000008000000AULL,
    0x000000008000808BULL, 0x800000000000008BULL,
    0x8000000000008089ULL, 0x8000000000008003ULL,
    0x8000000000008002ULL, 0x8000000000000080ULL,
    0x000000000000800AULL, 0x800000008000000AULL,
    0x8000000080008081ULL, 0x8000000000008080ULL,
    0x0000000080000001ULL, 0x8000000080008008ULL
};

/* Rotation offsets */
static const int ROTC[24] = {
     1,  3,  6, 10, 15, 21, 28, 36,
    45, 55,  2, 14, 27, 41, 56,  8,
    25, 43, 62, 18, 39, 61, 20, 44
};

/* Pi lane permutation */
static const int PILN[24] = {
    10,  7, 11, 17, 18,  3,  5, 16,
     8, 21, 24,  4, 15, 23, 19, 13,
    12,  2, 20, 14, 22,  9,  6,  1
};

static inline uint64_t rotl64(uint64_t x, int y) {
    return (x << y) | (x >> (64 - y));
}

/* Keccak-f[1600] permutation */
static void keccakf(uint64_t state[25]) {
    uint64_t t, bc[5];

    for (int round = 0; round < 24; round++) {
        /* Theta */
        for (int i = 0; i < 5; i++)
            bc[i] = state[i] ^ state[i + 5] ^ state[i + 10] ^ state[i + 15] ^ state[i + 20];

        for (int i = 0; i < 5; i++) {
            t = bc[(i + 4) % 5] ^ rotl64(bc[(i + 1) % 5], 1);
            for (int j = 0; j < 25; j += 5)
                state[j + i] ^= t;
        }

        /* Rho and Pi */
        t = state[1];
        for (int i = 0; i < 24; i++) {
            int j = PILN[i];
            bc[0] = state[j];
            state[j] = rotl64(t, ROTC[i]);
            t = bc[0];
        }

        /* Chi */
        for (int j = 0; j < 25; j += 5) {
            for (int i = 0; i < 5; i++)
                bc[i] = state[j + i];
            for (int i = 0; i < 5; i++)
                state[j + i] ^= (~bc[(i + 1) % 5]) & bc[(i + 2) % 5];
        }

        /* Iota */
        state[0] ^= RC[round];
    }
}

void keccak256(const uint8_t *input, size_t inlen, uint8_t *output) {
    uint64_t state[25];
    uint8_t temp[136]; /* rate = 1088 bits = 136 bytes */
    const size_t rate = 136;

    memset(state, 0, sizeof(state));

    /* Absorb full blocks */
    while (inlen >= rate) {
        for (size_t i = 0; i < rate / 8; i++) {
            uint64_t lane;
            memcpy(&lane, input + i * 8, 8);
            state[i] ^= lane;
        }
        keccakf(state);
        input += rate;
        inlen -= rate;
    }

    /* Absorb final block with padding */
    memset(temp, 0, rate);
    memcpy(temp, input, inlen);
    temp[inlen] = 0x01;       /* Keccak padding (NOT SHA3's 0x06) */
    temp[rate - 1] |= 0x80;   /* Final bit of pad10*1 */

    for (size_t i = 0; i < rate / 8; i++) {
        uint64_t lane;
        memcpy(&lane, temp + i * 8, 8);
        state[i] ^= lane;
    }
    keccakf(state);

    /* Squeeze: extract 32 bytes (256 bits) */
    memcpy(output, state, 32);
}
