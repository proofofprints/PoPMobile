/**
 * Keccak-f[1600] and cSHAKE256 implementation for Kaspa mining.
 *
 * Implements:
 *   - Keccak-f[1600] permutation
 *   - cSHAKE256 (NIST SP 800-185) with domain separation
 *
 * cSHAKE256 is used by rusty-kaspa for:
 *   - "ProofOfWorkHash": outer PoW hash (80 bytes input)
 *   - "HeavyHash": inner heavy hash after matrix multiply (32 bytes input)
 *
 * Padding byte is 0x04 (cSHAKE), NOT 0x01 (Keccak) or 0x06 (SHA-3).
 *
 * Copyright (c) 2026 Proof of Prints
 */

#include "keccak.h"
#include <string.h>

#define KECCAK_RATE 136  /* rate in bytes for 256-bit security (1088 bits) */

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
void keccakf(uint64_t state[25]) {
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

/* ===== cSHAKE256 (NIST SP 800-185) ===== */

void cshake256_hash(const uint8_t *name, size_t name_len,
                    const uint8_t *data, size_t data_len,
                    uint8_t *output) {
    uint64_t state[25];
    memset(state, 0, sizeof(state));

    /* 1. Build and absorb bytepad block (domain separation)
     *
     * bytepad(encode_string("") || encode_string(N), 136):
     *   left_encode(136) = [0x01, 0x88]
     *   encode_string("") = left_encode(0) = [0x01, 0x00]
     *   encode_string(N) = left_encode(len(N)*8) || N
     *   Pad with zeros to 136 bytes
     */
    uint8_t header[KECCAK_RATE];
    size_t pos = 0;
    memset(header, 0, KECCAK_RATE);

    /* left_encode(136) */
    header[pos++] = 1;
    header[pos++] = 136;

    /* encode_string("") = left_encode(0) */
    header[pos++] = 1;
    header[pos++] = 0;

    /* encode_string(name) = left_encode(name_len*8) || name */
    uint64_t bitlen = name_len * 8;
    if (bitlen < 256) {
        header[pos++] = 1;
        header[pos++] = (uint8_t)bitlen;
    } else {
        header[pos++] = 2;
        header[pos++] = (uint8_t)(bitlen >> 8);
        header[pos++] = (uint8_t)(bitlen & 0xFF);
    }
    memcpy(header + pos, name, name_len);
    /* Rest is already zeroed — header is padded to 136 bytes */

    /* XOR bytepad block into state and permute */
    for (int i = 0; i < KECCAK_RATE / 8; i++) {
        uint64_t lane;
        memcpy(&lane, header + i * 8, 8);
        state[i] ^= lane;
    }
    keccakf(state);

    /* 2. Absorb data + finalize with cSHAKE256 padding (0x04)
     *
     * For our use cases, data_len <= 136 (80 for PowHash, 32 for HeavyHash),
     * so it fits in one rate block. */
    uint8_t block[KECCAK_RATE];
    memset(block, 0, KECCAK_RATE);
    memcpy(block, data, data_len);

    /* cSHAKE256 padding: 0x04 at data_len, 0x80 at last byte of rate */
    block[data_len] = 0x04;
    block[KECCAK_RATE - 1] |= 0x80;

    /* XOR into state and permute */
    for (int i = 0; i < KECCAK_RATE / 8; i++) {
        uint64_t lane;
        memcpy(&lane, block + i * 8, 8);
        state[i] ^= lane;
    }
    keccakf(state);

    /* 3. Squeeze 32 bytes */
    memcpy(output, state, 32);
}
