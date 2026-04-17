# KASDeck Arduino Port Reference — Verified Working Mining Logic

This document contains the complete, verified-working kHeavyHash mining implementation
from the PoPMiner Android app, formatted for porting to Arduino/ESP32 (KASDeck).

**Verified:** All shares accepted by rusty-kaspa bridge (0 invalid).
**Test vector match:** Python cSHAKE256 reference ↔ C code ↔ rusty-kaspa bridge.

---

## Critical Differences from Original KASDeck Code

The original KASDeck used **plain Keccak-256** (padding 0x01) and **XoShiRo256StarStar**.
The rusty-kaspa bridge requires:

| Component | Old (KASDeck/Go bridge) | New (rusty-kaspa bridge) |
|-----------|------------------------|--------------------------|
| Hash function | Keccak-256 (pad 0x01) | **cSHAKE256** (pad 0x04, domain separation) |
| PRNG | XoShiRo256**StarStar** | XoShiRo256**PlusPlus** |
| Matrix shifts | sum1>>6 & 0xF0, sum2>>10 & 0x0F | **Both >> 10**: `((sum1>>10)<<4) \| (sum2>>10)` |
| Matrix rank | No check | **Must be full rank (64)** |
| Nonce offset | byte 32 | **byte 72** |
| Timestamp | Ignored | **byte 32, required** |
| Difficulty compare | Big-endian | **Little-endian** (byte[31] is MSB) |
| Nonce submit format | bare hex | hex with **0x prefix** |

---

## 1. Work Buffer Layout (80 bytes)

```
Offset  Size  Field
------  ----  -----
0       32    pre_pow_hash (from mining.notify params[1])
32      8     timestamp (from mining.notify params[2], uint64 LE)
40      32    zero padding
72      8     nonce (uint64 LE, what you iterate)
```

---

## 2. Stratum Protocol

### Connect sequence:
```
→ {"id":1,"method":"mining.subscribe","params":["KASDeck"]}
  (wait 500ms)
→ {"id":2,"method":"mining.authorize","params":["wallet.worker"]}
```

### Receive mining.notify:
```json
{"method":"mining.notify","params":["job_id",[u64_0,u64_1,u64_2,u64_3],timestamp]}
```
- `params[0]` = job ID (string, incrementing number)
- `params[1]` = pre_pow_hash as 4 x uint64 array (LE byte order within each word)
- `params[2]` = timestamp as uint64 (milliseconds)

### Parse pre_pow_hash from uint64 array:
```c
// params[1] = [13655482766881680259, 15146921664149529878, ...]
for (int i = 0; i < 4; i++) {
    uint64_t value = parse_uint64(params_1[i]);  // careful: >2^53 needs string parsing
    for (int j = 0; j < 8; j++) {
        header_hash[i * 8 + j] = (value >> (j * 8)) & 0xFF;
    }
}
```

### Receive mining.set_difficulty:
```json
{"method":"mining.set_difficulty","params":[0.001]}
```
- Can be fractional (< 1.0). Store as double/float64.

### Submit share:
```
→ {"id":4,"method":"mining.submit","params":["wallet.worker","job_id","0x00000000deadbeef"]}
```
- 3 params: wallet.worker, job_id, nonce_hex
- Nonce: 16-char zero-padded lowercase hex **with 0x prefix**
- `String.format("0x%016x", nonce)`

### Share response:
- Accepted: `{"id":4,"result":true}`
- Rejected: `{"id":4,"error":[23,"Invalid difficulty",null]}`

---

## 3. kHeavyHash Algorithm (6 steps)

```
Step 1: Generate 64x64 matrix from pre_pow_hash using XoShiRo256++
        (only when pre_pow_hash changes, i.e., new job — cache it)
Step 2: hash1 = cSHAKE256("ProofOfWorkHash", 80_byte_work_buffer)
Step 3: Split hash1 into 64 nibbles (4-bit values)
Step 4: Matrix-vector multiply with >> 10 truncation
Step 5: XOR product with hash1
Step 6: output = cSHAKE256("HeavyHash", product)
```

---

## 4. cSHAKE256 Implementation

cSHAKE256 is Keccak with:
- **Domain separation** via `bytepad(encode_string("") || encode_string(N), 136)`
- **Padding byte 0x04** (not 0x01 like plain Keccak, not 0x06 like SHA-3)
- **Rate = 136 bytes** (1088 bits)

### NIST SP 800-185 encoding:

```c
// left_encode(x): [num_bytes_needed, x_bytes_big_endian]
// left_encode(0)   = [1, 0]         (2 bytes)
// left_encode(72)  = [1, 72]        (2 bytes)
// left_encode(120) = [1, 120]       (2 bytes)
// left_encode(136) = [1, 136]       (2 bytes)

// encode_string(S) = left_encode(len(S)*8) || S

// bytepad(X, w) = left_encode(w) || X || zeros_to_fill_multiple_of_w
```

### Domain initialization for "ProofOfWorkHash" (15 bytes):
```
bytepad block (136 bytes, zero-padded):
  [0]    = 0x01        // left_encode(136): length byte
  [1]    = 0x88 (136)  // left_encode(136): value
  [2]    = 0x01        // encode_string(""): left_encode(0) length
  [3]    = 0x00        // encode_string(""): left_encode(0) value
  [4]    = 0x01        // encode_string("ProofOfWorkHash"): left_encode(120) length
  [5]    = 0x78 (120)  // encode_string("ProofOfWorkHash"): left_encode(120) value
  [6-20] = "ProofOfWorkHash"  (15 bytes)
  [21-135] = zeros
```

### Domain initialization for "HeavyHash" (9 bytes):
```
bytepad block (136 bytes, zero-padded):
  [0]    = 0x01
  [1]    = 0x88 (136)
  [2]    = 0x01
  [3]    = 0x00
  [4]    = 0x01
  [5]    = 0x48 (72)   // 9 * 8 = 72
  [6-14] = "HeavyHash"  (9 bytes)
  [15-135] = zeros
```

### Finalization:
After absorbing data, pad with `0x04` at current position, `0x80` at byte 135 of rate block,
XOR into state, run keccakf.

### Complete cSHAKE256 C code:

```c
#define KECCAK_RATE 136

// Keccak-f[1600] round constants
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

static const int ROTC[24] = {
     1,  3,  6, 10, 15, 21, 28, 36,
    45, 55,  2, 14, 27, 41, 56,  8,
    25, 43, 62, 18, 39, 61, 20, 44
};

static const int PILN[24] = {
    10,  7, 11, 17, 18,  3,  5, 16,
     8, 21, 24,  4, 15, 23, 19, 13,
    12,  2, 20, 14, 22,  9,  6,  1
};

static inline uint64_t rotl64(uint64_t x, int y) {
    return (x << y) | (x >> (64 - y));
}

void keccakf(uint64_t state[25]) {
    uint64_t t, bc[5];
    for (int round = 0; round < 24; round++) {
        // Theta
        for (int i = 0; i < 5; i++)
            bc[i] = state[i] ^ state[i+5] ^ state[i+10] ^ state[i+15] ^ state[i+20];
        for (int i = 0; i < 5; i++) {
            t = bc[(i+4)%5] ^ rotl64(bc[(i+1)%5], 1);
            for (int j = 0; j < 25; j += 5) state[j+i] ^= t;
        }
        // Rho and Pi
        t = state[1];
        for (int i = 0; i < 24; i++) {
            int j = PILN[i];
            bc[0] = state[j];
            state[j] = rotl64(t, ROTC[i]);
            t = bc[0];
        }
        // Chi
        for (int j = 0; j < 25; j += 5) {
            for (int i = 0; i < 5; i++) bc[i] = state[j+i];
            for (int i = 0; i < 5; i++) state[j+i] ^= (~bc[(i+1)%5]) & bc[(i+2)%5];
        }
        // Iota
        state[0] ^= RC[round];
    }
}

void cshake256_hash(const uint8_t *name, size_t name_len,
                    const uint8_t *data, size_t data_len,
                    uint8_t *output) {
    uint64_t state[25];
    memset(state, 0, sizeof(state));

    // 1. Build and absorb bytepad block
    uint8_t header[256];
    size_t pos = 0;

    // left_encode(136)
    header[pos++] = 1;
    header[pos++] = 136;

    // encode_string("") = left_encode(0)
    header[pos++] = 1;
    header[pos++] = 0;

    // encode_string(name) = left_encode(name_len*8) || name
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
    pos += name_len;

    // Pad to 136 bytes
    memset(header + pos, 0, 136 - pos);

    // XOR into state and permute
    for (int i = 0; i < 136/8; i++) {
        uint64_t lane;
        memcpy(&lane, header + i*8, 8);
        state[i] ^= lane;
    }
    keccakf(state);

    // 2. Absorb data (data_len must be <= 136 for our use cases)
    uint8_t block[136];
    memset(block, 0, 136);
    memcpy(block, data, data_len);

    // 3. Apply cSHAKE256 padding: 0x04 at data_len, 0x80 at byte 135
    block[data_len] = 0x04;
    block[135] |= 0x80;

    // XOR into state and permute
    for (int i = 0; i < 136/8; i++) {
        uint64_t lane;
        memcpy(&lane, block + i*8, 8);
        state[i] ^= lane;
    }
    keccakf(state);

    // 4. Squeeze 32 bytes
    memcpy(output, state, 32);
}
```

**NOTE:** This simplified version works when data fits in one rate block (≤ 136 bytes).
ProofOfWorkHash uses 80 bytes, HeavyHash uses 32 bytes — both fit.

---

## 5. XoShiRo256PlusPlus PRNG

**CRITICAL:** Must be PlusPlus (++), NOT StarStar (**).

```c
typedef struct { uint64_t s[4]; } XoShiRo256PlusPlus;

void xoshiro_init(XoShiRo256PlusPlus *rng, const uint8_t *hash) {
    // Read 32-byte hash as 4 x uint64 in little-endian
    for (int i = 0; i < 4; i++) {
        rng->s[i] = 0;
        for (int j = 0; j < 8; j++) {
            rng->s[i] |= ((uint64_t)hash[i * 8 + j]) << (j * 8);
        }
    }
}

uint64_t xoshiro_next(XoShiRo256PlusPlus *rng) {
    // PlusPlus: rotl(s[0] + s[3], 23) + s[0]
    // StarStar would be: rotl(s[1] * 5, 7) * 9  <-- WRONG for rusty-kaspa
    uint64_t result = rotl64(rng->s[0] + rng->s[3], 23) + rng->s[0];
    uint64_t t = rng->s[1] << 17;

    rng->s[2] ^= rng->s[0];
    rng->s[3] ^= rng->s[1];
    rng->s[1] ^= rng->s[2];
    rng->s[0] ^= rng->s[3];
    rng->s[2] ^= t;
    rng->s[3] = rotl64(rng->s[3], 45);

    return result;
}
```

---

## 6. Matrix Generation (with rank check)

```c
void matrix_fill(uint16_t matrix[64][64], XoShiRo256PlusPlus *rng) {
    for (int i = 0; i < 64; i++) {
        for (int j = 0; j < 64; j += 16) {
            uint64_t val = xoshiro_next(rng);
            for (int shift = 0; shift < 16; shift++) {
                matrix[i][j + shift] = (val >> (4 * shift)) & 0x0F;
            }
        }
    }
}

// Gaussian elimination rank check (uses float64)
int matrix_compute_rank(uint16_t matrix[64][64]) {
    double mat[64][64];
    bool row_selected[64] = {false};
    int rank = 0;
    const double EPS = 1e-9;

    for (int i = 0; i < 64; i++)
        for (int j = 0; j < 64; j++)
            mat[i][j] = (double)matrix[i][j];

    for (int i = 0; i < 64; i++) {
        int j = 0;
        while (j < 64) {
            if (!row_selected[j] && fabs(mat[j][i]) > EPS) break;
            j++;
        }
        if (j != 64) {
            rank++;
            row_selected[j] = true;
            for (int p = i+1; p < 64; p++) mat[j][p] /= mat[j][i];
            for (int k = 0; k < 64; k++) {
                if (k != j && fabs(mat[k][i]) > EPS) {
                    for (int p = i+1; p < 64; p++)
                        mat[k][p] -= mat[j][p] * mat[k][i];
                }
            }
        }
    }
    return rank;
}

// Generate matrix — loops until full rank (almost always first try)
void matrix_generate(uint16_t matrix[64][64], const uint8_t *pre_pow_hash) {
    XoShiRo256PlusPlus rng;
    xoshiro_init(&rng, pre_pow_hash);
    do {
        matrix_fill(matrix, &rng);
    } while (matrix_compute_rank(matrix) != 64);
}
```

**ESP32 NOTE:** The 64x64 float64 matrix for rank check needs ~32KB RAM. On ESP32 with
limited SRAM, consider using PSRAM or computing rank with integer arithmetic.

---

## 7. Matrix-Vector Multiply

**Both shifts are >> 10.** This is different from the old KASDeck code.

```c
// hash1 = 32-byte intermediate hash from cSHAKE256("ProofOfWorkHash")
// vec = 64 nibbles extracted from hash1

// Extract nibbles
uint8_t vec[64];
for (int i = 0; i < 32; i++) {
    vec[2*i]     = hash1[i] >> 4;       // upper nibble
    vec[2*i + 1] = hash1[i] & 0x0F;    // lower nibble
}

// Matrix-vector multiply
uint8_t product[32];
for (int i = 0; i < 32; i++) {
    uint16_t sum1 = 0, sum2 = 0;
    for (int j = 0; j < 64; j++) {
        sum1 += matrix[2*i][j]     * vec[j];
        sum2 += matrix[2*i + 1][j] * vec[j];
    }
    // BOTH use >> 10 (not >> 6 and >> 10 like old KASDeck)
    product[i] = (uint8_t)(((sum1 >> 10) << 4) | (sum2 >> 10));
}

// XOR with original hash
for (int i = 0; i < 32; i++) {
    product[i] ^= hash1[i];
}
```

---

## 8. Difficulty Target (Little-Endian)

```c
// Compare as LE Uint256: byte[31] = MSB, byte[0] = LSB
bool check_difficulty(const uint8_t *hash, const uint8_t *target) {
    for (int i = 31; i >= 0; i--) {
        if (hash[i] < target[i]) return true;
        if (hash[i] > target[i]) return false;
    }
    return true;
}

// For difficulty <= 1.0 (typical for low-hashrate miners):
// Set all 32 target bytes to 0xFF (accept everything locally,
// let the bridge validate). The bridge computes its own target.
void set_target_from_difficulty(double difficulty, uint8_t *target) {
    if (difficulty <= 1.0) {
        memset(target, 0xFF, 32);  // accept all — bridge validates
    } else {
        // For diff > 1.0: target = MAX_TARGET / difficulty
        // MAX_TARGET = 2^224 - 1 (LE bytes [0..27] = 0xFF)
        // ... (see full implementation in kheavyhash.c)
    }
}
```

---

## 9. Complete Mining Loop (Pseudocode)

```
1. Connect to bridge stratum port (TCP)
2. Send mining.subscribe, wait, send mining.authorize
3. Wait for mining.set_difficulty → store as current_difficulty
4. Wait for mining.notify → parse pre_pow_hash, timestamp, job_id

5. Generate matrix from pre_pow_hash (cache until next job)
6. Set target from current_difficulty

7. Mining loop:
   a. Build 80-byte work buffer:
      [0..31]  = pre_pow_hash
      [32..39] = timestamp (uint64 LE)
      [40..71] = zeros
      [72..79] = nonce (uint64 LE)

   b. hash1 = cSHAKE256("ProofOfWorkHash", work_buffer)
   c. Extract 64 nibbles from hash1
   d. product = matrix_multiply(matrix, nibbles) using >> 10
   e. product ^= hash1
   f. final_hash = cSHAKE256("HeavyHash", product)

   g. If check_difficulty(final_hash, target):
      Submit: {"method":"mining.submit","params":["wallet.worker","job_id","0x<nonce_hex>"]}

   h. nonce++
   i. Check for new mining.notify (update job if received)

8. On mining.set_difficulty → update target, continue mining
9. On new mining.notify → update pre_pow_hash, timestamp, job_id, regenerate matrix
```

---

## 10. Test Vector (Verified Against Python + Bridge)

```
Input:
  pre_pow_hash = [0x2A] * 32  (all bytes = 42)
  timestamp    = 5435345234 (0x144284352)
  nonce        = 432432432  (0x19C6C530)

Expected results:
  PowHash intermediate (first 16 bytes): 2fb72b63dd0dd0d82b00cd9f83d4eca0
  Full kHeavyHash (first 16 bytes):      5a5bcd6e352eb8c87c80d0f0574a45a5
  Matrix[0][0:4] = 4 5 4 5
  Matrix[1][0:4] = 9 11 1 11
```

Use this test vector to verify your ESP32 port produces identical output
before connecting to the bridge.
